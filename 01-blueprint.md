> 参见 [02-reference.md](02-reference.md) 查看接口定义与实现细节

# Pulsar

## 概述

**Pulsar** 是一个跨平台的自建串流与远程访问服务器。

它的目标是成为一个**统一的远程访问基础设施**：游戏玩家可以用它串流游戏，直播者可以用它推流录制，企业可以用它替代 RDP/VNC，开发者可以用它在 Headless 服务器上工作——所有这些场景运行在同一套代码上，通过配置文件切换，而不是依赖多个互不兼容的工具。

Pulsar 的核心设计原则是**模块隔离**：编码器、传输协议、采集方式都是独立的 Adapter，互不感知。更换一个组件不影响其他模块，新增一种协议不需要触碰任何已有代码。这也是它与现有工具最根本的区别——Pulsar 不是为某个场景定制的工具，而是为扩展而设计的平台。

### 为什么重新做

现有工具（Sunshine、Parsec、各平台 RDP/VNC 实现）各有其价值，但存在共性问题：

- **协议碎片化**：每个工具只支持自己的协议，RDP 工具不支持 WebRTC，串流工具不支持 RDP，用户被迫在多个工具之间切换
- **代码耦合严重**：历史包袱重，第三方依赖混杂，替换一个编码器或传输协议往往要动核心代码
- **扩展成本高**：现有工具的架构不是为扩展而设计的，新增平台或协议支持需要大规模改动

### 本项目的定位

从零设计，以**模块隔离和可扩展性**为第一优先级：

- 串流、RDP、VNC 统一在同一套管道架构下，切换只需修改配置文件
- Linux、Windows、macOS 三平台地位对等，均为一等公民，无平台歧视
- 新增协议或平台 = 新增 Adapter，core 层不动，不影响已有实现
- 第三方依赖严格分层：模块私有库直接放各自 `lib/` / `include/` 目录，全局 header-only 库放 `common/include/`，平台原生 SDK 走系统自带，core 层零外部依赖，构建时零网络请求

### 支持的使用场景

| 场景 | 支持方式 |
|------|---------|
| 游戏串流（局域网/公网）| RTP / WebRTC / QUIC，低延迟硬件编码 |
| 远程桌面 | RDP（mstsc 直连）/ VNC |
| 多人游戏协作 | 协作会话，多客户端分工键鼠/手柄/摇杆，多人语音，文字聊天 |
| 直播与录制 | 多路推流（Twitch/YouTube）+ 本地录制副本 |
| Headless 服务器 | 虚拟显示驱动，无物理显示器也可串流 |
| 云游戏/企业远程 | 应用程序启动管理，OAuth/Passkey 认证，LDAP 集成 |

---

## 设计目标

| 目标 | 实现方式 |
|------|----------|
| 架构清晰 | 六边形架构（端口与适配器模式），依赖方向单向强制 |
| 模块隔离 | 每个 Adapter 是独立的 CMake 编译目标，互不链接 |
| 可扩展性 | 新增协议/平台 = 新增 Adapter，core 层不动 |
| 第三方依赖控制 | 模块私有库直接放各模块 `lib/`（静态库）或 `include/`（SDK 头文件）；全局 header-only 库放 `common/include/`；平台原生 SDK 走系统自带；构建时零网络请求，core 层零外部依赖 |
| 运行时灵活性 | JSON 配置文件 + 工厂能力协商，启动时自动选最优链路 |
| 三平台对等 | Linux / Windows / macOS 均为一等公民，各自最优路径实现 |
| 低延迟优先 | 零拷贝数据流、异步流水线、SPSC 无锁队列、硬件编码 |

---

## 技术选型

| 项目 | 决策 | 原因 |
|------|------|------|
| 语言 | C++20 | 直接调用所有 C/C++ SDK，多媒体/系统级 API 无摩擦 |
| 构建系统 | CMake | 跨平台构建标准，依赖直接随源码入库（`lib/` + `include/`），无需包管理器 |
| 架构模式 | 六边形架构 + Adapter + 工厂模式 | 强隔离，实现可随时替换 |
| 配置格式 | JSON（nlohmann/json） | 单头文件库，零依赖，通用性强 |
| 依赖管理 | 模块私有库放各模块 `lib/`（静态库）或 `include/`（SDK 头文件）；全局 header-only 库放 `common/include/`；平台原生 SDK 走系统自带；core 层零外部依赖，构建时零网络请求 |

---

## 系统架构图

### 整体系统架构

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                                   CLIENT                                        │
│                (RDP / VNC / WebRTC / RTP / QUIC / ...)                         │
└──────┬──────────────────┬───────────────────┬──────────────────┬────────────────┘
       │ :3389            │ :47984            │ :47985           │ :47986
       ▼ RDP              ▼ RTP               ▼ WebRTC           ▼ QUIC
