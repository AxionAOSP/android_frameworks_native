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

#include <renderengine/AxRenderEngineMedia.h>

#include <renderengine/ExternalTexture.h>
#include <renderengine/LayerSettings.h>

#include <hardware/gralloc.h>
#include <hardware/gralloc1.h>
#include <system/graphics.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>
#include <utils/Errors.h>

#include <cstdint>
#include <memory>

namespace android::renderengine {
namespace {

constexpr uint32_t kVendorPixelFormatMask = 0x7f000000;
constexpr uint32_t kVendorPixelFormatValue = 0x7f000000;

bool isLimitedRangeDataspace(ui::Dataspace dataspace) {
    const int32_t value = static_cast<int32_t>(dataspace);
    return (value & HAL_DATASPACE_RANGE_MASK) == HAL_DATASPACE_RANGE_LIMITED;
}

bool isVideoDataspace(ui::Dataspace dataspace) {
    const int32_t value = static_cast<int32_t>(dataspace);
    const int32_t standard = value & HAL_DATASPACE_STANDARD_MASK;
    const int32_t transfer = value & HAL_DATASPACE_TRANSFER_MASK;

    switch (standard) {
        case HAL_DATASPACE_STANDARD_BT601_625:
        case HAL_DATASPACE_STANDARD_BT601_625_UNADJUSTED:
        case HAL_DATASPACE_STANDARD_BT601_525:
        case HAL_DATASPACE_STANDARD_BT601_525_UNADJUSTED:
            return true;
        case HAL_DATASPACE_STANDARD_BT709:
            return transfer == HAL_DATASPACE_TRANSFER_SMPTE_170M ||
                    isLimitedRangeDataspace(dataspace);
        case HAL_DATASPACE_STANDARD_BT2020:
        case HAL_DATASPACE_STANDARD_BT2020_CONSTANT_LUMINANCE:
            return true;
        default:
            return false;
    }
}

bool isWideColorDataspace(ui::Dataspace dataspace) {
    const int32_t value = static_cast<int32_t>(dataspace);
    const int32_t standard = value & HAL_DATASPACE_STANDARD_MASK;

    switch (standard) {
        case HAL_DATASPACE_STANDARD_BT2020:
        case HAL_DATASPACE_STANDARD_BT2020_CONSTANT_LUMINANCE:
        case HAL_DATASPACE_STANDARD_FILM:
        case HAL_DATASPACE_STANDARD_DCI_P3:
        case HAL_DATASPACE_STANDARD_ADOBE_RGB:
            return true;
        default:
            return false;
    }
}

bool isVendorPixelFormat(PixelFormat format) {
    return (static_cast<uint32_t>(format) & kVendorPixelFormatMask) == kVendorPixelFormatValue;
}

bool isYuvPixelFormat(PixelFormat format) {
    switch (format) {
        case HAL_PIXEL_FORMAT_YCBCR_422_SP:
        case HAL_PIXEL_FORMAT_YCRCB_420_SP:
        case HAL_PIXEL_FORMAT_YCBCR_422_I:
        case HAL_PIXEL_FORMAT_YCbCr_420_888:
        case HAL_PIXEL_FORMAT_Y8:
        case HAL_PIXEL_FORMAT_Y16:
        case HAL_PIXEL_FORMAT_YV12:
        case HAL_PIXEL_FORMAT_YCBCR_P010:
            return true;
        default:
            return isVendorPixelFormat(format);
    }
}

bool isHdrRgbPixelFormat(PixelFormat format) {
    switch (format) {
        case PIXEL_FORMAT_RGBA_FP16:
        case PIXEL_FORMAT_RGBA_1010102:
        case PIXEL_FORMAT_RGBA_10101010:
        case PIXEL_FORMAT_BGRA_1010102:
        case PIXEL_FORMAT_BGRX_1010102:
            return true;
        default:
            return false;
    }
}

bool hasVisualMediaUsage(uint64_t usage) {
    return (usage & (GRALLOC_USAGE_HW_VIDEO_ENCODER | GRALLOC_USAGE_HW_CAMERA_WRITE |
                     GRALLOC_USAGE_HW_CAMERA_READ | GRALLOC_USAGE_HW_IMAGE_ENCODER |
                     GRALLOC1_PRODUCER_USAGE_VIDEO_DECODER)) != 0;
}

bool getBufferDataspace(const std::shared_ptr<ExternalTexture>& buffer,
                        ui::Dataspace* dataspace) {
    if (!buffer || !buffer->getBuffer()) {
        return false;
    }
    return buffer->getBuffer()->getDataspace(dataspace) == OK;
}

bool hasVisualMediaBufferProperties(const std::shared_ptr<ExternalTexture>& buffer,
                                    ui::Dataspace dataspace, float maxLuminanceNits) {
    if (!buffer) {
        return false;
    }

    const PixelFormat format = buffer->getPixelFormat();
    return isVideoDataspace(dataspace) || isYuvPixelFormat(format) ||
            hasVisualMediaUsage(buffer->getUsage()) || maxLuminanceNits > 0.0f ||
            (isHdrRgbPixelFormat(format) && isWideColorDataspace(dataspace)) ||
            (format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED && isWideColorDataspace(dataspace));
}

bool isVisualMediaBuffer(const std::shared_ptr<ExternalTexture>& buffer, ui::Dataspace dataspace,
                         float maxLuminanceNits) {
    if (!buffer) {
        return false;
    }

    ui::Dataspace bufferDataspace = ui::Dataspace::UNKNOWN;
    const bool hasBufferDataspace = getBufferDataspace(buffer, &bufferDataspace);

    return hasVisualMediaBufferProperties(buffer, dataspace, maxLuminanceNits) ||
            (hasBufferDataspace &&
             hasVisualMediaBufferProperties(buffer, bufferDataspace, maxLuminanceNits));
}

}

AxBufferContentHint AxRenderEngineMedia::getContentHint(const Buffer& buffer,
                                                        ui::Dataspace dataspace,
                                                        bool hasPictureProfile,
                                                        float currentHdrSdrRatio,
                                                        float desiredHdrSdrRatio) {
    if (hasPictureProfile || currentHdrSdrRatio > 1.0f || desiredHdrSdrRatio > 1.0f ||
        hasVisualMediaBufferProperties(buffer.buffer, dataspace, buffer.maxLuminanceNits)) {
        return AxBufferContentHint::VisualMedia;
    }
    return AxBufferContentHint::Unknown;
}

bool AxRenderEngineMedia::hasVisualMediaContent(const Buffer& buffer, ui::Dataspace dataspace) {
    return buffer.axContentHint == AxBufferContentHint::VisualMedia ||
            isVisualMediaBuffer(buffer.buffer, dataspace, buffer.maxLuminanceNits);
}

bool AxRenderEngineMedia::hasVisualMediaContent(const LayerSettings& layer) {
    return hasVisualMediaContent(layer.source.buffer, layer.sourceDataspace);
}

}
