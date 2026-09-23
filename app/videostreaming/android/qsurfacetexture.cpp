#include "qsurfacetexture.h"

#include <QAndroidJniEnvironment>
#include <QDebug>
#include <QOpenGLContext>
#include <QSGGeometryNode>
#include <QSGSimpleMaterialShader>

#include <atomic>
#include <cstdlib>
#include <sys/system_properties.h>

#include "../vscommon/QOpenHDVideoHelper.hpp"
#include "../vscommon/video_ratio_helper.hpp"

struct State {
    // the texture transform matrix
    QMatrix4x4 uSTMatrix;
    GLuint textureId = 0;
    // YUV 模式：紧致 NV12 上传的 Y / UV 纹理（legacy Surface 模式下为 0）
    GLuint yTexId = 0;
    GLuint uvTexId = 0;
    bool yuvMode = false;
    // 诊断用渲染模式（adb shell setprop debug.qopenhd.videodiag N）：
    // 0=正常; 1=固定采样+正常几何(测外部纹理); 2=常量红+正常几何(测材质通路);
    // 4=常量红+平凡几何(测几何/裁剪); 5=正常采样+平凡几何; 6=采样已知 2D 棋盘纹理
    float diagMode = 0.0f;

    int compare(const State *other) const
    {
        return (uSTMatrix == other->uSTMatrix && textureId == other->textureId
                && yTexId == other->yTexId && uvTexId == other->uvTexId
                && yuvMode == other->yuvMode
                && diagMode == other->diagMode) ? 0 : -1;
    }
};

class SurfaceTextureShader : QSGSimpleMaterialShader<State>
{
    QSG_DECLARE_SIMPLE_COMPARABLE_SHADER(SurfaceTextureShader, State)
public:
    // vertex & fragment shaders are shamelessly "stolen" from MyGLSurfaceView.java :)
    const char *vertexShader() const override {
        return
                "uniform mat4 qt_Matrix;                            \n"
                "uniform mat4 uSTMatrix;                            \n"
                "attribute vec4 aPosition;                          \n"
                "attribute vec4 aTextureCoord;                      \n"
                "varying vec2 vTextureCoord;                        \n"
                "void main() {                                      \n"
                "  gl_Position = qt_Matrix * aPosition;             \n"
                "  vTextureCoord = (uSTMatrix * aTextureCoord).xy;  \n"
                "}";
    }

    const char *fragmentShader() const override {
        return
                "#extension GL_OES_EGL_image_external : require                     \n"
                "precision mediump float;                                           \n"
                "varying vec2 vTextureCoord;                                        \n"
                "uniform lowp float qt_Opacity;                                     \n"
                "uniform lowp float uDiagMode;                                      \n"
                "uniform lowp float uUseYuv;                                        \n"
                "uniform samplerExternalOES sTexture;                               \n"
                "uniform sampler2D sTexture2d;                                      \n"
                "uniform sampler2D sTexY;                                           \n"
                "uniform sampler2D sTexUV;                                          \n"
                "void main() {                                                      \n"
                "  if (uDiagMode > 5.5) {                                           \n"
                "    gl_FragColor = texture2D(sTexture2d, vTextureCoord);           \n"
                "  } else if (uDiagMode > 3.5) {                                    \n"
                "    gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);                       \n"
                "  } else if (uDiagMode > 2.5) {                                    \n"
                "    gl_FragColor = texture2D(sTexture, vTextureCoord) * qt_Opacity;\n"
                "  } else if (uDiagMode > 1.5) {                                    \n"
                "    gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);                       \n"
                "  } else if (uDiagMode > 0.5) {                                    \n"
                "    gl_FragColor = texture2D(sTexture, vec2(0.5, 0.5));            \n"
                "  } else if (uUseYuv > 0.5) {                                      \n"
                "    float y = (texture2D(sTexY, vTextureCoord).r - 0.0627) * 1.16438;\n"
                "    vec2 c = texture2D(sTexUV, vTextureCoord).ra - vec2(0.5, 0.5); \n"
                "    gl_FragColor = vec4(y + 1.596 * c.y,                           \n"
                "                       y - 0.813 * c.y - 0.391 * c.x,             \n"
                "                       y + 2.018 * c.x, 1.0) * qt_Opacity;        \n"
                "  } else {                                                         \n"
                "    gl_FragColor = texture2D(sTexture, vTextureCoord) * qt_Opacity;\n"
                "  }                                                                \n"
                "}";
    }

