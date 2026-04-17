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
#include <cerrno>
#include <cstring>
#include <linux/sched.h>
#include <linux/sched/types.h>
#include <mutex>
#include <sys/syscall.h>
#include <unistd.h>

#include <android-base/properties.h>
#include <log/log.h>

#include "ax_process_utils.h"

namespace android::scheduler::SfCpuPolicy {

namespace {

constexpr nsecs_t kNsPerSec = 1000000000LL;
constexpr nsecs_t kFrameIntervalWindow = 300000000LL;
constexpr nsecs_t kMinFrameInterval = 40000000LL;

constexpr int kAffinityGroupSmall = axion::process::kCpuGroupSmall;
constexpr int kAffinityGroupAll = axion::process::kCpuGroupAll;
constexpr int kAffinityGroupBalanced = axion::process::kCpuGroupBalanced;
constexpr int kAffinityGroupBig = axion::process::kCpuGroupBig;
constexpr int kAffinityGroupPrime = axion::process::kCpuGroupPrime;
constexpr int kAffinityGroupInvalid = -1;
constexpr int kAffinityProfileInvalid = -1;
constexpr int kAffinityProfileTopApp = 0;
constexpr int kAffinityProfileBoost = 1;
constexpr int kUclampInvalid = -1;
constexpr nsecs_t kAffinityWarnInterval = 5LL * kNsPerSec;
constexpr nsecs_t kAffinityRetryInterval = 1LL * kNsPerSec;

constexpr int kFgVsyncPeriodNs = 25000000;
constexpr int kFgTimeoutMs = 30;
constexpr int kFgMidVsyncPeriodNs = 125000000;
constexpr int kFgMidTimeoutMs = 5;

constexpr int kBgVsyncPeriodNs = 25000000;
constexpr int kBgTimeoutMs = 10;
constexpr int kBgMidVsyncPeriodNs = 125000000;
constexpr int kBgMidTimeoutMs = 5;

struct Props {
    unsigned int min60;
    unsigned int min90;
    unsigned int min120;
    unsigned int upboundUclampMin;
    unsigned int lowboundUclampMin;
    int hwCompMin;
    int gpuCompMin;
    int hwCompSuspend;
    int hwHfrSuspend;
    int vpLpSuspend;
    int forceSuspend;
    int powerUp120;
    int powerUp90;
    int powerUpMargin;
    int powerDown120;
    int powerDown90;
    int powerDownMargin;
    int earlyHeavy;
    int refreshAsFps;
    int boostSr;
    int boostEarly;
    bool log;
    nsecs_t powerUpTime;
    nsecs_t powerDownTime;
    nsecs_t midPowerUpTime;
    nsecs_t midPowerDownTime;

