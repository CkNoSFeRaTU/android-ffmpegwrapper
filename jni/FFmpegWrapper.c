/*
 * Copyright (c) 2013, David Brodsky. All rights reserved.
 * Copyright (c) 2016, Konstantin Kuzov. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <jni.h>
#include <android/log.h>
#include <string.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"

#define LOG_TAG "FFmpegWrapper"
#define LOGI(...)  __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...)  __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// For debugging: Write RAW frames to separated files for inspection
//#define WRITE_RAW_FILES
//#define FFMPEG_LOGGING

#ifdef WRITE_RAW_FILES
const char *raw_audio_filename = "/mnt/sdcard/raw.aac";
const char *raw_video_filename = "/mnt/sdcard/raw.h264";
FILE *raw_audio = NULL;
FILE *raw_video = NULL;
#endif

// Output
const char *outputPath;
const char *outputFormatName = "flv";
int hlsSegmentDurationSec = 10;
int audioStreamIndex = -1;
int videoStreamIndex = -1;

// Video
int VIDEO_PIX_FMT = PIX_FMT_YUV420P;
int VIDEO_CODEC_ID = CODEC_ID_H264;
int VIDEO_WIDTH = 1280;
int VIDEO_HEIGHT = 720;

// Audio
int AUDIO_CODEC_ID = CODEC_ID_AAC;
int AUDIO_SAMPLE_FMT = AV_SAMPLE_FMT_S16;
int AUDIO_SAMPLE_RATE = 44100;
int AUDIO_CHANNELS = 1;

int64_t VIDEO_FIRST_PTS = AV_NOPTS_VALUE;
int64_t AUDIO_FIRST_PTS = AV_NOPTS_VALUE;

AVFormatContext *outputFormatContext = NULL;

int IS_RUNNING = 0;

// FFmpeg Utilities
#ifdef FFMPEG_LOGGING
void log_callback(void *ptr, int level, const char *fmt, va_list vl)
{
    __android_log_vprint(ANDROID_LOG_DEBUG, LOG_TAG, fmt, vl);
}
#endif

void ffmpeg_init() {
    IS_RUNNING = 0;
    av_register_all();
    avformat_network_init();
    avcodec_register_all();

#ifdef FFMPEG_LOGGING
    av_log_set_level(AV_LOG_VERBOSE);
    av_log_set_callback(log_callback);
#endif

#ifdef WRITE_RAW_FILES
    raw_audio = fopen(raw_audio_file, "w");
    raw_video = fopen(raw_video_file, "w");
#endif
}

char* stringForAVErrorNumber(int errorNumber) {
    char *errorBuffer = malloc(sizeof(char) * AV_ERROR_MAX_STRING_SIZE);

    int strErrorResult = av_strerror(errorNumber, errorBuffer, AV_ERROR_MAX_STRING_SIZE);
    if (strErrorResult != 0) {
        LOGE("av_strerror error: %d", strErrorResult);
        return NULL;
    }

    return errorBuffer;
}

int addVideoStream(AVFormatContext *dest, uint8_t *extradata, size_t extradata_size) {
    AVCodecContext *c = NULL;
    AVStream *st = NULL;
    AVCodec *codec = NULL;

    /* find the video encoder.
     * If there are encoders isn't builtin than ffmpeg would return null
     * As we are passing H264 Annex B frames that's not a issue as encoding isn't needed
     * TODO: make possible using software encoder by request */
    codec = avcodec_find_encoder(VIDEO_CODEC_ID);
    if (!codec) {
        LOGI("add_video_stream codec not found");
    }

    st = avformat_new_stream(dest, codec);
    if (!st) {
        LOGE("add_video_stream could not alloc stream");
        return -1;
    }

    videoStreamIndex = st->index;
    LOGI("addVideoStream at index %d", videoStreamIndex);
    c = st->codec;

    avcodec_get_context_defaults3(c, codec);

    if (!codec) {
        c->codec_id = VIDEO_CODEC_ID;
        c->codec_type = AVMEDIA_TYPE_VIDEO;
    }

    if (extradata_size > 0) {
        LOGI("video extradata size: %d\n", (int)extradata_size);
        c->extradata_size = extradata_size;
        c->extradata = av_malloc(c->extradata_size);
        memcpy(c->extradata, extradata, c->extradata_size);
    }

    /* Resolution must be a multiple of two. */
    c->width = VIDEO_WIDTH;
    c->height = VIDEO_HEIGHT;

    c->pix_fmt = VIDEO_PIX_FMT;

    /* Some formats want stream headers to be separate. */
    if (dest->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    return 0;
}

