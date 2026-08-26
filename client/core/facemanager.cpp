#include "facemanager.h"
#include "alsa_capture.h"
#include <QDir>
#include <QDebug>
#include <QTimer>
#include <unistd.h>
#include <cstring>
#include <cJSON.h>
#include <QFile>
#include <QDataStream>
#include <algorithm>
#include <cstdio>




static void yuyv_to_nv12_c(const uint8_t *yuyv, uint8_t *nv12, int w, int h)
{
    uint8_t *y_plane  = nv12;
    uint8_t *uv_plane = nv12 + w * h;
    for (int j = 0; j < h; j += 2) {
        const uint8_t *row0 = yuyv + j * w * 2;
        const uint8_t *row1 = yuyv + (j + 1) * w * 2;
        uint8_t *y0 = y_plane + j * w;
        uint8_t *y1 = y_plane + (j + 1) * w;
        uint8_t *uv = uv_plane + (j / 2) * w;
        for (int i = 0; i < w; i += 2) {
            y0[i]     = row0[i * 2];
            y0[i + 1] = row0[i * 2 + 2];
            y1[i]     = row1[i * 2];
            y1[i + 1] = row1[i * 2 + 2];
            uv[i]     = row0[i * 2 + 1];
            uv[i + 1] = row0[i * 2 + 3];
        }
    }
}


static void nv12_scale_nn(const uint8_t *src, uint8_t *dst, int sw, int sh, int dw, int dh)
{
    for (int j = 0; j < dh; j++) {
        const uint8_t *s = src + (j * sh / dh) * sw;
        uint8_t *d = dst + j * dw;
        for (int i = 0; i < dw; i++) d[i] = s[i * sw / dw];
    }
    for (int j = 0; j < dh / 2; j++) {
        const uint8_t *s = src + sw * sh + (j * sh / dh) * sw;
        uint8_t *d = dst + dw * dh + j * dw;
        for (int i = 0; i < dw; i += 2) {
            int sx = (i * sw / dw) & ~1;
            d[i] = s[sx]; d[i + 1] = s[sx + 1];
        }
    }
}


static float computeIoU(int ax, int ay, int aw, int ah,
                        int bx, int by, int bw, int bh)
{
    int ix1 = std::max(ax, bx);
    int iy1 = std::max(ay, by);
    int ix2 = std::min(ax + aw, bx + bw);
    int iy2 = std::min(ay + ah, by + bh);
    if (ix2 <= ix1 || iy2 <= iy1) return 0.0f;
    float inter = (float)((ix2 - ix1) * (iy2 - iy1));
    float area_a = (float)(aw * ah);
    float area_b = (float)(bw * bh);
    return inter / (area_a + area_b - inter + 1e-6f);
}


/* Convert a BGR888 buffer (as produced by RGA) to an RGB QImage. */
static QImage bgrToQImage(const uint8_t *bgr, int w, int h)
{
    QImage image(bgr, w, h, w * 3, QImage::Format_RGB888);
    return image.rgbSwapped().copy();
}



FaceManager* FaceManager::getInstance()
{
    static FaceManager* instance = nullptr;
    if (!instance) {
        instance = new FaceManager();
    }
    return instance;
}



FaceManager::FaceManager(QObject *parent)
    : QObject(parent)
    , m_isCameraOpened(false)
    , m_v4l2Ctx(nullptr)
    , m_rgaAvailable(false)
    , m_v4l2PixFmt(0)
    , m_v4l2Width(0)
    , m_v4l2Height(0)
    , m_streamWidth(640)
    , m_streamHeight(480)
    , m_streamFps(30)
    , m_scaledFd(-1)
    , m_yuyvToNv12Fd(-1)
    , m_nv12Buf(nullptr)
    , m_scaledBuf(nullptr)
    , m_detectRknnCtx(0)
    , m_detectRknnData(nullptr)
    , m_detectRknnSize(0)
    , m_rknnInputFd(-1)
    , m_rknnCtx(0)
    , m_rknnModelData(nullptr)
    , m_rknnModelSize(0)
    , m_featureInputFd(-1)
    , m_framesSinceReid(0)
    , m_lastFaceValid(false)
    , m_enableLandmark(false)
    , m_landmarkCtx(nullptr)
    , m_landmarkLoaded(false)
    , m_enableAntispoof(false)
    , m_antispoofCtx(nullptr)
    , m_antispoofLoaded(false)
    , m_recorder(nullptr)
    , m_isRecording(false)
    , m_osdCtx(nullptr)
    , m_enableOSD(false)
    , m_osdDetectInterval(5)
    , m_osdFrameCounter(0)
    , m_streamer(nullptr)
    , m_streamingBaseTimeMs(0)
    , m_isStreaming(false)
    , m_isMonitoring(false)
    , m_monitorPaused(false)
    , m_monitorThread(nullptr)
    , m_monitorRunning(false)
    , m_audioThread(nullptr)
    , m_audioRunning(false)
    , m_audioPaused(false)
{
    memset(m_lastFaceRect, 0, sizeof(m_lastFaceRect));
    memset(m_lastFeature, 0, sizeof(m_lastFeature));

    qDebug() << "========================================";
    qDebug() << "FaceManager 初始化开始...";
    qDebug() << "========================================";

    initModels();

    
    m_osdCtx = rga_osd_create(640, 480);
    m_enableOSD = (m_osdCtx != nullptr);

    
    m_rknnInputFd = rga_dma_buf_alloc(DETECT_INPUT_W * DETECT_INPUT_H * 3);
    m_featureInputFd = rga_dma_buf_alloc(INPUT_SIZE * INPUT_SIZE * 3);

    
    loadFeatureModel();

    qDebug() << "\n========================================";
    qDebug() << "初始化完成";
    qDebug() << "采集后端: V4L2 MMAP";
    qDebug() << "检测后端: RKNN NPU";
    qDebug() << "推理后端: RKNN NPU";
    qDebug() << "关键点对齐:" << (m_enableLandmark ? "PFLD NPU" : "禁用");
    qDebug() << "活体检测:" << (m_enableAntispoof ? "MiniFASNet NPU" : "禁用");
    qDebug() << "单人脸跟踪: IoU+帧数缓存, 间隔" << REID_INTERVAL << "帧刷新";
    qDebug() << "OSD叠加:" << (m_enableOSD ? "已启用" : "禁用");
    qDebug() << "========================================\n";
}

