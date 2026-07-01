> 参见 [01-blueprint.md](01-blueprint.md) 查看系统蓝图与设计目标

# 串流服务器实现参考

---

## 接口定义（core/ 层）

### 优化全景图

下表列出本架构支持的所有优化类别，以及每个优化在哪个模块实现、当前接口是否已预留扩展点：

| 优化类别 | 所在模块 | 接口扩展点 |
|---------|---------|-----------|
| 零拷贝（GPU buffer 传递） | Capture → Preprocessor → Encoder | `FrameBuffer::native_handle()` |
| 脏区域更新（只传变化像素） | Capture | `RawFrame::dirty_rects` |
| HDR / 色彩空间透传 | Capture → Preprocessor → Encoder | `RawFrame::color_info` |
| 帧率自适应（静止降采集率） | Pipeline | `PipelineConfig::idle_fps` |
| Preprocessor 直通模式 | Preprocessor | `IPreprocessor::process()` 返回 `optional` |
| GOP 结构控制（B 帧/低延迟预设）| Encoder | `EncoderParams::gop_config` |
| 强制关键帧（场景切换） | Encoder | `SubmitFlags::force_keyframe` |
| 码率自适应（网络拥塞） | Transport → Encoder | `TransportEvent` + `update_params()` |
| FEC 前向纠错 | Transport | `ITransport::set_fec_params()` |
| 网络指标反馈（RTT/丢包/带宽）| Transport → Pipeline | `NetworkStats` 结构 |
| scatter-gather 批量发送 | Transport | `send_batch()` |
| 自适应抖动缓冲 | Transport | `set_jitter_buffer()` |
| 发送端 pacing | Transport | `pacing_enabled` 配置项 |
| SPSC 无锁队列 + 背压 | Pipeline | `BoundedQueue<T>` |
| BufferPool 预分配 | 全局 | `IBufferPool` |
| 线程优先级 + CPU 亲和性 | Pipeline | `ThreadConfig` |
| 单调时钟统一时间戳 | Capture | `RawFrame::pts_us` + 注释 |
| QoS 策略（按用户码率/帧率）| SessionManager | `QosPolicy` |
| 工厂能力协商（自动最优链路）| Factory | `AdapterCapabilities` |
| 服务器探测（自动选最优编码器）| Profiler | `IProfiler`、`ServerProfile` |
| 客户端协议自动检测 | ConnectionDispatcher | `IConnectionDispatcher`、`ProtocolHint` |
| Headless 无显示器支持 | VirtualDisplayCapture | `supports_headless` 能力标志 |
| 音频管道（采集/编码/AV Sync）| IAudioCapture + IAudioEncoder | `AudioFrame::pts_us` 对齐 |
| 断线重连（状态保留/恢复）| Pipeline 状态机 | `ReconnectPolicy`、`PipelineState` |
| 网络抖动弹性（FEC+NACK+jitter）| Transport | `NetworkStats::nack_requests/fec_recovered` |
| 编码器崩溃降级（NVENC→x264）| Pipeline 错误处理 | 异常捕获 + 重新初始化 |
| 客户端能力协商（编解码器/分辨率）| ICapabilityNegotiator | `ClientCapabilities`、`NegotiatedParams` |
| 硬件光标采集与客户端合成 | ICaptureSource | `CursorState`、`next_cursor()` |
| 多显示器枚举与热切换 | ICaptureSource | `DisplayInfo`、`enumerate_displays()` |
| VSync 感知（对齐显示刷新率）| ICaptureSource | `display_refresh_rate()` |
| TLS 证书管理与自动续签 | 安全层 | `IPairingManager`、`security` 配置 |
| 客户端授权（OAuth / Passkey）| IAuthProvider | `OAuthAuth`、`PasskeyAuth` Adapter |
| NAT 穿透（UPnP/STUN/TURN）| Transport | `nat` 配置节点 |
| 虚拟游戏手柄（ViGEm/uinput）| IInputHandler | `create_gamepad()`、`destroy_gamepad()` |
| 剪贴板同步 | IInputHandler | `set_clipboard()`、`get_clipboard()` |
| 触摸屏输入（移动端客户端）| IInputHandler | `TouchPoint`、`TouchBegin/Update/End` |
| 手柄振动反馈（服务端→客户端）| IInputHandler + ITransport | `HapticCommand`、`send_haptic()` |
| 双向音频（麦克风）| IAudioPlayback + ITransport | `set_mic_callback()`、`IAudioPlayback` |
| 多人语音（Party Chat）| IAudioMixer | `push()`、`mix_for()`、`set_volume()`、PTT/VAD |
| 文字聊天 | IChatChannel | `broadcast()`、`set_message_callback()` |
| Wake-on-LAN | IWakeOnLan | `listen()`、`send()`、Magic Packet 广播 |
| 多路串流（多客户端/推流）| IOutputMultiplexer | `add_sink()`、`broadcast()` |
| 本地录制 | IRecorder | `write_video()`、`write_audio()` |
| 第三方推流（RTMP/SRT）| Transport Adapter | `transport/rtmp/`、`transport/srt/` |
| 应用程序启动管理 | IAppManager | `launch()`、`terminate()`、`list_apps()` |
| 画质预设（低延迟/高画质）| IEncoder | `QualityPreset` 枚举 |
| 高刷新率（144Hz/240Hz）| IEncoder + Pipeline | `GopConfig::target_fps`、队列容量自适应 |
| OSD 统计叠加 | ITransport / OverlayPreprocessor | `send_stats()`、`preprocessor/overlay/` |
| 协作会话（多客户端共享+输入仲裁）| IInputArbiter | `ClientDeviceBinding`、`bind()`、`allow()` |
| 设备配对与信任管理 | IPairingManager | `authorized_clients/` 目录 + TOTP 可选 |

### 零拷贝与缓冲区设计

原始接口用裸指针传递数据，每个阶段都会发生内存拷贝。在 60fps、4K 分辨率下每帧约 32MB，每秒约 1.9GB 的无效拷贝。

**核心思路：用 `shared_ptr` 传递所有权，各阶段只传递"句柄"，不拷贝像素数据。**

```
DXGI 显存 → FrameBuffer（持有显存句柄）→ 编码器直接读显存 → PacketBuffer（持有编码器输出句柄）→ Transport 直接发送
```

硬件路径（DXGI → NVENC）可以完全不经过 CPU 内存。

### 数据结构

```cpp
// core/include/core/frame.h

enum class PixelFormat   { BGRA, RGBA, NV12, YUV420 };
enum class CodecType     { H264, H265, AV1 };
enum class ColorSpace    { BT709, BT2020 };        // 色彩空间
enum class TransferFunc  { SDR, PQ, HLG };         // HDR 传输函数

// HDR / 色彩空间元数据：Capture 填写，Preprocessor 和 Encoder 读取
// 支持 HDR 内容端到端透传，不丢失 HDR 信息
struct ColorInfo {
    ColorSpace   color_space   = ColorSpace::BT709;
    TransferFunc transfer_func = TransferFunc::SDR;
    float        max_luminance = 100.0f;   // nits，SDR 时为 100
    float        min_luminance = 0.0f;
};

// 脏区域：Capture 填写，描述本帧相对上一帧的变化区域
// Preprocessor 可只对脏区域做格式转换，Encoder 可使用 intra-refresh 而非全帧刷新
// 静止画面时 dirty_rects 为空，Pipeline 可据此触发帧率自适应降速
struct DirtyRect {
    int x, y, width, height;
};

// 抽象帧缓冲区：各 Adapter 子类化后持有平台原生句柄
// native_handle() 供 GPU 路径使用（DXGI 纹理、DMABUF fd、IOSurface 等），完全绕过 CPU 拷贝
struct FrameBuffer {
    virtual ~FrameBuffer() = default;
    virtual uint8_t* data()          const = 0;
    virtual size_t   size()          const = 0;
    virtual void*    native_handle() const { return nullptr; }
};

// 抽象编码输出缓冲区：同理
struct PacketBuffer {
    virtual ~PacketBuffer() = default;
    virtual const uint8_t* data()          const = 0;
    virtual size_t         size()          const = 0;
    virtual void*          native_handle() const { return nullptr; }
};

struct RawFrame {
    std::shared_ptr<FrameBuffer> buffer;
    int         width;
    int         height;
    // PTS 单位：微秒，来源：采集端使用单调时钟
    //   Linux/POSIX：CLOCK_MONOTONIC
    //   Windows：QueryPerformanceCounter
    //   macOS：mach_absolute_time
    // 所有下游阶段只读，禁止修改
    int64_t     pts_us;
    PixelFormat format;
    ColorInfo   color_info;                        // HDR / 色彩空间元数据
    std::vector<DirtyRect> dirty_rects;            // 空 = 全帧变化（或无脏区域信息）
};

struct EncodedPacket {
    std::shared_ptr<PacketBuffer> buffer;
    bool      is_keyframe;
    int64_t   pts_us;
    CodecType codec;    // Transport 依据此字段选择封包格式（如 RTP H264/H265 payload type）
};

// InputEvent 完整定义见 core/include/core/input.h（IInputHandler 章节）
```

### 缓冲区池

预分配固定数量的 FrameBuffer，运行时复用，避免每帧 heap 分配/释放造成延迟抖动。

```cpp
// core/include/core/buffer_pool.h
class IBufferPool {
public:
    virtual ~IBufferPool() = default;
    // 获取一个空闲缓冲区；若池中无空闲则阻塞等待
    virtual std::shared_ptr<FrameBuffer> acquire() = 0;
};
```

### 模块间队列（背压机制）

```cpp
// core/include/core/queue.h
// 内部实现工具，不是对外端口接口，外部模块不应依赖此类。
// Pipeline 内部直接实例化 SPSCQueue / MPMCQueue。
template<typename T>
class SPSCQueue {   // 单生产者单消费者，无锁，用于相邻阶段（Q1/Q2/Q3）
public:
    explicit SPSCQueue(size_t capacity);
    bool try_push(T item, int timeout_ms = 0);   // 队列满时返回 false
    bool try_pop(T& item,  int timeout_ms = 0);  // 队列空时返回 false
};

template<typename T>
class MPMCQueue {   // 多生产者多消费者，用于多路输入场景
public:
    explicit MPMCQueue(size_t capacity);
    bool try_push(T item, int timeout_ms = 0);
    bool try_pop(T& item,  int timeout_ms = 0);
};
```

### 能力协商

工厂在组装时查询各 Adapter 的能力，自动选择最优链路（如优先零拷贝路径）。

```cpp
// core/include/core/capabilities.h
struct AdapterCapabilities {
    std::vector<PixelFormat> input_formats;    // 支持的输入格式
    std::vector<PixelFormat> output_formats;   // 支持的输出格式
    std::vector<ColorSpace>  color_spaces;     // 支持的色彩空间（含 HDR）
    bool supports_dmabuf;                      // Linux 零拷贝路径
    bool supports_gpu_preprocessing;           // GPU 格式转换
    bool supports_async_encode;                // 异步编码提交
    bool supports_dirty_rect;                  // 脏区域感知编码
    bool supports_hdr;                         // HDR 编码输出
    bool supports_headless;                    // 无物理显示器时可用（虚拟显示 Adapter）
    bool requires_display;                     // 依赖物理/虚拟显示器（DXGICapture = true）
};

// 每个 Adapter 实现此接口，工厂据此选择最优组合
class ICapabilityProvider {
public:
    virtual AdapterCapabilities capabilities() const = 0;
};
```

### 线程配置

```cpp
// core/include/core/thread_config.h
struct ThreadConfig {
    int priority;       // 线程优先级（越高越优先）
    int cpu_affinity;   // 绑定的 CPU 核心编号，-1 表示不绑定
};
```

各平台实现方式：

