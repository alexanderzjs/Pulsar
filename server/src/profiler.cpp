// server/src/profiler.cpp
// ProfilerImpl: benchmarks available encoders + collects real hardware info.
// Results are cached to disk with TTL validation.

#include "server_profiler.h"

#include "nvenc_encoder.h"
#include "vaapi_encoder.h"
#include "x264_encoder.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <sys/sysinfo.h>
#include <unistd.h>

namespace pulsar::server {

using namespace pulsar::core;

// ─── Synthetic frame (NV12) ───────────────────────────────────────────────────
namespace {
struct SynBuf final : public FrameBuffer {
    std::vector<uint8_t> d_;
    SynBuf(int w,int h):d_(static_cast<size_t>(w*h*3/2),0x80){}
    uint8_t* data() const override{return const_cast<uint8_t*>(d_.data());}
    size_t size()   const override{return d_.size();}
};
RawFrame make_test_frame(int w,int h,int64_t pts){
    RawFrame f; f.buffer=std::make_shared<SynBuf>(w,h);
    f.width=w;f.height=h;f.pts_us=pts;
    f.format=PixelFormat::NV12;f.dirty_rects.push_back({0,0,w,h});
    return f;
}

// ─── Real hardware info ───────────────────────────────────────────────────────
HardwareProfile collect_hardware() {
    HardwareProfile hw;

    // CPU cores
    hw.cpu_cores = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));

    // CPU max frequency from /sys
    {
        std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
        if (f) {
            long khz = 0; f >> khz;
            hw.cpu_freq_ghz = static_cast<float>(khz) / 1'000'000.0f;
        }
        if (hw.cpu_freq_ghz <= 0.0f) {
            // Fallback: parse /proc/cpuinfo "model name" for GHz
            std::ifstream ci("/proc/cpuinfo");
            std::string line;
            while (std::getline(ci, line)) {
                auto pos = line.find("@ ");
                if (pos != std::string::npos) {
                    try { hw.cpu_freq_ghz = std::stof(line.substr(pos + 2)); }
                    catch(...) {}
                    break;
                }
            }
        }
    }

    // Available memory from sysinfo
    {
        struct sysinfo si{};
        if (sysinfo(&si) == 0)
            hw.available_memory_mb = static_cast<int>(
                si.freeram * si.mem_unit / (1024 * 1024));
    }

    // GPU VRAM and utilization via nvidia-smi (works if nvidia-smi is installed)
    {
        FILE* p = popen("nvidia-smi --query-gpu=memory.total,utilization.gpu "
                        "--format=csv,noheader,nounits 2>/dev/null", "r");
        if (p) {
            int vram = 0; float util = 0;
            if (fscanf(p, "%d , %f", &vram, &util) == 2) {
                hw.gpu_vram_mb     = vram;
                hw.gpu_utilization = util / 100.0f;
            }
            pclose(p);
        }
    }

    return hw;
}

// ─── Benchmark a single encoder ───────────────────────────────────────────────
EncoderBenchmark benchmark_encoder(IEncoder& encoder,
                                   const std::string& backend,
                                   int frames, int target_kbps)
{
    constexpr int kW = 1280, kH = 720;
    EncoderBenchmark bench; bench.backend=backend; bench.available=false;

    size_t packets=0, total_bytes=0;
    encoder.set_encoded_callback([&](EncodedPacket p){
        ++packets; if(p.buffer) total_bytes+=p.buffer->size();
    });
    EncoderParams params; params.bitrate_kbps=target_kbps; params.fps=60;
    params.gop.gop_size=60; encoder.update_params(params);

    // Warm-up
    { auto f=make_test_frame(kW,kH,0); encoder.submit_frame(std::move(f),SubmitFlags::ForceKeyframe); }

    std::vector<float> latencies; latencies.reserve(static_cast<size_t>(frames));
    const auto run_start = std::chrono::steady_clock::now();
    for (int i=0; i<frames; ++i) {
        auto t0=std::chrono::steady_clock::now();
        encoder.submit_frame(make_test_frame(kW,kH,static_cast<int64_t>(i)*16666));
        latencies.push_back(std::chrono::duration<float,std::milli>(
            std::chrono::steady_clock::now()-t0).count());
    }
    const float elapsed=std::chrono::duration<float>(
        std::chrono::steady_clock::now()-run_start).count();

    if (packets==0||elapsed<=0) return bench;
    bench.available=true;
    bench.encode_fps=static_cast<float>(frames)/elapsed;
    float mean=0; for (float v:latencies) mean+=v;
    mean/=static_cast<float>(latencies.size());
    bench.latency_ms=mean;
    float var=0; for (float v:latencies) var+=(v-mean)*(v-mean);
    bench.latency_jitter_ms=std::sqrt(var/static_cast<float>(latencies.size()));
    bench.bitrate_efficiency=target_kbps>0
        ? (static_cast<float>(total_bytes*8)/1000.0f/elapsed)/static_cast<float>(target_kbps)
        : 1.0f;
    return bench;
}