    QList<QByteArray> attributes() const override
    {
        return QList<QByteArray>() << "aPosition" << "aTextureCoord";
    }

    void updateState(const State *state, const State *) override
    {
        static std::atomic<int> s_state_calls{0};
        const int n = ++s_state_calls;
        program()->setUniformValue(m_uSTMatrixLoc, state->uSTMatrix);
        program()->setUniformValue(m_uDiagModeLoc, state->diagMode);
        program()->setUniformValue(m_uUseYuvLoc, state->yuvMode ? 1.0f : 0.0f);
        if (state->yuvMode) {
            glActiveTexture(GL_TEXTURE0 + 2);
            glBindTexture(GL_TEXTURE_2D, state->yTexId);
            glActiveTexture(GL_TEXTURE0 + 3);
            glBindTexture(GL_TEXTURE_2D, state->uvTexId);
        } else {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, state->textureId);
        }
        // 诊断用模式 6：采样一张已知内容的 2D 纹理（品红/黄色棋盘），
        // 用于区分"采样通路坏了"与"外部纹理内容就是黑的"
        if (state->diagMode >= 5.5f) {
            static GLuint s_checkerTex = 0;
            if (s_checkerTex == 0) {
                glGenTextures(1, &s_checkerTex);
                glBindTexture(GL_TEXTURE_2D, s_checkerTex);
                const uint32_t px[4] = {0xFFFF00FFu, 0xFF00FFFFu, 0xFF00FFFFu, 0xFFFF00FFu};
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                qInfo() << "QSurfaceTexture: checker 2D texture created id=" << s_checkerTex
                        << "glErr=" << glGetError();
            }
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, s_checkerTex);
        }
        // QSG 约定：材质 updateState 返回时活跃纹理单元必须是 0 —— 其它材质
        // （文字图集/图片）会把纹理绑到"当前活跃单元"，不恢复会让整片 UI
        // 采样到错误纹理（部分驱动上表现为文字变成白色块）
        glActiveTexture(GL_TEXTURE0);
        if (n == 1 || n % 300 == 0) {
            // 诊断用：确认材质状态更新路径真的被执行（且无 GL 错误）
            qInfo() << "QSurfaceTexture: updateState calls=" << n
                    << "stMatrixLoc=" << m_uSTMatrixLoc << "diagLoc=" << m_uDiagModeLoc
                    << "texId=" << state->textureId << "checker2dLoc=" << m_sTexture2dLoc
                    << "yuvMode=" << state->yuvMode << "yTexId=" << state->yTexId
                    << "uvTexId=" << state->uvTexId
                    << "glErr=" << glGetError();
        }
    }

    void resolveUniforms() override
    {
        m_uSTMatrixLoc = program()->uniformLocation("uSTMatrix");
        m_uDiagModeLoc = program()->uniformLocation("uDiagMode");
        m_uUseYuvLoc = program()->uniformLocation("uUseYuv");
        m_sTexture2dLoc = program()->uniformLocation("sTexture2d");
        m_sTexYLoc = program()->uniformLocation("sTexY");
        m_sTexUVLoc = program()->uniformLocation("sTexUV");
        program()->setUniformValue("sTexture", 0); // we need to set the texture once
        program()->setUniformValue("sTexture2d", 1);
        program()->setUniformValue("sTexY", 2);
        program()->setUniformValue("sTexUV", 3);
        // 诊断用：确认着色器编译/链接成功（部分设备驱动可能静默失败），
        // 并记录 uniform location —— -1 会让 setUniformValue 静默失效
        qInfo() << "QSurfaceTexture: shader linked=" << program()->isLinked()
                << "log=" << program()->log().trimmed()
                << "uSTMatrixLoc=" << m_uSTMatrixLoc << "uDiagModeLoc=" << m_uDiagModeLoc
                << "uUseYuvLoc=" << m_uUseYuvLoc
                << "sTextureLoc=" << program()->uniformLocation("sTexture")
                << "sTexture2dLoc=" << m_sTexture2dLoc
                << "sTexYLoc=" << m_sTexYLoc << "sTexUVLoc=" << m_sTexUVLoc;
    }

