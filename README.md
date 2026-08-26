# RetailClient

无人零售自助收银系统的客户端，跑在 RK3568 上。Qt 界面 + 人脸支付 + 语音交互 + 音视频推流/录像。

## 技术亮点

### 全链路零拷贝多媒体管线

客户端最核心的设计是**零拷贝**：从摄像头采集到推流/录像，CPU 全程不碰图像数据，全部走 DMA-BUF 在硬件之间传递。

```
摄像头(MIPI) ──> V4L2(MMAP/DMA-BUF) ──┬──> VPU 硬编码 H264 ──> RTSP 推流
                                      │                     └─> MP4 录像
                                      └──> RGA 转 BGR ──> NPU 人脸推理
```

- 采集用 `V4L2` 的 `VIDIOC_EXPBUF` 导出 DMA-BUF fd，直接喂给 VPU/NPU，不做一次 memcpy。
- 人脸识别复用同源 NV12 帧，经 RGA 转成 RGB 再进 NPU，和推流管线并行。
- 视频编码用 Rockchip 的 `h264_rkmpp`（VPU 硬件编码），输入直接是 DMA-BUF fd，编码不占 CPU。没有 VPU 时才回退到 `libx264` 软件编码。

### 人脸识别：检测 + 特征 + 活体，全在 NPU

人脸不是简单调一个模型，而是四段流水线，全部用 RKNN 在 NPU 上推理：

| 阶段 | 模型 | 输入 | 输出 |
|------|------|------|------|
| 检测 | UltraFace-RFB-320 | 320×240 | 人脸框 + 置信度 |
| 关键点 | PFLD | 裁剪人脸 | 关键点（对齐用） |
| 特征提取 | MobileFaceNet | 112×112 | 128 维特征向量 |
| 活体检测 | MiniFASNet | 裁剪人脸 | 活体分数 |

- 特征比对用余弦相似度，阈值 0.6；本地 `QMap` 做特征库缓存，注册写盘、启动加载。
- 单人脸跟踪：IoU 匹配 + 30 帧强制重识别，省掉不必要的多目标跟踪。
- 模型不随仓库提交，用 `client/face_model/download_models.sh` 下载、`convert_to_rknn.py` 转 RKNN。

### 音视频：RTSP 推流 + MP4 录像

- **RTSP**：V4L2 NV12 → VPU H264 → RTP 打包，支持 TCP/UDP，音视频一起（H264 + AAC）。PTS 在采集瞬间计算并透传，不手动改写。
- **MP4 录像**：音视频混合录制，支持 faststart、时长限制、暂停续录。
- 码率按 `宽 × 高 × 帧率 × bpp` 动态算，不再写死。

### 语音：Vosk 离线识别 + 规则意图引擎

- 采集走原生 ALSA，进 Vosk 前先过一遍**音频预处理（NS 降噪 → AGC 增益 → VAD 端点检测）**，只把有效语音帧送识别，减少无效推理。
- 识别结果进**规则意图引擎**（关键词匹配 → 意图分类 → 参数提取），支持选购、推荐、查余额、下单、充值、注册等，纯离线。
- 意图通过 Qt 信号槽驱动界面动作，还有 TTS 播报信号。

### 可靠性：三层防护 + OTA 回滚

- **三层看门狗**：软件狗（30s 超时主动退出）→ systemd（`Restart=always` 拉起）→ 硬件狗（60s 超时整机复位），超时逐层放大，正常故障不会被硬件狗误触发。
- **OTA**：加密传输、32KB 分块、SHA256 校验、失败自动回滚。

## 目录结构

```
client/          Qt 客户端
  ui/            界面（主窗口/会员/商品/结算/人脸/订单/充值）
  services/      业务服务（服务端路由、语音、OTA、看门狗封装）
  core/          核心层
    networkmanager.cpp/h    TCP/加密/心跳/重连
    localdbmanager.cpp/h    SQLite 本地库
    facemanager.cpp/h       人脸管理（检测+特征+活体，RKNN）
    face_landmark.c/h       关键点
    face_antispoof.c/h      活体
    video_encoder.c/h       视频编码（h264_rkmpp）
    rtsp_streamer.c/h       RTSP 推流
    mp4_recorder.c/h        MP4 录像
    av_recorder.c/h         音视频录像
    v4l2_capture.c/h        V4L2 采集
    rkisp_camera.c/h        MIPI 摄像头（rkisp ISP）
    alsa_capture.c/h        ALSA 采集
    audio_encoder.c/h       AAC 编码
    audio_preproc.c/h       音频预处理（NS/AGC/VAD）
    rga_osd.c/h  rga_transform.c/h   RGA 图像处理/OSD
    ota.c/h                 OTA（SHA256/断点续传/回滚）
    watchdog.c/h  hw_watchdog.c/h     软/硬看门狗
  config/         配置、支付接口
  face_model/     模型脚本（下载/转换）
common/         共享库（AES 加密 / 日志 / cJSON / 公共类型）
driver/         内核驱动（RC522 RFID / BH1750 / 电机 / LED / DMA-SPI）
deploy/         部署（deploy.sh / systemd 服务）
```

## 摄像头

主摄像是 MIPI 接口的 OV5695，走 `rkisp` ISP（rkaiq 3A 自动对焦/曝光/白平衡），输出 NV12；rkisp 初始化失败时自动回退 USB 摄像头。

## 编译运行

```bash
# 依赖（x86 桌面调试）
sudo apt install qt5-default libqt5sql5-sqlite libsqlite3-dev libopencv-dev \
                 libavcodec-dev libavformat-dev libswscale-dev libasound2-dev \
                 libspeexdsp-dev

# 模型下载
cd client/face_model && ./download_models.sh --all

# x86 调试编译
cd client && qmake RetailClient.pro && make -j$(nproc)

# RK3568 交叉编译
cd client && qmake RetailClient_ARM64.pro && make -j$(nproc)
# 或 CMake + 工具链
cmake -B build_arm64 -DCMAKE_TOOLCHAIN_FILE=client/aarch64-linux-gnu.cmake \
      -DRK3568_SYSROOT=/path/to/arm64/rootfs

# 运行
./RetailClient <client_id> [local_port] [server_ip] [server_port]
```

## License

MIT，详见 [LICENSE](LICENSE)。
