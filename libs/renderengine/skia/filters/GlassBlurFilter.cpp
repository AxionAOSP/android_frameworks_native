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
    return 0.1667f;
}

}

const SkString kEffectSource_GlassBlurFilter_UpSampleEffect(
        "uniform shader child;\n"
        "uniform float4 in_offsets;\n"
        "half4 main(float2 xy) {\n"
        "    float2 d0 = in_offsets.xy;\n"
        "    float2 d1 = in_offsets.zw;\n"
        "    half3 c = child." "ev" "al(xy).rgb * half(4.0);\n"
        "    c += child." "ev" "al(xy + d0).rgb;\n"
        "    c += child." "ev" "al(xy - d0).rgb;\n"
        "    c += child." "ev" "al(xy + d1).rgb;\n"
        "    c += child." "ev" "al(xy - d1).rgb;\n"
        "    return half4(c * half(0.125), half(1.0));\n"
        "}");

const SkString kEffectSource_GlassBlurFilter_FinalUpSampleEffect(
        "uniform shader child;\n"
        "uniform float4 in_offsets;\n"
        "half4 main(float2 xy) {\n"
        "    float2 d0 = in_offsets.xy;\n"
        "    float2 d1 = in_offsets.zw;\n"
        "    half3 c = child." "ev" "al(xy).rgb * half(4.0);\n"
        "    c += child." "ev" "al(xy + d0).rgb;\n"
        "    c += child." "ev" "al(xy - d0).rgb;\n"
        "    c += child." "ev" "al(xy + d1).rgb;\n"
        "    c += child." "ev" "al(xy - d1).rgb;\n"
        "    return half4(c * half(0.125), half(1.0));\n"
        "}");

GlassBlurFilter::GlassBlurFilter(RuntimeEffectManager& effectManager)
      : BlurFilter(effectManager, 0.0f, BlurFilter::kInputScale) {
    mHalfResDownSampleBlurEffect =
            effectManager.mKnownEffects[kKawaseBlurDualFilterV2_HalfResDownSampleBlurEffect];
    mUpSampleBlurEffect = effectManager.mKnownEffects[kGlassBlurFilter_UpSampleEffect];
    mRotatedUpSampleBlurEffect = effectManager.mKnownEffects[kGlassBlurFilter_FinalUpSampleEffect];
    mInputScale = readGlassInputScale();
    mRadiusToScaledRadius = mInputScale * kRadiusToSigma;
}

uint32_t GlassBlurFilter::effectiveRadius(uint32_t radius) const {
    return radius;
}

