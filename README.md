# RetailClient

无人零售自助收银系统的客户端，跑在 RK3568 上。Qt 界面 + 人脸支付 + 语音交互 + 音视频推流。

## 目录结构

```
client/          Qt 客户端
  ui/            界面（主窗口/会员/商品/结算/人脸/订单/充值）
  services/      业务服务（服务端路由、语音、OTA、看门狗封装）
  core/          核心层
    networkmanager.cpp/h    TCP/加密/心跳/重连
    localdbmanager.cpp/h    SQLite 本地库
    facemanager.cpp/h       人脸管理（检测+关键点+特征+活体，RKNN）
    face_landmark.c/h       关键点 + 仿射对齐
    face_antispoof.c/h      活体检测
    face_image_utils.c/h    人脸图像裁剪/缩放
    video_encoder.c/h       视频编码（h264_rkmpp / libx264）
    rtsp_streamer.c/h       RTSP 推流
    mp4_recorder.c/h        本地录像
    v4l2_capture.c/h        V4L2 采集
    rkisp_camera.c/h        MIPI 摄像头（rkisp ISP）
    alsa_capture.c/h        ALSA 采集
    audio_encoder.c/h       AAC 编码
    audio_preproc.c/h       音频预处理（AEC/NS/AGC/VAD）
    h264_parser.c/h         H264 流解析统计
    rga_osd.c/h  rga_transform.c/h   RGA 图像处理/OSD
    hardware_api.c/h  hardwareservice 硬件抽象（RFID/电机/LED）
    ota.c/h                 OTA（SHA256/断点续传/回滚）
    watchdog.c/h  hw_watchdog.c/h     软/硬看门狗
  config/         配置、支付接口
  face_model/     模型脚本（下载/转换）
common/         共享库（AES 加密 / 日志 / cJSON / 公共类型）
driver/         内核驱动（RC522 RFID / BH1750 / 电机 / LED / DMA-SPI）
deploy/         部署（deploy.sh / systemd 服务）
```

## 音视频推流

推流这一路的重点是尽量少让 CPU 碰图像数据，从摄像头出来一直用 DMA-BUF 在硬件之间转。

```
摄像头(MIPI) ──> V4L2(MMAP/EXPBUF) ──> VPU 硬编码 H264 ──> RTSP
                                    └──> RGA 转 BGR ──> NPU 人脸推理
```

- 采集用 `V4L2` 的 `VIDIOC_EXPBUF` 把缓冲导成 DMA-BUF fd，直接喂给 VPU，中间不 memcpy。
- 编码优先走 Rockchip 的 `h264_rkmpp`（VPU 硬编，输入就是 DMA-BUF fd）；没有 VPU 时降级到 `libx264` 软件编码。
- PTS 在采集那一刻算好，透传给编码器，不做二次改写；音视频统一用 `CLOCK_MONOTONIC` 做时间基。
- 音频走原生 ALSA，编码前先过 SpeexDSP 的 NS 降噪 + AGC 增益，再压成 AAC。
- 码率按 `宽 × 高 × 帧率 × bpp` 动态算，不写死。
- 本地录像和推流共用同一条编码管线，支持时长分段和暂停续录。

## 端侧人脸识别

人脸不是单个模型，而是四段流水线，全部跑在 NPU 上：

| 阶段 | 模型 | 输入 | 输出 |
|------|------|------|------|
| 检测 | UltraFace-RFB-320 | 320×240 | 人脸框 + 置信度 |
| 关键点 | PFLD | 裁剪人脸 | 关键点（对齐用） |
| 特征提取 | MobileFaceNet | 112×112 | 128 维特征向量 |
| 活体检测 | MiniFASNet | 裁剪人脸 | 活体分数 |

- 关键点先做仿射对齐，消除人脸偏转再进特征提取，比直接裁剪更稳。
- 特征比对用 L2 归一化 + 余弦相似度，阈值 0.6；本地 `QMap` 做特征库缓存，注册写盘、启动加载。
- 单人脸场景用 IoU 匹配 + 30 帧强制重识别，没上多目标跟踪那套复杂度。
- 模型量化用 RKNN Hybrid：对敏感层保留 FP16 精度，其余 W8A8。全 W8A8 时相似度掉到 0.93，Hybrid 能拉回 0.96，体积也没有膨胀。
- 模型不随仓库提交，用 `client/face_model/download_models.sh` 下载、`convert_to_rknn.py` 转 RKNN。

## 语音

- 采集走原生 ALSA，进 Vosk 前先过 AEC/NS/AGC/VAD，只把有效语音帧送识别。
- 识别结果进规则意图引擎，支持 11 类意图，商品用别名模糊匹配（"可乐""coke"都能命中"可口可乐"）。
- 意图通过 Qt 信号槽驱动界面，另有 TTS 播报信号。

## 可靠性与 OTA

- 系统层用 Squashfs + Overlayfs 双分层，A/B 槽原子切换；系统 OTA 走 swupdate + U-Boot，三启失败自动回滚。
- 应用层 OTA 用 tar.gz 原子替换，异常超限回滚，跟系统升级解耦。
- 三层看门狗：软件狗（30s 超时）→ systemd（`Restart=always`）→ 硬件狗（60s 超时整机复位），超时逐层放大。
- 崩溃时通过信号栈回溯 dump 调用栈，配合启动熔断判断是否需要回滚。

## 基础组件

- 日志：无锁环形队列 + 异步刷盘，自动轮转，支持控制台/syslog/远端多渠道输出。
- 加密：OpenSSL AES-256-GCM，带认证标签。

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
