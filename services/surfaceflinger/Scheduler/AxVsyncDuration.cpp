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

#undef LOG_TAG
#define LOG_TAG "AxVsyncDuration"

#include "AxVsyncDuration.h"

#include <cinttypes>
#include <cstdlib>

#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <cutils/properties.h>
#include <log/log.h>

namespace android {

namespace {

constexpr const char* kPropBasePath = "persist.sys.sf.dynamic_duration.";

constexpr const char* kPropSfDecouple = "persist.sys.sf.dynamic_duration.sf.decouple";
constexpr const char* kPropAppDecouple = "persist.sys.sf.dynamic_duration.app.decouple";
constexpr const char* kPropDynamicSwitch = "persist.sys.sf.dynamic_duration.switch";
constexpr const char* kPropSwitchDecouple = "persist.sys.sf.dynamic_duration.switch.decouple";
constexpr const char* kPropExtraBuffers = "persist.sys.sf.dynamic_duration.extra_buffers";

nsecs_t alignOffset(nsecs_t workDuration, nsecs_t totalDuration, nsecs_t vsyncPeriod) {
    if (vsyncPeriod <= 0) return 0;
    const nsecs_t floor = (totalDuration / vsyncPeriod) * vsyncPeriod;
    const nsecs_t pad = (vsyncPeriod <= workDuration) ? 0 : vsyncPeriod;
    return pad + (floor - totalDuration);
}

}

AxVsyncDuration& AxVsyncDuration::getInstance() {
    static AxVsyncDuration instance;
    return instance;
}

AxVsyncDuration::AxVsyncDuration()
      : mDynamicSwitchEnabled(false),
        mDecoupleEnabled(false),
        mDecoupleModeState(false),
        mDecoupleModeUpdate(false),
        mSfDecoupleDuration(0),
        mAppDecoupleDuration(0),
        mSfDecoupleOffset(0),
        mAppDecoupleOffset(0),
        mVsyncPeriod(0),
        mExtraBuffers(0),
        mDefaultExtraVsync(0),
        mActiveDisplayId(PhysicalDisplayId{}) {
    mDynamicSwitchEnabled = base::GetBoolProperty(kPropDynamicSwitch, false);

    if (mDynamicSwitchEnabled) {
        mSfDecoupleDuration = loadAxVsyncDurationProperty(kPropSfDecouple);
        mAppDecoupleDuration = loadAxVsyncDurationProperty(kPropAppDecouple);
        mExtraBuffers = base::GetIntProperty(kPropExtraBuffers, 1);
    }

    mDecoupleEnabled = base::GetBoolProperty(kPropSwitchDecouple, false);
}

long AxVsyncDuration::loadAxVsyncDurationProperty(const char* name) {
    if (!name) return -1;
    return base::GetIntProperty(name, -1);
}

bool AxVsyncDuration::isDynamicSwitchEnabled() const {
    return mDynamicSwitchEnabled;
}

bool AxVsyncDuration::isDecoupleActive() const {
    std::lock_guard lock(mDecoupleMutex);
    return mDynamicSwitchEnabled && mDecoupleEnabled && mSfDecoupleDuration > 0 &&
           mAppDecoupleDuration > 0 && !mDecoupleModeUpdate;
}

nsecs_t AxVsyncDuration::loadPerHzDuration(const char* prefix, nsecs_t vsyncPeriod,
                                           nsecs_t fallback) const {
    if (!mDynamicSwitchEnabled || !prefix || vsyncPeriod <= 0) return fallback;

    const int hz = static_cast<int>((1000000000LL + vsyncPeriod / 2) / vsyncPeriod);

    std::string key = std::string(kPropBasePath) + prefix + std::to_string(hz);
    long val = loadAxVsyncDurationProperty(key.c_str());
    if (val > 0) return val;

    return fallback;
}

scheduler::VsyncConfig AxVsyncDuration::getDecoupleConfig(
        const scheduler::VsyncConfig& fallback) const {
    std::lock_guard lock(mDecoupleMutex);
    if (mSfDecoupleDuration > 0 && mAppDecoupleDuration > 0) {
        scheduler::VsyncConfig result;
        result.sfOffset = mSfDecoupleOffset;
        result.appOffset = mAppDecoupleOffset;
        result.sfWorkDuration = std::chrono::nanoseconds(mSfDecoupleDuration);
        result.appWorkDuration = std::chrono::nanoseconds(mAppDecoupleDuration);
        return result;
    }
    return fallback;
}

void AxVsyncDuration::updateDecoupleDurations(nsecs_t vsyncPeriod) {
    if (vsyncPeriod <= 0) return;
    mVsyncPeriod = vsyncPeriod;

    std::lock_guard lock(mDecoupleMutex);
    if (mSfDecoupleDuration > 0 && mAppDecoupleDuration > 0) {
        const nsecs_t total = mSfDecoupleDuration + mAppDecoupleDuration;
        mSfDecoupleOffset = alignOffset(mSfDecoupleDuration, mSfDecoupleDuration, vsyncPeriod);
        mAppDecoupleOffset = alignOffset(mAppDecoupleDuration, total, vsyncPeriod);
    }
}

uint32_t AxVsyncDuration::onDisplayRefresh() {
    return isDecoupleModeChange() ? 1 : 0;
}

bool AxVsyncDuration::isDecoupleModeChange() {
    std::lock_guard lock(mDecoupleMutex);
    if (!mDecoupleEnabled) return false;

    const bool update = getDecoupleModeUpdate();
    if (mDecoupleModeState == update) return false;

    mDecoupleModeState = update;
    return true;
}

bool AxVsyncDuration::getDecoupleModeUpdate() {
    std::lock_guard lock(mDisplayMutex);
    for (const auto& display : mDisplays) {
        if (display.decoupleEnabled) return true;
    }
    return false;
}

bool AxVsyncDuration::getDecoupleModeLocked(PhysicalDisplayId displayId) const {
    std::lock_guard lock(mDisplayMutex);
    for (const auto& display : mDisplays) {
        if (display.displayId == displayId) return display.decoupleEnabled;
    }
    return false;
}

size_t AxVsyncDuration::getActiveDisplayIndex() const {
    std::lock_guard lock(mDisplayMutex);
    for (size_t i = 0; i < mDisplays.size(); ++i) {
        if (mDisplays[i].displayId == mActiveDisplayId) return i;
    }
    return 0;
}

uint32_t AxVsyncDuration::getExtraVsyncCount() const {
    std::lock_guard lock(mDisplayMutex);
    if (mDecoupleModeUpdate) return 0;

    for (const auto& req : mExtraVsyncRequests) {
        if (req.displayId == mActiveDisplayId) {
            return req.count > 0 ? req.count : mDefaultExtraVsync;
        }
    }
    return mDefaultExtraVsync;
}

int AxVsyncDuration::getExtraBuffers() const {
    if (mDecoupleModeUpdate) return 0;
    return mExtraBuffers;
}

void AxVsyncDuration::setDecoupleMode(PhysicalDisplayId displayId, bool enabled) {
    std::lock_guard lock(mDisplayMutex);
    for (auto& display : mDisplays) {
        if (display.displayId == displayId) {
            display.decoupleEnabled = enabled;
            return;
        }
    }
}

void AxVsyncDuration::updateActiveDisplayId(PhysicalDisplayId displayId) {
    std::lock_guard lock(mDisplayMutex);
    mActiveDisplayId = displayId;
}

void AxVsyncDuration::onNewInternalDisplay(PhysicalDisplayId displayId) {
    std::lock_guard lock(mDisplayMutex);
    for (const auto& display : mDisplays) {
        if (display.displayId == displayId) return;
    }
    mDisplays.push_back({displayId, false, 0});
}

void AxVsyncDuration::decreTransactionFrames() {
    std::lock_guard lock(mDisplayMutex);
    for (auto& req : mExtraVsyncRequests) {
        if (req.displayId == mActiveDisplayId && req.count > 0) {
            req.count--;
        }
    }
}

void AxVsyncDuration::dump(std::string& result) const {
    using base::StringAppendF;
    StringAppendF(&result, "AxVsyncDuration:\n");
    StringAppendF(&result, "  dynamic switch: %d\n", mDynamicSwitchEnabled);
    StringAppendF(&result, "  decouple enabled: %d\n", mDecoupleEnabled);
    StringAppendF(&result, "  decouple mode state: %d\n", mDecoupleModeState);
    StringAppendF(&result, "  sf decouple duration: %lld\n", (long long)mSfDecoupleDuration);
    StringAppendF(&result, "  app decouple duration: %lld\n", (long long)mAppDecoupleDuration);
    StringAppendF(&result, "  vsync period: %lld\n", (long long)mVsyncPeriod);
    StringAppendF(&result, "  extra buffers: %d\n", mExtraBuffers);
    StringAppendF(&result, "  displays: %zu\n", mDisplays.size());
}

} // namespace android
