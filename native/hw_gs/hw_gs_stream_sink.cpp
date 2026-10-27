// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from the TASK-026 specification (docs/hw-renderer-spec), clean room per decision 0070.
// Chapter 12.2/12.3 hookup on main's TASK-035 seam: GT4_RENDERER=hw feeds the GS stream hooks to the HWR backend
// in the game process (native/gs_stream/gs_stream_sink.hpp). Replaces the M0b gs.cpp splice of task/027-hw-renderer
// (native/hw_gs_frontend.inc and the paraLLEl-GS dispatch, dropped with paraLLEl-GS by decision 0070).
//
// The backend receives every GIF chunk, privileged write and VSYNC, draws through the GPU write-back renderer (M4/M5)
// and presents the composed display with the VBlank's field (M6: 13.3 field offset, 13.4 deinterlace). Local-to-host
// reads are answered from the shadow local memory after the open batch is flushed (10.2).

#include "gs_stream/gs_stream_sink.hpp"

#include "hw_gs_backend.hpp"
#include "hw_gs_options.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gt4::hw {
namespace {

Backend *g_backend = nullptr;
bool g_odd_field = false;
std::uint64_t g_vsyncs = 0;
std::uint64_t g_unmirrored_privileged = 0;
// GT4_HW_PROFILE_EVERY state (see maybe_profile).
std::uint32_t g_profile_every = 0;
RenderCounters g_profile_last;
std::chrono::steady_clock::time_point g_profile_wall;

// M6 frame dumps (chapter 12.9 asks for them; the donor renderer cannot provide them in this mode).
// GT4_HW_DUMP_DIR=<folder> writes P6 PPM files, GT4_HW_DUMP_EVERY=<n> the interval in VBlanks (default
// 100). The image is the composed scanout of the GPU write-back path (Backend::scanout). The RAW VRAM
// buffer decode of chapter 12.9 stays with the CPU reference tool, which owns that oracle (IN-325).
std::string g_dump_dir;
std::uint32_t g_dump_every = 0;
std::uint64_t g_dump_from = 0;  // GT4_HW_DUMP_FROM=<vblank>: no dumps before it (consecutive frames of one scene)
std::uint64_t g_dumps = 0;
std::uint64_t g_dumps_without_image = 0;

void read_dump_options() {
  const char *dir = std::getenv("GT4_HW_DUMP_DIR");
  if (dir && *dir) {
    g_dump_dir = dir;
    g_dump_every = 100;
    if (const char *every = std::getenv("GT4_HW_DUMP_EVERY")) {
      const long value = std::strtol(every, nullptr, 10);
      g_dump_every = value > 0 ? static_cast<std::uint32_t>(value) : 100u;
    }
    if (const char *from = std::getenv("GT4_HW_DUMP_FROM")) g_dump_from = std::strtoull(from, nullptr, 10);
    std::fprintf(stderr, "[hw] frame dumps: %s every %u VBlanks from %llu (composed scanout image)\n",
                 g_dump_dir.c_str(), g_dump_every, static_cast<unsigned long long>(g_dump_from));
  }
}

// One P6 PPM of the composed scanout image. Returns false when there was no image this VBlank.
bool write_dump(Backend &hw, std::uint64_t index) {
  ScanoutRequest request;
  if (!hw.scanout(request) || request.width == 0 || request.height == 0) return false;
  char name[64];
  std::snprintf(name, sizeof(name), "frame_%05llu.ppm", static_cast<unsigned long long>(index));
  const std::string path = g_dump_dir + "\\" + name;
  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (!file) return false;
  std::fprintf(file, "P6\n%u %u\n255\n", request.width, request.height);
  for (std::uint32_t pixel : request.pixels) {
    const unsigned char rgb[3] = {static_cast<unsigned char>(pixel & 0xFFu),
                                  static_cast<unsigned char>((pixel >> 8) & 0xFFu),
                                  static_cast<unsigned char>((pixel >> 16) & 0xFFu)};
    std::fwrite(rgb, 1, 3, file);
  }
  std::fclose(file);
  return true;
}

void maybe_dump(Backend &hw) {
  if (g_dump_every == 0) return;
  if (g_vsyncs < g_dump_from || g_vsyncs % g_dump_every != 0) return;
  if (write_dump(hw, g_vsyncs)) {
    ++g_dumps;
  } else {
    ++g_dumps_without_image;
  }
}

void start_gs_worker();

bool start_backend(std::string &error) {
  Options options;
  try {
    options = parse_options([](const char *name) -> const char * { return std::getenv(name); });
  } catch (const OptionError &problem) {
    error = problem.what();
    return false;
  }
  read_dump_options();
  if (const char *every = std::getenv("GT4_HW_PROFILE_EVERY")) {
    const long value = std::strtol(every, nullptr, 10);
    g_profile_every = value > 0 ? static_cast<std::uint32_t>(value) : 0u;
    g_profile_wall = std::chrono::steady_clock::now();
  }
  // Created once and never destroyed: the Vulkan device must not be torn down during process exit.
  auto *created = new Backend;
  if (!created->init(options, error)) return false;
  g_backend = created;
  std::fprintf(stderr, "[hw] GT4_RENDERER=hw: HWR backend started in process, no GS process and no donor Vulkan renderer\n");
  start_gs_worker();
  for (const std::string &line : describe_options(options)) std::fprintf(stderr, "[hw] %s\n", line.c_str());
  return true;
}

Backend &backend() {
  if (!g_backend) {
    std::fprintf(stderr, "[hw] HWR backend used before start; stopping\n");
    std::exit(EXIT_FAILURE);
  }
  return *g_backend;
}

void gif_now(int path, const std::uint8_t *data, std::size_t bytes) { backend().gif_transfer(path, data, bytes); }

void privileged_now(std::uint32_t address, std::uint64_t value) {
  if (!backend().write_privileged(address, value)) ++g_unmirrored_privileged;
}

void read_fifo_now(std::uint8_t *data, std::uint32_t qwords) {
  const std::size_t filled = backend().read_transfer_fifo(data, qwords);
  if (filled < qwords) std::memset(data + filled * 16, 0, (static_cast<std::size_t>(qwords) - filled) * 16);
}

// Once per guest VBlank: submit the recorded work, then present (window) or close the frame (headless).
// GT4_HW_PROFILE_EVERY=<n>: every n VBlanks, one stderr line with the interval's per-frame averages (CPU ms per
// pass and work counts), so heavy scenes (3D cars, races) can be profiled from a headless run (M9 work).

void maybe_profile(Backend &hw) {
  if (g_profile_every == 0 || g_vsyncs % g_profile_every != 0) return;
  const RenderCounters now = hw.render_counters();
  const RenderCounters &was = g_profile_last;
  const double n = static_cast<double>(g_profile_every);
  const auto wall = std::chrono::steady_clock::now();
  const double wallMs = std::chrono::duration<double, std::milli>(wall - g_profile_wall).count() / n;
  std::fprintf(stderr,
               "[hw] profile vsync=%llu wall_ms=%.2f gif_ms=%.2f upload_ms=%.2f translate_ms=%.2f execute_ms=%.2f "
               "decode_ms=%.2f "
               "writeback_ms=%.2f load_ms=%.2f batches=%.1f prims=%.1f draws=%.1f chunks=%.1f textures=%.1f "
               "cached=%.1f texels=%.0f writebacks=%.2f loads=%.2f submissions=%.2f barriers=%.1f "
               "wb_other=%.2f wb_texture=%.2f wb_clut=%.2f wb_transfer=%.2f wb_display=%.2f wb_evict=%.2f "
               "wb_overlap=%.2f wb_fallback=%.2f wb_sprite=%.2f\n",
               static_cast<unsigned long long>(g_vsyncs), wallMs, (now.gif_us - was.gif_us) / n / 1000.0,
               (now.upload_us - was.upload_us) / n / 1000.0, (now.translate_us - was.translate_us) / n / 1000.0,
               (now.execute_us - was.execute_us) / n / 1000.0, (now.decode_us - was.decode_us) / n / 1000.0,
               (now.cache_writeback_us - was.cache_writeback_us) / n / 1000.0,
               (now.cache_load_us - was.cache_load_us) / n / 1000.0, static_cast<double>(now.batches - was.batches) / n,
               static_cast<double>(now.primitives - was.primitives) / n,
               static_cast<double>(now.gpu_draws - was.gpu_draws) / n, static_cast<double>(now.gpu_chunks - was.gpu_chunks) / n,
               static_cast<double>(now.textures - was.textures) / n,
               static_cast<double>(now.textures_cached - was.textures_cached) / n,
               static_cast<double>(now.texels - was.texels) / n,
               static_cast<double>(now.cache_writebacks - was.cache_writebacks) / n,
               static_cast<double>(now.cache_loads - was.cache_loads) / n,
               static_cast<double>(now.gpu_submissions - was.gpu_submissions) / n,
               static_cast<double>(now.gpu_barriers - was.gpu_barriers) / n,
               static_cast<double>(now.writeback_reasons[0] - was.writeback_reasons[0]) / n,
               static_cast<double>(now.writeback_reasons[1] - was.writeback_reasons[1]) / n,
               static_cast<double>(now.writeback_reasons[2] - was.writeback_reasons[2]) / n,
               static_cast<double>(now.writeback_reasons[3] - was.writeback_reasons[3]) / n,
               static_cast<double>(now.writeback_reasons[4] - was.writeback_reasons[4]) / n,
               static_cast<double>(now.writeback_reasons[5] - was.writeback_reasons[5]) / n,
               static_cast<double>(now.writeback_reasons[6] - was.writeback_reasons[6]) / n,
               static_cast<double>(now.writeback_reasons[7] - was.writeback_reasons[7]) / n,
               static_cast<double>(now.writeback_reasons[8] - was.writeback_reasons[8]) / n);
  if (now.gpu_busy_ns != was.gpu_busy_ns) {
    std::fprintf(stderr,
                 "[hw] gpu vsync=%llu busy_ms=%.2f pass_ms=%.2f decode_ms=%.2f clut_ms=%.2f swizzle_ms=%.2f "
                 "upload_ms=%.2f download_ms=%.2f scaled_ms=%.2f refresh_ms=%.2f early_submissions=%.2f\n",
                 static_cast<unsigned long long>(g_vsyncs), (now.gpu_busy_ns - was.gpu_busy_ns) / n / 1e6,
                 (now.gpu_ns[0] - was.gpu_ns[0]) / n / 1e6, (now.gpu_ns[1] - was.gpu_ns[1]) / n / 1e6,
                 (now.gpu_ns[2] - was.gpu_ns[2]) / n / 1e6, (now.gpu_ns[3] - was.gpu_ns[3]) / n / 1e6,
                 (now.gpu_ns[4] - was.gpu_ns[4]) / n / 1e6, (now.gpu_ns[5] - was.gpu_ns[5]) / n / 1e6,
                 (now.gpu_ns[6] - was.gpu_ns[6]) / n / 1e6, (now.gpu_ns[7] - was.gpu_ns[7]) / n / 1e6,
                 static_cast<double>(now.gpu_early_submissions - was.gpu_early_submissions) / n);
  }
  // IN-367 efficiency line (owner principle 2026-10-22: measure cost, not only FPS): CPU time of the thread that
  // runs the GS stream (the GS worker, or the EE thread with GT4_HW_GS_THREAD=0) and of the whole process (all
  // threads: EE, VU1 worker, GS worker, audio, driver), in ms per frame; render passes, barriers and bytes moved
  // between CPU and GPU per frame.
  {
    static double lastThreadMs = 0.0;
    static double lastProcessMs = 0.0;
    static bool primed = false;
    auto filetime_ms = [](const FILETIME &kernel, const FILETIME &user) {
      const auto ticks = [](const FILETIME &t) {
        return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
      };
      return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;  // 100 ns units
    };
    FILETIME created{}, exited{}, kernel{}, user{};
    double threadMs = 0.0, processMs = 0.0;
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) threadMs = filetime_ms(kernel, user);
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) processMs = filetime_ms(kernel, user);
    if (primed) {
      std::fprintf(stderr,
                   "[hw] cost vsync=%llu gs_thread_cpu_ms=%.2f process_cpu_core_ms=%.2f render_passes=%.1f "
                   "barriers=%.1f upload_mb=%.2f readback_mb=%.3f scaled_image_mb=%.1f\n",
                   static_cast<unsigned long long>(g_vsyncs), (threadMs - lastThreadMs) / n,
                   (processMs - lastProcessMs) / n,
                   static_cast<double>(now.gpu_render_passes - was.gpu_render_passes) / n,
                   static_cast<double>(now.gpu_barriers - was.gpu_barriers) / n,
                   static_cast<double>(now.gpu_upload_bytes - was.gpu_upload_bytes) / n / 1e6,
                   static_cast<double>(now.gpu_readback_bytes - was.gpu_readback_bytes) / n / 1e6,
                   static_cast<double>(now.gpu_scaled_image_bytes) / 1e6);
      // IN-384 follow-up (owner test 11, sporadic 1x frames): why a VBlank fell back to the CPU 1x presentation.
      // 0 GpuComposeApplies declined, 1 zero output size, 2 interlaced frame larger than the deinterlace images,
      // 3 the GPU compose or its flush failed.
      const unsigned long long skips[4] = {now.gpu_compose_skips[0] - was.gpu_compose_skips[0],
                                           now.gpu_compose_skips[1] - was.gpu_compose_skips[1],
                                           now.gpu_compose_skips[2] - was.gpu_compose_skips[2],
                                           now.gpu_compose_skips[3] - was.gpu_compose_skips[3]};
      if (skips[0] + skips[1] + skips[2] + skips[3] != 0) {
        std::fprintf(stderr, "[hw] compose_skips applies=%llu zero_size=%llu too_big=%llu failed=%llu\n", skips[0],
                     skips[1], skips[2], skips[3]);
      }
    }
    primed = true;
    lastThreadMs = threadMs;
    lastProcessMs = processMs;
  }
  g_profile_last = now;
  g_profile_wall = wall;
}

