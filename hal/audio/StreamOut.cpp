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

#define LOG_TAG "AudioHAL"
#define ATRACE_TAG ATRACE_TAG_AUDIO

#include <memory>

#include <log/log.h>
#include <utils/Trace.h>

#include "StreamOut.h"
#include "StreamIn.h"
#include "Util.h"
#include "EffectMap.h"

namespace android {
namespace hardware {
namespace audio {
namespace V2_0 {
namespace salvator {

using ::android::hardware::audio::common::V2_0::ThreadInfo;

namespace {

class WriteThread : public Thread {
public:
    // WriteThread's lifespan never exceeds StreamOut's lifespan.
    WriteThread(std::atomic<bool> *stop, StreamOut *stream,
                StreamOut::CommandMQ *commandMQ, StreamOut::DataMQ *dataMQ,
                StreamOut::StatusMQ *statusMQ, EventFlag *efGroup)
        : Thread(false /*canCallJava*/),
          mStop(stop),
          mStream(stream),
          mCommandMQ(commandMQ),
          mDataMQ(dataMQ),
          mStatusMQ(statusMQ),
          mEfGroup(efGroup),
          mBuffer(nullptr) {}
    bool init() {
        mBuffer.reset(new (std::nothrow) uint8_t[mDataMQ->getQuantumCount()]);
        return mBuffer != nullptr;
    }
    virtual ~WriteThread() {}

private:
    std::atomic<bool> *mStop;
    StreamOut *mStream;
    StreamOut::CommandMQ *mCommandMQ;
    StreamOut::DataMQ *mDataMQ;
    StreamOut::StatusMQ *mStatusMQ;
    EventFlag *mEfGroup;
    std::unique_ptr<uint8_t[]> mBuffer;
    IStreamOut::WriteStatus mStatus;

    bool threadLoop() override;

