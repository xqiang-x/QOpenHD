#include "lowlagdecoder.h"

#include <unistd.h>
#include <sstream>
#include <vector>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <sys/system_properties.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <media/NdkMediaFormat.h>
#include <utility>

#define MLOGD if(m_printDebugInfo) qDebug()<<"["<<m_logTag.c_str()<<"]"

using namespace std::chrono;

// 诊断用：adb shell setprop debug.qopenhd.yuvprobe 1
// 解码器不配置输出 Surface（ByteBuffer 模式），直接读回解码后的 YUV 字节，
// 用于判定"解码内容是否真的是黑的"（排除显示/采样路径的干扰）
static std::atomic<bool> s_yuvProbe{false};

static std::array<int,2> h264_configureAMediaFormat(CodecConfigFinder& kff, AMediaFormat* format, bool verboseLogging,
                                       const std::string &logTag){
    const auto sps=kff.getCSD0();
    const auto pps=kff.getCSD1();
    const auto videoWH= sps.sps_get_width_height();
    AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_WIDTH,videoWH[0]);
    AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_HEIGHT,videoWH[1]);
    AMediaFormat_setBuffer(format,"csd-0",sps.getData(),(size_t)sps.getSize());
    AMediaFormat_setBuffer(format,"csd-1",pps.getData(),(size_t)pps.getSize());
    if (verboseLogging) {
        qDebug() << "[" << logTag.c_str() << "]" << "Video WH:" << videoWH[0] << " H:" << videoWH[1];
    }
    //AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_BIT_RATE,5*1024*1024);
    //AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_FRAME_RATE,60);
    //AVCProfileBaseline==1
    //AMediaFormat_setInt32(decoder.format,AMEDIAFORMAT_KEY_PROFILE,1);
    //AMediaFormat_setInt32(decoder.format,AMEDIAFORMAT_KEY_PRIORITY,0);
    //writeAndroidPerformanceParams(format);
    return videoWH;
}
static std::array<int,2> h265_configureAMediaFormat(CodecConfigFinder& kff, AMediaFormat* format, bool verboseLogging,
                                       const std::string &logTag){
    std::vector<uint8_t> buff={};
    const auto SPS=kff.getCSD0();
    const auto PPS=kff.getCSD1();
    const auto VPS=kff.getVPS();
    buff.reserve(SPS.getSize()+PPS.getSize()+VPS.getSize());
    CodecConfigFinder::appendNaluData(buff,VPS);
    CodecConfigFinder::appendNaluData(buff,SPS);
    CodecConfigFinder::appendNaluData(buff,PPS);
    const auto videoWH= std::array<int,2>{1280,720};
    AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_WIDTH,videoWH[0]);
    AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_HEIGHT,videoWH[1]);
    AMediaFormat_setBuffer(format,"csd-0",buff.data(),buff.size());
    if (verboseLogging) {
        qDebug() << "[" << logTag.c_str() << "]" << "Video WH:" << videoWH[0] << " H:" << videoWH[1];
    }
    //writeAndroidPerformanceParams(format);
    return videoWH;
}

LowLagDecoder::LowLagDecoder(JNIEnv* env, bool verboseLogging, std::string logTag)
    : m_printDebugInfo(verboseLogging), m_logTag(std::move(logTag)){
    //env->GetJavaVM(&javaVm);
    resetStatistics();
}

LowLagDecoder::~LowLagDecoder()
{
    // Joins the output thread and stops the codec if it is still running
    releaseDecoderResources(true);
}

void LowLagDecoder::releaseDecoderResources(bool releaseWindow)
{
    std::lock_guard<std::mutex> lock(mMutexInputPipe);
    releaseDecoderResourcesLocked(releaseWindow);
}

void LowLagDecoder::releaseDecoderResourcesLocked(bool releaseWindow)
{
    inputPipeClosed=true;
    if(decoder.configured){
        if (decoder.codec) {
            AMediaCodec_stop(decoder.codec);
            AMediaCodec_delete(decoder.codec);
            decoder.codec=nullptr;
        }
        mKeyFrameFinder.reset();
        decoder.configured=false;
        if(mCheckOutputThread && mCheckOutputThread->joinable()){
            mCheckOutputThread->join();
        }
        mCheckOutputThread.reset();
        mOutWidth=0;
        mOutHeight=0;
        mOutStride=0;
        mOutSliceHeight=0;
        mOutColorFormat=0;
    }
    if (releaseWindow && decoder.window) {
        ANativeWindow_release(decoder.window);
        decoder.window=nullptr;
    }
    resetStatistics();
    if (!releaseWindow && (decoder.window || mByteBufferMode.load())) {
        // keep decoding (and re-configure as soon as config data arrives again)
        inputPipeClosed=false;
    }
}