FaceManager::~FaceManager()
{
    if (m_isMonitoring) stopMonitor();
    releaseCamera();
    saveFeatureModel();

    if (m_osdCtx) { rga_osd_destroy(&m_osdCtx); m_osdCtx = nullptr; }
    if (m_rknnInputFd >= 0)    { rga_dma_buf_free(m_rknnInputFd);    m_rknnInputFd = -1; }
    if (m_featureInputFd >= 0) { rga_dma_buf_free(m_featureInputFd); m_featureInputFd = -1; }
    if (m_scaledFd >= 0)       { rga_dma_buf_free(m_scaledFd);       m_scaledFd = -1; }
    if (m_yuyvToNv12Fd >= 0)   { rga_dma_buf_free(m_yuyvToNv12Fd);   m_yuyvToNv12Fd = -1; }
    if (m_nv12Buf)   { free(m_nv12Buf);   m_nv12Buf = nullptr; }
    if (m_scaledBuf) { free(m_scaledBuf); m_scaledBuf = nullptr; }
    if (m_recorder)  { mp4_recorder_close(&m_recorder); }

    if (m_rknnCtx) { rknn_destroy(m_rknnCtx); }
    free(m_rknnModelData);

    if (m_detectRknnCtx) { rknn_destroy(m_detectRknnCtx); }
    free(m_detectRknnData);

    if (m_landmarkCtx)   { face_landmark_destroy(m_landmarkCtx);     m_landmarkCtx = nullptr; }
    if (m_antispoofCtx)  { face_antispoof_destroy(m_antispoofCtx);   m_antispoofCtx = nullptr; }
}



void FaceManager::initModels()
{
    
    qDebug() << "\n[1/4] 加载人脸检测模型...";
    {
        QStringList detectPaths = {
            "./face_model/ultraface.rknn", "./face_model/retinaface.rknn",
            "./face_model/scrfd.rknn",     "../face_model/ultraface.rknn",
            "../face_model/retinaface.rknn","../face_model/scrfd.rknn",
            "../build/face_model/ultraface.rknn",
        };
        for (const auto& p : detectPaths) {
            if (QFile::exists(p) && loadRknnModel(p.toUtf8().constData(),
                    &m_detectRknnCtx, &m_detectRknnData, &m_detectRknnSize))
                break;
        }
    }
    qDebug() << "  人脸检测:" << (m_detectRknnCtx ? "RKNN NPU加速" : "加载失败!");

    
    qDebug() << "\n[2/4] 加载MobileFaceNet特征提取模型...";
    {
        QStringList featPaths = {
            "./face_model/mobilefacenet.rknn",
            "../face_model/mobilefacenet.rknn",
            "../build/face_model/mobilefacenet.rknn",
        };
        for (const auto& p : featPaths) {
            if (QFile::exists(p) && loadRknnModel(p.toUtf8().constData(),
                    &m_rknnCtx, &m_rknnModelData, &m_rknnModelSize))
                break;
        }
    }
    qDebug() << "  MobileFaceNet:" << (m_rknnCtx ? "RKNN NPU加速" : "加载失败!");

    
    qDebug() << "\n[3/4] 加载人脸关键点模型...";
    m_landmarkCtx = face_landmark_create();
    if (m_landmarkCtx) {
        QStringList lmPaths = {
            "./face_model/pfld_106.rknn",
            "../face_model/pfld_106.rknn",
            "/opt/retail/face_model/pfld_106.rknn",
        };
        for (const auto& p : lmPaths) {
            if (face_landmark_load_model(m_landmarkCtx, p.toUtf8().constData()) == 0) {
                m_landmarkLoaded = true;
                m_enableLandmark = true;
                break;
            }
        }
        if (!m_enableLandmark) {
            face_landmark_destroy(m_landmarkCtx);
            m_landmarkCtx = nullptr;
        }
    }
    qDebug() << "  关键点检测:" << (m_enableLandmark ? "PFLD-106 NPU" : "未加载");

    
    qDebug() << "\n[4/4] 加载活体检测模型...";
    m_antispoofCtx = face_antispoof_create();
    if (m_antispoofCtx) {
        QStringList asPaths = {
            "./face_model/miniFASNet.rknn",
            "../face_model/miniFASNet.rknn",
            "/opt/retail/face_model/miniFASNet.rknn",
        };
        for (const auto& p : asPaths) {
            if (face_antispoof_load_model(m_antispoofCtx, p.toUtf8().constData()) == 0) {
                m_antispoofLoaded = true;
                m_enableAntispoof = true;
                break;
            }
        }
        if (!m_enableAntispoof) {
            face_antispoof_destroy(m_antispoofCtx);
            m_antispoofCtx = nullptr;
        }
    }
    qDebug() << "  活体检测:" << (m_enableAntispoof ? "MiniFASNet NPU" : "未加载");
}




bool FaceManager::loadRknnModel(const char *path, rknn_context *ctx,
                                 unsigned char **data, int *size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    *size = (int)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    *data = (unsigned char*)malloc(*size);
    if (!*data) { fclose(fp); return false; }

    size_t read = fread(*data, 1, *size, fp);
    fclose(fp);

    if (read != (size_t)*size) {
        free(*data); *data = nullptr;
        return false;
    }

    int ret = rknn_init(ctx, *data, (uint32_t)*size, 0, NULL);
    if (ret != 0) {
        free(*data); *data = nullptr;
        return false;
    }

    qDebug() << "  RKNN模型加载成功:" << path;
    return true;
}



void FaceManager::setStreamParams(int width, int height, int fps)
{
    m_streamWidth  = width;
    m_streamHeight = height;
    m_streamFps    = fps;
    qDebug() << "推流参数:" << width << "x" << height << "@" << fps << "fps";
}

bool FaceManager::initCamera(int cameraIndex)
{
    if (m_isCameraOpened) return true;

    int capW = (m_streamWidth > 320) ? 320 : m_streamWidth;
    int capH = (m_streamHeight > 240) ? 240 : m_streamHeight;

    
    m_v4l2Ctx = rkisp_camera_open(nullptr, capW, capH, m_streamFps);
    if (m_v4l2Ctx) {
        qInfo() << "MIPI 摄像头打开成功 (rkisp + rkaiq 3A)";
    } else {
        char devPath[32];
        if (v4l2_find_usb_camera(devPath, sizeof(devPath)) != 0) {
            snprintf(devPath, sizeof(devPath), "/dev/video%d", cameraIndex);
            qWarning() << "未发现USB摄像头, 降级使用" << devPath;
        }
        m_v4l2Ctx = v4l2_capture_open(devPath, capW, capH, m_streamFps);
        if (!m_v4l2Ctx) {
            qWarning() << "V4L2 MMAP打开失败! 设备:" << devPath;
            return false;
        }
    }

    m_v4l2PixFmt  = m_v4l2Ctx->pixfmt;
    m_v4l2Width   = m_v4l2Ctx->width;
    m_v4l2Height  = m_v4l2Ctx->height;
    m_rgaAvailable = rga_transform_available();

    m_isCameraOpened = true;
    qDebug() << "摄像头V4L2打开成功"
             << m_v4l2Width << "x" << m_v4l2Height
             << "格式:" << v4l2_pixfmt_name(m_v4l2PixFmt)
             << "RGA:" << (m_rgaAvailable ? "硬件加速" : "不可用");
    return true;
}