private:
    int m_uSTMatrixLoc;
    int m_uDiagModeLoc;
    int m_uUseYuvLoc;
    int m_sTexture2dLoc;
    int m_sTexYLoc;
    int m_sTexUVLoc;
};

class SurfaceTextureNode : public QSGGeometryNode
{
public:
    SurfaceTextureNode(const QAndroidJniObject &surfaceTexture, GLuint textureId)
        : QSGGeometryNode()
        , m_surfaceTexture(surfaceTexture)
        , m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4)
        , m_textureId(textureId)
    {
        // we're going to use "preprocess" method to update the texture image
        // and to get the new matrix.
        setFlag(UsePreprocess);

        setGeometry(&m_geometry);

        // Create and set our SurfaceTextureShader
        QSGSimpleMaterial<State> *material = SurfaceTextureShader::createMaterial();
        material->state()->textureId = m_textureId;
        material->setFlag(QSGMaterial::Blending, false);
        setMaterial(material);
        setFlag(OwnsMaterial);

        // We're going to get the transform matrix for every frame
        // so, let's create the array once
        QAndroidJniEnvironment env;
        jfloatArray array = env->NewFloatArray(16);
        m_uSTMatrixArray = jfloatArray(env->NewGlobalRef(array));
        env->DeleteLocalRef(array);
    }

    ~SurfaceTextureNode() override
    {
        // delete the global reference, now the gc is free to free it
        QAndroidJniEnvironment()->DeleteGlobalRef(m_uSTMatrixArray);
    }

    // YUV 模式：节点不直接持有纹理内容，仅从 item 接收待上传帧并在 preprocess 里上传
    void setYuvMode(bool enable){ m_yuvMode=enable; }
    void setYuvTextureIds(GLuint yTex,GLuint uvTex,int w,int h){
        m_yTexId=yTex;
        m_uvTexId=uvTex;
        m_yuvTexW=w;
        m_yuvTexH=h;
    }
    void setPendingFrame(YuvFramePtr frame){ m_pendingYuvFrame=std::move(frame); }

    // QSGNode interface
    void preprocess() override;

private:
    void uploadYuvFrame(const YuvFrame &frame);

    QAndroidJniObject m_surfaceTexture;
    QSGGeometry m_geometry;
    jfloatArray m_uSTMatrixArray = nullptr;
    GLuint m_textureId;
    bool m_yuvMode=false;
    YuvFramePtr m_pendingYuvFrame;
    GLuint m_yTexId=0;
    GLuint m_uvTexId=0;
    int m_yuvTexW=0;
    int m_yuvTexH=0;
};

