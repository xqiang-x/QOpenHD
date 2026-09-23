#ifndef QSURFACETEXTURE_H
#define QSURFACETEXTURE_H

#include <QAndroidJniObject>
#include <QQuickItem>

#include <atomic>
#include <mutex>

#include "yuv_frame.h"

class QSurfaceTexture : public QQuickItem
{
    Q_OBJECT
public:
    QSurfaceTexture(QQuickItem *parent = nullptr);
    ~QSurfaceTexture();

    // returns surfaceTexture Java object (only valid in the legacy Surface mode).
    const QAndroidJniObject &surfaceTexture() const { return m_surfaceTexture; }

    // Needs to be explicitly called from c++ to do proper aspect ratio
    // not thread safe, but this doesn't matter
    void set_video_texture_size(int width_px,int height_px);

    // ByteBuffer/YUV 渲染模式：解码线程投递紧致 NV12 帧，渲染线程上传到 Y/UV 两平面纹理。
    // 仅在 YUV 模式（debug.qopenhd.surfacevideo 未开启）下使用。
    void set_yuv_frame(YuvFramePtr frame);

    // QQuickItem interface
protected:
    QSGNode *updatePaintNode(QSGNode *n, UpdatePaintNodeData *) override;

signals:
    void surfaceTextureChanged(QSurfaceTexture *surfaceTexture);

private:
    YuvFramePtr take_pending_yuv_frame();
    void ensure_yuv_textures(int width_px,int height_px);

    // our texture (legacy Surface mode)
    uint32_t m_textureId = 0;
    // YUV mode: Y plane (full res) + interleaved UV plane (half res)
    uint32_t m_yTexId = 0;
    uint32_t m_uvTexId = 0;
    int m_yuvTexW = 0;
    int m_yuvTexH = 0;
    // Y/UV 纹理创建成功标志：创建失败（部分 GPU 拒绝 LUMINANCE 格式）时不绘制，
    // 否则会采样未定义纹理在部分驱动上输出整块异常颜色
    bool m_yuvTexOk = false;

    // Java SurfaceTexture object (legacy Surface mode)
    QAndroidJniObject m_surfaceTexture;

    std::atomic<int> m_texture_width_px{0};
    std::atomic<int> m_texture_height_px{0};

    std::mutex m_yuvMutex;
    YuvFramePtr m_pendingYuvFrame;
};

#endif // QSURFACETEXTURE_H