void LowLagDecoder::setOutputSurface(JNIEnv* env,jobject surface){
    USE_SW_DECODER_INSTEAD=false;

    std::lock_guard<std::mutex> lock(mMutexInputPipe);

    if(surface==nullptr){
        // No surface: this is also the cleanup path for ByteBuffer mode, where a
        // running codec has configured==true but window==nullptr
        if(decoder.window== nullptr && !decoder.configured){
            return;
        }
        releaseDecoderResourcesLocked(true);
        return;
    }

    if(decoder.window!=nullptr){
        releaseDecoderResourcesLocked(true);
    }

    mByteBufferMode.store(false);
    decoder.window=ANativeWindow_fromSurface(env,surface);
    // open the input pipe - now the decoder will start as soon as enough data is available
    inputPipeClosed=false;
}

void LowLagDecoder::setByteBufferMode(bool enable)
{
    std::lock_guard<std::mutex> lock(mMutexInputPipe);
    // A running codec was configured for the other output mode - release it so it
    // gets re-configured with the requested mode once config data arrives again
    if(decoder.configured || decoder.window!=nullptr){
        releaseDecoderResourcesLocked(true);
    }
    mByteBufferMode.store(enable);
    if(enable){
        // open the input pipe - the decoder starts as soon as enough data is available
        inputPipeClosed=false;
    }
}

void LowLagDecoder::registerOnYuvFrameCallback(YUV_FRAME_CALLBACK yuvFrameCallback){
    onYuvFrameCallback=std::move(yuvFrameCallback);
}

void LowLagDecoder::registerOnDecoderRatioChangedCallback(DECODER_RATIO_CHANGED decoderRatioChangedC) {
    onDecoderRatioChangedCallback=std::move(decoderRatioChangedC);
}

void LowLagDecoder::registerOnDecodingInfoChangedCallback(DECODING_INFO_CHANGED_CALLBACK decodingInfoChangedCallback){
    onDecodingInfoChangedCallback=std::move(decodingInfoChangedCallback);
}

void LowLagDecoder::interpretNALU(const NALU& nalu){
    //return;
    // TODO: RN switching between h264 / h265 requires re-setting the surface
    std::lock_guard<std::mutex> lock(mMutexInputPipe);
    if(decoder.configured && mDecoderError.exchange(false)){
        // The codec died (error return / EOS / stalled). Drop it and wait for fresh
        // config data (SPS/PPS arrive with every key frame), then configure anew.
        qWarning() << "LowLagDecoder: decoder became unusable, releasing for recovery";
        releaseDecoderResourcesLocked(false);
        mDecoderError.store(false);
        mConfigureCooldownUntil=std::chrono::steady_clock::now()+CONFIGURE_RETRY_COOLDOWN;
    }
    if(decoder.configured && nalu.IS_H265_PACKET!=IS_H265){
        MLOGD << "Codec change detected while decoder is configured; restarting decoder";
        releaseDecoderResourcesLocked(false);
    }
    IS_H265=nalu.IS_H265_PACKET;
    //MLOGD<<"Is H265 "<<nalu.IS_H265_PACKET;
    //MLOGD<<"NALU size "<<StringHelper::memorySizeReadable(nalu.getSize());
    //MLOGD<<"NALU type "<<nalu.get_nal_name();
    //nalu.debug();
    //MLOGD<<"DATA:"<<nalu.dataAsString();
    //return;
    decodingInfo.nNALU++;
    if(nalu.getSize()<=4){
        //No data in NALU (e.g at the beginning of a stream)
        return;
    }
    //if(nalu.get_nal_unit_type()==NAL_UNIT_TYPE_AUD){
    //    return;
    //}
    nNALUBytesFed.add(nalu.getSize());
    if(inputPipeClosed){
        //A feedD thread (e.g. file or udp) thread might be running even tough no output surface was set
        //But at least we can buffer the sps/pps data
        mKeyFrameFinder.save_if_config(nalu);
        return;
    }
    if(decoder.configured){
        feedDecoder(nalu);
        decodingInfo.nNALUSFeeded++;
        // manually feeding AUDs doesn't seem to change anything for high latency streams
        // Only for the x264 sw encoded example stream it might improve latency slightly
        //if(!nalu.IS_H265_PACKET && nalu.get_nal_unit_type()==NAL_UNIT_TYPE_CODED_SLICE_NON_IDR){
            //MLOGD<<"Feeding special AUD";
            //feedDecoder(NALU::createExampleH264_AUD());
        //}
    }else{
        //Store sps,pps, vps(H265 only)
        // As soon as enough data has been buffered to initialize the decoder,do so.
        mKeyFrameFinder.save_if_config(nalu);
        if(mKeyFrameFinder.all_config_available(IS_H265) && std::chrono::steady_clock::now()>=mConfigureCooldownUntil){
            configureStartDecoder();
        }
    }
}