    void doGetLatency();
    void doGetPresentationPosition();
    void doWrite();
};

void WriteThread::doWrite() {
    const size_t availToRead = mDataMQ->availableToRead();
    mStatus.retval = Result::OK;
    mStatus.reply.written = 0;
    if (mDataMQ->read(&mBuffer[0], availToRead)) {
        ssize_t writeResult = mStream->outWrite(mStream, &mBuffer[0],
                                                availToRead);
        if (writeResult >= 0) {
            mStatus.reply.written = writeResult;
        } else {
            mStatus.retval = Stream::analyzeStatus("write", writeResult);
        }
    }
}

void WriteThread::doGetPresentationPosition() {
    mStatus.retval = StreamOut::getPresentationPositionImpl(
                         &mStatus.reply.presentationPosition.frames,
                         &mStatus.reply.presentationPosition.timeStamp);
}

void WriteThread::doGetLatency() {
    mStatus.retval = Result::OK;
    mStatus.reply.latencyMs = (SHORT_PERIOD_SIZE * PLAYBACK_SHORT_PERIOD_COUNT *
                               1000) / pcm_config_dac.rate;//mStream->get_latency(mStream);
}

bool WriteThread::threadLoop() {
    // This implementation doesn't return control back to the Thread until it
    // decides to stop,
    // as the Thread uses mutexes, and this can lead to priority inversion.
    while (!std::atomic_load_explicit(mStop, std::memory_order_acquire)) {
        uint32_t efState = 0;
        mEfGroup->wait(static_cast<uint32_t>(MessageQueueFlagBits::NOT_EMPTY),
                       &efState);
        if (!(efState &
                static_cast<uint32_t>(MessageQueueFlagBits::NOT_EMPTY))) {
            continue;  // Nothing to do.
        }
        if (!mCommandMQ->read(&mStatus.replyTo)) {
            continue;  // Nothing to do.
        }
        switch (mStatus.replyTo) {
        case IStreamOut::WriteCommand::WRITE:
            doWrite();
            break;
        case IStreamOut::WriteCommand::GET_PRESENTATION_POSITION:
            doGetPresentationPosition();
            break;
        case IStreamOut::WriteCommand::GET_LATENCY:
            doGetLatency();
            break;
        default:
            ALOGE("Unknown write thread command code %d", mStatus.replyTo);
            mStatus.retval = Result::NOT_SUPPORTED;
            break;
        }
        if (!mStatusMQ->write(&mStatus)) {
            ALOGE("status message queue write failed");
        }
        mEfGroup->wake(static_cast<uint32_t>(MessageQueueFlagBits::NOT_FULL));
    }

    return false;
}

}  // namespace

pthread_mutex_t StreamOut::lock;

StreamOut::StreamOut(const sp<Device> &device, output_type type)
    : mIsClosed(false),
      mDevice(device),
      mStreamCommon(new Stream(this)),
      mEfGroup(nullptr),
      mStopWriteThread(false),
      resampler(nullptr),
      buffer(nullptr),
      mStreamType(type) {}

StreamOut::~StreamOut() {
    ATRACE_CALL();
    close();
    if (mWriteThread.get()) {
        ATRACE_NAME("mWriteThread->join");
        status_t status = mWriteThread->join();
        ALOGE_IF(status, "write thread exit error: %s", strerror(-status));
    }
    if (mEfGroup) {
        status_t status = EventFlag::deleteEventFlag(&mEfGroup);
        ALOGE_IF(status, "write MQ event flag deletion error: %s",
                 strerror(-status));
    }
    mCallback.clear();
    mDevice->closeOutputStream(this);
    //mStream = nullptr;
}

// Methods from ::android::hardware::audio::V2_0::IStream follow.
Return<uint64_t> StreamOut::getFrameSize() {
    size_t chan_samp_sz;
    audio_format_t format = AUDIO_FORMAT_PCM_16_BIT;
    if (audio_has_proportional_frames(format)) {
        chan_samp_sz = audio_bytes_per_sample(format);
        return audio_channel_count_from_out_mask(this->channel_mask) * chan_samp_sz;
    } else {
        return sizeof(int8_t);
    }

}

Return<uint64_t> StreamOut::getFrameCount() {
    return mStreamCommon->getFrameCount();
}

Return<uint64_t> StreamOut::getBufferSizeLatency() {
    /* take resampling into account and return the closest majoring
    multiple of 16 frames, as audioflinger expects audio buffers to
    be a multiple of 16 frames. Note: we use the default rate here
    from pcm_config_dac.rate. */
    size_t size = (SHORT_PERIOD_SIZE * DEFAULT_OUT_SAMPLING_RATE) /
                  pcm_config_dac.rate;
    size = ((size + 15) / 16) * 16;
    size_t chan_samp_sz;
    audio_format_t format = AUDIO_FORMAT_PCM_16_BIT;
    size_t temp;
    if (audio_has_proportional_frames(format)) {
        chan_samp_sz = audio_bytes_per_sample(format);
        temp = popcount(channel_mask) * chan_samp_sz;
    } else {
        temp = sizeof(int8_t);
    }
    return size * temp;
}

Return<uint64_t> StreamOut::getBufferSizeHdmi() {
    size_t chan_samp_sz;
    audio_format_t format = AUDIO_FORMAT_PCM_16_BIT;
    size_t temp;
    if (audio_has_proportional_frames(format)) {
        chan_samp_sz = audio_bytes_per_sample(format);
        temp = popcount(channel_mask) * chan_samp_sz;
    } else {
        temp = sizeof(int8_t);
    }
    return HDMI_PERIOD_SIZE * temp;
}

Return<uint64_t> StreamOut::getBufferSize() {
    if (mStreamType == OUTPUT_LOW_LATENCY)
        return getBufferSizeLatency();
    else
        return getBufferSizeHdmi();
}

Return<uint32_t> StreamOut::getSampleRate() {
    return requested_rate;
}

Return<void> StreamOut::getSupportedSampleRates(
    getSupportedSampleRates_cb _hidl_cb) {
    return mStreamCommon->getSupportedSampleRates(_hidl_cb);
}

Return<Result> StreamOut::setSampleRate(uint32_t sampleRateHz) {
    requested_rate = sampleRateHz;
    return mStreamCommon->setSampleRate(sampleRateHz);
}

Return<AudioChannelMask> StreamOut::getChannelMask() {
    return AudioChannelMask(this->requested_channel_mask);
}

Return<void> StreamOut::getSupportedChannelMasks(
    getSupportedChannelMasks_cb _hidl_cb) {
    return mStreamCommon->getSupportedChannelMasks(_hidl_cb);
}

Return<Result> StreamOut::setChannelMask(AudioChannelMask mask) {
    return mStreamCommon->setChannelMask(mask);
}

Return<AudioFormat> StreamOut::getFormat() {
    return AudioFormat(AUDIO_FORMAT_PCM_16_BIT);
}

Return<void> StreamOut::getSupportedFormats(getSupportedFormats_cb _hidl_cb) {
    return mStreamCommon->getSupportedFormats(_hidl_cb);
}

Return<Result> StreamOut::setFormat(AudioFormat format) {
    return mStreamCommon->setFormat(format);
}

Return<void> StreamOut::getAudioProperties(getAudioProperties_cb _hidl_cb) {
    uint32_t halSampleRate = this->requested_rate;
    audio_channel_mask_t halMask = this->requested_channel_mask;
    audio_format_t halFormat = AUDIO_FORMAT_PCM_16_BIT;
    _hidl_cb(halSampleRate, AudioChannelMask(halMask), AudioFormat(halFormat));
    return Void();
}

Return<Result> StreamOut::addEffect(uint64_t effectId) {
    effect_handle_t effect = EffectMap::getInstance().get(effectId);

    if (effect == nullptr)
        return Result::INVALID_ARGUMENTS;

    return Result::OK;
}

Return<Result> StreamOut::removeEffect(uint64_t effectId) {
    effect_handle_t effect = EffectMap::getInstance().get(effectId);

    if (effect == nullptr)
        return Result::INVALID_ARGUMENTS;

    return Result::OK;
}

Return<Result> StreamOut::standby() {

    PTHREAD_MUTEX_LOCK(&mDevice->lock);
    PTHREAD_MUTEX_LOCK(&this->lock);
    doOutStandby(this);
    PTHREAD_MUTEX_UNLOCK(&this->lock);
    PTHREAD_MUTEX_UNLOCK(&mDevice->lock);
    return Result::OK;
}

int StreamOut::doOutStandby(StreamOut *out) {
    bool all_outputs_in_standby = true;
    pcm_type type = (out->mStreamType == OUTPUT_LOW_LATENCY) ? PCM_NORMAL : PCM_HDMI;

    if (!this->_standby) {
        this->_standby = 1;

        //for (i = 0; i < PCM_TOTAL; i++) {
        if (this->_pcm[type]) {
            pcm_close(this->_pcm[type]);
            this->_pcm[type] = NULL;
        }
        //}

        //for (i = 0; i < OUTPUT_TOTAL; i++) {
        if (mDevice->outputs[out->mStreamType] != NULL
                && !static_cast<StreamOut *>(mDevice->outputs[out->mStreamType])->_standby) {
            all_outputs_in_standby = false;
            //break;
        }
        //}

        /* stop writing to echo reference */
        if (this->echo_reference != NULL) {
            this->echo_reference->write(this->echo_reference, NULL);
            this->echo_reference = NULL;
        }
    }
    return 0;
}

Return<AudioDevice> StreamOut::getDevice() {
    return mStreamCommon->getDevice();
}

Return<Result> StreamOut::setDevice(const DeviceAddress &address) {
    return mStreamCommon->setDevice(address);
}

Return<Result> StreamOut::setConnectedState(const DeviceAddress &address,
        bool connected) {
    return mStreamCommon->setConnectedState(address, connected);
}

Return<Result> StreamOut::setHwAvSync(uint32_t hwAvSync) {
    return mStreamCommon->setHwAvSync(hwAvSync);
}

Return<void> StreamOut::getParameters(const hidl_vec<hidl_string> &keys,
                                      getParameters_cb _hidl_cb) {
    return mStreamCommon->getParameters(keys, _hidl_cb);
}

Return<Result> StreamOut::setParameters(
    const hidl_vec<ParameterValue> &parameters) {
    ALOGV("%s", __FUNCTION__);
    if (parameters.size() == 0)
        return Result::OK;

    AudioParameter params;
    for (size_t i = 0; i < parameters.size(); ++i) {
        params.add(String8(parameters[i].key.c_str()),
                   String8(parameters[i].value.c_str()));
    }

    char* temp_str_parms = (char*)malloc(strlen(params.toString().string()) + 1);
    strcpy(temp_str_parms, params.toString().string());
    const char *kvpairs = temp_str_parms;
    struct str_parms *parms;
    char value[32];
    int ret, val = 0;

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value,
                            sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        PTHREAD_MUTEX_LOCK(&mDevice->lock);
        PTHREAD_MUTEX_LOCK(&this->lock);
        if (((mDevice->devices & AUDIO_DEVICE_OUT_ALL) != val) && (val != 0)) {
            mDevice->devices &= ~AUDIO_DEVICE_OUT_ALL;
            mDevice->devices |= val;
            //do_output_standby(out);
            int i;
            bool all_outputs_in_standby = true;

            if (!this->_standby) {
                this->_standby = 1;

                for (i = 0; i < PCM_TOTAL; i++) {
                    if (this->_pcm[i]) {
                        pcm_close(this->_pcm[i]);
                        this->_pcm[i] = NULL;
                    }
                }

                for (i = 0; i < OUTPUT_TOTAL; i++) {
                    if (mDevice->outputs[i] != NULL
                            && !static_cast<StreamOut *>(mDevice->outputs[i])->_standby) {
                        all_outputs_in_standby = false;
                        break;
                    }
                }

                /* stop writing to echo reference */
                if (this->echo_reference != NULL) {
                    this->echo_reference->write(this->echo_reference, NULL);
                    this->echo_reference = NULL;
                }
            }
        }
        PTHREAD_MUTEX_UNLOCK(&this->lock);
        PTHREAD_MUTEX_UNLOCK(&mDevice->lock);
    }