QImage FaceManager::getCameraFrame()
{
    QImage frame;
    if (!m_isCameraOpened || !m_v4l2Ctx) return frame;

    void  *frameBuf = nullptr;
    size_t frameLen = 0;
    if (v4l2_capture_dequeue(m_v4l2Ctx, &frameBuf, &frameLen) < 0)
        return frame;

    if (m_rgaAvailable) {
        rga_image_t src = {};
        src.vir_addr = frameBuf;
        src.width    = m_v4l2Width;
        src.height   = m_v4l2Height;
        src.format   = (m_v4l2PixFmt == V4L2_PIX_FMT_YUYV) ? RGA_FMT_YUYV : RGA_FMT_NV12;

        QByteArray bgrBuf(m_v4l2Width * m_v4l2Height * 3, 0);

        rga_image_t dst = {};
        dst.vir_addr = bgrBuf.data();
        dst.width    = m_v4l2Width;
        dst.height   = m_v4l2Height;
        dst.format   = RGA_FMT_BGR888;

        if (rga_transform_process(&src, &dst, nullptr) == 0) {
            if (m_isRecording && m_recorder)
                mp4_recorder_write_bgr(m_recorder, (const uint8_t*)bgrBuf.data(), bgrBuf.size());
            frame = bgrToQImage((const uint8_t*)bgrBuf.constData(), m_v4l2Width, m_v4l2Height);
        }
    }

    v4l2_capture_enqueue(m_v4l2Ctx);

    return frame;
}

void FaceManager::releaseCamera()
{
    if (!m_isCameraOpened) return;
    if (m_v4l2Ctx) v4l2_capture_close(&m_v4l2Ctx);
    m_v4l2PixFmt = 0;
    m_v4l2Width = m_v4l2Height = 0;
    m_isCameraOpened = false;
    qDebug() << "摄像头已释放";
}



void FaceManager::generateUltraFacePriors()
{
    constexpr int image_w = DETECT_INPUT_W;
    constexpr int image_h = DETECT_INPUT_H;
    constexpr float strides[] = {8.0f, 16.0f, 32.0f, 64.0f};
    constexpr int min_boxes_per_level[][3] = {
        {10, 16, 24}, {32, 48, -1}, {64, 96, -1}, {128, 192, 256}
    };
    constexpr int boxes_per_level[] = {3, 2, 2, 3};

    m_detectPriors.clear();
    for (int level = 0; level < 4; level++) {
        float stride = strides[level];
        int feature_w = (int)std::ceil(image_w / stride);
        int feature_h = (int)std::ceil(image_h / stride);
        for (int j = 0; j < feature_h; j++) {
            for (int i = 0; i < feature_w; i++) {
                float cx = (i + 0.5f) / feature_w;
                float cy = (j + 0.5f) / feature_h;
                for (int k = 0; k < boxes_per_level[level]; k++) {
                    int min_box = min_boxes_per_level[level][k];
                    PriorBox pb;
                    pb.cx = std::max(0.0f, std::min(1.0f, cx));
                    pb.cy = std::max(0.0f, std::min(1.0f, cy));
                    pb.w  = std::max(0.0f, std::min(1.0f, (float)min_box / image_w));
                    pb.h  = std::max(0.0f, std::min(1.0f, (float)min_box / image_h));
                    m_detectPriors.push_back(pb);
                }
            }
        }
    }
    qDebug() << "  UltraFace先验框已生成:" << m_detectPriors.size() << "个";
}


std::vector<FaceRect> FaceManager::detectFace(const QImage& frame)
{
    std::vector<FaceRect> faces;
    if (frame.isNull() || !m_detectRknnCtx) return faces;

    QImage rgb = frame.convertToFormat(QImage::Format_RGB888);
    QImage resized = rgb.scaled(DETECT_INPUT_W, DETECT_INPUT_H,
                                Qt::IgnoreAspectRatio, Qt::FastTransformation);

    rknn_input inputs[1];
    memset(&inputs[0], 0, sizeof(rknn_input));
    inputs[0].index = 0;
    inputs[0].buf   = resized.bits();
    inputs[0].size  = DETECT_INPUT_W * DETECT_INPUT_H * 3;
    inputs[0].type  = RKNN_TENSOR_UINT8;
    inputs[0].fmt   = RKNN_TENSOR_NHWC;

    if (rknn_inputs_set(m_detectRknnCtx, 1, inputs) < 0) return faces;
    if (rknn_run(m_detectRknnCtx, NULL) < 0) return faces;

    rknn_output outputs[2];
    memset(outputs, 0, sizeof(outputs));
    outputs[0].want_float = 1; outputs[0].is_prealloc = 0;
    outputs[1].want_float = 1; outputs[1].is_prealloc = 0;
    if (rknn_outputs_get(m_detectRknnCtx, 2, outputs, NULL) < 0) return faces;

    faces = decodeUltraFaceOutputs(outputs, frame.width(), frame.height());

    rknn_outputs_release(m_detectRknnCtx, 2, outputs);
    return faces;
}


std::vector<FaceRect> FaceManager::detectFaceFd(int src_fd)
{
    std::vector<FaceRect> faces;
    if (src_fd < 0 || !m_detectRknnCtx || m_rknnInputFd < 0 || !m_rgaAvailable)
        return faces;

    rga_image_t src = {};
    src.fd     = src_fd;
    src.width  = m_v4l2Width;
    src.height = m_v4l2Height;
    src.format = RGA_FMT_NV12;

    rga_image_t dst = {};
    dst.fd      = m_rknnInputFd;
    dst.width   = DETECT_INPUT_W;
    dst.height  = DETECT_INPUT_H;
    dst.format  = RGA_FMT_RGB888;

    if (rga_transform_process(&src, &dst, nullptr) < 0) {
        fprintf(stderr, "[FaceDetect] RGA DMA-BUF transform 失败\n");
        return faces;
    }

    rknn_input inputs[1];
    memset(&inputs[0], 0, sizeof(rknn_input));
    inputs[0].index        = 0;
    inputs[0].type         = RKNN_TENSOR_UINT8;
    inputs[0].fmt          = RKNN_TENSOR_NHWC;
    inputs[0].size         = DETECT_INPUT_W * DETECT_INPUT_H * 3;
    inputs[0].buf          = (void*)(intptr_t)m_rknnInputFd;
    inputs[0].pass_through = 1;

    if (rknn_inputs_set(m_detectRknnCtx, 1, inputs) < 0) return faces;
    if (rknn_run(m_detectRknnCtx, NULL) < 0) return faces;

    rknn_output outputs[2];
    memset(outputs, 0, sizeof(outputs));
    outputs[0].want_float = 1; outputs[0].is_prealloc = 0;
    outputs[1].want_float = 1; outputs[1].is_prealloc = 0;
    if (rknn_outputs_get(m_detectRknnCtx, 2, outputs, NULL) < 0) return faces;

    faces = decodeUltraFaceOutputs(outputs, m_v4l2Width, m_v4l2Height);

    rknn_outputs_release(m_detectRknnCtx, 2, outputs);
    return faces;
}


