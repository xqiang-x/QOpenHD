#ifndef QOPENHD_RTC_CLIENT_H
#define QOPENHD_RTC_CLIENT_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef QOPENHD_HAS_RTC
namespace pull { class Subscriber; }
#endif

/**
 * QOpenHD 的 RTC 直连客户端（单例）：手机直接连 VPS SFU，不再依赖 OpenHD 地面端中转。
 * 一条 Subscriber 订阅同时承载：
 *  - 视频下行：OnFrame 按 SfuFrameType 过滤出视频帧（整 AU，Annex-B）→ 投递给 RtcReceiver 拆 NALU
 *  - 遥测/命令下行：OnControlData（MAVLink 原样）→ MavlinkTelemetry::feed_rtc_bytes
 *  - 命令上行：MavlinkTelemetry::sendMessage → send_ctrl_upstream → Subscriber::SendCtrlUpstream
 * 断线由看护线程重订阅（≥2s 节流）；连接成功立即 RequestKeyFrame(PLI) 快速出画。
 * 设置项改动需重启应用生效。
 */
class RtcClient
{
public:
    static RtcClient& instance();

    RtcClient(const RtcClient&) = delete;
    RtcClient& operator=(const RtcClient&) = delete;

    // 设置（QSettings，键与 qml/ui/elements/AppSettings.qml 一致）
    void read_from_settings();
    bool is_enabled() const { return m_enabled; }
    bool is_connected() const { return m_connected.load(); }

    // 在 MavlinkTelemetry::start() 之后调用；仅设置启用时真正启动
    void start_if_enabled();
    // 进程退出前调用（返回后不再有任何来自 RTC 的回调）
    void stop();

    // ---- 视频下行（由 RtcReceiver 注册） ----
    using VideoAuCb = std::function<void(std::vector<uint8_t>&& au_payload)>;
    void set_video_au_callback(VideoAuCb cb);
    void clear_video_au_callback();
    void request_keyframe();

    // ---- MAVLink ctrl 上行 ----
    bool send_ctrl_upstream(const uint8_t* data, size_t len);

private:
    RtcClient();
    ~RtcClient();

    void supervisor_loop();
    void start_subscriber();
    void stop_subscriber();
    void on_frame(std::vector<uint8_t>&& data);

    // 默认值与 OpenHD RtcLink 地面端 / M5 实测一致
    bool m_enabled = false;
    std::string m_server = "106.15.177.248";
    int m_port = 9200;
    bool m_use_udp = true;
    std::string m_stream_id = "openhd0";
    std::string m_user_id = "qopenhd";

    std::atomic<bool> m_supervisor_running{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_needs_restart{false};
    std::thread m_supervisor_thread;

    std::mutex m_subscriber_mutex;
#ifdef QOPENHD_HAS_RTC
    std::unique_ptr<pull::Subscriber> m_subscriber;
#endif

    std::mutex m_video_cb_mutex;
    VideoAuCb m_video_au_cb;

    std::atomic<uint64_t> m_n_video_frames{0};
    std::atomic<uint64_t> m_n_video_idr{0};
    std::atomic<uint64_t> m_n_ctrl_rx{0};
    std::atomic<uint64_t> m_n_ctrl_tx{0};
    std::atomic<uint64_t> m_n_restarts{0};
};

#endif // QOPENHD_RTC_CLIENT_H