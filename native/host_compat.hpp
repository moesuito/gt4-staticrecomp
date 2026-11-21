#pragma once

// TASK-074 (Linux build, plan step L1: host-only part): POSIX stand-ins for the few MSVC CRT and Win32 calls that the otherwise
// platform-neutral host modules and fixtures use. On Windows this header defines nothing, so the qualified Windows
// build compiles exactly the code it compiled before. Each stand-in keeps the documented Windows semantics that the
// callers rely on; anything beyond that is out of scope here.

#ifndef _WIN32

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>  // also provides _exit, the same function as the MSVC CRT's

// Win32 DWORD: an unsigned 32-bit value.
using DWORD = unsigned int;

// MSVC _putenv_s: sets NAME to VALUE; an empty VALUE removes the variable. Returns 0 or an errno value.
inline int _putenv_s(const char *name, const char *value) {
  if (value == nullptr || value[0] == '\0')
    return unsetenv(name) == 0 ? 0 : errno;
  return setenv(name, value, 1) == 0 ? 0 : errno;
}

// MSVC _getpid: the process id.
inline int _getpid() { return static_cast<int>(getpid()); }

// MSVC tmpfile_s: opens a temporary binary file that is removed when closed. Returns 0 or an errno value.
inline int tmpfile_s(std::FILE **stream) {
  if (stream == nullptr) return EINVAL;
  *stream = std::tmpfile();
  return *stream != nullptr ? 0 : errno;
}

// MSVC fopen_s: std::fopen with the result in *STREAM. Returns 0 or an errno value.
inline int fopen_s(std::FILE **stream, const char *path, const char *mode) {
  if (stream == nullptr) return EINVAL;
  *stream = std::fopen(path, mode);
  return *stream != nullptr ? 0 : errno;
}

// Win32 GetEnvironmentVariableA: copies NAME's value into BUFFER (SIZE bytes, including the terminator).
// Returns the value's length without the terminator when it fits; the size needed, terminator included, when it does
// not fit (BUFFER is then left untouched); 0 when the variable is not set.
inline DWORD GetEnvironmentVariableA(const char *name, char *buffer, DWORD size) {
  const char *value = std::getenv(name);
  if (value == nullptr) return 0;
  const std::size_t length = std::strlen(value);
  if (buffer == nullptr || length + 1 > size) return static_cast<DWORD>(length + 1);
  std::memcpy(buffer, value, length + 1);
  return static_cast<DWORD>(length);
}

#endif // !_WIN32