| 平台 | 优先级 API | CPU 亲和性 API |
|------|-----------|--------------|
| Linux | `pthread_setschedparam` + `SCHED_FIFO`，或 `chrt -f <priority> <pid>` | `sched_setaffinity(pid, sizeof(mask), &mask)` |
| Windows | `SetThreadPriority(THREAD_PRIORITY_HIGHEST)` | `SetThreadAffinityMask` |
| macOS | `pthread_setschedparam` + `SCHED_RR` | `thread_policy_set` |

Linux 注意事项：`SCHED_FIFO` 需要 `CAP_SYS_NICE` 权限（root 或 `setcap cap_sys_nice+ep`），无权限时自动降级为 `nice -5`，记录 `LogLevel::Warn`，不崩溃。

建议线程配置（可由 `config.json threading.*` 覆盖）：

| 阶段 | 优先级 | CPU 绑定 | 原因 |
|------|-------|---------|------|
| 采集 | 高（FIFO 80） | 绑定核 0 | 帧率节拍，抖动直接影响延迟 |
| 编码 | 高（FIFO 70） | 绑定核 2 | 吞吐量，避免被其他线程抢占 |
| 传输 | 中（FIFO 50） | 不绑定 | 受网络 I/O 限制，绑核收益小 |
| 音频采集/编码 | 中（FIFO 60） | 不绑定 | 实时但容忍小抖动 |

### 可观测性（Metrics）

各模块向统一的 Metrics 收集器上报运行时数据，用于监控、调优和自动参数调整。

```cpp
// core/include/core/metrics.h

struct PipelineMetrics {
    // 采集阶段
    float   capture_fps;              // 实际采集帧率
    int64_t capture_latency_us;       // 采集到推入 Q1 的延迟
    int     capture_dropped_frames;   // 因背压丢弃的帧数

    // 编码阶段
    float   encode_fps;               // 实际编码帧率
    int64_t encode_latency_us;        // 从 submit 到 callback 的延迟
    float   encode_bitrate_kbps;      // 实际输出码率

    // 传输阶段
    float   transport_rtt_ms;         // 往返延迟
    float   transport_loss_rate;      // 丢包率（0.0 ~ 1.0）
    float   transport_bandwidth_kbps; // 可用带宽估算

    // 端到端
    int64_t e2e_latency_us;           // 从采集到发送完成的总延迟
};

class IMetricsCollector {
public:
    virtual ~IMetricsCollector() = default;
    virtual void report(const PipelineMetrics& m) = 0;
};
```

各模块持有 `IMetricsCollector*`（由 pipeline 注入），上报自己的数据。Pipeline 汇总后可用于：
- 日志记录
- 触发自动码率调整
- 暴露给外部监控系统（HTTP API / stdout）

### Adapter 接口（端口定义）

#### ICaptureSource

```cpp
// core/include/core/capture.h

// 硬件光标信息：与视频帧分离传输，因光标更新频率远高于视频帧率
// 客户端在本地合成光标，避免光标被编码进视频（导致光标模糊/延迟）
struct CursorState {
    int     x, y;                       // 光标位置（相对于采集区域）
    bool    visible;
    std::vector<uint8_t> image_rgba;    // 自定义光标图像（为空则用系统默认）
    int     image_width;
    int     image_height;
    int     hotspot_x, hotspot_y;       // 热点偏移
};

// 显示器信息：用于多显示器枚举和切换
struct DisplayInfo {
    int         index;
    std::string name;                   // 显示器名称（如 "\\.\DISPLAY1"）
    int         width;
    int         height;
    int         refresh_rate;           // Hz，用于 VSync 感知和帧率匹配
    bool        is_primary;
    bool        hdr_supported;
};

enum class CaptureEvent { DeviceLost, FormatChanged, ResolutionChanged, DisplayChanged };

class ICaptureSource : public ICapabilityProvider {
public:
    virtual ~ICaptureSource() = default;

    // 无新帧时返回 nullopt（画面静止 / idle_fps 节流）
    virtual std::optional<RawFrame> next_frame() = 0;

    // 光标状态：独立于视频帧，由 Transport 通过独立消息通道发送给客户端
    // 客户端本地合成光标，延迟感知优于编码进视频
    virtual std::optional<CursorState> next_cursor() = 0;

    // 多显示器：枚举所有可用显示器，支持会话中途动态切换
    virtual std::vector<DisplayInfo> enumerate_displays() const = 0;
    virtual void select_display(int index) = 0;

    // VSync 感知：采集端对齐显示器刷新率，减少撕裂和采集延迟
    // 返回当前选中显示器的刷新率（Hz），编码器据此设置目标帧率上限
    virtual int display_refresh_rate() const = 0;

    // 窗口串流（可选扩展）：只采集特定应用窗口而非整个桌面
    // 未实现时返回 false，工厂降级到桌面采集
    virtual bool supports_window_capture() const { return false; }
    virtual bool select_window(uint64_t window_id) { return false; }

    virtual void set_event_callback(std::function<void(CaptureEvent)> cb) = 0;
};
```

#### IPreprocessor

```cpp
// core/include/core/preprocessor.h
class IPreprocessor : public ICapabilityProvider {
public:
    virtual ~IPreprocessor() = default;

    // 格式转换、缩放、色彩空间转换
    // 返回 nullopt 表示"直通"：上游格式已满足编码器要求，无需处理
    // 避免不必要的转换开销（如 Capture 已输出 NV12，Encoder 也接受 NV12）
    virtual std::optional<RawFrame> process(RawFrame frame) = 0;
};
```

#### IEncoder

```cpp
// core/include/core/encoder.h

struct GopConfig {
    int  max_b_frames;       // B 帧数量，低延迟场景设为 0
    int  gop_size;           // 两个关键帧之间的最大帧数
    bool low_latency_preset; // 开启编码器内置低延迟预设
    bool intra_refresh;      // 用渐进式刷新替代关键帧，降低突发码率
    int  target_fps;         // 帧率变化时自动等比缩放 gop_size（gop_size = target_fps * gop_seconds）
};

enum class QualityPreset {
    LowLatency,   // max_b_frames=0, low_latency_preset=true，优先延迟（竞技游戏）
    Balanced,     // 默认平衡
    HighQuality,  // 允许 B 帧，更高 gop_size，优先画质（高画质直播/录制）
};

struct EncoderParams {
    int          bitrate_kbps;
    int          fps;
    GopConfig    gop;
    QualityPreset preset = QualityPreset::LowLatency;
};

// submit_frame 的控制标志
enum class SubmitFlags : uint32_t {
    None          = 0,
    ForceKeyframe = 1 << 0,   // 强制本帧为关键帧（场景切换、客户端请求时使用）
};

class IEncoder : public ICapabilityProvider {
public:
    virtual ~IEncoder() = default;

    // 异步提交：立即返回，编码完成后触发 on_encoded 回调
    // 编码线程与传输线程因此可并行工作
    virtual void submit_frame(RawFrame frame,
                              SubmitFlags flags = SubmitFlags::None) = 0;
    virtual void set_encoded_callback(std::function<void(EncodedPacket)> cb) = 0;

    // 网络拥塞时由 pipeline 层调用，无需重建编码器
    // 含 GOP 控制：网络持续拥塞时可动态增大 gop_size、关闭 B 帧以降低延迟
    virtual void update_params(const EncoderParams& params) = 0;
};
```

#### ITransport

```cpp
// core/include/core/transport.h

// 网络实时指标：Transport 采样后通过 set_stats_callback 上报
// Pipeline 据此触发码率调整和帧率自适应
struct NetworkStats {
    float   rtt_ms;             // 往返延迟
    float   loss_rate;          // 丢包率（0.0 ~ 1.0）
    float   bandwidth_kbps;     // 可用带宽估算（来自拥塞控制算法）
    int64_t jitter_us;          // 抖动（延迟方差）
    int     nack_requests;      // 客户端请求重传次数（高 = 丢包严重）
    int     fec_recovered;      // FEC 恢复的包数（用于评估 FEC 效果）
};

// FEC（前向纠错）参数：在丢包率较高时开启，牺牲带宽换取无需重传的可靠性
// 适用于实时场景（重传会引入延迟，FEC 在延迟敏感场景比重传更优）
struct FecParams {
    bool    enabled;
    int     data_shards;        // 原始数据分片数
    int     parity_shards;      // 冗余分片数，可恢复 parity_shards 个丢包
};

enum class TransportEvent { Disconnected, Congested, Ready };

// core/include/core/transport.h
// #include "packet_sink.h"   ← IPacketSink 定义于此（独立头文件，避免与 multiplexer.h 循环包含）
//
// ITransport 继承 IPacketSink，可直接注册到 IOutputMultiplexer。
// on_packet() 是 IPacketSink 要求的统一接口，实现上等同于 send()；
// send() 保留为语义更清晰的直接调用入口，实现类通常让 send() 调用 on_packet() 或反之。
class ITransport : public IPacketSink {
public:
    virtual ~ITransport() = default;

    // 连接生命周期
    virtual bool connect(const std::string& endpoint) = 0;   // 建立连接
    virtual void disconnect() = 0;                            // 主动断开

    // 单包发送（on_packet 为 IPacketSink 的统一入口，两者等价）
    virtual void send(EncodedPacket packet) = 0;

    // scatter-gather 批量发送：多包合并为一次系统调用，降低 syscall 开销
    // 适合高帧率场景（120fps 时每秒 120 次 syscall → 若干次批量调用）
    virtual void send_batch(std::vector<EncodedPacket> packets) = 0;

    // 网络状态事件回调：Congested → pipeline 通知 Encoder 降码率；Ready → 恢复
    virtual void set_event_callback(std::function<void(TransportEvent)> cb) = 0;

    // 网络指标定期上报（每隔 200ms 左右），供 Pipeline 和 Metrics 使用
    // 比 TransportEvent 更细粒度：含 RTT、丢包率、带宽估算数值
    virtual void set_stats_callback(std::function<void(NetworkStats)> cb) = 0;

    // 自适应抖动缓冲：网络良好时缩小（降低延迟），抖动大时放大（防卡顿）
    virtual void set_jitter_buffer(int min_ms, int max_ms) = 0;

    // FEC 前向纠错：丢包率上升时由 Pipeline 动态开启，下降时关闭
    virtual void set_fec_params(const FecParams& params) = 0;

    // 输入反向通道：Transport 收到客户端输入事件后触发此回调
    // Pipeline 将其接到 IInputHandler::inject()
    // RDPTransport 通过 RDP 虚拟通道接收，WebRTC 通过 DataChannel 接收
    virtual void set_input_callback(std::function<void(InputEvent)> cb) = 0;

    // 音频发送（AV 同步：按 pts_us 与视频交错发送）
    virtual void send_audio(AudioPacket packet) = 0;

    // 手柄振动反馈 → 客户端
    virtual void send_haptic(const HapticCommand& cmd) = 0;

    // 实时统计数据 → 客户端 OSD 展示
    virtual void send_stats(const PipelineMetrics& m) = 0;

    // 双向音频：客户端麦克风数据到达时触发，服务端将其送入 IAudioPlayback
    virtual void set_mic_callback(std::function<void(AudioFrame)> cb) = 0;
};
```

#### IInputHandler