std::vector<FaceRect> FaceManager::decodeUltraFaceOutputs(rknn_output outputs[2], int img_w, int img_h)
{
    std::vector<FaceRect> faces;

    float *scores = (float*)outputs[0].buf;
    float *boxes  = (float*)outputs[1].buf;
    int num_anchors = (int)(outputs[0].size / sizeof(float)) / 2;

    if (m_detectPriors.empty())
        generateUltraFacePriors();

    constexpr float center_variance = 0.1f;
    constexpr float size_variance   = 0.2f;
    constexpr float conf_threshold  = 0.7f;
    constexpr float nms_threshold   = 0.3f;

    struct DetBox { float x1, y1, x2, y2, score; };
    std::vector<DetBox> candidates;

    for (int i = 0; i < num_anchors && i < (int)m_detectPriors.size(); i++) {
        float fg_score = scores[i * 2 + 1];
        if (fg_score < conf_threshold) continue;

        float dcx = boxes[i * 4 + 0];
        float dcy = boxes[i * 4 + 1];
        float dw  = std::max(-5.0f, std::min(5.0f, boxes[i * 4 + 2]));
        float dh  = std::max(-5.0f, std::min(5.0f, boxes[i * 4 + 3]));

        float cx = dcx * center_variance * m_detectPriors[i].w + m_detectPriors[i].cx;
        float cy = dcy * center_variance * m_detectPriors[i].h + m_detectPriors[i].cy;
        float w  = std::exp(dw * size_variance) * m_detectPriors[i].w;
        float h  = std::exp(dh * size_variance) * m_detectPriors[i].h;

        DetBox db;
        db.x1 = std::max(0.0f, (cx - w * 0.5f) * img_w);
        db.y1 = std::max(0.0f, (cy - h * 0.5f) * img_h);
        db.x2 = std::min((float)img_w, (cx + w * 0.5f) * img_w);
        db.y2 = std::min((float)img_h, (cy + h * 0.5f) * img_h);
        db.score = fg_score;

        if (db.x2 - db.x1 > 10 && db.y2 - db.y1 > 10)
            candidates.push_back(db);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const DetBox& a, const DetBox& b) { return a.score > b.score; });

    std::vector<bool> suppressed(candidates.size(), false);
    for (size_t i = 0; i < candidates.size(); i++) {
        if (suppressed[i]) continue;
        faces.push_back(FaceRect{
            (int)candidates[i].x1, (int)candidates[i].y1,
            (int)(candidates[i].x2 - candidates[i].x1),
            (int)(candidates[i].y2 - candidates[i].y1)});
        for (size_t j = i + 1; j < candidates.size(); j++) {
            if (suppressed[j]) continue;
            float ix1 = std::max(candidates[i].x1, candidates[j].x1);
            float iy1 = std::max(candidates[i].y1, candidates[j].y1);
            float ix2 = std::min(candidates[i].x2, candidates[j].x2);
            float iy2 = std::min(candidates[i].y2, candidates[j].y2);
            float inter = std::max(0.0f, ix2 - ix1) * std::max(0.0f, iy2 - iy1);
            float area_i = (candidates[i].x2 - candidates[i].x1) * (candidates[i].y2 - candidates[i].y1);
            float area_j = (candidates[j].x2 - candidates[j].x1) * (candidates[j].y2 - candidates[j].y1);
            if (inter / (area_i + area_j - inter + 1e-6f) > nms_threshold)
                suppressed[j] = true;
        }
    }
    return faces;
}




QVector<float> FaceManager::extractFeatureRknnFd()
{
    if (m_featureInputFd < 0 || !m_rknnCtx) return QVector<float>();

    rknn_input inputs[1];
    memset(&inputs[0], 0, sizeof(rknn_input));
    inputs[0].index        = 0;
    inputs[0].buf          = (void*)(intptr_t)m_featureInputFd;
    inputs[0].size         = INPUT_SIZE * INPUT_SIZE * 3;
    inputs[0].type         = RKNN_TENSOR_UINT8;
    inputs[0].fmt          = RKNN_TENSOR_NHWC;
    inputs[0].pass_through = 1;

    if (rknn_inputs_set(m_rknnCtx, 1, inputs) < 0) return QVector<float>();
    if (rknn_run(m_rknnCtx, NULL) < 0) return QVector<float>();

    rknn_output outputs[1];
    memset(&outputs[0], 0, sizeof(rknn_output));
    outputs[0].want_float  = 1;
    outputs[0].is_prealloc = 0;
    if (rknn_outputs_get(m_rknnCtx, 1, outputs, NULL) < 0) return QVector<float>();

    float *output_data = (float*)outputs[0].buf;
    int dim = (int)(outputs[0].size / sizeof(float));

    QVector<float> feature(dim);
    float norm = 0.0f;
    for (int i = 0; i < dim; i++) {
        feature[i] = output_data[i];
        norm += output_data[i] * output_data[i];
    }
    norm = sqrt(norm);
    if (norm > 0) {
        for (int i = 0; i < dim; i++) feature[i] /= norm;
    }

    rknn_outputs_release(m_rknnCtx, 1, outputs);
    return feature;
}


QVector<float> FaceManager::extractFeatureVector(const QImage& frame, const FaceRect& roi)
{
    if (!m_rknnCtx || m_featureInputFd < 0 || frame.isNull() || !m_rgaAvailable)
        return QVector<float>();

    QImage faceCrop = frame.copy(roi.x, roi.y, roi.width, roi.height)
                          .convertToFormat(QImage::Format_RGB888);

    rga_image_t src = {};
    src.vir_addr = faceCrop.bits();
    src.width    = faceCrop.width();
    src.height   = faceCrop.height();
    src.format   = RGA_FMT_RGB888;

    rga_image_t dst = {};
    dst.fd       = m_featureInputFd;
    dst.width    = INPUT_SIZE;
    dst.height   = INPUT_SIZE;
    dst.format   = RGA_FMT_RGB888;

    if (rga_transform_process(&src, &dst, nullptr) != 0) {
        qWarning() << "[Feature] RGA 准备特征输入失败";
        return QVector<float>();
    }
    return extractFeatureRknnFd();
}