void SurfaceTextureNode::preprocess()
{
    static std::atomic<int> s_update_calls{0};
    const int n = ++s_update_calls;

    auto mat = static_cast<QSGSimpleMaterial<State> *>(material());
    if (!mat)
        return;

    // 诊断用：通过系统属性切换渲染模式，区分"材质/几何不渲染"与"外部纹理采样为黑"
    // adb shell setprop debug.qopenhd.videodiag 1  (常量红) / 2 (忽略矩阵固定采样)
    char diagBuf[PROP_VALUE_MAX] = {0};
    float diagMode = 0.0f;
    if (__system_property_get("debug.qopenhd.videodiag", diagBuf) > 0) {
        diagMode = (float)atoi(diagBuf);
    }
    mat->state()->diagMode = diagMode;

    if (m_yuvMode) {
        // YUV 模式：把最新解码帧上传到 Y / UV 两平面纹理（不经过 SurfaceTexture）
        if (m_pendingYuvFrame) {
            uploadYuvFrame(*m_pendingYuvFrame);
            m_pendingYuvFrame.reset();
        }
        if (n == 1 || n % 300 == 0) {
            qInfo() << "QSurfaceTexture: yuv preprocess calls=" << n
                    << "yTex=" << m_yTexId << "uvTex=" << m_uvTexId
                    << "texSize=" << m_yuvTexW << "x" << m_yuvTexH
                    << "diag=" << diagMode;
        }
        return;
    }

    // update the texture content
    m_surfaceTexture.callMethod<void>("updateTexImage");

    QAndroidJniEnvironment env;
    if (env->ExceptionCheck()) {
        // 诊断用：SurfaceTexture 消费失败会静默导致解码器输出缓冲耗尽
        qWarning() << "QSurfaceTexture: updateTexImage raised a JNI exception";
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (n == 1) {
        qInfo() << "QSurfaceTexture: preprocess glContext=" << (QOpenGLContext::currentContext()!=nullptr)
                << "texId=" << m_textureId << "diag=" << diagMode;
    }
    if (n == 1 || n % 60 == 0) {
        // 诊断用：getTimestamp 是最近一帧的采集时间戳，恒为 0 表示队列里从未有过帧
        const jlong lastFrameTs = m_surfaceTexture.callMethod<jlong>("getTimestamp");
        qInfo() << "QSurfaceTexture: updateTexImage calls=" << n
                << "lastFrameTimestamp=" << (long long)lastFrameTs;
    }

    // get the new texture transform matrix
    m_surfaceTexture.callMethod<void>("getTransformMatrix", "([F)V", m_uSTMatrixArray);
    env->GetFloatArrayRegion(m_uSTMatrixArray, 0, 16, mat->state()->uSTMatrix.data());
    if (n == 1 || n % 300 == 0 || (diagMode >= 2.5f && n % 60 == 0)) {
        // 诊断用：采样矩阵的关键 6 个分量（恒 0 或异常值会导致采样错误）
        const float *m = mat->state()->uSTMatrix.constData();
        qInfo() << "QSurfaceTexture: uSTMatrix [0,1,4,5,12,13]="
                << m[0] << m[1] << m[4] << m[5] << m[12] << m[13]
                << "diag=" << diagMode;
    }
}

void SurfaceTextureNode::uploadYuvFrame(const YuvFrame &frame)
{
    if (m_yTexId==0 || m_uvTexId==0) {
        // 纹理尚未创建（首帧早于分辨率就绪）——丢弃，纹理建立后的下一帧就会上传
        return;
    }
    if (frame.width!=m_yuvTexW || frame.height!=m_yuvTexH) {
        static std::atomic<bool> s_dimMismatchLogged{false};
        if(!s_dimMismatchLogged.exchange(true)){
            qWarning() << "QSurfaceTexture: yuv frame size" << frame.width << "x" << frame.height
                       << "does not match texture size" << m_yuvTexW << "x" << m_yuvTexH;
        }
        return;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D,m_yTexId);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,frame.width,frame.height,GL_LUMINANCE,GL_UNSIGNED_BYTE,frame.data.data());
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D,m_uvTexId);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,frame.width/2,frame.height/2,GL_LUMINANCE_ALPHA,GL_UNSIGNED_BYTE,
                    frame.data.data()+(size_t)frame.width*frame.height);
    glActiveTexture(GL_TEXTURE0);
    static std::atomic<int> s_uploads{0};
    const int un=++s_uploads;
    if(un==1 || un%300==0){
        qInfo() << "QSurfaceTexture: yuv upload#" << un << "size=" << frame.width << "x" << frame.height
                << "glErr=" << glGetError();
    }
}


/*extern "C" void Java_org_openhd_SurfaceTextureListener_frameAvailable(JNIEnv *, jobject , jlong ptr, jobject)
{
    // a new frame was decoded, let's update our item
    //QMetaObject::invokeMethod(reinterpret_cast<QSurfaceTexture*>(ptr), "update", Qt::QueuedConnection);
}*/

QSurfaceTexture::QSurfaceTexture(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlags(ItemHasContents);
}

QSurfaceTexture::~QSurfaceTexture()
{
    // Delete our texture
    if (m_textureId) {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        glDeleteTextures(1, &m_textureId);
    }
    if (m_yTexId) {
        glDeleteTextures(1, &m_yTexId);
    }
    if (m_uvTexId) {
        glDeleteTextures(1, &m_uvTexId);
    }
}

void QSurfaceTexture::set_video_texture_size(int width_px, int height_px)
{
    m_texture_width_px.store(width_px);
    m_texture_height_px.store(height_px);
    // 消费端同步设置默认缓冲尺寸，让同一 BufferQueue 的默认几何与视频一致
    // （解码器上报真实分辨率后调用；1x1 默认值会让部分平台的输出缓冲分配错配）
    // YUV 模式下没有 SurfaceTexture，跳过
    if (m_surfaceTexture.isValid() && width_px>0 && height_px>0) {
        m_surfaceTexture.callMethod<void>("setDefaultBufferSize","(II)V",width_px,height_px);
        qInfo() << "QSurfaceTexture: setDefaultBufferSize" << width_px << "x" << height_px;
    }
}