// ─── JSON cache serialization ─────────────────────────────────────────────────
static nlohmann::json bench_to_json(const EncoderBenchmark& b) {
    return {{"backend",b.backend},{"available",b.available},
            {"encode_fps",b.encode_fps},{"latency_ms",b.latency_ms},
            {"latency_jitter_ms",b.latency_jitter_ms},
            {"bitrate_efficiency",b.bitrate_efficiency}};
}
static EncoderBenchmark bench_from_json(const nlohmann::json& j) {
    EncoderBenchmark b;
    b.backend           = j.value("backend","x264");
    b.available         = j.value("available",false);
    b.encode_fps        = j.value("encode_fps",0.0f);
    b.latency_ms        = j.value("latency_ms",0.0f);
    b.latency_jitter_ms = j.value("latency_jitter_ms",0.0f);
    b.bitrate_efficiency= j.value("bitrate_efficiency",1.0f);
    return b;
}

static bool save_cache(const std::string& path, const ServerProfile& p) {
    try {
        nlohmann::json j;
        j["timestamp"] = static_cast<int64_t>(std::time(nullptr));
        j["hardware"]["cpu_cores"]           = p.hardware.cpu_cores;
        j["hardware"]["cpu_freq_ghz"]        = p.hardware.cpu_freq_ghz;
        j["hardware"]["gpu_vram_mb"]         = p.hardware.gpu_vram_mb;
        j["hardware"]["available_memory_mb"] = p.hardware.available_memory_mb;
        for (const auto& b : p.encoders) j["encoders"].push_back(bench_to_json(b));
        std::ofstream f(path);
        if (!f) return false;
        f << j.dump(2);
        return true;
    } catch (...) { return false; }
}

static bool load_cache(const std::string& path, int ttl_s, ServerProfile& out) {
    try {
        std::ifstream f(path);
        if (!f) return false;
        nlohmann::json j; f >> j;
        // TTL check
        int64_t ts = j.value("timestamp", (int64_t)0);
        if (std::time(nullptr) - ts > ttl_s) return false;
        // Hardware
        if (j.contains("hardware")) {
            auto& h = j["hardware"];
            out.hardware.cpu_cores           = h.value("cpu_cores",0);
            out.hardware.cpu_freq_ghz        = h.value("cpu_freq_ghz",0.0f);
            out.hardware.gpu_vram_mb         = h.value("gpu_vram_mb",0);
            out.hardware.available_memory_mb = h.value("available_memory_mb",0);
        }
        // Encoders
        if (j.contains("encoders")) {
            for (const auto& e : j["encoders"])
                out.encoders.push_back(bench_from_json(e));
        }
        return !out.encoders.empty();
    } catch (...) { return false; }
}
} // namespace

// ─── run_profiler ─────────────────────────────────────────────────────────────
ServerProfile run_profiler(const ServerConfig& cfg) {
    ServerProfile profile;
    profile.hardware = collect_hardware();

    if (!cfg.profiler.enabled) {
        auto add = [&](const std::string& name, bool avail) {
            EncoderBenchmark b; b.backend=name; b.available=avail;
            b.encode_fps=avail?60.0f:0.0f; b.latency_ms=avail?5.0f:0.0f;
            b.latency_jitter_ms=0.5f; b.bitrate_efficiency=1.0f;
            profile.encoders.push_back(b);
        };
        add("nvenc", pulsar::encoder::nvenc::nvenc_is_available());
        add("vaapi", pulsar::encoder::vaapi::vaapi_is_available());
        add("x264",  true);
        return profile;
    }

    // Try disk cache first
    if (cfg.profiler.cache_result) {
        if (load_cache(cfg.profiler.cache_path, cfg.profiler.cache_ttl_seconds, profile)) {
            std::cerr << "[profiler] using cached profile (age < "
                      << cfg.profiler.cache_ttl_seconds << "s)\n";
            return profile;
        }
    }

    const int   frames      = cfg.profiler.benchmark_frames;
    const int   target_kbps = 8000;
    std::cerr << "[profiler] benchmarking encoders (" << frames << " frames each)...\n";

    { pulsar::encoder::nvenc::NvencEncoder e("h264");
      auto b=benchmark_encoder(e,"nvenc",frames,target_kbps);
      std::cerr<<"[profiler] nvenc: fps="<<b.encode_fps<<" lat="<<b.latency_ms<<"ms\n";
      profile.encoders.push_back(b); }

    { pulsar::encoder::vaapi::VaapiEncoder e("h264");
      auto b=benchmark_encoder(e,"vaapi",frames,target_kbps);
      std::cerr<<"[profiler] vaapi: fps="<<b.encode_fps<<" lat="<<b.latency_ms<<"ms\n";
      profile.encoders.push_back(b); }

    { pulsar::encoder::x264::X264Encoder e;
      auto b=benchmark_encoder(e,"x264",frames,target_kbps);
      std::cerr<<"[profiler] x264:  fps="<<b.encode_fps<<" lat="<<b.latency_ms<<"ms\n";
      profile.encoders.push_back(b); }

    if (cfg.profiler.cache_result)
        if (save_cache(cfg.profiler.cache_path, profile))
            std::cerr<<"[profiler] profile cached to "<<cfg.profiler.cache_path<<"\n";

    return profile;
}

} // namespace pulsar::server
