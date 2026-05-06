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

#include <scheduler/Fps.h>
#include <utils/Timers.h>

namespace android::scheduler::SfCpuPolicy {

struct Config {
    nsecs_t vsyncPeriod;
    int timeout;
    nsecs_t midVsyncPeriod;
    int midTimeout;

    Config(nsecs_t vp = 0, int t = 0, nsecs_t mvp = 0, int mt = 0)
          : vsyncPeriod(vp), timeout(t), midVsyncPeriod(mvp), midTimeout(mt) {}
};

void registerMainThread();

void onFrameStart(nsecs_t timestamp, nsecs_t vsyncPeriod);
void onFrameEnd(nsecs_t timestamp);
void onSpeedUpRE(int tid);
void notifyHwcHwbinderTid();

void onRefreshRateChanged(Fps fps);
void onPerformanceMode(bool enabled);
void onPowerSuspend(bool suspended);
void onVpLpEnable(bool enabled);
void onScreenRecording(bool recording);
void onForeground(bool foreground);

void setupConfig(const Config& config);

} // namespace android::scheduler::SfCpuPolicy
