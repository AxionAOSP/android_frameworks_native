/*
 * Copyright 2024 The Android Open Source Project
 * Copyright 2025 AxionsOS
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
#include <SkImage.h>
#include <SkRuntimeEffect.h>
#include <SkSurface.h>
#include "BlurFilter.h"

#include "RuntimeEffectManager.h"

namespace android {
namespace renderengine {
namespace skia {

class GlassBlurFilter : public BlurFilter {
public:
    explicit GlassBlurFilter(RuntimeEffectManager& effectManager);
    virtual ~GlassBlurFilter() {}

    // Execute blur, saving it to a texture
    sk_sp<SkImage> generate(SkiaGpuContext* context, const uint32_t radius,
                            const sk_sp<SkImage> blurInput, const SkRect& blurRect) const override;

private:
    sk_sp<SkRuntimeEffect> mLowSampleBlurEffect;
    sk_sp<SkRuntimeEffect> mHighSampleBlurEffect;

    void blurInto(const sk_sp<SkSurface>& drawSurface, const sk_sp<SkImage>& readImage,
                  const float radius, const float alpha, const sk_sp<SkRuntimeEffect>&) const;

    void blurInto(const sk_sp<SkSurface>& drawSurface, const sk_sp<SkShader> input,
                  const float radius, const float alpha, const sk_sp<SkRuntimeEffect>&) const;
};

} // namespace skia
} // namespace renderengine
} // namespace android
