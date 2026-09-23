#include "RtcReceiver.h"

#include <QDebug>

#include "RtcClient.h"
#include "../videostreaming/vscommon/nalu/NALU.hpp"

RtcReceiver::RtcReceiver(QOpenHDVideoHelper::VideoCodec video_codec)
    : m_codec(video_codec)
{
    m_splitter = std::make_shared<Splitter>();
    m_splitter->nalu_data.resize(NALU::NALU_MAXLEN);
}

RtcReceiver::~RtcReceiver()
{
    stop_receiving();
}

void RtcReceiver::start_receiving(NEW_FRAME_CALLBACK cb)
{
    {
        std::lock_guard<std::mutex> lock(m_splitter->mutex);
        m_splitter->cb = std::move(cb);
        m_splitter->nalu_data_position = 0;
        m_splitter->nalu_search_state = 0;
    }
    auto splitter = m_splitter;
    RtcClient::instance().set_video_au_callback([splitter](std::vector<uint8_t>&& au) {
        feed_au(splitter, std::move(au));
    });
    // 订阅成功（或流刚开始推）时请求关键帧，让解码器尽快出画
    RtcClient::instance().request_keyframe();
    qInfo() << "RtcReceiver: started, codec="
            << (m_codec == QOpenHDVideoHelper::VideoCodecH265 ? "H265" : "H264");
}

void RtcReceiver::stop_receiving()
{
    RtcClient::instance().clear_video_au_callback();
    std::lock_guard<std::mutex> lock(m_splitter->mutex);
    m_splitter->cb = nullptr;
    m_splitter->nalu_data_position = 0;
    m_splitter->nalu_search_state = 0;
}

// 与 RawReceiver::udp_raw_data_callback 相同的 0,0,1 / 0,0,0,1 起始码状态机，
// 区别只是输入为整个 AU（RTC 帧），而非 UDP 分片
void RtcReceiver::feed_au(const std::shared_ptr<Splitter>& splitter, std::vector<uint8_t>&& au)
{
    std::lock_guard<std::mutex> lock(splitter->mutex);
    if (!splitter->cb) {
        return;
    }
    uint8_t* nalu_data = splitter->nalu_data.data();
    size_t& nalu_data_position = splitter->nalu_data_position;
    int& nalu_search_state = splitter->nalu_search_state;
    for (const auto byte : au) {
        nalu_data[nalu_data_position++] = byte;
        if (nalu_data_position >= NALU::NALU_MAXLEN - 1) {
            qDebug() << "RtcReceiver: NALU size exceeded, resetting parser";
            nalu_data_position = 0;
        }
        int parsed_prefix_len;
        switch (nalu_search_state) {
            case 0:
            case 1:
                if (byte == 0) {
                    nalu_search_state++;
                } else {
                    nalu_search_state = 0;
                }
                break;
            case 2:
            case 3:
                if (byte == 0) {
                    if (nalu_search_state == 3) {
                        // 连续的 0：忽略，不重复累计
                        nalu_data_position--;
                    }
                    nalu_search_state = 3;
                } else if (byte == 1) {
                    parsed_prefix_len = nalu_search_state == 3 ? 4 : 3;
                    if (NALU::has_valid_prefix(nalu_data) && nalu_data_position > 4) {
                        const size_t nalu_len = nalu_data_position - parsed_prefix_len;
                        if (nalu_len >= 6) {
                            auto out = std::make_shared<std::vector<uint8_t>>(
                                nalu_data, nalu_data + nalu_len);
                            splitter->cb(out);
                        }
                    }
                    // 把新 NALU 的起始码搬到缓冲头部，继续累计
                    NALU::write_prefix(nalu_data, parsed_prefix_len == 4);
                    nalu_data_position = parsed_prefix_len;
                    nalu_search_state = 0;
                } else {
                    nalu_search_state = 0;
                }
                break;
            default:
                break;
        }
    }
}