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

#include <log/log.h>
#include <utils/Trace.h>
#include <memory>

#include "StreamIn.h"
#include "StreamOut.h"
#include "Util.h"
#include "EffectMap.h"

namespace android {
namespace hardware {
namespace audio {
namespace V2_0 {
namespace salvator {

using ::android::hardware::audio::common::V2_0::ThreadInfo;
using ::android::hardware::audio::V2_0::MessageQueueFlagBits;

namespace {

class ReadThread : public Thread {
public:
    // ReadThread's lifespan never exceeds StreamIn's lifespan.
    ReadThread(std::atomic<bool> *stop, StreamIn *stream,
               StreamIn::CommandMQ *commandMQ, StreamIn::DataMQ *dataMQ,
               StreamIn::StatusMQ *statusMQ, EventFlag *efGroup)
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
    virtual ~ReadThread() {}

private:
    std::atomic<bool> *mStop;
    StreamIn *mStream;
    StreamIn::CommandMQ *mCommandMQ;
    StreamIn::DataMQ *mDataMQ;
    StreamIn::StatusMQ *mStatusMQ;
    EventFlag *mEfGroup;
    std::unique_ptr<uint8_t[]> mBuffer;
    IStreamIn::ReadParameters mParameters;
    IStreamIn::ReadStatus mStatus;

    bool threadLoop() override;

