#include "qandroidmediaplayer.h"
#include "qsurfacetexture.h"

#include <QAndroidJniEnvironment>
#include <QDebug>

#include <cstdlib>
#include <sys/system_properties.h>

#include <decodingstatistcs.h>

#include "../../rtc/RtcClient.h"

// 视频渲染模式：默认 YUV（ByteBuffer 解码 + Y/UV 纹理上传，见 qsurfacetexture.cpp），
// adb shell setprop debug.qopenhd.surfacevideo 1 可切回 legacy Surface/SurfaceTexture 模式
static bool qopenhd_yuv_video_mode(){
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.qopenhd.surfacevideo", buf) > 0) {
        return atoi(buf) == 0;
    }
    return true;
}

QAndroidMediaPlayer::QAndroidMediaPlayer(QObject *parent)
    : QObject(parent)
{
    const auto settings = QOpenHDVideoHelper::read_config_from_settings();
    m_primaryVideoEnabled = !settings.generic.dev_disable_primary_video;
    if (!m_primaryVideoEnabled) {
        qInfo() << "Primary Android video disabled in settings; skipping decoder initialization";
        return;
    }
    setup_start_video_decoder_display(settings);
}

void QAndroidMediaPlayer::setup_start_video_decoder_display(const QOpenHDVideoHelper::VideoStreamConfig &config)
{
    // Everything should be cleaned up
    assert(m_low_lag_decoder==nullptr);
    assert(m_receiver==nullptr);
    assert(m_rtc_receiver==nullptr);
    m_low_lag_decoder=std::make_unique<LowLagDecoder>(nullptr, false, "primary");
    auto stats_cb=[](const DecodingInfo di){
        std::stringstream ss;
        ss<<di.avgDecodingTime_ms<<"ms";
        DecodingStatistcs::instance().set_decode_time(ss.str().c_str());
    };
    m_low_lag_decoder->registerOnDecodingInfoChangedCallback(stats_cb);
    auto ratio_changed_cb=[this](const VideoRatio ratio){
        DecodingStatistcs::instance().util_set_primary_stream_frame_format("android",ratio.width,ratio.height);
        if(m_videoOut){
            m_videoOut->set_video_texture_size(ratio.width,ratio.height);
        }
    };
    m_low_lag_decoder->registerOnDecoderRatioChangedCallback(ratio_changed_cb);
    auto codec=config.primary_stream_config.video_codec;
    if (RtcClient::instance().is_enabled()) {
        // RTC 直连 SFU：不占用 RTP 端口，视频帧由 RtcClient 网络线程提供
        qInfo() << "QAndroidMediaPlayer: RTC direct link enabled, using RtcReceiver";
        m_rtc_receiver=std::make_unique<RtcReceiver>(codec);
    } else {
        const int port = config.generic.qopenhd_switch_primary_secondary ? 5601 : 5600;
        m_receiver=std::make_unique<GstRtpReceiver>(port,codec);
    }
}

void QAndroidMediaPlayer::stop_cleanup_decoder_display()
{
    // first, we stop the (rtp or rtc) receiver
    if (m_receiver) {
        m_receiver->stop_receiving();
        m_receiver=nullptr;
    }
    if (m_rtc_receiver) {
        m_rtc_receiver->stop_receiving();
        m_rtc_receiver=nullptr;
    }
    // Then we can safely clean up the decoder (and its surface)
    if (m_low_lag_decoder) {
        m_low_lag_decoder->setOutputSurface(nullptr,nullptr);
        m_low_lag_decoder=nullptr;
    }
}

QAndroidMediaPlayer::~QAndroidMediaPlayer()
{
    /*QAndroidJniEnvironment env;
    m_mediaPlayer.callMethod<void>("stop");
    m_mediaPlayer.callMethod<void>("reset");
    m_mediaPlayer.callMethod<void>("release");*/
    //m_mediaPlayer.callMethod<void>("releaseAll");

    stop_cleanup_decoder_display();
}

QSurfaceTexture *QAndroidMediaPlayer::videoOut() const
{
    return m_videoOut;
}