bool FaceManager::registerFace(const QString& uid, const QImage& faceFrame)
{
    std::vector<FaceRect> faces = detectFace(faceFrame);
    if (faces.empty()) { qWarning() << "注册失败：未检测到人脸"; return false; }

    FaceRect mainFace = faces[0];
    QVector<float> feature = extractFeatureVector(faceFrame, mainFace);
    if (feature.isEmpty()) { qWarning() << "注册失败：特征提取失败"; return false; }

    m_featureMap[uid] = feature;
    saveFeatureModel();
    qDebug() << "会员" << uid << "人脸注册成功, 已注册总数：" << m_featureMap.size();
    return true;
}

QString FaceManager::extractFaceFeature(const QImage& faceFrame)
{
    std::vector<FaceRect> faces = detectFace(faceFrame);
    if (faces.empty()) return "";

    FaceRect mainFace = faces[0];
    QImage rgbFrame = faceFrame.convertToFormat(QImage::Format_RGB888);

    
    if (m_antispoofCtx && m_enableAntispoof) {
        int roi[4] = { mainFace.x, mainFace.y,
                       mainFace.x + mainFace.width, mainFace.y + mainFace.height };
        face_liveness_result liveness = face_antispoof_check(m_antispoofCtx,
                                                              rgbFrame.bits(),
                                                              rgbFrame.width(), rgbFrame.height(),
                                                              roi);
        if (liveness == LIVENESS_SPOOF) {
            emit signalSpoofDetected(-1);
            qDebug() << "[AntiSpoof] 检测到攻击(照片/视频)";
        }
    }

    
    m_framesSinceReid++;
    if (m_lastFaceValid) {
        float iou = computeIoU(mainFace.x, mainFace.y, mainFace.width, mainFace.height,
                               m_lastFaceRect[0], m_lastFaceRect[1],
                               m_lastFaceRect[2], m_lastFaceRect[3]);
        if (iou > 0.3f && m_framesSinceReid < REID_INTERVAL) {
            cJSON* root = cJSON_CreateObject();
            cJSON* featArray = cJSON_CreateArray();
            for (int j = 0; j < FEATURE_DIMENSION; j++)
                cJSON_AddItemToArray(featArray, cJSON_CreateNumber(m_lastFeature[j]));
            cJSON_AddStringToObject(root, "version", "mobilefacenet_v1");
            cJSON_AddNumberToObject(root, "dimension", FEATURE_DIMENSION);
            cJSON_AddItemToObject(root, "feature", featArray);
            char* jsonBuf = cJSON_PrintUnformatted(root);
            QString jsonStr = QString::fromUtf8(jsonBuf);
            cJSON_Delete(root);
            free(jsonBuf);
            return jsonStr;
        }
    }

    
    QImage alignedFace;
    if (m_landmarkCtx && m_enableLandmark) {
        int roi[4] = { mainFace.x, mainFace.y,
                       mainFace.x + mainFace.width, mainFace.y + mainFace.height };
        face_5point_t lm_points;
        if (face_landmark_detect(m_landmarkCtx, rgbFrame.bits(),
                                  rgbFrame.width(), rgbFrame.height(),
                                  roi, &lm_points) == 0) {
            uint8_t aligned_buf[112 * 112 * 3];
            if (face_landmark_align(m_landmarkCtx, rgbFrame.bits(),
                                     rgbFrame.width(), rgbFrame.height(),
                                     &lm_points, aligned_buf) == 0) {
                alignedFace = QImage(aligned_buf, 112, 112, 112 * 3,
                                     QImage::Format_RGB888).copy();
            }
        }
    }
    if (alignedFace.isNull())
        alignedFace = faceFrame.copy(mainFace.x, mainFace.y, mainFace.width, mainFace.height)
                          .convertToFormat(QImage::Format_RGB888);

    
    QVector<float> feature = extractFeatureVector(
        alignedFace, FaceRect{0, 0, alignedFace.width(), alignedFace.height()});
    if (feature.isEmpty()) return "";

    
    m_lastFaceRect[0] = mainFace.x;
    m_lastFaceRect[1] = mainFace.y;
    m_lastFaceRect[2] = mainFace.width;
    m_lastFaceRect[3] = mainFace.height;
    memcpy(m_lastFeature, feature.data(), FEATURE_DIMENSION * sizeof(float));
    m_lastFaceValid = true;
    m_framesSinceReid = 0;

    
    cJSON* root = cJSON_CreateObject();
    cJSON* featArray = cJSON_CreateArray();
    for (int i = 0; i < feature.size(); ++i)
        cJSON_AddItemToArray(featArray, cJSON_CreateNumber(feature[i]));
    cJSON_AddStringToObject(root, "version", "mobilefacenet_v1");
    cJSON_AddNumberToObject(root, "dimension", FEATURE_DIMENSION);
    cJSON_AddItemToObject(root, "feature", featArray);
    char* jsonBuf = cJSON_PrintUnformatted(root);
    QString jsonStr = QString::fromUtf8(jsonBuf);
    cJSON_Delete(root);
    free(jsonBuf);
    return jsonStr;
}

QImage FaceManager::captureFace()
{
    QImage frame = getCameraFrame();
    if (frame.isNull()) return QImage();

    std::vector<FaceRect> faces = detectFace(frame);
    if (faces.empty()) return QImage();

    FaceRect mainFace = faces[0];
    return frame.copy(mainFace.x, mainFace.y, mainFace.width, mainFace.height);
}

void FaceManager::removeFaceData(const QString& uid)
{
    if (m_featureMap.contains(uid)) {
        m_featureMap.remove(uid);
        saveFeatureModel();
    }
}



void FaceManager::saveFeatureModel()
{
    QFile file("./data/face_features.dat");
    if (file.open(QIODevice::WriteOnly)) {
        QDataStream out(&file);
        out << m_featureMap;
        file.close();
    }
}

void FaceManager::loadFeatureModel()
{
    QFile file("./data/face_features.dat");
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        QDataStream in(&file);
        in >> m_featureMap;
        file.close();
        qDebug() << "人脸特征库加载成功 - 用户数：" << m_featureMap.size();
    }
}



bool FaceManager::startStreaming(const QString &rtspUrl)
{
    if (m_isStreaming) return false;

    rtsp_streamer_config_t config;
    memset(&config, 0, sizeof(config));
    strncpy(config.rtsp_url, rtspUrl.toUtf8().constData(), sizeof(config.rtsp_url) - 1);
    config.width  = m_streamWidth;
    config.height = m_streamHeight;
    config.fps    = m_streamFps;
    config.video_bit_rate = m_streamWidth * m_streamHeight * m_streamFps * 0.10;
    config.audio_sample_rate = 0;
    config.audio_channels    = 1;
    config.audio_bit_rate    = 64000;
    config.use_tcp = true;
    config.base_time_ms = m_streamingBaseTimeMs;

    m_streamer = rtsp_streamer_open(&config);
    if (!m_streamer) return false;

    if (m_streamingBaseTimeMs == 0)
        m_streamingBaseTimeMs = m_streamer->start_time_ms;

    m_isStreaming = true;
    qDebug() << "开始RTSP推流:" << rtspUrl
             << (config.base_time_ms > 0 ? "(重连,保留PTS基准)" : "(首次连接)");
    return true;
}