void vsync_now(bool odd_field) {
  Backend &hw = backend();
  g_odd_field = odd_field;
  ++g_vsyncs;
  hw.set_display_field(odd_field);
  hw.flush();
  hw.next_frame();  // one guest frame per VBlank, windowed or headless (the session line's `frames`)
  if (hw.has_window()) hw.present(odd_field ? PresentPhase::field_odd : PresentPhase::field_even);
  maybe_dump(hw);
  maybe_profile(hw);
}

void gs_reset_now() {
  Backend &hw = backend();
  hw.reset_context();
  hw.reset_gif_paths();
  hw.clear_vram();
}

void gif_reset_now() { backend().reset_gif_paths(); }

// ---- GS worker thread (IN-363) ----
// The game's EE thread used to run the whole GS stream (parse, translate, record, submit, compose, present)
// synchronously. Now it queues the stream, in order, to one worker thread, which runs exactly the calls above in
// the same order, so results are unchanged; the EE continues while the GS work of the frame runs. The calls that
// return data or change the window (LOCAL->HOST FIFO reads, window attach and resize, the end of the session)
// first wait until the worker has executed everything queued before them, then run under the lock the worker
// holds for each command. At a VBlank the EE waits while kFramesAhead VBlanks are still queued (back-pressure).
// GT4_HW_GS_THREAD=0 keeps the synchronous path (diagnostic switch, not a 12.8 option).
struct GsCommand {
  enum class Kind : std::uint8_t { Gif, Privileged, Vsync, GsReset, GifReset } kind = Kind::Gif;
  int path = 0;
  std::vector<std::uint8_t> bytes;
  std::uint32_t address = 0;
  std::uint64_t value = 0;
  bool oddField = false;
};