```cpp
// core/include/core/input.h

// 触摸点（移动端客户端多点触摸）
struct TouchPoint {
    int   id;          // 触摸追踪 ID（多指时区分手指）
    float x, y;        // 归一化坐标（0.0~1.0，相对于串流画面）
    float pressure;    // 压力（0.0~1.0，不支持时为 1.0）
    bool  down;        // true = 按下，false = 抬起
};

// 手柄振动命令（服务端 → 客户端反向，从 ViGEm/XInput 读取后发回客户端）
struct HapticCommand {
    int   gamepad_id;
    float low_freq;    // 低频振动强度（0.0~1.0）
    float high_freq;   // 高频振动强度（0.0~1.0）
    int   duration_ms;
};

struct InputEvent {
    enum class Type {
        KeyDown, KeyUp,
        MouseMove, MouseButton, MouseWheel,
        GamepadButton, GamepadAxis,
        TouchBegin, TouchUpdate, TouchEnd,  // 移动端多点触摸（客户端 → 服务端）
        ClipboardText,
    };
    Type    type;
    int32_t code;
    int32_t value;
    int32_t gamepad_id;
    std::vector<TouchPoint> touches;        // TouchBegin/Update/End 时填写
};

class IInputHandler {
public:
    virtual ~IInputHandler() = default;
    virtual void inject(const InputEvent& event) = 0;

    // 虚拟游戏手柄管理
    //   Windows：ViGEm 驱动注入虚拟 Xbox 360 / DualShock 4
    //   Linux：uinput kernel module 创建虚拟手柄
    //   macOS：暂不支持
    virtual bool create_gamepad(int id)  { return false; }
    virtual void destroy_gamepad(int id) {}

    // 手柄振动反馈（服务端 → 客户端）
    // Pipeline 从 ViGEm/XInput 轮询振动状态后，通过 Transport::send_haptic() 发回客户端
    virtual void set_haptic_callback(std::function<void(HapticCommand)> cb) {}

    // 剪贴板同步（需 config.json clipboard.enabled = true）
    virtual void set_clipboard(const std::string& text) {}
    virtual std::string get_clipboard() const { return {}; }
};
```

`ITransport` 完整接口定义见上方（含 `send_haptic`、`send_stats`、`send_audio`）。

各平台 Input Adapter 实现：

| 功能 | Windows | Linux | macOS |
|------|---------|-------|-------|
| 键盘/鼠标 | `SendInput` | `uinput` | IOKit `CGEvent` |
| 触摸注入 | `InjectTouchInput` | `uinput MT` | `CGEventCreateMouseEvent` |
| 虚拟手柄 | `ViGEm` 驱动 | `uinput gamepad` | ❌ 不支持 |
| 振动读取 | `XInput GetState` | `/dev/input` forcefeedback | ❌ 不支持 |
| 剪贴板 | OLE Clipboard | X11 Selection / Wayland | NSPasteboard |

**Linux uinput 权限**：默认需要 root 或 `input` 用户组。推荐通过 udev 规则授权：

```
# /etc/udev/rules.d/99-stream-server.rules
KERNEL=="uinput", GROUP="input", MODE="0660"
```

服务器启动时检测权限，无权限时记录 `StreamError::InputPermissionDenied` 并提示用户添加 udev 规则，不静默失败。

### 客户端协议自动检测（ConnectionDispatcher）

服务器按接入端口直接判断协议，各协议独立端口监听，无需字节嗅探：

```json
"protocols": {
  "rtp":    { "enabled": true, "port": 47984 },
  "webrtc": { "enabled": true, "port": 47985 },
  "quic":   { "enabled": true, "port": 47986 },
  "rdp":    { "enabled": true, "port": 3389  },
  "vnc":    { "enabled": true, "port": 5900  }
}
```

接口定义：

```cpp
// core/include/core/dispatcher.h
enum class ProtocolHint { RDP, VNC, WebRTC, QUIC, RTP, Unknown };

class IConnectionDispatcher {
public:
    virtual ~IConnectionDispatcher() = default;
    // 按监听端口直接判断协议，无需字节嗅探
    virtual ProtocolHint detect_by_port(int port) const = 0;
};
```

工厂层根据 `ProtocolHint` 组装不同的 Adapter 链路，同一台服务器可以同时被 `mstsc`、WebRTC 客户端、自定义客户端连接，无需修改任何配置。

### 多路输出（IOutputMultiplexer / IRecorder）

```cpp
// core/include/core/packet_sink.h
// IPacketSink 单独放在此文件，避免 transport.h 与 multiplexer.h 之间的循环包含：
//   transport.h   #include "packet_sink.h"   (ITransport 继承 IPacketSink)
//   multiplexer.h #include "packet_sink.h"   (IOutputMultiplexer::add_sink 使用 IPacketSink)
//   两者不互相包含

// 统一 sink 基类：ITransport 和 IRecorder 都继承此接口，
// 使两者都能注册到 IOutputMultiplexer
class IPacketSink {
public:
    virtual ~IPacketSink() = default;
    virtual std::string sink_id() const = 0;
    virtual void on_packet(EncodedPacket packet) = 0;
    virtual void on_audio(AudioPacket packet) = 0;
};

class IOutputMultiplexer {
public:
    virtual ~IOutputMultiplexer() = default;
    virtual void add_sink(std::shared_ptr<IPacketSink> sink) = 0;
    virtual void remove_sink(const std::string& sink_id) = 0;
    // 每个 EncodedPacket 广播到所有已注册 sink，shared_ptr 零拷贝
    virtual void broadcast(EncodedPacket packet) = 0;
    virtual void broadcast_audio(AudioPacket packet) = 0;
};
```

```cpp
// core/include/core/recorder.h

// IRecorder 继承 IPacketSink，可直接注册到 IOutputMultiplexer
class IRecorder : public IPacketSink {
public:
    virtual void start(const std::string& output_path) = 0;
    virtual void stop() = 0;
    // on_packet / on_audio 由 IPacketSink 继承，IRecorder 实现写入容器
};
// 实现：MP4 容器（H.264/H.265 + AAC）
```

`ITransport` 继承 `IPacketSink`，完整接口定义见上方 ITransport 章节。

### 应用程序启动管理（IAppManager）

```cpp
// core/include/core/app_manager.h

struct AppEntry {
    std::string id;
    std::string name;
    std::string executable;
    std::string args;
    std::string working_dir;
    std::string cover_image;    // 封面图路径，客户端展示游戏列表用
};

class IAppManager {
public:
    virtual ~IAppManager() = default;
    virtual std::vector<AppEntry> list_apps() const = 0;
    virtual bool launch(const std::string& app_id) = 0;
    virtual bool terminate(const std::string& app_id) = 0;
    virtual bool is_running(const std::string& app_id) const = 0;
};
```

### 协作会话（IInputArbiter）

```cpp
// core/include/core/shared_session.h

struct ClientDeviceBinding {
    std::string                   client_id;
    std::vector<InputEvent::Type> claimed_types;  // 该客户端负责的输入类型
    int                           gamepad_id = -1; // 手柄槽位（-1 = 不使用）
};

class IInputArbiter {
public:
    virtual ~IInputArbiter() = default;
    virtual void bind(const ClientDeviceBinding& binding) = 0;
    virtual void unbind(const std::string& client_id) = 0;
    // 类型匹配则允许注入，否则丢弃
    virtual bool allow(const std::string& client_id, const InputEvent& event) const = 0;
    virtual std::vector<ClientDeviceBinding> list_bindings() const = 0;
};
```

Pipeline 集成：

```cpp
// pipeline.cpp：所有客户端输入经 Arbiter 按设备类型过滤后注入
for (auto& [client_id, transport] : transports) {
    transport->set_input_callback([&, client_id](InputEvent e) {
        if (arbiter->allow(client_id, e))
            input.inject(e);
    });
}
```

### 认证与 Session 接口

```cpp
// core/include/core/auth.h

// 服务器告诉客户端：当前认证方式需要提供哪些字段
struct AuthChallenge {
    std::string scheme;                                      // "password" / "pam" / "oauth" / "ldap"
    std::unordered_map<std::string, std::string> fields;    // 字段名 → 描述，供客户端展示
};

// 客户端提交的认证数据，通用 KV 结构，core 层不关心具体字段
// 扩展新认证方式无需修改此结构
struct AuthToken {
    std::string scheme;
    std::unordered_map<std::string, std::string> data;
};

class IAuthProvider {
public:
    virtual ~IAuthProvider() = default;
    virtual AuthChallenge challenge() const = 0;            // 告诉客户端需要什么
    virtual bool authenticate(const AuthToken& token) = 0;  // 各 Adapter 自己解析 data
};

// core/include/core/session.h
enum class SessionState { Authenticating, Active, Disconnected };

// QoS 策略：由 SessionManager 根据用户身份下发，Pipeline 启动时应用
struct QosPolicy {
    int   max_bitrate_kbps;
    int   max_fps;
    int   priority;              // 高优先级用户获得更低延迟调度
    int   max_cpu_percent;       // 该 Session 最多占用的 CPU 百分比（-1 = 不限制）
    int   max_memory_mb;         // 最大内存用量（-1 = 不限制）
    int   max_concurrent_sessions; // 该用户最多同时在线 Session 数
};

// 客户端上报的能力（握手阶段协商，决定编码器和分辨率选择）
struct ClientCapabilities {
    std::vector<CodecType>   supported_codecs;    // 客户端解码器支持列表
    std::vector<PixelFormat> supported_formats;
    int                      max_width;
    int                      max_height;
    int                      max_fps;
    bool                     supports_hdr;
    bool                     supports_audio;
    std::string              client_version;       // 用于向后兼容性检查
};

// 服务器响应给客户端的协商结果
// AudioCodec 定义见 audio.h
struct NegotiatedParams {
    CodecType   codec;           // 双方均支持的最优编解码器
    int         width;
    int         height;
    int         fps;
    int         bitrate_kbps;
    bool        hdr_enabled;
    AudioCodec  audio_codec;     // 见 audio.h: enum class AudioCodec
};

struct SessionConfig {
    NegotiatedParams negotiated;  // 握手协商结果，替代原来的硬编码字段
    std::string      encoder_backend;
    std::string      transport_backend;
    QosPolicy        qos;
};

struct Session {
    std::string   id;
    SessionState  state;
    SessionConfig config;
};

class ISessionManager {
public:
    virtual ~ISessionManager() = default;
    virtual Session  create(const AuthToken& token) = 0;
    virtual void     terminate(const std::string& session_id) = 0;
    virtual Session* find(const std::string& session_id) = 0;
    // Session 状态转换：Authenticating → Active（认证成功后调用）
    virtual void     activate(const std::string& session_id) = 0;
    // 按用户身份返回 QoS 策略，由 pipeline 层应用到 Encoder 和线程调度
    virtual QosPolicy get_qos(const std::string& session_id) const = 0;
    // 返回默认 QoS（用于 RDP 等不经过 IAuthProvider 的协议）
    virtual QosPolicy default_qos() const = 0;
};

// core/include/core/negotiation.h
// #include "audio.h"   ← AudioCodec 枚举定义于此（NegotiatedParams 中使用）
// 客户端能力协商：握手阶段运行，决定编解码器、分辨率、帧率
// 结果写入 SessionConfig::negotiated，工厂据此选择编码器和参数
class ICapabilityNegotiator {
public:
    virtual ~ICapabilityNegotiator() = default;
    // 收到客户端能力后，结合服务器能力（ServerProfile）输出协商结果
    // 协商优先级：双方均支持的最高画质编解码器 > 最高分辨率 > QoS 码率上限
    virtual NegotiatedParams negotiate(
        const ClientCapabilities& client,
        const ServerProfile&      server,
        const QosPolicy&          qos) const = 0;
};
```

### 管道编排器

