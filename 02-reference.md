> 参见 [01-blueprint.md](01-blueprint.md) 查看系统蓝图与设计目标

# 实现参考

## 优化全景图

| 优化类别 | 模块 | 接口扩展点 |
|---------|------|-----------|
| 零拷贝（GPU buffer 传递） | Capture → Preprocessor → Encoder | `FrameBuffer::native_handle()` |
| 脏区域更新 | Capture | `RawFrame::dirty_rects` |
| HDR / 色彩空间透传 | Capture → Encoder | `RawFrame::color_info` |
| 帧率自适应（静止降采集率） | Pipeline | `PipelineConfig::idle_fps` |
| Preprocessor 直通（格式已匹配） | Preprocessor | Factory 返回 `nullptr`，阶段完全不存在 |
| 低延迟 GOP（B 帧关闭 / intra-refresh） | Encoder | `GopConfig::low_latency_preset` |
| 强制关键帧 | Encoder | `SubmitFlags::force_keyframe` |
| 码率自适应 | Transport → Encoder | `TransportEvent::Congested` + `update_params()` |
| FEC 前向纠错 | Transport | `set_fec_params()` |
| 网络指标反馈（RTT / 丢包 / 带宽） | Transport → Pipeline | `NetworkStats` |
| scatter-gather 批量发送 | Transport | `send_batch()` |
| 自适应抖动缓冲 | Transport | `set_jitter_buffer()` |
| SPSC 无锁队列 + 背压 | Pipeline | `SPSCQueue<T>` |
| 线程优先级 + CPU 亲和性 | Pipeline | `ThreadConfig` |
| 服务器探测（自动最优编码器） | Profiler | `IProfiler::run()` → `ServerProfile` |
| 能力协商（自动最优链路） | Factory | `AdapterCapabilities` |
| 断线重连（状态保留） | Pipeline | `ReconnectPolicy`、`PipelineState` |
| 编码器崩溃降级（NVENC → x264） | Pipeline | 异常捕获 + 重新初始化 |
| 多路输出（直播 + 录制） | IOutputMultiplexer | `add_sink()`、`broadcast()` |
| 多客户端输入仲裁 | IInputArbiter | `bind()`、`allow()` |
| 虚拟手柄 | IInputHandler | `create_gamepad()`、`destroy_gamepad()` |
| 双向音频（麦克风） | ITransport + IAudioPlayback | `set_mic_callback()` |
| 多人语音混音 | IAudioMixer | `push()`、`mix_for()` |
| Wake-on-LAN | IWakeOnLan | `listen()`、`send()` |
| TLS / DTLS 加密 | Transport | `security` 配置节点 |
| NAT 穿透（UPnP / STUN / TURN） | Transport | `nat` 配置节点 |

---

## 核心数据结构

### 视频帧（`core/include/frame.h`）

| 类型 | 字段 | 说明 |
|------|------|------|
| `enum PixelFormat` | BGRA / RGBA / NV12 / YUV420 | — |
| `enum CodecType` | H264 / H265 / AV1 | — |
| `enum ColorSpace` | BT709 / BT2020 | — |
| `enum TransferFunc` | SDR / PQ / HLG | HDR 传输函数 |
| `ColorInfo` | color_space / transfer_func / max_luminance / min_luminance | HDR 元数据 |
| `DirtyRect` | x / y / width / height | 帧变化区域 |
| `FrameBuffer` | data() / size() / native_handle() | GPU 路径通过 native_handle() 绕过 CPU |
| `PacketBuffer` | data() / size() / native_handle() | 编码器输出，同上 |
| `RawFrame` | buffer / width / height / pts_us / format / color_info / dirty_rects | pts_us 来自单调时钟，下游只读 |
| `EncodedPacket` | buffer / is_keyframe / pts_us / codec | Transport 按 codec 选择封包格式 |

### 网络统计（`core/include/transport.h`）

| 字段 | 类型 | 说明 |
|------|------|------|
| rtt_ms | float | 往返延迟 ms |
| loss_rate | float | 丢包率 0.0~1.0 |
| bandwidth_kbps | float | 可用带宽估算 |
| jitter_us | int64_t | 延迟抖动 |
| nack_requests | int | 客户端重传请求次数 |
| fec_recovered | int | FEC 恢复的包数 |

### 编码参数（`core/include/encoder.h`）

