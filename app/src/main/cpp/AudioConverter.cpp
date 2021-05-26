//
// Created by ZhaoLinlin on 2021/5/18.
//

#include "AudioConverter.h"
//#include "share/pch.h"

extern "C" {
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
}

#include <android/log.h>
#include <string>
#include <pthread.h>
#include <mutex>
#include <utility>

extern AVCodec ff_android_hw_h264_encoder;
#define SCALE_FLAGS SWS_BICUBIC
#define STREAM_FRAME_RATE 25 /* 25 images/s */
//const char AudioConverter::TAG[] = PROJECTIZED( "AudioConverter" );
const char AudioConverter::TAG[] = "AudioConverter";
//static jmethodID progressMethod = nullptr;
class ProcessCallback {
public:
    virtual ~ProcessCallback() = default;
    virtual void onProgress(int progress) = 0;
 };


class JavaProgressCallback : public ProcessCallback {

public:
    static void init(JNIEnv *env, jclass clazz) {

        progressMethod = env->GetMethodID(clazz, "onProgress", "(I)V");
    }
    explicit JavaProgressCallback(JNIEnv* env, jobject javaObject):
            env(env) {
        this->javaObject = env->NewGlobalRef(javaObject);
    }


    ~JavaProgressCallback() override {
        env->DeleteGlobalRef(javaObject);
    }

    void onProgress(int progress) override {
        if (progress > 100)
            progress = 100;
        if (progress > lasProgress) {
            lasProgress = progress;
            env->CallVoidMethod(javaObject, progressMethod, progress);
        }
    }

public:
    JNIEnv* env;
    jobject javaObject;
    int lasProgress = 0;
public:
    static jmethodID progressMethod;
};

jmethodID JavaProgressCallback::progressMethod = nullptr;


class ConvertException : public std::exception {
public:
    explicit ConvertException(std::string  what): w(std::move(what)) {

    }
    explicit ConvertException(const char* what): w(what) {
    }

    const char * what() const noexcept override {
        return w.c_str();
    }

private:
    std::string w;
};

class InputStreamCallback {
public:
    virtual void onInit() = 0;
    virtual void onAudioStream(AVCodecContext* codecContext) = 0;
    virtual void onVideoStream(AVCodecContext* codecContext, AVStream* stream) = 0;
    virtual void onStart() = 0;
    virtual void onAudioFrame(AVFrame* frame) = 0;
    virtual void onVideoFrame(AVFrame* frame) = 0;
    virtual void onEnd() = 0;
};

class InputStream {

public:
    explicit InputStream(ProcessCallback* processCallback, InputStreamCallback* callback, const char *path) : processCallback(processCallback),callback(callback)
    , sourcePath(path)
    ,lockMutex() {
    }

    ~InputStream() {
        release();
    }

private:
    void release() {
        if (audio_dec_ctx != nullptr) {
            avcodec_free_context(&audio_dec_ctx);
            audio_dec_ctx = nullptr;
        }

        if (fmt_ctx != nullptr) {
            avformat_close_input(&fmt_ctx);
            fmt_ctx = nullptr;
        }

        if (pkt != nullptr) {
            av_packet_free(&pkt);
            pkt = nullptr;
        }

        if (frame != nullptr) {
            av_frame_free(&frame);
            frame = nullptr;
        }
    }

private:
    InputStreamCallback* callback;
    AVFormatContext *fmt_ctx = nullptr;
    AVCodecContext *audio_dec_ctx = nullptr;
    AVCodecContext *video_dec_ctx = nullptr;
    int width = 0, height = 0;
    enum AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    AVStream *audio_stream = nullptr;
    AVStream *video_stream = nullptr;
    const char *src_filename = nullptr;
    int audio_stream_idx = -1;
    int video_stream_idx = -1;
    AVFrame *frame = nullptr;
    AVFrame *videoFrame = nullptr;
    AVPacket *pkt = nullptr;
    int audio_frame_count = 0;
    int64_t duration = 0;
    std::unique_ptr<ProcessCallback> processCallback;
    bool stopped = false;
    std::mutex lockMutex;

    bool isStopped() {
        std::lock_guard<std::mutex> lg(lockMutex);
        return stopped;
    }

    void output_audio_frame(AVFrame *audioFrame) {
        callback->onAudioFrame(audioFrame);
    }

    void output_video_frame(AVFrame *pFrame) {
        callback->onVideoFrame(pFrame);
    }