int addAudioStream(AVFormatContext *formatContext, uint8_t *extradata, size_t extradata_size) {
    AVCodecContext *c = NULL;
    AVStream *st = NULL;
    AVCodec *codec = NULL;

    /* find the audio encoder
     * If there are encoders isn't builtin than ffmpeg would return null
     * As we are passing AAC LC frames that's not a issue as encoding isn't needed
     * TODO: make possible using software encoder by request */
    codec = avcodec_find_encoder(AUDIO_CODEC_ID);
    if (!codec) {
        LOGI("add_audio_stream codec not found");
    }

    st = avformat_new_stream(formatContext, codec);
    if (!st) {
        LOGE("add_audio_stream could not alloc stream");
        return -1;
    }

    audioStreamIndex = st->index;
    LOGI("addAudioStream at index %d", audioStreamIndex);

    c = st->codec;
    avcodec_get_context_defaults3(c, codec);

    if (!codec) {
        c->codec_id = AUDIO_CODEC_ID;
        c->codec_type = AVMEDIA_TYPE_AUDIO;
    }

    if (extradata_size > 0)
    {
        LOGI("audio extradata size: %d\n", (int)extradata_size);
        c->extradata_size = extradata_size;
        c->extradata = av_malloc(c->extradata_size);
        memcpy(c->extradata, extradata, c->extradata_size);
    }

    c->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL; // for native aac support

    c->sample_fmt = AUDIO_SAMPLE_FMT;
    c->sample_rate = AUDIO_SAMPLE_RATE;
    c->channels = AUDIO_CHANNELS;

    // some formats want stream headers to be separate
    if (formatContext->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    return 0;
}

int openFileForWriting(AVFormatContext *avfc, const char *path) {
    if (!(avfc->oformat->flags & AVFMT_NOFILE)) {
        LOGI("Opening output file for writing at path %s", path);
        return avio_open(&avfc->pb, path, AVIO_FLAG_WRITE);
    }

    return 0;
}

int writeFileHeader(AVFormatContext *avfc) {
    AVDictionary *dict = NULL;

    // Write header for output file

    int writeHeaderResult = avformat_write_header(avfc, &dict);
    if (writeHeaderResult < 0)
        LOGE("Error writing header: %s", stringForAVErrorNumber(writeHeaderResult));

    LOGI("Wrote file header");
    return writeHeaderResult;
}

int writeFileTrailer(AVFormatContext *avfc) {
#ifdef WRITE_RAW_FILES
    fclose(raw_audio);
    fclose(raw_video);
#endif

    return av_write_trailer(avfc);
}

  /////////////////////
  //  JNI FUNCTIONS  //
  /////////////////////

/*
 * Prepares an AVFormatContext for output.
 * Currently, the output format and codecs are hardcoded in this file.
 */
int Java_net_openwatch_ffmpegwrapper_FFmpegWrapper_prepareAVFormatContext(JNIEnv *env, jobject obj, jstring jOutputPath, jobject jVideoData, jint jVideoSize, jobject jAudioData, jint jAudioSize, jlong jMaxInterleaveDelta) {
    int result = 0;

    ffmpeg_init();

    outputPath = (*env)->GetStringUTFChars(env, jOutputPath, NULL);

    result = avformat_alloc_output_context2(&outputFormatContext, NULL, outputFormatName, outputPath);
    if (result < 0) {
        LOGE("failed to open output context");
        return result;
    }

    // Creating codecContext manually if extradata present
    if (jVideoSize > 0)
    {
        result = addVideoStream(outputFormatContext, (*env)->GetDirectBufferAddress(env, jVideoData), jVideoSize);
        if(result < 0)
            return result;
    }
    if (jAudioSize > 0)
    {
        result = addAudioStream(outputFormatContext, (*env)->GetDirectBufferAddress(env, jAudioData), jAudioSize);
        if(result < 0)
            return result;
    }

    if (strcmp(outputFormatName, "hls") == 0)
        av_opt_set_int(outputFormatContext->priv_data, "hls_time", hlsSegmentDurationSec, 0);

    outputFormatContext->max_interleave_delta = (uint64_t) jMaxInterleaveDelta;

    result = openFileForWriting(outputFormatContext, outputPath);
    if(result < 0) {
        LOGE("openFileForWriting error: %d", result);
        return result;
    }

    IS_RUNNING = 1;

    return writeFileHeader(outputFormatContext);
}

/*
 * Override default AV Options. Must be called before prepareAVFormatContext
 */
void Java_net_openwatch_ffmpegwrapper_FFmpegWrapper_setAVOptions(JNIEnv *env, jobject obj, jobject jOpts) {
    // 1: Get your Java object's "jclass"!
    jclass ClassAVOptions = (*env)->GetObjectClass(env, jOpts);

    // 2: Get Java object field ids using the jclasss and field name as **hardcoded** strings!
    jfieldID jVideoHeightId = (*env)->GetFieldID(env, ClassAVOptions, "videoHeight", "I");
    jfieldID jVideoWidthId = (*env)->GetFieldID(env, ClassAVOptions, "videoWidth", "I");

    jfieldID jAudioSampleRateId = (*env)->GetFieldID(env, ClassAVOptions, "audioSampleRate", "I");
    jfieldID jNumAudioChannelsId = (*env)->GetFieldID(env, ClassAVOptions, "numAudioChannels", "I");

    jfieldID jHlsSegmentDurationSec = (*env)->GetFieldID(env, ClassAVOptions, "hlsSegmentDurationSec", "I");
    jfieldID jOutputFormatName = (*env)->GetFieldID(env, ClassAVOptions, "outputFormatName", "Ljava/lang/String;");

    jstring jsOutputFormatName = (*env)->GetObjectField(env, jOpts, jOutputFormatName);


    // 3: Get the Java object field values with the field ids!
    VIDEO_HEIGHT = (*env)->GetIntField(env, jOpts, jVideoHeightId);
    VIDEO_WIDTH = (*env)->GetIntField(env, jOpts, jVideoWidthId);

    AUDIO_SAMPLE_RATE = (*env)->GetIntField(env, jOpts, jAudioSampleRateId);
    AUDIO_CHANNELS = (*env)->GetIntField(env, jOpts, jNumAudioChannelsId);

    hlsSegmentDurationSec = (*env)->GetIntField(env, jOpts, jHlsSegmentDurationSec);
    outputFormatName = (*env)->GetStringUTFChars(env, jsOutputFormatName, 0);
}

/*
 * Consruct an AVPacket from MediaCodec output and call
 * av_interleaved_write_frame with our AVFormatContext
 */
int Java_net_openwatch_ffmpegwrapper_FFmpegWrapper_writeAVPacketFromEncodedData(JNIEnv *env, jobject obj, jobject jData, jint jIsVideo, jint jSize, jint jFlags, jlong jPts, jint jIsInterleave) {
    AVPacket *packet = NULL;

    // Ignore data if avstream is not initialized
    if ((((int) jIsVideo) == JNI_TRUE && videoStreamIndex == -1) || (((int) jIsVideo) != JNI_TRUE && audioStreamIndex == -1))
        return;

    if(packet == NULL)
        packet = av_malloc(sizeof(AVPacket));

    // jData is a ByteBuffer managed by Android's MediaCodec.
    // Because the audo track of the resulting output mostly works, I'm inclined to rule out this data marshaling being an issue
    uint8_t *data = (*env)->GetDirectBufferAddress(env, jData);

#ifdef WRITE_RAW_FILES
    if (((int) jIsVideo) == JNI_TRUE && raw_video != NULL) {
        fwrite(data, sizeof(uint8_t), (int)jSize, raw_video);
    } else if (raw_audio != NULL) {
        fwrite(data, sizeof(uint8_t), (int)jSize, raw_audio);
    }
#endif

    av_init_packet(packet);

    int64_t curr_pts = (int64_t) jPts;

    if ( ((int) jIsVideo) == JNI_TRUE) {
        packet->stream_index = videoStreamIndex;
        if (VIDEO_FIRST_PTS == AV_NOPTS_VALUE && curr_pts == 0)
        {
            av_free_packet(packet);
            return;
        }
        else if (VIDEO_FIRST_PTS == AV_NOPTS_VALUE)
            VIDEO_FIRST_PTS = curr_pts;
        packet->pts = curr_pts - VIDEO_FIRST_PTS;
    } else {
        packet->stream_index = audioStreamIndex;
        curr_pts = curr_pts / AUDIO_CHANNELS;
        if (AUDIO_FIRST_PTS == AV_NOPTS_VALUE)
            AUDIO_FIRST_PTS = curr_pts;
        packet->pts = curr_pts - AUDIO_FIRST_PTS;
    }

    packet->size = (int) jSize;
    packet->data = data;
    packet->dts = AV_NOPTS_VALUE;

    if (((int) jIsVideo) == JNI_TRUE && jFlags & 1 > 0)
        packet->flags |= AV_PKT_FLAG_KEY;

    packet->pts = av_rescale_q(packet->pts, AV_TIME_BASE_Q, outputFormatContext->streams[packet->stream_index]->time_base);

    int writeFrameResult;

    if ( ((int) jIsInterleave) == JNI_TRUE)
        writeFrameResult = av_interleaved_write_frame(outputFormatContext, packet);
    else
        writeFrameResult = av_write_frame(outputFormatContext, packet);

    if(writeFrameResult < 0) {
        LOGE("av_interleaved_write_frame video: %d size: %d error: %s", ((int) jIsVideo), ((int) jSize), stringForAVErrorNumber(writeFrameResult));
    }

    av_free_packet(packet);

    return writeFrameResult;
}

/*
 * Finalize file. Basically a wrapper around av_write_trailer
 */
void Java_net_openwatch_ffmpegwrapper_FFmpegWrapper_finalizeAVFormatContext(JNIEnv *env, jobject obj){
    LOGI("finalizeAVFormatContext");
    if (IS_RUNNING)
    {
        int writeTrailerResult = writeFileTrailer(outputFormatContext);
        if(writeTrailerResult < 0) {
            LOGE("av_write_trailer error: %d", writeTrailerResult);
        }

        avio_flush(outputFormatContext->pb);
        avio_close(outputFormatContext->pb);
    }

    avformat_free_context(outputFormatContext);

    audioStreamIndex = -1;
    videoStreamIndex = -1;
    
    VIDEO_FIRST_PTS = AV_NOPTS_VALUE;
    AUDIO_FIRST_PTS = AV_NOPTS_VALUE;
    
    IS_RUNNING = 0;
}
