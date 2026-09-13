#ifndef FACEMANAGER_H
#define FACEMANAGER_H

#include <QObject>
#include <QImage>
#include <QString>
#include <QRect>
#include <QMap>
#include <QVector>
#include <opencv2/opencv.hpp>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <vector>

#include "v4l2_capture.h"
#include "rkisp_camera.h"
#include "rga_transform.h"
#include "face_landmark.h"
#include "face_antispoof.h"
#include "mp4_recorder.h"
#include "rga_osd.h"

#include "rknn_api.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include "video_encoder.h"
#include "rtsp_streamer.h"

/**
 * @brief Face detection rectangle.
 */
struct FaceRect {
    int x;
    int y;
    int width;
    int height;
};

/**
 * @brief Face manager singleton.
 *
 * Zero-copy: V4L2 MMAP -> NV12 -> VPU/RTSP; NV12 -> RGA BGR -> face detect/recognize.
 */
class FaceManager : public QObject
{
    Q_OBJECT

public:
    static FaceManager* getInstance();
    explicit FaceManager(QObject *parent = nullptr);
    ~FaceManager();

    /* ---- Camera ---- */
    bool initCamera(int cameraIndex = 0);
    QImage getCameraFrame();
    void releaseCamera();
    bool isCameraOpened() const { return m_isCameraOpened; }

    /* ---- Stream parameters ---- */
    void setStreamParams(int width, int height, int fps = 30);
    int streamWidth()  const { return m_streamWidth; }
    int streamHeight() const { return m_streamHeight; }
    int streamFps()    const { return m_streamFps; }

    /* ---- Face detection ---- */
    std::vector<FaceRect> detectFace(const QImage& frame);

    /* ---- Face registration/features ---- */
    bool registerFace(const QString& uid, const QImage& faceFrame);
    QString extractFaceFeature(const QImage& faceFrame);
    QVector<float> extractFeatureVector(const QImage& frame, const FaceRect& roi);

    /* ---- Face library management ---- */
    bool hasFaceData(const QString& uid) const { return m_featureMap.contains(uid); }
    void removeFaceData(const QString& uid);
    int getRegisteredFaceCount() const { return m_featureMap.size(); }

    /* ---- UI utilities ---- */
    QImage captureFace();

    /* ---- Video streaming ---- */
    bool startStreaming(const QString &rtspUrl);
    void stopStreaming();
    bool isStreaming() const { return m_isStreaming; }

    /* ---- Local recording ---- */
    bool startRecording(const QString &outputPath);
    void stopRecording();
    bool isRecording() const { return m_isRecording; }

    /* ---- Monitor mode (server-controlled) ---- */
    bool startMonitor(const QString &rtspUrl);
    void stopMonitor();
    bool isMonitoring() const { return m_isMonitoring; }
    void pauseMonitor();
    void resumeMonitor();

    /* ---- Audio device conflict avoidance ---- */
    void pauseAudioStream();
    void resumeAudioStream();
    bool isAudioStreaming() const;

    /* ---- Status queries ---- */
    bool isModelLoaded() const { return m_rknnCtx != 0; }

signals:
    void signalFaceRecognized(QString uid);
    void signalFaceDetected(bool detected, QRect rect = QRect());
    void signalSpoofDetected(int uid);
    void signalRecordingChanged(bool on);

private:
    void initModels();

    bool loadRknnModel(const char *path, rknn_context *ctx, unsigned char **data, int *size);

    struct PriorBox { float cx, cy, w, h; };
    std::vector<PriorBox> m_detectPriors;
    void generateUltraFacePriors();
    std::vector<FaceRect> detectFaceFd(int src_fd);       /* DMA-BUF zero-copy */
    std::vector<FaceRect> decodeUltraFaceOutputs(rknn_output outputs[2], int img_w, int img_h);

    QVector<float> extractFeatureRknnFd();                 /* m_featureInputFd -> RKNN NPU */

    void saveFeatureModel();
    void loadFeatureModel();

    void monitorThreadFunc();
    void audioThreadFunc();

    /* Camera */
    bool             m_isCameraOpened;
    v4l2_capture_t  *m_v4l2Ctx;
    bool             m_rgaAvailable;
    uint32_t         m_v4l2PixFmt;
    int              m_v4l2Width;
    int              m_v4l2Height;

    /* Stream parameters */
    int              m_streamWidth;
    int              m_streamHeight;
    int              m_streamFps;
    int              m_scaledFd;           /* RGA-scaled NV12 DMA-BUF */
    int              m_yuyvToNv12Fd;       /* YUYV->NV12 DMA-BUF */
    unsigned char   *m_nv12Buf;            /* Software encoding YUYV->NV12 buf */
    unsigned char   *m_scaledBuf;          /* Software encoding scaled NV12 buf */

    /* Face detection */
    rknn_context     m_detectRknnCtx;
    unsigned char   *m_detectRknnData;
    int              m_detectRknnSize;
    int              m_rknnInputFd;        /* Pre-allocated 320x240 RGB DMA-BUF */

    /* Feature extraction */
    rknn_context     m_rknnCtx;
    unsigned char   *m_rknnModelData;
    int              m_rknnModelSize;
    int              m_featureInputFd;     /* Pre-allocated 112x112 RGB DMA-BUF */

    /* Single-face tracking + feature cache */
    int              m_lastFaceRect[4];
    float            m_lastFeature[128];
    int              m_framesSinceReid;
    bool             m_lastFaceValid;
    static constexpr int REID_INTERVAL = 30;

    /* Landmark/liveness */
    bool             m_enableLandmark;
    face_landmark_t *m_landmarkCtx;
    bool             m_landmarkLoaded;
    bool             m_enableAntispoof;
    face_antispoof_t *m_antispoofCtx;
    bool              m_antispoofLoaded;

    /* Recording/OSD */
    mp4_recorder_t  *m_recorder;
    bool             m_isRecording;
    rga_osd_t       *m_osdCtx;
    bool             m_enableOSD;
    int              m_osdDetectInterval;
    int              m_osdFrameCounter;

    /* Feature library */
    QMap<QString, QVector<float>> m_featureMap;

    /* Streaming */
    rtsp_streamer_t *m_streamer;
    int64_t          m_streamingBaseTimeMs;
    std::mutex       m_streamerMtx;
    bool             m_isStreaming;

    /* Monitor */
    bool                     m_isMonitoring;
    std::atomic<bool>        m_monitorPaused;
    QString                  m_monitorRtspUrl;
    std::thread             *m_monitorThread;
    std::atomic<bool>        m_monitorRunning;
    std::mutex               m_monitorMtx;
    std::condition_variable  m_monitorCV;

    /* Audio */
    std::thread             *m_audioThread;
    std::atomic<bool>        m_audioRunning;
    std::atomic<bool>        m_audioPaused;

    /* Constants */
    static constexpr int FEATURE_DIMENSION = 128;
    static constexpr int INPUT_SIZE = 112;
    static constexpr int DETECT_INPUT_W = 320;
    static constexpr int DETECT_INPUT_H = 240;
};

#endif