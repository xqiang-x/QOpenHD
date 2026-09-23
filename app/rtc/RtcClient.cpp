#include "RtcClient.h"

#include <QDebug>
#include <QSettings>

#ifdef QOPENHD_HAS_RTC

#include <chrono>

#include "Subscriber.h"
#include "SfuFrameType.h"

#include "../telemetry/MavlinkTelemetry.h"

RtcClient& RtcClient::instance()
{
    static RtcClient instance{};
    return instance;
}

RtcClient::RtcClient()
{
    read_from_settings();
}

RtcClient::~RtcClient()
{
    // 正常退出路径已在 main() 调用 stop()；这里兜底保证看护线程退出
    m_supervisor_running.store(false);
    if (m_supervisor_thread.joinable()) {
        m_supervisor_thread.join();
    }
}

void RtcClient::read_from_settings()
{
    QSettings settings;
    m_enabled = settings.value("qopenhd_rtc_enabled", false).toBool();
    m_server = settings.value("qopenhd_rtc_server", "106.15.177.248").toString().toStdString();
    m_port = settings.value("qopenhd_rtc_server_port", 9200).toInt();
    m_use_udp = settings.value("qopenhd_rtc_use_udp", true).toBool();
    m_stream_id = settings.value("qopenhd_rtc_stream_id", "openhd0").toString().toStdString();
    m_user_id = settings.value("qopenhd_rtc_user_id", "qopenhd").toString().toStdString();
    qInfo() << "RtcClient: settings enabled=" << m_enabled
            << "server=" << m_server.c_str() << "port=" << m_port
            << "use_udp=" << m_use_udp
            << "stream=" << m_stream_id.c_str() << "user=" << m_user_id.c_str();
}

void RtcClient::start_if_enabled()
{
    if (!m_enabled) {
        qInfo() << "RtcClient: disabled in settings (regular RTP/telemetry path unchanged)";
        return;
    }
    if (m_supervisor_running.load()) {
        return;
    }
    m_supervisor_running.store(true);
    m_supervisor_thread = std::thread([this] { supervisor_loop(); });
    // sendMessage 优先走 RTC ctrl 上行；未连接时 hook 返回 false，回落原有 UDP/TCP 逻辑
    MavlinkTelemetry::instance().set_rtc_upstream_sender(
        [](const uint8_t* data, size_t len) {
            return RtcClient::instance().send_ctrl_upstream(data, len);
        });
    qInfo() << "RtcClient: starting RTC direct link, SFU" << m_server.c_str() << ":" << m_port
            << "udp=" << m_use_udp << "stream=" << m_stream_id.c_str();
}

void RtcClient::stop()
{
    if (m_supervisor_running.exchange(false)) {
        if (m_supervisor_thread.joinable()) {
            m_supervisor_thread.join();
        }
    }
    MavlinkTelemetry::instance().set_rtc_upstream_sender(nullptr);
    qInfo() << "RtcClient: stopped";
}

void RtcClient::supervisor_loop()
{
    qInfo() << "RtcClient: supervisor thread started";
    uint64_t last_video_frames = 0;
    uint64_t last_ctrl_rx = 0;
    auto last_rebuild_at = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    while (m_supervisor_running.load()) {
        bool rebuild = false;
        {
            std::lock_guard<std::mutex> lock(m_subscriber_mutex);
            rebuild = !m_subscriber || m_needs_restart.load() || !m_subscriber->IsRunning();
        }
        const auto now = std::chrono::steady_clock::now();
        if (rebuild && (now - last_rebuild_at) >= std::chrono::seconds(2)) {
            last_rebuild_at = now;
            m_needs_restart.store(false);
            stop_subscriber();
            if (!m_supervisor_running.load()) {
                break;
            }
            m_n_restarts.fetch_add(1);
            start_subscriber();
        }
        // 1s 统计日志（帧计数/重连情况，供 adb logcat 远程验证）
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto n_video_frames = m_n_video_frames.load();
        const auto n_ctrl_rx = m_n_ctrl_rx.load();
        qInfo() << "RtcClient stats: connected=" << m_connected.load()
                << "video_frames=" << n_video_frames << "(+" << (n_video_frames - last_video_frames) << "/s)"
                << "idr=" << m_n_video_idr.load()
                << "ctrl_rx=" << n_ctrl_rx << "(+" << (n_ctrl_rx - last_ctrl_rx) << "/s)"
                << "ctrl_tx=" << m_n_ctrl_tx.load()
                << "restarts=" << m_n_restarts.load();
        last_video_frames = n_video_frames;
        last_ctrl_rx = n_ctrl_rx;
    }
    stop_subscriber();
    qInfo() << "RtcClient: supervisor thread stopped";
}