void FaceManager::stopStreaming()
{
    if (!m_isStreaming) return;
    if (m_streamer) {
        m_streamingBaseTimeMs = m_streamer->start_time_ms;
        rtsp_streamer_close(&m_streamer);
    }
    m_isStreaming = false;
    qDebug() << "RTSP推流已停止";
}



bool FaceManager::startRecording(const QString &outputPath)
{
    if (m_isRecording || !m_isCameraOpened) return false;

    mp4_recorder_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.output_path, outputPath.toUtf8().constData(), sizeof(cfg.output_path) - 1);
    cfg.width = 640; cfg.height = 480;
    cfg.fps = 15;
    cfg.video_bit_rate = 640 * 480 * 15 * 0.15;
    cfg.gop_size = 30;
    cfg.enable_audio = false;
    cfg.faststart = true;

    m_recorder = mp4_recorder_open(&cfg);
    if (!m_recorder) return false;

    m_isRecording = true;
    emit signalRecordingChanged(true);
    qDebug() << "开始录像:" << outputPath;
    return true;
}

void FaceManager::stopRecording()
{
    if (!m_isRecording || !m_recorder) return;
    mp4_recorder_close(&m_recorder);
    m_isRecording = false;
    emit signalRecordingChanged(false);
    qDebug() << "录像已停止";
}



void FaceManager::pauseAudioStream()
{
    if (!m_isMonitoring) {
        qDebug() << "[Audio] 非监控状态, 忽略pauseAudioStream";
        return;
    }
    m_audioPaused.store(true, std::memory_order_release);
    qDebug() << "[Audio] 推流音轨已暂停 (释放麦克风给语音识别)";
}

void FaceManager::resumeAudioStream()
{
    if (!m_isMonitoring) {
        qDebug() << "[Audio] 非监控状态, 忽略resumeAudioStream";
        return;
    }
    m_audioPaused.store(false, std::memory_order_release);
    qDebug() << "[Audio] 推流音轨已恢复";
}

bool FaceManager::isAudioStreaming() const
{
    return m_audioRunning.load(std::memory_order_acquire) &&
           !m_audioPaused.load(std::memory_order_acquire);
}