void QSurfaceTexture::set_yuv_frame(YuvFramePtr frame)
{
    {
        std::lock_guard<std::mutex> lock(m_yuvMutex);
        // 只保留最新帧：渲染跟不上时旧的未渲染帧直接丢弃
        m_pendingYuvFrame=std::move(frame);
    }
    // 解码线程调用 —— 经事件队列在 GUI 线程安排一次渲染
    QMetaObject::invokeMethod(this,"update",Qt::QueuedConnection);
}

YuvFramePtr QSurfaceTexture::take_pending_yuv_frame()
{
    std::lock_guard<std::mutex> lock(m_yuvMutex);
    return std::move(m_pendingYuvFrame);
}

void QSurfaceTexture::ensure_yuv_textures(int width_px,int height_px)
{
    if (m_yTexId!=0 && m_yuvTexW==width_px && m_yuvTexH==height_px) {
        return;
    }
    if (m_yTexId) {
        glDeleteTextures(1, &m_yTexId);
        m_yTexId=0;
    }
    if (m_uvTexId) {
        glDeleteTextures(1, &m_uvTexId);
        m_uvTexId=0;
    }
    glActiveTexture(GL_TEXTURE0);
    m_yuvTexOk = false;
    while (glGetError() != GL_NO_ERROR) {}
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glGenTextures(1, &m_yTexId);
    glBindTexture(GL_TEXTURE_2D, m_yTexId);
    glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE,width_px,height_px,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,nullptr);
    glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glGenTextures(1, &m_uvTexId);
    glBindTexture(GL_TEXTURE_2D, m_uvTexId);
    glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE_ALPHA,width_px/2,height_px/2,0,GL_LUMINANCE_ALPHA,GL_UNSIGNED_BYTE,nullptr);
    glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    m_yuvTexW=width_px;
    m_yuvTexH=height_px;
    const GLenum err = glGetError();
    m_yuvTexOk = (m_yTexId != 0 && m_uvTexId != 0 && err == GL_NO_ERROR);
    qInfo() << "QSurfaceTexture: yuv textures created Y=" << m_yTexId << "UV=" << m_uvTexId
            << width_px << "x" << height_px << "glErr=" << err << "ok=" << m_yuvTexOk;
    if (!m_yuvTexOk) {
        qWarning() << "QSurfaceTexture: yuv texture allocation failed on this GPU;"
                      " video node will draw nothing instead of sampling undefined textures";
    }
}

// No idea how / why this works, but it works.
static void qrectf_flip_horizontally(QRectF& rect){
    float tmp = rect.top();
    rect.setTop(rect.bottom());
    rect.setBottom(tmp);
}

// 诊断用：读取渲染模式属性
// 0=正常; 1=常量红+正常几何(测材质通路); 2=固定采样+正常几何(测纹理内容);
// 4=常量红+平凡几何(测几何/裁剪); 5=正常采样+平凡几何
static float qopenhd_diag_mode(){
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.qopenhd.videodiag", buf) > 0) {
        return (float)atoi(buf);
    }
    return 0.0f;
}

// YUV 渲染模式（默认开启）：解码走 ByteBuffer，帧数据上传到 Y/UV 两平面纹理再着色，
// 不经过 SurfaceTexture -> samplerExternalOES（该路径在部分平台故障）。
// adb shell setprop debug.qopenhd.surfacevideo 1 可切回 legacy Surface 模式。
static bool qopenhd_yuv_mode(){
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.qopenhd.surfacevideo", buf) > 0) {
        return atoi(buf) == 0;
    }
    return true;
}