┌─────────────────────────────────────────────────────────────────────────────────┐
│                              server/                                            │
│                                                                                 │
│   ┌───────────────────┐   ┌─────────────────┐   ┌──────────────────────────┐   │
│   │ ConnectionDispatch│   │  Factory        │   │  Pipeline                │   │
│   │ 按端口判断协议    │──▶│  (能力协商 +    │──▶│  (异步四阶段流水线)       │   │
│   │ 无需字节嗅探      │   │   最优链路组装) │   └──────────────────────────┘   │
│   └───────────────────┘   └────────┬────────┘                                  │
│   ┌───────────────────┐            │                                            │
│   │ config.json       │────────────┘                                            │
│   └───────────────────┘                                                         │
│   ┌───────────────────┐                                                         │
│   │ SessionMgr        │                                                         │
│   │ (认证/QoS)        │                                                         │
│   └───────────────────┘                                                         │
└─────────────────────────────────────────────────────────────────────────────────┘
                                    │ 组装并启动
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│              数据管道（四阶段异步流水线，每阶段独立线程，SPSC无锁队列连接）        │
│                                                                                 │
│  ┌──────────────────┐  Q1   ┌───────────────┐  Q2   ┌──────────────────┐      │
│  │     Capture      │──────▶│ Preprocessor  │──────▶│    Encoder       │      │
│  │                  │       │  （可选）     │       │                  │      │
│  │ • DXGI           │       │ • GPU shader  │       │ • NVENC          │      │
│  │ • PipeWire       │       │ • DMABUF导入  │       │ • VAAPI          │      │
│  │ • SCKit          │       │ • CPU libyuv  │       │ • VideoToolbox   │      │
│  │                  │       │               │       │ • x264           │      │
│  │ RawFrame 携带：  │       │ 工厂返回      │       │                  │      │
│  │ • pixels         │       │ nullptr 时    │       │ GPU异步编码完成  │      │
│  │ • dirty_rects    │       │ 此阶段跳过    │       │ 后回调推入 Q3    │      │
│  │ • HDR metadata   │       │               │       │                  │      │
│  │ • pts_us         │       │               │       │                  │      │
│  │  [实时线程/CPU亲和] │    │               │       │  [实时线程/CPU亲和] │   │
│  └──────────────────┘       └───────────────┘       └────────┬─────────┘      │
│                                                               │ Q3             │
│                                                               ▼                │
│                                                    ┌──────────────────┐        │
│                                                    │   Transport      │        │
│                                                    │                  │        │
│                                                    │ • RTP/RTSP       │        │
│                                                    │ • WebRTC         │        │
│                                                    │ • QUIC           │        │
│                                                    │ • RDP            │        │
│                                                    │ • VNC            │        │
│                                                    │                  │        │
│                                                    │ FEC / pacing     │        │
│                                                    │  [普通线程]      │        │
│                                                    └────────┬─────────┘        │
│                                                             │                  │
│  ◀── NetworkStats（RTT/丢包/带宽）+ TransportEvent ─────────┘                  │
│        │                                                                        │
│        ├─▶ Encoder::update_params()   码率/GOP 自适应                          │
│        ├─▶ Transport::set_fec_params() FEC 动态开关                            │
│        └─▶ Pipeline::set_idle_fps()   帧率自适应（无脏区域时降帧率）           │
│                                                                                 │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │  IInputHandler（反向通道）：客户端键盘/鼠标/手柄 → 注入本机              │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                 │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │  IMetricsCollector：各阶段上报延迟/帧率/丢帧/码率 → stdout / 监控系统   │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 依赖层次图

```
┌──────────────────────────────────────────────────────────────┐
│                         server/                              │
│   main.cpp  factory.cpp                                      │
│   （唯一知道所有具体类型的地方）                               │
└──────┬───────────────────────────────────────┬───────────────┘
       │ 依赖                                   │ 依赖
       ▼                                        ▼
┌─────────────────────────────┐   ┌─────────────────────────────────────────┐
│          core/              │   │              adapters/                  │
│                             │◀──│                                         │
│  接口（纯虚类）：            │   │  video/capture/linux/pipewire          │
│  ICaptureSource             │   │  video/capture/linux/drm_virtual       │
│  IPreprocessor              │   │  video/capture/windows/dxgi            │
│  IEncoder                  │   │  video/capture/windows/virtual_display │
│  ITransport                │   │  video/capture/macos/sckit             │
│  IInputHandler             │   │  video/capture/macos/cgvirtual         │
│  IAuthProvider             │   │  video/preprocessor/linux/gpu          │
│  ISessionManager           │   │  video/preprocessor/linux/dmabuf       │
│  IAudioCapture             │   │  video/preprocessor/windows/gpu        │
│  IAudioEncoder             │   │  video/preprocessor/all/cpu            │
│  IAudioPlayback            │   │  video/encoder/nvenc/linux             │
│                             │   │  video/encoder/vaapi/linux             │
│  数据结构：                 │   │  video/encoder/videotoolbox/macos      │
│  RawFrame  EncodedPacket   │   │  video/encoder/x264/all                │
│  AudioFrame AudioPacket    │   │  video/encoder/rdp/all                 │
│  ColorInfo DirtyRect       │   │  video/encoder/vnc/all                 │
│  NetworkStats FecParams    │   │  audio/capture/linux/pipewire          │
│  QosPolicy  AuthToken      │   │  audio/capture/windows/wasapi          │
│  ReconnectPolicy           │   │  audio/capture/macos/coreaudio         │
│                             │   │  audio/encoder/opus/all                │
│                             │   │  audio/encoder/aac/windows             │
│                             │   │  audio/encoder/aac/macos               │
│                             │   │  transport/rtp/all  transport/rdp/all  │
│                             │   │  transport/webrtc/linux                │
│                             │   │  transport/quic/linux                  │
│                             │   │  transport/vnc/all                     │
│                             │   │  input/linux/uinput                    │
│                             │   │  input/windows/win32                   │
│                             │   │  input/macos/iokit                     │
│                             │   │  auth/all/password  auth/all/oauth     │
│                             │   │                                         │
│                             │   │  每个 Adapter：                         │
│                             │   │  • 只依赖 core 接口                     │
│                             │   │  • 静态库放在 lib/ 子目录               │
│                             │   │  • 零第三方源码                         │
│                             │   │                                         │
│  基础设施：                 │   └─────────────────────────────────────────┘
│  BoundedQueue               │
│  IBufferPool                │   规则：adapters → core（单向）
│  AdapterCapabilities        │         core 不知道任何 adapter 存在
│  ThreadConfig               │         违反此规则 = 架构腐化起点
│  PipelineMetrics            │
└─────────────────────────────┘
```

### 数据流与优化路径图

