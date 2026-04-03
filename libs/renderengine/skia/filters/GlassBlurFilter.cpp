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
#include <SkImageFilters.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <log/log.h>
#include <utils/Trace.h>

#include "RuntimeEffectManager.h"

namespace android {
namespace renderengine {
namespace skia {

GlassBlurFilter::GlassBlurFilter(RuntimeEffectManager& effectManager)
      : BlurFilter(effectManager) {
    mQuarterResDownSampleBlurEffect =
            effectManager.mKnownEffects[kKawaseBlurDualFilterV2_QuarterResDownSampleBlurEffect];
    mHalfResDownSampleBlurEffect =
            effectManager.mKnownEffects[kKawaseBlurDualFilterV2_HalfResDownSampleBlurEffect];
    mUpSampleBlurEffect =
            effectManager.mKnownEffects[kKawaseBlurDualFilterV2_UpSampleBlurEffect];
}

void GlassBlurFilter::blurInto(const sk_sp<SkSurface>& drawSurface,
                                const sk_sp<SkImage>& readImage, const float radius,
                                const float alpha,
                                const sk_sp<SkRuntimeEffect>& blurEffect) const {
    const float scale = static_cast<float>(drawSurface->width()) / readImage->width();
    SkMatrix blurMatrix = SkMatrix::Scale(scale, scale);
    blurInto(drawSurface,
             readImage->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                   SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
                                   blurMatrix),
             radius, alpha, blurEffect);
}

void GlassBlurFilter::blurInto(const sk_sp<SkSurface>& drawSurface, sk_sp<SkShader> input,
                                const float radius, const float alpha,
                                const sk_sp<SkRuntimeEffect>& blurEffect) const {
    SkPaint paint;
    if (blurEffect == mUpSampleBlurEffect) {
        if (radius == 0) {
            paint.setShader(std::move(input));
            paint.setAlphaf(alpha);
        } else {
            SkRuntimeShaderBuilder blurBuilder(blurEffect);
            blurBuilder.child("child") = std::move(input);
            blurBuilder.uniform("in_crossFade") = alpha;
            blurBuilder.uniform("in_weightedCrossFade") = alpha * 0.125f;
            blurBuilder.uniform("in_blurOffset") = radius;
            paint.setShader(blurBuilder.makeShader(nullptr));
        }
    } else {
        SkRuntimeShaderBuilder blurBuilder(blurEffect);
        blurBuilder.child("child") = std::move(input);
        paint.setShader(blurBuilder.makeShader(nullptr));
    }
    paint.setBlendMode(alpha == 1.0f ? SkBlendMode::kSrc : SkBlendMode::kSrcOver);
    drawSurface->getCanvas()->drawPaint(paint);
}

sk_sp<SkImage> GlassBlurFilter::generate(SkiaGpuContext* context, const uint32_t blurRadius,
                                          const sk_sp<SkImage> input,
                                          const SkRect& blurRect) const {
    const float radius = blurRadius * 0.57735f;

    constexpr int kMaxSurfaces = 4;
    const float filterDepth = std::min(kMaxSurfaces - 1.0f, radius * kInputScale / 2.5f);
    const int filterPasses = std::min(kMaxSurfaces - 1, static_cast<int>(ceil(filterDepth)));

    SkIRect targetBlurRect;
    blurRect.roundIn(&targetBlurRect);

    auto makeSurface = [&](float scale) -> sk_sp<SkSurface> {
        const int newW =
                std::max(1, static_cast<int>(static_cast<float>(targetBlurRect.width()) / scale));
        const int newH =
                std::max(1, static_cast<int>(static_cast<float>(targetBlurRect.height()) / scale));
        sk_sp<SkSurface> surface =
                context->createRenderTarget(input->imageInfo().makeWH(newW, newH));
        LOG_ALWAYS_FATAL_IF(!surface, "%s: Failed to create surface for blurring!", __func__);
        return surface;
    };

    sk_sp<SkSurface> surfaces[kMaxSurfaces] =
            {filterPasses >= 0 ? makeSurface(1 * kInverseInputScale) : nullptr,
             filterPasses >= 1 ? makeSurface(2 * kInverseInputScale) : nullptr,
             filterPasses >= 2 ? makeSurface(4 * kInverseInputScale) : nullptr,
             filterPasses >= 3 ? makeSurface(8 * kInverseInputScale) : nullptr};

    float sumSquaredR = 0;
    float sumSquaredStep = 0;
    for (int i = 0; i < filterPasses; i++) {
        const float alpha = std::min(1.0f, filterDepth - i);
        sumSquaredR += powf(powf(2.0f, i - 1) * alpha * M_SQRT2, 2.0f);
        sumSquaredStep += powf(powf(2.0f, i) * alpha, 2.0f);
    }
    float step = sqrt(max(0.0f, powf(radius * kInputScale, 2) - sumSquaredR) /
                      (sumSquaredStep == 0 ? 1 : sumSquaredStep));

    {
        SkMatrix blurMatrix = SkMatrix::Translate(-blurRect.fLeft, -blurRect.fTop);
        blurMatrix.postScale(kInputScale, kInputScale);
        const auto sourceShader =
                input->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                  SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
                                  blurMatrix);
        blurInto(surfaces[0], std::move(sourceShader), 0, 1.0f,
                 mQuarterResDownSampleBlurEffect);
    }

    for (int i = 0; i < filterPasses; i++) {
        blurInto(surfaces[i + 1], surfaces[i]->makeTemporaryImage(), 0, 1.0f,
                 mHalfResDownSampleBlurEffect);
    }

    for (int i = filterPasses - 1; i >= 0; i--) {
        blurInto(surfaces[i], surfaces[i + 1]->makeTemporaryImage(), step,
                 std::min(1.0f, filterDepth - i), mUpSampleBlurEffect);
    }

    const float sigmaScale = blurRadius * kInputScale * 0.5f;
    SkPaint overlayPaint;
    overlayPaint.setBlendMode(SkBlendMode::kSrc);
    sk_sp<SkImageFilter> finalFilter = SkImageFilters::Blur(sigmaScale, sigmaScale,
                                                            SkTileMode::kClamp, nullptr);
    overlayPaint.setImageFilter(finalFilter);

    sk_sp<SkImage> preFinal = surfaces[0]->makeTemporaryImage();
    surfaces[0]->getCanvas()->drawImage(preFinal.get(), 0, 0, SkSamplingOptions(), &overlayPaint);

    return surfaces[0]->makeTemporaryImage();
}

} // namespace skia
} // namespace renderengine
} // namespace android