QSGNode *QSurfaceTexture::updatePaintNode(QSGNode *n, QQuickItem::UpdatePaintNodeData *)
{
    static std::atomic<int> s_paint_calls{0};
    const int paint_n = ++s_paint_calls;
    const bool yuvMode = qopenhd_yuv_mode();
    if (paint_n == 1 || paint_n % 120 == 0) {
        const QPointF scenePos = mapToScene(QPointF(0, 0));
        qInfo() << "QSurfaceTexture: updatePaintNode calls=" << paint_n
                << "visible=" << isVisible() << "opacity=" << opacity()
                << "size=" << width() << "x" << height()
                << "scenePos=" << scenePos << "yuvMode=" << yuvMode;
    }
    SurfaceTextureNode *node = static_cast<SurfaceTextureNode *>(n);
    if (!node) {
        if (yuvMode) {
            // YUV 模式：不创建外部纹理与 Java SurfaceTexture（该路径在部分平台故障），
            // Y/UV 纹理在分辨率就绪后创建，内容由解码线程逐帧投递
            node = new SurfaceTextureNode(QAndroidJniObject(), 0);
            node->setYuvMode(true);
            qInfo() << "QSurfaceTexture: yuv mode node created (no SurfaceTexture)";
        } else {
            // Create texture
            glGenTextures(1, &m_textureId);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_textureId);

            // Can't do mipmapping with camera source
            glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            // Clamp to edge is the only option
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            // Create surface texture Java object
            m_surfaceTexture = QAndroidJniObject("android/graphics/SurfaceTexture", "(I)V", m_textureId);

            // We need to setOnFrameAvailableListener, to be notify when a new frame was decoded
            // and is ready to be displayed. Check android/src/com/kdab/android/SurfaceTextureListener.java
            // file for implementation details.
            /*m_surfaceTexture.callMethod<void>("setOnFrameAvailableListener",
                                              "(Landroid/graphics/SurfaceTexture$OnFrameAvailableListener;)V",
                                              QAndroidJniObject("org/openhd/SurfaceTextureListener",
                                                                "(J)V", jlong(this)).object());*/

            // Create our SurfaceTextureNode
            node = new SurfaceTextureNode(m_surfaceTexture, m_textureId);
            emit surfaceTextureChanged(this);
        }
    }

    if (yuvMode) {
        const int texW = m_texture_width_px.load();
        const int texH = m_texture_height_px.load();
        if (texW>0 && texH>0) {
            ensure_yuv_textures(texW,texH);
        }
        auto *mat = static_cast<QSGSimpleMaterial<State> *>(node->material());
        mat->state()->yuvMode=true;
        mat->state()->yTexId=m_yTexId;
        mat->state()->uvTexId=m_uvTexId;
        node->setYuvTextureIds(m_yTexId,m_uvTexId,m_yuvTexW,m_yuvTexH);
        node->setPendingFrame(take_pending_yuv_frame());
    }

    QRectF rect(boundingRect());
    const float diagMode = qopenhd_diag_mode();
    if (diagMode < 0.5f && yuvMode && !m_yuvTexOk) {
        // 首帧/纹理未就绪或纹理分配失败：本帧不绘制，避免采样未定义纹理
        // （部分驱动会输出整块异常颜色），纹理就绪后自动恢复
        rect = QRectF();
    } else if (diagMode >= 3.5f) {
        // 诊断用：平凡几何——直接用整个 item 矩形，跳过 viewport 计算与翻转，
        // 用于区分"几何/裁剪把视频弄没"与"采样内容是黑的"
        static std::atomic<int> s_trivial_logged{0};
        if (++s_trivial_logged == 1) {
            qInfo() << "QSurfaceTexture: diag trivial geometry mode, rect=" << rect;
        }
    } else if(m_texture_width_px.load()>0 && m_texture_height_px.load()>0){
        auto coords=helper::ratio::calculate_viewport(boundingRect().width(),boundingRect().height(),m_texture_width_px.load(),m_texture_height_px.load(),QOpenHDVideoHelper::get_primary_video_scale_to_fit());
        rect=QRectF(coords.x,coords.y,coords.width,coords.height);
        if (!yuvMode) {
            // flip vertical - for some reason video by default is upside down on android otherwise
            // （外部纹理采样方向与 QSG 惯例相反；YUV 2D 纹理的行序已经与显示一致，无需翻转）
            qrectf_flip_horizontally(rect);
        }
    }

     const QRectF texture_coords=QRectF(0, 0, 1, 1);

    //qDebug()<<rect.width()<<" "<<rect.height();
    //rect.setWidth(rect.width()*0.5);

    QSGGeometry::updateTexturedRectGeometry(node->geometry(), rect, texture_coords);
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    // XX
    QMetaObject::invokeMethod(reinterpret_cast<QSurfaceTexture*>(this), "update", Qt::QueuedConnection);
    return node;
}