```cpp
// core/include/core/pipeline.h
struct PipelineConfig {
    int  queue_capacity;             // 各阶段队列容量（帧数），超出时触发背压
    bool drop_on_overflow;           // true = 丢帧；false = 阻塞等待
    int  idle_fps;                   // 画面静止时降至此帧率采集，节省 CPU 和带宽
    ThreadConfig capture_thread;
    ThreadConfig preprocess_thread;
    ThreadConfig encode_thread;
    ThreadConfig transport_thread;
    ThreadConfig audio_capture_thread;
    ThreadConfig audio_encode_thread;
    int  audio_video_offset_ms = 40; // 音视频时间戳允许的最大偏差半径（±40ms 双向对称）
    ReconnectPolicy reconnect;       // 断线重连策略
    IMetricsCollector* metrics = nullptr;  // 可选，注入后各阶段自动上报指标
};

void run_pipeline(
    ICaptureSource&          capture,
    IPreprocessor*           preprocessor,   // nullptr = 跳过预处理阶段，零开销直通
    IEncoder&                encoder,
    ITransport&              transport,
    IInputHandler&           input,
    const PipelineConfig&    cfg
);

// pipeline.cpp 内部结构：
//
//  采集线程：  next_frame() → 若返回 nullopt 且计数超阈值则切换 idle_fps
//              → Q1.push（满则丢帧或阻塞）
//
//  预处理线程：Q1.pop → preprocessor.process()
//              → 若返回 nullopt（直通模式）则直接推 Q2，跳过格式转换
//              → Q2.push
//
//  编码线程：  Q2.pop → encoder.submit_frame()（异步）
//              on_encoded_callback → Q3.push
//
//  传输线程：  Q3.pop → transport.send() / send_batch()
//
//  自适应闭环（Transport → Encoder）：
//  transport.set_event_callback([&](TransportEvent e) {
//      if (e == TransportEvent::Congested)
//          encoder.update_params({ .bitrate_kbps = current * 0.7, .gop = low_latency_gop });
//      else if (e == TransportEvent::Ready)
//          encoder.update_params({ .bitrate_kbps = session_qos.max_bitrate_kbps });
//  });
//
//  网络指标联动（Transport → FEC / 帧率自适应）：
//  transport.set_stats_callback([&](NetworkStats s) {
//      metrics->report({ ..., .transport_rtt_ms = s.rtt_ms, ... });
//      if (s.loss_rate > 0.05f)
//          transport.set_fec_params({ .enabled = true, .data_shards = 10, .parity_shards = 2 });
//      else
//          transport.set_fec_params({ .enabled = false });
//  });
//
//  强制关键帧（客户端请求或场景切换检测）：
//  encoder.submit_frame(frame, SubmitFlags::ForceKeyframe);
```

---

## 音频管道

### 设计原则

音频是独立于视频的并行管道，两者共享同一个 Transport 连接，但各自有独立的采集、编码线程和队列。音视频同步（AV Sync）通过统一的单调时钟 PTS 在 Transport 层完成。

```
视频管道：Capture → Preprocessor → VideoEncoder → ─┐
                                                     ├─▶ Transport（复用同一连接）
音频管道：AudioCapture ──────────── AudioEncoder → ─┘
              ↑                          ↑
          独立线程                    独立线程
          中优先级                    中优先级

AV Sync：视频帧 pts_us 与音频帧 pts_us 均来自同一单调时钟
          Transport 层按 pts_us 交错发送，保证音视频同步
```

### 音频数据结构

```cpp
// core/include/core/audio.h（数据结构）
// core/include/core/audio_capture.h（IAudioCapture 接口）
// core/include/core/audio_encoder.h（IAudioEncoder 接口）
// core/include/core/audio_playback.h（IAudioPlayback 接口）

enum class AudioFormat { PCM_S16LE, PCM_F32LE };
enum class AudioCodec  { AAC, OPUS, PCM };

struct AudioFrame {
    std::shared_ptr<uint8_t[]> data;
    size_t   size;
    int      sample_rate;     // 采样率（44100 / 48000）
    int      channels;        // 声道数（1 = 单声道，2 = 立体声）
    int      samples;         // 本帧样本数
    int64_t  pts_us;          // 与视频帧同源的单调时钟时间戳
    AudioFormat format;
};

struct AudioPacket {
    std::shared_ptr<uint8_t[]> data;
    size_t    size;
    int64_t   pts_us;
    AudioCodec codec;
};
```

### 音频接口

```cpp
// core/include/core/audio_capture.h
enum class AudioCaptureEvent { DeviceLost, FormatChanged };

class IAudioCapture {
public:
    virtual ~IAudioCapture() = default;
    // 无新帧时返回 nullopt（静音时某些系统不产生帧）
    virtual std::optional<AudioFrame> next_frame() = 0;
    virtual void set_event_callback(std::function<void(AudioCaptureEvent)> cb) = 0;
};

// core/include/core/audio_encoder.h
class IAudioEncoder {
public:
    virtual ~IAudioEncoder() = default;
    virtual void submit_frame(AudioFrame frame) = 0;
    virtual void set_encoded_callback(std::function<void(AudioPacket)> cb) = 0;
};
```

### 音频 Adapter 实现

| 模块 | Windows | Linux | macOS |
|------|---------|-------|-------|
| AudioCapture | WASAPI（系统音频回路采集）| PipeWire / ALSA | CoreAudio |
| AudioEncoder | AAC（Media Foundation）| OPUS（libopus）| AAC（AudioToolbox）|

```
audio/
├── capture/
│   ├── wasapi/          # Windows — 系统音频回路（含桌面应用音频）
│   ├── pipewire/        # Linux — PipeWire 音频节点
│   └── coreaudio/       # macOS — CoreAudio
└── encoder/
    ├── aac/             # AAC 编码（Media Foundation / AudioToolbox）
    └── opus/            # OPUS 编码（libopus，低延迟优先）
```

### 双向音频（麦克风 / 语音通话）

多人游戏场景需要客户端麦克风音频传回服务端播放（Party Chat、游戏内语音）。这是与单向视频串流的关键区别。

```
正向（服务端 → 客户端）：系统音频 + 游戏音效
反向（客户端 → 服务端）：麦克风音频
```

反向音频通过 Transport 的独立通道传输（`ITransport::set_mic_callback`），服务端接收后注入 `IAudioPlayback`：

```cpp
// core/include/core/audio_playback.h
class IAudioPlayback {
public:
    virtual ~IAudioPlayback() = default;
    // 将客户端麦克风音频注入服务端音频输出（虚拟音频设备）
    virtual void play(const AudioFrame& frame) = 0;
};
```

各平台音频注入实现：

| 平台 | 实现 |
|------|------|
| Windows | Virtual Audio Cable / WASAPI loopback |
| Linux | PipeWire 虚拟 sink / PulseAudio null sink |
| macOS | BlackHole / CoreAudio 虚拟设备 |

config.json 新增：

```json
"audio": {
  "mic_enabled": false,
  "mic_noise_suppress": true,
  "mic_echo_cancel": true
}
```

### 多人语音（IAudioMixer）

> `IAudioMixer`、`IChatChannel`、`IWakeOnLan` 这三个接口的实现逻辑简单、不依赖平台 API，直接在 `server/` 层内联实现，不需要独立 Adapter 目录，也不需要额外的 `add_subdirectory`。



协作会话中，服务端作为混音中心：每个客户端 mic 帧经 `set_mic_callback` 到达后交由 `IAudioMixer` 处理，混音结果通过 `ITransport::send_audio` 回传给**其他**客户端（排除自身，避免回声）。

```cpp
// core/include/core/audio_mixer.h

struct MixerClientConfig {
    std::string client_id;
    float       volume = 1.0f;   // 该客户端在混音中的音量权重（0.0 ~ 2.0）
    bool        muted  = false;
};

class IAudioMixer {
public:
    virtual ~IAudioMixer() = default;

    // 注册/注销客户端
    virtual void add_client(const MixerClientConfig& cfg) = 0;
    virtual void remove_client(const std::string& client_id) = 0;

    // 推入某客户端的 mic 帧
    virtual void push(const std::string& client_id, const AudioFrame& frame) = 0;

    // 获取某客户端应收到的混音帧（已排除自身，应用了各客户端 volume 权重）
    // 由 pipeline 定期调用，结果通过 ITransport::send_audio 发送
    virtual std::optional<AudioFrame> mix_for(const std::string& client_id) = 0;

    // 运行时调整音量 / 静音（无需重建 Mixer）
    virtual void set_volume(const std::string& client_id, float volume) = 0;
    virtual void set_muted(const std::string& client_id, bool muted) = 0;
};
```

Pipeline 集成（server 层连线，示意）：

```cpp
// 每个客户端连接时注册到 Mixer
mixer->add_client({ client_id, 1.0f, false });
transport->set_mic_callback([&, client_id](AudioFrame f) {
    mixer->push(client_id, f);
});

// 独立混音线程，按音频帧率（如 50fps = 20ms/帧）定时轮询
// 每帧调用一次，对每个在线客户端获取其专属混音结果并发送
while (running) {
    for (auto& [cid, transport] : client_transports) {
        if (auto frame = mixer->mix_for(cid))
            transport->send_audio(*frame);
    }
    sleep_until_next_audio_frame();  // 对齐 audio_encoder 的帧率
}
```

Push-to-Talk / VAD 策略：

| 模式 | 行为 |
|------|------|
| `"always"` | 持续传输，适合低噪音环境 |
| `"ptt"` | 客户端按下按键才推送 mic 帧，服务端不做 VAD |
| `"vad"` | 服务端在 `push()` 内做能量检测，低于阈值的帧直接丢弃 |

config.json：

```json
"voice_chat": {
  "enabled": false,
  "mode": "vad",
  "vad_threshold": 0.02,
  "per_client_volume": true
}
```

### 文字聊天（IChatChannel）

协作会话的低带宽文本通信通道。各 Transport 实现通过各自的控制子通道传输文本帧（WebRTC DataChannel、QUIC stream、RTP 独立 TCP 通道），`IChatChannel` 作为 server 层的消息路由中心，持有所有在线客户端的 Transport 引用，收到消息后调用 `broadcast()` 遍历转发。

```cpp
// core/include/core/chat.h

struct ChatMessage {
    std::string client_id;
    std::string text;
    int64_t     timestamp_us;
};

class IChatChannel {
public:
    virtual ~IChatChannel() = default;

    // 广播一条消息给所有客户端（exclude_sender=true 时排除发送者自身）
    // server 层实现：遍历 client_transports，调用各 transport 的控制通道发送
    virtual void broadcast(const ChatMessage& msg, bool exclude_sender = true) = 0;

    // 某客户端通过 Transport 发来聊天消息时触发
    // 由各 Transport 在收到控制帧后调用此回调
    virtual void set_message_callback(
        std::function<void(const ChatMessage&)> cb) = 0;
};
```

### Wake-on-LAN（IWakeOnLan）

游戏玩家最实用的功能：客户端远程唤醒服务器，无需服务器常开。服务端监听独立的轻量端口，收到 Magic Packet 后广播到本地网段。

```cpp
// core/include/core/wake_on_lan.h

class IWakeOnLan {
public:
    virtual ~IWakeOnLan() = default;

    // 开始监听 Magic Packet（UDP，默认端口 9）
    virtual void listen(int port = 9) = 0;
    virtual void stop() = 0;

    // 主动向目标 MAC 发送 Magic Packet（用于级联唤醒场景）
    virtual void send(const std::string& mac_address,
                      const std::string& broadcast_addr = "255.255.255.255") = 0;
};
```

config.json：

```json
"wake_on_lan": {
  "enabled": false,
  "listen_port": 9,
  "broadcast_address": "255.255.255.255"
}
```

---

### 音视频同步机制

```
采集端：视频帧和音频帧各自在采集时打上 pts_us（同源单调时钟）

Transport 层发送逻辑：
  维护两个队列：video_q（EncodedPacket）和 audio_q（AudioPacket）
  按 pts_us 从小到大交错取包发送
  允许 ±audio_video_offset_ms（默认 ±40ms）的偏差：
    音频 pts 落后视频超过 40ms：丢弃该音频帧（补静音）
    音频 pts 超前视频超过 40ms：插入静音帧，等待视频追上
```

`ITransport::send_audio` 完整定义见 ITransport 章节。

`PipelineConfig` 完整定义见管道编排器章节（含音频线程配置和重连策略字段）。

---

## 服务器探测与自动链路选择（Profiler）

### 为什么需要探测

`AdapterCapabilities` 只能回答"这个 Adapter 支不支持某功能"，但不能回答"在当前这台服务器上，哪个组合实际性能最好"。