    int decode_packet(AVCodecContext *dec, const AVPacket *package) {
        int ret;

        // submit the packet to the decoder
        ret = avcodec_send_packet(dec, package);
        if (ret < 0) {

            throw ConvertException(std::string("decode error: Error submitting a packet for decoding: ") + av_err2str(ret));
//            fprintf(stderr, "Error submitting a packet for decoding (%s)\n", av_err2str(ret));
        }

        // get all the available frames from the decoder
        while (ret >= 0) {
            ret = avcodec_receive_frame(dec, frame);
            if (ret < 0) {
                // those two return values are special and mean there is no output
                // frame available, but there were no errors during decoding
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                    return 0;

                throw ConvertException(std::string("decode error: Error during decoding: ") + av_err2str(ret));
//                fprintf(stderr, "Error during decoding (%s)\n", av_err2str(ret));
//                return ret;
            }

            // write the frame data to output file
            if (dec->codec->type == AVMEDIA_TYPE_VIDEO)
                output_video_frame(frame);
            else
                output_audio_frame(frame);

            av_frame_unref(frame);
        }

        return 0;
    }
    int decode_packet_video(AVCodecContext *dec, const AVPacket *package) {
        int ret;

        // submit the packet to the decoder
        ret = avcodec_send_packet(dec, package);
        if (ret < 0) {

            throw ConvertException(std::string("decode error: Error submitting a packet for decoding: ") + av_err2str(ret));
//            fprintf(stderr, "Error submitting a packet for decoding (%s)\n", av_err2str(ret));
        }

        // get all the available frames from the decoder
        while (ret >= 0) {
            ret = avcodec_receive_frame(dec, videoFrame);
            if (ret < 0) {
                // those two return values are special and mean there is no output
                // frame available, but there were no errors during decoding
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                    return 0;

                throw ConvertException(std::string("decode error: Error during decoding: ") + av_err2str(ret));
//                fprintf(stderr, "Error during decoding (%s)\n", av_err2str(ret));
//                return ret;
            }

            // write the frame data to output file
            output_video_frame(videoFrame);

            av_frame_unref(videoFrame);
        }

        return 0;
    }

    static void open_codec_context(int *stream_idx,
                           AVCodecContext **dec_ctx, AVFormatContext *formatContext,
                           enum AVMediaType type) {
        int ret, stream_index;
        AVStream *st;
        const AVCodec *dec;
        AVDictionary *opts = nullptr;

        ret = av_find_best_stream(formatContext, type, -1, -1, nullptr, 0);
        if (ret < 0) {
            throw ConvertException("audio stream not found.");
//            fprintf(stderr, "Could not find %s stream in input file '%s'\n",
//                    av_get_media_type_string(type), src_filename);
//            return ret;
        } else {
            stream_index = ret;
            st = formatContext->streams[stream_index];

            /* find decoder for the stream */
            dec = avcodec_find_decoder(st->codecpar->codec_id);
            if (!dec) {
//                fprintf(stderr, "Failed to find %s codec\n",
//                        av_get_media_type_string(type));
                throw ConvertException("decode error: Failed to find codec");
//                return AVERROR(EINVAL);
            }

            /* Allocate a codec context for the decoder */
            *dec_ctx = avcodec_alloc_context3(dec);
            if (!*dec_ctx) {
//                fprintf(stderr, "Failed to allocate the %s codec context\n",
//                        av_get_media_type_string(type));
                throw ConvertException("decode error: Failed to allocated the codec context");
//                return AVERROR(ENOMEM);
            }

            /* Copy codec parameters from input stream to output codec context */
            if ((ret = (avcodec_parameters_to_context(*dec_ctx, st->codecpar))) < 0) {
//                fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
//                        av_get_media_type_string(type));
                throw ConvertException(std::string("decode error: Failed to copy codec parameters: ") + av_err2str(ret));
//                return ret;
            }

            /* Init the decoders */
            if ((ret = (avcodec_open2(*dec_ctx, dec, &opts))) < 0) {
//                fprintf(stderr, "Failed to open %s codec\n",
//                        av_get_media_type_string(type));
                throw ConvertException(std::string("decode error: Failed to open codec") + av_err2str(ret));
//                return ret;
            }
            *stream_idx = stream_index;
        }

    }

public:
    void stop() {
            std::lock_guard<std::mutex> lg(lockMutex);
            if (stopped)
                return;

            stopped = true;

    }
    void init() {
        src_filename = sourcePath.c_str();
        int ret;
        /* open input file, and allocate format context */
        if ((ret = avformat_open_input(&fmt_ctx, src_filename, nullptr, nullptr)) < 0) {
//            fprintf(stderr, "Could not open source file %s\n", src_filename);
//            exit(1);
            throw ConvertException(std::string("open source: file failed: ") + av_err2str(ret));
        }

        /* retrieve stream information */
        if ((ret = avformat_find_stream_info(fmt_ctx, nullptr)) < 0) {
//            fprintf(stderr, "Could not find stream information\n");
//            exit(1);
            throw ConvertException(std::string("open source: Could not find stream information") + av_err2str(ret));
        }

        callback->onInit();
 
        open_codec_context(&audio_stream_idx, &audio_dec_ctx, fmt_ctx, AVMEDIA_TYPE_AUDIO);
        audio_stream = fmt_ctx->streams[audio_stream_idx];
        
        duration = audio_stream->duration; 

        /* dump input information to stderr */
//        av_dump_format(fmt_ctx, 0, src_filename, 0);

        frame = av_frame_alloc();
        if (!frame) {
//            fprintf(stderr, "Could not allocate frame\n");
//            AVERROR(ENOMEM);
            throw ConvertException("memory error: Could not allocate frame");
        }

        pkt = av_packet_alloc();
        if (!pkt) {
//            fprintf(stderr, "Could not allocate packet\n");
//            ret = AVERROR(ENOMEM);
            throw ConvertException("memory error: Could not allocate packet");
        }

        callback->onAudioStream(audio_dec_ctx);

        //open video stream
        videoFrame = av_frame_alloc();
        if (!videoFrame) {
            throw ConvertException("memory error: Could not allocate frame");
        }

        open_codec_context(&video_stream_idx, &video_dec_ctx, fmt_ctx, AVMEDIA_TYPE_VIDEO);
        video_stream = fmt_ctx->streams[video_stream_idx];

        /* allocate image where the decoded image will be put */
        width = video_dec_ctx->width;
        height = video_dec_ctx->height;
        pix_fmt = video_dec_ctx->pix_fmt;

        callback->onVideoStream(video_dec_ctx, video_stream);
    }