void QAndroidMediaPlayer::setVideoOut(QSurfaceTexture *videoOut)
{
    if (m_videoOut == videoOut)
        return;
    if (m_videoOut)
        m_videoOut->disconnect(this);
    m_videoOut = videoOut;
    if (!m_primaryVideoEnabled) {
        qInfo() << "Primary Android video is disabled; not attaching video surface";
        emit videoOutChanged();
        return;
    }

    auto startReceiving=[this](){
        if ((!m_receiver && !m_rtc_receiver) || !m_low_lag_decoder) {
            qWarning() << "Primary Android video pipeline not initialized; cannot start playback";
            return;
        }
        auto cb=[this](std::shared_ptr<std::vector<uint8_t>> sample){
            bool is_h265=false;
            if(m_rtc_receiver){
                is_h265=m_rtc_receiver->get_codec()==QOpenHDVideoHelper::VideoCodecH265;
            }else{
                is_h265=m_receiver->get_codec()==QOpenHDVideoHelper::VideoCodecH265;
            }
            NALU nalu(sample->data(),sample->size(),is_h265);
            //qDebug()<<"XGot frame:"<<nalu.get_nal_unit_type_as_string().c_str();
            m_low_lag_decoder->interpretNALU(nalu);
        };
        if(m_rtc_receiver){
            m_rtc_receiver->start_receiving(cb);
        }else{
            m_receiver->start_receiving(cb);
        }
    };

    if (qopenhd_yuv_video_mode()) {
        // YUV 模式：解码器 ByteBuffer 输出（无 Surface），NV12 帧交给渲染 item。
        // 不依赖 SurfaceTextureChanged，直接启动接收
        qInfo() << "QAndroidMediaPlayer: yuv texture mode (ByteBuffer decode, no Surface)";
        m_low_lag_decoder->setByteBufferMode(true);
        m_low_lag_decoder->registerOnYuvFrameCallback([this](YuvFramePtr frame){
            if (m_videoOut) {
                m_videoOut->set_yuv_frame(std::move(frame));
            }
        });
        startReceiving();
        emit videoOutChanged();
        return;
    }

    auto setSurfaceTexture = [=]{
        // Create a new Surface object from our SurfaceTexture
        /*QAndroidJniObject surface("android/view/Surface",
                                  "(Landroid/graphics/SurfaceTexture;)V",
                                   m_videoOut->surfaceTexture().object());

        // Set the new surface to m_mediaPlayer object
        m_mediaPlayer.callMethod<void>("setSurface",
                                       "(Landroid/view/Surface;)V",
                                       surface.object());*/
        /*m_mediaPlayer.callMethod<void>("setSurfaceTexture",
                                       "(Landroid/graphics/SurfaceTexture;)V",
                                       m_videoOut->surfaceTexture().object());*/
        {
            QAndroidJniObject surface("android/view/Surface",
                                              "(Landroid/graphics/SurfaceTexture;)V",
                                               m_videoOut->surfaceTexture().object());
            QAndroidJniEnvironment env;
            m_low_lag_decoder->setOutputSurface(env,surface.object());
            qInfo() << "QAndroidMediaPlayer: decoder output surface set, surface_valid=" << surface.isValid();
        }
        startReceiving();
    };
    if (videoOut->surfaceTexture().isValid()){
        setSurfaceTexture();
    }else{
        connect(m_videoOut.data(), &QSurfaceTexture::surfaceTextureChanged, this, setSurfaceTexture);
    }
    emit videoOutChanged();
}

void QAndroidMediaPlayer::switch_primary_secondary()
{
    // first, we stop the rtp receiver (RTC 直连模式无端口切换概念)
    if (m_receiver) {
        m_receiver->stop_receiving();
    }
    // Then we can safely clean up the decoder (and its surface)
    if (m_low_lag_decoder) {
        m_low_lag_decoder->setOutputSurface(nullptr,nullptr);
    }
    // and re-create it with the right video port as input
}

/*void QAndroidMediaPlayer::playFile(const QString &file)
{
    qDebug()<<"QAndroidMediaPlayer::playFile"<<file;
    QAndroidJniEnvironment env;
    m_mediaPlayer.callMethod<void>("playUrl", "(Ljava/lang/String;)V", QAndroidJniObject::fromString(file).object());
    m_mediaPlayer.callMethod<void>("stop"); // try to stop the media player.
    m_mediaPlayer.callMethod<void>("reset"); // try to reset the media player.

    // http://developer.android.com/reference/android/media/MediaPlayer.html#setDataSource(java.lang.String) - the path of the file, or the http/rtsp URL of the stream you want to play.
    m_mediaPlayer.callMethod<void>("setDataSource", "(Ljava/lang/String;)V", QAndroidJniObject::fromString(file).object());

    // prepare media player
    m_mediaPlayer.callMethod<void>("prepare");

    // start playing
    m_mediaPlayer.callMethod<void>("start");
}*/
