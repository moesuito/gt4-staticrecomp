#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace gt4recomp::tools {

// Read and hash-check the one USA v2.00 CORE accepted by the native tools.
[[nodiscard]] std::vector<std::uint8_t> read_verified_core(const std::filesystem::path& path);

// The pinned USA v2.00 CORE SHA-256 hex the reader enforces, for checkpoint
// provenance: recording it states what the writer verified, not a fresh
// measurement.
[[nodiscard]] const char* pinned_core_sha256() noexcept;

} // namespace gt4recomp::tools