```
最优路径（Linux，全程零拷贝）：
  显卡显存
     │ dmabuf fd（不经过 CPU）
     ▼
  PipeWireCapture ──▶ DMABUFImporter ──▶ VAAPIEncoder ──▶ RTP/QUIC
  [填写 dirty_rects]   [fd→encoder       [GPU内编码        [FEC+pacing
   [填写 pts_us]        surface]          无CPU拷贝]        发送]

次优路径（Windows，GPU 内转换）：
  DXGI 纹理（显存）
     │ D3D11 纹理句柄（不经过 CPU）
     ▼
  DXGICapture ──▶ GPUPreprocessor ──▶ NVENCEncoder ──▶ RTP/QUIC
  [BGRA+dirty]    [GPU: BGRA→NV12]    [GPU内编码]

次优路径（macOS，系统框架硬件编码）：
  ScreenCaptureKit 帧缓冲（CVPixelBuffer，显存）
     │ Metal 纹理句柄（不经过 CPU）
     ▼
  SCKitCapture ──▶ GPUPreprocessor ──▶ VideoToolboxEncoder ──▶ RTP/QUIC
  [BGRA+dirty]    [Metal: BGRA→NV12]   [Apple Silicon/Intel   [FEC+pacing
                                        硬件编码]               发送]

兜底路径（软件编码，全平台）：
  Capture ──▶ CPUPreprocessor ──▶ X264Encoder ──▶ RTP
  [任意格式]   [CPU: 格式转换]    [CPU编码]

直通路径（格式已匹配，跳过 Preprocessor）：
  Capture(NV12) ──▶ nullptr ──▶ Encoder(NV12)
                  [工厂返回 nullptr，Pipeline 直接转发，Preprocessor 阶段不存在]
```

---

## 架构设计

### 核心原则：依赖方向

```
server → core ← adapters
```

- `core` 不依赖任何 Adapter，不依赖任何第三方库
- 所有 Adapter 依赖 `core` 定义的接口（只能向内依赖）
- `server` 依赖所有层：读取配置，通过工厂能力协商组装最优链路，启动管道

**违反这个方向是架构腐化的根本原因。**

### 三平台实现路径

Linux、Windows、macOS 各自采用最优的原生 API 实现。

| 模块 | Linux | Windows | macOS |
|------|-------|---------|-------|
| Capture | PipeWire（dmabuf 零拷贝）| DXGI Desktop Duplication | ScreenCaptureKit（IOSurface 零拷贝）|
| Encoder | VAAPI（Intel/AMD）/ NVENC（NVIDIA）/ x264（软件兜底）| NVENC / AMF / x264（软件兜底）| VideoToolbox / x264（软件兜底）|
| Preprocessor | DMABUFImporter / GPU shader | GPU shader | GPU shader |
| Input | uinput | SendInput / ViGEm | IOKit CGEvent |
| Audio | PipeWire | WASAPI | CoreAudio |
| Headless | DRM/KMS virtual display | IDD 虚拟显示驱动 | CGVirtualDisplay |

### Session 生命周期

```
客户端连接 → 端口识别协议 → 认证（Challenge/Token） → 创建 Session → 探测/选链路 → 运行管道 → Session 结束
```

### 客户端协议自动检测（ConnectionDispatcher）

服务器在各协议的独立端口监听，按接入端口直接判断协议类型，组装对应 Adapter 链路，无需字节嗅探。同一台服务器可以同时被 `mstsc`（:3389）、WebRTC 客户端（:47985）、RTP 客户端（:47984）连接，无需修改任何配置。

---

### Headless 服务器支持

云主机、无物理显示器的 GPU 服务器在三个平台上都会遇到同一个问题：采集 API 依赖显示器存在，没有显示器就没有可采集的桌面。各平台通过虚拟显示驱动解决，各自采用最优的原生方案：

| 平台 | 方案 | 说明 |
|------|------|------|
| Windows | IDD（Indirect Display Driver）| 安装虚拟显示驱动，创建虚拟桌面，DXGI 正常工作 |
| Linux | DRM/KMS virtual display | 内核级虚拟帧缓冲，PipeWire 正常采集，不依赖 Xvfb |
| macOS | `CGVirtualDisplay`（macOS 12+）| 系统 API 直接创建虚拟屏幕 |

服务器启动时，工厂通过 `ICaptureSource::enumerate_displays()` 检测物理显示器数量，为零时自动切换到对应平台的虚拟显示 Adapter，无需手动配置。`config.json` 中可通过 `"capture": { "backend": "auto" }` 保持默认自动行为，也可显式指定强制使用虚拟显示。

---

### 支持的协议总览

本项目实现串流模式（RTP/WebRTC/QUIC）、RDP 和 VNC。其他协议架构上可拆解支持，但不在当前实现范围内。

| 协议 | 状态 | 拆解方式 |
|------|------|---------|
| RTP/RTSP | ✅ 实现 | Transport Adapter |
| WebRTC | ✅ 实现 | Transport Adapter |
| QUIC | ✅ 实现 | Transport Adapter |
| RDP | ✅ 实现 | RDPEncoder + RDPTransport 成对 Adapter |
| VNC (RFB) | ✅ 实现 | VNCEncoder + VNCTransport 成对 Adapter |

---

### RDP 模式 Adapter 拆解

RDP 完整协议栈（认证、压缩、传输、输入通道）可拆解为两个成对 Adapter 嵌入现有管道，管道结构不变：

```
串流模式：DXGICapture → GPUPreprocessor  → NVENCEncoder → RtpTransport
RDP 模式：DXGICapture → Passthrough(null) → RDPEncoder   → RDPTransport
```

| Adapter | 职责 |
|---------|------|
| `RDPEncoder` | 实现 `IEncoder`，RemoteFX / H.264 封装成 RDP Bitmap Update 格式 |
| `RDPTransport` | 实现 `ITransport`，RDP 握手、NLA/CredSSP 认证、虚拟通道管理 |

- NLA 认证在 `RDPTransport::connect()` 内部处理，不经过 `IAuthProvider`
- 键鼠输入经 RDP 虚拟通道反向传回，通过 `set_input_callback()` 注入 `IInputHandler`
- 工厂不创建任何 Preprocessor 实例（传 `nullptr`），Pipeline 直接跳过预处理阶段