    void start() {
        callback->onStart();
        int ret = 0;

//        if (audio_stream)
//            printf("Demuxing audio from file '%s' into '%s'\n", src_filename, audio_dst_filename);

        /* read frames from the file */
        while (!isStopped() && av_read_frame(fmt_ctx, pkt) >= 0) {
            // check if the packet belongs to a stream we are interested in, otherwise
            // skip it
//            if (pkt->stream_index == video_stream_idx)
//                ret = decode_packet(video_dec_ctx, pkt);
//            else if (pkt->stream_index == audio_stream_idx)
            if (pkt->stream_index == audio_stream_idx) {
                ret = decode_packet(audio_dec_ctx, pkt);

                if (ret >= 0) {
                    int progress = pkt->pts * 100 / duration;
                    processCallback->onProgress(progress);
                }
            } else if (pkt->stream_index == video_stream_idx) {
                ret = decode_packet_video(video_dec_ctx, pkt);

            }
            av_packet_unref(pkt);
            if (ret < 0)
                break;

        }

        if (isStopped()) {
            throw ConvertException("cancelled");
        }

        /* flush the decoders */
        if (video_dec_ctx)
            decode_packet(video_dec_ctx, nullptr);
        if (audio_dec_ctx)
            decode_packet(audio_dec_ctx, nullptr);

//        printf("Demuxing succeeded.\n");

//        if (audio_stream) {
//            enum AVSampleFormat sfmt = audio_dec_ctx->sample_fmt;

//            __android_log_print(6, "AudioConverter", "audio %d, %d, %d, %d", audio_dec_ctx->sample_fmt, audio_dec_ctx->sample_rate, audio_dec_ctx->channels, audio_dec_ctx->channel_layout);
//            int n_channels = audio_dec_ctx->channels;
//            const char *fmt;

//            if (av_sample_fmt_is_planar(sfmt)) {
//                const char *packed = av_get_sample_fmt_name(sfmt);
//                __android_log_print(6, "AudioConverter", "planar %s", packed);
//                printf("Warning: the sample format the decoder produced is planar "
//                       "(%s). This example will output the first channel only.\n",
//                       packed ? packed : "?");
//                sfmt = av_get_packed_sample_fmt(sfmt);
//                n_channels = 1;
//            }

//            ret = get_format_from_sample_fmt(&fmt, sfmt);

//            printf("Play the output audio file with the command:\n"
//                   "ffplay -f %s -ac %d -ar %d %s\n",
//                   fmt, n_channels, audio_dec_ctx->sample_rate,
//                   audio_dst_filename);
//        }

        callback->onEnd();

        release();
    }

private:
    std::string sourcePath;
};

class OutputStream : public InputStreamCallback {
public:
    OutputStream(const char *targetPath, const char *format)
    :targetPath(targetPath), format(format){

    }

    ~OutputStream() {
//        __android_log_write(6, "AudioConverter", "~OutputStream");
        if (codecContext != nullptr)
            avcodec_free_context(&codecContext);

        if (frame != nullptr)
            av_frame_free(&frame);

        if (frame1 != nullptr) {
            av_frame_free(&frame1);
        }

        if (frameWrite != nullptr) {
            av_frame_free(&frameWrite);
        }

        if (sws_ctx != nullptr) {
            sws_freeContext(sws_ctx);
            sws_ctx = nullptr;
        }
        if (swr_ctx != nullptr)
            swr_free(&swr_ctx);



        if (context != nullptr) {
            if (!(context->oformat->flags & AVFMT_NOFILE))
                /* Close the output file. */
                avio_closep(&context->pb);

            /* free the stream */
            avformat_free_context(context);
        }
    }
    

private:
    std::string targetPath;
    std::string format;

    AVFormatContext *context{};
    AVStream *stream{};
    AVCodecContext *codecContext{};
    AVCodec *encoder{};
    AVFrame *frame{};
    bool immediate{true};
    bool lastFrame{false};
    int lastFramePos = 0;
    AVFrame *frame1{};
    AVFrame *frameWrite{};
    struct SwrContext *swr_ctx{};
    int samples_count{};
    int64_t next_pts{};

    int sourceSample_rate = 0;
    uint64_t sourceLayout = 0;
    int sourceChannels = 0;

