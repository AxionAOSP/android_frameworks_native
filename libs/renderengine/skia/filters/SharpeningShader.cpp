/*
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

#include "SharpeningShader.h"

namespace android {
namespace renderengine {
namespace skia {

const SkString kEffectSource_SharpeningEffect(R"(
    uniform shader image;
    uniform float2 texSize;
    uniform half strength;

    half4 main(float2 xy) {
        float2 step = 1.0 / texSize;
        half4 c = image.eval(xy);
        half4 n = image.eval(xy + float2(0.0, -step.y));
        half4 s = image.eval(xy + float2(0.0,  step.y));
        half4 e = image.eval(xy + float2( step.x, 0.0));
        half4 w = image.eval(xy + float2(-step.x, 0.0));

        half3 detail = c.rgb * 4.0 - n.rgb - s.rgb - e.rgb - w.rgb;

        half luma = dot(abs(detail), half3(0.2126, 0.7152, 0.0722));
        half adaptiveStrength = strength * clamp(1.0 - luma * 2.0, 0.0, 1.0);

        half3 result = c.rgb + detail * adaptiveStrength;
        return half4(clamp(result, 0.0, c.a > 0.0 ? c.a : 1.0), c.a);
    }
)");

SharpeningShader::SharpeningShader(RuntimeEffectManager& effectManager) {
    mEffect = effectManager.mKnownEffects[kSharpeningEffect];
}

sk_sp<SkShader> SharpeningShader::apply(sk_sp<SkShader> input, float width, float height) {
    if (!mEffect || width <= 0 || height <= 0) {
        return input;
    }

    if (mBuilder == nullptr) {
        mBuilder = std::make_unique<SkRuntimeShaderBuilder>(mEffect);
    }

    mBuilder->child("image") = std::move(input);
    mBuilder->uniform("texSize") = SkPoint{width, height};
    mBuilder->uniform("strength") = 0.15f;

    return mBuilder->makeShader();
}

} // namespace skia
} // namespace renderengine
} // namespace android
