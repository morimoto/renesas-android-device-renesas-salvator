/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ANDROID_HARDWARE_AUDIO_V2_0_STREAMIN_H
#define ANDROID_HARDWARE_AUDIO_V2_0_STREAMIN_H

#include <atomic>
#include <memory>

#include <android/hardware/audio/2.0/IStreamIn.h>
#include <hidl/MQDescriptor.h>
#include <fmq/EventFlag.h>
#include <fmq/MessageQueue.h>
#include <hidl/Status.h>
#include <utils/Thread.h>

#include "Device.h"
#include "Stream.h"
#include "Util.h"


namespace android {
namespace hardware {
namespace audio {
namespace V2_0 {
namespace salvator {

using ::android::hardware::audio::common::V2_0::AudioChannelMask;
using ::android::hardware::audio::common::V2_0::AudioDevice;
using ::android::hardware::audio::common::V2_0::AudioFormat;
using ::android::hardware::audio::common::V2_0::AudioSource;
using ::android::hardware::audio::V2_0::DeviceAddress;
using ::android::hardware::audio::V2_0::IStream;
using ::android::hardware::audio::V2_0::IStreamIn;
using ::android::hardware::audio::V2_0::IStreamOut;
using ::android::hardware::audio::V2_0::ParameterValue;
using ::android::hardware::audio::V2_0::Result;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::hidl_vec;
using ::android::hardware::hidl_string;
using ::android::sp;

struct StreamIn : public IStreamIn {
    typedef MessageQueue<ReadParameters, kSynchronizedReadWrite> CommandMQ;
    typedef MessageQueue<uint8_t, kSynchronizedReadWrite> DataMQ;
    typedef MessageQueue<ReadStatus, kSynchronizedReadWrite> StatusMQ;

    StreamIn(const sp<Device> &device);

    // Methods from ::android::hardware::audio::V2_0::IStream follow.
    Return<uint64_t> getFrameSize()  override;
    Return<uint64_t> getFrameCount()  override;
    Return<uint64_t> getBufferSize()  override;
    Return<uint32_t> getSampleRate()  override;
    Return<void> getSupportedSampleRates(getSupportedSampleRates_cb _hidl_cb)
    override;
    Return<Result> setSampleRate(uint32_t sampleRateHz)  override;
    Return<AudioChannelMask> getChannelMask()  override;
    Return<void> getSupportedChannelMasks(getSupportedChannelMasks_cb _hidl_cb)
    override;
    Return<Result> setChannelMask(AudioChannelMask mask)  override;
    Return<AudioFormat> getFormat()  override;
    Return<void> getSupportedFormats(getSupportedFormats_cb _hidl_cb)  override;
    Return<Result> setFormat(AudioFormat format)  override;
    Return<void> getAudioProperties(getAudioProperties_cb _hidl_cb)  override;
    Return<Result> addEffect(uint64_t effectId)  override;
    Return<Result> removeEffect(uint64_t effectId)  override;
    Return<Result> standby()  override;
    Return<AudioDevice> getDevice()  override;
    Return<Result> setDevice(const DeviceAddress &address)  override;
    Return<Result> setConnectedState(const DeviceAddress &address,
                                     bool connected)  override;
    Return<Result> setHwAvSync(uint32_t hwAvSync)  override;
    Return<void> getParameters(
        const hidl_vec<hidl_string> &keys, getParameters_cb _hidl_cb)  override;
    Return<Result> setParameters(const hidl_vec<ParameterValue> &parameters)
    override;
    Return<void> debugDump(const hidl_handle &fd)  override;
    Return<Result> close()  override;

    // Methods from ::android::hardware::audio::V2_0::IStreamIn follow.
    Return<void> getAudioSource(getAudioSource_cb _hidl_cb)  override;
    Return<Result> setGain(float gain)  override;
    Return<void> prepareForReading(
        uint32_t frameSize, uint32_t framesCount,
        prepareForReading_cb _hidl_cb)  override;
    Return<uint32_t> getInputFramesLost()  override;
    Return<void> getCapturePosition(getCapturePosition_cb _hidl_cb)  override;
    Return<Result> start() override;
    Return<Result> stop() override;
    Return<void> createMmapBuffer(int32_t minSizeFrames,
                                  createMmapBuffer_cb _hidl_cb) override;
    Return<void> getMmapPosition(getMmapPosition_cb _hidl_cb) override;

    static Result getCapturePositionImpl(
        StreamIn *stream, uint64_t *frames, uint64_t *time);

    void putEchoReference(sp<Device> dev, echo_reference_itfe *reference);
    int startInputStream(StreamIn *in);
    echo_reference_itfe *getEchoReference(sp<Device> dev,
                                          audio_format_t format,
                                          uint32_t channel_count,
                                          uint32_t sampling_rate);
    void addEchoReference(IStreamOut *out,
                          echo_reference_itfe *reference);
    int startInputStream();
    ssize_t processFrames(StreamIn *in, void *buffer, ssize_t frames);
    ssize_t readFrames(StreamIn *in, void *buffer, ssize_t frames);
    int getNextBuffer(StreamIn *in, resampler_buffer_provider *buffer_provider,
                      resampler_buffer *buffer);
    int32_t updateEchoReference(StreamIn *in, size_t frames);
    void pushEchoReference(StreamIn *in, size_t frames);
    void getCaptureDelay(StreamIn *in,
                         size_t frames,
                         echo_reference_buffer *buffer);
    int setPreprocessorEchoDelay(effect_handle_t handle,
                                 int32_t delay_us);
    int setPreprocessorParam(effect_handle_t handle,
                             effect_param_t *param);
    ssize_t readStreamIn(StreamIn *stream, void *buffer,
                         size_t bytes);
    int configureReverse(StreamIn *in);
    int doInputStandby(StreamIn *in);
    static int getNextBuffer(struct resampler_buffer_provider *buffer_provider,
                             struct resampler_buffer *buffer);
    static void releaseBuffer(struct resampler_buffer_provider *buffer_provider,
                              struct resampler_buffer *buffer);

private:
    const sp<Device> mDevice;
    bool mIsClosed;
    const sp<Stream> mStreamCommon;
    std::unique_ptr<CommandMQ> mCommandMQ;
    std::unique_ptr<DataMQ> mDataMQ;
    std::unique_ptr<StatusMQ> mStatusMQ;
    EventFlag *mEfGroup;
    std::atomic<bool> mStopReadThread;
    sp<Thread> mReadThread;

    virtual ~StreamIn();

public:
    static pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    pcm_config config;
    pcm *_pcm;
    int device;
    resampler_itfe *resampler;
    resampler_buffer_provider buf_provider;
    unsigned int requested_rate;
    int _standby;
    int source;
    echo_reference_itfe *echo_reference;
    bool need_echo_reference;

    int16_t *read_buf;
    size_t read_buf_size;
    size_t read_buf_frames;

    int16_t *proc_buf_in;
    int16_t *proc_buf_out;
    size_t proc_buf_size;
    size_t proc_buf_frames;

    int16_t *ref_buf;
    size_t ref_buf_size;
    size_t ref_buf_frames;

    int read_status;

    int num_preprocessors;
    effect_handle_t preprocessors[MAX_PREPROCESSORS];

    bool aux_channels_changed;
    uint32_t main_channels;
    uint32_t aux_channels;

    friend struct StreamOut;
};

}  // namespace salvator
}  // namespace V2_0
}  // namespace audio
}  // namespace hardware
}  // namespace android

#endif  // ANDROID_HARDWARE_AUDIO_V2_0_STREAMIN_H