void LowLagDecoder::configureStartDecoder(){
    const std::string MIME=IS_H265 ? "video/hevc" : "video/avc";
    // 诊断用：adb shell setprop debug.qopenhd.swdecoder 1 可强制走软件解码器
    {
        char swBuf[PROP_VALUE_MAX] = {0};
        USE_SW_DECODER_INSTEAD=(__system_property_get("debug.qopenhd.swdecoder", swBuf) > 0 && atoi(swBuf) != 0);
        if(USE_SW_DECODER_INSTEAD){
            qInfo() << "LowLagDecoder: forcing SW decoder via property";
        }
    }
    // 诊断用：YUV 探针，无 Surface 配置解码器并直接读回 YUV 字节
    {
        char yuvBuf[PROP_VALUE_MAX] = {0};
        s_yuvProbe.store(__system_property_get("debug.qopenhd.yuvprobe", yuvBuf) > 0 && atoi(yuvBuf) != 0);
        if(s_yuvProbe.load()){
            qInfo() << "LowLagDecoder: YUV probe mode, decoder configured WITHOUT surface";
        }
    }
    if(USE_SW_DECODER_INSTEAD){
        if(IS_H265){
            // Not sure if google.hevc.decoder is even SW ?!
            decoder.codec = AMediaCodec_createCodecByName("OMX.google.hevc.decoder");
        }else{
            decoder.codec = AMediaCodec_createCodecByName("OMX.google.h264.decoder");
        }
    }else {
        decoder.codec = AMediaCodec_createDecoderByType(MIME.c_str());
        //decoder.codec = AMediaCodec_createDecoderByType("video/mjpeg");
        //const std::string s=(decoder.codec== nullptr ? "No" : "YES");
        //MDebug::log("Created decoder"+s);
        //char* name;
        //AMediaCodec_getName(decoder.codec,&name);
        //MLOGD<<"Created decoder "<<std::string(name);
        //AMediaCodec_releaseName(decoder.codec,name);
    }
    if (decoder.codec== nullptr) {
        MLOGD<<"Cannot create decoder";
        //set csd-0 and csd-1 back to 0, maybe they were just faulty but we have better luck with the next ones
        mKeyFrameFinder.reset();
        return;
    }
    AMediaFormat* format=AMediaFormat_new();
    AMediaFormat_setString(format,AMEDIAFORMAT_KEY_MIME,MIME.c_str());
    std::array<int,2> videoWH{};
    if(IS_H265){
        videoWH=h265_configureAMediaFormat(mKeyFrameFinder, format, m_printDebugInfo, m_logTag);
    }else{
        videoWH=h264_configureAMediaFormat(mKeyFrameFinder, format, m_printDebugInfo, m_logTag);
        //mKeyFrameFinder.h264_configureAMediaFormat(format);
    }
    // 输出窗口(BufferQueue)的默认几何是 SurfaceTexture 创建时的 1x1。MTK 等平台的
    // OMX 解码器按此默认几何分配/校验色彩转换目标缓冲，导致 1280x720 的帧无法写入
    // （"PrepareAvaliableColorConvertBuffer, no free buffer can't queue to CCDst"），
    // 输出缓冲始终不回收、画面全黑。配置解码器前显式设置缓冲几何，使其一次分配正确。
    // format 传 0 = 保持窗口原本的格式，只改尺寸。
    if(decoder.window!=nullptr && videoWH[0]>0 && videoWH[1]>0){
        // 诊断用：确认几何设置是否真的生效（读回值）
        qInfo() << "LowLagDecoder: window geometry before set:"
                << ANativeWindow_getWidth(decoder.window) << "x" << ANativeWindow_getHeight(decoder.window)
                << "fmt=" << ANativeWindow_getFormat(decoder.window);
        const auto geoResult=ANativeWindow_setBuffersGeometry(decoder.window,videoWH[0],videoWH[1],0);
        qInfo() << "LowLagDecoder: window geometry after set (want"
                << videoWH[0] << "x" << videoWH[1] << "):"
                << ANativeWindow_getWidth(decoder.window) << "x" << ANativeWindow_getHeight(decoder.window)
                << "fmt=" << ANativeWindow_getFormat(decoder.window)
                << "ret=" << geoResult;
        if(geoResult!=0){
            qWarning() << "LowLagDecoder: ANativeWindow_setBuffersGeometry failed, result=" << geoResult;
        }
    }

    MLOGD << "Configuring decoder:" << AMediaFormat_toString(format);

    // ByteBuffer 模式（YUV 探针或正常渲染走 YUV 纹理）不配置输出 Surface，
    // 解码输出由 checkOutputLoop 读回并归一化后交给上层
    const bool useByteBuffer = mByteBufferMode.load() || s_yuvProbe.load();
    const auto configureResult=AMediaCodec_configure(decoder.codec,format, useByteBuffer ? nullptr : decoder.window, nullptr, 0);
    AMediaFormat_delete(format);
    format=AMediaCodec_getOutputFormat(decoder.codec);
    //MLOGD<<"Output format"<<AMediaFormat_toString(format);
    AMediaFormat_delete(format);

    if (configureResult!=AMEDIA_OK) {
        //e.g. the CSD (SPS/PPS) carries a profile this decoder does not support
        qWarning() << "LowLagDecoder: configure failed, result=" << (int)configureResult;
        AMediaCodec_delete(decoder.codec);
        decoder.codec=nullptr;
        //set csd-0 and csd-1 back to 0, maybe they were just faulty but we have better luck with the next ones
        mKeyFrameFinder.reset();
        mConfigureCooldownUntil=std::chrono::steady_clock::now()+CONFIGURE_RETRY_COOLDOWN;
        return;
    }
    const auto startResult=AMediaCodec_start(decoder.codec);
    if (startResult!=AMEDIA_OK) {
        qWarning() << "LowLagDecoder: start failed, result=" << (int)startResult;
        AMediaCodec_delete(decoder.codec);
        decoder.codec=nullptr;
        mKeyFrameFinder.reset();
        mConfigureCooldownUntil=std::chrono::steady_clock::now()+CONFIGURE_RETRY_COOLDOWN;
        return;
    }
    mDecoderError.store(false);
    mLastFeedSuccess=std::chrono::steady_clock::now();
    mCheckOutputThread=std::make_unique<std::thread>(&LowLagDecoder::checkOutputLoop,this);
    //NDKThreadHelper::setName(mCheckOutputThread->native_handle(),"LLDCheckOutput");
    decoder.configured=true;
}


