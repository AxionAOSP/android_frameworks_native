/*
 * Copyright 2025 The Android Open Source Project
 * Copyright 2025-2026 AxionOS
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "GlassBlurFilter.h"
#include <SkAlphaType.h>
#include <SkBlendMode.h>
#include <SkCanvas.h>
#include <SkData.h>
#include <SkPaint.h>
#include <SkRRect.h>
#include <SkRuntimeEffect.h>
#include <SkShader.h>
#include <SkSize.h>
#include <SkString.h>
#include <SkSurface.h>
#include <SkTileMode.h>
#include <SkSamplingOptions.h>
#include <android-base/properties.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <log/log.h>
#include <utils/Trace.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

#include "RuntimeEffectManager.h"

namespace android {
namespace renderengine {
namespace skia {
namespace {

constexpr const char* kGlassInputScaleProperty = "persist.sys.sf.gb_scale";

float readGlassInputScale() {
    const std::string value = base::GetProperty(kGlassInputScaleProperty, "");
    if (!value.empty()) {
        char* end = nullptr;
        const float scale = std::strtof(value.c_str(), &end);
        if (end != value.c_str() && std::isfinite(scale)) {
            return std::clamp(scale, 0.05f, 0.25f);
        }
    }
    return 0.20f;
}

}

const SkString kEffectSource_GlassBlurFilter_UpSampleEffect(R"(
    uniform shader child;
    uniform float in_blurOffset;

    const float2 STEP_0 = float2( 1.0, 0.0);
    const float2 STEP_1 = float2( 0.623489802,  0.781831482);
    const float2 STEP_2 = float2(-0.222520934,  0.974927912);
    const float2 STEP_3 = float2(-0.900968868,  0.433883739);
    const float2 STEP_4 = float2(-0.900968868, -0.433883739);
    const float2 STEP_5 = float2(-0.222520934, -0.974927912);
    const float2 STEP_6 = float2( 0.623489802, -0.781831482);

    half4 main(float2 xy) {
        float step = in_blurOffset;
        half3 c = child.eval(xy).rgb;
        c += child.eval(xy + STEP_0 * step).rgb;
        c += child.eval(xy + STEP_1 * step).rgb;
        c += child.eval(xy + STEP_2 * step).rgb;
        c += child.eval(xy + STEP_3 * step).rgb;
        c += child.eval(xy + STEP_4 * step).rgb;
        c += child.eval(xy + STEP_5 * step).rgb;
        c += child.eval(xy + STEP_6 * step).rgb;

        return half4(c * 0.125, 1.0);
    }
)");

const SkString kEffectSource_GlassBlurFilter_FinalUpSampleEffect(R"(
    uniform shader child;
    uniform float in_blurOffset;

    const float2 STEP_0 = float2( 0.900968868,  0.433883739);
    const float2 STEP_1 = float2( 0.222520934,  0.974927912);
    const float2 STEP_2 = float2(-0.623489802,  0.781831482);
    const float2 STEP_3 = float2(-1.0, 0.0);
    const float2 STEP_4 = float2(-0.623489802, -0.781831482);
    const float2 STEP_5 = float2( 0.222520934, -0.974927912);
    const float2 STEP_6 = float2( 0.900968868, -0.433883739);

    half4 main(float2 xy) {
        float step = in_blurOffset;
        half3 c = child.eval(xy).rgb;
        c += child.eval(xy + STEP_0 * step).rgb;
        c += child.eval(xy + STEP_1 * step).rgb;
        c += child.eval(xy + STEP_2 * step).rgb;
        c += child.eval(xy + STEP_3 * step).rgb;
        c += child.eval(xy + STEP_4 * step).rgb;
        c += child.eval(xy + STEP_5 * step).rgb;
        c += child.eval(xy + STEP_6 * step).rgb;

        return half4(c * 0.125, 1.0);
    }
)");

GlassBlurFilter::GlassBlurFilter(RuntimeEffectManager& effectManager)
      : BlurFilter(effectManager, 0.0f, BlurFilter::kInputScale) {
    mQuarterResDownSampleBlurEffect =
            effectManager.mKnownEffects[kKawaseBlurDualFilterV2_QuarterResDownSampleBlurEffect];
    mHalfResDownSampleBlurEffect =
            effectManager.mKnownEffects[kKawaseBlurDualFilterV2_HalfResDownSampleBlurEffect];
    mUpSampleBlurEffect = effectManager.mKnownEffects[kGlassBlurFilter_UpSampleEffect];
    mRotatedUpSampleBlurEffect = effectManager.mKnownEffects[kGlassBlurFilter_FinalUpSampleEffect];
    mInputScale = readGlassInputScale();
    mRadiusToScaledRadius = mInputScale * 0.57735f;
}

uint32_t GlassBlurFilter::effectiveRadius(uint32_t radius) const {
    if (radius < 8) {
        return radius;
    }
    return (radius + 15u) & ~15u;
}

sk_sp<SkSurface> GlassBlurFilter::obtainSurface(SkiaGpuContext* context, const SkImageInfo& info,
                                                int index) const {
    if (index < 0 || index >= kMaxSurfaces || !context) {
        return nullptr;
    }

    if (index == 0) {
        ++mFrameCounter;
        SurfaceSlot* candidate = nullptr;
        for (size_t i = 0; i < mLevel0Count; ++i) {
            auto& slot = mLevel0Pool[i];
            if (slot.surface && slot.context == context && slot.info == info) {
                if (slot.lastUsedFrame == mFrameCounter - 1) {
                    candidate = &slot;
                    continue;
                }
                slot.lastUsedFrame = mFrameCounter;
                if (i > 1) {
                    std::swap(mLevel0Pool[0], mLevel0Pool[i]);
                    return mLevel0Pool[0].surface;
                }
                return slot.surface;
            }
        }
        if (candidate && mLevel0Count >= kLevel0PoolCapacity) {
            candidate->lastUsedFrame = mFrameCounter;
            return candidate->surface;
        }

        ATRACE_NAME("GlassBlurSurfaceCreate");
        sk_sp<SkSurface> surface = context->createRenderTarget(info);
        if (!surface) {
            return nullptr;
        }

        if (mLevel0Count < kLevel0PoolCapacity) {
            mLevel0Pool[mLevel0Count] = {info, context, surface, mFrameCounter};
            ++mLevel0Count;
        } else {
            size_t lruIndex = 0;
            uint64_t oldest = mLevel0Pool[0].lastUsedFrame;
            for (size_t i = 1; i < mLevel0Count; ++i) {
                if (mLevel0Pool[i].lastUsedFrame < oldest) {
                    oldest = mLevel0Pool[i].lastUsedFrame;
                    lruIndex = i;
                }
            }
            mLevel0Pool[lruIndex] = {info, context, surface, mFrameCounter};
        }
        return surface;
    }

    const size_t interIndex = static_cast<size_t>(index - 1);
    auto& pool = mIntermediatePools[interIndex];
    size_t& count = mIntermediateCounts[interIndex];

    for (size_t i = 0; i < count; ++i) {
        if (pool[i].surface && pool[i].context == context && pool[i].info == info) {
            pool[i].lastUsedFrame = mFrameCounter;
            if (i > 0) {
                std::swap(pool[0], pool[i]);
                return pool[0].surface;
            }
            return pool[i].surface;
        }
    }

    ATRACE_NAME("GlassBlurSurfaceCreate");
    sk_sp<SkSurface> surface = context->createRenderTarget(info);
    if (!surface) {
        return nullptr;
    }

    if (count < kIntermediatePoolCapacity) {
        pool[count] = {info, context, surface, mFrameCounter};
        ++count;
    } else {
        size_t lruIndex = 0;
        uint64_t oldest = pool[0].lastUsedFrame;
        for (size_t i = 1; i < count; ++i) {
            if (pool[i].lastUsedFrame < oldest) {
                oldest = pool[i].lastUsedFrame;
                lruIndex = i;
            }
        }
        pool[lruIndex] = {info, context, surface, mFrameCounter};
    }
    return surface;
}

static const SkSamplingOptions kLinearSampling(SkFilterMode::kLinear, SkMipmapMode::kNone);

void GlassBlurFilter::blurInto(const sk_sp<SkSurface>& drawSurface,
                                const sk_sp<SkImage>& readImage,
                                const sk_sp<const SkData>& uniforms,
                                const sk_sp<SkRuntimeEffect>& blurEffect) const {
    SkMatrix blurMatrix =
            SkMatrix::Scale(static_cast<float>(drawSurface->width()) / readImage->width(),
                            static_cast<float>(drawSurface->height()) / readImage->height());
    blurInto(drawSurface,
             readImage->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                   kLinearSampling, blurMatrix),
             uniforms, blurEffect);
}

void GlassBlurFilter::blurInto(const sk_sp<SkSurface>& drawSurface,
                                sk_sp<SkShader> input,
                                const sk_sp<const SkData>& uniforms,
                                const sk_sp<SkRuntimeEffect>& blurEffect) const {
    sk_sp<SkShader> children[1] = {std::move(input)};
    sk_sp<SkShader> shader = blurEffect->makeShader(uniforms, children, 1);
    SkPaint paint;
    paint.setShader(std::move(shader));
    paint.setBlendMode(SkBlendMode::kSrc);
    SkCanvas* canvas = drawSurface->getCanvas();
    canvas->discard();
    canvas->drawPaint(paint);
}

sk_sp<SkImage> GlassBlurFilter::generate(SkiaGpuContext* context, const uint32_t blurRadius,
                                          const sk_sp<SkImage> input,
                                          const SkRect& blurRect) const {
    if (!context || !input) {
        return nullptr;
    }
    const uint32_t effRadius = effectiveRadius(blurRadius);
    const float scaledRadius = effRadius * mRadiusToScaledRadius;

    int filterPasses = 3;
    if (scaledRadius < 3.5f) {
        filterPasses = 1;
    } else if (scaledRadius < 8.0f) {
        filterPasses = 2;
    }

    SkIRect targetBlurRect;
    blurRect.roundOut(&targetBlurRect);

    const int rawW0 = std::max(1, (targetBlurRect.width() + 4) / 5);
    const int rawH0 = std::max(1, (targetBlurRect.height() + 4) / 5);
    const int w0 = std::max(32, (rawW0 + 31) & ~31);
    const int h0 = std::max(32, (rawH0 + 63) & ~63);

    auto makeSurface = [&](int index) -> sk_sp<SkSurface> {
        const int newW = std::max(1, w0 >> index);
        const int newH = std::max(1, h0 >> index);
        return obtainSurface(context, input->imageInfo().makeWH(newW, newH), index);
    };

    std::array<sk_sp<SkSurface>, kMaxSurfaces> surfaces = {};
    for (int i = 0; i <= filterPasses; i++) {
        surfaces[i] = makeSurface(i);
        if (!surfaces[i]) {
            return input;
        }
    }

    {
        SkMatrix blurMatrix;
        const float sx = static_cast<float>(surfaces[0]->width()) / blurRect.width();
        const float sy = static_cast<float>(surfaces[0]->height()) / blurRect.height();
        if (blurRect.fLeft == 0.0f && blurRect.fTop == 0.0f) {
            blurMatrix.setScale(sx, sy);
        } else {
            blurMatrix.setTranslate(-blurRect.fLeft, -blurRect.fTop);
            blurMatrix.postScale(sx, sy);
        }
        const auto sourceShader =
                input->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                  kLinearSampling, blurMatrix);
        blurInto(surfaces[0], sourceShader, nullptr, mQuarterResDownSampleBlurEffect);
    }

    for (int i = 0; i < filterPasses; i++) {
        blurInto(surfaces[i + 1], surfaces[i]->makeTemporaryImage(), nullptr,
                 mHalfResDownSampleBlurEffect);
    }

    const float step = std::max(2.0f, scaledRadius * 0.40f);
    if (step != mLastStep || !mLastUniforms) {
        mLastStep = step;
        mLastUniforms = SkData::MakeWithCopy(&step, sizeof(step));
    }
    const auto& upsampleUniforms = mLastUniforms;

    for (int i = filterPasses - 1; i >= 0; i--) {
        const sk_sp<SkRuntimeEffect>& upEffect =
                (i % 2 == 0) ? mRotatedUpSampleBlurEffect : mUpSampleBlurEffect;
        blurInto(surfaces[i], surfaces[i + 1]->makeTemporaryImage(), upsampleUniforms, upEffect);
    }

    sk_sp<SkImage> result = surfaces[0]->makeTemporaryImage();
    return result ? result : input;
}

} // namespace skia
} // namespace renderengine
} // namespace android