    void doGetCapturePosition();
    void doRead();
};

void ReadThread::doRead() {
    size_t availableToWrite = mDataMQ->availableToWrite();
    size_t requestedToRead = mParameters.params.read;
    if (requestedToRead > availableToWrite) {
        ALOGW(
            "truncating read data from %d to %d due to insufficient data queue "
            "space",
            (int32_t)requestedToRead, (int32_t)availableToWrite);
        requestedToRead = availableToWrite;
    }
    ssize_t readResult = mStream->readStreamIn(mStream, &mBuffer[0],
                         requestedToRead);
    mStatus.retval = Result::OK;
    uint64_t read = 0;
    if (readResult >= 0) {
        mStatus.reply.read = readResult;
        if (!mDataMQ->write(&mBuffer[0], readResult)) {
            ALOGW("data message queue write failed");
        }
    } else {
        mStatus.retval = Stream::analyzeStatus("read", readResult);
    }
}

void ReadThread::doGetCapturePosition() {
    /*mStatus.retval = StreamIn::getCapturePositionImpl(
        mStream, &mStatus.reply.capturePosition.frames,
        &mStatus.reply.capturePosition.time);*/
}

bool ReadThread::threadLoop() {
    // This implementation doesn't return control back to the Thread until it
    // decides to stop,
    // as the Thread uses mutexes, and this can lead to priority inversion.
    while (!std::atomic_load_explicit(mStop, std::memory_order_acquire)) {
        uint32_t efState = 0;
        mEfGroup->wait(static_cast<uint32_t>(MessageQueueFlagBits::NOT_FULL),
                       &efState);
        if (!(efState &
                static_cast<uint32_t>(MessageQueueFlagBits::NOT_FULL))) {
            continue;  // Nothing to do.
        }
        if (!mCommandMQ->read(&mParameters)) {
            continue;  // Nothing to do.
        }
        mStatus.replyTo = mParameters.command;
        switch (mParameters.command) {
        case IStreamIn::ReadCommand::READ:
            doRead();
            break;
        case IStreamIn::ReadCommand::GET_CAPTURE_POSITION:
            doGetCapturePosition();
            break;
        default:
            ALOGE("Unknown read thread command code %d",
                  mParameters.command);
            mStatus.retval = Result::NOT_SUPPORTED;
            break;
        }
        if (!mStatusMQ->write(&mStatus)) {
            ALOGW("status message queue write failed");
        }
        mEfGroup->wake(static_cast<uint32_t>(MessageQueueFlagBits::NOT_EMPTY));
    }

    return false;
}

}  // namespace

pthread_mutex_t StreamIn::lock;

StreamIn::StreamIn(const sp<Device> &device)
    : mIsClosed(false),
      mDevice(device),
      mStreamCommon(new Stream(this)),
      mEfGroup(nullptr),
      mStopReadThread(false),
      read_buf(nullptr),
      resampler(nullptr),
      proc_buf_in(nullptr),
      proc_buf_out(nullptr),
      echo_reference(nullptr),
      ref_buf(nullptr) {}

StreamIn::~StreamIn() {
    ATRACE_CALL();
    close();
    if (mReadThread.get()) {
        ATRACE_NAME("mReadThread->join");
        status_t status = mReadThread->join();
        ALOGE_IF(status, "read thread exit error: %s", strerror(-status));
    }
    if (mEfGroup) {
        status_t status = EventFlag::deleteEventFlag(&mEfGroup);
        ALOGE_IF(status, "read MQ event flag deletion error: %s",
                 strerror(-status));
    }
    mDevice->closeInputStream(this);
}

// Methods from ::android::hardware::audio::V2_0::IStream follow.
Return<uint64_t> StreamIn::getFrameSize() {
    //return audio_stream_in_frame_size(mStream);
    size_t chan_samp_sz;
    audio_format_t format = AUDIO_FORMAT_PCM_16_BIT;

    if (audio_has_proportional_frames(format)) {
        chan_samp_sz = audio_bytes_per_sample(format);
        return audio_channel_count_from_in_mask(main_channels) * chan_samp_sz;
    }

    return sizeof(int8_t);
}

Return<uint64_t> StreamIn::getFrameCount() {
    return mStreamCommon->getFrameCount();
}

Return<uint64_t> StreamIn::getBufferSize() {
    //return mStreamCommon->getBufferSize();

    uint32_t sample_rate = requested_rate;
    AudioFormat format = AudioFormat::PCM_16_BIT;
    int channel_count = popcount(main_channels);
    size_t size;
    size_t device_rate;

    if (this->mDevice->checkInputParameters(sample_rate, format,
                                            channel_count) != 0)
        return 0;

    /* take resampling into account and return the closest majoring
    multiple of 16 frames, as audioflinger expects audio buffers to
    be a multiple of 16 frames */
    size = (pcm_config_mic.period_size * sample_rate) / pcm_config_mic.rate;
    size = ((size + 15) / 16) * 16;

    return size * channel_count * sizeof(short);
}

Return<uint32_t> StreamIn::getSampleRate() {
    return this->requested_rate;
}

Return<void> StreamIn::getSupportedSampleRates(
    getSupportedSampleRates_cb _hidl_cb) {
    return mStreamCommon->getSupportedSampleRates(_hidl_cb);
}

Return<Result> StreamIn::setSampleRate(uint32_t sampleRateHz) {
    return mStreamCommon->setSampleRate(sampleRateHz);
}

Return<AudioChannelMask> StreamIn::getChannelMask() {
    return AudioChannelMask(this->main_channels);
}

Return<void> StreamIn::getSupportedChannelMasks(
    getSupportedChannelMasks_cb _hidl_cb) {
    return mStreamCommon->getSupportedChannelMasks(_hidl_cb);
}

Return<Result> StreamIn::setChannelMask(AudioChannelMask mask) {
    return mStreamCommon->setChannelMask(mask);
}

Return<AudioFormat> StreamIn::getFormat() {
    return AudioFormat(AUDIO_FORMAT_PCM_16_BIT);
}

Return<void> StreamIn::getSupportedFormats(getSupportedFormats_cb _hidl_cb) {
    return mStreamCommon->getSupportedFormats(_hidl_cb);
}

Return<Result> StreamIn::setFormat(AudioFormat format) {
    return mStreamCommon->setFormat(format);
}

Return<void> StreamIn::getAudioProperties(getAudioProperties_cb _hidl_cb) {
    uint32_t halSampleRate = this->getSampleRate();
    audio_channel_mask_t halMask = this->main_channels;
    audio_format_t halFormat = AUDIO_FORMAT_PCM_16_BIT;
    _hidl_cb(halSampleRate, AudioChannelMask(halMask), AudioFormat(halFormat));
    return Void();
}

Return<Result> StreamIn::addEffect(uint64_t effectId) {
    Result status;
    effect_descriptor_t desc;
    effect_handle_t effect = EffectMap::getInstance().get(effectId);

    if (effect == nullptr)
        return Result::INVALID_ARGUMENTS;

    PTHREAD_MUTEX_LOCK(&this->mDevice->lock);
    PTHREAD_MUTEX_LOCK(&this->lock);
    if (this->num_preprocessors >= MAX_PREPROCESSORS) {
        status = Result::NOT_SUPPORTED;
        ALOGW("addEffect() error %d", status);
        PTHREAD_MUTEX_UNLOCK(&this->lock);
        PTHREAD_MUTEX_UNLOCK(&this->mDevice->lock);
        return status;
    }

    status = Stream::analyzeStatus("addEffect", (*effect)->get_descriptor(effect, &desc));
    if (status != Result::OK) {
        ALOGW("addEffect() error %d", status);
        PTHREAD_MUTEX_UNLOCK(&this->lock);
        PTHREAD_MUTEX_UNLOCK(&this->mDevice->lock);
        return status;
    }

    this->preprocessors[this->num_preprocessors] = effect;
    this->num_preprocessors++;

    ALOGV("addEffect(), effect type: %08x", desc.type.timeLow);

    if (memcmp(&desc.type, FX_IID_AEC, sizeof(effect_uuid_t)) == 0) {
        this->need_echo_reference = true;
        doInputStandby(this);
        configureReverse(this);
    }


    ALOGW_IF(status != Result::OK, "addEffect() error %d", status);
    PTHREAD_MUTEX_UNLOCK(&this->lock);
    PTHREAD_MUTEX_UNLOCK(&this->mDevice->lock);
    return status;
}

Return<Result> StreamIn::removeEffect(uint64_t effectId) {
    int i;
    Result status = Result::INVALID_ARGUMENTS;
    effect_descriptor_t desc;
    effect_handle_t effect = EffectMap::getInstance().get(effectId);

    if (effect == nullptr)
        return Result::INVALID_ARGUMENTS;

    PTHREAD_MUTEX_LOCK(&this->mDevice->lock);
    PTHREAD_MUTEX_LOCK(&this->lock);
    if (this->num_preprocessors <= 0) {
        status = Result::NOT_SUPPORTED;
        ALOGW("removeEffect() error %d", status);
        PTHREAD_MUTEX_UNLOCK(&this->lock);
        PTHREAD_MUTEX_UNLOCK(&this->mDevice->lock);
        return status;
    }

    for (i = 0; i < this->num_preprocessors; i++) {
        if (status == Result::OK) {
            /* status == Result::OK means an effect was removed from a previous slot */
            this->preprocessors[i - 1] = this->preprocessors[i];
            ALOGV("removeEffect moving fx from %d to %d", i, i - 1);
            continue;
        }
        if (this->preprocessors[i] == effect) {
            ALOGV("removeEffect found fx at index %d", i);
            status = Result::OK;
        }
    }

    if (status != Result::OK) {
        ALOGW("removeEffect() error %d", status);
        PTHREAD_MUTEX_UNLOCK(&this->lock);
        PTHREAD_MUTEX_UNLOCK(&this->mDevice->lock);
        return status;
    }

    this->num_preprocessors--;
    /* if we remove one effect, at least the last preproc should be reset */
    this->preprocessors[this->num_preprocessors] = NULL;

    status = Stream::analyzeStatus("removeEffect", (*effect)->get_descriptor(effect, &desc));
    if (status != Result::OK) {
        ALOGW( "removeEffect() error %d", status);
        PTHREAD_MUTEX_UNLOCK(&this->lock);
        PTHREAD_MUTEX_UNLOCK(&this->mDevice->lock);
        return status;
    }

    ALOGV("removeEffect(), effect type: %08x", desc.type.timeLow);

    if (memcmp(&desc.type, FX_IID_AEC, sizeof(effect_uuid_t)) == 0) {
        this->need_echo_reference = false;
        doInputStandby(this);
    }

    ALOGW_IF(status != Result::OK, "removeEffect() error %d", status);
    PTHREAD_MUTEX_UNLOCK(&this->lock);
    PTHREAD_MUTEX_UNLOCK(&this->mDevice->lock);
    return status;
}

Return<Result> StreamIn::standby() {
    PTHREAD_MUTEX_LOCK(&mDevice->lock);
    PTHREAD_MUTEX_LOCK(&this->lock);
    doInputStandby(this);
    PTHREAD_MUTEX_UNLOCK(&this->lock);
    PTHREAD_MUTEX_UNLOCK(&mDevice->lock);
    return Result::OK;
}

Return<AudioDevice> StreamIn::getDevice() {
    return mStreamCommon->getDevice();
}

Return<Result> StreamIn::setDevice(const DeviceAddress &address) {
    return mStreamCommon->setDevice(address);
}

Return<Result> StreamIn::setConnectedState(const DeviceAddress &address,
        bool connected) {
    return mStreamCommon->setConnectedState(address, connected);
}

Return<Result> StreamIn::setHwAvSync(uint32_t hwAvSync) {
    return mStreamCommon->setHwAvSync(hwAvSync);
}

Return<void> StreamIn::getParameters(const hidl_vec<hidl_string> &keys,
                                     getParameters_cb _hidl_cb) {
    return mStreamCommon->getParameters(keys, _hidl_cb);
}

Return<Result> StreamIn::setParameters(
    const hidl_vec<ParameterValue> &parameters) {
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
    bool do_standby = false;

    parms = str_parms_create_str(kvpairs);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE, value,
                            sizeof(value));

