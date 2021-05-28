//
// Created by ZhaoLinlin on 2021/5/18.
//

#include "MediaConverter.h"
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
#include "libyuv.h"

#include "mediacodec/NXMediaCodecEncInterface.h"

extern AVCodec ff_android_hw_h264_encoder;
#define SCALE_FLAGS SWS_BICUBIC
#define STREAM_FRAME_RATE 25 /* 25 images/s */
//const char AudioConverter::TAG[] = PROJECTIZED( "AudioConverter" );
const char MediaConverter::TAG[] = "AudioConverter";

#include <mutex>
#include <thread>
#include <condition_variable>
#include <deque>

class ConvertException : public std::exception {
public:
    explicit ConvertException(std::string  what): w(std::move(what)) {

    }
    explicit ConvertException(const char* what) noexcept: w(what)  {
    }

    explicit ConvertException(const std::exception& e) noexcept :w(e.what()) {

    }

    ConvertException& operator =(const std::exception& e) noexcept {
        if (&e == this)
            return *this;

        w = e.what();
        return *this;
    }

    const char * what() const noexcept override {
        return w.c_str();
    }

private:
    std::string w;
};

namespace in {
    template <typename T>
    class queue
    {
    private:
        std::mutex              d_mutex{};
        std::condition_variable d_condition{};
        std::condition_variable d_condition_full{};
        std::deque<T>           d_queue{};
    public:
        void push(T const& value) {
//            __android_log_print(6, "AudioConverter", "push");
            {
                std::unique_lock<std::mutex> lock(this->d_mutex);
                this->d_condition_full.wait(lock, [=]{
                    return this->d_queue.size() < 2; }
                    );
                d_queue.push_front(value);
            }
            this->d_condition.notify_one();
//            __android_log_print(6, "AudioConverter", "push end");
        }
        T pop() {
//            __android_log_print(6, "AudioConverter", "pop");
            std::unique_lock<std::mutex> lock(this->d_mutex);
            this->d_condition.wait(lock, [=]{ return !this->d_queue.empty(); });
            T rc(this->d_queue.back());
            this->d_queue.pop_back();
            this->d_condition_full.notify_one();
//            __android_log_print(6, "AudioConverter", "pop end");
            return rc;
        }
    };
    
    enum class TYPE {
        ON_INIT,
        ON_AUDIO,
        ON_VIDEO,
        ON_START,
        ON_AUDIO_FRAME,
        ON_VIDEO_FRAME,
        ON_END,
        ON_EXCEPTION
    };
    
    class Message {
    private:
        explicit Message(const TYPE& type): t(type) {
            
        }

    public:
        static Message onInit() {
            return Message(TYPE::ON_INIT);
        }

        static Message onAudio(AVCodecContext *sourceCodecContext) {
            Message msg = Message(TYPE::ON_AUDIO);

            msg.sourceSample_rate = sourceCodecContext->sample_rate;
            msg.sourceLayout = sourceCodecContext->channel_layout;
            msg.sourceChannels = sourceCodecContext->channels;
            msg.sourceSampleFormat = sourceCodecContext->sample_fmt;

            return msg;
        }

        static Message onVideo(AVCodecContext *sourceCodecContext, AVStream* st) {
            Message msg = Message(TYPE::ON_VIDEO);
            msg.sourceWidth = sourceCodecContext->width;
            msg.sourceHeight = sourceCodecContext->height;
            msg.sourceVideoTimebase = sourceCodecContext->pkt_timebase;
            AVDictionaryEntry *pEntry = av_dict_get(st->metadata, "rotate", nullptr,
                                                    AV_DICT_MATCH_CASE);
            if (pEntry != nullptr) {
                msg.sourceRotate = pEntry->value;
            }
            return msg;
        }

        static Message onStart() {
            Message msg = Message(TYPE::ON_START);
            return msg;
        }
        
        static Message onAudioFrame(AVFrame* pFrame) {
            Message msg = Message(TYPE::ON_AUDIO_FRAME);
            msg.audioFrame = pFrame;
            return msg;
        }
        
        static Message onVideoFrame(AVFrame* pFrame) {
            Message msg = Message(TYPE::ON_VIDEO_FRAME);
            msg.videoFrame = pFrame;
            return msg;
        }
        
