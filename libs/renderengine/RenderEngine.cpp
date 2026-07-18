/*
 * Copyright 2013 The Android Open Source Project
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

#include <renderengine/RenderEngine.h>

#include "renderengine/AxRenderEngineMedia.h"
#include "renderengine/ExternalTexture.h"
#include "skia/GaneshVkRenderEngine.h"
#include "skia/GraphiteVkRenderEngine.h"
#include "skia/SkiaGLRenderEngine.h"
#include "threaded/RenderEngineThreaded.h"
#include "ui/GraphicTypes.h"

#include <android-base/properties.h>
#include <com_android_graphics_surfaceflinger_flags.h>
#include <cutils/properties.h>
#include <ftl/enum.h>
#include <log/log.h>

#include <algorithm>
#include <initializer_list>

namespace android {
namespace renderengine {

namespace {

constexpr const char* kUseOpenGlForMediaProperty = "persist.sys.vk_use_ogl_for_media";

bool hasProtectedBuffer(const std::shared_ptr<ExternalTexture>& buffer) {
    return buffer && (buffer->getUsage() & GRALLOC_USAGE_PROTECTED);
}

bool hasMediaBuffer(const LayerSettings& layer) {
    return AxRenderEngineMedia::hasVisualMediaContent(layer);
}

bool hasProtectedBuffer(const LayerSettings& layer) {
    return hasProtectedBuffer(layer.source.buffer.buffer);
}

bool hasMediaBuffer(const std::vector<LayerSettings>& layers) {
    return std::any_of(layers.begin(), layers.end(),
                       [](const LayerSettings& layer) { return hasMediaBuffer(layer); });
}

bool hasProtectedBuffer(const std::vector<LayerSettings>& layers) {
    return std::any_of(layers.begin(), layers.end(),
                       [](const LayerSettings& layer) { return hasProtectedBuffer(layer); });
}

bool hasProtectedBuffer(std::initializer_list<const ExternalTexture*> buffers) {
    return std::any_of(buffers.begin(), buffers.end(),
                       [](const ExternalTexture* buffer) {
                           return buffer && (buffer->getUsage() & GRALLOC_USAGE_PROTECTED);
                       });
}

RenderEngineCreationArgs createMediaFallbackArgs(const RenderEngineCreationArgs& args);

std::unique_ptr<RenderEngine> createRenderEngineForArgs(const RenderEngineCreationArgs& args) {
    if (args.skiaBackend == RenderEngine::SkiaBackend::Graphite) {
        return android::renderengine::skia::GraphiteVkRenderEngine::create(args);
    } else { // GANESH
        if (args.graphicsApi == RenderEngine::GraphicsApi::Vk) {
            auto engine = android::renderengine::skia::GaneshVkRenderEngine::create(args);
            if (engine) {
                return engine;
            }
            ALOGE("Falling back to OpenGL RenderEngine after Vulkan context creation failed");
            return android::renderengine::skia::SkiaGLRenderEngine::create(
                    createMediaFallbackArgs(args));
        } else { // GL
            return android::renderengine::skia::SkiaGLRenderEngine::create(args);
        }
    }
}

threaded::CreateInstanceFactory createInstanceFactoryForArgs(
        const RenderEngineCreationArgs& args) {
    return [args]() { return createRenderEngineForArgs(args); };
}

RenderEngineCreationArgs createMediaFallbackArgs(const RenderEngineCreationArgs& args) {
    return RenderEngineCreationArgs::Builder()
            .setPixelFormat(args.pixelFormat)
            .setImageCacheSize(args.imageCacheSize)
            .setEnableProtectedContext(args.enableProtectedContext)
            .setPrecacheToneMapperShaderOnly(args.precacheToneMapperShaderOnly)
            .setBlurAlgorithm(args.blurAlgorithm)
            .setContextPriority(args.contextPriority)
            .setThreaded(args.threaded)
            .setGraphicsApi(RenderEngine::GraphicsApi::GL)
            .setSkiaBackend(RenderEngine::SkiaBackend::Ganesh)
            .build();
}

} // namespace

class MediaFallbackRenderEngine final : public RenderEngine {
public:
    MediaFallbackRenderEngine(std::unique_ptr<RenderEngine> primary,
                              std::unique_ptr<RenderEngine> media)
          : RenderEngine(Threaded::Yes), mPrimary(std::move(primary)), mMedia(std::move(media)) {}

    std::future<void> primeCache(PrimeCacheConfig config) override {
        auto result = mPrimary->primeCache(config);
        static_cast<void>(mMedia->primeCache(config));
        return result;
    }

    void dump(std::string& result) override {
        mPrimary->dump(result);
        mMedia->dump(result);
    }

    size_t getMaxTextureSize() const override {
        return std::min(mPrimary->getMaxTextureSize(), mMedia->getMaxTextureSize());
    }

    size_t getMaxViewportDims() const override {
        return std::min(mPrimary->getMaxViewportDims(), mMedia->getMaxViewportDims());
    }

    bool supportsProtectedContent() const override { return mPrimary->supportsProtectedContent(); }

    void onActiveDisplaySizeChanged(ui::Size size) override {
        mPrimary->onActiveDisplaySizeChanged(size);
        mMedia->onActiveDisplaySizeChanged(size);
    }

    void cleanupPostRender() override {
        mPrimary->cleanupPostRender();
        mMedia->cleanupPostRender();
    }

    int getContextPriority() override { return mPrimary->getContextPriority(); }

    bool supportsBackgroundBlur() override {
        return mPrimary->supportsBackgroundBlur() && mMedia->supportsBackgroundBlur();
    }

    void setEnableTracing(bool tracingEnabled) override {
        mPrimary->setEnableTracing(tracingEnabled);
        mMedia->setEnableTracing(tracingEnabled);
    }

    void rdocCaptureNextFrame() override {
        mPrimary->rdocCaptureNextFrame();
        mMedia->rdocCaptureNextFrame();
    }

protected:
    void mapExternalTextureBuffer(const sp<GraphicBuffer>& buffer, bool isRenderable) override {
        mPrimary->mapExternalTextureBuffer(buffer, isRenderable);
        mMedia->mapExternalTextureBuffer(buffer, isRenderable);
    }

    void unmapExternalTextureBuffer(sp<GraphicBuffer>&& buffer) override {
        sp<GraphicBuffer> mediaBuffer = buffer;
        mPrimary->unmapExternalTextureBuffer(std::move(buffer));
        mMedia->unmapExternalTextureBuffer(std::move(mediaBuffer));
    }

    bool canSkipPostRenderCleanup() const override {
        return mPrimary->canSkipPostRenderCleanup() && mMedia->canSkipPostRenderCleanup();
    }

    void useProtectedContext(bool useProtectedContext) override {
        mPrimary->useProtectedContext(useProtectedContext);
        mMedia->useProtectedContext(useProtectedContext);
    }

    void drawLayersInternal(
            const std::shared_ptr<std::promise<FenceResult>>&& resultPromise,
            const DisplaySettings& display, const std::vector<LayerSettings>& layers,
            const std::shared_ptr<ExternalTexture>& buffer, base::unique_fd&& bufferFence) override {
        auto result = selectRenderEngine(layers).drawLayers(display, layers, buffer,
                                                            std::move(bufferFence)).get();
        resultPromise->set_value(std::move(result));
    }

    void tonemapAndDrawGainmapInternal(
            const std::shared_ptr<std::promise<FenceResult>>&& resultPromise,
            const std::shared_ptr<ExternalTexture>& hdr, base::borrowed_fd&& hdrFence,
            float hdrSdrRatio, ui::Dataspace dataspace, const std::shared_ptr<ExternalTexture>& sdr,
            const std::shared_ptr<ExternalTexture>& gainmap) override {
        auto result = selectGainmapRenderEngine({hdr.get(), sdr.get(), gainmap.get()})
                              ->tonemapAndDrawGainmap(hdr, std::move(hdrFence), hdrSdrRatio,
                                                      dataspace, sdr, gainmap)
                              .get();
        resultPromise->set_value(std::move(result));
    }

private:
    RenderEngine& selectRenderEngine(const std::vector<LayerSettings>& layers) const {
        if (hasMediaBuffer(layers) &&
            (!hasProtectedBuffer(layers) || mMedia->supportsProtectedContent())) {
            return *mMedia;
        }
        return *mPrimary;
    }

    RenderEngine* selectGainmapRenderEngine(
            std::initializer_list<const ExternalTexture*> buffers) const {
        if (!hasProtectedBuffer(buffers) || mMedia->supportsProtectedContent()) {
            return mMedia.get();
        }
        return mPrimary.get();
    }

    std::unique_ptr<RenderEngine> mPrimary;
    std::unique_ptr<RenderEngine> mMedia;
};

// TODO: b/341728634 - Don't compile Ganesh unless requested once Graphite is the stable default.
std::unique_ptr<RenderEngine> RenderEngine::create(const RenderEngineCreationArgs& args) {
    ALOGD("%sRenderEngine with Skia%s Backend (%s)",
          args.threaded == Threaded::Yes ? "Threaded " : "",
          ftl::enum_string(args.graphicsApi).c_str(), ftl::enum_string(args.skiaBackend).c_str());

    if (args.threaded != Threaded::Yes) {
        ALOGE("Non-threaded RenderEngine not supported, please update or "
              "remove " PROPERTY_DEBUG_RENDERENGINE_BACKEND " to use a supported backend");
    }

    auto createInstanceFactory = createInstanceFactoryForArgs(args);
    if (args.graphicsApi == GraphicsApi::Vk &&
        base::GetBoolProperty(kUseOpenGlForMediaProperty, false)) {
        const auto mediaArgs = createMediaFallbackArgs(args);
        createInstanceFactory = [args, mediaArgs]() -> std::unique_ptr<RenderEngine> {
            auto primary = createRenderEngineForArgs(args);
            auto media = createRenderEngineForArgs(mediaArgs);
            if (!primary || !media) {
                return primary;
            }
            return std::make_unique<MediaFallbackRenderEngine>(std::move(primary),
                                                               std::move(media));
        };
    }

    return renderengine::threaded::RenderEngineThreaded::create(createInstanceFactory);
}

RenderEngine::~RenderEngine() = default;

void RenderEngine::validateInputBufferUsage(const sp<GraphicBuffer>& buffer) {
    LOG_ALWAYS_FATAL_IF(!(buffer->getUsage() & GraphicBuffer::USAGE_HW_TEXTURE),
                        "input buffer not gpu readable");
}

void RenderEngine::validateOutputBufferUsage(const sp<GraphicBuffer>& buffer) {
    LOG_ALWAYS_FATAL_IF(!(buffer->getUsage() & GraphicBuffer::USAGE_HW_RENDER),
                        "output buffer not gpu writeable");
}

ftl::Future<FenceResult> RenderEngine::drawLayers(const DisplaySettings& display,
                                                  const std::vector<LayerSettings>& layers,
                                                  const std::shared_ptr<ExternalTexture>& buffer,
                                                  base::unique_fd&& bufferFence) {
    const auto resultPromise = std::make_shared<std::promise<FenceResult>>();
    std::future<FenceResult> resultFuture = resultPromise->get_future();
    updateProtectedContext(layers, {buffer.get()});
    drawLayersInternal(std::move(resultPromise), display, layers, buffer, std::move(bufferFence));
    return resultFuture;
}

ftl::Future<FenceResult> RenderEngine::tonemapAndDrawGainmap(
        const std::shared_ptr<ExternalTexture>& hdr, base::borrowed_fd&& hdrFence,
        float hdrSdrRatio, ui::Dataspace dataspace, const std::shared_ptr<ExternalTexture>& sdr,
        const std::shared_ptr<ExternalTexture>& gainmap) {
    const auto resultPromise = std::make_shared<std::promise<FenceResult>>();
    std::future<FenceResult> resultFuture = resultPromise->get_future();
    updateProtectedContext({}, {sdr.get(), hdr.get(), gainmap.get()});
    tonemapAndDrawGainmapInternal(std::move(resultPromise), hdr, std::move(hdrFence), hdrSdrRatio,
                                  dataspace, sdr, gainmap);
    return resultFuture;
}

void RenderEngine::updateProtectedContext(const std::vector<LayerSettings>& layers,
                                          vector<const ExternalTexture*> buffers) {
    const bool needsProtectedContext =
            std::any_of(layers.begin(), layers.end(),
                        [](const LayerSettings& layer) {
                            const std::shared_ptr<ExternalTexture>& buffer =
                                    layer.source.buffer.buffer;
                            return buffer && (buffer->getUsage() & GRALLOC_USAGE_PROTECTED);
                        }) ||
            std::any_of(buffers.begin(), buffers.end(), [](const ExternalTexture* buffer) {
                return buffer && (buffer->getUsage() & GRALLOC_USAGE_PROTECTED);
            });
    useProtectedContext(needsProtectedContext);
}

} // namespace renderengine
} // namespace android