    Props()
          : min60(82),
            min90(102),
            min120(106),
            upboundUclampMin(344),
            lowboundUclampMin(106),
            hwCompMin(-1),
            gpuCompMin(-1),
            hwCompSuspend(-1),
            hwHfrSuspend(-1),
            vpLpSuspend(-1),
            forceSuspend(-1),
            powerUp120(-1),
            powerUp90(-1),
            powerUpMargin(-1),
            powerDown120(-1),
            powerDown90(-1),
            powerDownMargin(-1),
            earlyHeavy(2),
            refreshAsFps(-1),
            boostSr(-1),
            boostEarly(-1),
            log(false),
            powerUpTime(25000000),
            powerDownTime(125000000),
            midPowerUpTime(25000000),
            midPowerDownTime(125000000) {}
};

std::atomic<int> sMainTid{0};
std::atomic<int> sHwcTid{0};
std::atomic<int> sReTid{0};
std::atomic<int> sCurrentHz{0};
std::atomic<bool> sPerfMode{false};
std::atomic<bool> sPowerSuspended{false};
std::atomic<bool> sVpLpEnabled{false};
std::atomic<bool> sIsForeground{true};
std::atomic<bool> sScreenRecording{false};
std::atomic<bool> sEarlyFrameBoost{false};
std::atomic<bool> sUclampSupported{true};
std::atomic<int> sCurrentAffinityGroup{kAffinityGroupInvalid};
std::atomic<int> sCurrentAffinityProfile{kAffinityProfileInvalid};
std::atomic<int> sCurrentHwcAffinityGroup{kAffinityGroupInvalid};
std::atomic<int> sCurrentReAffinityGroup{kAffinityGroupInvalid};
std::atomic<int> sCurrentMainUclampMin{kUclampInvalid};
std::atomic<int> sCurrentHwcUclampMin{kUclampInvalid};
std::atomic<int> sCurrentReUclampMin{kUclampInvalid};
std::atomic<int> sFailedAffinityGroup{kAffinityGroupInvalid};
std::atomic<int> sFailedAffinityProfile{kAffinityProfileInvalid};
std::atomic<nsecs_t> sNextAffinityWarn{0};
std::atomic<nsecs_t> sNextAffinityRetry{0};

std::atomic<nsecs_t> sLastFrameStart{0};
std::atomic<nsecs_t> sLastFrameEnd{0};
std::atomic<int> sFrameCount{0};
std::atomic<nsecs_t> sFrameWindowStart{0};
std::atomic<float> sCurrentFps{0.0f};

Config sConfigFg;
Config sConfigBg;
Config sConfigHeavy;
Props sProps;

int hzBucket(float fps) {
    const int hz = static_cast<int>(fps + 0.5f);
    if (hz >= 110) return 120;
    if (hz >= 80) return 90;
    if (hz >= 50) return 60;
    return hz;
}

int hzBucket(Fps fps) {
    return hzBucket(fps.getValue());
}

unsigned int readUint(const char* key, unsigned int fallback) {
    return base::GetUintProperty<unsigned int>(key, fallback);
}

int readInt(const char* key, int fallback) {
    return base::GetIntProperty(key, fallback);
}

unsigned int perHzMin(int hz) {
    switch (hz) {
        case 60: return sProps.min60;
        case 90: return sProps.min90;
        case 120: return sProps.min120;
        default: return 0;
    }
}

void initProps() {
    sProps.min60 = readUint("persist.sys.sf.cpupolicy.min_60", 82);
    sProps.min90 = readUint("persist.sys.sf.cpupolicy.min_90", 102);
    sProps.min120 = readUint("persist.sys.sf.cpupolicy.min_120", 106);
    sProps.upboundUclampMin = readUint("persist.sys.sf.cpupolicy.upbound_uclamp_min", 344);
    sProps.lowboundUclampMin = readUint("persist.sys.sf.cpupolicy.lowbound_uclamp_min", 106);
    sProps.hwCompMin = readInt("persist.sys.sf.cpupolicy.hw_comp_min", -1);
    sProps.gpuCompMin = readInt("persist.sys.sf.cpupolicy.gpu_comp_min", -1);
    sProps.hwCompSuspend = readInt("persist.sys.sf.cpupolicy.hw_comp_suspend", -1);
    sProps.hwHfrSuspend = readInt("persist.sys.sf.cpupolicy.hw_hfr_suspend", -1);
    sProps.vpLpSuspend = readInt("persist.sys.sf.cpupolicy.vp_lp_suspend", -1);
    sProps.forceSuspend = readInt("persist.sys.sf.cpupolicy.force_suspend", -1);
    sProps.powerUp120 = readInt("persist.sys.sf.cpupolicy.power_up_120", -1);
    sProps.powerUp90 = readInt("persist.sys.sf.cpupolicy.power_up_90", -1);
    sProps.powerUpMargin = readInt("persist.sys.sf.cpupolicy.power_up_margin", -1);
    sProps.powerDown120 = readInt("persist.sys.sf.cpupolicy.power_down_120", -1);
    sProps.powerDown90 = readInt("persist.sys.sf.cpupolicy.power_down_90", -1);
    sProps.powerDownMargin = readInt("persist.sys.sf.cpupolicy.power_down_margin", -1);
    sProps.earlyHeavy = readInt("persist.sys.sf.cpupolicy.early_heavy", 2);
    sProps.refreshAsFps = readInt("persist.sys.sf.cpupolicy.refresh_as_fps", -1);
    sProps.boostSr = readInt("persist.sys.sf.cpupolicy.boost_sr", -1);
    sProps.boostEarly = readInt("persist.sys.sf.cpupolicy.boost_early", -1);
    sProps.log = base::GetBoolProperty("persist.sys.sf.cpupolicy.log", false);

    int powerUpTime = readInt("persist.sys.sf.cpupolicy.power_up_time", -1);
    if (powerUpTime > 0) sProps.powerUpTime = powerUpTime;
    int powerDownTime = readInt("persist.sys.sf.cpupolicy.power_down_time", -1);
    if (powerDownTime > 0) sProps.powerDownTime = powerDownTime;
    int midPowerUpTime = readInt("persist.sys.sf.cpupolicy.mid_power_up_time", -1);
    if (midPowerUpTime > 0) sProps.midPowerUpTime = midPowerUpTime;
    int midPowerDownTime = readInt("persist.sys.sf.cpupolicy.mid_power_down_time", -1);
    if (midPowerDownTime > 0) sProps.midPowerDownTime = midPowerDownTime;

    sConfigFg = Config(kFgVsyncPeriodNs, kFgTimeoutMs, kFgMidVsyncPeriodNs, kFgMidTimeoutMs);
    sConfigBg = Config(kBgVsyncPeriodNs, kBgTimeoutMs, kBgMidVsyncPeriodNs, kBgMidTimeoutMs);
    sConfigHeavy = Config(kBgVsyncPeriodNs, kBgTimeoutMs, kBgMidVsyncPeriodNs, kBgMidTimeoutMs);
}

bool shouldSuspend() {
    if (sProps.forceSuspend > 0) return true;
    if (sPowerSuspended.load(std::memory_order_relaxed)) return true;
    if (sVpLpEnabled.load(std::memory_order_relaxed) && sProps.vpLpSuspend > 0) return true;
    if (sProps.hwHfrSuspend > 0) {
        const int hz = sCurrentHz.load(std::memory_order_relaxed);
        if (hz >= 90) return true;
    }
    if (!sIsForeground.load(std::memory_order_relaxed)) {
        const float fps = sCurrentFps.load(std::memory_order_relaxed);
        const int hz = sCurrentHz.load(std::memory_order_relaxed);
        if (fps > 0.0f && hz > 0) {
            const int effectiveHz = (sProps.refreshAsFps > 0) ? static_cast<int>(fps + 0.5f) : hz;
            if (effectiveHz < 60) return true;
        }
    }
    return false;
}

bool checkIfHeavy(bool isHeavyFps, bool isScreenRecording, bool isHeavyLayer) {
    return (isScreenRecording || isHeavyLayer) || (isHeavyFps && sProps.earlyHeavy != 0);
}

bool boostScreenRecord(bool recording, int hz) {
    if (sProps.boostSr <= 0) return false;
    return recording && hz > 60;
}

bool earlyFrameBoostActive(int hz) {
    return sEarlyFrameBoost.load(std::memory_order_relaxed) && hz >= 50;
}

unsigned int computeUclampMin() {
    if (shouldSuspend()) return 0;

    const int hz = sCurrentHz.load(std::memory_order_relaxed);
    if (hz <= 0) return 0;

    unsigned int target = perHzMin(hz);

    const float fps = sCurrentFps.load(std::memory_order_relaxed);
    if (fps > 0.0f && sProps.refreshAsFps > 0) {
        const int fpsBucket = hzBucket(fps);
        const unsigned int fpsTarget = perHzMin(fpsBucket);
        if (fpsTarget > target) target = fpsTarget;
    }

    if (boostScreenRecord(sScreenRecording.load(std::memory_order_relaxed), hz)) {
        target = static_cast<unsigned int>(sProps.boostSr);
    }

    if (earlyFrameBoostActive(hz)) {
        const unsigned int earlyTarget = sProps.boostEarly > 0
                ? static_cast<unsigned int>(sProps.boostEarly)
                : sProps.upboundUclampMin;
        if (earlyTarget > target) target = earlyTarget;
    }

    if (target < sProps.lowboundUclampMin) target = sProps.lowboundUclampMin;
    if (target > sProps.upboundUclampMin) target = sProps.upboundUclampMin;

    return target;
}

int computeAffinityGroup() {
    if (shouldSuspend()) return kAffinityGroupSmall;

    const int hz = sCurrentHz.load(std::memory_order_relaxed);
    if (boostScreenRecord(sScreenRecording.load(std::memory_order_relaxed), hz)) {
        return kAffinityGroupPrime;
    }

    if (earlyFrameBoostActive(hz)) {
        return kAffinityGroupBig;
    }

    if (sPerfMode.load(std::memory_order_relaxed)) {
        return kAffinityGroupBig;
    }

    return kAffinityGroupAll;
}

int profileForGroup(int group) {
    return group == kAffinityGroupBig || group == kAffinityGroupPrime ? kAffinityProfileBoost
                                                                      : kAffinityProfileTopApp;
}

const char* profileName(int profile) {
    return profile == kAffinityProfileBoost ? "CPUSET_SP_TOP_APP" : "ProcessCapacityMax";
}

bool shouldLogAffinityWarning() {
    const nsecs_t now = systemTime();
    if (now < sNextAffinityWarn.load(std::memory_order_relaxed)) return false;
    sNextAffinityWarn.store(now + kAffinityWarnInterval, std::memory_order_relaxed);
    return true;
}

void warnAffinityFailure(const char* message, int tid, int group) {
    if (shouldLogAffinityWarning()) {
        ALOGW("%s tid=%d group=%d", message, tid, group);
    }
}

void warnAffinityProfileFailure(int tid, int profile, const char* name) {
    if (shouldLogAffinityWarning()) {
        ALOGW("SetThreadProfile failed tid=%d profile=%d name=%s", tid, profile, name);
    }
}

bool shouldSkipAffinityAttempt(int group, int profile) {
    if (sFailedAffinityGroup.load(std::memory_order_relaxed) != group) return false;
    if (sFailedAffinityProfile.load(std::memory_order_relaxed) != profile) return false;
    return systemTime() < sNextAffinityRetry.load(std::memory_order_relaxed);
}

void recordAffinityFailure(int group, int profile) {
    sFailedAffinityGroup.store(group, std::memory_order_relaxed);
    sFailedAffinityProfile.store(profile, std::memory_order_relaxed);
    sNextAffinityRetry.store(systemTime() + kAffinityRetryInterval, std::memory_order_relaxed);
}

void clearAffinityFailure() {
    sFailedAffinityGroup.store(kAffinityGroupInvalid, std::memory_order_relaxed);
    sFailedAffinityProfile.store(kAffinityProfileInvalid, std::memory_order_relaxed);
    sNextAffinityRetry.store(0, std::memory_order_relaxed);
}

bool applyAffinityProfile(int tid, int profile) {
    if (sCurrentAffinityProfile.load(std::memory_order_relaxed) == profile) return true;

    const char* name = profileName(profile);
    if (axion::process::SetThreadProfile(tid, name, false)) {
        sCurrentAffinityProfile.store(profile, std::memory_order_relaxed);
        return true;
    }

    warnAffinityProfileFailure(tid, profile, name);
    return false;
}

void applyAffinity() {
    const int tid = sMainTid.load(std::memory_order_relaxed);
    if (tid <= 0) return;

    const int targetGroup = computeAffinityGroup();
    const int targetProfile = profileForGroup(targetGroup);
    const int currentGroup = sCurrentAffinityGroup.load(std::memory_order_relaxed);
    const int currentProfile = sCurrentAffinityProfile.load(std::memory_order_relaxed);
    if (targetGroup == currentGroup && targetProfile == currentProfile) return;
    if (shouldSkipAffinityAttempt(targetGroup, targetProfile)) return;

    const bool profileApplied = applyAffinityProfile(tid, targetProfile);
    if (!profileApplied) {
        recordAffinityFailure(targetGroup, targetProfile);
    }
    sCurrentMainUclampMin.store(kUclampInvalid, std::memory_order_relaxed);

    if (targetGroup == currentGroup) {
        if (profileApplied) {
            clearAffinityFailure();
        }
        return;
    }

    if (axion::process::SetSingleThreadAffinity(tid, targetGroup)) {
        sCurrentAffinityGroup.store(targetGroup, std::memory_order_relaxed);
        if (profileApplied) {
            clearAffinityFailure();
        }
        if (sProps.log) {
            ALOGI("affinity tid=%d group=%d hz=%d", tid, targetGroup,
                  sCurrentHz.load(std::memory_order_relaxed));
        }
    } else {
        recordAffinityFailure(targetGroup, targetProfile);
        warnAffinityFailure("SetSingleThreadAffinity failed", tid, targetGroup);
    }
}

void writeUclampForThread(int tid, unsigned int uclampMin, std::atomic<int>& currentUclamp) {
    if (tid <= 0) return;
    if (!sUclampSupported.load(std::memory_order_relaxed)) return;
    if (currentUclamp.load(std::memory_order_relaxed) == static_cast<int>(uclampMin)) return;

    sched_attr attr = {};
    attr.size = sizeof(attr);
    attr.sched_flags = (SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP_MIN);
    attr.sched_util_min = uclampMin;

    if (syscall(__NR_sched_setattr, tid, &attr, 0)) {
        const int savedErrno = errno;
        if (savedErrno == E2BIG || savedErrno == EINVAL || savedErrno == ENOSYS ||
            savedErrno == EOPNOTSUPP || savedErrno == EPERM) {
            if (sUclampSupported.exchange(false, std::memory_order_relaxed)) {
                ALOGW("sched_setattr uclamp_min unsupported, disabling SfCpuPolicy uclamp writes: %s",
                      strerror(savedErrno));
            }
            return;
        }
        ALOGW("sched_setattr uclamp_min=%u tid=%d failed: %s", uclampMin, tid,
              strerror(savedErrno));
        return;
    }
    currentUclamp.store(static_cast<int>(uclampMin), std::memory_order_relaxed);
}

void applyWorkerAffinity(int tid, int group, std::atomic<int>& currentGroup) {
    if (tid <= 0 || currentGroup.load(std::memory_order_relaxed) == group) return;
    if (axion::process::SetSingleThreadAffinity(tid, group)) {
        currentGroup.store(group, std::memory_order_relaxed);
    } else {
        warnAffinityFailure("SetSingleThreadAffinity failed", tid, group);
    }
}

void applyWorkerPolicy(int tid, int group, unsigned int uclampMin,
                       std::atomic<int>& currentGroup, std::atomic<int>& currentUclamp) {
    if (tid <= 0) return;
    applyWorkerAffinity(tid, group, currentGroup);
    writeUclampForThread(tid, uclampMin, currentUclamp);
}

void writeUclamp(unsigned int uclampMin) {
    const int mainTid = sMainTid.load(std::memory_order_relaxed);
    const int hwcTid = sHwcTid.load(std::memory_order_relaxed);
    const int reTid = sReTid.load(std::memory_order_relaxed);
    const int group = computeAffinityGroup();

    writeUclampForThread(mainTid, uclampMin, sCurrentMainUclampMin);
    applyWorkerPolicy(hwcTid, group, uclampMin, sCurrentHwcAffinityGroup, sCurrentHwcUclampMin);
    applyWorkerPolicy(reTid, group, uclampMin, sCurrentReAffinityGroup, sCurrentReUclampMin);

    if (sProps.log) {
        ALOGI("uclamp_min=%u main=%d hwc=%d re=%d hz=%d perf=%d fps=%.1f", uclampMin,
              mainTid, hwcTid, reTid,
              sCurrentHz.load(std::memory_order_relaxed),
              sPerfMode.load(std::memory_order_relaxed),
              sCurrentFps.load(std::memory_order_relaxed));
    }
}

void apply() {
    if (sMainTid.load(std::memory_order_relaxed) <= 0) return;
    applyAffinity();
    writeUclamp(computeUclampMin());
}

void updateFps(nsecs_t now) {
    nsecs_t windowStart = sFrameWindowStart.load(std::memory_order_relaxed);
    int frameCount = sFrameCount.load(std::memory_order_relaxed);

    if (windowStart == 0 || now - windowStart > kFrameIntervalWindow) {
        if (frameCount > 1 && windowStart > 0) {
            const nsecs_t elapsed = now - windowStart;
            if (elapsed > 0) {
                sCurrentFps.store(static_cast<float>(frameCount - 1) * kNsPerSec / elapsed,
                                  std::memory_order_relaxed);
            }
        }
        sFrameWindowStart.store(now, std::memory_order_relaxed);
        sFrameCount.store(1, std::memory_order_relaxed);
    } else {
        sFrameCount.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

void registerMainThread() {
    const int tid = gettid();
    if (sMainTid.load(std::memory_order_relaxed) != tid) {
        sCurrentMainUclampMin.store(kUclampInvalid, std::memory_order_relaxed);
    }
    sMainTid.store(tid, std::memory_order_relaxed);
    initProps();
    axion::process::RefreshCpuSets();
}

void onFrameStart(nsecs_t timestamp, nsecs_t vsyncPeriod) {
    (void)vsyncPeriod;
    sLastFrameStart.store(timestamp, std::memory_order_relaxed);

    const nsecs_t lastEnd = sLastFrameEnd.load(std::memory_order_relaxed);
    if (lastEnd > 0 && timestamp > lastEnd) {
        const nsecs_t interval = timestamp - lastEnd;
        if (interval >= kMinFrameInterval) {
            updateFps(timestamp);
        }
    }

    apply();
}

void onFrameEnd(nsecs_t timestamp) {
    sLastFrameEnd.store(timestamp, std::memory_order_relaxed);

    const nsecs_t start = sLastFrameStart.load(std::memory_order_relaxed);
    if (start > 0 && timestamp > start) {
        const nsecs_t duration = timestamp - start;
        const int hz = sCurrentHz.load(std::memory_order_relaxed);
        const nsecs_t vsyncPeriod = hz > 0 ? kNsPerSec / hz : 16666666;
        const bool heavy = checkIfHeavy(
                duration > vsyncPeriod * sProps.earlyHeavy,
                sScreenRecording.load(std::memory_order_relaxed), false);
        sEarlyFrameBoost.store(heavy, std::memory_order_relaxed);
    }

    apply();
}

void onSpeedUpRE(int tid) {
    if (tid <= 0) return;
    if (sReTid.load(std::memory_order_relaxed) != tid) {
        sCurrentReAffinityGroup.store(kAffinityGroupInvalid, std::memory_order_relaxed);
        sCurrentReUclampMin.store(kUclampInvalid, std::memory_order_relaxed);
    }
    sReTid.store(tid, std::memory_order_relaxed);
    const unsigned int uclampMin = computeUclampMin();
    const int group = computeAffinityGroup();
    applyWorkerPolicy(tid, group, uclampMin, sCurrentReAffinityGroup, sCurrentReUclampMin);
    if (sProps.log) {
        ALOGI("onSpeedUpRE tid=%d group=%d uclamp=%u", tid, group, uclampMin);
    }
}

void notifyHwcHwbinderTid() {
    static std::once_flag sCaptured;
    std::call_once(sCaptured, []() {
        const int tid = gettid();
        if (tid <= 0) return;
        sHwcTid.store(tid, std::memory_order_relaxed);
        sCurrentHwcAffinityGroup.store(kAffinityGroupInvalid, std::memory_order_relaxed);
        sCurrentHwcUclampMin.store(kUclampInvalid, std::memory_order_relaxed);
        const unsigned int uclampMin = computeUclampMin();
        const int group = computeAffinityGroup();
        applyWorkerPolicy(tid, group, uclampMin, sCurrentHwcAffinityGroup, sCurrentHwcUclampMin);
        if (sProps.log) {
            ALOGI("notifyHwcHwbinderTid tid=%d group=%d uclamp=%u", tid, group, uclampMin);
        }
    });
}

void onRefreshRateChanged(Fps fps) {
    sCurrentHz.store(hzBucket(fps), std::memory_order_relaxed);
    apply();
}

void onPerformanceMode(bool enabled) {
    sPerfMode.store(enabled, std::memory_order_relaxed);
    apply();
}

void onPowerSuspend(bool suspended) {
    sPowerSuspended.store(suspended, std::memory_order_relaxed);
    apply();
}

void onVpLpEnable(bool enabled) {
    sVpLpEnabled.store(enabled, std::memory_order_relaxed);
    apply();
}

void onScreenRecording(bool recording) {
    sScreenRecording.store(recording, std::memory_order_relaxed);
    apply();
}

void onForeground(bool foreground) {
    sIsForeground.store(foreground, std::memory_order_relaxed);
    apply();
}

void setupConfig(const Config& config) {
    sConfigFg = config;
}

} // namespace android::scheduler::SfCpuPolicy
