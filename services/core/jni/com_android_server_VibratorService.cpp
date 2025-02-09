/*
 * Copyright (C) 2009 The Android Open Source Project
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

#define LOG_TAG "VibratorService"

#include <android/hardware/vibrator/1.0/IVibrator.h>
#include <android/hardware/vibrator/1.0/types.h>
#include <android/hardware/vibrator/1.1/IVibrator.h>

#include "jni.h"
#include "JNIHelp.h"
#include "android_runtime/AndroidRuntime.h"

#include <utils/misc.h>
#include <utils/Log.h>
#include <hardware/vibrator.h>

#include <inttypes.h>
#include <stdio.h>

using android::hardware::Return;
using android::hardware::vibrator::V1_0::Effect;
using android::hardware::vibrator::V1_0::EffectStrength;
using android::hardware::vibrator::V1_0::IVibrator;
using android::hardware::vibrator::V1_0::Status;
using android::hardware::vibrator::V1_1::Effect_1_1;
using IVibrator_1_1 = android::hardware::vibrator::V1_1::IVibrator;

namespace android
{

static constexpr int NUM_TRIES = 2;

// Creates a Return<R> with STATUS::EX_NULL_POINTER.
template<class R>
inline Return<R> NullptrStatus() {
    using ::android::hardware::Status;
    return Return<R>{Status::fromExceptionCode(Status::EX_NULL_POINTER)};
}

// Helper used to transparently deal with the vibrator HAL becoming unavailable.
template<class R, class I, class... Args0, class... Args1>
Return<R> halCall(Return<R> (I::* fn)(Args0...), Args1&&... args1) {
    // Assume that if getService returns a nullptr, HAL is not available on the
    // device.
    static sp<I> sHal = I::getService();
    static bool sAvailable = sHal != nullptr;

    if (!sAvailable) {
        return NullptrStatus<R>();
    }

    // Return<R> doesn't have a default constructor, so make a Return<R> with
    // STATUS::EX_NONE.
    using ::android::hardware::Status;
    Return<R> ret{Status::fromExceptionCode(Status::EX_NONE)};

    // Note that ret is guaranteed to be changed after this loop.
    for (int i = 0; i < NUM_TRIES; ++i) {
        ret = (sHal == nullptr) ? NullptrStatus<R>()
                : (*sHal.*fn)(std::forward<Args1>(args1)...);

        if (ret.isOk()) {
            break;
        }

        ALOGE("Failed to issue command to vibrator HAL. Retrying.");
        // Restoring connection to the HAL.
        sHal = I::tryGetService();
    }
    return ret;
}

static void vibratorInit(JNIEnv /* env */, jobject /* clazz */)
{
    halCall(&IVibrator::ping).isOk();
}

static jboolean vibratorExists(JNIEnv* /* env */, jobject /* clazz */)
{
    return halCall(&IVibrator::ping).isOk() ? JNI_TRUE : JNI_FALSE;
}

static void vibratorOn(JNIEnv* /* env */, jobject /* clazz */, jlong timeout_ms)
{
    Status retStatus = halCall(&IVibrator::on, timeout_ms).withDefault(Status::UNKNOWN_ERROR);
    if (retStatus != Status::OK) {
        ALOGE("vibratorOn command failed (%" PRIu32 ").", static_cast<uint32_t>(retStatus));
    }
}

static void vibratorOff(JNIEnv* /* env */, jobject /* clazz */)
{
    Status retStatus = halCall(&IVibrator::off).withDefault(Status::UNKNOWN_ERROR);
    if (retStatus != Status::OK) {
        ALOGE("vibratorOff command failed (%" PRIu32 ").", static_cast<uint32_t>(retStatus));
    }
}

static jlong vibratorSupportsAmplitudeControl(JNIEnv*, jobject) {
    return halCall(&IVibrator::supportsAmplitudeControl).withDefault(false);
}

static void vibratorSetAmplitude(JNIEnv*, jobject, jint amplitude) {
    Status status = halCall(&IVibrator::setAmplitude, static_cast<uint32_t>(amplitude))
        .withDefault(Status::UNKNOWN_ERROR);
    if (status != Status::OK) {
      ALOGE("Failed to set vibrator amplitude (%" PRIu32 ").",
            static_cast<uint32_t>(status));
    }
}

static jlong vibratorPerformEffect(JNIEnv*, jobject, jlong effect, jint strength) {
    Status status;
    uint32_t lengthMs;
    auto callback = [&status, &lengthMs](Status retStatus, uint32_t retLengthMs) {
        status = retStatus;
        lengthMs = retLengthMs;
    };
    EffectStrength effectStrength(static_cast<EffectStrength>(strength));

    if (effect < 0  || effect > static_cast<uint32_t>(Effect_1_1::TICK)) {
        ALOGW("Unable to perform haptic effect, invalid effect ID (%" PRId32 ")",
                static_cast<int32_t>(effect));
    } else if (effect == static_cast<uint32_t>(Effect_1_1::TICK)) {
        auto ret = halCall(&IVibrator_1_1::perform_1_1, static_cast<Effect_1_1>(effect),
                           effectStrength, callback);
        if (!ret.isOk()) {
            ALOGW("Failed to perform effect (%" PRId32 "), insufficient HAL version",
                    static_cast<int32_t>(effect));
        }
    } else {
        auto ret = halCall(&IVibrator::perform, static_cast<Effect>(effect), effectStrength,
                           callback);
        if (!ret.isOk()) {
            ALOGW("Failed to perform effect (%" PRId32 ")", static_cast<int32_t>(effect));
        }
    }

    if (status == Status::OK) {
        return lengthMs;
    } else if (status != Status::UNSUPPORTED_OPERATION) {
        // Don't warn on UNSUPPORTED_OPERATION, that's a normal even and just means the motor
        // doesn't have a pre-defined waveform to perform for it, so we should just fall back
        // to the framework waveforms.
        ALOGE("Failed to perform haptic effect: effect=%" PRId64 ", strength=%" PRId32
                ", error=%" PRIu32 ").", static_cast<int64_t>(effect),
                static_cast<int32_t>(strength), static_cast<uint32_t>(status));
    }
    return -1;
}

static const JNINativeMethod method_table[] = {
    { "vibratorExists", "()Z", (void*)vibratorExists },
    { "vibratorInit", "()V", (void*)vibratorInit },
    { "vibratorOn", "(J)V", (void*)vibratorOn },
    { "vibratorOff", "()V", (void*)vibratorOff },
    { "vibratorSupportsAmplitudeControl", "()Z", (void*)vibratorSupportsAmplitudeControl},
    { "vibratorSetAmplitude", "(I)V", (void*)vibratorSetAmplitude},
    { "vibratorPerformEffect", "(JJ)J", (void*)vibratorPerformEffect}
};

int register_android_server_VibratorService(JNIEnv *env)
{
    return jniRegisterNativeMethods(env, "com/android/server/VibratorService",
            method_table, NELEM(method_table));
}

};