void FaceManager::monitorThreadFunc()
{
    qDebug() << "[MonitorThread] 推流线程启动, rtsp:" << m_monitorRtspUrl;

    constexpr int MAX_CONSECUTIVE_FAILS    = 30;
    constexpr int MAX_CAMERA_RETRIES       = 3;
    constexpr int MAX_RTSP_RETRIES         = 3;
    constexpr int MAX_CONSECUTIVE_SENDFAIL = 10;
    constexpr int STABLE_FRAMES_TO_RESET   = 100;

    int consecutive_capture_fail = 0;
    int consecutive_send_fail    = 0;
    int camera_retries           = 0;
    int rtsp_retries             = 0;
    int stable_frame_count       = 0;

    while (m_monitorRunning.load(std::memory_order_acquire)) {
        
        if (m_monitorPaused.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(m_monitorMtx);
            m_monitorCV.wait(lock, [this]() {
                return !m_monitorPaused.load(std::memory_order_acquire)
                    || !m_monitorRunning.load(std::memory_order_acquire);
            });
        }
        if (!m_monitorRunning.load(std::memory_order_acquire)) break;

        bool frame_ok  = false;
        bool send_ok   = true;
        bool conn_lost = false;

        if (m_v4l2Ctx && m_streamer) {
            void  *frameBuf = nullptr;
            size_t frameLen = 0;

            if (v4l2_capture_dequeue(m_v4l2Ctx, &frameBuf, &frameLen) == 0 && frameBuf) {
                int frame_fd = v4l2_capture_get_fd(m_v4l2Ctx);
                bool is_yuyv = (m_v4l2PixFmt == V4L2_PIX_FMT_YUYV);
                bool use_sw  = m_streamer->video_encoder && !m_streamer->video_encoder->is_hw_encoder;

                /* YUYV→NV12 */
                if (is_yuyv) {
                    if (use_sw && m_nv12Buf) {
                        yuyv_to_nv12_c((const uint8_t *)frameBuf, m_nv12Buf,
                                       m_v4l2Width, m_v4l2Height);
                    } else if (m_yuyvToNv12Fd >= 0) {
                        rga_image_t yuyv_src = {};
                        yuyv_src.fd     = frame_fd;
                        yuyv_src.width  = m_v4l2Width;
                        yuyv_src.height = m_v4l2Height;
                        yuyv_src.format = RGA_FMT_YUYV;

                        rga_image_t nv12_dst = {};
                        nv12_dst.fd     = m_yuyvToNv12Fd;
                        nv12_dst.width  = m_v4l2Width;
                        nv12_dst.height = m_v4l2Height;
                        nv12_dst.format = RGA_FMT_NV12;

                        if (rga_transform_process(&yuyv_src, &nv12_dst, nullptr) != 0) {
                            qWarning() << "[MonitorThread] YUYV→NV12 RGA转换失败";
                            v4l2_capture_enqueue(m_v4l2Ctx);
                            continue;
                        }
                    }
                }

                
                if (m_enableOSD && m_osdCtx && !is_yuyv) {
                    m_osdFrameCounter++;
                    rga_osd_draw_t draw;
                    memset(&draw, 0, sizeof(draw));

                    if (m_osdFrameCounter >= m_osdDetectInterval) {
                        m_osdFrameCounter = 0;
                        std::vector<FaceRect> osdFaces = detectFaceFd(frame_fd);
                        if (!osdFaces.empty()) {
                            static rga_osd_rect_t faceRects[16];
                            int n = (int)osdFaces.size() > 16 ? 16 : (int)osdFaces.size();
                            for (int i = 0; i < n; i++) {
                                faceRects[i].x = osdFaces[i].x;
                                faceRects[i].y = osdFaces[i].y;
                                faceRects[i].w = osdFaces[i].width;
                                faceRects[i].h = osdFaces[i].height;
                                faceRects[i].r = 0; faceRects[i].g = 255; faceRects[i].b = 0;
                                faceRects[i].thickness = 2;
                                faceRects[i].corner_radius = 0;
                            }
                            draw.rects = faceRects;
                            draw.rect_count = n;
                        }
                    }
                    rga_osd_draw_nv12(m_osdCtx, frame_fd, &draw);
                }

                
                int send_ret = -1;
                if (use_sw) {
                    const uint8_t *send_ptr = nullptr;
                    int send_size = 0;

                    if (is_yuyv && m_nv12Buf) {
                        send_ptr  = m_nv12Buf;
                        send_size = m_v4l2Width * m_v4l2Height * 3 / 2;
                    } else {
                        send_ptr  = (const uint8_t *)frameBuf;
                        send_size = (int)frameLen;
                    }

                    if (m_scaledBuf &&
                        (m_v4l2Width != m_streamWidth || m_v4l2Height != m_streamHeight)) {
                        bool scaled = false;
                        if (m_rgaAvailable) {
                            rga_image_t src = {};
                            src.vir_addr = (void *)send_ptr;
                            src.width    = m_v4l2Width;
                            src.height   = m_v4l2Height;
                            src.format   = RGA_FMT_NV12;
                            rga_image_t dst = {};
                            dst.vir_addr = m_scaledBuf;
                            dst.width    = m_streamWidth;
                            dst.height   = m_streamHeight;
                            dst.format   = RGA_FMT_NV12;
                            scaled = (rga_transform_process(&src, &dst, nullptr) == 0);
                        }
                        if (!scaled) {
                            nv12_scale_nn(send_ptr, m_scaledBuf,
                                          m_v4l2Width, m_v4l2Height,
                                          m_streamWidth, m_streamHeight);
                        }
                        send_ptr  = m_scaledBuf;
                        send_size = m_streamWidth * m_streamHeight * 3 / 2;
                    }
                    send_ret = rtsp_streamer_send_nv12_ptr(m_streamer, send_ptr, send_size);
                } else {
                    int send_fd   = -1;
                    int send_size = 0;

                    if (is_yuyv && m_yuyvToNv12Fd >= 0) {
                        send_fd   = m_yuyvToNv12Fd;
                        send_size = m_v4l2Width * m_v4l2Height * 3 / 2;
                    } else {
                        send_fd   = frame_fd;
                        send_size = (int)frameLen;
                    }

                    if (m_scaledFd >= 0 && m_rgaAvailable &&
                        (m_v4l2Width != m_streamWidth || m_v4l2Height != m_streamHeight)) {
                        rga_image_t src = {};
                        src.fd     = send_fd;
                        src.width  = m_v4l2Width;
                        src.height = m_v4l2Height;
                        src.format = RGA_FMT_NV12;

                        rga_image_t dst = {};
                        dst.fd     = m_scaledFd;
                        dst.width  = m_streamWidth;
                        dst.height = m_streamHeight;
                        dst.format = RGA_FMT_NV12;

                        if (rga_transform_process(&src, &dst, nullptr) == 0) {
                            send_fd   = m_scaledFd;
                            send_size = m_streamWidth * m_streamHeight * 3 / 2;
                        }
                    }
                    send_ret = rtsp_streamer_send_nv12(m_streamer, send_fd, send_size);
                }

                if (m_v4l2Ctx) v4l2_capture_enqueue(m_v4l2Ctx);
                frame_ok = true;
                send_ok  = (send_ret >= 0);

                if (send_ret < 0) {
                    conn_lost = m_streamer && !m_streamer->connected;
                    if (conn_lost) {
                        ++rtsp_retries;
                        qWarning() << "[MonitorThread] RTSP连接断开 ("
                                   << rtsp_retries << "/" << MAX_RTSP_RETRIES << ")";
                    } else {
                        ++consecutive_send_fail;
                        if (consecutive_send_fail % 5 == 1) {
                            qWarning() << "[MonitorThread] 发送失败(连接未断), 累计"
                                       << consecutive_send_fail << "次";
                        }
                    }
                }
            }
        }

        
        if (conn_lost) {
            {
                std::lock_guard<std::mutex> lk(m_streamerMtx);
                stopStreaming();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            {
                std::lock_guard<std::mutex> lk(m_streamerMtx);
                if (startStreaming(m_monitorRtspUrl)) {
                    rtsp_retries = 0;
                    consecutive_send_fail = 0;
                    qDebug() << "[MonitorThread] RTSP重连成功";
                } else if (rtsp_retries >= MAX_RTSP_RETRIES) {
                    qCritical() << "[MonitorThread] RTSP重连" << rtsp_retries << "次仍失败, 退出监控";
                    break;
                } else {
                    std::this_thread::sleep_for(std::chrono::seconds(1 << (rtsp_retries - 1)));
                    continue;
                }
            }
        }

        
        if (!conn_lost && consecutive_send_fail >= MAX_CONSECUTIVE_SENDFAIL && m_streamer) {
            qWarning() << "[MonitorThread] 连续发送失败" << consecutive_send_fail
                       << "次(连接未断), 主动重连";
            {
                std::lock_guard<std::mutex> lk(m_streamerMtx);
                stopStreaming();
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                startStreaming(m_monitorRtspUrl);
            }
            consecutive_send_fail = 0;
        }

        if (!frame_ok) {
            ++consecutive_capture_fail;
            if (consecutive_capture_fail >= MAX_CONSECUTIVE_FAILS) {
                qWarning() << "[MonitorThread] 采集连续失败" << consecutive_capture_fail
                           << "次, 重启摄像头 (" << (camera_retries + 1)
                           << "/" << MAX_CAMERA_RETRIES << ")";
                releaseCamera();
                if (initCamera(0)) {
                    consecutive_capture_fail = 0;
                    ++camera_retries;
                } else {
                    ++camera_retries;
                    if (camera_retries >= MAX_CAMERA_RETRIES) {
                        qCritical() << "[MonitorThread] 摄像头重启" << camera_retries
                                    << "次仍失败, 退出监控";
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
            continue;
        }

        consecutive_capture_fail = 0;
        consecutive_send_fail = 0;
        if (send_ok) rtsp_retries = 0;

        ++stable_frame_count;
        if (stable_frame_count >= STABLE_FRAMES_TO_RESET) {
            if (camera_retries > 0) {
                qDebug() << "[MonitorThread] 稳定运行" << STABLE_FRAMES_TO_RESET
                         << "帧, 清零摄像头重启计数(原" << camera_retries << ")";
            }
            camera_retries = 0;
            stable_frame_count = 0;
        }
    }
    qDebug() << "[MonitorThread] 推流线程退出";
}

void FaceManager::audioThreadFunc()
{
    qDebug() << "[AudioThread] 音频推流线程启动";

    const unsigned int SAMPLE_RATE  = 16000;
    const unsigned int CHANNELS     = 1;
    const unsigned int PERIOD_SIZE  = 1024;
    const char *ALSA_DEVICE         = "default";
    const int BUF_SIZE = PERIOD_SIZE * CHANNELS * 2;

    uint8_t *pcmBuf = (uint8_t*)malloc(BUF_SIZE);
    if (!pcmBuf) {
        qWarning() << "[AudioThread] PCM缓冲区分配失败";
        return;
    }

    alsa_capture_t *alsaCtx = nullptr;

    while (m_audioRunning.load(std::memory_order_acquire)) {
        if (m_audioPaused.load(std::memory_order_acquire)) {
            if (alsaCtx) {
                qDebug() << "[AudioThread] 暂停: 释放ALSA设备给语音识别";
                alsa_capture_close(&alsaCtx);
                alsaCtx = nullptr;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (!alsaCtx) {
            alsaCtx = alsa_capture_open(ALSA_DEVICE, SAMPLE_RATE, CHANNELS, PERIOD_SIZE);
            if (!alsaCtx) {
                qWarning() << "[AudioThread] ALSA设备重新打开失败, 1s后重试";
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            qDebug() << "[AudioThread] ALSA设备已重新打开, 继续推流";
        }

        int readFrames = alsa_capture_read(alsaCtx, pcmBuf, PERIOD_SIZE);
        if (readFrames <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        bool streamerOk = false;
        {
            std::lock_guard<std::mutex> lk(m_streamerMtx);
            streamerOk = (m_streamer && m_streamer->connected);
            if (streamerOk) {
                int pcmBytes = readFrames * CHANNELS * 2;
                rtsp_streamer_send_audio(m_streamer, pcmBuf, pcmBytes);
            }
        }
        if (!streamerOk)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    free(pcmBuf);
    if (alsaCtx) alsa_capture_close(&alsaCtx);
    qDebug() << "[AudioThread] 音频推流线程退出";
}

bool FaceManager::startMonitor(const QString &rtspUrl)
{
    if (m_isMonitoring) return true;

    bool cameraOpenedByUs = false;
    if (!m_isCameraOpened) {
        if (!initCamera(0)) {
            qWarning() << "监控启动失败: 摄像头初始化失败";
            return false;
        }
        cameraOpenedByUs = true;
    }

    if (!startStreaming(rtspUrl)) {
        qWarning() << "监控启动失败: RTSP推流器初始化失败";
        if (cameraOpenedByUs) releaseCamera();
        return false;
    }

    
    if (m_yuyvToNv12Fd >= 0) { rga_dma_buf_free(m_yuyvToNv12Fd); m_yuyvToNv12Fd = -1; }
    if (m_v4l2PixFmt == V4L2_PIX_FMT_YUYV && m_rgaAvailable) {
        int yuyv_nv12_size = m_v4l2Width * m_v4l2Height * 3 / 2;
        m_yuyvToNv12Fd = rga_dma_buf_alloc(yuyv_nv12_size);
        qDebug() << "YUYV→NV12 DMA-BUF:" << (m_yuyvToNv12Fd >= 0 ? "已分配" : "分配失败");
    }

    if (m_scaledFd >= 0) { rga_dma_buf_free(m_scaledFd); m_scaledFd = -1; }
    int scaled_nv12_size = m_streamWidth * m_streamHeight * 3 / 2;
    m_scaledFd = rga_dma_buf_alloc(scaled_nv12_size);
    qDebug() << "RGA 缩放 DMA-BUF:" << (m_scaledFd >= 0 ? "已分配" : "分配失败");

    bool use_sw_encoder = m_streamer && m_streamer->video_encoder &&
                          !m_streamer->video_encoder->is_hw_encoder;
    if (use_sw_encoder) {
        int yuyv_nv12_size = m_v4l2Width * m_v4l2Height * 3 / 2;
        if (m_nv12Buf) free(m_nv12Buf);
        m_nv12Buf = (unsigned char *)malloc(yuyv_nv12_size);
        if (m_scaledBuf) free(m_scaledBuf);
        m_scaledBuf = (unsigned char *)malloc(scaled_nv12_size);
        qDebug() << "软件编码缓冲区: m_nv12Buf=" << (void*)m_nv12Buf
                 << "m_scaledBuf=" << (void*)m_scaledBuf;
    }

    m_isMonitoring = true;
    m_monitorPaused.store(false, std::memory_order_release);
    m_monitorRtspUrl = rtspUrl;

    m_monitorRunning.store(true, std::memory_order_release);
    m_monitorThread = new std::thread(&FaceManager::monitorThreadFunc, this);

    m_audioRunning.store(true, std::memory_order_release);
    m_audioThread = new std::thread(&FaceManager::audioThreadFunc, this);

    qDebug() << "监控模式已启动:" << rtspUrl << " (独立推流线程已启动)";
    return true;
}

void FaceManager::stopMonitor()
{
    if (!m_isMonitoring) return;
    qDebug() << "停止监控模式...";

    m_monitorRunning.store(false, std::memory_order_release);
    m_audioRunning.store(false, std::memory_order_release);
    m_monitorPaused.store(false, std::memory_order_release);
    m_monitorCV.notify_all();

    if (m_monitorThread) {
        if (m_monitorThread->joinable()) m_monitorThread->join();
        delete m_monitorThread;
        m_monitorThread = nullptr;
    }

    if (m_audioThread) {
        if (m_audioThread->joinable()) m_audioThread->join();
        delete m_audioThread;
        m_audioThread = nullptr;
    }

    if (m_yuyvToNv12Fd >= 0) { rga_dma_buf_free(m_yuyvToNv12Fd); m_yuyvToNv12Fd = -1; }
    if (m_scaledFd >= 0)       { rga_dma_buf_free(m_scaledFd);       m_scaledFd = -1; }
    if (m_nv12Buf)   { free(m_nv12Buf);   m_nv12Buf = nullptr; }
    if (m_scaledBuf) { free(m_scaledBuf); m_scaledBuf = nullptr; }

    stopStreaming();
    releaseCamera();
    m_streamingBaseTimeMs = 0;

    m_isMonitoring = false;
    m_monitorPaused.store(false, std::memory_order_release);
    m_monitorRtspUrl.clear();
    qDebug() << "监控模式已停止";
}

void FaceManager::pauseMonitor()
{
    if (!m_isMonitoring || m_monitorPaused.load(std::memory_order_acquire)) return;
    m_monitorPaused.store(true, std::memory_order_release);
    qDebug() << "[MonitorThread] 推流已暂停, 摄像头移交给人脸识别界面";
}

void FaceManager::resumeMonitor()
{
    if (!m_isMonitoring || !m_monitorPaused.load(std::memory_order_acquire)) return;
    m_monitorPaused.store(false, std::memory_order_release);
    m_monitorCV.notify_all();
    qDebug() << "[MonitorThread] 推流已恢复";
}