| 结构 / 枚举 | 关键字段 | 说明 |
|------------|---------|------|
| `GopConfig` | max_b_frames / gop_size / low_latency_preset / intra_refresh / target_fps | |
| `EncoderParams` | bitrate_kbps / fps / gop / preset | |
| `QualityPreset` | LowLatency / Balanced / HighQuality | |
| `SubmitFlags` | None / ForceKeyframe | submit_frame 控制标志 |
| `FecParams` | enabled / data_shards / parity_shards | |

### 音频数据（`core/include/audio.h`）

| 类型 | 关键字段 | 说明 |
|------|---------|------|
| `enum AudioFormat` | PCM_S16LE / PCM_F32LE | — |
| `enum AudioCodec` | AAC / OPUS / PCM | — |
| `AudioFrame` | data / size / sample_rate / channels / samples / pts_us / format | pts_us 与视频同源 |
| `AudioPacket` | data / size / pts_us / codec | — |

---

## 接口定义

### ICaptureSource（`core/include/capture.h`）

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `next_frame()` | `optional<RawFrame>` | 无新帧返回 nullopt（idle_fps 节流时） |
| `next_cursor()` | `optional<CursorState>` | 光标状态，独立于视频帧由 Transport 单独发送 |
| `enumerate_displays()` | `vector<DisplayInfo>` | 多显示器枚举 |
| `select_display(int)` | `void` | 动态切换显示器 |
| `display_refresh_rate()` | `int` | 当前显示器刷新率 Hz，Pipeline 据此设置帧率上限 |
| `supports_window_capture()` | `bool` | 可选：窗口级采集 |
| `set_event_callback(fn)` | `void` | DeviceLost / FormatChanged / ResolutionChanged |
| `capabilities()` | `AdapterCapabilities` | 工厂能力协商 |

`CursorState`：x / y / visible / image_rgba / hotspot_x / hotspot_y

`DisplayInfo`：index / name / width / height / refresh_rate / is_primary / hdr_supported

`CaptureEvent`：DeviceLost / FormatChanged / ResolutionChanged / DisplayChanged

### IPreprocessor（`core/include/preprocessor.h`）

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `process(RawFrame)` | `optional<RawFrame>` | 返回 nullopt = 直通，格式已匹配时跳过 |
| `capabilities()` | `AdapterCapabilities` | — |

Factory 在格式已匹配或 dmabuf 路径时返回 `nullptr`，Pipeline 完全跳过此阶段（零开销）。

### IEncoder（`core/include/encoder.h`）

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `submit_frame(frame, flags)` | `void` | 异步提交，立即返回 |
| `set_encoded_callback(fn)` | `void` | 编码完成后回调，推入 Q3 |
| `update_params(params)` | `void` | 热更新码率/GOP，无需重建编码器 |
| `capabilities()` | `AdapterCapabilities` | — |

### ITransport（`core/include/transport.h`）

ITransport 继承 IPacketSink，可直接注册到 IOutputMultiplexer。

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `connect(endpoint)` | `bool` | 建立连接 |
| `disconnect()` | `void` | 主动断开 |
| `send(packet)` | `void` | 单包发送 |
| `send_batch(packets)` | `void` | 批量发送，降低 syscall 开销 |
| `send_audio(packet)` | `void` | 音频发送，按 pts_us 与视频交错 |
| `send_haptic(cmd)` | `void` | 手柄振动反馈 → 客户端 |
| `send_stats(metrics)` | `void` | 统计数据 → 客户端 OSD |
| `set_event_callback(fn)` | `void` | Disconnected / Congested / Ready |
| `set_stats_callback(fn)` | `void` | 定期上报 NetworkStats（约 200ms） |
| `set_input_callback(fn)` | `void` | 客户端输入事件 → IInputArbiter |
| `set_mic_callback(fn)` | `void` | 客户端麦克风数据 → IAudioPlayback |
| `set_jitter_buffer(min, max)` | `void` | 自适应抖动缓冲 ms |
| `set_fec_params(params)` | `void` | FEC 动态开关 |

### IInputHandler（`core/include/input.h`）

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `inject(event)` | `void` | 注入键鼠/手柄/触摸事件 |
| `create_gamepad(id)` | `bool` | 创建虚拟手柄（ViGEm/uinput） |
| `destroy_gamepad(id)` | `void` | — |
| `set_haptic_callback(fn)` | `void` | 从 ViGEm/XInput 轮询振动状态 |
| `set_clipboard(text)` | `void` | 剪贴板同步 |
| `get_clipboard()` | `string` | — |