    str_parms_destroy(parms);
    free(temp_str_parms);

    if (ret >= OK)
        return Result::OK;
    else
        return Result::INVALID_ARGUMENTS;
}

Return<void> StreamOut::debugDump(const hidl_handle &/*fd*/) {
    return Void();
}

Return<Result> StreamOut::close() {
    if (mIsClosed) return Result::INVALID_STATE;
    mIsClosed = true;
    if (mWriteThread.get()) {
        mStopWriteThread.store(true, std::memory_order_release);
    }
    if (mEfGroup) {
        mEfGroup->wake(static_cast<uint32_t>(MessageQueueFlagBits::NOT_EMPTY));
    }
    return Result::OK;
}

// Methods from ::android::hardware::audio::V2_0::IStreamOut follow.
Return<uint32_t> StreamOut::getLatency() {
    return (SHORT_PERIOD_SIZE * PLAYBACK_SHORT_PERIOD_COUNT * 1000) /
           pcm_config_dac.rate;
}

Return<Result> StreamOut::setVolume(float left, float right) {
    ALOGV("%s: left = %f, right = %f", __FUNCTION__, left, right);
    return Result::NOT_SUPPORTED;
}

Return<void> StreamOut::prepareForWriting(uint32_t frameSize,
        uint32_t framesCount,
        prepareForWriting_cb _hidl_cb) {
    status_t status;
    ThreadInfo threadInfo = {0, 0};

    // Wrap the _hidl_cb to return an error
    auto sendError = [&threadInfo, &_hidl_cb](Result result) {
        _hidl_cb(result, CommandMQ::Descriptor(), DataMQ::Descriptor(),
                 StatusMQ::Descriptor(), threadInfo);
    };

    // Create message queues.
    if (mDataMQ) {
        ALOGE("the client attempts to call prepareForWriting twice");
        sendError(Result::INVALID_STATE);
        return Void();
    }
    std::unique_ptr<CommandMQ> tempCommandMQ(new CommandMQ(1));

    // Check frameSize and framesCount
    if (frameSize == 0 || framesCount == 0) {
        ALOGE("Null frameSize (%u) or framesCount (%u)", frameSize,
              framesCount);
        sendError(Result::INVALID_ARGUMENTS);
        return Void();
    }
    if (frameSize > Stream::MAX_BUFFER_SIZE / framesCount) {
        ALOGE("Buffer too big: %u*%u bytes > MAX_BUFFER_SIZE (%u)", frameSize,
              framesCount,
              Stream::MAX_BUFFER_SIZE);
        sendError(Result::INVALID_ARGUMENTS);
        return Void();
    }
    std::unique_ptr<DataMQ> tempDataMQ(
        new DataMQ(frameSize * framesCount, true /* EventFlag */));

    std::unique_ptr<StatusMQ> tempStatusMQ(new StatusMQ(1));
    if (!tempCommandMQ->isValid() || !tempDataMQ->isValid() ||
            !tempStatusMQ->isValid()) {
        ALOGE_IF(!tempCommandMQ->isValid(), "command MQ is invalid");
        ALOGE_IF(!tempDataMQ->isValid(), "data MQ is invalid");
        ALOGE_IF(!tempStatusMQ->isValid(), "status MQ is invalid");
        sendError(Result::INVALID_ARGUMENTS);
        return Void();
    }
    EventFlag *tempRawEfGroup{};
    status = EventFlag::createEventFlag(tempDataMQ->getEventFlagWord(),
                                        &tempRawEfGroup);
    std::unique_ptr<EventFlag, void (*)(EventFlag *)> tempElfGroup(
    tempRawEfGroup, [](auto * ef) {
        EventFlag::deleteEventFlag(&ef);
    });
    if (status != OK || !tempElfGroup) {
        ALOGE("failed creating event flag for data MQ: %s", strerror(-status));
        sendError(Result::INVALID_ARGUMENTS);
        return Void();
    }

    // Create and launch the thread.
    auto tempWriteThread = std::make_unique<WriteThread>(
                               &mStopWriteThread, this, tempCommandMQ.get(), tempDataMQ.get(),
                               tempStatusMQ.get(), tempElfGroup.get());
    if (!tempWriteThread->init()) {
        ALOGW("failed to start writer thread: %s", strerror(-status));
        sendError(Result::INVALID_ARGUMENTS);
        return Void();
    }
    status = tempWriteThread->run("writer", PRIORITY_URGENT_AUDIO);
    if (status != OK) {
        ALOGW("failed to start writer thread: %s", strerror(-status));
        sendError(Result::INVALID_ARGUMENTS);
        return Void();
    }

    mCommandMQ = std::move(tempCommandMQ);
    mDataMQ = std::move(tempDataMQ);
    mStatusMQ = std::move(tempStatusMQ);
    mWriteThread = tempWriteThread.release();
    mEfGroup = tempElfGroup.release();
    threadInfo.pid = getpid();
    threadInfo.tid = mWriteThread->getTid();
    _hidl_cb(Result::OK, *mCommandMQ->getDesc(), *mDataMQ->getDesc(),
             *mStatusMQ->getDesc(), threadInfo);
    return Void();
}

Return<void> StreamOut::getRenderPosition(getRenderPosition_cb _hidl_cb) {
    uint32_t halDspFrames = 0;
    Result retval = Result::NOT_SUPPORTED;
    _hidl_cb(retval, halDspFrames);
    return Void();
}

Return<void> StreamOut::getNextWriteTimestamp(
    getNextWriteTimestamp_cb _hidl_cb) {
    Result retval(Result::NOT_SUPPORTED);
    int64_t timestampUs = 0;
    _hidl_cb(retval, timestampUs);
    return Void();
}

Return<Result> StreamOut::setCallback(const sp<IStreamOutCallback> & /*callback*/) {
    return Result::NOT_SUPPORTED;
}

Return<Result> StreamOut::clearCallback() {
    return Result::OK;
}

Return<void> StreamOut::supportsPauseAndResume(
    supportsPauseAndResume_cb _hidl_cb) {
    _hidl_cb(false, false);
    return Void();
}

Return<Result> StreamOut::pause() {
    return Result::NOT_SUPPORTED;
}

Return<Result> StreamOut::resume() {
    return Result::NOT_SUPPORTED;
}

Return<bool> StreamOut::supportsDrain() {
    return false;
}

Return<Result> StreamOut::drain(AudioDrain /*type*/) {
    return Result::NOT_SUPPORTED;
}

Return<Result> StreamOut::flush() {
    return Result::NOT_SUPPORTED;
}

// static
Result StreamOut::getPresentationPositionImpl(uint64_t */*frames*/,
        TimeSpec */*timeStamp*/) {
    // Don't logspam on EINVAL--it's normal for get_presentation_position
    // to return it sometimes. EAGAIN may be returned by A2DP audio HAL
    // implementation. ENODATA can also be reported while the writer is
    // continuously querying it, but the stream has been stopped.

    static const std::vector<int> ignoredErrors{EINVAL, EAGAIN, ENODATA};
    Result retval(Result::NOT_SUPPORTED);
    return retval;
}

Return<void> StreamOut::getPresentationPosition(
    getPresentationPosition_cb _hidl_cb) {
    uint64_t frames = 0;
    TimeSpec timeStamp = {0, 0};
    Result retval = getPresentationPositionImpl(&frames, &timeStamp);
    _hidl_cb(retval, frames, timeStamp);
    return Void();
}

Return<Result> StreamOut::start() {
    return Result::NOT_SUPPORTED;
}

Return<Result> StreamOut::stop() {
    return Result::NOT_SUPPORTED;
}

Return<void> StreamOut::createMmapBuffer(int32_t minSizeFrames,
        createMmapBuffer_cb _hidl_cb) {
    return mStreamCommon->createMmapBuffer(minSizeFrames, _hidl_cb);
}

Return<void> StreamOut::getMmapPosition(getMmapPosition_cb _hidl_cb) {
    return mStreamCommon->getMmapPosition(_hidl_cb);
}

#define AUDIO_STREAM_FRANE_SIZE_CALC(audioStreamFameSize)    if (audio_has_proportional_frames(format)) {\
        chan_samp_sz = audio_bytes_per_sample(format);\
        audioStreamFameSize = popcount(channel_mask) * chan_samp_sz;\
    } else\
        audioStreamFameSize = sizeof(int8_t);

ssize_t StreamOut::outWriteLowLatency(StreamOut *out, const void *buffer,
                                      size_t bytes) {
    int ret;
    size_t frame_size;
    size_t chan_samp_sz;
    audio_format_t format = AUDIO_FORMAT_PCM_16_BIT;

    if (audio_has_proportional_frames(format)) {
        chan_samp_sz = audio_bytes_per_sample(format);
        frame_size = popcount(channel_mask) * chan_samp_sz;
    } else {
        frame_size = sizeof(int8_t);
    }

    size_t in_frames = bytes / frame_size;
    size_t out_frames = in_frames;
    bool force_input_standby = false;
    StreamIn *in;
    int i = 0;

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the output stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    PTHREAD_MUTEX_LOCK(&out->mDevice->lock);
    PTHREAD_MUTEX_LOCK(&out->lock);
    if (out->_standby) {
        ret = startOutputStreamLowLatency(out);
        if (ret != 0) {
            PTHREAD_MUTEX_UNLOCK(&out->mDevice->lock);
            PTHREAD_MUTEX_UNLOCK(&out->lock);

            size_t audioStreamFameSize;

            if (audio_has_proportional_frames(format)) {
                chan_samp_sz = audio_bytes_per_sample(format);
                audioStreamFameSize = popcount(channel_mask) * chan_samp_sz;
            } else
                audioStreamFameSize = sizeof(int8_t);

            if (ret != 0) {
                usleep(bytes * 1000000 / audioStreamFameSize /
                       DEFAULT_OUT_SAMPLING_RATE);
            }

            if (force_input_standby) {
                PTHREAD_MUTEX_LOCK(&out->mDevice->lock);
                if (out->mDevice->active_input) {
                    in = static_cast<StreamIn *>(out->mDevice->active_input);
                    PTHREAD_MUTEX_LOCK(&in->lock);
                    //do_input_standby(in);

                    if (!in->_standby) {
                        pcm_close(in->_pcm);
                        in->_pcm = NULL;

                        in->mDevice->active_input = 0;
                        if (in->mDevice->mode != AUDIO_MODE_IN_CALL)
                            in->mDevice->devices &= ~AUDIO_DEVICE_IN_ALL;

                        if (in->echo_reference != NULL) {
                            /* stop reading from echo reference */
                            in->echo_reference->read(in->echo_reference, NULL);
                            in->putEchoReference(in->mDevice, in->echo_reference);
                            in->echo_reference = NULL;
                        }

                        in->_standby = 1;
                    }

                    PTHREAD_MUTEX_UNLOCK(&in->lock);
                }
                PTHREAD_MUTEX_UNLOCK(&out->mDevice->lock);
            }

            return bytes;
        }
        out->_standby = 0;
        /* a change in output device may change the microphone selection */
        if (out->mDevice->active_input &&
                static_cast<StreamIn *>(out->mDevice->active_input)->source ==
                AUDIO_SOURCE_VOICE_COMMUNICATION)
            force_input_standby = true;
    }
    PTHREAD_MUTEX_UNLOCK(&out->mDevice->lock);

    //for (i = 0; i < PCM_TOTAL; i++) {
    /* only use resampler if required */
    if (out->_pcm[PCM_NORMAL] && (out->config[PCM_NORMAL].rate != DEFAULT_OUT_SAMPLING_RATE)) {
        out_frames = out->buffer_frames;
        out->resampler->resample_from_input(out->resampler,
                                            (int16_t *)buffer,
                                            &in_frames,
                                            (int16_t *)out->buffer,
                                            &out_frames);
        //    break;
    }
    //}

    if (out->echo_reference != NULL) {
        struct echo_reference_buffer b;
        b.raw = (void *)buffer;
        b.frame_count = in_frames;

        getPlaybackDelay(out, out_frames, &b);
        out->echo_reference->write(out->echo_reference, &b);
    }

    /* Write to all active PCMs */
    //for (i = 0; i < PCM_TOTAL; i++) {
    if (out->_pcm[PCM_NORMAL]) {
        if (out->config[i].rate == DEFAULT_OUT_SAMPLING_RATE)
            /* PCM uses native sample rate */
            ret = pcm_write(out->_pcm[PCM_NORMAL], (void *)buffer, bytes);
        else
            /* PCM needs resampler */
            ret = pcm_write(out->_pcm[PCM_NORMAL], (void *)out->buffer, out_frames * frame_size);
        //if (ret)
        //    break;
    }
    //}

    PTHREAD_MUTEX_UNLOCK(&out->lock);

    size_t audioStreamFameSize;

    if (audio_has_proportional_frames(format)) {
        chan_samp_sz = audio_bytes_per_sample(format);
        audioStreamFameSize = popcount(channel_mask) * chan_samp_sz;
    } else
        audioStreamFameSize = sizeof(int8_t);

    if (ret != 0) {
        usleep(bytes * 1000000 / audioStreamFameSize /
               DEFAULT_OUT_SAMPLING_RATE);
    }

    if (force_input_standby) {
        PTHREAD_MUTEX_LOCK(&out->mDevice->lock);
        if (out->mDevice->active_input) {
            in = static_cast<StreamIn *>(out->mDevice->active_input);
            PTHREAD_MUTEX_LOCK(&in->lock);
            //do_input_standby(in);

            if (!in->_standby) {
                pcm_close(in->_pcm);
                in->_pcm = NULL;

                in->mDevice->active_input = 0;
                if (in->mDevice->mode != AUDIO_MODE_IN_CALL)
                    in->mDevice->devices &= ~AUDIO_DEVICE_IN_ALL;

                if (in->echo_reference != NULL) {
                    /* stop reading from echo reference */
                    in->echo_reference->read(in->echo_reference, NULL);
                    in->putEchoReference(in->mDevice, in->echo_reference);
                    in->echo_reference = NULL;
                }

                in->_standby = 1;
            }

            PTHREAD_MUTEX_UNLOCK(&in->lock);
        }
        PTHREAD_MUTEX_UNLOCK(&out->mDevice->lock);
    }

    return bytes;
}

ssize_t StreamOut::outWriteHdmi(StreamOut *out, const void *buffer,
                                size_t bytes) {
    int ret;
    size_t frame_size;
    size_t chan_samp_sz;
    audio_format_t format = AUDIO_FORMAT_PCM_16_BIT;

    if (audio_has_proportional_frames(format)) {
        chan_samp_sz = audio_bytes_per_sample(format);
        frame_size = popcount(channel_mask) * chan_samp_sz;
    } else {
        frame_size = sizeof(int8_t);
    }

    size_t in_frames = bytes / frame_size;

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the output stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    PTHREAD_MUTEX_LOCK(&out->mDevice->lock);
    PTHREAD_MUTEX_LOCK(&out->lock);
    if (out->_standby) {
        ret = startOutputStreamHdmi(out);
        if (ret != 0) {
            PTHREAD_MUTEX_UNLOCK(&out->mDevice->lock);
            goto exit;
        }
        out->_standby = 0;
    }
    PTHREAD_MUTEX_UNLOCK(&out->mDevice->lock);

    ret = pcm_write(out->_pcm[PCM_HDMI],
                    buffer,
                    pcm_frames_to_bytes(out->_pcm[PCM_HDMI], in_frames));

exit:
    PTHREAD_MUTEX_UNLOCK(&out->lock);

    if (ret != 0) {
        size_t audioStreamFameSize;
        AUDIO_STREAM_FRANE_SIZE_CALC(audioStreamFameSize);
        usleep(bytes * 1000000 / audioStreamFameSize /
               out->config[PCM_HDMI].rate);
    }
    /* FIXME: workaround for HDMI multi channel channel swap on first playback after opening
     * the output stream: force reopening the pcm driver after writing a few periods. */
    if ((out->restart_periods_cnt > 0) &&
            (--out->restart_periods_cnt == 0))
        standby();

    return bytes;
}

#undef AUDIO_STREAM_FRANE_SIZE_CALC

ssize_t StreamOut::outWrite(StreamOut *out, const void *buffer,
                            size_t bytes) {
    if (out->mStreamType == OUTPUT_LOW_LATENCY)
        return outWriteLowLatency(out, buffer, bytes);
    else
        return outWriteHdmi(out, buffer, bytes);
}

int StreamOut::getPlaybackDelay(StreamOut *out,
                                size_t frames,
                                echo_reference_buffer *buffer) {
    unsigned int kernel_frames;
    int status;
    int primary_pcm = 0;

    /* Find the first active PCM to act as primary */
    while ((primary_pcm < PCM_TOTAL) && !out->_pcm[primary_pcm])
        primary_pcm++;

    status = pcm_get_htimestamp(out->_pcm[primary_pcm], &kernel_frames,
                                &buffer->time_stamp);
    if (status < 0) {
        buffer->time_stamp.tv_sec  = 0;
        buffer->time_stamp.tv_nsec = 0;
        buffer->delay_ns           = 0;
        ALOGV("get_playback_delay(): pcm_get_htimestamp error,"
              "setting playbackTimestamp to 0");
        return status;
    }

    kernel_frames = pcm_get_buffer_size(out->_pcm[primary_pcm]) - kernel_frames;

    /* adjust render time stamp with delay added by current driver buffer.
     * Add the duration of current frame as we want the render time of the last
     * sample being written. */
    buffer->delay_ns = (long)(((int64_t)(kernel_frames + frames) * 1000000000) /
                              DEFAULT_OUT_SAMPLING_RATE);

    return 0;
}

/* must be called with hw device and output stream mutexes locked */
int StreamOut::startOutputStreamLowLatency(StreamOut *out) {
    ALOGV("%s", __FUNCTION__);
    /* Something not a dock in use */
    out->config[PCM_NORMAL] = pcm_config_dac;
    out->config[PCM_NORMAL].rate = DEFAULT_OUT_SAMPLING_RATE;
    out->_pcm[PCM_NORMAL] = pcm_open(CARD_GEN3_DEFAULT, PORT_DAC,
                                     PCM_OUT, &out->config[PCM_NORMAL]);

    /* Close any PCMs that could not be opened properly and return an error */
    if (out->_pcm[PCM_NORMAL] && !pcm_is_ready(out->_pcm[PCM_NORMAL])) {
        ALOGE("cannot open pcm_out driver normal: %s",
              pcm_get_error(out->_pcm[PCM_NORMAL]));
        pcm_close(out->_pcm[PCM_NORMAL]);
        out->_pcm[PCM_NORMAL] = NULL;
        return -ENOMEM;
    }

    out->buffer_frames = pcm_config_dac.period_size * 2;

    size_t frame_size;
    size_t chan_samp_sz;
    audio_format_t format = AUDIO_FORMAT_PCM_16_BIT;

    if (audio_has_proportional_frames(format)) {
        chan_samp_sz = audio_bytes_per_sample(format);
        frame_size = popcount(channel_mask) * chan_samp_sz;
    } else {
        frame_size = sizeof(int8_t);
    }


    if (out->buffer == NULL)
        out->buffer = (char *)malloc(out->buffer_frames * frame_size);

    if (out->mDevice->echo_reference != NULL)
        out->echo_reference = out->mDevice->echo_reference;
    out->resampler->reset(out->resampler);

    return 0;
}

int StreamOut::startOutputStreamHdmi(StreamOut *out) {
    ALOGV("%s", __FUNCTION__);

    /* force standby on low latency output stream to close HDMI driver in case it was in use */
    if (out->mDevice->outputs[OUTPUT_LOW_LATENCY] != NULL &&
            !(static_cast<StreamOut *>(out->mDevice->outputs[OUTPUT_LOW_LATENCY])->_standby)) {
        StreamOut *ll_out = static_cast<StreamOut *>(out->mDevice->outputs[OUTPUT_LOW_LATENCY]);
        PTHREAD_MUTEX_LOCK(&ll_out->lock);
        doOutStandby(ll_out);
        PTHREAD_MUTEX_UNLOCK(&ll_out->lock);
    }

    out->_pcm[PCM_HDMI] = pcm_open(CARD_GEN3_HDMI, PORT_HDMI, PCM_OUT, &out->config[PCM_HDMI]);

    if (out->_pcm[PCM_HDMI] && !pcm_is_ready(out->_pcm[PCM_HDMI])) {
        ALOGE("cannot open pcm_out driver: %s", pcm_get_error(out->_pcm[PCM_HDMI]));
        pcm_close(out->_pcm[PCM_HDMI]);
        out->_pcm[PCM_HDMI] = NULL;
        return -ENOMEM;
    }
    return 0;
}

}  // namespace salvator
}  // namespace V2_0
}  // namespace audio
}  // namespace hardware
}  // namespace android
