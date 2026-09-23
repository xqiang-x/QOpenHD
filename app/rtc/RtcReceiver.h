#ifndef QOPENHD_RTC_RECEIVER_H
#define QOPENHD_RTC_RECEIVER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "../videostreaming/vscommon/QOpenHDVideoHelper.hpp"

/**
 * RTC 视频接收器：与 GstRtpReceiver 相同的鸭子接口（start_receiving / stop_receiving /
 * get_codec），由 QAndroidMediaPlayer 根据设置二选一。
 * RtcClient 网络线程投递整 AU（Annex-B，含起始码），这里按起始码拆成单个 NALU 后回调——
 * LowLagDecoder::interpretNALU() 每次只接受一个 NALU，不能整 AU 喂入。
 */
class RtcReceiver
{
public:
    using NEW_FRAME_CALLBACK = std::function<void(std::shared_ptr<std::vector<uint8_t>> frame)>;

    explicit RtcReceiver(QOpenHDVideoHelper::VideoCodec video_codec);
    ~RtcReceiver();
    RtcReceiver(const RtcReceiver&) = delete;
    RtcReceiver& operator=(const RtcReceiver&) = delete;

    void start_receiving(NEW_FRAME_CALLBACK cb);
    void stop_receiving();
    QOpenHDVideoHelper::VideoCodec get_codec() const { return m_codec; }

private:
    // 拆帧状态挂在 shared_ptr 上，独立于本对象生命周期：
    // 停止/析构后网络线程可能仍持有回调副本，此时共享状态仍有效且 cb 为空，安全丢弃
    struct Splitter {
        std::mutex mutex;
        NEW_FRAME_CALLBACK cb;          // 空 = 未启动
        std::vector<uint8_t> nalu_data; // 当前 NALU 累计缓冲（含起始码）
        size_t nalu_data_position = 0;
        int nalu_search_state = 0;
    };
    static void feed_au(const std::shared_ptr<Splitter>& splitter, std::vector<uint8_t>&& au);

    const QOpenHDVideoHelper::VideoCodec m_codec;
    std::shared_ptr<Splitter> m_splitter;
};

#endif // QOPENHD_RTC_RECEIVER_H