void LowLagDecoder::feedDecoder(const NALU& nalu){
    if(IS_H265 && (nalu.isSPS() || nalu.isPPS() || nalu.isVPS())){
        // looks like h265 doesn't like feeding sps/pps/vps during decoding
        // it could also be that they have to be merged together, but for now just skip them
        return;
    }
    // Hm, for some reason not feeding AUDs to the decoder increases latency for the x264/testVideo.h264 file
    //if(nalu.isAUD()){
    //    return;
    //}
    const auto now=std::chrono::steady_clock::now();
    const auto deltaParsing=now-nalu.creationTime;
    while(true){
        const auto index=AMediaCodec_dequeueInputBuffer(decoder.codec,BUFFER_TIMEOUT_US);
        if (index >=0) {
            size_t inputBufferSize;
            void* buf = AMediaCodec_getInputBuffer(decoder.codec,(size_t)index,&inputBufferSize);
            // I have not seen any case where the input buffer returned by MediaCodec is too small to hold the NALU
            // But better be safe than crashing with a memory exception
            if(nalu.getSize()>inputBufferSize){
                MLOGD<<"Nalu too big"<<nalu.getSize();
                return;
            }
            std::memcpy(buf, nalu.getData(),(size_t)nalu.getSize());
            //this timestamp will be later used to calculate the decoding latency
            const uint64_t presentationTimeUS=(uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
            //Doing so causes garbage bug TODO investigate
            //const auto flag=nalu.isPPS() || nalu.isSPS() ? AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG : 0;
            //AMediaCodec_queueInputBuffer(decoder.codec, (size_t)index, 0, (size_t)nalu.data_length,presentationTimeUS, flag);
            const auto feedResult=AMediaCodec_queueInputBuffer(decoder.codec, (size_t)index, 0, (size_t)nalu.getSize(),presentationTimeUS,0);
            if(feedResult!=AMEDIA_OK){
                qWarning() << "LowLagDecoder: queueInputBuffer failed, result=" << (int)feedResult;
                mDecoderError.store(true);
                return;
            }
            mLastFeedSuccess=steady_clock::now();
            waitForInputB.add(steady_clock::now() - now);
            parsingTime.add(deltaParsing);
            // 诊断用：确认喂给解码器的 NALU 大小与帧率（区分"没有数据"与"数据在但解不出"）
            static std::atomic<int> s_fedCount{0};
            const int fedN=++s_fedCount;
            if(fedN<=3 || fedN%120==0){
                qInfo() << "LowLagDecoder: fed#" << fedN << " nalu_size=" << (int)nalu.getSize();
            }
            return;
        }else if(index==AMEDIACODEC_INFO_TRY_AGAIN_LATER){
            //just try again. But if we had no success in the last 1 second,log a warning and return.
            const auto elapsedTimeTryingForBuffer=std::chrono::steady_clock::now()-now;
            if(elapsedTimeTryingForBuffer>std::chrono::seconds(1)){
                // Since OpenHD provides a lossy link it is really unlikely, but possible that we somehow 'break' the codec by feeding corrupt data.
                // It will probably recover itself as soon as we feed enough valid data though;
                // But if the codec has not accepted anything for a long time while data keeps
                // flowing, it is wedged - treat like an error so the caller can reconfigure it.
                if(steady_clock::now()-mLastFeedSuccess>FEED_STALL_TIMEOUT){
                    qWarning() << "LowLagDecoder: codec not accepting input, treating as error";
                    mDecoderError.store(true);
                }
                return;
            }
        }else{
            //A negative return that is not TRY_AGAIN means the codec is unusable (e.g. it moved
            //to the released state after an internal error) - keep feeding it would only spam errors
            qWarning() << "LowLagDecoder: dequeueInputBuffer failed, result=" << (int)index;
            mDecoderError.store(true);
            return;
        }
    }
}

void LowLagDecoder::checkOutputLoop() {
    //NDKThreadHelper::setProcessThreadPriorityAttachDetach(javaVm,FPV_VR_PRIORITY::CPU_PRIORITY_DECODER_OUTPUT,"DecoderCheckOutput");
    AMediaCodecBufferInfo info;
    bool decoderSawEOS=false;
    bool decoderProducedUnknown=false;
    // 诊断用：区分"真帧"与空输出。MTK 卡在色彩转换时仍会按帧率吐出 size=0 的输出，
    // 只数 dequeue 成功次数会得到假的 30fps。
    int64_t nOutputsTotal=0;
    int64_t nOutputsWithData=0;
    while(!decoderSawEOS && !decoderProducedUnknown) {
        const ssize_t index=AMediaCodec_dequeueOutputBuffer(decoder.codec,&info,BUFFER_TIMEOUT_US);
        if (index >= 0) {
            nOutputsTotal++;
            if (info.size > 0) {
                nOutputsWithData++;
            }
            if (nOutputsTotal <= 8) {
                qInfo() << "LowLagDecoder: output#" << (long long)nOutputsTotal << "idx=" << (int)index
                        << "size=" << info.size << "flags=" << info.flags
                        << "pts=" << (long long)info.presentationTimeUs;
            }
            // 诊断用：YUV 探针——直接统计解码输出 Y 平面的像素分布
            // （全 0/全 16 = 解码内容就是黑的；有明显亮像素 = 解码内容正常，问题在显示链路）
            if(s_yuvProbe.load() && info.size>0 && (nOutputsTotal<=5 || nOutputsTotal%60==0)){
                size_t outSize=0;
                uint8_t* obuf=AMediaCodec_getOutputBuffer(decoder.codec,(size_t)index,&outSize);
                if(obuf!=nullptr && outSize>0){
                    int64_t nz=0,nbright=0;
                    uint8_t vmin=255,vmax=0;
                    const size_t n=std::min<size_t>(outSize,(size_t)1024*1024);
                    for(size_t i=0;i<n;i++){
                        const uint8_t v=obuf[i];
                        if(v) nz++;
                        if(v>128) nbright++;
                        vmin=std::min(vmin,v);
                        vmax=std::max(vmax,v);
                    }
                    qInfo() << "LowLagDecoder: YUVPROBE out#" << (long long)nOutputsTotal
                            << "infoSize=" << info.size << "bufSize=" << (long long)outSize
                            << "nz=" << (long long)nz << "/" << (long long)n
                            << "bright=" << (long long)nbright
                            << "min=" << (int)vmin << "max=" << (int)vmax;
                }else{
                    qInfo() << "LowLagDecoder: YUVPROBE out#" << (long long)nOutputsTotal
                            << "getOutputBuffer returned null/size0";
                }
            }
            const auto now=steady_clock::now();
            const int64_t nowUS=(int64_t)duration_cast<microseconds>(now.time_since_epoch()).count();
            //the timestamp for releasing the buffer is in NS, just release as fast as possible (e.g. now)
            //https://android.googlesource.com/platform/frameworks/av/+/master/media/ndk/NdkMediaCodec.cpp
            //-> renderOutputBufferAndRelease which is in https://android.googlesource.com/platform/frameworks/av/+/3fdb405/media/libstagefright/MediaCodec.cpp
            //-> Message kWhatReleaseOutputBuffer -> onReleaseOutputBuffer
            // also https://android.googlesource.com/platform/frameworks/native/+/5c1139f/libs/gui/SurfaceTexture.cpp
            // Android 11+ (codec2) is stricter about buffer ownership tracking and
            // issuing AMediaCodec_releaseOutputBufferAtTime() with a "render now"
            // timestamp has been observed to occasionally trigger
            // "Client returned a buffer it does not own" warnings. Releasing the
            // buffer with an explicit render flag keeps the codec bookkeeping
            // consistent across devices.
            // ByteBuffer 渲染模式：先把解码输出读回并归一化为紧致 NV12 交给上层，
// 再释放输出缓冲（Surface 模式下则由 releaseOutputBuffer 直接呈现）
            const bool byteBufferOutput = mByteBufferMode.load() || s_yuvProbe.load();
            if(mByteBufferMode.load() && info.size > 0 && onYuvFrameCallback != nullptr){
                deliverYuvFrame((size_t)index);
            }
            const bool renderFrame = info.size > 0 && decoder.window != nullptr && !byteBufferOutput;
            AMediaCodec_releaseOutputBuffer(decoder.codec, (size_t) index, renderFrame);
            //but the presentationTime is in US
            decodingTime.add(std::chrono::microseconds(nowUS - info.presentationTimeUs));
            nDecodedFrames.add(1);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                MLOGD<<"Decoder saw EOS";
                decoderSawEOS=true;
                mDecoderError.store(true);
                continue;
            }
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ) {
            auto format = AMediaCodec_getOutputFormat(decoder.codec);
            int width=0,height=0;
            AMediaFormat_getInt32(format,AMEDIAFORMAT_KEY_WIDTH,&width);
            AMediaFormat_getInt32(format,AMEDIAFORMAT_KEY_HEIGHT,&height);
            int stride=0,sliceHeight=0,colorFormat=0;
            // These keys are not exposed via all NDK headers, but the key strings are stable
            AMediaFormat_getInt32(format,"stride",&stride);
            AMediaFormat_getInt32(format,"slice-height",&sliceHeight);
            AMediaFormat_getInt32(format,AMEDIAFORMAT_KEY_COLOR_FORMAT,&colorFormat);
            if(stride<=0) stride=width;
            if(sliceHeight<=0) sliceHeight=height;
            mOutWidth=width;
            mOutHeight=height;
            mOutStride=stride;
            mOutSliceHeight=sliceHeight;
            mOutColorFormat=colorFormat;
            MLOGD<<"Actual Width and Height in output "<<width<<","<<height;
            qInfo() << "LowLagDecoder: output format width=" << width << "height=" << height
                    << "stride=" << stride << "sliceHeight=" << sliceHeight
                    << "colorFormat=" << colorFormat;
            if(onDecoderRatioChangedCallback!= nullptr && width != 0 && height != 0){
                onDecoderRatioChangedCallback({width, height});
            }
            MLOGD << "AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED " << width << " " << height << " " << AMediaFormat_toString(format);
            AMediaFormat_delete(format);
        } else if(index==AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED){
            MLOGD<<"AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED";
        } else if(index==AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            //MLOGD<<"AMEDIACODEC_INFO_TRY_AGAIN_LATER";
        } else {
            // Most like AMediaCodec_stop() was called, or the codec died internally
            MLOGD<<"dequeueOutputBuffer idx: "<<(int)index<<" .Exit.";
            decoderProducedUnknown=true;
            mDecoderError.store(true);
            continue;
        }
        //every 2 seconds recalculate the current fps and bitrate
        const auto now=steady_clock::now();
        const auto delta=now-decodingInfo.lastCalculation;
        if(delta>DECODING_INFO_RECALCULATION_INTERVAL){
            decodingInfo.lastCalculation=steady_clock::now();
            decodingInfo.currentFPS=(float)nDecodedFrames.getDeltaSinceLastCall()/(float)duration_cast<seconds>(delta).count();
            decodingInfo.currentKiloBitsPerSecond=((float)nNALUBytesFed.getDeltaSinceLastCall()/duration_cast<seconds>(delta).count())/1024.0f*8.0f;
            //and recalculate the avg latencies. If needed,also print the log.
            decodingInfo.avgDecodingTime_ms=decodingTime.getAvg_ms();
            decodingInfo.avgParsingTime_ms=parsingTime.getAvg_ms();
            decodingInfo.avgWaitForInputBTime_ms=waitForInputB.getAvg_ms();
            decodingInfo.nDecodedFrames=nDecodedFrames.getAbsolute();
            // 诊断用：无论 verbose 与否都打印输出侧状态（排除"App 未取走解码输出"）
            const QString winStr = decoder.window != nullptr
                ? QString("%1x%2").arg(ANativeWindow_getWidth(decoder.window)).arg(ANativeWindow_getHeight(decoder.window))
                : QStringLiteral("null");
            qInfo() << "LowLagDecoder: out-frames=" << decodingInfo.nDecodedFrames
                    << "fps=" << decodingInfo.currentFPS
                    << "win=" << winStr
                    << "outs=" << (long long)nOutputsTotal
                    << "data=" << (long long)nOutputsWithData
                    << "avgDecode_ms=" << decodingInfo.avgDecodingTime_ms
                    << "err=" << mDecoderError.load();
            printAvgLog();
            if(onDecodingInfoChangedCallback!= nullptr){
                onDecodingInfoChangedCallback(decodingInfo);
            }
        }
    }
    MLOGD<<"Exit CheckOutputLoop";
}

