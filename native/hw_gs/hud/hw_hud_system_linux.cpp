// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-074 (Linux): hw_hud_system.hpp without the Windows APIs of hw_hud_system.cpp. The same results, from Linux sources:
//   - the font: the system's monospaced font (fontconfig "monospace"; Windows uses Consolas), drawn by FreeType into the
//     same glyph atlas, with the same rule for the cell (each glyph drawn apart with room around it, the cell as wide as
//     the ink of all glyphs);
//   - the game's processor share: process CPU time (CLOCK_PROCESS_CPUTIME_ID) over wall time and all hardware threads;
//   - its memory: the resident set (/proc/self/statm), the counterpart of the Windows working set;
//   - its graphics card use: the DRM client statistics the kernel keeps per open GPU file (/proc/self/fdinfo/*,
//     drm-engine-<name> in nanoseconds and drm-memory-vram / drm-resident-vram), counted once per DRM client: the
//     busiest engine's share of the interval, and the video memory of the game's clients, as Task Manager shows them;
//   - the refresh rate: native/display_refresh.hpp, which the Linux window host fills from SDL3.
// A value a driver does not report stays unknown (-1, the HUD shows "--").
#include "hw_hud_system.hpp"

#include "../../display_refresh.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace gt4::hw::hud {
namespace {

std::uint64_t clock_ns(clockid_t which) {
  timespec time{};
  if (clock_gettime(which, &time) != 0) return 0;
  return static_cast<std::uint64_t>(time.tv_sec) * 1000000000u + static_cast<std::uint64_t>(time.tv_nsec);
}

// The path of the system's monospaced font, empty when fontconfig finds none.
std::string monospaced_font_file() {
  if (!FcInit()) return {};
  std::string path;
  FcPattern *pattern = FcNameParse(reinterpret_cast<const FcChar8 *>("monospace:style=Regular"));
  if (pattern != nullptr) {
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern *match = FcFontMatch(nullptr, pattern, &result);
    if (match != nullptr) {
      FcChar8 *file = nullptr;
      if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr)
        path = reinterpret_cast<const char *>(file);
      FcPatternDestroy(match);
    }
    FcPatternDestroy(pattern);
  }
  return path;
}

// The DRM statistics of this process's open GPU files, one entry per DRM client (several files may share a client).
struct DrmUsage {
  std::map<std::string, std::uint64_t> engine_ns;  // busy time per engine name, summed over the clients
  std::uint64_t vram_kib = 0;
  bool engines_known = false, vram_known = false;
};

DrmUsage read_drm_usage() {
  DrmUsage usage;
  std::set<std::string> clients;
  DIR *folder = opendir("/proc/self/fdinfo");
  if (folder == nullptr) return usage;
  while (dirent *entry = readdir(folder)) {
    if (entry->d_name[0] == '.') continue;
    std::ifstream file(std::string("/proc/self/fdinfo/") + entry->d_name);
    std::string line, client;
    std::map<std::string, std::uint64_t> engines;
    std::uint64_t vram = 0;
    bool drm = false, has_vram = false;
    while (std::getline(file, line)) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      const std::string key = line.substr(0, colon);
      std::istringstream value(line.substr(colon + 1));
      if (key == "drm-driver") drm = true;
      else if (key == "drm-client-id") value >> client;
      else if (key.rfind("drm-engine-", 0) == 0 && key.find("capacity") == std::string::npos) {
        std::uint64_t ns = 0;
        if (value >> ns) engines[key.substr(11)] += ns;
      } else if (key == "drm-memory-vram" || key == "drm-resident-vram") {
        std::uint64_t amount = 0;
        std::string unit;
        if (value >> amount) {
          value >> unit;
          const std::uint64_t kib = unit == "MiB" ? amount * 1024 : unit == "KiB" ? amount : amount / 1024;
          vram = std::max(vram, kib);  // the two keys name the same memory on drivers that give both
          has_vram = true;
        }
      }
    }
    if (!drm || client.empty() || !clients.insert(client).second) continue;
    for (const auto &[name, ns] : engines) usage.engine_ns[name] += ns;
    usage.engines_known = usage.engines_known || !engines.empty();
    if (has_vram) {
      usage.vram_kib += vram;
      usage.vram_known = true;
    }
  }
  closedir(folder);
  return usage;
}

double resident_mb() {
  std::ifstream statm("/proc/self/statm");
  long pages = 0, resident = 0;
  if (!(statm >> pages >> resident)) return -1.0;
  return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
}

}  // namespace

