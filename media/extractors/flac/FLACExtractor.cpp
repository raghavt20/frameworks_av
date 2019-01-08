/*
 * Copyright (C) 2011 The Android Open Source Project
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
#define LOG_TAG "FLACExtractor"
#include <utils/Log.h>

#include <stdint.h>

#include "FLACExtractor.h"
// libFLAC parser
#include "FLAC/stream_decoder.h"

#include <android/binder_ibinder.h> // for AIBinder_getCallingUid
#include <audio_utils/primitives.h>
#include <media/MediaExtractorPluginApi.h>
#include <media/NdkMediaFormat.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/base64.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MetaDataUtils.h>
#include <media/stagefright/MediaBufferBase.h>
#include <private/android_filesystem_config.h> // for AID_MEDIA
#include <system/audio.h>

namespace android {

// MediaServer is capable of handling float extractor output, but general processes
// may not be able to do so.
// TODO: Improve API to set extractor float output.
// (Note: duplicated with WAVExtractor.cpp)
static inline bool shouldExtractorOutputFloat(int bitsPerSample)
{
    return bitsPerSample > 16 && AIBinder_getCallingUid() == AID_MEDIA;
}

class FLACParser;

class FLACSource : public MediaTrackHelperV2 {

public:
    FLACSource(
            DataSourceHelper *dataSource,
            AMediaFormat *meta,
            bool outputFloat);

    virtual media_status_t start();
    virtual media_status_t stop();
    virtual media_status_t getFormat(AMediaFormat *meta);

    virtual media_status_t read(
            MediaBufferBase **buffer, const ReadOptions *options = NULL);

protected:
    virtual ~FLACSource();

private:
    DataSourceHelper *mDataSource;
    AMediaFormat *mTrackMetadata;
    const bool mOutputFloat;
    FLACParser *mParser;
    bool mInitCheck;
    bool mStarted;

    // no copy constructor or assignment
    FLACSource(const FLACSource &);
    FLACSource &operator=(const FLACSource &);

};

// FLACParser wraps a C libFLAC parser aka stream decoder

class FLACParser {

public:
    enum {
        kMaxChannels = FCC_8,
    };

    explicit FLACParser(
        DataSourceHelper *dataSource,
        bool outputFloat,
        // If metadata pointers aren't provided, we don't fill them
        AMediaFormat *fileMetadata = 0,
        AMediaFormat *trackMetadata = 0);

    virtual ~FLACParser();

    status_t initCheck() const {
        return mInitCheck;
    }

    // stream properties
    unsigned getMaxBlockSize() const {
        return mStreamInfo.max_blocksize;
    }
    unsigned getSampleRate() const {
        return mStreamInfo.sample_rate;
    }
    unsigned getChannels() const {
        return mStreamInfo.channels;
    }
    unsigned getBitsPerSample() const {
        return mStreamInfo.bits_per_sample;
    }
    FLAC__uint64 getTotalSamples() const {
        return mStreamInfo.total_samples;
    }

    // media buffers
    void allocateBuffers();
    void releaseBuffers();
    MediaBufferBase *readBuffer() {
        return readBuffer(false, 0LL);
    }
    MediaBufferBase *readBuffer(FLAC__uint64 sample) {
        return readBuffer(true, sample);
    }

private:
    DataSourceHelper *mDataSource;
    const bool mOutputFloat;
    AMediaFormat *mFileMetadata;
    AMediaFormat *mTrackMetadata;
    bool mInitCheck;

    // media buffers
    size_t mMaxBufferSize;
    MediaBufferGroup *mGroup;
    void (*mCopy)(int16_t *dst, const int * src[kMaxChannels], unsigned nSamples, unsigned nChannels);

    // handle to underlying libFLAC parser
    FLAC__StreamDecoder *mDecoder;

    // current position within the data source
    off64_t mCurrentPos;
    bool mEOF;

    // cached when the STREAMINFO metadata is parsed by libFLAC
    FLAC__StreamMetadata_StreamInfo mStreamInfo;
    bool mStreamInfoValid;

    // cached when a decoded PCM block is "written" by libFLAC parser
    bool mWriteRequested;
    bool mWriteCompleted;
    FLAC__FrameHeader mWriteHeader;
    FLAC__int32 const * mWriteBuffer[kMaxChannels];

    // most recent error reported by libFLAC parser
    FLAC__StreamDecoderErrorStatus mErrorStatus;

    status_t init();
    MediaBufferBase *readBuffer(bool doSeek, FLAC__uint64 sample);

    // no copy constructor or assignment
    FLACParser(const FLACParser &);
    FLACParser &operator=(const FLACParser &);

    // FLAC parser callbacks as C++ instance methods
    FLAC__StreamDecoderReadStatus readCallback(
            FLAC__byte buffer[], size_t *bytes);
    FLAC__StreamDecoderSeekStatus seekCallback(
            FLAC__uint64 absolute_byte_offset);
    FLAC__StreamDecoderTellStatus tellCallback(
            FLAC__uint64 *absolute_byte_offset);
    FLAC__StreamDecoderLengthStatus lengthCallback(
            FLAC__uint64 *stream_length);
    FLAC__bool eofCallback();
    FLAC__StreamDecoderWriteStatus writeCallback(
            const FLAC__Frame *frame, const FLAC__int32 * const buffer[]);
    void metadataCallback(const FLAC__StreamMetadata *metadata);
    void errorCallback(FLAC__StreamDecoderErrorStatus status);
    size_t getOutputSampleSize() const { return mOutputFloat ? sizeof(float) : sizeof(int16_t); }

    // FLAC parser callbacks as C-callable functions
    static FLAC__StreamDecoderReadStatus read_callback(
            const FLAC__StreamDecoder *decoder,
            FLAC__byte buffer[], size_t *bytes,
            void *client_data);
    static FLAC__StreamDecoderSeekStatus seek_callback(
            const FLAC__StreamDecoder *decoder,
            FLAC__uint64 absolute_byte_offset,
            void *client_data);
    static FLAC__StreamDecoderTellStatus tell_callback(
            const FLAC__StreamDecoder *decoder,
            FLAC__uint64 *absolute_byte_offset,
            void *client_data);
    static FLAC__StreamDecoderLengthStatus length_callback(
            const FLAC__StreamDecoder *decoder,
            FLAC__uint64 *stream_length,
            void *client_data);
    static FLAC__bool eof_callback(
            const FLAC__StreamDecoder *decoder,
            void *client_data);
    static FLAC__StreamDecoderWriteStatus write_callback(
            const FLAC__StreamDecoder *decoder,
            const FLAC__Frame *frame, const FLAC__int32 * const buffer[],
            void *client_data);
    static void metadata_callback(
            const FLAC__StreamDecoder *decoder,
            const FLAC__StreamMetadata *metadata,
            void *client_data);
    static void error_callback(
            const FLAC__StreamDecoder *decoder,
            FLAC__StreamDecoderErrorStatus status,
            void *client_data);

};

// The FLAC parser calls our C++ static callbacks using C calling conventions,
// inside FLAC__stream_decoder_process_until_end_of_metadata
// and FLAC__stream_decoder_process_single.
// We immediately then call our corresponding C++ instance methods
// with the same parameter list, but discard redundant information.

FLAC__StreamDecoderReadStatus FLACParser::read_callback(
        const FLAC__StreamDecoder * /* decoder */, FLAC__byte buffer[],
        size_t *bytes, void *client_data)
{
    return ((FLACParser *) client_data)->readCallback(buffer, bytes);
}