典型的反直觉场景：

| 场景 | 直觉 | 实际 |
|------|------|------|
| 有 NVENC，网络带宽受限 (<5Mbps) | 用 NVENC | x264 在低码率下画质更好，因为 NVENC 的率失真控制在低码率下不如 x264 |
| 有 NVENC，但 GPU 同时在跑游戏 | 用 NVENC | GPU 负载高时 NVENC 编码延迟抖动大，CPU 编码延迟反而更稳定 |
| 高 RTT 网络（>100ms） | 低延迟 GOP | 高 RTT 下 FEC 比重传更关键，GOP 调整优先级下降 |
| CPU 核数多（16+ 核） | 用硬件编码 | 多核 CPU 跑 x264 veryfast 吞吐量不输 NVENC，且画质更好 |

---

### 探测接口设计

```cpp
// core/include/core/profiler.h

// 单个编码器的实测基准结果
struct EncoderBenchmark {
    std::string backend;          // "nvenc" / "x264" / "vaapi" 等
    float       encode_fps;       // 实测编码帧率（编 30 帧取平均）
    float       latency_ms;       // 平均编码延迟
    float       latency_jitter_ms;// 延迟抖动（方差），抖动大 = 不稳定
    float       bitrate_efficiency;// 同画质下实际码率 / 目标码率，越接近 1 越好
    bool        available;        // 是否可用（驱动缺失则 false）
};

// 硬件资源快照
struct HardwareProfile {
    int     cpu_cores;
    float   cpu_freq_ghz;
    int     gpu_vram_mb;          // 0 = 无独立 GPU
    float   gpu_utilization;      // 当前 GPU 负载（0.0~1.0）
    int     available_memory_mb;
};

// 网络探测结果（探测到客户端，或探测到公网）
struct NetworkProfile {
    float   bandwidth_kbps;       // 估算可用带宽
    float   rtt_ms;               // 往返延迟
    float   loss_rate;            // 丢包率
};

// 完整服务器探测结果，工厂用此做决策
struct ServerProfile {
    HardwareProfile               hardware;
    std::vector<EncoderBenchmark> encoders;  // 所有可用编码器的实测结果
    NetworkProfile                network;   // 可选，客户端连接后填充
};

class IProfiler {
public:
    virtual ~IProfiler() = default;
    // 启动时运行一次，耗时约 1~3 秒（对每个编码器编 30 帧）
    virtual ServerProfile run() = 0;
};
```

---

### 探测流程

```
服务器启动
    │
    ▼
Profiler::run()
    ├── 探测硬件：CPU 核数 / GPU 型号 / 可用内存 / 当前 GPU 负载
    ├── 探测编码器（逐个）：
    │     对每个可用 backend（nvenc/vaapi/x264 等）：
    │       1. 初始化编码器
    │       2. 用内置测试帧生成器生成 30 帧（无需任何采集 Adapter）
    │       3. 测量：编码延迟、延迟抖动、实际码率 vs 目标码率
    │       4. 记录 EncoderBenchmark
    └── 输出 ServerProfile（缓存，客户端连接时复用）

客户端连接
    │
    ▼
NetworkProfile 探测（可选）：
    ├── 发送探测包，估算 RTT 和带宽
    └── 填充 ServerProfile::network

工厂根据完整 ServerProfile 选择最优链路
```

---

### 工厂决策逻辑

决策采用**能力（Capability）+ 探测得分（Benchmark Score）双维度**：
- **能力维度**：驱动是否存在、格式是否兼容、是否支持目标分辨率/帧率
- **探测得分**：实测延迟、抖动、码率效率的加权综合分

两者缺一不可：能力维度排除不可用选项，探测得分在可用选项中选最优。

```cpp
// server/src/factory.cpp

// 能力 + 探测得分综合评分
float score_encoder(const EncoderBenchmark& bench,
                    const NetworkProfile& net,
                    const HardwareProfile& hw)
{
    if (!bench.available) return -1.0f;

    float score = 0.0f;
    // 稳定性（抖动越小越好）：权重 40%
    score += (1.0f - std::min(bench.latency_jitter_ms / 10.0f, 1.0f)) * 40.0f;
    // 码率效率（实际码率/目标码率接近 1 最好）：权重 30%
    score += (1.0f - std::abs(1.0f - bench.bitrate_efficiency)) * 30.0f;
    // 吞吐量（fps 越高越好，归一化到 60fps）：权重 20%
    score += std::min(bench.encode_fps / 60.0f, 1.0f) * 20.0f;
    // 网络条件修正：带宽受限时惩罚码率效率差的编码器
    if (net.bandwidth_kbps > 0 && net.bandwidth_kbps < 8000)
        score -= (1.0f - bench.bitrate_efficiency) * 20.0f;
    // GPU 高负载时惩罚硬件编码器（抖动会变大）
    if (hw.gpu_utilization > 0.8f && bench.backend != "x264")
        score -= 30.0f;

    return score;
}

std::unique_ptr<IEncoder> select_encoder(
    const ServerProfile& profile,
    const nlohmann::json& cfg)
{
    auto hint = cfg["encoder"]["backend"].get<std::string>();
    if (hint != "auto") return build_encoder_by_name(hint, cfg);

    // 对所有可用编码器打分，选得分最高的
    std::string best_backend = "x264";  // 兜底
    float       best_score   = -1.0f;
    for (const auto& bench : profile.encoders) {
        float s = score_encoder(bench, profile.network, profile.hardware);
        if (s > best_score) { best_score = s; best_backend = bench.backend; }
    }
    return build_encoder_by_name(best_backend, cfg);
}
```

---

### 探测结果与配置的关系

探测结果影响三个层面的决策：

```
1. 编码器选择（最重要）：NVENC vs x264 vs VAAPI
2. GOP 参数：高 RTT 时自动调整为更大 gop_size + intra_refresh
3. FEC 参数：高丢包率时自动开启 FEC，低丢包率时关闭

config.json 中 "auto" 触发探测，显式指定时直接使用：
  "encoder": { "backend": "auto" }   → 探测后选择
  "encoder": { "backend": "nvenc" }  → 强制使用 NVENC，跳过探测
```

探测结果通过 `config.json` 的 `profiler` 节点配置：

```json
"profiler": {
  "enabled": true,
  "benchmark_frames": 30,
  "cache_result": true,
  "cache_ttl_seconds": 3600
}
```

`cache_result` 为 `true` 时将探测结果缓存到磁盘，下次启动时若硬件未变化则直接复用，跳过 1~3 秒的编码器基准测试。

---

## 安全性

### TLS 证书管理

所有控制通道和数据通道均需 TLS 加密。服务器启动时自动生成自签名证书，或加载用户提供的证书：

```
首次启动：
  → 生成 RSA-2048 自签名证书，保存到 certs/ 目录
  → 证书指纹显示在日志中，供客户端首次配对时核验

后续启动：
  → 加载已有证书
  → 检查有效期，距过期 30 天内自动续签（自签名）

用户自定义：
  → config.json 中指定 cert_path / key_path，使用外部证书（如 Let's Encrypt）
```

```json
"security": {
  "cert_path": "",
  "key_path": "",
  "auto_generate": true,
  "tls_min_version": "1.2"
}
```

### 客户端授权

串流协议（RTP/WebRTC/QUIC）的认证走 `IAuthProvider` 接口，支持以下 Adapter（config.json 中 `auth.method` 切换）：

| 方式 | 适用场景 | 实现 |
|------|---------|------|
| `password` | 简单家用场景 | 本地 users.json 文件 |
| `oauth` | 个人/企业，用现有账号登录 | OAuth 2.0 授权码流程 |
| `passkey` | 最佳用户体验，无密码 | FIDO2/WebAuthn |
| `ldap` | 企业 AD/LDAP 环境 | 已有实现 |

#### OAuth 认证流程

```
客户端发起连接
    → 服务器返回 AuthChallenge { scheme="oauth", fields={"auth_url":"..."} }
    → 客户端打开浏览器访问 auth_url（Google/GitHub/企业 IdP）
    → 用户登录授权，IdP 回调返回 access_token
    → 客户端提交 AuthToken { scheme="oauth", data={"token":"..."} }
    → OAuthAuth Adapter 向 IdP 验证 token，返回用户身份
    → 认证通过，建立 Session
```

```cpp
// auth/oauth/oauth_auth.cpp
AuthChallenge challenge() const override {
    return { "oauth", {
        {"auth_url",  build_auth_url()},   // 跳转到 IdP 的授权页
        {"client_id", cfg_["oauth"]["client_id"]}
    }};
}
bool authenticate(const AuthToken& token) override {
    // 用 token.data["token"] 向 IdP 的 userinfo 端点验证
    return verify_with_idp(token.data.at("token"));
}
```

config.json：

```json
"auth": {
  "method": "oauth",
  "oauth": {
    "provider": "google",
    "client_id": "xxx.apps.googleusercontent.com",
    "client_secret": "xxx",
    "allowed_emails": ["alice@gmail.com", "bob@company.com"]
  }
}
```

#### Passkey 认证流程（FIDO2/WebAuthn）

Passkey 是目前体验最好的无密码方案：设备生物识别（指纹/Face ID）直接完成认证，无需记忆密码。

```
注册（首次，一次性）：
    → 客户端生成 FIDO2 密钥对，私钥存于设备安全芯片/TPM
    → 服务器存储公钥（paired_clients.json）

登录（每次）：
    → 服务器返回 challenge 随机数
    → 客户端用设备生物识别签名 challenge（指纹/Face ID/PIN）
    → 服务器验证签名 → 通过
    → 全程无密码传输，抗钓鱼
```

```cpp
// auth/passkey/passkey_auth.cpp
AuthChallenge challenge() const override {
    auto nonce = generate_random_nonce();   // 每次生成新随机数，防重放
    return { "passkey", {
        {"challenge", nonce},
        {"rp_id",     cfg_["passkey"]["rp_id"]}  // Relying Party ID
    }};
}
bool authenticate(const AuthToken& token) override {
    // 验证客户端对 challenge 的 FIDO2 签名
    return verify_fido2_assertion(
        token.data.at("credential_id"),
        token.data.at("authenticator_data"),
        token.data.at("signature"),
        stored_challenge_
    );
}
```

config.json：

```json
"auth": {
  "method": "passkey",
  "passkey": {
    "rp_id": "stream.myserver.local",
    "rp_name": "My Stream Server"
  }
}
```

**与 RDP 的关系**：RDP 有自己的 NLA/CredSSP 认证，不经过 `IAuthProvider`，无需配置上述任何方式。

### NAT 穿透

局域网内直连，公网访问通过以下机制支持：

```
优先级（工厂按顺序尝试）：
  1. 局域网直连（同网段，延迟最低）
  2. UPnP 自动端口映射（路由器支持时自动配置，用户无感知）
  3. STUN（获取公网地址，P2P 打洞）
  4. TURN Relay（全部失败时的兜底，由用户自行部署）
```

```json
"nat": {
  "upnp_enabled": true,
  "stun_server": "stun.l.google.com:19302",
  "turn_server": "",
  "turn_username": "",
  "turn_password": "",
  "ipv6_enabled": true
}
```

NAT 穿透状态通过 `IMetricsCollector` 上报，可在日志中查看当前使用的连接路径。

### 其他安全要点

- **传输加密**：视频/音频数据通过 SRTP（WebRTC）或 DTLS（QUIC）加密，RDP 通过 TLS 加密
- **输入通道加密**：控制通道（键鼠输入）同样经过 TLS，防止输入劫持
- **IPv6 支持**：监听地址同时绑定 IPv4 和 IPv6（`::` 地址），`nat.ipv6_enabled` 控制
- **剪贴板安全**：剪贴板同步默认关闭，需用户显式启用；大内容（>1MB）截断处理

---

## 工厂层能力协商

工厂在组装时查询各 Adapter 的能力，自动选择端到端最优链路：