sk_sp<SkSurface> GlassBlurFilter::obtainSurface(SkiaGpuContext* context, const SkImageInfo& info,
                                                int index) const {
    if (index < 0 || index >= kMaxSurfaces || !context) {
        return nullptr;
    }

    auto& pool = mPools[index];
    size_t& count = mCounts[index];

    const uint64_t minAvailableFrame = mFrameCounter >= 2 ? mFrameCounter - 2 : 0;
    SurfaceSlot* candidate = nullptr;
    int staleSlotIndex = -1;
    for (size_t i = 0; i < count; ++i) {
        auto& slot = pool[i];
        if (!slot.surface || slot.context != context) continue;
        if (slot.info != info) {
            if (staleSlotIndex < 0 && slot.lastUsedFrame < minAvailableFrame) staleSlotIndex = i;
            continue;
        }
        if (slot.lastUsedFrame < minAvailableFrame) {
            slot.lastUsedFrame = mFrameCounter;
            return slot.surface;
        }
        if (!candidate) candidate = &slot;
    }
    if (candidate && count >= kPoolCapacity) {
        candidate->lastUsedFrame = mFrameCounter;
        return candidate->surface;
    }

    ATRACE_NAME("GlassBlurSurfaceCreate");
    sk_sp<SkSurface> surface = context->createRenderTarget(info);
    if (!surface) {
        return nullptr;
    }

    if (staleSlotIndex >= 0) {
        pool[staleSlotIndex] = {info, context, surface, mFrameCounter};
    } else if (count < kPoolCapacity) {
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
static const SkMatrix kDownscaleMatrix = SkMatrix::Scale(0.5f, 0.5f);
static const SkMatrix kUpscaleMatrix = SkMatrix::Scale(2.0f, 2.0f);

void GlassBlurFilter::blurInto(const sk_sp<SkSurface>& drawSurface,
                                sk_sp<SkShader> input,
                                const sk_sp<const SkData>& uniforms,
                                const float alpha,
                                const sk_sp<SkRuntimeEffect>& blurEffect) const {
    sk_sp<SkShader> children[1] = {std::move(input)};
    sk_sp<SkShader> shader = blurEffect->makeShader(uniforms, children, 1);
    if (!shader) {
        return;
    }
    SkPaint paint;
    paint.setShader(std::move(shader));
    SkCanvas* canvas = drawSurface->getCanvas();
    if (alpha != 1.0f) {
        paint.setAlphaf(alpha);
        paint.setBlendMode(SkBlendMode::kSrcOver);
    } else {
        paint.setBlendMode(SkBlendMode::kSrc);
        canvas->discard();
    }
    canvas->drawPaint(paint);
}

sk_sp<SkImage> GlassBlurFilter::generate(SkiaGpuContext* context, const uint32_t blurRadius,
                                          const sk_sp<SkImage> input,
                                          const SkRect& blurRect) const {
    if (!context || !input) {
        return nullptr;
    }
    const uint32_t effRadius = effectiveRadius(blurRadius);
    if (effRadius == 0 || blurRect.isEmpty()) {
        return input;
    }
    ++mFrameCounter;
    const float scale = mInputScale;
    const float scaledRadius = effRadius * mRadiusToScaledRadius;

    SkIRect targetBlurRect;
    blurRect.roundOut(&targetBlurRect);

    const int rawW0 = std::max(1, static_cast<int>(std::round((targetBlurRect.width() + 4) * scale)));
    const int rawH0 = std::max(1, static_cast<int>(std::round((targetBlurRect.height() + 4) * scale)));
    const int w0 = std::max(32, (rawW0 + 31) & ~31);
    const int h0 = std::max(32, (rawH0 + 31) & ~31);

    int maxPasses = kMaxSurfaces - 1;
    while (maxPasses > 1 && ((w0 >> maxPasses) < 16 || (h0 >> maxPasses) < 16)) {
        --maxPasses;
    }

    const float filterDepth = std::min(static_cast<float>(maxPasses), std::max(1.0f, scaledRadius / 3.0f));
    const int filterPasses = std::min(maxPasses, static_cast<int>(ceilf(filterDepth)));
    const float step = std::max(1.5f, 1.0f + scaledRadius * 0.35f);

    auto makeSurface = [&](int index) -> sk_sp<SkSurface> {
        const int newW = w0 >> index;
        const int newH = h0 >> index;
        SkImageInfo info = input->imageInfo().makeWH(newW, newH).makeAlphaType(kOpaque_SkAlphaType);
        if (info.colorType() == kRGBA_F16_SkColorType) {
            info = info.makeColorType(kRGBA_8888_SkColorType);
        }
        return obtainSurface(context, info, index);
    };

    std::array<sk_sp<SkSurface>, kMaxSurfaces> surfaces = {};
    for (int i = 0; i <= filterPasses; i++) {
        surfaces[i] = makeSurface(i);
        if (!surfaces[i]) {
            return input;
        }
    }

    {
        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kSrc);
        SkCanvas* canvas = surfaces[0]->getCanvas();
        canvas->discard();
        canvas->drawImageRect(input, blurRect,
                              SkRect::MakeIWH(w0, h0),
                              kLinearSampling, &paint,
                              SkCanvas::SrcRectConstraint::kFast_SrcRectConstraint);
    }

    for (int i = 0; i < filterPasses; i++) {
        sk_sp<SkImage> tempImg = surfaces[i]->makeTemporaryImage();
        if (!tempImg) {
            return input;
        }
        blurInto(surfaces[i + 1],
                 tempImg->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                     kLinearSampling, kDownscaleMatrix),
                 nullptr, 1.0f, mHalfResDownSampleBlurEffect);
    }

    if (!mLastUniformsAxis || fabsf(step - mLastStep) > 0.001f) {
        mLastStep = step;
        const float axisOffsets[4] = {step, 0.0f, 0.0f, step};
        const float d = step * 0.70710678f;
        const float diagOffsets[4] = {d, d, d, -d};
        mLastUniformsAxis = SkData::MakeWithCopy(axisOffsets, sizeof(axisOffsets));
        mLastUniformsDiag = SkData::MakeWithCopy(diagOffsets, sizeof(diagOffsets));
    }

    for (int i = filterPasses - 1; i >= 0; i--) {
        sk_sp<SkImage> tempImg = surfaces[i + 1]->makeTemporaryImage();
        if (!tempImg) return input;
        const float alpha = std::min(1.0f, filterDepth - i);
        const auto& u = (i % 2 == 0) ? mLastUniformsAxis : mLastUniformsDiag;
        blurInto(surfaces[i], tempImg->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, kLinearSampling, kUpscaleMatrix), u, alpha, mUpSampleBlurEffect);
    }

    sk_sp<SkImage> result = surfaces[0]->makeTemporaryImage();
    return result ? result : input;
}

} // namespace skia
} // namespace renderengine
} // namespace android