### VNC 模式 Adapter 拆解

VNC（RFB 协议）同样拆解为成对 Adapter，管道结构不变：

```
VNC 模式：任意Capture → Passthrough(null) → VNCEncoder → VNCTransport
```

| Adapter | 职责 |
|---------|------|
| `VNCEncoder` | 实现 `IEncoder`，输出 RFB FramebufferUpdate（Raw/ZRLE/Tight 格式），直接接受 BGRA，无需 NV12 预处理 |
| `VNCTransport` | 实现 `ITransport`，RFB 握手（版本协商、安全类型、像素格式），VNC Auth 认证 |

- VNC 认证（VNC Auth / None）在 `VNCTransport::connect()` 内部处理
- 键鼠输入经 RFB KeyEvent/PointerEvent 反向传回，通过 `set_input_callback()` 注入
- VNC 直接操作 BGRA 像素，工厂传 `nullptr` 跳过格式转换

### 输入反向通道（所有协议通用）

所有协议的键鼠输入反向通道通过同一机制处理，pipeline.cpp 统一连线：各 Transport 收到 `InputEvent` 后通过 `set_input_callback` 注入 `IInputHandler`，实现代码见 reference.md。

- `RDPTransport`：通过 RDP 虚拟通道（MCS/T.125）接收
- `WebRTCTransport`：通过 DataChannel 接收
- `RtpTransport`：通过独立 TCP 控制通道接收（自定义扩展，非 RTP 协议标准）

---

## 游戏与直播扩展功能

### 多路串流（同时向多个客户端输出）

直播者最常见的需求：同一画面同时推送给多个观众客户端，或同时向 Twitch/YouTube 推流。

当前架构是 1 Session → 1 Transport（单播）。扩展为多路输出只需在 `server` 层增加一个 `IOutputMultiplexer` 分发器，核心管道不需要改动：

```
视频管道输出（EncodedPacket）
         │
         ▼
  IOutputMultiplexer（分发器）
    ├── RtpTransport（客户端 A）
    ├── WebRTCTransport（客户端 B）
    ├── RtmpTransport（Twitch/YouTube 推流）   ← 直播者
    └── FileRecorder（IRecorder 实现，本地录制）← 录制存档
```

各 sink 独立管理连接状态，某个客户端断线不影响其他 sink。编码器只编码一次，分发零拷贝（`shared_ptr<PacketBuffer>` 共享同一块内存）。接口定义见 reference.md。

---

### 本地录制

通过 `IOutputMultiplexer` 注册为一个 sink，与串流共用同一路编码输出，不增加额外编码开销。实现为 MP4 容器（H.264/H.265 + AAC）。接口定义见 reference.md。

config.json：

```json
"recording": {
  "enabled": false,
  "output_dir": "recordings/",
  "format": "mp4",
  "max_duration_minutes": 120
}
```

---

### 第三方推流（RTMP / SRT）

RTMP 和 SRT 都是标准串流协议，作为新的 Transport Adapter 实现即可：

```
encoder/
└── nvenc/        (已有)

transport/
├── rtp/          (已有)
├── webrtc/       (已有)
├── rtmp/         ← 新增：推流到 Twitch / YouTube / Bilibili
└── srt/          ← 新增：低延迟推流（SRT 协议，比 RTMP 更稳定）
```

config.json 中配置推流地址：

```json
"streaming": {
  "rtmp": {
    "enabled": false,
    "url": "rtmp://live.twitch.tv/app/<stream_key>"
  },
  "srt": {
    "enabled": false,
    "url": "srt://ingest.example.com:9000"
  }
}
```

---

### 应用程序启动管理

允许客户端远程启动/关闭服务器上的游戏或程序，无需 RDP 桌面操作。这是云游戏场景的基础能力。游戏列表在 config.json 中配置，客户端通过 Transport 控制通道发送启动/关闭命令。接口定义（`IAppManager`、`AppEntry`）见 reference.md。

```json
"apps": [
  { "id": "cs2",   "name": "CS2",    "executable": "C:/Games/cs2.exe" },
  { "id": "steam", "name": "Steam",  "executable": "C:/Program Files (x86)/Steam/steam.exe" }
]
```

---

### 编码器画质预设

直播者需要在"低延迟模式"（竞技游戏）和"高画质模式"（观影/桌面演示）之间快速切换，不重建管道。调用 `encoder.update_params()` 即可热切换，`QualityPreset` 枚举和 `EncoderParams` 结构定义见 reference.md。

---

### 高刷新率支持（144Hz / 240Hz）

高刷新率对架构的影响主要在三个地方：

1. **采集端**：`display_refresh_rate()` 返回实际刷新率，VSync 对齐后 `next_frame()` 调用频率提高
2. **编码器**：`GopConfig::gop_size` 需要按帧率等比调整（60fps 时 gop_size=60，144fps 时应 =144）
3. **队列容量**：`PipelineConfig::queue_capacity` 建议随帧率等比增加（防止高帧率下背压触发过频）

`GopConfig::target_fps` 字段定义见 reference.md。config.json 配置：

```json
"encoder": {
  "fps": 144,
  "gop": { "target_fps": 144, "gop_seconds": 1 }
}
```

---

### OSD 统计叠加 / 水印

两种实现方式，接口扩展点已预留：

**方式 A（推荐）：Transport 反向推送 → 客户端渲染**
Transport 通过 `send_stats()` 把 `PipelineMetrics`（帧率、延迟、码率）实时发给客户端，由客户端 UI 渲染 OSD，不污染视频流，质量最优。

**方式 B：Preprocessor 帧内叠加**
新增 `OverlayPreprocessor`（继承 `IPreprocessor`），在视频帧上用 GPU shader 叠加文字/水印。适合需要在录制文件中包含水印的场景。