    AVStream *videoStream{};
    AVCodecContext *videoCodecContext{};
    AVCodec *videoEncoder{};
    AVFrame *videoFrame{};
    AVFrame *tmpVideoFrame{};
    AVSampleFormat sourceSampleFormat = AV_SAMPLE_FMT_NONE;
    struct SwsContext *sws_ctx{};
    int64_t next_ptsVideo{0};
    int sourceWidth = 0;
    int sourceHeight = 0;
    const char* sourceRotate = nullptr;
public:
    /*
     * useless
     */
    void init() {
        int ret;
        ret = avformat_alloc_output_context2(&context, nullptr, format.c_str(), targetPath.c_str());
        if (ret < 0) {
            throw ConvertException(std::string("create target: can't alloc output") + av_err2str(ret));
        }

        add_stream(context->oformat->audio_codec, true);

        AVDictionary *opt = nullptr;
        open_audio(opt);

        if (!(context->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open(&context->pb, targetPath.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0) {
                throw ConvertException(std::string("create target: can't open avio:") + av_err2str(ret));
            }
        }
        /* Write the stream header, if any. */

        ret = avformat_write_header(context, &opt);
        if (ret < 0)
            throw ConvertException(std::string("create target: can't write header") + av_err2str(ret));
    }

    void addAudio() {
//        context->oformat->audio_codec = AV_CODEC_ID_MP3;
        add_stream(context->oformat->audio_codec, true);

        AVDictionary *opt = nullptr;
        open_audio(opt);
    }

    void addVideo(AVCodecContext *sourceCodecContext) {
        __android_log_print(6, "AudioConverter", "addVideo");
        if (context->oformat->video_codec == AV_CODEC_ID_MPEG4) {
            context->oformat->video_codec = AV_CODEC_ID_H264;
        }
        add_stream(context->oformat->video_codec, false);
        AVDictionary *opt = nullptr;
        open_video(opt);
    }

    void end() {
        av_write_trailer(context);

        avcodec_free_context(&codecContext);
        avcodec_free_context(&videoCodecContext);
        av_frame_free(&frame);
        av_frame_free(&videoFrame);
        av_frame_free(&tmpVideoFrame);
        sws_freeContext(sws_ctx);
        swr_free(&swr_ctx);


        if (!(context->oformat->flags & AVFMT_NOFILE))
            avio_closep(&context->pb);

        avformat_free_context(context);

        context = nullptr;
        codecContext = nullptr;
        frame = nullptr;
        swr_ctx = nullptr;
        sws_ctx = nullptr;
    }

    /* Add an output stream. */
    void add_stream(enum AVCodecID codec_id, bool audio) {
        __android_log_print(6, "AudioConverter", "add stream %d", codec_id);
        int i;
        AVCodec* enc;
        /* find the encoder */
        if (codec_id == AV_CODEC_ID_H264) {
            enc = &ff_android_hw_h264_encoder;
        } else {
            enc = avcodec_find_encoder(codec_id);
        }

        if (!enc) {
            throw ConvertException("encode error: can't find encoder");
        }

        AVStream * str = avformat_new_stream(context, nullptr);
        if (!str) {
            throw ConvertException("encode error: can't new stream");
        }
        str->id = (int)(context->nb_streams - 1);
        AVCodecContext* c = avcodec_alloc_context3(enc);
        if (!c) {
            throw ConvertException("encode error: can't alloc context3");
        }
        if (audio) {
            encoder = enc;
            stream = str;
            codecContext = c;
        } else {
            videoEncoder = enc;
            videoStream = str;
            videoCodecContext = c;
        }

        switch (enc->type) {
            case AVMEDIA_TYPE_AUDIO:
                codecContext->sample_fmt = encoder->sample_fmts ?
                                                   encoder->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
                codecContext->bit_rate = 64000;

                codecContext->sample_rate = 44100;
                if (encoder->supported_samplerates) {
                    codecContext->sample_rate = encoder->supported_samplerates[0];
                    for (i = 0; encoder->supported_samplerates[i]; i++) {
                        if (encoder->supported_samplerates[i] == 44100)
                            codecContext->sample_rate = 44100;
                    }
                }
                codecContext->channels = av_get_channel_layout_nb_channels(
                        codecContext->channel_layout);
                codecContext->channel_layout = AV_CH_LAYOUT_STEREO;
                if (encoder->channel_layouts) {
                    codecContext->channel_layout = encoder->channel_layouts[0];
                    for (i = 0; encoder->channel_layouts[i]; i++) {
                        if (encoder->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
                            codecContext->channel_layout = AV_CH_LAYOUT_STEREO;
                    }
                }


                codecContext->channels = av_get_channel_layout_nb_channels(
                        codecContext->channel_layout);
                stream->time_base = (AVRational) {1, codecContext->sample_rate};
                break;

            case AVMEDIA_TYPE_VIDEO:
                videoCodecContext->codec_type = AVMEDIA_TYPE_VIDEO;
                videoCodecContext->codec_id = codec_id;

                videoCodecContext->bit_rate = 4000000;
                /* Resolution must be a multiple of two. */
//                videoCodecContext->width    = 352;
//                videoCodecContext->height   = 288;
                videoCodecContext->width    = 640;
                videoCodecContext->height = 360;
                /* timebase: This is the fundamental unit of time (in seconds) in terms
                 * of which frame timestamps are represented. For fixed-fps content,
                 * timebase should be 1/framerate and timestamp increments should be
                 * identical to 1. */
                videoStream->time_base = (AVRational){ 1, STREAM_FRAME_RATE };
                videoCodecContext->framerate = (AVRational){STREAM_FRAME_RATE, 1};
                videoCodecContext->time_base = videoStream->time_base;
                videoCodecContext->gop_size      = 6; /* emit one intra frame every twelve frames at most */
                videoCodecContext->max_b_frames = 0;
                videoCodecContext->pix_fmt       = AV_PIX_FMT_YUV420P;
                if (videoEncoder->pix_fmts) {
                    videoCodecContext->pix_fmt = videoEncoder->pix_fmts[0];
                    for (i = 0; videoEncoder->pix_fmts[i]; i++) {
                        if (videoEncoder->pix_fmts[i] == AV_PIX_FMT_YUV420P)
                            videoCodecContext->pix_fmt = AV_PIX_FMT_YUV420P;
                    }
                }

                if (videoCodecContext->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
                    /* just for testing, we also add B-frames */
                    videoCodecContext->max_b_frames = 2;
                }
                if (videoCodecContext->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
                    /* Needed to avoid using macroblocks in which some coeffs overflow.
                     * This does not happen with normal video, it just happens here as
                     * the motion of the chroma plane does not match the luma plane. */
                    videoCodecContext->mb_decision = 2;
                }
                break;

            default:
                break;
        }
        /* Some formats want stream headers to be separate. */
        if (context->oformat->flags & AVFMT_GLOBALHEADER)
            c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    void open_audio(AVDictionary *opt_arg) {
//        AVFormatContext *oc = context;
        AVCodec *codec = encoder;
        AVCodecContext *c;
        OutputStream *ost = this;
        int nb_samples;
        int ret;
        AVDictionary *opt = nullptr;

        c = ost->codecContext;

        /* open it */
        av_dict_copy(&opt, opt_arg, 0);
        ret = avcodec_open2(c, codec, &opt);
        av_dict_free(&opt);
        if (ret < 0) {
            throw ConvertException(std::string("encode error: Could not open audio codec: ") + av_err2str(ret));
        }

        if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
            nb_samples = 10000;
        else {
            nb_samples = c->frame_size;
            immediate = false;
        }



        ost->frame = alloc_audio_frame(c->sample_fmt, c->channel_layout,
                                       c->sample_rate, nb_samples);

        ost->frame1 = alloc_audio_frame(c->sample_fmt, c->channel_layout,
                                       c->sample_rate, nb_samples);

        ost->frameWrite = alloc_audio_frame(c->sample_fmt, c->channel_layout,
                                       c->sample_rate, nb_samples);

        /* copy the stream parameters to the muxer */
        ret = avcodec_parameters_from_context(ost->stream->codecpar, c);
        if (ret < 0) {
            throw ConvertException(std::string("encode error: Could not copy the stream parameters: ") + av_err2str(ret));
        }

        /* create resampler context */
        ost->swr_ctx = swr_alloc();
        if (!ost->swr_ctx) {
            throw ConvertException("encode error: Could not allocate resampler context");
        }

        /* set options */
        av_opt_set_int(ost->swr_ctx, "in_channel_layout",  sourceLayout, 0);
        av_opt_set_int(ost->swr_ctx, "out_channel_layout", c->channel_layout,  0);
        av_opt_set_int(ost->swr_ctx, "in_channel_count", sourceChannels, 0);
        av_opt_set_int(ost->swr_ctx, "out_channel_count", c->channels, 0);
        av_opt_set_int(ost->swr_ctx, "in_sample_rate", sourceSample_rate, 0);
        av_opt_set_int(ost->swr_ctx, "out_sample_rate", c->sample_rate, 0);
        av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt", sourceSampleFormat, 0);
        av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt", c->sample_fmt, 0);

        /* initialize the resampling context */
        if ((ret = (swr_init(ost->swr_ctx))) < 0) {
            throw ConvertException(std::string("encode error: Failed to initialize the resampling context: ") + av_err2str(ret));
        }
    }

    void open_video(AVDictionary *opt_arg)
    {
        int ret;
        AVCodecContext *c = videoCodecContext;
        AVCodec *codec = videoEncoder;
        AVDictionary *opt = nullptr;

//        if (codec->id == AV_CODEC_ID_H264)
//            av_opt_set(c->priv_data, "preset", "slow", 0);

        av_dict_copy(&opt, opt_arg, 0);

        /* open the codec */
        ret = avcodec_open2(c, codec, &opt);
        av_dict_free(&opt);
        if (ret < 0) {
            throw ConvertException(std::string("encode error: Could not open video codec: ") + av_err2str(ret));
        }

        /* allocate and init a re-usable frame */
        videoFrame = alloc_picture(c->pix_fmt, c->width, c->height);

        tmpVideoFrame = nullptr;

        /* copy the stream parameters to the muxer */
        ret = avcodec_parameters_from_context(videoStream->codecpar, c);
        if (ret < 0) {
            throw ConvertException(std::string("encode error: Could not copy the video stream parameters: ") + av_err2str(ret));
        }

        if (sourceRotate != nullptr) {
            av_dict_set(&videoStream->metadata, "rotate", sourceRotate, 0);
        }

    }

    static AVFrame* alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                               uint64_t channel_layout,
                                               int sample_rate, int nb_samples) {
        AVFrame *frame = av_frame_alloc();
        int ret;

        if (!frame) {
            throw ConvertException("memory error: Error allocating an audio frame");
        }

        frame->format = sample_fmt;
        frame->channel_layout = channel_layout;
        frame->sample_rate = sample_rate;
        frame->nb_samples = nb_samples;

        if (nb_samples) {
            ret = av_frame_get_buffer(frame, 0);
            if (ret < 0) {
                throw ConvertException("memory error: Error allocating an audio buffer");
            }
        }

        return frame;
    }

    static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
    {
        AVFrame *picture;
        int ret;

        picture = av_frame_alloc();
        if (!picture)
            throw ConvertException("memory error: Error allocating an video buffer 1");

        picture->format = pix_fmt;
        picture->width  = width;
        picture->height = height;

        /* allocate the buffers for the frame data */
        ret = av_frame_get_buffer(picture, 0);
        if (ret < 0) {
            throw ConvertException("memory error: Error allocating an video buffer 2");
        }

        return picture;
    }


    void write_audio_frame(AVFrame* audioFrame) {
        int ret;
        int dst_nb_samples;

        if (audioFrame) {
            int r = swr_get_delay(swr_ctx, sourceSample_rate);
            dst_nb_samples = av_rescale_rnd(
                    r +
                            audioFrame->nb_samples,
                    codecContext->sample_rate, sourceSample_rate, AV_ROUND_UP);
            ret = av_frame_make_writable(frame);
            if (ret < 0)
                throw ConvertException(std::string("encode error: av_frame_make_writable error: ") + av_err2str(ret));
//            exit(1);

            /* convert to destination format */
            ret = swr_convert(swr_ctx,
                              frame->data, dst_nb_samples,
                              (const uint8_t **) audioFrame->data, audioFrame->nb_samples);
            if (ret < 0) {
                throw ConvertException(std::string("encode error: swr_convert error: ") + av_err2str(ret));
            }

            frame->nb_samples = ret;

            if (immediate) {
                audioFrame = frame;

                audioFrame->pts = av_rescale_q(samples_count,
                                               (AVRational) {1, codecContext->sample_rate},
                                               codecContext->time_base);
                samples_count += ret;

                write_frame(codecContext, stream, audioFrame);
            } else {
                if (!lastFrame) {
                    if (ret == codecContext->frame_size) {
                        audioFrame = frame;

                        audioFrame->pts = av_rescale_q(samples_count,
                                                       (AVRational) {1, codecContext->sample_rate},
                                                       codecContext->time_base);
                        samples_count += ret;
                        write_frame(codecContext, stream, audioFrame);
                        return;
                    }

                    lastFrame = true;
                    lastFramePos = 0;
                    AVFrame* tmp = frame1;
                    frame1 = frame;
                    frame = tmp;
                    return;
                }

                ret = av_frame_make_writable(frameWrite);
                if (ret < 0)
                    throw ConvertException(std::string("encode error: av_frame_make_writable 2 error: ") + av_err2str(ret));

                int nextPos = combineFrame(frame1, lastFramePos, frame, frameWrite, codecContext->frame_size);
                if (frameWrite->nb_samples < codecContext->frame_size) {
                    lastFrame = true;
                    AVFrame* tmp = frame1;
                    frame1 = frameWrite;
                    frameWrite = tmp;
                    lastFramePos = 0;
                    return;
                }

                if (nextPos == frame->nb_samples) {
                    lastFrame = false;
                    lastFramePos = 0;
                } else {
                    lastFrame = true;
                    AVFrame* tmp = frame1;
                    frame1 = frame;
                    frame = tmp;
                    lastFramePos = nextPos;
                }

                frameWrite->pts = av_rescale_q(samples_count,
                                               (AVRational) {1, codecContext->sample_rate},
                                               codecContext->time_base);

                samples_count += frameWrite->nb_samples;
                write_frame(codecContext, stream, frameWrite);
            }
        } else {
            write_frame(codecContext, stream, audioFrame);
        }
    }
    void write_audio_frame_immediate(AVFrame* audioFrame) {
        int ret;
        int dst_nb_samples;

        if (audioFrame) {

//            __android_log_print(6, "AudioConverter", "write audio");
            dst_nb_samples = av_rescale_rnd(
                    swr_get_delay(swr_ctx, sourceSample_rate) +
                            audioFrame->nb_samples,
                    codecContext->sample_rate, sourceSample_rate, AV_ROUND_UP);
//            __android_log_print(6, "AudioConverter", "resample %d, %d, %d, %d, %d, %d", sourceSampleFormat, codecContext->sample_fmt, sourceSample_rate, codecContext->sample_rate, audioFrame->nb_samples, dst_nb_samples);

            ret = av_frame_make_writable(frame);
            if (ret < 0)
                throw ConvertException(std::string("encode error: av_frame_make_writable error: ") + av_err2str(ret));

            ret = swr_convert(swr_ctx,
                              frame->data, dst_nb_samples,
                              (const uint8_t **) audioFrame->data, audioFrame->nb_samples);
            if (ret < 0) {
                throw ConvertException(std::string("encode error: swr_convert error: ") + av_err2str(ret));
            }

            frame->nb_samples = ret;

            audioFrame = frame;

            audioFrame->pts = av_rescale_q(samples_count,
                                           (AVRational) {1, codecContext->sample_rate},
                                           codecContext->time_base);
            samples_count += ret;
//            samples_count += audioFrame->nb_samples;

            write_frame(codecContext, stream, audioFrame);
        } else {
            write_frame(codecContext, stream, audioFrame);
        }
    }

    //return next position
    static int combineFrame(const AVFrame* frame1, int pos, const AVFrame* frame2, AVFrame* target, int size) {
        int remainLast = frame1->nb_samples - pos;
        memcpy(target->data[0], frame1->data[0] + pos * 4, remainLast * 4);
        int need = size - remainLast;
        if (need > frame2->nb_samples) {
            need = frame2->nb_samples;
        }

        memcpy(target->data[0] + remainLast * 4, frame2->data[0], need * 4);

        memcpy(target->data[1], frame1->data[1] + pos * 4, remainLast * 4);
        memcpy(target->data[1] + remainLast * 4, frame2->data[1], need * 4);
        target->nb_samples = need + remainLast;
        return need;
    }

    void write_video_frame(AVFrame *pFrame) {
        if (!pFrame) {
            write_frame(videoCodecContext, videoStream, pFrame);
            return;
        }

        AVCodecContext *c = videoCodecContext;
//        __android_log_print(6, "AudioConverter", "write video %d, %d, %d, %d, %d, %d", pFrame->format, pFrame->width, pFrame->height, c->pix_fmt, c->width, c->height);
        int ret;

        /* when we pass a frame to the encoder, it may keep a reference to it
         * internally; make sure we do not overwrite it here */
        if ((ret = av_frame_make_writable(videoFrame)) < 0)
            throw ConvertException(std::string("encode error: av_frame_make_writable video error: ") + av_err2str(ret));

        if (c->pix_fmt != pFrame->format || c->width != pFrame->width || c->height != pFrame->height) {
            /* as we only generate a YUV420P picture, we must convert it
             * to the codec pixel format if needed */
            if (!sws_ctx) {
                sws_ctx = sws_getContext(pFrame->width, pFrame->height,
                                         (enum AVPixelFormat)pFrame->format,
                                              c->width, c->height,
                                              c->pix_fmt,
                                              SCALE_FLAGS, nullptr, nullptr, nullptr);
                if (!sws_ctx) {
                    throw ConvertException("Could not initialize the sws conversion context");
                }
            }

//            fill_yuv_image(ost->tmp_frame, ost->next_pts, c->width, c->height);
            sws_scale(sws_ctx, pFrame->data,
                        pFrame->linesize, 0, pFrame->height, videoFrame->data,
                      videoFrame->linesize);

            videoFrame->pts = ++next_ptsVideo;

            write_frame(videoCodecContext, videoStream, videoFrame);
        } else {

            pFrame->pts = ++next_ptsVideo;

            write_frame(videoCodecContext, videoStream, pFrame);
//            fill_yuv_image(ost->frame, ost->next_pts, c->width, c->height);
        }
    }

    void write_frame(AVCodecContext *c, AVStream *st, AVFrame *pFrame) {
        AVFormatContext *fmt_ctx = context;
        int ret = 0;


        // send the frame to the encoder
        ret = avcodec_send_frame(c, pFrame);
        if (ret < 0) {
            throw ConvertException(std::string("encode error: Error sending a video frame to the encoder: ") + av_err2str(ret));
        }

        while (ret >= 0) {
            AVPacket pkt = {nullptr};

            ret = avcodec_receive_packet(c, &pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            else if (ret < 0) {
                throw ConvertException(std::string("encode error: Error encoding a frame: ") + av_err2str(ret));
            }

//            __android_log_print(6, "AudioConverter", "video pts: %ld, %d, %d", pkt.pts, c->time_base.num, c->time_base.den);
            /* rescale output packet timestamp values from codec to stream timebase */
            av_packet_rescale_ts(&pkt, c->time_base, st->time_base);
            pkt.stream_index = st->index;

            /* Write the compressed frame to the media file. */
//        log_packet(fmt_ctx, &pkt);
            ret = av_interleaved_write_frame(fmt_ctx, &pkt);
            av_packet_unref(&pkt);
            if (ret < 0) {
                throw ConvertException(std::string("encode error: av_interleaved_write_frame: ") + av_err2str(ret));
            }
        }

    }

    void onInit() override {
        int ret;
        ret = avformat_alloc_output_context2(&context, nullptr, format.c_str(), targetPath.c_str());
        if (ret < 0) {
            throw ConvertException(std::string("create target: can't alloc output:") + av_err2str(ret));
        }
    }

    void onAudioStream(AVCodecContext *sourceCodecContext) override {
        sourceSample_rate = sourceCodecContext->sample_rate;
        sourceLayout = sourceCodecContext->channel_layout;
        sourceChannels = sourceCodecContext->channels;
        sourceSampleFormat = sourceCodecContext->sample_fmt;
        __android_log_print(6, "AudioConverter", "onAudioStream %d, %d", sourceSample_rate, sourceSampleFormat);

        addAudio();

    }

    void onVideoStream(AVCodecContext *sourceCodecContext, AVStream* st) override {
        sourceWidth = sourceCodecContext->width;
        sourceHeight = sourceCodecContext->height;
        AVDictionaryEntry *pEntry = av_dict_get(st->metadata, "rotate", nullptr,
                                                AV_DICT_MATCH_CASE);
        if (pEntry != nullptr) {
            sourceRotate = pEntry->value;
        }
        __android_log_print(6, "AudioConverter", "onVideoStream %d, %d", sourceWidth, sourceHeight);
        addVideo(sourceCodecContext);
    }

    void onStart() override {
        int ret;

        if (!(context->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open(&context->pb, targetPath.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0) {
                throw ConvertException(std::string("create target: can't open avio:") + av_err2str(ret));
            }
        }

        AVDictionary *opt = nullptr;
        ret = avformat_write_header(context, &opt);
        if (ret < 0)
            throw ConvertException(std::string("create target: can't write header") + av_err2str(ret));
    }

    void onAudioFrame(AVFrame *audioFrame) override {
//        write_audio_frame_immediate(audioFrame);
        write_audio_frame(audioFrame);
    }

    void onVideoFrame(AVFrame *pFrame) override {
        write_video_frame(pFrame);
    }

    void onEnd() override {
        write_audio_frame(nullptr);
        write_video_frame(nullptr);
        end();
    }

};

AudioConverter::AudioConverter(ProcessCallback* callback, const char *sourcePath, const char *targetPath, const char *format)
        : target(new OutputStream(targetPath, format)),
        inputStream(new InputStream(callback, target.get(), sourcePath)) {

}

AudioConverter::~AudioConverter() noexcept = default;

const char* AudioConverter::convert() {
    try {
        inputStream->init();
        inputStream->start();
//        inputStream.reset();
//        target.reset();
    } catch (std::exception& e) {
//        __android_log_write(6, "AudioConverter", e.what());
        return e.what();
    }

    return nullptr;
}

void AudioConverter::cancel() {
    inputStream->stop();
}


//////////////jni

static jlong nativeInit(JNIEnv* env,
                       jobject  thzz, jstring source, jstring target, jstring format) {

    jboolean copy;
    const char *sourcePath = env->GetStringUTFChars(source, &copy);
    const char *targetPath = env->GetStringUTFChars(target, &copy);
    const char *formatUTF = env->GetStringUTFChars(format, &copy);

    auto* progressCallback = new JavaProgressCallback(env, thzz);
    auto* converter = new AudioConverter(progressCallback, sourcePath, targetPath, formatUTF);

    env->ReleaseStringUTFChars(source, sourcePath);
    env->ReleaseStringUTFChars(target, targetPath);
    env->ReleaseStringUTFChars(format, formatUTF);

    return jlong(converter);
}

static jstring nativeConvert(JNIEnv* env,
                          jobject  /*thzz*/, jlong ptr) {
    auto* converter = reinterpret_cast<AudioConverter*>(ptr);
    const char* error = converter->convert();
    if (error == nullptr)
        return nullptr;

    return env->NewStringUTF(error);
}

static void nativeStop(JNIEnv* /*env*/,
                       jobject  /*thzz*/, jlong ptr) {
    auto* converter = reinterpret_cast<AudioConverter*>(ptr);
    converter->cancel();
}

static void nativeRelease(JNIEnv* /*env*/,
                       jobject  /*thzz*/, jlong ptr) {
    auto* converter = reinterpret_cast<AudioConverter*>(ptr);
    delete converter;
}


static const JNINativeMethod methods[] =
        {
                { "nativeInit",         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)J", ( void* )nativeInit     },
                { "nativeConvert",               "(J)Ljava/lang/String;",     ( void* )nativeConvert           },
                { "nativeStop",            "(J)V",                             ( void* )nativeStop        },
                { "nativeRelease",            "(J)V",                             ( void* )nativeRelease        },
        };

void AudioConverter::initClass(JNIEnv *env, jclass clazz) {
    env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0]));
    JavaProgressCallback::init(env, clazz);
//    JavaProgressCallback::progressMethod = env->GetMethodID(clazz, "onProgress", "(I)V");
//    if ( env->ExceptionCheck() )
//    {
//        _env->ExceptionDescribe();
//    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mxtech_audio_AudioConverter_nativeInitClass(
        JNIEnv* env,
        jclass clazz) {
    AudioConverter::initClass(env, clazz);
}