FLAC__StreamDecoderSeekStatus FLACParser::seek_callback(
        const FLAC__StreamDecoder * /* decoder */,
        FLAC__uint64 absolute_byte_offset, void *client_data)
{
    return ((FLACParser *) client_data)->seekCallback(absolute_byte_offset);
}

FLAC__StreamDecoderTellStatus FLACParser::tell_callback(
        const FLAC__StreamDecoder * /* decoder */,
        FLAC__uint64 *absolute_byte_offset, void *client_data)
{
    return ((FLACParser *) client_data)->tellCallback(absolute_byte_offset);
}

FLAC__StreamDecoderLengthStatus FLACParser::length_callback(
        const FLAC__StreamDecoder * /* decoder */,
        FLAC__uint64 *stream_length, void *client_data)
{
    return ((FLACParser *) client_data)->lengthCallback(stream_length);
}

FLAC__bool FLACParser::eof_callback(
        const FLAC__StreamDecoder * /* decoder */, void *client_data)
{
    return ((FLACParser *) client_data)->eofCallback();
}

FLAC__StreamDecoderWriteStatus FLACParser::write_callback(
        const FLAC__StreamDecoder * /* decoder */, const FLAC__Frame *frame,
        const FLAC__int32 * const buffer[], void *client_data)
{
    return ((FLACParser *) client_data)->writeCallback(frame, buffer);
}