```
preprocessor/
└── overlay/    ← 可选，实现 IPreprocessor，GPU 叠加文字/图像到 RawFrame
```

---

### 协作会话（多客户端共享画面 + 输入）

**与多路串流的区别**：

| | 多路串流 | 协作会话 |
|-|---------|---------|
| 客户端角色 | 被动观看 | 各自控制不同设备 |
| 画面来源 | 同一路编码广播 | 同一路编码广播（相同） |
| 输入 | 无 | 多客户端按设备类型分工输入 |
| 场景 | 直播观看 | 多人游戏协作（A 键鼠+B 手柄+C 摇杆）|

#### 设备分工模型（FreeForAll + 设备路由）

协作会话采用 **FreeForAll + 设备类型路由**：所有客户端的输入全部注入，但每个客户端只绑定特定的设备类型。

**核心洞察**：键盘、鼠标、手柄、摇杆本来就是不同的 `InputEvent::Type`，天然互不冲突。多个人协商各自用什么设备，服务器按设备类型路由即可，不需要轮流抢控制权。

```
客户端 A（PC，键鼠）：发送 KeyDown/MouseMove/MouseButton 事件  → 全部注入
客户端 B（手机，手柄）：发送 GamepadButton/GamepadAxis 事件      → 全部注入
客户端 C（平板，触摸）：发送 TouchBegin/TouchUpdate/TouchEnd 事件 → 全部注入

三者同时工作，互不干扰——就像本地同时插了键鼠+手柄+触摸屏一样
```

客户端连接后通过控制通道声明设备绑定，协商结果广播给所有客户端（让每个人知道当前谁负责什么设备）。`IInputArbiter`、`ClientDeviceBinding` 接口定义及 Pipeline 集成代码见 reference.md。

config.json：

```json
"shared_session": {
  "enabled": false,
  "max_clients": 4
}
```

#### 光标区分

多客户端协作时，各客户端的鼠标光标应当可区分（不同颜色/标签），让所有人知道谁在操作哪里。这是一个显示层特性：服务端通过 `IOutputMultiplexer` 的元数据通道广播各客户端光标位置，客户端本地渲染带颜色标签的多光标叠加层。

#### 多人语音（Party Chat）

协作会话中多个客户端可以互相通话，服务端作为混音中心：每个客户端的麦克风数据经 `set_mic_callback` 到达服务端后，由 `IAudioMixer` 负责混音，再通过 `ITransport::send_audio` 分别回传给**其他**客户端（不含自己，避免回声）。

支持 Push-to-Talk 和 VAD（语音活动检测）：PTT 由客户端控制发送时机，VAD 由服务端在混音前过滤静音帧，两者均可降低无效音频带宽。支持每客户端独立音量权重，主播声音可以优先于观众。

接口定义见 reference.md（`IAudioMixer`）。

config.json：

```json
"voice_chat": {
  "enabled": false,
  "mode": "vad",
  "vad_threshold": 0.02,
  "per_client_volume": true
}
```

#### 文字聊天

协作会话的低带宽备用通信方案，延迟比语音更低。通过 `ITransport` 控制通道广播文本消息，服务端转发给所有在线客户端，接口定义见 reference.md（`IChatChannel`）。

#### Wake-on-LAN

游戏玩家最实用的功能：客户端远程唤醒服务器，无需提前进入 RDP 或保持服务器常开。服务端监听一个独立的轻量端口（`listen()`），收到 Magic Packet 后广播到本地网段。接口同时提供 `send()` 用于级联唤醒场景（如先唤醒网关，再由网关唤醒游戏主机）。接口定义见 reference.md（`IWakeOnLan`）。

config.json：

```json
"wake_on_lan": {
  "enabled": false,
  "listen_port": 9,
  "broadcast_address": "255.255.255.255"
}
```

---

## 工程结构图

### 模块依赖关系图

```
                         ┌─────────────────────────────────────────┐
                         │              server/                    │
                         │  main.cpp         factory.cpp           │
                         └──┬──────────────────────────────────────┘
                            │ 链接所有模块
          ┌─────────────────┼──────────────────────────────────┐
          │                 │                                  │
          ▼                 ▼                                  ▼
   ┌────────────┐   ┌───────────────┐                 ┌──────────────┐
   │   core/    │   │  auth/        │                 │  capture/    │
   │  (接口层)  │◀──│  password/    │            ┌───▶│  dxgi/       │
   │            │   │  ldap/        │            │    │  pipewire/   │
   │ 零第三方   │   └───────────────┘            │    │  sckit/      │
   │ 依赖       │                                │    │  drm_virtual/│
   │            │◀───────────────────────────────┘    └──────────────┘
   │            │
   │            │◀──────────────────────────────────  ┌──────────────┐
   │            │                                      │preprocessor/ │
   │            │                                 ┌───▶  gpu/        │
   │            │                                 │    │  dmabuf/     │
   │            │◀────────────────────────────────┘    │  cpu/        │
   │            │                                      └──────────────┘
   │            │
   │            │◀──────────────────────────────────  ┌──────────────┐
   │            │                                      │  encoder/    │
   │            │                                 ┌───▶  nvenc/      │
   │            │                                 │    │  vaapi/      │
   │            │◀────────────────────────────────┘    │  videotoolbox│
   │            │                                      │  x264/       │
   │            │                                      └──────────────┘
   │            │
   │            │◀──────────────────────────────────  ┌──────────────┐
   │            │                                      │  transport/  │
   │            │                                 ┌───▶  rtp/        │
   │            │                                 │    │  webrtc/     │
   │            │◀────────────────────────────────┘    │  quic/       │
   │            │                                      └──────────────┘
   │            │
   │            │◀──────────────────────────────────  ┌──────────────┐
   └────────────┘                                      │  input/      │
                                                  ┌───▶  win32/       │
                                                  │    │  uinput/     │
                                                  │    │  iokit/      │
                                                  │    └──────────────┘
                                                  │
                                           所有 adapter 只向 core 依赖
                                           core 不知道任何 adapter 存在
```