void LowLagDecoder::deliverYuvFrame(size_t index)
{
    size_t outSize=0;
    uint8_t* obuf=AMediaCodec_getOutputBuffer(decoder.codec,index,&outSize);
    if(obuf==nullptr || outSize==0){
        return;
    }
    const int w=mOutWidth;
    const int h=mOutHeight;
    if(w<=0 || h<=0){
        // OUTPUT_FORMAT_CHANGED always arrives before the first buffer with data,
        // so this is unexpected - drop the frame instead of guessing the geometry
        static std::atomic<bool> s_noFormatLogged{false};
        if(!s_noFormatLogged.exchange(true)){
            qWarning() << "LowLagDecoder: yuv frame without output format, dropping frames";
        }
        return;
    }
    const int stride=mOutStride>0?mOutStride:w;
    const int sliceHeight=mOutSliceHeight>0?mOutSliceHeight:h;
    const int uvW=w/2;
    const int uvH=h/2;

    auto frame=std::make_shared<YuvFrame>();
    frame->width=w;
    frame->height=h;
    frame->data.resize((size_t)w*h + (size_t)uvW*uvH*2);
    uint8_t* dstY=frame->data.data();
    uint8_t* dstUV=dstY+(size_t)w*h;

    // Y 平面：逐行拷贝，跳过 stride 填充
    bool ok=true;
    for(int y=0;y<h && ok;y++){
        const size_t srcOff=(size_t)y*stride;
        if(srcOff+(size_t)w>outSize){
            ok=false;
            break;
        }
        std::memcpy(dstY+(size_t)y*w,obuf+srcOff,(size_t)w);
    }
    if(ok){
        // 19 = YUV420Planar (I420, U 平面在前), 20 = YUV420PackedPlanar (YV12, V 平面在前),
        // 21 = YUV420SemiPlanar (NV12, U/V 交错) - 统一归一化为 NV12
        if(mOutColorFormat==21){
            const size_t uvOff=(size_t)stride*sliceHeight;
            for(int y=0;y<uvH && ok;y++){
                const size_t srcOff=uvOff+(size_t)y*stride;
                if(srcOff+(size_t)w>outSize){
                    ok=false;
                    break;
                }
                std::memcpy(dstUV+(size_t)y*w,obuf+srcOff,(size_t)w);
            }
        }else if(mOutColorFormat==19 || mOutColorFormat==20){
            const size_t uOff=(size_t)stride*sliceHeight;
            const size_t vOff=uOff+(size_t)(stride/2)*(sliceHeight/2);
            const size_t uPlane=(mOutColorFormat==19)?uOff:vOff;
            const size_t vPlane=(mOutColorFormat==19)?vOff:uOff;
            for(int y=0;y<uvH && ok;y++){
                const size_t rowOff=(size_t)y*(stride/2);
                if(uPlane+rowOff+(size_t)uvW>outSize || vPlane+rowOff+(size_t)uvW>outSize){
                    ok=false;
                    break;
                }
                const uint8_t* uRow=obuf+uPlane+rowOff;
                const uint8_t* vRow=obuf+vPlane+rowOff;
                uint8_t* dstRow=dstUV+(size_t)y*w;
                for(int x=0;x<uvW;x++){
                    dstRow[x*2]=uRow[x];
                    dstRow[x*2+1]=vRow[x];
                }
            }
        }else{
            // e.g. vendor proprietary tiled formats - layout unknown, cannot normalize
            static std::atomic<int> s_unknownFormat{0};
            if(s_unknownFormat.fetch_add(1)==0){
                qWarning() << "LowLagDecoder: unsupported output color format" << mOutColorFormat
                           << "- cannot deliver yuv frames";
            }
            ok=false;
        }
    }
    if(!ok){
        return;
    }
    const auto cb=onYuvFrameCallback;
    if(cb!=nullptr){
        cb(std::move(frame));
    }
}