void RtcClient::start_subscriber()
{
    auto sub = std::make_unique<pull::Subscriber>();
    auto* sub_raw = sub.get();
    sub->SetOnConnected([this, sub_raw](uint32_t sessionId) {
        qInfo() << "RtcClient: subscribed stream ok, session=" << sessionId << "-> request keyframe";
        m_connected.store(true);
        m_needs_restart.store(false);
        sub_raw->RequestKeyFrame();
    });
    sub->SetOnDisconnected([this] {
        qWarning() << "RtcClient: disconnected from SFU";
        m_connected.store(false);
    });
    sub->SetOnSubscribeFailed([this](const std::string& reason) {
        qWarning() << "RtcClient: subscribe failed:" << reason.c_str() << "(will retry)";
        m_connected.store(false);
        m_needs_restart.store(true);
    });
    sub->SetOnFrame([this](uint32_t /*sourceSessionId*/, std::vector<uint8_t>&& data) {
        on_frame(std::move(data));
    });
    sub->SetOnControlData([this](uint32_t /*sourceSessionId*/, const uint8_t* data, size_t len) {
        m_n_ctrl_rx.fetch_add(1);
        MavlinkTelemetry::instance().feed_rtc_bytes(data, len);
    });
    const int udp_port = m_use_udp ? m_port + 1 : 0;
    // Start() 非阻塞：连接/订阅都在 SDK 网络线程内进行
    sub->Start(m_server, m_port, m_stream_id, m_user_id, m_use_udp, udp_port);
    {
        std::lock_guard<std::mutex> lock(m_subscriber_mutex);
        m_subscriber = std::move(sub);
    }
}

void RtcClient::stop_subscriber()
{
    std::unique_ptr<pull::Subscriber> sub;
    {
        std::lock_guard<std::mutex> lock(m_subscriber_mutex);
        sub = std::move(m_subscriber);
    }
    if (sub) {
        sub->Stop();
    }
    m_connected.store(false);
}

void RtcClient::on_frame(std::vector<uint8_t>&& data)
{
    if (data.empty()) {
        return;
    }
    // data[0]=SfuFrameType，其余为 Annex-B AU
    const auto frame_type = static_cast<sfu::FrameType>(data[0]);
    if (!sfu::IsVideoFrame(frame_type)) {
        // 音频（AUDIO_PCM）等不在本次范围，忽略
        return;
    }
    if (frame_type == sfu::FrameType::VIDEO_IDR) {
        m_n_video_idr.fetch_add(1);
    }
    m_n_video_frames.fetch_add(1);
    VideoAuCb cb;
    {
        std::lock_guard<std::mutex> lock(m_video_cb_mutex);
        cb = m_video_au_cb;
    }
    if (!cb) {
        return;
    }
    data.erase(data.begin());
    cb(std::move(data));
}

void RtcClient::set_video_au_callback(VideoAuCb cb)
{
    std::lock_guard<std::mutex> lock(m_video_cb_mutex);
    m_video_au_cb = std::move(cb);
}

void RtcClient::clear_video_au_callback()
{
    std::lock_guard<std::mutex> lock(m_video_cb_mutex);
    m_video_au_cb = nullptr;
}

void RtcClient::request_keyframe()
{
    std::lock_guard<std::mutex> lock(m_subscriber_mutex);
    if (m_subscriber && m_subscriber->IsRunning()) {
        m_subscriber->RequestKeyFrame();
        qInfo() << "RtcClient: requested keyframe (PLI)";
    }
}

bool RtcClient::send_ctrl_upstream(const uint8_t* data, size_t len)
{
    if (!m_enabled || !m_connected.load()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_subscriber_mutex);
    if (!m_subscriber || !m_subscriber->IsRunning()) {
        return false;
    }
    // 必须走可靠 TCP 上行：实测 UDP(不可靠)上行 0/27 送达（air mav_rx 冻结），
    // TCP(reliable) 上行 15/15 全达；否则手机遥测/命令永远到不了 air
    m_subscriber->SendCtrlUpstream(data, len, true);
    m_n_ctrl_tx.fetch_add(1);
    return true;
}

#else // !QOPENHD_HAS_RTC
// tj_rtc SDK 不在（例如 CI 仅检出 QOpenHD 仓库）：RTC 功能不可用，其余功能不受影响

RtcClient& RtcClient::instance()
{
    static RtcClient instance{};
    return instance;
}

RtcClient::RtcClient()
{
    qInfo() << "RtcClient: built without tj_rtc SDK, RTC direct link unavailable";
}

RtcClient::~RtcClient() = default;

void RtcClient::read_from_settings() {}

void RtcClient::start_if_enabled() {}

void RtcClient::stop() {}

void RtcClient::set_video_au_callback(VideoAuCb cb) {}

void RtcClient::clear_video_au_callback() {}

void RtcClient::request_keyframe() {}

bool RtcClient::send_ctrl_upstream(const uint8_t* data, size_t len)
{
    return false;
}

#endif // QOPENHD_HAS_RTC