```
示例（Linux）：
  capture.capabilities().supports_dmabuf == true
  encoder.capabilities().supports_dmabuf == true
  → 工厂选择：PipeWireCapture → DMABUFImporter → VAAPIEncoder（全程零拷贝）

示例（Windows HDR）：
  capture.capabilities().supports_hdr == true
  encoder.capabilities().supports_hdr == true
  → 工厂选择：DXGICapture → GPUPreprocessor(BGRA→NV12, HDR 透传) → NVENCEncoder（HDR 输出）

示例（Windows SDR）：
  capture 输出 BGRA，encoder 要求 NV12
  → 工厂选择：DXGICapture → GPUPreprocessor(BGRA→NV12) → NVENCEncoder

兜底链路：
  → DXGICapture → CPUPreprocessor(BGRA→NV12) → X264Encoder（软件编码）

直通链路（格式已匹配，**Preprocessor 实例完全不创建**）：
  PipeWireCapture 输出 NV12，VAAPIEncoder 接受 NV12，dmabuf 路径
  → PipeWireCapture → [无 Preprocessor] → VAAPIEncoder（全程零开销）
```

Preprocessor 的零开销直通规则：工厂在以下情况**不创建任何 Preprocessor 实例**，`run_pipeline` 接收 `nullptr` 时直接跳过该阶段：

```cpp
// nullptr = 完全跳过 Preprocessor 阶段，无虚函数调用开销
std::unique_ptr<IPreprocessor> select_preprocessor(
    const AdapterCapabilities& cap_caps,
    const AdapterCapabilities& enc_caps,
    const std::string& hint)
{
    if (hint != "auto") {
        if (hint == "passthrough") return nullptr;               // 强制跳过
        if (hint == "dmabuf")      return std::make_unique<DMABUFImporter>();
        if (hint == "gpu")         return std::make_unique<GPUPreprocessor>();
        if (hint == "cpu")         return std::make_unique<CPUPreprocessor>();
    }

    // dmabuf 零拷贝路径：capture 和 encoder 都支持 dmabuf
    // → 不需要任何预处理，直接传递 fd，Preprocessor = nullptr
    if (cap_caps.supports_dmabuf && enc_caps.supports_dmabuf)
        return nullptr;

    // 格式已匹配（如 capture 输出 NV12，encoder 接受 NV12）→ 跳过
    bool formats_match = !cap_caps.output_formats.empty() &&
        std::find(enc_caps.input_formats.begin(), enc_caps.input_formats.end(),
                  cap_caps.output_formats[0]) != enc_caps.input_formats.end();
    if (formats_match)
        return nullptr;

    // 需要格式转换
    if (cap_caps.supports_gpu_preprocessing)
        return std::make_unique<GPUPreprocessor>();
    return std::make_unique<CPUPreprocessor>();
}
// pipeline.cpp 中 preprocessor 为 nullptr 时：
//   Q1 直接连接 Q2，Preprocessor 线程不启动，零额外开销
```

---

## 断线重连与网络弹性

### 问题分类

串流场景的网络异常分三种，处理策略不同：

| 类型 | 典型触发 | 目标 |
|------|---------|------|
| 短暂抖动（<2s） | Wi-Fi 信号波动、路由器瞬间丢包 | 客户端无感知，靠 FEC + 缓冲吸收 |
| 中断重连（2s~30s）| 网络切换（Wi-Fi 切 LAN）、短暂断电 | 客户端自动重连，管道保留状态快速恢复 |
| 长时间断开（>30s）| 休眠、网络故障 | 管道销毁，重连后重新握手建立 |

---

### 网络抖动弹性（短暂抖动）

这一层完全在 Transport 内部处理，上层管道无感知：

```
抖动防护三层：
  1. FEC（前向纠错）：丢包率 2%~10% 时开启，无需重传即可恢复
  2. 自适应 jitter buffer：缓冲 20~150ms，吸收延迟抖动
  3. NACK（选择性重传）：仅重传关键帧，非关键帧丢失直接等下一帧

触发强制关键帧：
  连续丢包超过阈值 → Transport 通过 set_event_callback(Congested) 通知
  → Pipeline 调用 encoder.submit_frame(frame, ForceKeyframe)
  → 客户端收到关键帧后画面立即恢复
```

`NetworkStats` 结构体（含 `nack_requests`、`fec_recovered` 字段）定义见 ITransport 章节。

---

### 断线重连（中断重连）

重连的核心问题是：**管道是否需要销毁重建，还是可以保留状态**。

```
状态保留策略（2s~30s 断线）：
  Transport 断开时不立即销毁管道
  → 管道进入 Suspended 状态：
      采集线程暂停（停止向 Q1 推帧）
      编码线程清空队列
      Transport 进入等待重连模式（监听同端口的新连接）
  → 客户端重连后：
      Transport 恢复，发送强制关键帧
      采集线程恢复推帧
      整个过程 < 500ms

状态销毁策略（>30s 断线）：
  管道完全销毁，重连后走完整握手流程
```

`ReconnectPolicy` 接口：

```cpp
// core/include/core/reconnect.h

struct ReconnectPolicy {
    int  suspend_timeout_ms   = 30000;  // 超过此时间销毁管道，默认 30s
    int  reconnect_interval_ms = 1000;  // 重连轮询间隔
    int  max_retries           = -1;    // -1 = 无限重试
    bool force_keyframe_on_resume = true; // 恢复后立即发送关键帧
};

// Pipeline 状态机
enum class PipelineState {
    Running,      // 正常运行
    Suspended,    // Transport 断开，等待重连，管道暂停
    Recovering,   // Transport 重连成功，正在恢复（发送关键帧）
    Stopped       // 已销毁
};
```

`PipelineConfig` 完整定义见管道编排器章节（含 `reconnect` 字段）。

Pipeline 状态机在 `pipeline.cpp` 内部实现：

```
Transport::set_event_callback 收到 Disconnected：
    pipeline_state = Suspended
    采集线程：检测到 Suspended 状态后暂停 next_frame() 调用
    启动重连计时器

Transport 重连成功（新连接到达同端口）：
    pipeline_state = Recovering
    encoder.submit_frame(current_frame, ForceKeyframe)  // 立即恢复画面
    pipeline_state = Running
    采集线程恢复

重连计时超过 suspend_timeout_ms：
    pipeline_state = Stopped
    通知 server 层销毁本 Session
```

---

### 管道错误恢复（设备丢失 / 编码器崩溃）

除网络断线外，还需处理本地硬件故障：

```cpp
// pipeline.cpp 内部错误处理

// 1. Capture 设备丢失（如显示器拔出）
capture.set_event_callback([&](CaptureEvent e) {
    if (e == CaptureEvent::DeviceLost) {
        // 尝试重新初始化 Capture（等待 1s 后重试，最多 3 次）
        // 若为物理显示器丢失且配置了 headless_fallback，切换到虚拟显示
        // 3 次失败后 → 发送 ForceKeyframe + 终止 Session
    }
    if (e == CaptureEvent::ResolutionChanged) {
        // 分辨率变化：重建 Preprocessor（重新协商格式）+ 发送 ForceKeyframe
    }
});

// 2. 编码器崩溃（NVENC 驱动异常）
// IEncoder::submit_frame 抛出异常时：
//   尝试重新初始化编码器（原参数）
//   失败则降级到 x264 软件编码（保证服务不中断）
//   通过 Metrics 上报编码器切换事件

// 3. AudioCapture 设备丢失
// 插入静音帧（全零 PCM），保持音视频时间线连续
// 设备恢复后无缝切回真实音频
```

---

## 配置文件（config.json）

客户端通过 IP 地址直连服务器，格式为 `IP:port`（如 `192.168.1.100:47984`），不需要任何中间发现服务。

注意：`transport.port` 是 Transport 出站发送时绑定的本地端口，与 `protocols.*port`（服务器监听的入站端口）是不同概念。

```json
{
  "protocols": {
    "rtp":    { "enabled": true, "port": 47984 },
    "webrtc": { "enabled": true, "port": 47985 },
    "quic":   { "enabled": true, "port": 47986 },
    "rdp":    { "enabled": true, "port": 3389  },
    "vnc":    { "enabled": true, "port": 5900  }
  },
  "auth": {
    "method": "password",
    "password_file": "users.json"
  },
  "capture": {
    "backend": "dxgi"
  },
  "preprocessor": {
    "backend": "auto"
  },
  "encoder": {
    "backend": "nvenc",
    "codec": "h265",
    "bitrate_kbps": 20000,
    "fps": 60,
    "gop": {
      "max_b_frames": 0,
      "gop_size": 60,
      "low_latency_preset": true,
      "intra_refresh": false,
      "target_fps": 60
    }
  },
  "transport": {
    "backend": "rtp",
    "port": 47998,
    "pacing_enabled": true,
    "jitter_buffer_min_ms": 20,
    "jitter_buffer_max_ms": 150,
    "fec": {
      "enabled": false,
      "data_shards": 10,
      "parity_shards": 2
    }
  },
  "input": {
    "backend": "win32"
  },
  "pipeline": {
    "queue_capacity": 4,
    "drop_on_overflow": true,
    "idle_fps": 10
  },
  "threading": {
    "capture_priority": 10,
    "capture_cpu_affinity": 0,
    "encode_priority": 10,
    "encode_cpu_affinity": 2,
    "transport_priority": 5,
    "transport_cpu_affinity": -1
  },
  "qos": {
    "default_max_bitrate_kbps": 20000,
    "default_max_fps": 60,
    "default_max_cpu_percent": -1,
    "default_max_memory_mb": -1,
    "default_max_concurrent_sessions": 2,
    "per_user": {
      "alice": { "max_bitrate_kbps": 30000, "max_fps": 120, "priority": 10 },
      "bob":   { "max_bitrate_kbps": 10000, "max_fps": 30,  "priority": 5  }
    }
  },
  "metrics": {
    "enabled": true,
    "report_interval_ms": 1000,
    "output": "stdout"
  },
  "profiler": {
    "enabled": true,
    "benchmark_frames": 30,
    "cache_result": true,
    "cache_ttl_seconds": 3600
  },
  "security": {
    "cert_path": "",
    "key_path": "",
    "auto_generate": true,
    "tls_min_version": "1.2"
  },
  "nat": {
    "upnp_enabled": true,
    "stun_server": "stun.l.google.com:19302",
    "turn_server": "",
    "ipv6_enabled": true
  },
  "clipboard": {
    "enabled": false,
    "max_size_kb": 1024
  },
  "display": {
    "index": 0,
    "allow_client_switch": true
  },
  "audio": {
    "capture_backend": "auto",
    "encoder": "opus",
    "sample_rate": 48000,
    "channels": 2,
    "bitrate_kbps": 128,
    "mic_enabled": false,
    "mic_backend": "auto",
    "mic_noise_suppress": true,
    "mic_echo_cancel": true
  },
  "recording": {
    "enabled": false,
    "output_dir": "recordings/",
    "format": "mp4",
    "max_duration_minutes": 120
  },
  "streaming": {
    "rtmp": {
      "enabled": false,
      "url": "rtmp://live.twitch.tv/app/<stream_key>"
    },
    "srt": {
      "enabled": false,
      "url": "srt://ingest.example.com:9000"
    }
  },
  "shared_session": {
    "enabled": false,
    "max_clients": 4
  },
  "apps": [
    { "id": "cs2",   "name": "CS2",   "executable": "/usr/bin/cs2" },
    { "id": "steam", "name": "Steam", "executable": "/usr/bin/steam" }
  ],
  "logging": {
    "level": "info",
    "output": "stdout",
    "file": "logs/stream-server.log",
    "max_size_mb": 50,
    "max_files": 5
  },
  "shutdown": { "drain_timeout_ms": 500 }
}
```

`preprocessor.backend` 为 `"auto"` 时由工厂根据能力协商自动选择，也可手动指定 `"gpu"` / `"dmabuf"` / `"cpu"`。