class GsWorker {
public:
  static constexpr std::size_t kFramesAhead = 2;

  void start() {
    thread_ = std::thread([this] { run(); });
  }

  void push(GsCommand &&command) {
    std::unique_lock<std::mutex> lock(queueMutex_);
    if (command.kind == GsCommand::Kind::Vsync) {
      canQueue_.wait(lock, [this] { return queuedVsyncs_ < kFramesAhead; });
      ++queuedVsyncs_;
    }
    queue_.push_back(std::move(command));
    hasWork_.notify_one();
  }

  // A recycled byte buffer for a GIF command (no allocation per packet once warm).
  std::vector<std::uint8_t> buffer() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (spare_.empty()) return {};
    std::vector<std::uint8_t> bytes = std::move(spare_.back());
    spare_.pop_back();
    return bytes;
  }

  // Waits until every queued command has executed; the caller then runs with the backend lock held.
  std::unique_lock<std::mutex> drain() {
    std::unique_lock<std::mutex> lock(queueMutex_);
    drained_.wait(lock, [this] { return queue_.empty() && !busy_; });
    lock.unlock();
    return std::unique_lock<std::mutex>(backendMutex_);
  }

private:
  void run() {
    for (;;) {
      GsCommand command;
      {
        std::unique_lock<std::mutex> lock(queueMutex_);
        hasWork_.wait(lock, [this] { return !queue_.empty(); });
        command = std::move(queue_.front());
        queue_.pop_front();
        busy_ = true;
      }
      {
        std::lock_guard<std::mutex> backendLock(backendMutex_);
        switch (command.kind) {
          case GsCommand::Kind::Gif: gif_now(command.path, command.bytes.data(), command.bytes.size()); break;
          case GsCommand::Kind::Privileged: privileged_now(command.address, command.value); break;
          case GsCommand::Kind::Vsync: vsync_now(command.oddField); break;
          case GsCommand::Kind::GsReset: gs_reset_now(); break;
          case GsCommand::Kind::GifReset: gif_reset_now(); break;
        }
      }
      {
        std::lock_guard<std::mutex> lock(queueMutex_);
        busy_ = false;
        if (command.kind == GsCommand::Kind::Vsync) {
          --queuedVsyncs_;
          canQueue_.notify_all();
        }
        if (command.kind == GsCommand::Kind::Gif && spare_.size() < 4096) {
          command.bytes.clear();
          spare_.push_back(std::move(command.bytes));
        }
        if (queue_.empty()) drained_.notify_all();
      }
    }
  }

  std::thread thread_;
  std::mutex queueMutex_;
  std::mutex backendMutex_;
  std::condition_variable hasWork_;
  std::condition_variable canQueue_;
  std::condition_variable drained_;
  std::deque<GsCommand> queue_;
  std::vector<std::vector<std::uint8_t>> spare_;
  std::size_t queuedVsyncs_ = 0;
  bool busy_ = false;
};

