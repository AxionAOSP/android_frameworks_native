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
#pragma once

#include <SkCanvas.h>
#include <SkData.h>
#include <SkImage.h>
#include <SkRuntimeEffect.h>
#include <SkSurface.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "BlurFilter.h"

#include "RuntimeEffectManager.h"

namespace android {
namespace renderengine {
namespace skia {

class GlassBlurFilter : public BlurFilter {
public:
    explicit GlassBlurFilter(RuntimeEffectManager& effectManager);
    virtual ~GlassBlurFilter() {}

    sk_sp<SkImage> generate(SkiaGpuContext* context, const uint32_t radius,
                            const sk_sp<SkImage> blurInput, const SkRect& blurRect) const override;
    uint32_t effectiveRadius(uint32_t radius) const override;

private:
    sk_sp<SkRuntimeEffect> mQuarterResDownSampleBlurEffect;
    sk_sp<SkRuntimeEffect> mHalfResDownSampleBlurEffect;
    sk_sp<SkRuntimeEffect> mUpSampleBlurEffect;
    sk_sp<SkRuntimeEffect> mRotatedUpSampleBlurEffect;

    float mInputScale = 0.20f;
    float mRadiusToScaledRadius = 0.11547f;

    static constexpr int kMaxSurfaces = 4;
    static constexpr size_t kLevel0PoolCapacity = 16;
    static constexpr size_t kIntermediatePoolCapacity = 8;

    struct SurfaceSlot {
        SkImageInfo info;
        SkiaGpuContext* context = nullptr;
        sk_sp<SkSurface> surface;
        uint64_t lastUsedFrame = 0;
    };

    mutable std::array<SurfaceSlot, kLevel0PoolCapacity> mLevel0Pool;
    mutable size_t mLevel0Count = 0;
    mutable std::array<std::array<SurfaceSlot, kIntermediatePoolCapacity>, kMaxSurfaces - 1> mIntermediatePools;
    mutable std::array<size_t, kMaxSurfaces - 1> mIntermediateCounts = {};
    mutable uint64_t mFrameCounter = 0;

    mutable float mLastStep = -1.0f;
    mutable sk_sp<const SkData> mLastUniforms;

    sk_sp<SkSurface> obtainSurface(SkiaGpuContext* context, const SkImageInfo& info,
                                   int index) const;

    void blurInto(const sk_sp<SkSurface>& drawSurface, const sk_sp<SkImage>& readImage,
                  const sk_sp<const SkData>& uniforms,
                  const sk_sp<SkRuntimeEffect>& blurEffect) const;

    void blurInto(const sk_sp<SkSurface>& drawSurface, sk_sp<SkShader> input,
                  const sk_sp<const SkData>& uniforms,
                  const sk_sp<SkRuntimeEffect>& blurEffect) const;
};

} // namespace skia
} // namespace renderengine
} // namespace android