    PTHREAD_MUTEX_LOCK(&mDevice->lock);
    PTHREAD_MUTEX_LOCK(&this->lock);
    if (ret >= 0) {
        val = atoi(value);
        /* no audio source uses val == 0 */
        if ((this->source != val) && (val != 0)) {
            this->source = val;
            do_standby = true;
        }
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value,
                            sizeof(value));
    if (ret >= 0) {
        val = atoi(value);
        if ((this->device != val) && (val != 0)) {
            this->device = val;
            do_standby = true;
        }
    }

    if (do_standby) {
        doInputStandby(this);
    }
    PTHREAD_MUTEX_UNLOCK(&this->lock);
    PTHREAD_MUTEX_UNLOCK(&mDevice->lock);

    str_parms_destroy(parms);
    free(temp_str_parms);

    if (ret >= 0)
        return Result::OK;
    else
        return Result::INVALID_ARGUMENTS;
}

Return<void> StreamIn::debugDump(const hidl_handle &fd) {
    return Void();
}

Return<Result> StreamIn::start() {
    return Result::NOT_SUPPORTED;
}

Return<Result> StreamIn::stop() {
    return Result::NOT_SUPPORTED;
}

Return<void> StreamIn::createMmapBuffer(int32_t minSizeFrames,
                                        createMmapBuffer_cb _hidl_cb) {
    return mStreamCommon->createMmapBuffer(minSizeFrames, _hidl_cb);
}

Return<void> StreamIn::getMmapPosition(getMmapPosition_cb _hidl_cb) {
    return mStreamCommon->getMmapPosition(_hidl_cb);
}

Return<Result> StreamIn::close() {
    if (mIsClosed) return Result::INVALID_STATE;
    mIsClosed = true;
    if (mReadThread.get()) {
        mStopReadThread.store(true, std::memory_order_release);
    }
    if (mEfGroup) {
        mEfGroup->wake(static_cast<uint32_t>(MessageQueueFlagBits::NOT_FULL));
    }
    return Result::OK;
}

// Methods from ::android::hardware::audio::V2_0::IStreamIn follow.
Return<void> StreamIn::getAudioSource(getAudioSource_cb _hidl_cb) {
    int halSource;
    Result retval =
        mStreamCommon->getParam(AudioParameter::keyInputSource, &halSource);
    AudioSource source(AudioSource::DEFAULT);
    if (retval == Result::OK) {
        source = AudioSource(halSource);
    }
    _hidl_cb(retval, source);
    return Void();
}

Return<Result> StreamIn::setGain(float gain) {
    if (!isGainNormalized(gain)) {
        ALOGW("Can not set a stream input gain (%f) outside [0,1]", gain);
        return Result::INVALID_ARGUMENTS;
    }
    return Result::OK;
}

Return<void> StreamIn::prepareForReading(uint32_t frameSize,
        uint32_t framesCount,
        prepareForReading_cb _hidl_cb) {
    status_t status;
    ThreadInfo threadInfo = {0, 0};

    // Wrap the _hidl_cb to return an error
    auto sendError = [this, &threadInfo, &_hidl_cb](Result result) {
        _hidl_cb(result, CommandMQ::Descriptor(), DataMQ::Descriptor(),
                 StatusMQ::Descriptor(), threadInfo);

    };

    // Create message queues.
    if (mDataMQ) {
        ALOGE("the client attempts to call prepareForReading twice");
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
    auto tempReadThread = std::make_unique<ReadThread>(
                              &mStopReadThread, this/*mStream*/, tempCommandMQ.get(), tempDataMQ.get(),
                              tempStatusMQ.get(), tempElfGroup.get());
    if (!tempReadThread->init()) {
        ALOGW("failed to start reader thread: %s", strerror(-status));
        sendError(Result::INVALID_ARGUMENTS);
        return Void();
    }
    status = tempReadThread->run("reader", PRIORITY_URGENT_AUDIO);
    if (status != OK) {
        ALOGW("failed to start reader thread: %s", strerror(-status));
        sendError(Result::INVALID_ARGUMENTS);
        return Void();
    }

    mCommandMQ = std::move(tempCommandMQ);
    mDataMQ = std::move(tempDataMQ);
    mStatusMQ = std::move(tempStatusMQ);
    mReadThread = tempReadThread.release();
    mEfGroup = tempElfGroup.release();
    threadInfo.pid = getpid();
    threadInfo.tid = mReadThread->getTid();
    _hidl_cb(Result::OK, *mCommandMQ->getDesc(), *mDataMQ->getDesc(),
             *mStatusMQ->getDesc(), threadInfo);
    return Void();
}

Return<uint32_t> StreamIn::getInputFramesLost() {
    return 0;
}

// static
Result StreamIn::getCapturePositionImpl(StreamIn *stream,
                                        uint64_t *frames, uint64_t *time) {
    // HAL may have a stub function, always returning ENOSYS, don't
    // spam the log in this case.
    static const std::vector<int> ignoredErrors{ENOSYS};
    Result retval(Result::NOT_SUPPORTED);
    return retval;
};

Return<void> StreamIn::getCapturePosition(getCapturePosition_cb _hidl_cb) {
    uint64_t frames = 0, time = 0;
    Result retval =
        Result::NOT_SUPPORTED;
    _hidl_cb(retval, frames, time);
    return Void();
}

void StreamIn::putEchoReference(sp<Device> dev,
                                echo_reference_itfe *reference) {
    if (mDevice->echo_reference != NULL &&
            reference == mDevice->echo_reference) {
        /* echo reference is taken from the low latency output stream used
         * for voice use cases */
        if (mDevice->outputs[OUTPUT_LOW_LATENCY] != NULL &&
                !(static_cast<StreamOut *>(mDevice->outputs[OUTPUT_LOW_LATENCY]))->_standby) {
            //remove_echo_reference(mDevice->outputs[OUTPUT_LOW_LATENCY], reference);
            StreamOut *so = static_cast<StreamOut *>(mDevice->outputs[OUTPUT_LOW_LATENCY]);
            PTHREAD_MUTEX_LOCK(&so->lock);
            if (so->echo_reference == reference) {
                /* stop writing to echo reference */
                reference->write(reference, NULL);
                so->echo_reference = NULL;
            }
            PTHREAD_MUTEX_UNLOCK(&so->lock);
        }
        release_echo_reference(reference);
        mDevice->echo_reference = NULL;
    }
}

//in_read
ssize_t StreamIn::readStreamIn(StreamIn *stream, void *buffer,
                               size_t bytes) {
    int ret = 0;
    StreamIn *in = (StreamIn *)stream;
    sp<Device> adev = in->mDevice;

    size_t chan_samp_sz;
    size_t audioStreamFrameSize;
    audio_format_t format = AUDIO_FORMAT_PCM_16_BIT;

    if (audio_has_proportional_frames(format)) {
        chan_samp_sz = audio_bytes_per_sample(format);
        audioStreamFrameSize = popcount(in->main_channels) * chan_samp_sz;
    } else
        audioStreamFrameSize = sizeof(int8_t);

    size_t frames_rq = bytes / audioStreamFrameSize;

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the input stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    PTHREAD_MUTEX_LOCK(&adev->lock);
    PTHREAD_MUTEX_LOCK(&in->lock);
    if (in->_standby) {
        ret = startInputStream(in);
        if (ret == 0)
            in->_standby = 0;
    }
    PTHREAD_MUTEX_UNLOCK(&adev->lock);

    if (ret < 0) {
        if (ret < 0)
            usleep(bytes * 1000000 / audioStreamFrameSize /
                   in->requested_rate);

        PTHREAD_MUTEX_UNLOCK(&in->lock);
        return bytes;
    }

    if (in->num_preprocessors != 0)
        ret = processFrames(in, buffer, frames_rq);
    else if (in->resampler != NULL)
        ret = readFrames(in, buffer, frames_rq);
    else
        ret = pcm_read(in->_pcm, buffer, bytes);

    if (ret > 0)
        ret = 0;

    if (ret == 0 && adev->mic_mute)
        memset(buffer, 0, bytes);

    if (ret < 0)
        usleep(bytes * 1000000 / audioStreamFrameSize /
               in->requested_rate);

    PTHREAD_MUTEX_UNLOCK(&in->lock);
    return bytes;
}

int StreamIn::configureReverse(StreamIn *in) {
    int32_t cmd_status;
    uint32_t size = sizeof(int);
    effect_config_t config;
    int32_t status = 0;
    int32_t fct_status = 0;
    int i;

    if (in->num_preprocessors > 0) {
        config.inputCfg.channels = in->main_channels;
        config.outputCfg.channels = in->main_channels;
        config.inputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
        config.outputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
        config.inputCfg.samplingRate = in->requested_rate;
        config.outputCfg.samplingRate = in->requested_rate;
        config.inputCfg.mask =
            ( EFFECT_CONFIG_SMP_RATE | EFFECT_CONFIG_CHANNELS | EFFECT_CONFIG_FORMAT );
        config.outputCfg.mask =
            ( EFFECT_CONFIG_SMP_RATE | EFFECT_CONFIG_CHANNELS | EFFECT_CONFIG_FORMAT );

        for (i = 0; i < in->num_preprocessors; i++) {
            if ((*in->preprocessors[i])->process_reverse == NULL)
                continue;
            fct_status = (*(in->preprocessors[i]))->command(
                             in->preprocessors[i],
                             EFFECT_CMD_SET_CONFIG_REVERSE,
                             sizeof(effect_config_t),
                             &config,
                             &size,
                             &cmd_status);
            do {
                if (fct_status != 0)
                    status = fct_status;
                else if (cmd_status != 0)
                    status = cmd_status;
            } while (0);
        }
    }
    return status;
}

int StreamIn::startInputStream(StreamIn *stream) {
    int ret = 0;

    stream->mDevice->active_input = stream;

    if (stream->mDevice->mode != AUDIO_MODE_IN_CALL) {
        stream->mDevice->devices &= ~AUDIO_DEVICE_IN_ALL;
        stream->mDevice->devices |= stream->device;
    }

    if (stream->aux_channels_changed) {
        stream->aux_channels_changed = false;
        stream->config.channels = popcount(stream->main_channels |
                                           stream->aux_channels);

        if (stream->resampler) {
            /* release and recreate the resampler with the new number of channel of the input */
            release_resampler(stream->resampler);
            stream->resampler = NULL;
            ret = create_resampler(stream->config.rate,
                                   stream->requested_rate,
                                   stream->config.channels,
                                   RESAMPLER_QUALITY_DEFAULT,
                                   &stream->buf_provider,
                                   &stream->resampler);
        }
        ALOGV("start_input_stream(): New channel configuration, "
              "main_channels = [%04x], aux_channels = [%04x], config.channels = %d",
              stream->main_channels, stream->aux_channels, stream->config.channels);
    }

    if (stream->need_echo_reference && stream->echo_reference == NULL)
        stream->echo_reference = getEchoReference(stream->mDevice,
                                 AUDIO_FORMAT_PCM_16_BIT,
                                 stream->config.channels,
                                 stream->requested_rate);

    /* this assumes routing is done previously */
    stream->_pcm = pcm_open(0, PORT_MIC, PCM_IN, &stream->config);
    if (!pcm_is_ready(stream->_pcm)) {
        ALOGE("cannot open pcm_in driver: %s", pcm_get_error(stream->_pcm));
        pcm_close(stream->_pcm);
        stream->mDevice->active_input = NULL;
        return -ENOMEM;
    }

    /* force read and proc buf reallocation case of frame size or channel count change */
    stream->read_buf_frames = 0;
    stream->read_buf_size = 0;
    stream->proc_buf_frames = 0;
    stream->proc_buf_size = 0;
    /* if no supported sample rate is available, use the resampler */
    if (stream->resampler) {
        stream->resampler->reset(stream->resampler);
    }
    return 0;
}

/* process_frames() reads frames from kernel driver (via read_frames()),
 * calls the active audio pre processings and output the number of frames requested
 * to the buffer specified */
ssize_t StreamIn::processFrames(StreamIn *in, void *buffer, ssize_t frames) {
    ssize_t frames_wr = 0;
    audio_buffer_t in_buf;
    audio_buffer_t out_buf;
    int i;
    bool has_aux_channels = (~in->main_channels & in->aux_channels);
    void *proc_buf_out;

    if (has_aux_channels)
        proc_buf_out = in->proc_buf_out;
    else
        proc_buf_out = buffer;

    /* since all the processing below is done in frames and using the config.channels
     * as the number of channels, no changes is required in case aux_channels are present */
    while (frames_wr < frames) {
        /* first reload enough frames at the end of process input buffer */
        if (in->proc_buf_frames < (size_t)frames) {
            ssize_t frames_rd;

            if (in->proc_buf_size < (size_t)frames) {
                size_t size_in_bytes = pcm_frames_to_bytes(in->_pcm, frames);

                in->proc_buf_size = (size_t)frames;
                in->proc_buf_in = (int16_t *)realloc(in->proc_buf_in, size_in_bytes);
                ALOG_ASSERT((in->proc_buf_in != NULL),
                            "process_frames() failed to reallocate proc_buf_in");
                if (has_aux_channels) {
                    in->proc_buf_out = (int16_t *)realloc(in->proc_buf_out, size_in_bytes);
                    ALOG_ASSERT((in->proc_buf_out != NULL),
                                "process_frames() failed to reallocate proc_buf_out");
                    proc_buf_out = in->proc_buf_out;
                }
                ALOGV("process_frames(): proc_buf_in %p extended to %zu bytes",
                      in->proc_buf_in, size_in_bytes);
            }
            frames_rd = readFrames(in,
                                   in->proc_buf_in +
                                   in->proc_buf_frames * in->config.channels,
                                   frames - in->proc_buf_frames);
            if (frames_rd < 0) {
                frames_wr = frames_rd;
                break;
            }
            in->proc_buf_frames += frames_rd;
        }

        if (in->echo_reference != NULL)
            pushEchoReference(in, in->proc_buf_frames);

        /* in_buf.frameCount and out_buf.frameCount indicate respectively
         * the maximum number of frames to be consumed and produced by process() */
        in_buf.frameCount = in->proc_buf_frames;
        in_buf.s16 = in->proc_buf_in;
        out_buf.frameCount = frames - frames_wr;
        out_buf.s16 = (int16_t *)proc_buf_out + frames_wr * in->config.channels;

        /* FIXME: this works because of current pre processing library implementation that
         * does the actual process only when the last enabled effect process is called.
         * The generic solution is to have an output buffer for each effect and pass it as
         * input to the next.
         */
        for (i = 0; i < in->num_preprocessors; i++) {
            (*in->preprocessors[i])->process(in->preprocessors[i],
                                             &in_buf,
                                             &out_buf);
        }

        /* process() has updated the number of frames consumed and produced in
         * in_buf.frameCount and out_buf.frameCount respectively
         * move remaining frames to the beginning of in->proc_buf_in */
        in->proc_buf_frames -= in_buf.frameCount;

        if (in->proc_buf_frames) {
            memcpy(in->proc_buf_in,
                   in->proc_buf_in + in_buf.frameCount * in->config.channels,
                   in->proc_buf_frames * in->config.channels * sizeof(int16_t));
        }

        /* if not enough frames were passed to process(), read more and retry. */
        if (out_buf.frameCount == 0) {
            ALOGW("No frames produced by preproc");
            continue;
        }

        if ((frames_wr + (ssize_t)out_buf.frameCount) <= frames) {
            frames_wr += out_buf.frameCount;
        } else {
            /* The effect does not comply to the API. In theory, we should never end up here! */
            ALOGE("preprocessing produced too many frames: %d + %zu  > %d !",
                  (unsigned int)frames_wr, out_buf.frameCount, (unsigned int)frames);
            frames_wr = frames;
        }
    }

    /* Remove aux_channels that have been added on top of main_channels
     * Assumption is made that the channels are interleaved and that the main
     * channels are first. */
    if (has_aux_channels) {
        size_t src_channels = in->config.channels;
        size_t dst_channels = popcount(in->main_channels);
        int16_t *src_buffer = (int16_t *)proc_buf_out;
        int16_t *dst_buffer = (int16_t *)buffer;

        if (dst_channels == 1) {
            for (i = frames_wr; i > 0; i--) {
                *dst_buffer++ = *src_buffer;
                src_buffer += src_channels;
            }
        } else {
            for (i = frames_wr; i > 0; i--) {
                memcpy(dst_buffer, src_buffer, dst_channels * sizeof(int16_t));
                dst_buffer += dst_channels;
                src_buffer += src_channels;
            }
        }
    }

    return frames_wr;
}

/* read_frames() reads frames from kernel driver, down samples to capture rate
 * if necessary and output the number of frames requested to the buffer specified */
ssize_t StreamIn::readFrames(StreamIn *in, void *buffer, ssize_t frames) {
    ssize_t frames_wr = 0;

    while (frames_wr < frames) {
        size_t frames_rd = frames - frames_wr;
        if (in->resampler != NULL) {
            in->resampler->resample_from_provider(in->resampler,
                                                  (int16_t *)((char *)buffer +
                                                          pcm_frames_to_bytes(in->_pcm , frames_wr)),
                                                  &frames_rd);
        } else {
            resampler_buffer buf = {
                { .raw = NULL, },
                .frame_count = frames_rd,
            };
            getNextBuffer(in, &in->buf_provider, &buf);
            if (buf.raw != NULL) {
                memcpy((char *)buffer +
                       pcm_frames_to_bytes(in->_pcm, frames_wr),
                       buf.raw,
                       pcm_frames_to_bytes(in->_pcm, buf.frame_count));
                frames_rd = buf.frame_count;
            }
            releaseBuffer(&in->buf_provider, &buf);
        }
        /* in->read_status is updated by getNextBuffer() also called by
         * in->resampler->resample_from_provider() */
        if (in->read_status != 0)
            return in->read_status;

        frames_wr += frames_rd;
    }
    return frames_wr;
}

int StreamIn::getNextBuffer(StreamIn *in,
                            resampler_buffer_provider *buffer_provider,
                            resampler_buffer *buffer) {

    if (buffer_provider == NULL || buffer == NULL)
        return -EINVAL;

    if (in->_pcm == NULL) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

    if (in->read_buf_frames == 0) {
        size_t size_in_bytes = pcm_frames_to_bytes(in->_pcm, in->config.period_size);
        if (in->read_buf_size < in->config.period_size) {
            in->read_buf_size = in->config.period_size;
            in->read_buf = (int16_t *) realloc(in->read_buf, size_in_bytes);
            ALOG_ASSERT((in->read_buf != NULL),
                        "get_next_buffer() failed to reallocate read_buf");
            ALOGV("get_next_buffer(): read_buf %p extended to %zu bytes",
                  in->read_buf, size_in_bytes);
        }

        in->read_status = pcm_read(in->_pcm, (void *)in->read_buf, size_in_bytes);

        if (in->read_status != 0) {
            ALOGE("get_next_buffer() pcm_read error %d", in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }
        in->read_buf_frames = in->config.period_size;
    }

    buffer->frame_count = (buffer->frame_count > in->read_buf_frames) ?
                          in->read_buf_frames : buffer->frame_count;
    buffer->i16 = in->read_buf + (in->config.period_size - in->read_buf_frames) *
                  in->config.channels;

    return in->read_status;

}

echo_reference_itfe *StreamIn::getEchoReference(sp<Device> dev,
        audio_format_t /*format*/,
        uint32_t channel_count,
        uint32_t sampling_rate) {
    putEchoReference(dev, dev->echo_reference);
    /* echo reference is taken from the low latency output stream used
     * for voice use cases */
    if (dev->outputs[OUTPUT_LOW_LATENCY] != NULL &&
            !(static_cast<StreamOut *>(dev->outputs[OUTPUT_LOW_LATENCY])->_standby)) {
        StreamOut *stream =
            static_cast<StreamOut *>(dev->outputs[OUTPUT_LOW_LATENCY]);
        uint32_t wr_channel_count = popcount(stream->channel_mask);
        uint32_t wr_sampling_rate = stream->getSampleRate();

        int status = create_echo_reference(AUDIO_FORMAT_PCM_16_BIT,
                                           channel_count,
                                           sampling_rate,
                                           AUDIO_FORMAT_PCM_16_BIT,
                                           wr_channel_count,
                                           wr_sampling_rate,
                                           &dev->echo_reference);
        if (status == 0)
            addEchoReference(dev->outputs[OUTPUT_LOW_LATENCY],
                             dev->echo_reference);
    }
    return dev->echo_reference;
}

void StreamIn::addEchoReference(IStreamOut *out,
                                echo_reference_itfe *reference) {
    StreamOut *stream = static_cast<StreamOut *>(out);
    PTHREAD_MUTEX_LOCK(&stream->lock);
    static_cast<StreamOut *>(stream)->echo_reference = reference;
    PTHREAD_MUTEX_UNLOCK(&stream->lock);
}

/* must be called with hw device and input stream mutexes locked */
int StreamIn::startInputStream() {
    int ret = 0;

    mDevice->active_input = this;

    if (mDevice->mode != AUDIO_MODE_IN_CALL) {
        mDevice->devices &= ~AUDIO_DEVICE_IN_ALL;
        mDevice->devices |= this->device;
    }

    if (this->aux_channels_changed) {
        this->aux_channels_changed = false;
        this->config.channels = popcount(this->main_channels | this->aux_channels);

        if (this->resampler) {
            /* release and recreate the resampler with the new number of channel of the input */
            release_resampler(this->resampler);
            this->resampler = NULL;
            ret = create_resampler(this->config.rate,
                                   this->requested_rate,
                                   this->config.channels,
                                   RESAMPLER_QUALITY_DEFAULT,
                                   &this->buf_provider,
                                   &this->resampler);
        }
        ALOGV("start_input_stream(): New channel configuration, "
              "main_channels = [%04x], aux_channels = [%04x], config.channels = %d",
              this->main_channels, this->aux_channels, this->config.channels);
    }

    if (this->need_echo_reference && this->echo_reference == NULL)
        this->echo_reference = getEchoReference(mDevice,
                                                AUDIO_FORMAT_PCM_16_BIT,
                                                this->config.channels,
                                                this->requested_rate);

    /* this assumes routing is done previously */
    this->_pcm = pcm_open(0, PORT_MIC, PCM_IN, &this->config);
    if (!pcm_is_ready(this->_pcm)) {
        ALOGE("cannot open pcm_in driver: %s", pcm_get_error(this->_pcm));
        pcm_close(this->_pcm);
        mDevice->active_input = NULL;
        return -ENOMEM;
    }

    /* force read and proc buf reallocation case of frame size or channel count change */
    this->read_buf_frames = 0;
    this->read_buf_size = 0;
    this->proc_buf_frames = 0;
    this->proc_buf_size = 0;
    /* if no supported sample rate is available, use the resampler */
    if (this->resampler) {
        this->resampler->reset(this->resampler);
    }
    return 0;
}

void StreamIn::pushEchoReference(StreamIn *in, size_t frames) {
    /* read frames from echo reference buffer and update echo delay
     * in->ref_buf_frames is updated with frames available in in->ref_buf */
    int32_t delay_us = updateEchoReference(in, frames) / 1000;
    int i;
    audio_buffer_t buf;

    if (in->ref_buf_frames < frames)
        frames = in->ref_buf_frames;

    buf.frameCount = frames;
    buf.raw = in->ref_buf;

    for (i = 0; i < in->num_preprocessors; i++) {
        if ((*in->preprocessors[i])->process_reverse == NULL)
            continue;

        (*in->preprocessors[i])->process_reverse(in->preprocessors[i],
                &buf,
                NULL);
        setPreprocessorEchoDelay(in->preprocessors[i], delay_us);
    }

    in->ref_buf_frames -= buf.frameCount;
    if (in->ref_buf_frames) {
        memcpy(in->ref_buf,
               in->ref_buf + buf.frameCount * in->config.channels,
               in->ref_buf_frames * in->config.channels * sizeof(int16_t));
    }
}

int32_t StreamIn::updateEchoReference(StreamIn *in, size_t frames) {
    echo_reference_buffer b;
    b.delay_ns = 0;

    ALOGV("update_echo_reference, frames = [%zu], in->ref_buf_frames = [%zu],  "
          "b.frame_count = [%zu]",
          frames, in->ref_buf_frames, frames - in->ref_buf_frames);
    if (in->ref_buf_frames < frames) {
        if (in->ref_buf_size < frames) {
            in->ref_buf_size = frames;
            in->ref_buf = (int16_t *)realloc(in->ref_buf, pcm_frames_to_bytes(in->_pcm,
                                             frames));
            ALOG_ASSERT((in->ref_buf != NULL),
                        "update_echo_reference() failed to reallocate ref_buf");
            ALOGV("update_echo_reference(): ref_buf %p extended to %d bytes",
                  in->ref_buf, pcm_frames_to_bytes(in->_pcm, frames));
        }
        b.frame_count = frames - in->ref_buf_frames;
        b.raw = (void *)(in->ref_buf + in->ref_buf_frames * in->config.channels);

        getCaptureDelay(in, frames, &b);

        if (in->echo_reference->read(in->echo_reference, &b) == 0) {
            in->ref_buf_frames += b.frame_count;
            ALOGV("update_echo_reference(): in->ref_buf_frames:[%zu], "
                  "in->ref_buf_size:[%zu], frames:[%zu], b.frame_count:[%zu]",
                  in->ref_buf_frames, in->ref_buf_size, frames, b.frame_count);
        }
    } else
        ALOGW("update_echo_reference(): NOT enough frames to read ref buffer");
    return b.delay_ns;
}

void StreamIn::getCaptureDelay(StreamIn *in,
                               size_t frames,
                               echo_reference_buffer *buffer) {
    /* read frames available in kernel driver buffer */
    unsigned int kernel_frames;
    timespec tstamp;
    long buf_delay;
    long rsmp_delay;
    long kernel_delay;
    long delay_ns;

    if (pcm_get_htimestamp(in->_pcm, &kernel_frames, &tstamp) < 0) {
        buffer->time_stamp.tv_sec  = 0;
        buffer->time_stamp.tv_nsec = 0;
        buffer->delay_ns           = 0;
        ALOGW("read get_capture_delay(): pcm_htimestamp error");
        return;
    }

    /* read frames available in audio HAL input buffer
     * add number of frames being read as we want the capture time of first sample
     * in current buffer */
    /* frames in in->buffer are at driver sampling rate while frames in in->proc_buf are
     * at requested sampling rate */
    buf_delay = (long)(((int64_t)(in->read_buf_frames) * 1000000000) /
                       in->config.rate +
                       ((int64_t)(in->proc_buf_frames) * 1000000000) /
                       in->requested_rate);

    /* add delay introduced by resampler */
    rsmp_delay = 0;
    if (in->resampler) {
        rsmp_delay = in->resampler->delay_ns(in->resampler);
    }

    kernel_delay = (long)(((int64_t)kernel_frames * 1000000000) / in->config.rate);

    delay_ns = kernel_delay + buf_delay + rsmp_delay;

    buffer->time_stamp = tstamp;
    buffer->delay_ns   = delay_ns;
    ALOGV("get_capture_delay time_stamp = [%ld].[%ld], delay_ns: [%d],"
          " kernel_delay:[%ld], buf_delay:[%ld], rsmp_delay:[%ld], kernel_frames:[%d], "
          "in->read_buf_frames:[%zu], in->proc_buf_frames:[%zu], frames:[%zu]",
          buffer->time_stamp.tv_sec , buffer->time_stamp.tv_nsec, buffer->delay_ns,
          kernel_delay, buf_delay, rsmp_delay, kernel_frames,
          in->read_buf_frames, in->proc_buf_frames, frames);

}

int StreamIn::setPreprocessorEchoDelay(effect_handle_t handle,
                                       int32_t delay_us) {
    uint32_t buf[sizeof(effect_param_t) / sizeof(uint32_t) + 2];
    effect_param_t *param = (effect_param_t *)buf;

    param->psize = sizeof(uint32_t);
    param->vsize = sizeof(uint32_t);
    *(uint32_t *)param->data = AEC_PARAM_ECHO_DELAY;
    *((int32_t *)param->data + 1) = delay_us;

    return setPreprocessorParam(handle, param);
}

int StreamIn::setPreprocessorParam(effect_handle_t handle,
                                   effect_param_t *param) {
    uint32_t size = sizeof(int);
    uint32_t psize = ((param->psize - 1) / sizeof(int) + 1) * sizeof(int) +
                     param->vsize;

    int status = (*handle)->command(handle,
                                    EFFECT_CMD_SET_PARAM,
                                    sizeof (effect_param_t) + psize,
                                    param,
                                    &size,
                                    &param->status);
    if (status == 0)
        status = param->status;

    return status;
}

int StreamIn::doInputStandby(StreamIn *in) {
    if (!in->_standby) {
        pcm_close(in->_pcm);
        in->_pcm = NULL;

        in->mDevice->active_input = 0;
        if (in->mDevice->mode != AUDIO_MODE_IN_CALL)
            in->mDevice->devices &= ~AUDIO_DEVICE_IN_ALL;

        if (in->echo_reference != NULL) {
            /* stop reading from echo reference */
            in->echo_reference->read(in->echo_reference, NULL);
            putEchoReference(in->mDevice, in->echo_reference);
            in->echo_reference = NULL;
        }

        in->_standby = 1;
    }
    return 0;
}

int StreamIn::getNextBuffer(struct resampler_buffer_provider *buffer_provider,
                            struct resampler_buffer *buffer) {
    StreamIn *in;

    if (buffer_provider == NULL || buffer == NULL)
        return -EINVAL;

    in = (StreamIn *)((char *)buffer_provider -
                      offsetof(StreamIn, buf_provider));

    if (in->_pcm == NULL) {
        buffer->raw = NULL;
        buffer->frame_count = 0;
        in->read_status = -ENODEV;
        return -ENODEV;
    }

    if (in->read_buf_frames == 0) {
        size_t size_in_bytes = pcm_frames_to_bytes(in->_pcm, in->config.period_size);
        if (in->read_buf_size < in->config.period_size) {
            in->read_buf_size = in->config.period_size;
            in->read_buf = (int16_t *) realloc(in->read_buf, size_in_bytes);
            ALOG_ASSERT((in->read_buf != NULL),
                        "get_next_buffer() failed to reallocate read_buf");
            ALOGV("get_next_buffer(): read_buf %p extended to %zu bytes",
                  in->read_buf, size_in_bytes);
        }

        in->read_status = pcm_read(in->_pcm, (void *)in->read_buf, size_in_bytes);

        if (in->read_status != 0) {
            ALOGE("get_next_buffer() pcm_read error %d", in->read_status);
            buffer->raw = NULL;
            buffer->frame_count = 0;
            return in->read_status;
        }
        in->read_buf_frames = in->config.period_size;
    }

    buffer->frame_count = (buffer->frame_count > in->read_buf_frames) ?
                          in->read_buf_frames : buffer->frame_count;
    buffer->i16 = in->read_buf + (in->config.period_size - in->read_buf_frames) *
                  in->config.channels;

    return in->read_status;

}

void StreamIn::releaseBuffer(struct resampler_buffer_provider *buffer_provider,
                             struct resampler_buffer *buffer) {
    StreamIn *in = NULL;
    if (buffer_provider == NULL || buffer == NULL)
        return;

    in = (StreamIn *)((char *)buffer_provider -
                      offsetof(StreamIn, buf_provider));

    if (in != NULL)
        in->read_buf_frames -= buffer->frame_count;
}

}  // namespace salvator
}  // namespace V2_0
}  // namespace audio
}  // namespace hardware
}  // namespace android