GlyphAtlas make_glyph_atlas(int pixel_height) {
  GlyphAtlas atlas;
  const std::string font_file = monospaced_font_file();
  FT_Library library = nullptr;
  if (font_file.empty() || FT_Init_FreeType(&library) != 0) return atlas;
  FT_Face face = nullptr;
  if (FT_New_Face(library, font_file.c_str(), 0, &face) != 0 || FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_height)) != 0) {
    if (face != nullptr) FT_Done_Face(face);
    FT_Done_FreeType(library);
    return atlas;
  }
  constexpr int glyphs = GlyphAtlas::kLast - GlyphAtlas::kFirst + 1;
  // The Windows text metrics: tmHeight is ascent plus descent, tmAveCharWidth the advance of a monospaced glyph.
  const int ascent = static_cast<int>((face->size->metrics.ascender + 63) >> 6);
  const int descent = static_cast<int>((-face->size->metrics.descender + 63) >> 6);
  const int advance = static_cast<int>((face->size->metrics.max_advance + 63) >> 6);
  const int cell_height = ascent + descent;
  if (advance > 0 && cell_height > 0) {
    // As on Windows: each glyph is drawn apart from the others, with room around it, and the cell is as wide as the ink.
    const int room = cell_height, pitch = advance + 2 * room;
    const std::size_t row = static_cast<std::size_t>(pitch) * glyphs;
    std::vector<std::uint8_t> pixels(row * static_cast<std::size_t>(cell_height), 0);
    for (int i = 0; i < glyphs; ++i) {
      if (FT_Load_Char(face, static_cast<FT_ULong>(GlyphAtlas::kFirst + i), FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) continue;
      const FT_Bitmap &bitmap = face->glyph->bitmap;
      const int left = i * pitch + room + face->glyph->bitmap_left, top = ascent - face->glyph->bitmap_top;
      for (unsigned y = 0; y < bitmap.rows; ++y) {
        for (unsigned x = 0; x < bitmap.width; ++x) {
          const int px = left + static_cast<int>(x), py = top + static_cast<int>(y);
          if (px < i * pitch || px >= (i + 1) * pitch || py < 0 || py >= cell_height) continue;
          std::uint8_t &out = pixels[static_cast<std::size_t>(py) * row + static_cast<std::size_t>(px)];
          out = std::max(out, bitmap.buffer[static_cast<std::size_t>(y) * static_cast<std::size_t>(bitmap.pitch) + x]);
        }
      }
    }
    const auto ink_at = [&](int glyph, int x, int y) {
      return pixels[static_cast<std::size_t>(y) * row + static_cast<std::size_t>(glyph * pitch + x)];
    };
    int first_ink = room, last_ink = room + advance - 1;
    for (int i = 0; i < glyphs; ++i) {
      for (int y = 0; y < cell_height; ++y) {
        for (int x = 0; x < pitch; ++x) {
          if (ink_at(i, x, y) != 0) {
            first_ink = std::min(first_ink, x);
            last_ink = std::max(last_ink, x);
          }
        }
      }
    }
    const int cell_width = last_ink - first_ink + 1;
    atlas.cell_width = cell_width;
    atlas.cell_height = cell_height;
    atlas.coverage.resize(static_cast<std::size_t>(cell_width) * glyphs * cell_height);
    std::size_t out = 0;
    for (int y = 0; y < cell_height; ++y) {
      for (int i = 0; i < glyphs; ++i) {
        for (int x = 0; x < cell_width; ++x) atlas.coverage[out++] = ink_at(i, first_ink + x, y);
      }
    }
  }
  FT_Done_Face(face);
  FT_Done_FreeType(library);
  return atlas;
}

SystemSampler::SystemSampler(void *window, std::function<void(const SystemReading &)> publish)
    : window_(window), publish_(std::move(publish)), thread_([this] { run(); }) {}

SystemSampler::~SystemSampler() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  stop_changed_.notify_all();
  thread_.join();
}

void SystemSampler::run() {
  // The HUD must not take time from the game: a lower priority for this thread only (on Linux, nice applies per thread).
  setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 5);
  (void)window_;  // the refresh rate comes from native/display_refresh.hpp
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  auto last_time = std::chrono::steady_clock::now();
  std::uint64_t last_cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
  DrmUsage last_drm = read_drm_usage();
  std::uint64_t readings = 0;
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_changed_.wait_for(lock, std::chrono::milliseconds(kPeriodMs), [this] { return stop_; })) {
    lock.unlock();
    SystemReading reading;
    const auto now = std::chrono::steady_clock::now();
    const double wall_ns = std::chrono::duration<double, std::nano>(now - last_time).count();
    const std::uint64_t cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    if (wall_ns > 0.0) reading.cpu_percent = std::min(100.0, static_cast<double>(cpu - last_cpu) / (wall_ns * threads) * 100.0);
    reading.memory_mb = resident_mb();
    const DrmUsage drm = read_drm_usage();
    if (drm.engines_known && last_drm.engines_known && wall_ns > 0.0) {
      double busiest = 0.0;
      for (const auto &[name, ns] : drm.engine_ns) {
        const auto before = last_drm.engine_ns.find(name);
        if (before != last_drm.engine_ns.end() && ns >= before->second)
          busiest = std::max(busiest, static_cast<double>(ns - before->second) / wall_ns * 100.0);
      }
      reading.gpu_percent = std::min(busiest, 100.0);
    }
    if (drm.vram_known) reading.video_memory_mb = static_cast<double>(drm.vram_kib) / 1024.0;
    reading.refresh_hz = gt4::display::g_refresh_hz.load(std::memory_order_relaxed);
    last_time = now;
    last_cpu = cpu;
    last_drm = drm;
    publish_(reading);
    // In the log every 40 readings, as on Windows (the game may end without destructors).
    if (++readings % 40 == 0) {
      std::fprintf(stderr, "[hud] sampler: %llu readings, %.2f ms of processor per reading\n",
                   static_cast<unsigned long long>(readings),
                   static_cast<double>(clock_ns(CLOCK_THREAD_CPUTIME_ID)) / 1e6 / static_cast<double>(readings));
    }
    lock.lock();
  }
}

}  // namespace gt4::hw::hud