        static Message onEnd() {
            return Message(TYPE::ON_END);
        }
                
        static Message onException(const std::exception& e) {
            Message msg = Message(TYPE::ON_EXCEPTION);
            msg.exception = e;
            return msg;
        }
        
        TYPE type() const {
            return t;
        }

    public:
        Message(const Message& msg) noexcept:
                sourceSample_rate(msg.sourceSample_rate),
                sourceLayout(msg.sourceLayout),
                sourceChannels(msg.sourceChannels),
                sourceSampleFormat(msg.sourceSampleFormat),
                sourceWidth(msg.sourceWidth),
                sourceHeight(msg.sourceHeight),
                sourceVideoTimebase(msg.sourceVideoTimebase),
                sourceRotate(msg.sourceRotate),
                audioFrame(msg.audioFrame),
                videoFrame(msg.videoFrame),
                exception(msg.exception),
                t(msg.t)
                {

        }

        Message& operator =(const Message &msg) noexcept {
            if (&msg == this)
                return *this;

            sourceSample_rate = msg.sourceSample_rate;
            sourceLayout = msg.sourceLayout;
            sourceChannels = msg.sourceChannels;
            sourceSampleFormat = msg.sourceSampleFormat;
            sourceWidth = msg.sourceWidth;
            sourceHeight = msg.sourceHeight;
            sourceVideoTimebase = msg.sourceVideoTimebase;
            sourceRotate = msg.sourceRotate;
            audioFrame = msg.audioFrame;
            videoFrame = msg.videoFrame;
            exception = msg.exception;
            t = msg.t;

            return *this;
        }


    public:
        int sourceSample_rate{0};
        uint64_t sourceLayout{0};
        int sourceChannels{0};
        AVSampleFormat sourceSampleFormat{AV_SAMPLE_FMT_NONE};
    public:
        int sourceWidth{0};
        int sourceHeight{0};
        AVRational sourceVideoTimebase{0, 1};
        std::string sourceRotate;
    public:
        AVFrame* audioFrame{nullptr};
        AVFrame* videoFrame{nullptr};
    public:
        ConvertException exception{""};
        
    private:
        TYPE t;
    };
}

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
            __android_log_print(6, "AudioConverter", "progress %d", lasProgress);
//            env->CallVoidMethod(javaObject, progressMethod, progress);
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




class InputStreamCallback {
public:
    virtual void onInit() = 0;
    virtual void onAudioStream(AVCodecContext* codecContext) = 0;
    virtual void onVideoStream(AVCodecContext* codecContext, AVStream* stream) = 0;
    virtual void onStart() = 0;
    virtual void onAudioFrame(AVFrame* frame) = 0;
    virtual void onVideoFrame(AVFrame* frame) = 0;
    virtual void onEnd() = 0;
    virtual void onException(const std::exception& e) = 0;
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
    AVFrame *frame1 = nullptr;
    AVFrame *videoFrame = nullptr;
    AVFrame *videoFrame1 = nullptr;
    AVFrame* audioFrameList[2];
    AVFrame* videoFrameList[2];
    AVPacket *pkt = nullptr;
    int audio_frame_count = 0;
    int64_t duration = 0;
    std::unique_ptr<ProcessCallback> processCallback;
    bool stopped = false;
    std::mutex lockMutex;
    std::unique_ptr<std::thread> thread{};
    
    AVFrame * getAudioFrame() {
//        AVFrame * f = audioFrameList[0];
//        AVFrame * f1 = audioFrameList[1];
//        audioFrameList[0] = f1;
//        audioFrameList[1] = f;
//
//        return f;
        AVFrame *pFrame = av_frame_alloc();
        return pFrame;
    }
    