---

## 工厂函数与启动流程

以下为示意性伪代码，说明工厂层的职责划分和启动流程，不是完整可编译实现。

```cpp
// server/src/factory.cpp（示意）

std::unique_ptr<IEncoder> build_encoder(const nlohmann::json& cfg) {
    auto backend = cfg["encoder"]["backend"].get<std::string>();
    if (backend == "nvenc")        return std::make_unique<NvencEncoder>(cfg);
    if (backend == "vaapi")        return std::make_unique<VaapiEncoder>(cfg);
    if (backend == "videotoolbox") return std::make_unique<VideoToolboxEncoder>(cfg);
    if (backend == "x264")         return std::make_unique<X264Encoder>(cfg);
    throw std::runtime_error("未知编码器：" + backend);
}

std::unique_ptr<ITransport> build_transport(
    const nlohmann::json& cfg, ProtocolHint protocol)
{
    switch (protocol) {
        case ProtocolHint::RTP:    return std::make_unique<RtpTransport>(cfg);
        case ProtocolHint::WebRTC: return std::make_unique<WebRtcTransport>(cfg);
        case ProtocolHint::QUIC:   return std::make_unique<QuicTransport>(cfg);
        case ProtocolHint::RDP:    return std::make_unique<RdpTransport>(cfg);
        case ProtocolHint::VNC:    return std::make_unique<VncTransport>(cfg);
        default: throw std::runtime_error("未知传输协议");
    }
}

std::unique_ptr<IAuthProvider> build_auth(const nlohmann::json& cfg) {
    auto method = cfg["auth"]["method"].get<std::string>();
    if (method == "password") return std::make_unique<PasswordAuth>(cfg);
    if (method == "oauth")    return std::make_unique<OAuthAuth>(cfg);
    if (method == "passkey")  return std::make_unique<PasskeyAuth>(cfg);
    if (method == "ldap")     return std::make_unique<LdapAuth>(cfg);
    throw std::runtime_error("未知认证方式：" + method);
}

// select_preprocessor 完整实现见上方"工厂层能力协商"章节
// select_encoder 完整实现见上方"服务器探测与自动链路选择"章节
```

```cpp
// server/src/main.cpp（示意）
int main() {
    auto cfg = nlohmann::json::parse(std::ifstream("config.json"));

    // 1. 探测服务器硬件能力（encoder.backend="auto" 时用于自动选链路）
    auto profiler = std::make_unique<ProfilerImpl>(cfg);
    auto profile  = profiler->run();     // 约 1~3 秒，有缓存时立即返回

    // 2. 初始化认证、会话管理、指标收集
    auto auth        = build_auth(cfg);
    auto session_mgr = std::make_unique<SessionManagerImpl>(std::move(auth), cfg);
    auto metrics     = std::make_unique<MetricsCollectorImpl>(cfg);

    // 3. 启动多协议端口监听（每个端口对应一种协议，按 config.json protocols 节配置）
    MultiPortListener listener(cfg);
    listener.on_connect([&](Connection& conn, ProtocolHint protocol) {

        // 串流协议走 IAuthProvider 认证；RDP 协议由 RDPTransport 内部 NLA 处理
        std::string session_id = authenticate(conn, protocol, *session_mgr);
        if (session_id.empty() && protocol != ProtocolHint::RDP) return;

        // 组装最优链路
        auto capture      = build_capture(cfg);
        auto encoder      = select_encoder(profile, cfg);
        auto preprocessor = select_preprocessor(
            capture->capabilities(), encoder->capabilities(),
            cfg["preprocessor"]["backend"].get<std::string>());
        auto transport    = build_transport(cfg, protocol);
        auto input        = build_input(cfg);

        // 应用 QoS 策略并启动流水线
        auto qos = session_mgr->get_qos(session_id);
        encoder->update_params(build_encoder_params(cfg, qos));
        PipelineConfig pcfg = build_pipeline_config(cfg, qos);
        pcfg.metrics = metrics.get();
        run_pipeline(*capture, preprocessor.get(), *encoder, *transport, *input, pcfg);
    });

    listener.run();
}
```

---

## CMake 结构

```cmake
# 根 CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(Pulsar)

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(core)

# 认证 Adapter（全平台相同逻辑）
add_subdirectory(auth/all/password)
add_subdirectory(auth/all/oauth)
add_subdirectory(auth/all/passkey)
add_subdirectory(auth/all/ldap)

if (UNIX AND NOT APPLE)    # ── Linux ──────────────────────────────────────
    # 视频采集
    add_subdirectory(video/capture/linux/pipewire)
    add_subdirectory(video/capture/linux/drm_virtual)   # Headless
    # 视频预处理
    add_subdirectory(video/preprocessor/linux/gpu)
    add_subdirectory(video/preprocessor/linux/dmabuf)
    # 视频编码（硬件）
    add_subdirectory(video/encoder/nvenc)               # 顶层分发 → linux/
    add_subdirectory(video/encoder/vaapi/linux)
    # 传输
    add_subdirectory(transport/webrtc/linux)
    add_subdirectory(transport/quic/linux)
    # 音频
    add_subdirectory(audio/capture/linux/pipewire)
    # 输入
    add_subdirectory(input/linux/uinput)

elseif (WIN32)             # ── Windows ─────────────────────────────────────
    add_subdirectory(video/capture/windows/dxgi)
    add_subdirectory(video/capture/windows/virtual_display)   # Headless
    add_subdirectory(video/preprocessor/windows/gpu)
    add_subdirectory(video/encoder/nvenc)               # 顶层分发 → windows/
    add_subdirectory(video/encoder/amf/windows)
    add_subdirectory(transport/webrtc/windows)
    add_subdirectory(transport/quic/windows)
    add_subdirectory(audio/capture/windows/wasapi)
    add_subdirectory(audio/encoder/aac/windows)
    add_subdirectory(input/windows/win32)

elseif (APPLE)             # ── macOS ────────────────────────────────────────
    add_subdirectory(video/capture/macos/sckit)
    add_subdirectory(video/capture/macos/cgvirtual)           # Headless
    add_subdirectory(video/preprocessor/macos/gpu)
    add_subdirectory(video/encoder/videotoolbox/macos)
    add_subdirectory(audio/capture/macos/coreaudio)
    add_subdirectory(audio/encoder/aac/macos)
    add_subdirectory(input/macos/iokit)
endif()

# 全平台通用模块
add_subdirectory(video/preprocessor/all/cpu)
add_subdirectory(video/preprocessor/all/overlay)        # 可选，OSD/水印
add_subdirectory(video/encoder/x264/all)                # 软件编码兜底
add_subdirectory(video/encoder/rdp/all)
add_subdirectory(video/encoder/vnc/all)
add_subdirectory(audio/encoder/opus/all)
add_subdirectory(transport/rtp/all)
add_subdirectory(transport/rdp/all)
add_subdirectory(transport/vnc/all)
add_subdirectory(transport/rtmp/all)                    # 第三方推流（预留）
add_subdirectory(transport/srt/all)                     # 低延迟推流（预留）

add_subdirectory(server)
```

```cmake
# core/CMakeLists.txt — 零第三方依赖
add_library(pulsar_core STATIC src/pipeline.cpp)
# 直接包含，无冗余的 core/ 子层
target_include_directories(pulsar_core PUBLIC include)
```

```cmake
# video/encoder/nvenc/CMakeLists.txt — 平台分发
if (UNIX AND NOT APPLE)
    add_subdirectory(linux)
elseif (WIN32)
    # add_subdirectory(windows)  # 预留
endif()

# video/encoder/nvenc/linux/CMakeLists.txt
add_library(encoder_nvenc STATIC src/nvenc_encoder.cpp)
target_include_directories(encoder_nvenc
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/../include   # 共享公共接口
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include      # 私有 nvEncodeAPI.h
)
target_link_libraries(encoder_nvenc PUBLIC pulsar_core PRIVATE ${CMAKE_DL_LIBS})
```

```cmake
# video/encoder/x264/all/CMakeLists.txt
add_library(encoder_x264 STATIC src/x264_encoder.cpp)
target_include_directories(encoder_x264
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if (UNIX AND NOT APPLE)
    set(_X264_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/linux_x86_64/libx264.a)
elseif (WIN32)
    set(_X264_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/windows_x64/libx264.lib)
elseif (APPLE)
    set(_X264_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/macos_universal/libx264.a)
endif()
target_link_libraries(encoder_x264 PUBLIC pulsar_core PRIVATE ${_X264_LIB})
```

```cmake
# server/CMakeLists.txt（Linux 示意）
add_executable(pulsar_server src/main.cpp src/factory.cpp src/config_parser.cpp src/profiler.cpp)
target_include_directories(pulsar_server PRIVATE include)
target_link_libraries(pulsar_server
    PRIVATE pulsar_core
    # 视频采集（Linux）
    PRIVATE capture_pipewire capture_drm_virtual
    # 视频预处理
    PRIVATE preprocessor_gpu preprocessor_dmabuf preprocessor_cpu
    # 视频编码
    PRIVATE encoder_nvenc encoder_vaapi encoder_x264 encoder_rdp encoder_vnc
    # 传输
    PRIVATE transport_rtp transport_webrtc transport_quic transport_rdp transport_vnc
    # 输入
    PRIVATE input_uinput
    # 音频
    PRIVATE audio_capture_pipewire audio_encoder_opus
    # 认证
    PRIVATE auth_password
)
```

---

## 基础设施接口

以下接口由 `core/` 层定义，`server/` 层在启动时创建并注入各模块，不走工厂能力协商流程。

### 统一日志（ILogger）

```cpp
// core/include/core/logger.h

enum class LogLevel { Debug, Info, Warn, Error };

class ILogger {
public:
    virtual ~ILogger() = default;
    virtual void log(LogLevel level,
                     const std::string& module,  // 来源模块，如 "capture"/"encoder"
                     const std::string& msg) = 0;
};
// 默认实现：开发环境用 stdout，生产环境用滚动文件，可替换为 syslog / 结构化 JSON
```

config.json：

```json
"logging": {
  "level": "info",
  "output": "stdout",
  "file": "logs/stream-server.log",
  "max_size_mb": 50,
  "max_files": 5
}
```

### 统一错误码（StreamError）

各模块不抛原始 `std::exception`，统一抛 `StreamError`，便于上层统一处理和日志记录：

```cpp
// core/include/core/error.h

enum class ErrorCode {
    CaptureDeviceLost, CaptureFormatUnsupported,
    EncoderInitFailed, EncoderCrashed,
    TransportDisconnected, TransportTimeout,
    AuthFailed, AuthTokenExpired,
    InputPermissionDenied,    // Linux uinput 权限不足（需要 input 用户组或 udev 规则）
    InvalidConfig, ResourceExhausted,
};

struct StreamError : public std::runtime_error {
    ErrorCode   code;
    std::string module;   // 来源模块名，用于日志归因
    StreamError(ErrorCode c, std::string msg, std::string mod)
        : std::runtime_error(msg), code(c), module(std::move(mod)) {}
};
// pipeline.cpp 统一 catch(const StreamError&)，根据 code 决定重试/降级/终止 Session
```

### 配置热重载

部分配置项支持运行时修改，无需重启服务器：

```cpp
// core/include/core/config_watcher.h

class IConfigWatcher {
public:
    virtual ~IConfigWatcher() = default;
    // 回调传原始 JSON 字符串，由调用方自行解析，保持 core 层零第三方依赖
    virtual void watch(const std::string& path,
                       std::function<void(const std::string&)> on_change) = 0;
    virtual void stop() = 0;
};
// 实现：Linux inotify，macOS FSEvents，Windows ReadDirectoryChangesW
```

热重载支持范围：

