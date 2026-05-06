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

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <scheduler/VsyncConfig.h>
#include <ui/DisplayId.h>
#include <utils/Timers.h>

namespace android {

class AxVsyncDuration {
public:
    static AxVsyncDuration& getInstance();

    bool isDynamicSwitchEnabled() const;

    bool isDecoupleActive() const;
    nsecs_t loadPerHzDuration(const char* prefix, nsecs_t vsyncPeriod, nsecs_t fallback) const;

    scheduler::VsyncConfig getDecoupleConfig(const scheduler::VsyncConfig& fallback) const;

    uint32_t onDisplayRefresh();
    uint32_t getExtraVsyncCount() const;
    int getExtraBuffers() const;

    void setDecoupleMode(PhysicalDisplayId displayId, bool enabled);

    void updateActiveDisplayId(PhysicalDisplayId displayId);
    void onNewInternalDisplay(PhysicalDisplayId displayId);

    void updateDecoupleDurations(nsecs_t vsyncPeriod);
    void decreTransactionFrames();

    void dump(std::string& result) const;

private:
    AxVsyncDuration();
    ~AxVsyncDuration() = default;
    AxVsyncDuration(const AxVsyncDuration&) = delete;
    AxVsyncDuration& operator=(const AxVsyncDuration&) = delete;

    bool isDecoupleModeChange();
    bool getDecoupleModeUpdate();
    bool getDecoupleModeLocked(PhysicalDisplayId displayId) const;
    size_t getActiveDisplayIndex() const;

    static long loadAxVsyncDurationProperty(const char* name);

    struct ExtraVsyncRequest {
        PhysicalDisplayId displayId;
        uint32_t count;
    };

    struct DisplayMML {
        PhysicalDisplayId displayId;
        bool decoupleEnabled;
        int extraVsyncCount;
    };

    bool mDynamicSwitchEnabled;
    bool mDecoupleEnabled;

    bool mDecoupleModeState;
    bool mDecoupleModeUpdate;

    nsecs_t mSfDecoupleDuration;
    nsecs_t mAppDecoupleDuration;
    nsecs_t mSfDecoupleOffset;
    nsecs_t mAppDecoupleOffset;

    nsecs_t mVsyncPeriod;

    int mExtraBuffers;
    uint32_t mDefaultExtraVsync;

    std::vector<DisplayMML> mDisplays;
    PhysicalDisplayId mActiveDisplayId;

    std::vector<ExtraVsyncRequest> mExtraVsyncRequests;

    mutable std::mutex mDecoupleMutex;
    mutable std::mutex mDisplayMutex;
};

} // namespace android