void LowLagDecoder::printAvgLog() {
    if(!m_printDebugInfo){
        return;
    }
    auto now=steady_clock::now();
    if((now-lastLog)>TIME_BETWEEN_LOGS){
        lastLog=now;
        std::ostringstream frameLog;
        frameLog<<std::fixed;
        float avgDecodingLatencySum=decodingInfo.avgParsingTime_ms+decodingInfo.avgWaitForInputBTime_ms+
                                    decodingInfo.avgDecodingTime_ms;
        frameLog<<"......................Decoding Latency Averages......................"<<
                "\nParsing:"<<decodingInfo.avgParsingTime_ms
                <<" | WaitInputBuffer:"<<decodingInfo.avgWaitForInputBTime_ms
                <<" | Decoding:"<<decodingInfo.avgDecodingTime_ms
                <<" | Decoding Latency Sum:"<<avgDecodingLatencySum<<
                "\nN NALUS:"<<decodingInfo.nNALU
                <<" | N NALUES feeded:" <<decodingInfo.nNALUSFeeded<<" | N Decoded Frames:"<<nDecodedFrames.getAbsolute()<<
                "\nFPS:"<<decodingInfo.currentFPS;
        MLOGD<<frameLog.str().c_str();
    }
}

void LowLagDecoder::resetStatistics() {
    nDecodedFrames.reset();
    nNALUBytesFed.reset();
    parsingTime.reset();
    waitForInputB.reset();
    decodingTime.reset();
    decodingInfo={};
}