    AVFrame * getVideoFrame() {
//        AVFrame * f = videoFrameList[0];
//        AVFrame * f1 = videoFrameList[1];
//        videoFrameList[0] = f1;
//        videoFrameList[1] = f;
//
//        return f;
        AVFrame *pFrame = av_frame_alloc();
        return pFrame;
    }

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
            AVFrame *pFrame = getAudioFrame();
            ret = avcodec_receive_frame(dec, pFrame);
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
//            if (dec->codec->type == AVMEDIA_TYPE_VIDEO)
//                output_video_frame(frame);
//            else
                output_audio_frame(pFrame);

//            av_frame_unref(frame);
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
            AVFrame *pFrame = getVideoFrame();
            ret = avcodec_receive_frame(dec, pFrame);
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
            output_video_frame(pFrame);

//            av_frame_unref(videoFrame);
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
            if (AVMEDIA_TYPE_VIDEO == type) {
                st->codecpar->format = AV_PIX_FMT_YUV420P;

            }

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
            throw ConvertException("memory error: Could not allocate frame");
        }

        frame1 = av_frame_alloc();
        if (!frame1) {
            throw ConvertException("memory error: Could not allocate frame");
        }

        audioFrameList[0] = frame;
        audioFrameList[1] = frame1;


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
        videoFrame1 = av_frame_alloc();
        if (!videoFrame1) {
            throw ConvertException("memory error: Could not allocate frame");
        }
        videoFrameList[0] = videoFrame;
        videoFrameList[1] = videoFrame1;

        open_codec_context(&video_stream_idx, &video_dec_ctx, fmt_ctx, AVMEDIA_TYPE_VIDEO);
        video_stream = fmt_ctx->streams[video_stream_idx];

        video_dec_ctx->pkt_timebase = video_stream->time_base;


        /* allocate image where the decoded image will be put */
        width = video_dec_ctx->width;
        height = video_dec_ctx->height;
        pix_fmt = video_dec_ctx->pix_fmt;

        __android_log_print(6, "MediaConverter", "find video stream %d", pix_fmt);

        callback->onVideoStream(video_dec_ctx, video_stream);
    }

    static void run(InputStream* stream) {
        stream->doStart();
    }

    void join() {
        thread->join();
    }

    void start() {
        thread.reset(new std::thread(run, this));
    }
    void doStart() {
        try {
            doStartInner();
        } catch (std::exception& e) {
            __android_log_print(6, "AudioConverter", "doStart exception %s", e.what());
            callback->onException(e);
        }
    }
    void doStartInner() {
        __android_log_print(6, "AudioConverter", "doStartInner");
        init();

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

        if (videoFrame != nullptr) {
            av_frame_free(&videoFrame);
        }

        if (videoFrameRotate != nullptr) {
            av_frame_free(&videoFrame);
        }

        if (videoFrameConvert != nullptr) {
            av_frame_free(&videoFrameConvert);
        }
        if (videoFrameScale != nullptr) {
            av_frame_free(&videoFrameScale);
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
    AVFrame *videoFrameScale{};
    AVFrame *videoFrameRotate{};
    AVFrame *videoFrameConvert{};
    AVFrame *tmpVideoFrame{};
    bool preRotate{false};
    AVSampleFormat sourceSampleFormat = AV_SAMPLE_FMT_NONE;
    struct SwsContext *sws_ctx{};
    int64_t next_ptsVideo{0};
    int sourceWidth = 0;
    int sourceHeight = 0;
    int targetWidth{0};
    int targetWidthTmp{0};
    int targetHeight{0};
    int targetHeightTmp{0};
    AVRational sourceVideoTimebase{};
    int64_t firstVideoPts{-1};
    bool firstVideoPtsGot{false};
    std::string sourceRotate{};
    in::queue<in::Message> messageQueue{};
public:
    void loop() {
        __android_log_print(6, "AudioConverter", "start loop");
        while (true) {
            const in::Message &message = messageQueue.pop();

//            __android_log_print(6, "AudioConverter", "loop %d", message.type());

            if (message.type() == in::TYPE::ON_INIT) {
                doOnInit();
            } else if (message.type() == in::TYPE::ON_START) {
                doOnStart();
            } else if (message.type() == in::TYPE::ON_AUDIO) {
                doOnAudioStream(message);
            } else if (message.type() == in::TYPE::ON_VIDEO) {
                doOnVideoStream(message);
            } else if (message.type() == in::TYPE::ON_AUDIO_FRAME) {
                doOnAudioFrame(message.audioFrame);
            } else if (message.type() == in::TYPE::ON_VIDEO_FRAME) {
                doOnVideoFrame(message.videoFrame);
            } else if (message.type() == in::TYPE::ON_END) {
                doOnEnd();
                break;
            } else if (message.type() == in::TYPE::ON_EXCEPTION) {
                doOnException(message.exception);
                throw message.exception;
            }
        }   
    }

    void addAudio() {
//        context->oformat->audio_codec = AV_CODEC_ID_MP3;
        add_stream(context->oformat->audio_codec, true);

        AVDictionary *opt = nullptr;
        open_audio(opt);
    }

    void addVideo() {
        __android_log_print(6, "AudioConverter", "addVideo");
        computeTargetSize();
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
        av_frame_free(&videoFrameRotate);
        av_frame_free(&videoFrameConvert);
        av_frame_free(&videoFrameScale);
        av_frame_free(&tmpVideoFrame);
        sws_freeContext(sws_ctx);
        swr_free(&swr_ctx);


        if (!(context->oformat->flags & AVFMT_NOFILE))
            avio_closep(&context->pb);

        avformat_free_context(context);

        context = nullptr;
        codecContext = nullptr;
        frame = nullptr;
        videoFrame = nullptr;
        videoFrameRotate = nullptr;
        videoFrameConvert = nullptr;
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

                videoCodecContext->bit_rate =  (5000000 * (int64_t)targetWidth * targetHeight) /( 1080 * 720);
                /* Resolution must be a multiple of two. */
//                videoCodecContext->width    = 352;
//                videoCodecContext->height   = 288;

                videoCodecContext->width    = targetWidth;
                videoCodecContext->height = targetHeight;

                __android_log_print(6, "AudioConverter", "add video parameter %ld, %d, %d, %d, %d", videoCodecContext->bit_rate, targetWidth, targetHeight, sourceWidth, sourceHeight);
                /* timebase: This is the fundamental unit of time (in seconds) in terms
                 * of which frame timestamps are represented. For fixed-fps content,
                 * timebase should be 1/framerate and timestamp increments should be
                 * identical to 1. */
//                videoStream->time_base = (AVRational){ 1, STREAM_FRAME_RATE };
                videoStream->time_base = sourceVideoTimebase;
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
//                    videoCodecContext->max_b_frames = 2;
                }
                if (videoCodecContext->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
                    /* Needed to avoid using macroblocks in which some coeffs overflow.
                     * This does not happen with normal video, it just happens here as
                     * the motion of the chroma plane does not match the luma plane. */
//                    videoCodecContext->mb_decision = 2;
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
//        videoFrame = alloc_picture(c->pix_fmt, targetWidthTmp, targetHeightTmp);
//        videoFrame = alloc_picture(AV_PIX_FMT_YUV420P, targetWidthTmp, targetHeightTmp);
//        videoFrameRotate = alloc_picture(AV_PIX_FMT_YUV420P, c->width, c->height);


        tmpVideoFrame = nullptr;

        /* copy the stream parameters to the muxer */
        ret = avcodec_parameters_from_context(videoStream->codecpar, c);
        if (ret < 0) {
            throw ConvertException(std::string("encode error: Could not copy the video stream parameters: ") + av_err2str(ret));
        }

//        if (!sourceRotate.empty()) {
//            av_dict_set(&videoStream->metadata, "rotate", sourceRotate.c_str(), 0);
//        }

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

        if (!firstVideoPtsGot) {
            firstVideoPtsGot = true;
            firstVideoPts = pFrame->pts;
        }

        int64_t p = pFrame->pts - firstVideoPts;

        AVCodecContext *c = videoCodecContext;
//        __android_log_print(6, "MediaConverter", "write video %d, %d", pFrame->format, c->pix_fmt);
        int ret;

        /* when we pass a frame to the encoder, it may keep a reference to it
         * internally; make sure we do not overwrite it here */
//        if ((ret = av_frame_make_writable(videoFrame)) < 0)
//            throw ConvertException(std::string("encode error: av_frame_make_writable video error: ") + av_err2str(ret));

//        if (c->pix_fmt != pFrame->format || targetWidthTmp != pFrame->width || targetHeightTmp != pFrame->height) {
        {
//            __android_log_print(6, "MediaConverter", "convert for scale");
            AVFrame* tempFrame = convertForScale(pFrame);
//            __android_log_print(6, "MediaConverter", "scale");
            tempFrame = scaleVideo(tempFrame);


//            if (sourceRotate.empty()) {
//                videoFrame->pts = av_rescale_q(p, sourceVideoTimebase, c->time_base);
//            __android_log_print(6, "AudioConverter", "video pts: %ld, %ld, %ld, %ld, %d, %d, %d, %d", videoFrame->pts, p, pFrame->pts, firstVideoPts, c->time_base.num, c->time_base.den, sourceVideoTimebase.num, sourceVideoTimebase.den);
//                write_frame(videoCodecContext, videoStream, tempFrame);
//            } else {
//                __android_log_print(6, "AudioConverter", "rotate");
                AVFrame* rotated = rotateVideo(tempFrame);
//                __android_log_print(6, "AudioConverter", "convert");
                AVFrame* converted = convertVideo(rotated);
//                __android_log_print(6, "AudioConverter", "convert end");
                converted->pts = av_rescale_q(p, sourceVideoTimebase, c->time_base);

//            __android_log_print(6, "AudioConverter", "video pts: %ld, %ld, %ld, %ld, %d, %d, %d, %d", videoFrame->pts, p, pFrame->pts, firstVideoPts, c->time_base.num, c->time_base.den, sourceVideoTimebase.num, sourceVideoTimebase.den);
                write_frame(videoCodecContext, videoStream, converted);
//            }

        }
//        else {

//            pFrame->pts = av_rescale_q(p, sourceVideoTimebase, c->time_base);
//            write_frame(videoCodecContext, videoStream, pFrame);
//        }
    }

    AVFrame* convertForScale(AVFrame* pFrame) {
        if (pFrame->format == AV_PIX_FMT_YUV420P)
            return pFrame;

        int ret;
        if (videoFrame == nullptr) {
            videoFrame = alloc_picture(AV_PIX_FMT_YUV420P, pFrame->width, pFrame->height);

            if ((ret = av_frame_make_writable(videoFrame)) < 0)
                throw ConvertException(std::string("encode error: av_frame_make_writable video error: ") + av_err2str(ret));

        }

        if (AV_PIX_FMT_YUV422P == pFrame->format) {
            libyuv::I422ToI420(
                    pFrame->data[0], pFrame->linesize[0],
                    pFrame->data[1], pFrame->linesize[1],
                    pFrame->data[2], pFrame->linesize[2],

                    videoFrame->data[0], videoFrame->linesize[0],
                    videoFrame->data[1], videoFrame->linesize[1],
                    videoFrame->data[2], videoFrame->linesize[2],

                    pFrame->width,
                    pFrame->height
                    );

            return videoFrame;
        }
        else if (AV_PIX_FMT_YUV444P == pFrame->format) {
            libyuv::I444ToI420(
                    pFrame->data[0], pFrame->linesize[0],
                    pFrame->data[1], pFrame->linesize[1],
                    pFrame->data[2], pFrame->linesize[2],

                    videoFrame->data[0], videoFrame->linesize[0],
                    videoFrame->data[1], videoFrame->linesize[1],
                    videoFrame->data[2], videoFrame->linesize[2],

                    pFrame->width,
                    pFrame->height
                    );

            return videoFrame;
        } else if (AV_PIX_FMT_NV12 == pFrame->format) {
            libyuv::NV12ToI420(
                    pFrame->data[0], pFrame->linesize[0],
                    pFrame->data[1], pFrame->linesize[1],

                    videoFrame->data[0], videoFrame->linesize[0],
                    videoFrame->data[1], videoFrame->linesize[1],
                    videoFrame->data[2], videoFrame->linesize[2],

                    pFrame->width,
                    pFrame->height
                    );

            return videoFrame;
        } else if (AV_PIX_FMT_NV21 == pFrame->format) {
            libyuv::NV21ToI420(
                    pFrame->data[0], pFrame->linesize[0],
                    pFrame->data[1], pFrame->linesize[1],

                    videoFrame->data[0], videoFrame->linesize[0],
                    videoFrame->data[1], videoFrame->linesize[1],
                    videoFrame->data[2], videoFrame->linesize[2],

                    pFrame->width,
                    pFrame->height
                    );

            return videoFrame;
        } else if (AV_PIX_FMT_YUYV422 == pFrame->format) {
            libyuv::YUY2ToI420(
                    pFrame->data[0], pFrame->linesize[0],

                    videoFrame->data[0], videoFrame->linesize[0],
                    videoFrame->data[1], videoFrame->linesize[1],
                    videoFrame->data[2], videoFrame->linesize[2],

                    pFrame->width,
                    pFrame->height
            );

            return videoFrame;
        } else if (AV_PIX_FMT_UYVY422 == pFrame->format) {
            libyuv::UYVYToI420(
                    pFrame->data[0], pFrame->linesize[0],

                    videoFrame->data[0], videoFrame->linesize[0],
                    videoFrame->data[1], videoFrame->linesize[1],
                    videoFrame->data[2], videoFrame->linesize[2],

                    pFrame->width,
                    pFrame->height
            );

            return videoFrame;
        }

        /* as we only generate a YUV420P picture, we must convert it
         * to the codec pixel format if needed */
        if (!sws_ctx) {
            sws_ctx = sws_getContext(pFrame->width, pFrame->height,
                                     (enum AVPixelFormat)pFrame->format,
                                     pFrame->width, pFrame->height,
                                     (enum AVPixelFormat)videoFrame->format,
                                     SCALE_FLAGS, nullptr, nullptr, nullptr);
            if (!sws_ctx) {
                throw ConvertException("Could not initialize the sws conversion context");
            }
        }

//            __android_log_print(6, "AudioConverter", "ffmpeg convert");
        sws_scale(sws_ctx, pFrame->data,
                  pFrame->linesize, 0, pFrame->height, videoFrame->data,
                  videoFrame->linesize);

        return videoFrame;
    }

    AVFrame* scaleVideo(AVFrame* pFrame) {
        int ret;
        if (videoFrameScale == nullptr) {
            videoFrameScale = alloc_picture(AV_PIX_FMT_YUV420P, targetWidthTmp, targetHeightTmp);
        }
        if ((ret = av_frame_make_writable(videoFrameScale)) < 0)
            throw ConvertException(std::string("encode error: av_frame_make_writable video scale error: ") + av_err2str(ret));

        libyuv::FilterMode mode = libyuv::kFilterBox;
        libyuv::I420Scale(pFrame->data[0], pFrame->linesize[0],
                           pFrame->data[1], pFrame->linesize[1],
                           pFrame->data[2], pFrame->linesize[2],
                           pFrame->width, pFrame->height,

                          videoFrameScale->data[0], videoFrameScale->linesize[0],
                          videoFrameScale->data[1], videoFrameScale->linesize[1],
                          videoFrameScale->data[2], videoFrameScale->linesize[2],
                          videoFrameScale->width,
                          videoFrameScale->height,
                           mode
        );

        return videoFrameScale;
    }

    /*
     * now we only support AV_PIX_FMT_YUV420P rotate
     */
    AVFrame* rotateVideo(AVFrame *pFrame) {
        if (sourceRotate.empty())
            return pFrame;
        libyuv::RotationModeEnum mode = {};
        if (sourceRotate == "90") {
            mode = libyuv::kRotate90;
        } else if (sourceRotate == "180") {
            mode = libyuv::kRotate180;
        } else if (sourceRotate == "270"){
            mode = libyuv::kRotate270;
        } else {
            return pFrame;
        }

        if (pFrame->format == AV_PIX_FMT_YUV420P) {
            if (videoFrameRotate == nullptr)
                videoFrameRotate = alloc_picture(AV_PIX_FMT_YUV420P, videoCodecContext->width, videoCodecContext->height);

            if ((av_frame_make_writable(videoFrameRotate)) < 0)
                throw ConvertException(std::string("encode error: av_frame_make_writable rotate video error: "));


            libyuv::I420Rotate(pFrame->data[0], pFrame->linesize[0],
                               pFrame->data[1], pFrame->linesize[1],
                               pFrame->data[2], pFrame->linesize[2],

                               videoFrameRotate->data[0], videoFrameRotate->linesize[0],
                               videoFrameRotate->data[1], videoFrameRotate->linesize[1],
                               videoFrameRotate->data[2], videoFrameRotate->linesize[2],

                               pFrame->width,
                               pFrame->height,
                               mode
            );
        } else {
            return pFrame;
        }

        return videoFrameRotate;
    }

    AVFrame* convertVideo(AVFrame* pFrame) {
        if (videoCodecContext->pix_fmt == AV_PIX_FMT_YUV420P)
            return pFrame;

        //we only android mediacodec, on it pix_fmt only can be one of AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12
        if (videoFrameConvert == nullptr) {
            videoFrameConvert = alloc_picture(AV_PIX_FMT_NV12, videoCodecContext->width, videoCodecContext->height);
        }

        if ((av_frame_make_writable(videoFrameConvert)) < 0)
            throw ConvertException(std::string("encode error: av_frame_make_writable convert video error: "));


        libyuv::I420ToNV12(pFrame->data[0], pFrame->linesize[0],
                           pFrame->data[1], pFrame->linesize[1],
                           pFrame->data[2], pFrame->linesize[2],

                           videoFrameConvert->data[0], videoFrameConvert->linesize[0],
                           videoFrameConvert->data[1], videoFrameConvert->linesize[1],
                           pFrame->width,
                           pFrame->height

        );

        return videoFrameConvert;
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
    
    static int computeSize(int s, int max) {
        while (s > max) {
            s = s - 64;
        }
        
        return s;
    }
    
    static int normalizeSize(int s) {
        int y = s % 64;
        if (y == 0)
            return s;
        
        int r = s / 64;
        int ret = 64 * (r - 1);
        if (ret <= 0)
            return 64;
        else
            return ret;
    }
 
    void computeTargetSize() {
        int w = sourceWidth;
        int h = sourceHeight;

        if (w < h) {
            w = sourceHeight;
            h = sourceWidth;
        }

        int cW = computeSize(w, 1080);
        int cH = computeSize(h, 720);

        float scaleW, scaleH;
        scaleW = (float) cW / (float) w;
        scaleH = (float) cH / (float) h;

        float scale = scaleW < scaleH ? scaleW : scaleH;
        targetWidth = normalizeSize((int)(w * scale));
        targetHeight = normalizeSize((int)(h * scale));

        if (sourceWidth < sourceHeight) {
            int temp = targetWidth;
            targetWidth = targetHeight;
            targetHeight = temp;
        }

        targetWidthTmp = targetWidth;
        targetHeightTmp = targetHeight;

        if (sourceRotate == "90" || sourceRotate == "270") {
            int temp = targetWidth;
            targetWidth = targetHeight;
            targetHeight = temp;
        }
    }

    void onInit() override {
        const in::Message& message = in::Message::onInit();
        messageQueue.push(message);

    }

    void doOnInit() {
        int ret;
        ret = avformat_alloc_output_context2(&context, nullptr, format.c_str(), targetPath.c_str());
        if (ret < 0) {
            throw ConvertException(std::string("create target: can't alloc output:") + av_err2str(ret));
        }
    }

    void onAudioStream(AVCodecContext *sourceCodecContext) override {
        const in::Message &message = in::Message::onAudio(sourceCodecContext);
        messageQueue.push(message);
    }

    void doOnAudioStream(const in::Message& msg) {
        sourceSample_rate = msg.sourceSample_rate;
        sourceLayout = msg.sourceLayout;
        sourceChannels = msg.sourceChannels;
        sourceSampleFormat = msg.sourceSampleFormat;
        __android_log_print(6, "AudioConverter", "onAudioStream %d, %d", sourceSample_rate, sourceSampleFormat);
        addAudio();
    }

    void onVideoStream(AVCodecContext *sourceCodecContext, AVStream* st) override {
        const in::Message &message = in::Message::onVideo(sourceCodecContext, st);
        messageQueue.push(message);


    }

    void doOnVideoStream(const in::Message& msg) {
        sourceWidth = msg.sourceWidth;
        sourceHeight = msg.sourceHeight;
        sourceVideoTimebase = msg.sourceVideoTimebase;
        sourceRotate = msg.sourceRotate;
        __android_log_print(6, "AudioConverter", "onVideoStream %d, %d", sourceWidth, sourceHeight);
        addVideo();
    }

    void onStart() override {
        const in::Message &message = in::Message::onStart();
        messageQueue.push(message);
    }

    void doOnStart() {

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
        const in::Message &message = in::Message::onAudioFrame(audioFrame);
        messageQueue.push(message);
//        write_audio_frame_immediate(audioFrame);
//        write_audio_frame(audioFrame);
    }

    void doOnAudioFrame(AVFrame *audioFrame) {
        write_audio_frame(audioFrame);
        av_frame_unref(audioFrame);
        av_frame_free(&audioFrame);
    }

    void onVideoFrame(AVFrame *pFrame) override {
        const in::Message &message = in::Message::onVideoFrame(pFrame);
        messageQueue.push(message);
//        __android_log_print(6, "MediaConverter", "before write video");
//        write_video_frame(pFrame);
//        __android_log_print(6, "MediaConverter", "end write video");
    }

    void doOnVideoFrame(AVFrame *pFrame) {
        write_video_frame(pFrame);
        av_frame_unref(pFrame);
        av_frame_free(&pFrame);
    }

    void onEnd() override {
        const in::Message &message = in::Message::onEnd();
        messageQueue.push(message);

    }

    void doOnEnd() {
        write_audio_frame(nullptr);
        write_video_frame(nullptr);
        end();
    }
    
    void onException(const std::exception &e) override {
        const in::Message &message = in::Message::onException(e);
        messageQueue.push(message);
    }

    void doOnException(const std::exception& e) {

    }
};

MediaConverter::MediaConverter(ProcessCallback* callback, const char *sourcePath, const char *targetPath, const char *format)
        : target(new OutputStream(targetPath, format)),
        inputStream(new InputStream(callback, target.get(), sourcePath)) {

}

MediaConverter::~MediaConverter() noexcept = default;

const char* MediaConverter::convert() {
    try {
//        inputStream->init();
        inputStream->start();
        target->loop();
//        inputStream.reset();
//        target.reset();
    } catch (std::exception& e) {
        __android_log_write(6, "AudioConverter", e.what());
        return e.what();
    }
    inputStream->join();

    return nullptr;
}

void MediaConverter::cancel() {
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
    auto* converter = new MediaConverter(progressCallback, sourcePath, targetPath, formatUTF);

    env->ReleaseStringUTFChars(source, sourcePath);
    env->ReleaseStringUTFChars(target, targetPath);
    env->ReleaseStringUTFChars(format, formatUTF);

    return jlong(converter);
}

static jstring nativeConvert(JNIEnv* env,
                          jobject  /*thzz*/, jlong ptr) {
    auto* converter = reinterpret_cast<MediaConverter*>(ptr);
    const char* error = converter->convert();
    if (error == nullptr)
        return nullptr;

    return env->NewStringUTF(error);
}

static void nativeStop(JNIEnv* /*env*/,
                       jobject  /*thzz*/, jlong ptr) {
    auto* converter = reinterpret_cast<MediaConverter*>(ptr);
    converter->cancel();
}

static void nativeRelease(JNIEnv* /*env*/,
                       jobject  /*thzz*/, jlong ptr) {
    auto* converter = reinterpret_cast<MediaConverter*>(ptr);
    delete converter;
}


static const JNINativeMethod methods[] =
        {
                { "nativeInit",         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)J", ( void* )nativeInit     },
                { "nativeConvert",               "(J)Ljava/lang/String;",     ( void* )nativeConvert           },
                { "nativeStop",            "(J)V",                             ( void* )nativeStop        },
                { "nativeRelease",            "(J)V",                             ( void* )nativeRelease        },
        };

void MediaConverter::initClass(JNIEnv *env, jclass clazz) {
    env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0]));
    JavaProgressCallback::init(env, clazz);
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    jint version = env->GetVersion();
    __android_log_print(6, "MediaConverter", "initClass %d", version);
    YX_AMediaCodec_Enc_loadClassEnv(vm, version);
//    JavaProgressCallback::progressMethod = env->GetMethodID(clazz, "onProgress", "(I)V");
//    if ( env->ExceptionCheck() )
//    {
//        _env->ExceptionDescribe();
//    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mxtech_av_MediaConverter_nativeInitClass(
        JNIEnv* env,
        jclass clazz) {
    MediaConverter::initClass(env, clazz);
}