| 配置项 | 热重载方式 |
|--------|-----------|
| `encoder.bitrate_kbps` / `fps` / `gop.*` | `encoder->update_params()` |
| `transport.pacing_enabled` / `jitter_buffer_*` | `transport->set_jitter_buffer()` |
| `qos.per_user.*` | 更新 SessionManager QoS 策略 |
| `logging.level` | 更新 ILogger 级别 |

不支持热重载（需重启）：`capture.backend`、`encoder.backend`、`transport.backend`、`server.*`。

### 优雅关闭

收到 SIGTERM / Ctrl+C 时，按顺序安全停止各组件：

```
SIGTERM
  │
  ▼ 1. 停止接受新连接
  ▼ 2. 通知所有客户端服务器即将关闭
  ▼ 3. 停止采集线程（不再产生新帧）
  ▼ 4. 等待管道队列排空（最多 drain_timeout_ms，默认 500ms）
  ▼ 5. 停止编码/传输线程，发完队列中剩余数据
  ▼ 6. 关闭所有 Transport 连接
  ▼ 7. 持久化 Session 状态，退出
```

config.json：`"shutdown": { "drain_timeout_ms": 500 }`

---

## 测试策略

### 原则

`core/` 层的纯虚接口天然支持 mock 测试——测试代码实现假的 Adapter，不需要真实硬件。这是整个架构最重要的可测试性收益。

### 分层测试

```
单元测试（core/ 层接口 + pipeline 逻辑）
    无需任何硬件，全平台可跑，CI 必须通过

集成测试（Adapter 实现 + 真实硬件）
    需要特定硬件（NVENC/VAAPI/PipeWire），本地可选运行

端到端测试（完整 client-server 流程）
    用 StubTransport 模拟客户端，验证完整握手和串流流程
```

### 核心 Mock 接口

所有 Adapter 接口均可 mock，关键的几个：

```cpp
// test/mocks/mock_capture.h
class MockCapture : public ICaptureSource {
public:
    // 预设要返回的帧序列，用于测试 Pipeline 行为
    void enqueue(RawFrame frame) { frames_.push(frame); }

    std::optional<RawFrame> next_frame() override {
        if (frames_.empty()) return std::nullopt;
        auto f = frames_.front(); frames_.pop(); return f;
    }
    AdapterCapabilities capabilities() const override { return caps_; }
    void set_event_callback(std::function<void(CaptureEvent)> cb) override { cb_ = cb; }
    void fire_event(CaptureEvent e) { if (cb_) cb_(e); }

    // 以下为 ICaptureSource 纯虚方法的 stub 实现
    std::vector<DisplayInfo> enumerate_displays() const override { return {}; }
    void select_display(int index) override {}
    int  display_refresh_rate() const override { return 60; }
    std::optional<CursorState> next_cursor() override { return std::nullopt; }

private:
    std::queue<RawFrame> frames_;
    AdapterCapabilities  caps_;
    std::function<void(CaptureEvent)> cb_;
};

// test/mocks/mock_encoder.h
class MockEncoder : public IEncoder {
public:
    int submit_count = 0;
    EncoderParams last_params;

    void submit_frame(RawFrame f, SubmitFlags) override {
        ++submit_count;
        // 立即触发回调，方便测试端到端流程
        if (cb_) cb_(make_fake_packet(f.pts_us));
    }
    void set_encoded_callback(std::function<void(EncodedPacket)> cb) override { cb_ = cb; }
    void update_params(const EncoderParams& p) override { last_params = p; }
    AdapterCapabilities capabilities() const override { return {}; }
private:
    std::function<void(EncodedPacket)> cb_;
};

// test/mocks/mock_transport.h
class MockTransport : public ITransport {
public:
    void fire_event(TransportEvent e) { if (event_cb_) event_cb_(e); }

    bool connect(const std::string&) override { return true; }
    void disconnect() override {}
    void send(EncodedPacket) override {}
    void send_batch(std::vector<EncodedPacket>) override {}
    void set_event_callback(std::function<void(TransportEvent)> cb) override { event_cb_ = cb; }
    void set_stats_callback(std::function<void(NetworkStats)>) override {}
    void set_jitter_buffer(int, int) override {}
    void set_fec_params(const FecParams&) override {}
    void set_input_callback(std::function<void(InputEvent)>) override {}
    void send_audio(AudioPacket) override {}
    void send_haptic(const HapticCommand&) override {}
    void send_stats(const PipelineMetrics&) override {}
    void set_mic_callback(std::function<void(AudioFrame)>) override {}
    std::string sink_id() const override { return "mock_transport"; }
    void on_packet(EncodedPacket p) override { send(p); }
    void on_audio(AudioPacket) override {}
private:
    std::function<void(TransportEvent)> event_cb_;
};

// test/mocks/mock_input.h
class MockInputHandler : public IInputHandler {
public:
    std::vector<InputEvent> injected;
    void inject(const InputEvent& e) override { injected.push_back(e); }
};
```

### 典型测试场景

```cpp
// 测试 1：网络拥塞时码率自动降低
TEST(Pipeline, ReducesBitrateOnCongestion) {
    MockCapture      capture;
    MockEncoder      encoder;
    MockTransport    transport;
    MockInputHandler input;

    // 模拟 10 帧采集
    for (int i = 0; i < 10; i++) capture.enqueue(make_test_frame(i));

    // run_pipeline 在独立线程中运行，此处以伪代码形式示意异步启动
    std::thread pipeline_thread([&]{ run_pipeline(capture, nullptr, encoder, transport, input, cfg); });
    pipeline_thread.detach();

    // 触发拥塞事件
    transport.fire_event(TransportEvent::Congested);

    // 验证编码器参数被降低
    EXPECT_LT(encoder.last_params.bitrate_kbps, cfg.encoder.bitrate_kbps);
}

// 测试 2：设备丢失后触发重连状态机
TEST(Pipeline, EntersSuspendedOnDeviceLost) {
    MockCapture capture;
    // ... 启动 pipeline
    capture.fire_event(CaptureEvent::DeviceLost);
    EXPECT_EQ(pipeline.state(), PipelineState::Suspended);
}

// 测试 3：Preprocessor nullptr 路径（零拷贝）
TEST(Factory, SelectsNullPreprocessorForDmabuf) {
    AdapterCapabilities cap, enc;
    cap.supports_dmabuf = true;
    enc.supports_dmabuf = true;
    auto preprocessor = select_preprocessor(cap, enc, "auto");
    EXPECT_EQ(preprocessor, nullptr);
}
```

### 测试框架

推荐 **Google Test（gtest）**，与其他依赖一致，预编译后放入 `vendor/`：

```cmake
# test/CMakeLists.txt
if(WIN32)
    set(GTEST_LIB ${CMAKE_SOURCE_DIR}/vendor/windows/x64/$<CONFIG>/gtest.lib)
elseif(APPLE)
    set(GTEST_LIB ${CMAKE_SOURCE_DIR}/vendor/macos/universal/libgtest.a)
else()
    set(GTEST_LIB ${CMAKE_SOURCE_DIR}/vendor/linux/${CMAKE_SYSTEM_PROCESSOR}/libgtest.a)
endif()

add_executable(stream-server-tests
    test_pipeline.cpp
    test_factory.cpp
    test_reconnect.cpp
)
target_include_directories(stream-server-tests PRIVATE ${CMAKE_SOURCE_DIR}/vendor/include)
target_link_libraries(stream-server-tests PRIVATE core ${GTEST_LIB})
```

---

## 第三方依赖管理规则

### 分层依赖策略

所有第三方依赖统一纳入 `vendor/`，按类型分为预编译二进制和纯头文件两种形式，不依赖任何外部包管理器（vcpkg、apt、brew 等），不依赖构建时下载（FetchContent）。

| 类型 | 代表库 | 管理方式 | 获取方式 |
|------|--------|---------|---------|
| Header-only 库 | nlohmann/json.hpp | `vendor/include/` 直接入库 | 官方 release 下载单头文件 |
| 硬件 SDK 头文件 | nvEncodeAPI.h、AMFComponents.h | `vendor/include/` 直接入库 | 官方只提供头文件；运行时 `dlopen` 系统驱动 |
| 纯软件静态库 | libx264.a、libopus.a、libgtest.a | `vendor/<platform>/` 预编译二进制 | 各平台编译一次后入库，随源码提交 |
| 可获取预编译包的库 | libmsquic（QUIC）、libwebrtc | `vendor/<platform>/` 预编译二进制 | 官方 GitHub Releases 提供预编译包 |
| 平台系统库 | PipeWire、VAAPI、DRM、DXGI、IOKit、CoreAudio | 系统自带，`pkg-config` / `find_package` | 与 OS / 内核 / 驱动版本强绑定，无法 vendor |
| 运行时驱动库 | libnvidia-encode.so.1、libva.so、amfrt64 | `dlopen` 运行时加载 | GPU 驱动附带，版本与 GPU 驱动一致，不入库 |

**判断标准**：

- 能跨平台、跨 OS 版本运行的纯软件库 → **静态入库**（`libx264.a`、`libopus.a`）
- 与 OS 内核或平台 API 版本绑定的系统服务 → **pkg-config 动态链接**（PipeWire、VAAPI、DRM）
- 硬件驱动提供的运行时 → **dlopen**（NVENC、VAAPI 驱动、AMF）
- 仅有头文件、实现在系统驱动中 → **只入库头文件**（nvEncodeAPI.h）

### 依赖组织方式：模块自治

每个 Adapter 模块在自己的目录下直接存放依赖文件，无单独的 `vendor/` 子目录。
静态库放 `lib/`，SDK 头文件放 `include/`，全局 header-only 放根目录的 `common/include/`。

```
video/encoder/nvenc/linux/
├── include/
│   └── nvEncodeAPI.h          ← 私有 SDK 头文件（apt install libffmpeg-nvenc-dev 后复制）

video/encoder/x264/all/
├── include/
│   ├── x264.h
│   └── x264_config.h
└── lib/
    ├── linux_x86_64/libx264.a  ← 预编译静态库（各平台编译一次入库）
    ├── windows_x64/libx264.lib
    └── macos_universal/libx264.a

audio/encoder/opus/all/
├── include/opus/               ← opus.h 等头文件
└── lib/
    ├── linux_x86_64/libopus.a
    ├── windows_x64/libopus.lib
    └── macos_universal/libopus.a
```

CMake 模式（以 x264 为例）：

```cmake
# video/encoder/x264/all/CMakeLists.txt

if (WIN32)
    set(_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/windows_x64/libx264.lib)
elseif (APPLE)
    set(_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/macos_universal/libx264.a)
else()
    set(_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/linux_x86_64/libx264.a)
endif()

target_link_libraries(encoder_x264 PRIVATE ${_LIB})
target_include_directories(encoder_x264 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
```

全局只共享少量真正跨模块的依赖（如 `nlohmann/json.hpp`），放在根目录的 `common/include/`；其他均分散到各模块内部。

`core` 层依然零第三方依赖，这条规则由 CMake 强制执行：

```cmake
# core/CMakeLists.txt
add_library(pulsar_core STATIC src/pipeline.cpp)
target_include_directories(pulsar_core PUBLIC include)
# 严禁在这里 target_link_libraries 任何外部库
# CI 中用 cmake --graphviz 验证 core 无外部边
```

| 规则 | 说明 |
|------|------|
| `core` 层零外部依赖 | CMake 强制，CI graphviz 验证 |
| 构建时零网络请求 | 不用 FetchContent / vcpkg，clone 即可构建 |
| 版本锁定 | 二进制入库，所有开发者和 CI 使用完全相同的版本 |
| 每个 Adapter 管理自己的依赖 | 独立 `target_link_libraries`，不传播到其他模块 |
| 替换质量差的库 | 只改对应 Adapter 的链接，core 和其他模块零影响 |
| git LFS 管理二进制 | `.lib` / `.a` / `.so` / `.dylib` 文件走 git LFS，避免 repo 膨胀 |

---

