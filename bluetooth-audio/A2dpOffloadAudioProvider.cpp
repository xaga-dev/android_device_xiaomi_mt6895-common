/*
 * Copyright 2020 The Android Open Source Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "BTAudioProviderA2dpOffload"

#include "A2dpOffloadAudioProvider.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <dlfcn.h>
#include <fmq/MessageQueue.h>
#include <hidl/MQDescriptor.h>
#include <string>

#include "BluetoothAudioSessionReport_2_1.h"
#include "BluetoothAudioSupportedCodecsDB_2_1.h"

namespace android {
namespace hardware {
namespace bluetooth {
namespace audio {
namespace V2_1 {
namespace implementation {

using ::android::bluetooth::audio::BluetoothAudioSessionReport_2_1;
using ::android::hardware::kSynchronizedReadWrite;
using ::android::hardware::MessageQueue;
using ::android::hardware::Void;
using ::android::hardware::bluetooth::audio::V2_0::AudioConfiguration;

using DataMQ = MessageQueue<uint8_t, kSynchronizedReadWrite>;

static void* sAudioHalHandle = nullptr;
static void* sMtkAudioHw = nullptr;

static bool initAudioHal() {
  if (sMtkAudioHw != nullptr) {
    return true;
  }

  std::string platform = android::base::GetProperty("ro.board.platform", "");
  if (platform.empty()) {
    return false;
  }

  std::string path = "/vendor/lib64/hw/audio.primary." + platform + ".so";
  sAudioHalHandle = dlopen(path.c_str(), RTLD_NOW);
  if (sAudioHalHandle == nullptr) {
    LOG(ERROR) << __func__ << ": failed to open " << path << ": " << dlerror();
    return false;
  }

  auto createMTKAudioHardware = reinterpret_cast<void* (*)()>(
      dlsym(sAudioHalHandle, "createMTKAudioHardware"));
  if (createMTKAudioHardware != nullptr) {
    sMtkAudioHw = createMTKAudioHardware();
  }
  return (sMtkAudioHw != nullptr);
}

static void setBtOffloadParam(const sp<IBluetoothAudioPort>& hostIf,
                              const V2_1::AudioConfiguration& audioConfig,
                              bool bEnable, int sessionType) {
  if (!initAudioHal()) {
    return;
  }

  using SetParamFn = int (*)(void* hw, const void* hostIf,
                             const void* audioConfig,
                             bool bEnable, int btType);
  static auto fn = reinterpret_cast<SetParamFn>(dlsym(
      sAudioHalHandle,
      "_ZN7android17AudioALSAHardware29setBluetoothAudioOffloadParamERKNS_2spIN6"
      "vendor8mediatek8hardware9bluetooth5audio4V2_119IBluetoothAudioPortEEERKNS6"
      "_4V2_218AudioConfigurationEbi"));

  if (fn != nullptr) {
    fn(sMtkAudioHw, &hostIf, &audioConfig, bEnable, sessionType);
  }
}

static void setA2dpSuspendStatus(int status) {
  if (!initAudioHal()) {
    return;
  }

  using SetStatusFn = void (*)(void* hw, int status);
  static auto fn = reinterpret_cast<SetStatusFn>(dlsym(
      sAudioHalHandle,
      "_ZN7android17AudioALSAHardware20setA2dpSuspendStatusEi"));

  if (fn != nullptr) {
    fn(sMtkAudioHw, status);
  }
}

A2dpOffloadAudioProvider::A2dpOffloadAudioProvider()
    : BluetoothAudioProvider() {
  session_type_ = SessionType::A2DP_HARDWARE_OFFLOAD_DATAPATH;
}

bool A2dpOffloadAudioProvider::isValid(const V2_0::SessionType& sessionType) {
  return isValid(static_cast<SessionType>(sessionType));
}

bool A2dpOffloadAudioProvider::isValid(const SessionType& sessionType) {
  return (sessionType == session_type_);
}

Return<void> A2dpOffloadAudioProvider::startSession(
    const sp<IBluetoothAudioPort>& hostIf,
    const AudioConfiguration& audioConfig, startSession_cb _hidl_cb) {
  /**
   * Initialize the audio platform if audioConfiguration is supported.
   * Save the IBluetoothAudioPort interface, so that it can be used
   * later to send stream control commands to the HAL client, based on
   * interaction with Audio framework.
   */
  if (audioConfig.getDiscriminator() !=
      AudioConfiguration::hidl_discriminator::codecConfig) {
    LOG(WARNING) << __func__
                 << " - Invalid Audio Configuration=" << toString(audioConfig);
    _hidl_cb(BluetoothAudioStatus::UNSUPPORTED_CODEC_CONFIGURATION,
             DataMQ::Descriptor());
    return Void();
  } else if (!android::bluetooth::audio::IsOffloadCodecConfigurationValid(
                 session_type_, audioConfig.codecConfig())) {
    _hidl_cb(BluetoothAudioStatus::UNSUPPORTED_CODEC_CONFIGURATION,
             DataMQ::Descriptor());
    return Void();
  }

  return BluetoothAudioProvider::startSession(hostIf, audioConfig, _hidl_cb);
}

Return<void> A2dpOffloadAudioProvider::startSession_2_1(
    const sp<IBluetoothAudioPort>& hostIf,
    const V2_1::AudioConfiguration& audioConfig, startSession_cb _hidl_cb) {
  setBtOffloadParam(hostIf, audioConfig, true,
                    static_cast<int>(session_type_));

  return BluetoothAudioProvider::startSession_2_1(hostIf, audioConfig,
                                                  _hidl_cb);
}

Return<void> A2dpOffloadAudioProvider::streamStarted(
    BluetoothAudioStatus status) {
  setA2dpSuspendStatus(static_cast<int>(status));

  return BluetoothAudioProvider::streamStarted(status);
}

Return<void> A2dpOffloadAudioProvider::streamSuspended(
    BluetoothAudioStatus status) {
  setA2dpSuspendStatus(static_cast<int>(status) + 3);

  return BluetoothAudioProvider::streamSuspended(status);
}

Return<void> A2dpOffloadAudioProvider::endSession() {
  setBtOffloadParam(nullptr, {}, false, static_cast<int>(session_type_));

  return BluetoothAudioProvider::endSession();
}

Return<void> A2dpOffloadAudioProvider::onSessionReady(
    startSession_cb _hidl_cb) {
  BluetoothAudioSessionReport_2_1::OnSessionStarted(session_type_, stack_iface_,
                                                    nullptr, audio_config_);
  _hidl_cb(BluetoothAudioStatus::SUCCESS, DataMQ::Descriptor());
  return Void();
}

}  // namespace implementation
}  // namespace V2_1
}  // namespace audio
}  // namespace bluetooth
}  // namespace hardware
}  // namespace android