## 工程目录结构

> **依赖链接方式图例**
> - `[static]` — 静态库，预编译后随源码入库，放在实现目录的 `lib/` 子目录，clone 即可构建
> - `[header]` — 纯头文件或 SDK 头文件；私有的放在实现目录的 `include/`，全局共享的放在 `common/include/`
> - `[system]` — 平台系统库，与 OS 版本强绑定，无法入库，使用 `pkg-config` / `find_package`
> - `[dlopen]` — 运行时动态加载，不在构建时链接
> - `[builtin]` — 使用系统自带 API，无需外部库

```
Pulsar/
│
├── CMakeLists.txt
├── config.json
│
├── common/                              # 全局共享的 header-only 依赖（极少数）
│   └── include/
│       └── nlohmann/json.hpp            # JSON 解析 [header-only]
│
├── core/                                # 平台无关核心层 — 零第三方依赖
│   ├── CMakeLists.txt
│   ├── include/                         # 接口头文件直接放此处（无冗余的 core/ 子层）
│   │   ├── pipeline.h                   # run_pipeline()、PipelineConfig
│   │   ├── frame.h                      # RawFrame、EncodedPacket、DirtyRect
│   │   ├── audio.h                      # AudioFrame、AudioPacket
│   │   ├── encoder.h                    # IEncoder
│   │   ├── transport.h                  # ITransport
│   │   ├── capture.h                    # ICaptureSource
│   │   ├── preprocessor.h               # IPreprocessor
│   │   ├── input.h                      # IInputHandler
│   │   ├── auth.h                       # IAuthProvider、AuthToken
│   │   ├── session.h                    # ISessionManager、Session
│   │   ├── audio_capture.h              # IAudioCapture
│   │   ├── audio_encoder.h              # IAudioEncoder
│   │   ├── audio_playback.h             # IAudioPlayback
│   │   ├── capabilities.h               # AdapterCapabilities
│   │   ├── queue.h                      # SPSCQueue
│   │   ├── reconnect.h                  # ReconnectPolicy、PipelineState
│   │   ├── metrics.h                    # PipelineMetrics
│   │   ├── profiler.h                   # IProfiler、ServerProfile
│   │   ├── error.h                      # StreamError
│   │   ├── logger.h                     # ILogger
│   │   ├── buffer_pool.h                # IBufferPool
│   │   ├── packet_sink.h                # IPacketSink
│   │   ├── multiplexer.h                # IOutputMultiplexer
│   │   ├── recorder.h                   # IRecorder
│   │   ├── dispatcher.h                 # IConnectionDispatcher
│   │   ├── shared_session.h             # IInputArbiter（协作会话）
│   │   ├── audio_mixer.h                # IAudioMixer
│   │   ├── chat.h                       # IChatChannel
│   │   ├── wake_on_lan.h                # IWakeOnLan
│   │   └── app_manager.h               # IAppManager
│   └── src/
│       └── pipeline.cpp
│
├── video/
│   │
│   ├── capture/
│   │   ├── linux/
│   │   │   ├── pipewire/               # PipeWire 屏幕采集 [system: libpipewire-0.3]
│   │   │   │   ├── CMakeLists.txt
│   │   │   │   ├── include/pipewire_capture.h
│   │   │   │   └── src/pipewire_capture.cpp
│   │   │   └── drm_virtual/            # DRM/KMS 虚拟显示（Headless）[system: libdrm]
│   │   │       ├── CMakeLists.txt
│   │   │       ├── include/drm_virtual_capture.h
│   │   │       └── src/drm_virtual_capture.cpp
│   │   ├── windows/
│   │   │   ├── dxgi/                   # DXGI Desktop Duplication [builtin: d3d11/dxgi]
│   │   │   │   ├── CMakeLists.txt
│   │   │   │   ├── include/dxgi_capture.h
│   │   │   │   └── src/dxgi_capture.cpp
│   │   │   └── virtual_display/        # IDD 虚拟显示（Headless）[system: wdf/idd]
│   │   └── macos/
│   │       ├── sckit/                  # ScreenCaptureKit [system: framework]
│   │       └── cgvirtual/              # CGVirtualDisplay（Headless）[system: framework]
│   │
│   ├── preprocessor/
│   │   ├── linux/
│   │   │   ├── gpu/                    # VA-API VPP BGRA→NV12 [system: libva]
│   │   │   │   ├── CMakeLists.txt
│   │   │   │   ├── include/gpu_preprocessor.h
│   │   │   │   └── src/gpu_preprocessor.cpp
│   │   │   └── dmabuf/                 # DMABUF 零拷贝导入 [system: libdrm]
│   │   │       ├── CMakeLists.txt
│   │   │       ├── include/dmabuf_importer.h
│   │   │       └── src/dmabuf_importer.cpp
│   │   ├── windows/
│   │   │   └── gpu/                    # D3D11 shader BGRA→NV12 [builtin: d3d11]
│   │   ├── macos/
│   │   │   └── gpu/                    # Metal shader BGRA→NV12 [system: Metal]
│   │   └── all/
│   │       ├── cpu/                    # 软件 BGRA→NV12，全平台源码相同 [builtin]
│   │       │   ├── CMakeLists.txt
│   │       │   ├── include/cpu_preprocessor.h
│   │       │   └── src/cpu_preprocessor.cpp
│   │       └── overlay/                # OSD 叠加（可选）
│   │
│   └── encoder/
│       ├── nvenc/                       # NVIDIA NVENC 硬件编码
│       │   ├── include/nvenc_encoder.h  # 共享公共接口（Linux/Windows API 相同）
│       │   ├── linux/                   # CUDA context [dlopen: libcuda.so + libnvidia-encode.so]
│       │   │   ├── CMakeLists.txt
│       │   │   ├── include/nvEncodeAPI.h # 私有 SDK 头文件
│       │   │   └── src/nvenc_encoder.cpp
│       │   └── windows/                 # D3D11 device（预留）
│       ├── vaapi/                       # VA-API 硬件编码（Intel/AMD/NVIDIA on Linux）
│       │   └── linux/                   # [system: libva libva-drm]
│       │       ├── CMakeLists.txt
│       │       ├── include/vaapi_encoder.h
│       │       └── src/vaapi_encoder.cpp
│       ├── amf/                         # AMD AMF 硬件编码
│       │   ├── linux/                   # [dlopen: libamfrt64.so]
│       │   └── windows/                 # [dlopen: amfrt64.dll]
│       ├── videotoolbox/                # Apple VideoToolbox 硬件编码
│       │   └── macos/                   # [system: framework VideoToolbox]
│       ├── x264/                        # 软件 H.264 兜底，全平台源码相同
│       │   └── all/
│       │       ├── CMakeLists.txt
│       │       ├── include/x264_encoder.h
│       │       ├── include/x264.h       # SDK 头文件（随源码入库）
│       │       ├── src/x264_encoder.cpp
│       │       └── lib/                 # 预编译静态库（随源码入库）
│       │           ├── linux_x86_64/libx264.a
│       │           ├── windows_x64/libx264.lib
│       │           └── macos_universal/libx264.a
│       ├── rdp/                         # RDP Bitmap Update 封装，全平台相同
│       │   └── all/
│       │       ├── CMakeLists.txt
│       │       ├── include/rdp_encoder.h
│       │       └── src/rdp_encoder.cpp
│       └── vnc/                         # RFB FramebufferUpdate 封装，全平台相同
│           └── all/
│               ├── CMakeLists.txt
│               ├── include/vnc_encoder.h
│               └── src/vnc_encoder.cpp
│
├── audio/
│   ├── capture/
│   │   ├── linux/
│   │   │   └── pipewire/               # PipeWire 系统音频 [system: libpipewire-0.3]
│   │   │       ├── CMakeLists.txt
│   │   │       ├── include/pipewire_audio_capture.h
│   │   │       └── src/pipewire_audio_capture.cpp
│   │   ├── windows/
│   │   │   └── wasapi/                 # WASAPI [builtin: Win32 COM]
│   │   └── macos/
│   │       └── coreaudio/              # CoreAudio [system: framework CoreAudio]
│   └── encoder/
│       ├── opus/                        # Opus 编码，全平台源码相同
│       │   └── all/
│       │       ├── CMakeLists.txt
│       │       ├── include/opus_encoder.h
│       │       ├── include/opus/opus.h  # SDK 头文件（随源码入库）
│       │       ├── src/opus_encoder.cpp
│       │       └── lib/
│       │           ├── linux_x86_64/libopus.a
│       │           ├── windows_x64/libopus.lib
│       │           └── macos_universal/libopus.a
│       └── aac/
│           ├── windows/                # Media Foundation AAC [builtin: Win32]
│           └── macos/                  # AudioToolbox AAC [system: framework]
│
├── transport/
│   ├── rtp/
│   │   └── all/                        # RTP/UDP，POSIX socket，全平台源码相同 [builtin]
│   │       ├── CMakeLists.txt
│   │       ├── include/rtp_transport.h
│   │       └── src/rtp_transport.cpp
│   ├── webrtc/
│   │   ├── linux/                      # libwebrtc Linux 构建 [static: libwebrtc.a]
│   │   │   ├── CMakeLists.txt
│   │   │   ├── include/webrtc_transport.h
│   │   │   └── src/webrtc_transport.cpp
│   │   └── windows/                    # 预留
│   ├── quic/
│   │   ├── linux/                      # ngtcp2 [system: libngtcp2]
│   │   │   ├── CMakeLists.txt
│   │   │   ├── include/quic_transport.h
│   │   │   └── src/quic_transport.cpp
│   │   └── windows/                    # msquic（预留）
│   ├── rdp/
│   │   └── all/                        # RDP TCP 握手，全平台相同 [builtin: POSIX]
│   │       ├── CMakeLists.txt
│   │       ├── include/rdp_transport.h
│   │       └── src/rdp_transport.cpp
│   ├── vnc/
│   │   └── all/                        # RFB TCP 握手，全平台相同 [builtin: POSIX]
│   │       ├── CMakeLists.txt
│   │       ├── include/vnc_transport.h
│   │       └── src/vnc_transport.cpp
│   ├── rtmp/
│   │   └── all/                        # RTMP 推流（预留）[static: librtmp.a]
│   └── srt/
│       └── all/                        # SRT 推流（预留）[static: libsrt.a]
│
├── input/
│   ├── linux/
│   │   └── uinput/                     # uinput 内核注入 [builtin: /dev/uinput]
│   │       ├── CMakeLists.txt
│   │       ├── include/uinput_handler.h
│   │       └── src/uinput_handler.cpp
│   ├── windows/
│   │   └── win32/                      # SendInput / ViGEm [builtin: Win32 API]
│   └── macos/
│       └── iokit/                      # IOKit CGEvent [system: framework IOKit]
│
├── auth/
│   └── all/                            # 认证逻辑全平台相同
│       ├── password/                   # [builtin: 自实现]
│       │   ├── CMakeLists.txt
│       │   ├── include/password_auth.h
│       │   └── src/password_auth.cpp
│       ├── oauth/                      # [static: libcurl.a]
│       ├── passkey/                    # [static: libfido2.a]
│       └── ldap/                       # [static: libldap.a]
│
└── server/
    ├── CMakeLists.txt
    ├── include/                         # server 私有头文件（无冗余的 server/ 子层）
    │   ├── factory.h
    │   ├── config.h
    │   ├── config_parser.h
    │   └── profiler.h
    └── src/
        ├── factory.cpp
        ├── config_parser.cpp
        ├── profiler.cpp
        └── main.cpp
```

