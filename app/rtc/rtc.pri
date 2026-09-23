# tj_rtc RTC SDK（Subscriber 订阅端）源码内联编译：
# 手机直连 VPS SFU，与原有 RTP 接收路径二选一（见 app/rtc/RtcClient.h）。
# tj_rtc 仓库默认位于本仓库根目录（QOpenHD/ 的上一级），可用环境变量 TJ_RTC_SDK_ROOT 覆盖。
# SDK 不存在时（例如 CI 仅检出 QOpenHD 仓库）RtcClient 退化为空实现，RTC 功能不可用，其余功能不受影响。

TJ_RTC_SDK_ROOT = $$(TJ_RTC_SDK_ROOT)
isEmpty(TJ_RTC_SDK_ROOT) {
    TJ_RTC_SDK_ROOT = $$PWD/../../..
}

INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/RtcClient.h \
    $$PWD/RtcReceiver.h

SOURCES += \
    $$PWD/RtcClient.cpp \
    $$PWD/RtcReceiver.cpp

exists($$TJ_RTC_SDK_ROOT/client_sdk/rtc_client_sdk/src/Subscriber.cpp) {
    message("QOpenHD: RtcLink enabled (tj_rtc at $$TJ_RTC_SDK_ROOT)")
    DEFINES += QOPENHD_HAS_RTC

    INCLUDEPATH += \
        $$TJ_RTC_SDK_ROOT/client_sdk/rtc_client_sdk/include \
        $$TJ_RTC_SDK_ROOT/common/sfu_protocol \
        $$TJ_RTC_SDK_ROOT/libs/frame_protoc/src \
        $$TJ_RTC_SDK_ROOT/libs/frame_protoc/cm256cc \
        $$TJ_RTC_SDK_ROOT/libs/pacer \
        $$TJ_RTC_SDK_ROOT/libs/mem_pool

    SOURCES += \
        $$TJ_RTC_SDK_ROOT/client_sdk/rtc_client_sdk/src/Subscriber.cpp \
        $$TJ_RTC_SDK_ROOT/client_sdk/rtc_client_sdk/src/P2PManager.cpp \
        $$TJ_RTC_SDK_ROOT/libs/frame_protoc/src/protocol.cpp \
        $$TJ_RTC_SDK_ROOT/libs/frame_protoc/src/sender.cpp \
        $$TJ_RTC_SDK_ROOT/libs/frame_protoc/src/receiver.cpp \
        $$TJ_RTC_SDK_ROOT/libs/frame_protoc/src/proxy_receiver.cpp \
        $$TJ_RTC_SDK_ROOT/libs/frame_protoc/src/bitrate_controller.cpp \
        $$TJ_RTC_SDK_ROOT/libs/frame_protoc/cm256cc/cm256.cpp \
        $$TJ_RTC_SDK_ROOT/libs/frame_protoc/cm256cc/gf256.cpp \
        $$TJ_RTC_SDK_ROOT/libs/pacer/Pacer.cpp \
        $$TJ_RTC_SDK_ROOT/libs/mem_pool/MemPool.cpp

    # cm256 SIMD 分支，与 libs/frame_protoc/CMakeLists.txt 保持一致。
    # 注意：qmake 的 contains() 对列表元素做精确（anchored）正则匹配，
    # 必须使用完整 ABI 名（与 gst_video.pri 的写法一致），不能用 "arm64" 之类的子串。
    DEFINES += NO_RESTRICT
    android {
        contains(ANDROID_TARGET_ARCH, arm64-v8a) {
            DEFINES += USE_NEON
        } else:contains(ANDROID_TARGET_ARCH, armeabi-v7a) {
            DEFINES += USE_NEON
            QMAKE_CXXFLAGS += -mfpu=neon
        } else {
            DEFINES += USE_SSSE3
            QMAKE_CXXFLAGS += -mssse3
        }
    } else {
        contains(QT_ARCH, x86_64)|contains(QT_ARCH, i386) {
            DEFINES += USE_SSSE3
            QMAKE_CXXFLAGS += -mssse3
        } else {
            DEFINES += USE_NEON
        }
    }

    # Android (bionic) 的 pthread 内置于 libc，显式 -lpthread 会让 NDK r21 链接失败
    !android: LIBS += -lpthread
} else {
    message("QOpenHD: RtcLink disabled (tj_rtc SDK not found at $$TJ_RTC_SDK_ROOT)")
}