GsWorker *g_worker = nullptr;  // null: synchronous (GT4_HW_GS_THREAD=0)

void start_gs_worker() {
  const char *thread = std::getenv("GT4_HW_GS_THREAD");
  if (!(thread && std::string(thread) == "0")) {
    g_worker = new GsWorker;  // never destroyed, like the backend
    g_worker->start();
    std::fprintf(stderr, "[hw] GS stream on its own worker thread (IN-363; GT4_HW_GS_THREAD=0 for the EE thread)\n");
  }
}

void gif(int path, const std::uint8_t *data, std::uint32_t qwc) {
  const std::size_t bytes = static_cast<std::size_t>(qwc) * 16;
  if (!g_worker) {
    gif_now(path, data, bytes);
    return;
  }
  GsCommand command;
  command.kind = GsCommand::Kind::Gif;
  command.path = path;
  command.bytes = g_worker->buffer();
  command.bytes.assign(data, data + bytes);
  g_worker->push(std::move(command));
}

void privileged(std::uint32_t address, std::uint64_t value) {
  if (!g_worker) {
    privileged_now(address, value);
    return;
  }
  GsCommand command;
  command.kind = GsCommand::Kind::Privileged;
  command.address = address;
  command.value = value;
  g_worker->push(std::move(command));
}