### 依赖链接方式汇总

| 模块 | 依赖库 | 链接方式 | 平台 |
|------|-------|---------|------|
| `video/encoder/nvenc/linux/` | nvEncodeAPI.h | `[header]` 入库至 `linux/include/` | Linux |
| `video/encoder/nvenc/linux/` | libnvidia-encode.so.1 | `[dlopen]` 运行时加载 | Linux |
| `video/encoder/vaapi/linux/` | libva / libva-drm | `[system]` pkg-config | Linux |
| `video/encoder/amf/linux/` | AMF headers | `[header]` 入库至 `linux/include/` | Linux/Win |
| `video/encoder/amf/linux/` | libamfrt64.so | `[dlopen]` 运行时加载 | Linux/Win |
| `video/encoder/videotoolbox/macos/` | VideoToolbox.framework | `[system]` framework | macOS |
| `video/encoder/x264/all/` | libx264.a | `[static]` 入库至 `all/lib/` | 全平台 |
| `video/encoder/x264/all/` | x264.h | `[header]` 入库至 `all/include/` | 全平台 |
| `audio/encoder/opus/all/` | libopus.a | `[static]` 入库至 `all/lib/` | 全平台 |
| `audio/encoder/opus/all/` | opus.h | `[header]` 入库至 `all/include/opus/` | 全平台 |
| `audio/capture/linux/pipewire/` | libpipewire-0.3 | `[system]` pkg-config | Linux |
| `audio/capture/windows/wasapi/` | — | `[builtin]` Win32 COM | Windows |
| `audio/capture/macos/coreaudio/` | CoreAudio.framework | `[system]` framework | macOS |
| `video/capture/linux/pipewire/` | libpipewire-0.3 | `[system]` pkg-config | Linux |
| `video/capture/linux/drm_virtual/` | libdrm | `[system]` pkg-config | Linux |
| `video/capture/windows/dxgi/` | d3d11 / dxgi | `[builtin]` Win32 | Windows |
| `video/capture/macos/sckit/` | ScreenCaptureKit.framework | `[system]` framework | macOS |
| `transport/rtp/all/` | — | `[builtin]` POSIX socket | 全平台 |
| `transport/webrtc/linux/` | libwebrtc | `[static]` 入库至 `linux/lib/` | Linux/Win |
| `transport/quic/linux/` | libngtcp2 | `[system]` pkg-config | Linux |
| `input/linux/uinput/` | — | `[builtin]` /dev/uinput | Linux |
| `video/preprocessor/all/cpu/` | libyuv.a（正式版） | `[static]` 入库至 `all/lib/` | 全平台 |
| `video/preprocessor/linux/dmabuf/` | libdrm | `[system]` pkg-config | Linux |
| `video/preprocessor/linux/gpu/` | libva / libva-drm | `[system]` pkg-config | Linux |
| `core/` | 无 | — | 全平台，零外部依赖 |