void FLACParser::metadata_callback(
        const FLAC__StreamDecoder * /* decoder */,
        const FLAC__StreamMetadata *metadata, void *client_data)
{
    ((FLACParser *) client_data)->metadataCallback(metadata);
}

void FLACParser::error_callback(
        const FLAC__StreamDecoder * /* decoder */,
        FLAC__StreamDecoderErrorStatus status, void *client_data)
{
    ((FLACParser *) client_data)->errorCallback(status);
}

// These are the corresponding callbacks with C++ calling conventions

FLAC__StreamDecoderReadStatus FLACParser::readCallback(
        FLAC__byte buffer[], size_t *bytes)
{
    size_t requested = *bytes;
    ssize_t actual = mDataSource->readAt(mCurrentPos, buffer, requested);
    if (0 > actual) {
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    } else if (0 == actual) {
        *bytes = 0;
        mEOF = true;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    } else {
        assert(actual <= requested);
        *bytes = actual;
        mCurrentPos += actual;
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
}

FLAC__StreamDecoderSeekStatus FLACParser::seekCallback(
        FLAC__uint64 absolute_byte_offset)
{
    mCurrentPos = absolute_byte_offset;
    mEOF = false;
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

FLAC__StreamDecoderTellStatus FLACParser::tellCallback(
        FLAC__uint64 *absolute_byte_offset)
{
    *absolute_byte_offset = mCurrentPos;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus FLACParser::lengthCallback(
        FLAC__uint64 *stream_length)
{
    off64_t size;
    if (OK == mDataSource->getSize(&size)) {
        *stream_length = size;
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
    } else {
        return FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;
    }
}

FLAC__bool FLACParser::eofCallback()
{
    return mEOF;
}

FLAC__StreamDecoderWriteStatus FLACParser::writeCallback(
        const FLAC__Frame *frame, const FLAC__int32 * const buffer[])
{
    if (mWriteRequested) {
        mWriteRequested = false;
        // FLAC parser doesn't free or realloc buffer until next frame or finish
        mWriteHeader = frame->header;
        memmove(mWriteBuffer, buffer, sizeof(const FLAC__int32 * const) * getChannels());
        mWriteCompleted = true;
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    } else {
        ALOGE("FLACParser::writeCallback unexpected");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
}

void FLACParser::metadataCallback(const FLAC__StreamMetadata *metadata)
{
    switch (metadata->type) {
    case FLAC__METADATA_TYPE_STREAMINFO:
        if (!mStreamInfoValid) {
            mStreamInfo = metadata->data.stream_info;
            mStreamInfoValid = true;
        } else {
            ALOGE("FLACParser::metadataCallback unexpected STREAMINFO");
        }
        break;
    case FLAC__METADATA_TYPE_VORBIS_COMMENT:
        {
        const FLAC__StreamMetadata_VorbisComment *vc;
        vc = &metadata->data.vorbis_comment;
        for (FLAC__uint32 i = 0; i < vc->num_comments; ++i) {
            FLAC__StreamMetadata_VorbisComment_Entry *vce;
            vce = &vc->comments[i];
            if (mFileMetadata != 0 && vce->entry != NULL) {
                parseVorbisComment(mFileMetadata, (const char *) vce->entry,
                        vce->length);
            }
        }
        }
        break;
    case FLAC__METADATA_TYPE_PICTURE:
        if (mFileMetadata != 0) {
            const FLAC__StreamMetadata_Picture *p = &metadata->data.picture;
            AMediaFormat_setBuffer(mFileMetadata, AMEDIAFORMAT_KEY_ALBUMART,
                   p->data, p->data_length);
        }
        break;
    default:
        ALOGW("FLACParser::metadataCallback unexpected type %u", metadata->type);
        break;
    }
}

void FLACParser::errorCallback(FLAC__StreamDecoderErrorStatus status)
{
    ALOGE("FLACParser::errorCallback status=%d", status);
    mErrorStatus = status;
}

// Copy samples from FLAC native 32-bit non-interleaved to 16-bit signed
// or 32-bit float interleaved.
// These are candidates for optimization if needed.
static void copyTo16Signed(
        short *dst,
        const int *const *src,
        unsigned nSamples,
        unsigned nChannels,
        unsigned bitsPerSample) {
    const unsigned leftShift = 16 - bitsPerSample;
    for (unsigned i = 0; i < nSamples; ++i) {
        for (unsigned c = 0; c < nChannels; ++c) {
            *dst++ = src[c][i] << leftShift;
        }
    }
}

static void copyToFloat(
        float *dst,
        const int *const *src,
        unsigned nSamples,
        unsigned nChannels,
        unsigned bitsPerSample) {
    const unsigned leftShift = 32 - bitsPerSample;
    for (unsigned i = 0; i < nSamples; ++i) {
        for (unsigned c = 0; c < nChannels; ++c) {
            *dst++ = float_from_i32(src[c][i] << leftShift);
        }
    }
}

// FLACParser

FLACParser::FLACParser(
        DataSourceHelper *dataSource,
        bool outputFloat,
        AMediaFormat *fileMetadata,
        AMediaFormat *trackMetadata)
    : mDataSource(dataSource),
      mOutputFloat(outputFloat),
      mFileMetadata(fileMetadata),
      mTrackMetadata(trackMetadata),
      mInitCheck(false),
      mMaxBufferSize(0),
      mGroup(NULL),
      mDecoder(NULL),
      mCurrentPos(0LL),
      mEOF(false),
      mStreamInfoValid(false),
      mWriteRequested(false),
      mWriteCompleted(false),
      mErrorStatus((FLAC__StreamDecoderErrorStatus) -1)
{
    ALOGV("FLACParser::FLACParser");
    memset(&mStreamInfo, 0, sizeof(mStreamInfo));
    memset(&mWriteHeader, 0, sizeof(mWriteHeader));
    mInitCheck = init();
}

FLACParser::~FLACParser()
{
    ALOGV("FLACParser::~FLACParser");
    if (mDecoder != NULL) {
        FLAC__stream_decoder_delete(mDecoder);
        mDecoder = NULL;
    }
}

status_t FLACParser::init()
{
    // setup libFLAC parser
    mDecoder = FLAC__stream_decoder_new();
    if (mDecoder == NULL) {
        // The new should succeed, since probably all it does is a malloc
        // that always succeeds in Android.  But to avoid dependence on the
        // libFLAC internals, we check and log here.
        ALOGE("new failed");
        return NO_INIT;
    }
    FLAC__stream_decoder_set_md5_checking(mDecoder, false);
    FLAC__stream_decoder_set_metadata_ignore_all(mDecoder);
    FLAC__stream_decoder_set_metadata_respond(
            mDecoder, FLAC__METADATA_TYPE_STREAMINFO);
    FLAC__stream_decoder_set_metadata_respond(
            mDecoder, FLAC__METADATA_TYPE_PICTURE);
    FLAC__stream_decoder_set_metadata_respond(
            mDecoder, FLAC__METADATA_TYPE_VORBIS_COMMENT);
    FLAC__StreamDecoderInitStatus initStatus;
    initStatus = FLAC__stream_decoder_init_stream(
            mDecoder,
            read_callback, seek_callback, tell_callback,
            length_callback, eof_callback, write_callback,
            metadata_callback, error_callback, (void *) this);
    if (initStatus != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        // A failure here probably indicates a programming error and so is
        // unlikely to happen. But we check and log here similarly to above.
        ALOGE("init_stream failed %d", initStatus);
        return NO_INIT;
    }
    // parse all metadata
    if (!FLAC__stream_decoder_process_until_end_of_metadata(mDecoder)) {
        ALOGE("end_of_metadata failed");
        return NO_INIT;
    }
    if (mStreamInfoValid) {
        // check channel count
        if (getChannels() == 0 || getChannels() > kMaxChannels) {
            ALOGE("unsupported channel count %u", getChannels());
            return NO_INIT;
        }
        // check bit depth
        switch (getBitsPerSample()) {
        case 8:
        case 16:
        case 24:
            break;
        default:
            ALOGE("unsupported bits per sample %u", getBitsPerSample());
            return NO_INIT;
        }
        // check sample rate
        switch (getSampleRate()) {
        case  8000:
        case 11025:
        case 12000:
        case 16000:
        case 22050:
        case 24000:
        case 32000:
        case 44100:
        case 48000:
        case 88200:
        case 96000:
            break;
        default:
            ALOGE("unsupported sample rate %u", getSampleRate());
            return NO_INIT;
        }
        // populate track metadata
        if (mTrackMetadata != 0) {
            AMediaFormat_setString(mTrackMetadata,
                    AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_RAW);
            AMediaFormat_setInt32(mTrackMetadata,
                    AMEDIAFORMAT_KEY_CHANNEL_COUNT, getChannels());
            AMediaFormat_setInt32(mTrackMetadata,
                    AMEDIAFORMAT_KEY_SAMPLE_RATE, getSampleRate());
            AMediaFormat_setInt32(mTrackMetadata,
                    AMEDIAFORMAT_KEY_BITS_PER_SAMPLE, getBitsPerSample());
            // sample rate is non-zero, so division by zero not possible
            AMediaFormat_setInt64(mTrackMetadata,
                    AMEDIAFORMAT_KEY_DURATION, (getTotalSamples() * 1000000LL) / getSampleRate());
        }
    } else {
        ALOGE("missing STREAMINFO");
        return NO_INIT;
    }
    if (mFileMetadata != 0) {
        AMediaFormat_setString(mFileMetadata,
                AMEDIAFORMAT_KEY_MIME, MEDIA_MIMETYPE_AUDIO_FLAC);
    }
    return OK;
}

void FLACParser::allocateBuffers()
{
    CHECK(mGroup == NULL);
    mGroup = new MediaBufferGroup;
    mMaxBufferSize = getMaxBlockSize() * getChannels() * getOutputSampleSize();
    mGroup->add_buffer(MediaBufferBase::Create(mMaxBufferSize));
}

void FLACParser::releaseBuffers()
{
    CHECK(mGroup != NULL);
    delete mGroup;
    mGroup = NULL;
}

MediaBufferBase *FLACParser::readBuffer(bool doSeek, FLAC__uint64 sample)
{
    mWriteRequested = true;
    mWriteCompleted = false;
    if (doSeek) {
        // We implement the seek callback, so this works without explicit flush
        if (!FLAC__stream_decoder_seek_absolute(mDecoder, sample)) {
            ALOGE("FLACParser::readBuffer seek to sample %lld failed", (long long)sample);
            return NULL;
        }
        ALOGV("FLACParser::readBuffer seek to sample %lld succeeded", (long long)sample);
    } else {
        if (!FLAC__stream_decoder_process_single(mDecoder)) {
            ALOGE("FLACParser::readBuffer process_single failed");
            return NULL;
        }
    }
    if (!mWriteCompleted) {
        ALOGV("FLACParser::readBuffer write did not complete");
        return NULL;
    }
    // verify that block header keeps the promises made by STREAMINFO
    unsigned blocksize = mWriteHeader.blocksize;
    if (blocksize == 0 || blocksize > getMaxBlockSize()) {
        ALOGE("FLACParser::readBuffer write invalid blocksize %u", blocksize);
        return NULL;
    }
    if (mWriteHeader.sample_rate != getSampleRate() ||
        mWriteHeader.channels != getChannels() ||
        mWriteHeader.bits_per_sample != getBitsPerSample()) {
        ALOGE("FLACParser::readBuffer write changed parameters mid-stream: %d/%d/%d -> %d/%d/%d",
                getSampleRate(), getChannels(), getBitsPerSample(),
                mWriteHeader.sample_rate, mWriteHeader.channels, mWriteHeader.bits_per_sample);
        return NULL;
    }
    // acquire a media buffer
    CHECK(mGroup != NULL);
    MediaBufferBase *buffer;
    status_t err = mGroup->acquire_buffer(&buffer);
    if (err != OK) {
        return NULL;
    }
    const size_t bufferSize = blocksize * getChannels() * getOutputSampleSize();
    CHECK(bufferSize <= mMaxBufferSize);
    buffer->set_range(0, bufferSize);
    // copy PCM from FLAC write buffer to our media buffer, with interleaving
    const unsigned bitsPerSample = getBitsPerSample();
    if (mOutputFloat) {
        copyToFloat(reinterpret_cast<float*>(buffer->data()),
                    mWriteBuffer,
                    blocksize,
                    getChannels(),
                    bitsPerSample);
    } else {
        copyTo16Signed(reinterpret_cast<short*>(buffer->data()),
                       mWriteBuffer,
                       blocksize,
                       getChannels(),
                       bitsPerSample);
    }
    // fill in buffer metadata
    CHECK(mWriteHeader.number_type == FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER);
    FLAC__uint64 sampleNumber = mWriteHeader.number.sample_number;
    int64_t timeUs = (1000000LL * sampleNumber) / getSampleRate();
    buffer->meta_data().setInt64(kKeyTime, timeUs);
    buffer->meta_data().setInt32(kKeyIsSyncFrame, 1);
    return buffer;
}

// FLACsource

FLACSource::FLACSource(
        DataSourceHelper *dataSource,
        AMediaFormat *trackMetadata,
        bool outputFloat)
    : mDataSource(dataSource),
      mTrackMetadata(trackMetadata),
      mOutputFloat(outputFloat),
      mParser(new FLACParser(mDataSource, outputFloat)),
      mInitCheck(mParser->initCheck()),
      mStarted(false)
{
    ALOGV("FLACSource::FLACSource");
}

FLACSource::~FLACSource()
{
    ALOGV("~FLACSource::FLACSource");
    if (mStarted) {
        stop();
    }
    delete mParser;
}

media_status_t FLACSource::start()
{
    ALOGV("FLACSource::start");

    CHECK(!mStarted);
    mParser->allocateBuffers();
    mStarted = true;

    return AMEDIA_OK;
}

media_status_t FLACSource::stop()
{
    ALOGV("FLACSource::stop");

    CHECK(mStarted);
    mParser->releaseBuffers();
    mStarted = false;

    return AMEDIA_OK;
}

media_status_t FLACSource::getFormat(AMediaFormat *meta)
{
    const media_status_t status = AMediaFormat_copy(meta, mTrackMetadata);
    if (status == OK) {
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_PCM_ENCODING,
                mOutputFloat ? kAudioEncodingPcmFloat : kAudioEncodingPcm16bit);
    }
    return status;
}

media_status_t FLACSource::read(
        MediaBufferBase **outBuffer, const ReadOptions *options)
{
    MediaBufferBase *buffer;
    // process an optional seek request
    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;
    if ((NULL != options) && options->getSeekTo(&seekTimeUs, &mode)) {
        FLAC__uint64 sample;
        if (seekTimeUs <= 0LL) {
            sample = 0LL;
        } else {
            // sample and total samples are both zero-based, and seek to EOF ok
            sample = (seekTimeUs * mParser->getSampleRate()) / 1000000LL;
            if (sample >= mParser->getTotalSamples()) {
                sample = mParser->getTotalSamples();
            }
        }
        buffer = mParser->readBuffer(sample);
    // otherwise read sequentially
    } else {
        buffer = mParser->readBuffer();
    }
    *outBuffer = buffer;
    return buffer != NULL ? AMEDIA_OK : AMEDIA_ERROR_END_OF_STREAM;
}

// FLACExtractor

FLACExtractor::FLACExtractor(
        DataSourceHelper *dataSource)
    : mDataSource(dataSource),
      mParser(nullptr),
      mInitCheck(false)
{
    ALOGV("FLACExtractor::FLACExtractor");
    // FLACParser will fill in the metadata for us
    mFileMetadata = AMediaFormat_new();
    mTrackMetadata = AMediaFormat_new();
    mParser = new FLACParser(mDataSource, false /* outputFloat */, mFileMetadata, mTrackMetadata);
    mInitCheck = mParser->initCheck();
}

FLACExtractor::~FLACExtractor()
{
    ALOGV("~FLACExtractor::FLACExtractor");
    delete mParser;
    delete mDataSource;
    AMediaFormat_delete(mFileMetadata);
    AMediaFormat_delete(mTrackMetadata);
}

size_t FLACExtractor::countTracks()
{
    return mInitCheck == OK ? 1 : 0;
}

MediaTrackHelperV2 *FLACExtractor::getTrack(size_t index)
{
    if (mInitCheck != OK || index > 0) {
        return NULL;
    }

    return new FLACSource(
            mDataSource, mTrackMetadata, shouldExtractorOutputFloat(mParser->getBitsPerSample()));
}

media_status_t FLACExtractor::getTrackMetaData(
        AMediaFormat *meta,
        size_t index, uint32_t /* flags */) {
    if (mInitCheck != OK || index > 0) {
        return AMEDIA_ERROR_UNKNOWN;
    }
    const media_status_t status = AMediaFormat_copy(meta, mTrackMetadata);
    if (status == OK) {
        AMediaFormat_setInt32(meta, AMEDIAFORMAT_KEY_PCM_ENCODING,
                shouldExtractorOutputFloat(mParser->getBitsPerSample())
                        ? kAudioEncodingPcmFloat : kAudioEncodingPcm16bit);
    }
    return status;
}

media_status_t FLACExtractor::getMetaData(AMediaFormat *meta)
{
    return AMediaFormat_copy(meta, mFileMetadata);
}

// Sniffer

bool SniffFLAC(DataSourceHelper *source, float *confidence)
{
    // first 4 is the signature word
    // second 4 is the sizeof STREAMINFO
    // 042 is the mandatory STREAMINFO
    // no need to read rest of the header, as a premature EOF will be caught later
    uint8_t header[4+4];
    if (source->readAt(0, header, sizeof(header)) != sizeof(header)
            || memcmp("fLaC\0\0\0\042", header, 4+4))
    {
        return false;
    }

    *confidence = 0.5;

    return true;
}


extern "C" {
// This is the only symbol that needs to be exported
__attribute__ ((visibility ("default")))
ExtractorDef GETEXTRACTORDEF() {
    return {
            EXTRACTORDEF_VERSION_CURRENT,
            UUID("1364b048-cc45-4fda-9934-327d0ebf9829"),
            1,
            "FLAC Extractor",
            {
                .v2 = [](
                        CDataSource *source,
                        float *confidence,
                        void **,
                        FreeMetaFunc *) -> CreatorFuncV2 {
                    DataSourceHelper helper(source);
                    if (SniffFLAC(&helper, confidence)) {
                        return [](
                                CDataSource *source,
                                void *) -> CMediaExtractorV2* {
                            return wrapV2(new FLACExtractor(new DataSourceHelper(source)));};
                    }
                    return NULL;
                }
            }
     };
}

} // extern "C"

}  // namespace android