void read_fifo(std::uint8_t *data, std::uint32_t qwords) {
  if (!g_worker) {
    read_fifo_now(data, qwords);
    return;
  }
  const std::unique_lock<std::mutex> lock = g_worker->drain();  // the read observes everything queued before it
  read_fifo_now(data, qwords);
}

void vsync(bool odd_field) {
  if (!g_worker) {
    vsync_now(odd_field);
    return;
  }
  GsCommand command;
  command.kind = GsCommand::Kind::Vsync;
  command.oddField = odd_field;
  g_worker->push(std::move(command));
}

void gs_reset() {
  if (!g_worker) {
    gs_reset_now();
    return;
  }
  GsCommand command;
  command.kind = GsCommand::Kind::GsReset;
  g_worker->push(std::move(command));
}

void gif_reset() {
  if (!g_worker) {
    gif_reset_now();
    return;
  }
  GsCommand command;
  command.kind = GsCommand::Kind::GifReset;
  g_worker->push(std::move(command));
}

bool attach_window(std::uint64_t window, std::uint32_t width, std::uint32_t height) {
  std::unique_lock<std::mutex> lock;
  if (g_worker) lock = g_worker->drain();
  std::string error;
  if (!backend().attach_window(reinterpret_cast<void *>(window), error)) {
    std::fprintf(stderr, "[hw] window presentation refused: %s\n", error.c_str());
    return false;
  }
  backend().resize_window(width, height);
  std::fprintf(stderr, "[hw] window output through the HWR swapchain\n");
  return true;
}