---

## 开发路线

### MVP（最小可运行版本）目标
> 能在 Linux 或 Windows 上跑通一路有声音的视频串流，客户端可以控制服务器鼠标键盘。
> 所有高级功能（RDP/VNC/Headless/Profiler/多路推流）都在 MVP 之后添加。

```
━━━ 阶段一：MVP ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

第一步：定义 core/ 层核心接口（MVP 子集）
        frame.h / audio.h / queue.h / buffer_pool.h / capabilities.h
        error.h / logger.h（StreamError + ILogger）
        不写任何实现

第二步：实现 pipeline.cpp（基础版）
        视频：Capture → Encoder → Transport（跳过 Preprocessor，nullptr 路径）
        音频：AudioCapture → AudioEncoder → Transport
        SPSC 队列 + 背压 + 音视频 pts_us 对齐

第三步：MVP 最小链路
        Linux：PipeWireCapture → nullptr → VAAPIEncoder / NVENCEncoder / x264（按可用性自动选）→ RTP + PipeWire 音频
        Windows：DXGICapture → nullptr(NV12匹配) 或 CPUPreprocessor → NVENCEncoder / AMF / x264（按可用性自动选）→ RTP + WASAPI
        uinput / SendInput 输入注入
        编码器优先级：NVENC > VAAPI(Linux) / AMF(Windows) > x264
        验证：客户端能看到画面、听到声音、控制鼠标键盘

第四步：基础 Session + 认证
        PasswordAuth + ISessionManager + 多端口监听
        StreamError 统一错误处理，ILogger 接入

━━━ 阶段二：质量与稳定性 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

第五步：Preprocessor + 硬件编码 + 零拷贝
        Linux：DMABUFImporter → VAAPIEncoder 或 NVENCEncoder（全程零拷贝，按 GPU 厂商自动选）
        Windows：GPUPreprocessor → NVENCEncoder 或 AMFEncoder（GPU 内转换，按 GPU 厂商自动选）
        两平台均以 x264 作为无硬件加速时的最终兜底
        Profiler 双维度编码器选择，Preprocessor nullptr 路径验证

第六步：断线重连
        PipelineState 状态机（Suspended → Recovering → Running）
        设备丢失降级（NVENC crash → x264），恢复 < 500ms

第七步：WebRTC / QUIC + FEC
        WebRTCTransport（NAT 穿透，DataChannel 输入）
        QuicTransport + FEC 动态开关

━━━ 阶段三：协议扩展 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

第八步：RDP + VNC
        RDPEncoder + RDPTransport（mstsc 直连验证）
        VNCEncoder + VNCTransport（VNC 客户端验证）

第九步：Headless
        Linux：DRM/KMS virtual display + PipeWire
        Windows：IDD 虚拟显示驱动

━━━ 阶段四：高级功能 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

第十步：macOS 平台
        ScreenCaptureKit + VideoToolbox + CoreAudio + IOKit

第十一步：游戏/直播扩展
        多路串流（IOutputMultiplexer）+ 本地录制
        OAuth/Passkey 认证，协作会话（IInputArbiter）
        应用程序启动管理（IAppManager）
```