`InputEvent::Type`：KeyDown / KeyUp / MouseMove / MouseAbsolute / MouseButton / MouseWheel / GamepadButton / GamepadAxis / TouchBegin / TouchUpdate / TouchEnd / ClipboardText

Linux uinput now exposes a dedicated absolute pointer device for `MouseAbsolute` and falls back to relative motion only when that device cannot be created.

Linux / Wayland multiseat note: the virtual input device names should include `XDG_SEAT` when it is not `seat0`, and a udev rule should tag those devices with the matching seat. In headless mode, `XDG_SEAT` refers to the seat of the server-hosted virtual session (e.g. a systemd-started Wayland compositor), **not** a physically logged-in user session. A uinput write can succeed while the compositor still ignores the event if the seat does not match.

### IOutputMultiplexer（`core/include/multiplexer.h`）

| 方法 | 说明 |
|------|------|
| `add_sink(shared_ptr<IPacketSink>)` | 注册 sink（Transport 或 Recorder） |
| `remove_sink(id)` | 注销 |
| `broadcast(EncodedPacket)` | shared_ptr 零拷贝广播到所有 sink |
| `broadcast_audio(AudioPacket)` | — |

IRecorder 继承 IPacketSink，注册后接收视频/音频写入 MP4 容器。

### IInputArbiter（`core/include/shared_session.h`）

| 方法 | 说明 |
|------|------|
| `bind(ClientDeviceBinding)` | 绑定客户端 → 设备类型列表 |
| `unbind(client_id)` | 断开时注销 |
| `allow(client_id, event)` | 事件类型匹配则允许注入，否则丢弃 |
| `list_bindings()` | 广播当前绑定状态给所有客户端 |

`ClientDeviceBinding`：client_id / claimed_types（InputEvent::Type 列表）/ gamepad_id

### IAppManager（`core/include/app_manager.h`）

| 方法 | 说明 |
|------|------|
| `list_apps()` | 返回 AppEntry 列表 |
| `launch(app_id)` | 启动游戏/程序 |
| `terminate(app_id)` | 关闭 |
| `is_running(app_id)` | 状态查询 |

`AppEntry`：id / name / executable / args / working_dir / cover_image

### IAuthProvider（`core/include/auth.h`）

| 方法 | 说明 |
|------|------|
| `challenge()` | 返回 AuthChallenge（scheme + fields KV），告知客户端需要什么 |
| `authenticate(AuthToken)` | 验证，AuthToken 为通用 KV，各 Adapter 自行解析 |

支持 Adapter：PasswordAuth / OAuthAuth / PasskeyAuth / LdapAuth（通过 `auth.method` 切换）

### ISessionManager（`core/include/session.h`）

| 方法 | 说明 |
|------|------|
| `create(AuthToken)` | 创建 Session，状态 Authenticating |
| `activate(session_id)` | 认证成功，转为 Active |
| `terminate(session_id)` | 销毁 |
| `find(session_id)` | 查找 |
| `get_qos(session_id)` | 返回用户 QoS 策略（码率/帧率/优先级上限） |
| `default_qos()` | 未认证连接的默认策略 |

`QosPolicy`：max_bitrate_kbps / max_fps / priority / max_cpu_percent / max_memory_mb

`NegotiatedParams`：codec / width / height / fps / bitrate_kbps / hdr_enabled / audio_codec

### Pipeline（`core/include/pipeline.h`）

```
void run_pipeline(
    ICaptureSource&       capture,
    IPreprocessor*        preprocessor,  // nullptr = 跳过
    IEncoder&             encoder,
    IPacketSink&          sink,          // ITransport 或 IOutputMultiplexer 均可
    IInputHandler&        input,
    const PipelineConfig& cfg
)
```

`PipelineConfig` 关键字段：

| 字段 | 默认值 | 说明 |
|------|--------|------|
| queue_capacity | 4 | 各队列容量（帧数） |
| drop_on_overflow | true | 队列满时丢帧（false = 阻塞） |
| idle_fps | 10 | 画面静止时的降速帧率 |
| audio_video_offset_ms | 40 | 音视频允许最大偏差 ±ms |
| reconnect | ReconnectPolicy | 断线重连策略 |
| metrics | nullptr | 注入后各阶段自动上报指标 |
| capture/encode/transport_thread | ThreadConfig | 优先级 + CPU 亲和性 |

`ReconnectPolicy`：suspend_timeout_ms=30000 / reconnect_interval_ms=1000 / max_retries=-1 / force_keyframe_on_resume=true

`PipelineState`：Running / Suspended / Recovering / Stopped

### 音频接口

