/*
 * Copyright 2021 The Android Open Source Project
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

//#define LOG_NDEBUG 0

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <processgroup/sched_policy.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <system/thread_defs.h>
#include <utils/Log.h>
#include <mutex>
#include <unistd.h>

#include "ax_process_utils.h"
#include "BackgroundExecutor.h"

namespace android {

namespace {

void apply_thread_policy(SchedPolicy policy, int priority) {
    const int tid = gettid();
    if (axion::process::SetThreadPolicy(tid, policy, priority)) {
        return;
    }
    setpriority(PRIO_PROCESS, static_cast<id_t>(tid), priority);
}

void set_thread_affinity(bool highPriority) {
    const int group =
            highPriority ? axion::process::kCpuGroupAll : axion::process::kCpuGroupBalanced;
    if (axion::process::SetSingleThreadAffinity(gettid(), group)) {
        return;
    }

    cpu_set_t mask;
    CPU_ZERO(&mask);

    const long cpuCount = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpuCount <= 0 || cpuCount > CPU_SETSIZE) {
        return;
    }

    for (int cpu = 0; cpu < cpuCount; cpu++) {
        CPU_SET(cpu, &mask);
    }
    sched_setaffinity(gettid(), sizeof(mask), &mask);
}

void set_thread_priority(bool highPriority) {
    const SchedPolicy policy = highPriority ? SP_FOREGROUND_WINDOW : SP_FOREGROUND;
    const int priority = highPriority ? ANDROID_PRIORITY_DISPLAY : ANDROID_PRIORITY_NORMAL;
    apply_thread_policy(policy, priority);
    set_thread_affinity(highPriority);
    struct sched_param param = {0};
    sched_setscheduler(gettid(), SCHED_NORMAL, &param);
    apply_thread_policy(policy, priority);
}

} // anonymous namespace

BackgroundExecutor::BackgroundExecutor(bool highPriority) {
    // mSemaphore must be initialized before any calls to
    // BackgroundExecutor::sendCallbacks. For this reason, we initialize it
    // within the constructor instead of within mThread.
    LOG_ALWAYS_FATAL_IF(sem_init(&mSemaphore, 0, 0), "sem_init failed");
    mThread = std::thread([&, highPriority]() {
        set_thread_priority(highPriority);
        while (!mDone) {
            LOG_ALWAYS_FATAL_IF(sem_wait(&mSemaphore), "sem_wait failed (%d)", errno);
            auto callbacks = mCallbacksQueue.pop();
            if (!callbacks) {
                continue;
            }
            for (auto& callback : *callbacks) {
                callback();
            }
        }
    });
    if (highPriority) {
        pthread_setname_np(mThread.native_handle(), "BckgrndExec HP");
    } else {
        pthread_setname_np(mThread.native_handle(), "BckgrndExec LP");
    }
}

BackgroundExecutor::~BackgroundExecutor() {
    mDone = true;
    LOG_ALWAYS_FATAL_IF(sem_post(&mSemaphore), "sem_post failed");
    if (mThread.joinable()) {
        mThread.join();
        LOG_ALWAYS_FATAL_IF(sem_destroy(&mSemaphore), "sem_destroy failed");
    }
}

void BackgroundExecutor::sendCallbacks(Callbacks&& tasks) {
    mCallbacksQueue.push(std::move(tasks));
    LOG_ALWAYS_FATAL_IF(sem_post(&mSemaphore), "sem_post failed");
}

void BackgroundExecutor::flushQueue() {
    std::mutex mutex;
    std::condition_variable cv;
    bool flushComplete = false;
    sendCallbacks({[&]() {
        std::scoped_lock lock{mutex};
        flushComplete = true;
        cv.notify_one();
    }});
    std::unique_lock<std::mutex> lock{mutex};
    cv.wait(lock, [&]() { return flushComplete; });
}

} // namespace android
