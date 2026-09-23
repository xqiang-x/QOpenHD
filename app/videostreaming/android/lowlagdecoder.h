#ifndef LOWLAGDECODER_H
#define LOWLAGDECODER_H


#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <android/log.h>
#include <jni.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>

#include <nalu/NALU.hpp>
#include <nalu/CodecConfigFinder.hpp>

#include "yuv_frame.h"

#include "../../common/TimeHelper.hpp"

struct DecodingInfo{
    std::chrono::steady_clock::time_point lastCalculation=std::chrono::steady_clock::now();
    long nNALU=0;
    long nNALUSFeeded=0;
    long nDecodedFrames=0;
    float currentFPS=0;
    float currentKiloBitsPerSecond=0;
    float avgParsingTime_ms=0;
    float avgWaitForInputBTime_ms=0;
    float avgDecodingTime_ms=0;
    bool operator==(const DecodingInfo& d2)const{
        return nNALU==d2.nNALU && nNALUSFeeded==d2.nNALUSFeeded && currentFPS==d2.currentFPS &&
               currentKiloBitsPerSecond==d2.currentKiloBitsPerSecond && avgParsingTime_ms==d2.avgParsingTime_ms &&
               avgWaitForInputBTime_ms==d2.avgWaitForInputBTime_ms && avgDecodingTime_ms==d2.avgDecodingTime_ms;
    }
    bool operator !=(const DecodingInfo& d2)const{
        return !(*this==d2);
    }
};
struct VideoRatio{
    int width=0;
    int height=0;
    bool operator==(const VideoRatio& b)const{
        return width==b.width && height==b.height;
    }
    bool operator !=(const VideoRatio& b)const{
        return !(*this==b);
    }
};