// TASK-080: the race view's display-aspect fill (protocol 1.3 CONTROL display_aspect in pcsx2 mode). Called on the EE
// thread only when the request changes; the backend reads it atomically at its next present, so no drain is needed.
std::uint64_t g_display_aspect_changes = 0;
void display_aspect(bool fill, std::uint32_t ratio_millionths) {
  backend().set_display_aspect(fill, ratio_millionths);
  ++g_display_aspect_changes;
  if (!fill) {
    std::fprintf(stderr, "[hw] presentation: 16:9 pillarbox\n");
  } else if (ratio_millionths == 0) {
    std::fprintf(stderr, "[hw] presentation: fill the whole window\n");
  } else {
    std::fprintf(stderr, "[hw] presentation: fill at ratio %.4f\n", ratio_millionths / 1e6);
  }
}

void resize_window(std::uint32_t width, std::uint32_t height) {
  std::unique_lock<std::mutex> lock;
  if (g_worker) lock = g_worker->drain();
  backend().resize_window(width, height);
}

RenderCounters stats_counters() { return g_backend ? g_backend->render_counters() : RenderCounters{}; }

void finish(std::FILE *output) {
  if (!g_backend) return;
  std::unique_lock<std::mutex> lock;
  if (g_worker) lock = g_worker->drain();
  g_backend->wait_idle();
  const Stats stats = g_backend->stats();
  std::fprintf(output,
               "[hw] session: vsyncs=%llu frames=%llu presents=%llu black_presents=%llu gif_bytes=%llu "
               "privileged=%llu unmirrored_privileged=%llu faults=%llu\n",
               static_cast<unsigned long long>(g_vsyncs), static_cast<unsigned long long>(stats.frames),
               static_cast<unsigned long long>(stats.presents), static_cast<unsigned long long>(stats.black_presents),
               static_cast<unsigned long long>(stats.gif_bytes),
               static_cast<unsigned long long>(stats.privileged_writes),
               static_cast<unsigned long long>(g_unmirrored_privileged), static_cast<unsigned long long>(stats.faults));
  std::fprintf(output,
               "[hw] m6: flushes=%llu held_presents=%llu dropped_frames=%llu auxiliary_frames=%llu translate_us=%.1f "
               "execute_us=%.1f compose_us=%.1f upload_us=%.1f present_us=%.1f\n",
               static_cast<unsigned long long>(stats.flushes),
               static_cast<unsigned long long>(stats.held_presents),
               static_cast<unsigned long long>(stats.dropped_frames),
               static_cast<unsigned long long>(stats.auxiliary_frames), stats.translate_us, stats.execute_us,
               stats.compose_us, stats.upload_us, stats.present_us);
  // TASK-080: fill requests received and the rectangle of the last present (window only).
  std::fprintf(output, "[hw] presentation: aspect_changes=%llu last_rect=%u,%u %ux%u fill=%u\n",
               static_cast<unsigned long long>(g_display_aspect_changes), stats.present_left, stats.present_top,
               stats.present_width, stats.present_height, stats.present_fill);
  const RenderCounters counters = stats_counters();
  std::fprintf(output,
               "[hw] counters: batches=%llu primitives=%llu primitives_skipped_texture=%llu batches_empty=%llu "
               "textures=%llu texels=%llu gpu_draws=%llu writeback_pixels=%llu flat_deindexed=%llu sprites=%llu\n",
               static_cast<unsigned long long>(counters.batches),
               static_cast<unsigned long long>(counters.primitives),
               static_cast<unsigned long long>(counters.primitives_skipped_texture),
               static_cast<unsigned long long>(counters.batches_empty),
               static_cast<unsigned long long>(counters.textures),
               static_cast<unsigned long long>(counters.texels),
               static_cast<unsigned long long>(counters.gpu_draws),
               static_cast<unsigned long long>(counters.writeback_pixels),
               static_cast<unsigned long long>(counters.flat_deindexed),
               static_cast<unsigned long long>(counters.sprites));
  {
    const RenderCounters &video = counters;
    std::fprintf(output, "[hw] video: vblanks_480i=%llu vblanks_480p=%llu (SMODE2.INT; menus are 480i, races 480p with "
                         "GT4_FORCE_480P=1)\n",
                 static_cast<unsigned long long>(video.vblanks_interlaced),
                 static_cast<unsigned long long>(video.vblanks_progressive));
  }
  std::fprintf(output,
               "[hw] output: presentation_frames=%llu blank_frames=%llu deinterlaced_frames=%llu "
               "interlace_offset_frames=%llu merge_alpha_from_texel=%llu writeback_depth_words=%llu "
               "fifo_overruns=%llu\n",
               static_cast<unsigned long long>(counters.presentation_frames),
               static_cast<unsigned long long>(counters.blank_frames),
               static_cast<unsigned long long>(counters.deinterlaced_frames),
               static_cast<unsigned long long>(counters.interlace_offset_frames),
               static_cast<unsigned long long>(counters.merge_alpha_from_texel),
               static_cast<unsigned long long>(counters.writeback_depth_words),
               static_cast<unsigned long long>(counters.fifo_overruns));
  std::fprintf(output, "[hw] tone: strip_pages=%llu strip_batches=%llu verify_mismatch_bytes=%llu (IN-383)\n",
               static_cast<unsigned long long>(counters.tone_strip_pages),
               static_cast<unsigned long long>(counters.tone_strip_batches),
               static_cast<unsigned long long>(counters.tone_verify_mismatch_bytes));
  if (counters.scale > 1) {
    std::fprintf(output,
                 "[hw] scale: x%u draws=%llu passes=%llu refreshes=%llu refresh_pixels=%llu composes=%llu "
                 "circuits_from_slots=%llu circuits_from_glm=%llu circuits_from_shadows=%llu shadow_saves=%llu "
                 "tone_runs=%llu tone_carries=%llu\n",
                 counters.scale, static_cast<unsigned long long>(counters.scaled_draws),
                 static_cast<unsigned long long>(counters.scaled_passes),
                 static_cast<unsigned long long>(counters.scaled_refreshes),
                 static_cast<unsigned long long>(counters.scaled_refresh_pixels),
                 static_cast<unsigned long long>(counters.scaled_composes),
                 static_cast<unsigned long long>(counters.scaled_circuits_from_slots),
                 static_cast<unsigned long long>(counters.scaled_circuits_from_glm),
                 static_cast<unsigned long long>(counters.scaled_circuits_from_shadows),
                 static_cast<unsigned long long>(counters.scaled_shadow_saves),
                 static_cast<unsigned long long>(counters.tone_runs),
                 static_cast<unsigned long long>(counters.tone_carries));
  }
  if (g_dump_every != 0) {
    std::fprintf(output, "[hw] dumps: written=%llu without_image=%llu every=%u dir=%s\n",
                 static_cast<unsigned long long>(g_dumps), static_cast<unsigned long long>(g_dumps_without_image),
                 g_dump_every, g_dump_dir.c_str());
  }
  std::fflush(output);
}

}  // namespace
}  // namespace gt4::hw

namespace gt4::gs_stream {

const InProcessSink &hw_in_process_sink() {
  static const InProcessSink sink = [] {
    InProcessSink s;
    s.start = &gt4::hw::start_backend;
    s.gif = &gt4::hw::gif;
    s.privileged = &gt4::hw::privileged;
    s.read_fifo = &gt4::hw::read_fifo;
    s.vsync = &gt4::hw::vsync;
    s.gs_reset = &gt4::hw::gs_reset;
    s.gif_reset = &gt4::hw::gif_reset;
    s.attach_window = &gt4::hw::attach_window;
    s.resize_window = &gt4::hw::resize_window;
    s.display_aspect = &gt4::hw::display_aspect;
    s.finish = &gt4::hw::finish;
    return s;
  }();
  return sink;
}

}  // namespace gt4::gs_stream
