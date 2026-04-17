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
#define LOG_TAG "SfCpuPolicy"

#include "SfCpuPolicy.h"

#include <atomic>
#include <cstdio>
#include <linux/sched.h>
#include <linux/sched/types.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <android-base/properties.h>
#include <cutils/properties.h>
#include <log/log.h>

namespace android::scheduler::SfCpuPolicy {

namespace {

constexpr const char* kUpperBoundProp = "persist.sys.sf.cpupolicy.upbound_uclamp_min";
constexpr const char* kLowerBoundProp = "persist.sys.sf.cpupolicy.lowbound_uclamp_min";
constexpr const char* kBoostProp = "persist.sys.sf.cpupolicy.min_boost";
constexpr const char* kLogProp = "persist.sys.sf.cpupolicy.log";

constexpr unsigned int kUclampMax = 1024;
constexpr unsigned int kDefaultUpperBound = 344;
constexpr unsigned int kDefaultLowerBound = 106;
constexpr unsigned int kDefaultBoost = 106;
constexpr unsigned int kDefaultMin60 = 82;
constexpr unsigned int kDefaultMin90 = 102;
constexpr unsigned int kDefaultMin120 = 106;

std::atomic<int> sMainTid{0};
std::atomic<int> sCurrentHz{0};
std::atomic<bool> sPerfMode{false};

int hzBucket(Fps fps) {
    const int hz = static_cast<int>(fps.getValue() + 0.5f);
    if (hz >= 110) return 120;
    if (hz >= 80) return 90;
    if (hz >= 50) return 60;
    return hz;
}

unsigned int readUint(const char* key, unsigned int fallback) {
    return base::GetUintProperty<unsigned int>(key, fallback);
}

unsigned int defaultPerHzMin(int hz) {
    switch (hz) {
        case 60: return kDefaultMin60;
        case 90: return kDefaultMin90;
        case 120: return kDefaultMin120;
        default: return 0;
    }
}

unsigned int perHzMin(int hz) {
    char key[96];
    snprintf(key, sizeof(key), "persist.sys.sf.cpupolicy.min_%d", hz);
    return readUint(key, defaultPerHzMin(hz));
}

unsigned int computeUclampMin(int hz, bool perf) {
    const unsigned int upper = readUint(kUpperBoundProp, kDefaultUpperBound);
    const unsigned int lower = readUint(kLowerBoundProp, kDefaultLowerBound);
    if (!perf) return 0;

    unsigned int target = perHzMin(hz);
    if (target == 0) target = readUint(kBoostProp, kDefaultBoost);
    if (target < lower) target = lower;
    if (target > upper) target = upper;
    return target;
}

void writeUclamp(unsigned int uclampMin) {
    const int tid = sMainTid.load(std::memory_order_relaxed);
    sched_attr attr = {};
    attr.size = sizeof(attr);
    attr.sched_flags = (SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP);
    attr.sched_util_min = uclampMin;
    attr.sched_util_max = kUclampMax;
    if (syscall(__NR_sched_setattr, tid, &attr, 0)) {
        ALOGW("sched_setattr uclamp_min=%u tid=%d failed: %s", uclampMin, tid, strerror(errno));
        return;
    }
    if (property_get_bool(kLogProp, false)) {
        ALOGI("uclamp_min=%u tid=%d hz=%d perf=%d", uclampMin, tid,
              sCurrentHz.load(std::memory_order_relaxed),
              sPerfMode.load(std::memory_order_relaxed));
    }
}

void apply() {
    if (sMainTid.load(std::memory_order_relaxed) == 0) return;
    const int hz = sCurrentHz.load(std::memory_order_relaxed);
    const bool perf = sPerfMode.load(std::memory_order_relaxed);
    writeUclamp(computeUclampMin(hz, perf));
}

}

void registerMainThread() {
    sMainTid.store(gettid(), std::memory_order_relaxed);
}

void onRefreshRateChanged(Fps fps) {
    sCurrentHz.store(hzBucket(fps), std::memory_order_relaxed);
    apply();
}

void onPerformanceMode(bool enabled) {
    sPerfMode.store(enabled, std::memory_order_relaxed);
    apply();
}

}
