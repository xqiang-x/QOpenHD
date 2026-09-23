#ifndef YUV_FRAME_H
#define YUV_FRAME_H

#include <cstdint>
#include <memory>
#include <vector>

// 一帧解码后的视频数据，紧致 NV12 布局：
// data[0, width*height) = Y 平面（逐行，无 stride 填充）
// data[width*height, width*height*3/2) = UV 交错平面（U,V,U,V,... 每行 width 字节）
struct YuvFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;
};

using YuvFramePtr = std::shared_ptr<const YuvFrame>;

#endif // YUV_FRAME_H