| 接口 | 关键方法 | 说明 |
|------|---------|------|
| `IAudioCapture` | `next_frame()` → `optional<AudioFrame>` | 静音时返回 nullopt |
| `IAudioEncoder` | `submit_frame(AudioFrame)` + `set_encoded_callback(fn)` | — |
| `IAudioPlayback` | `play(AudioFrame)` | 将客户端 mic 注入虚拟音频设备 |

### IAudioMixer（`core/include/audio_mixer.h`）

| 方法 | 说明 |
|------|------|
| `add_client(MixerClientConfig)` | 注册（client_id / volume / muted） |
| `remove_client(id)` | 注销 |
| `push(client_id, AudioFrame)` | 推入 mic 帧 |
| `mix_for(client_id)` | 返回混音结果，已排除自身 |
| `set_volume(id, float)` | 运行时调音量 |
| `set_muted(id, bool)` | 运行时静音 |

PTT / VAD 模式通过 `voice_chat.mode` 配置（"always" / "ptt" / "vad"）。

### 其他接口

| 接口 | 文件 | 关键方法 |
|------|------|---------|
| `IChatChannel` | chat.h | `broadcast(msg, exclude_sender)` / `set_message_callback(fn)` |
| `IWakeOnLan` | wake_on_lan.h | `listen(port=9)` / `stop()` / `send(mac, broadcast_addr)` |
| `IProfiler` | profiler.h | `run()` → `ServerProfile`（含 HardwareProfile + EncoderBenchmark[] + NetworkProfile） |
| `ILogger` | logger.h | `log(level, msg)` / level: Debug/Info/Warn/Error |
| `IMetricsCollector` | metrics.h | `report(PipelineMetrics)` |
| `IBufferPool` | buffer_pool.h | `acquire()` → `shared_ptr<FrameBuffer>` |
| `ICapabilityNegotiator` | negotiation.h | `negotiate(ClientCapabilities, ServerProfile, QosPolicy)` → `NegotiatedParams` |

`AdapterCapabilities` 关键字段：supports_dmabuf / supports_gpu_preprocessing / supports_dirty_rect / supports_hdr / supports_headless

`supports_headless` 的含义：server 可以在没有实体用户登录本机物理桌面的情况下运行；它不表示“没有会话”。相反，headless server 仍然需要托管一个可被捕获和注入输入的虚拟会话（虚拟 compositor / 虚拟显示 / 虚拟桌面），并将观众连接到这个 server 侧会话上。管理员可以临时登录图形环境做排障，但这只是运维入口，不是 headless 的前提。

`shared_session` 的含义：多个网络客户端共享同一个 server 侧会话。共享的是输入归属和流会话，不是要求每个客户端对应一个本地桌面登录实例。

---

## 配置文件（config.json）

```json
{
  "protocols": {
    "rtp":          { "enabled": true,  "port": 47984 },
    "webrtc":       { "enabled": true,  "port": 47985 },
    "quic":         { "enabled": true,  "port": 47986 },
    "webtransport": { "enabled": false, "port": 47987 }
  },
  "auth": {
    "method": "password",
    "password_file": "users.json"
  },
  "capture": {
    "backend": "auto"
  },
  "preprocessor": {
    "backend": "auto"
  },
  "encoder": {
    "backend": "auto",
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
    "pacing_enabled": true,
    "jitter_buffer_min_ms": 20,
    "jitter_buffer_max_ms": 150,
    "fec": { "enabled": false, "data_shards": 10, "parity_shards": 2 }
  },
  "input": { "backend": "uinput" },
  "pipeline": {
    "queue_capacity": 4,
    "drop_on_overflow": true,
    "idle_fps": 10
  },
  "qos": {
    "default_max_bitrate_kbps": 20000,
    "default_max_fps": 60,
    "per_user": {
      "alice": { "max_bitrate_kbps": 30000, "max_fps": 120, "priority": 10 }
    }
  },
  "audio": {
    "capture_backend": "auto",
    "encoder": "opus",
    "sample_rate": 48000,
    "channels": 2,
    "bitrate_kbps": 128,
    "mic_enabled": false,
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
    "rtmp": { "enabled": false, "url": "rtmp://live.twitch.tv/app/<key>" },
    "srt":  { "enabled": false, "url": "srt://ingest.example.com:9000" }
  },
  "shared_session": { "enabled": false, "max_clients": 4 },
  "voice_chat": {
    "enabled": false,
    "mode": "vad",
    "vad_threshold": 0.02,
    "per_client_volume": true
  },
  "apps": [
    { "id": "cs2", "name": "CS2", "executable": "/usr/bin/cs2" }
  ],
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
  "wake_on_lan": { "enabled": false, "listen_port": 9, "broadcast_address": "255.255.255.255" },
  "clipboard": { "enabled": false, "max_size_kb": 1024 },
  "metrics": { "enabled": true, "report_interval_ms": 1000, "output": "stdout" },
  "logging": { "level": "info", "output": "stdout", "file": "logs/server.log" },
  "shutdown": { "drain_timeout_ms": 500 }
}
```

