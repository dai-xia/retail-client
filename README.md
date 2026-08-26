# RetailClient

无人零售自助收银系统 · 客户端（RK3568 嵌入式终端）。Qt GUI + 人脸支付 + 语音交互 + 音视频推流/录像。

## 核心功能

- **人脸识别**：UltraFace 检测 + MobileFaceNet 特征提取 + 活体检测，全程 RKNN NPU 加速
- **音视频**：MIPI 摄像头采集 → VPU 硬件 H264 编码 → RTSP 推流 / MP4 录像（零拷贝）
- **★ 语音交互**：Vosk 离线识别 + 规则意图引擎（选购/推荐/下单/查余额）
- **OTA 升级**：加密传输、SHA256 校验、失败自动回滚
- **硬件驱动**：RC522 RFID / BH1750 / 电机 / LED

## 人脸识别管线

```
摄像头 NV12 → RGA 转 BGR → 人脸检测 → 关键点 → 特征提取 → 活体检测
```

| 阶段 | 模型 | 输入 | 输出 |
|------|------|------|------|
| 人脸检测 | UltraFace-RFB-320 | 320×240 | 人脸框 + 置信度 |
| 关键点 | PFLD | 裁剪人脸 | 关键点（对齐） |
| 特征提取 | MobileFaceNet | 112×112 | 128 维特征向量 |
| 活体检测 | MiniFASNet | 裁剪人脸 | 活体分数 |

- 特征比对：128 维向量余弦相似度（阈值 0.6），本地 QMap 特征库缓存
- 模型不随仓库提交，用 `client/face_model/download_models.sh` 下载

## 目录结构

```
RetailClient/
├── client/                          # 客户端源码
│   ├── main.cpp                     # 程序入口
│   ├── RetailClient.pro             # Qt 项目（x86）
│   ├── RetailClient_ARM64.pro       # ARM64 交叉编译
│   ├── CMakeLists.txt               # CMake 构建
│   ├── local_config.pri.example     # 本地构建配置模板
│   ├── ui/                          # 界面层（主窗口/会员/商品/结算/人脸/订单/充值）
│   ├── services/                    # 业务服务层
│   │   ├── clientservice.*          # 服务端路由 / OTA / 商品同步
│   │   ├── voice_engine.*           # 语音意图引擎（规则匹配）
│   │   ├── voice_assist.*           # 语音识别（Vosk + 音频预处理）
│   │   ├── crashhandler.*           # 软件看门狗 Qt 封装
│   │   └── otaupdater.*             # OTA 封装
│   ├── core/                        # 核心层（网络 + 数据库 + 硬件 + 媒体）
│   │   ├── networkmanager.*         # TCP / 加密 / 心跳 / 重连
│   │   ├── localdbmanager.*         # SQLite 本地库
│   │   ├── facemanager.*            # 人脸管理（RKNN NPU）
│   │   ├── video_encoder.c/h        # 视频编码（h264_rkmpp VPU）
│   │   ├── rtsp_streamer.c/h        # RTSP 推流
│   │   ├── mp4_recorder.c/h         # MP4 录像
│   │   ├── av_recorder.c/h          # 音视频录像
│   │   ├── v4l2_capture.c/h         # V4L2 采集
│   │   ├── rkisp_camera.c/h         # MIPI 摄像头（rkisp ISP）
│   │   ├── alsa_capture.c/h         # ALSA 采集
│   │   ├── audio_encoder.c/h        # 音频编码（AAC）
│   │   ├── audio_preproc.c/h        # 音频预处理（NS/AGC/VAD）
│   │   ├── rga_osd.c/h / rga_transform.c/h  # RGA 图像处理
│   │   ├── ota.c/h                  # OTA（SHA256 / 断点续传 / 回滚）
│   │   ├── hw_watchdog.c/h          # 硬件看门狗
│   │   └── watchdog.c/h             # 软件看门狗
│   ├── config/                      # 配置 / 支付接口
│   ├── face_model/                  # 模型脚本（download_models.sh 等）
│   └── 3rdparty/jpeg_aarch64/       # libjpeg 头文件（aarch64）
├── common/                          # 共享库（crypto / logger / cJSON / 公共类型）
├── driver/                          # Linux 内核驱动（RC522/BH1750/电机/LED/DMA-SPI）
├── deploy/                          # 部署（deploy.sh / systemd 服务）
├── rk3568_toolchain.cmake           # RK3568 交叉编译工具链
├── CMakeLists.txt                   # 顶层 CMake
└── ...
```

## 音视频推流与录像

- **RTSP 推流**：V4L2 NV12 → VPU 硬编码 H264 → RTP → RTSP（TCP/UDP，H264 + AAC）
- **MP4 录像**：音视频混合录制，支持 faststart 与时长限制
- **零拷贝**：V4L2 DMA-BUF → RGA → VPU/NPU 全链路零拷贝，CPU 不处理图像数据

## 编译与运行

```bash
# 依赖
sudo apt install qt5-default libqt5sql5-sqlite libsqlite3-dev libopencv-dev \
                 libavcodec-dev libavformat-dev libswscale-dev libasound2-dev \
                 libspeexdsp-dev

# 模型下载
cd client/face_model && ./download_models.sh --all

# x86 编译（桌面调试）
cd client && qmake RetailClient.pro && make -j$(nproc)

# ARM64 交叉编译（RK3568）
cd client && qmake RetailClient_ARM64.pro && make -j$(nproc)

# 运行
./RetailClient <client_id> [local_port] [server_ip] [server_port]
```

## License

MIT License — 详见 [LICENSE](LICENSE)。