//Handles decoding of .h264 and .h265 video
// with low latency. Uses the AMediaCodec api
class LowLagDecoder {
private:
    struct Decoder{
        bool configured= false;
        AMediaCodec *codec= nullptr;
        ANativeWindow* window= nullptr;
    };
public:
    //Make sure to do no heavy lifting on this callback, since it is called from the low-latency mCheckOutputThread thread (best to copy values and leave processing to another thread)
    //The decoding info callback is called every DECODING_INFO_RECALCULATION_INTERVAL_MS
    typedef std::function<void(const DecodingInfo)> DECODING_INFO_CHANGED_CALLBACK;
    //The decoder ratio callback is called every time the output format changes
    typedef std::function<void(const VideoRatio)> DECODER_RATIO_CHANGED;
    //Called from the output thread with each decoded frame when running in ByteBuffer mode
    typedef std::function<void(YuvFramePtr)> YUV_FRAME_CALLBACK;
public:
    //We cannot initialize the Decoder until we have SPS and PPS data -
    //when streaming this data will be available at some point in future
    //Therefore we don't allocate the MediaCodec resources here
    LowLagDecoder(JNIEnv* env, bool verboseLogging = false, std::string logTag = "decoder");
    ~LowLagDecoder();
    // This call acquires or releases the output surface
    // After acquiring the surface, the decoder will be started as soon as enough configuration data was passed to it
    // When releasing the surface, the decoder will be stopped if running and any resources will be freed
    // After releasing the surface it is safe for the android os to delete it
    void setOutputSurface(JNIEnv* env,jobject surface);
    // Decode to ByteBuffer (no output Surface) and deliver NV12 frames via the yuv frame callback.
    // Needed on devices where the Surface -> SurfaceTexture path is broken (e.g. MTK color conversion).
    void setByteBufferMode(bool enable);
    //register the specified callbacks. Only one can be registered at a time
    void registerOnDecoderRatioChangedCallback(DECODER_RATIO_CHANGED decoderRatioChangedC);
    void registerOnDecodingInfoChangedCallback(DECODING_INFO_CHANGED_CALLBACK decodingInfoChangedCallback);
    void registerOnYuvFrameCallback(YUV_FRAME_CALLBACK yuvFrameCallback);
    //If the decoder has been configured, feed NALU. Else search for configuration data and
    //configure as soon as possible
    // If the input pipe was closed (surface has been removed or is not set yet), only buffer key frames
    void interpretNALU(const NALU& nalu);
private:
    void releaseDecoderResources(bool releaseWindow);
    void releaseDecoderResourcesLocked(bool releaseWindow);
    //Initialize decoder with SPS / PPS data from KeyFrameFinder
    //Set Decoder.configured to true on success
    void configureStartDecoder();
    //Wait for input buffer to become available before feeding NALU
    void feedDecoder(const NALU& nalu);
    //Runs until EOS arrives at output buffer or decoder is stopped
    void checkOutputLoop();
    //Read the output buffer at index, normalize to tight NV12 and invoke the yuv frame callback
    void deliverYuvFrame(size_t index);
    //Debug log
    void printAvgLog();
    void resetStatistics();
    std::unique_ptr<std::thread> mCheckOutputThread= nullptr;
    bool USE_SW_DECODER_INSTEAD=false;
    // Decode into ByteBuffer instead of an output Surface; frames are delivered via onYuvFrameCallback
    std::atomic<bool> mByteBufferMode{false};
    // Output format of the current codec instance (written/read by the output thread)
    int mOutWidth=0;
    int mOutHeight=0;
    int mOutStride=0;
    int mOutSliceHeight=0;
    int mOutColorFormat=0;
    //Holds the AMediaCodec instance, as well as the state (configured or not configured)
    Decoder decoder{};
    // Set by the feed- or output-thread when the codec becomes unusable (error return,
    // EOS, or not accepting input while data is flowing). The feed thread then releases
    // the codec and re-configures as soon as fresh config data arrives.
    std::atomic<bool> mDecoderError{false};
    std::chrono::steady_clock::time_point mLastFeedSuccess=std::chrono::steady_clock::now();
    // Minimum time between two decoder (re-)configuration attempts to avoid hammering
    // the codec service when the stream carries config data the decoder cannot handle
    std::chrono::steady_clock::time_point mConfigureCooldownUntil{};
    static constexpr auto CONFIGURE_RETRY_COOLDOWN=std::chrono::seconds(1);
    static constexpr auto FEED_STALL_TIMEOUT=std::chrono::seconds(3);
    DecodingInfo decodingInfo;
    // The input pipe is closed until we set a valid surface
    bool inputPipeClosed=true;
    std::mutex mMutexInputPipe;
    DECODER_RATIO_CHANGED onDecoderRatioChangedCallback= nullptr;
    DECODING_INFO_CHANGED_CALLBACK onDecodingInfoChangedCallback= nullptr;
    YUV_FRAME_CALLBACK onYuvFrameCallback= nullptr;
    // So we can temporarily attach the output thread to the vm and make ndk calls
    //JavaVM* javaVm=nullptr;
    std::chrono::steady_clock::time_point lastLog=std::chrono::steady_clock::now();
    RelativeCalculator nDecodedFrames;
    RelativeCalculator nNALUBytesFed;
    AvgCalculator parsingTime;
    AvgCalculator waitForInputB;
    AvgCalculator decodingTime;
    //Every n ms re-calculate the Decoding info
    static const constexpr auto DECODING_INFO_RECALCULATION_INTERVAL=std::chrono::milliseconds(1000);
    const bool m_printDebugInfo;
    const std::string m_logTag;
    static constexpr auto TIME_BETWEEN_LOGS=std::chrono::seconds(5);
    static constexpr int64_t BUFFER_TIMEOUT_US=35*1000; //40ms (a little bit more than 32 ms (==30 fps))
private:
    CodecConfigFinder mKeyFrameFinder;
    bool IS_H265= false;
};

#endif // LOWLAGDECODER_H