---

## CMake 结构

```cmake
# 根 CMakeLists.txt — 平台分发
add_subdirectory(core)
if (UNIX AND NOT APPLE)
    add_subdirectory(video/capture/linux/drm_virtual)
    add_subdirectory(video/preprocessor/linux/dmabuf)
    add_subdirectory(video/preprocessor/linux/gpu)
    add_subdirectory(video/encoder/nvenc/linux)
    add_subdirectory(video/encoder/vaapi/linux)
    add_subdirectory(audio/capture/linux/pipewire)
    add_subdirectory(transport/webrtc/linux)
    add_subdirectory(transport/quic/linux)
    # add_subdirectory(transport/webtransport/linux)  # 预留，HTTP/3 WebTransport
    add_subdirectory(input/linux/uinput)
elseif (WIN32)
    add_subdirectory(video/capture/windows/virtual_display)
    add_subdirectory(video/preprocessor/windows/gpu)
    add_subdirectory(video/encoder/nvenc/windows)
    add_subdirectory(audio/capture/windows/wasapi)
    add_subdirectory(input/windows/win32)
elseif (APPLE)
    add_subdirectory(video/capture/macos/cgvirtual)
    add_subdirectory(video/preprocessor/macos/gpu)
    add_subdirectory(video/encoder/videotoolbox/macos)
    add_subdirectory(audio/capture/macos/coreaudio)
    add_subdirectory(input/macos/iokit)
endif()
add_subdirectory(video/preprocessor/all/cpu)
add_subdirectory(video/encoder/x264/all)
add_subdirectory(audio/encoder/opus/all)
add_subdirectory(transport/rtp/all)
add_subdirectory(transport/rtmp/all)
add_subdirectory(transport/srt/all)
add_subdirectory(server)

# server/CMakeLists.txt（Linux 示意）
add_executable(pulsar_server src/main.cpp src/factory.cpp src/config_parser.cpp src/profiler.cpp)
target_link_libraries(pulsar_server
    PRIVATE pulsar_core
    PRIVATE capture_drm_virtual
    PRIVATE preprocessor_dmabuf preprocessor_gpu preprocessor_cpu
    PRIVATE encoder_nvenc encoder_vaapi encoder_x264
    PRIVATE audio_capture_pipewire audio_encoder_opus
    PRIVATE transport_rtp transport_webrtc transport_quic
    # PRIVATE transport_webtransport  # 预留
    PRIVATE input_uinput
    PRIVATE auth_password
)
```

---

## 断线重连

| 状态 | 触发 | 行为 |
|------|------|------|
| Running | — | 正常 |
| Suspended | Transport::Disconnected | 暂停采集/编码，等待重连（超时前保留管道状态） |
| Recovering | 新连接到达 | 发送 ForceKeyframe，恢复采集 |
| Stopped | suspend_timeout 超时 | 销毁管道，通知 server 层重新握手 |

编码器崩溃：submit_frame 抛出异常 → 重新初始化 → 失败则降级到 x264 → 通过 Metrics 上报。

设备丢失（Capture DeviceLost）：重试 3 次 → 失败则终止 Session。

分辨率变化（ResolutionChanged）：重建 Preprocessor + ForceKeyframe。

---

## 服务器探测（Profiler）

启动时对每个可用编码器编 30 帧，测量延迟 / 抖动 / 码率效率，结合 GPU 负载和网络条件综合评分，自动选最优链路。

评分权重：稳定性 40% + 码率效率 30% + 吞吐量 20% + 网络修正 10%。

特殊场景：
- 带宽 < 5 Mbps → 惩罚码率效率差的编码器（NVENC 低码率下输给 x264）
- GPU 负载 > 80% → 惩罚硬件编码器（延迟抖动大）
- CPU 核数 ≥ 16 → x264 veryfast 可能得分不低于 NVENC

结果缓存到磁盘（`profiler.cache_ttl_seconds`），硬件不变时下次启动复用。
