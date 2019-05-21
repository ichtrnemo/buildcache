//--------------------------------------------------------------------------------------------------
// Copyright (c) 2018 Marcus Geelnard
//
// This software is provided 'as-is', without any express or implied warranty. In no event will the
// authors be held liable for any damages arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose, including commercial
// applications, and to alter it and redistribute it freely, subject to the following restrictions:
//
//  1. The origin of this software must not be misrepresented; you must not claim that you wrote
//     the original software. If you use this software in a product, an acknowledgment in the
//     product documentation would be appreciated but is not required.
//
//  2. Altered source versions must be plainly marked as such, and must not be misrepresented as
//     being the original software.
//
//  3. This notice may not be removed or altered from any source distribution.
//--------------------------------------------------------------------------------------------------

#include <base/file_utils.hpp>

#include <base/debug_utils.hpp>
#include <base/string_list.hpp>
#include <base/unicode_utils.hpp>

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <sstream>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#include <shlobj.h>
#include <userenv.h>
#include <sys/utime.h>
#undef ERROR
#undef log
#else
#include <cstdlib>
#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>

// S_ISDIR/S_ISREG are not defined by MSVC, but _S_IFDIR/_S_IFREG are.
#if defined(_WIN32) && !defined(S_ISDIR)
#define S_ISDIR(x) (((x)&_S_IFDIR) != 0)
#endif
#if defined(_WIN32) && !defined(S_ISREG)
#define S_ISREG(x) (((x)&_S_IFREG) != 0)
#endif

namespace bcache {
namespace file {
namespace {
// Directory separator for paths.
#ifdef _WIN32
const char PATH_SEPARATOR_CHR = '\\';
#else
const char PATH_SEPARATOR_CHR = '/';
#endif
const auto PATH_SEPARATOR = std::string(1, PATH_SEPARATOR_CHR);

// Delimiter character for the PATH environment variable.
#ifdef _WIN32
const char PATH_DELIMITER_CHR = ';';
#else
const char PATH_DELIMITER_CHR = ':';
#endif
const auto PATH_DELIMITER = std::string(1, PATH_DELIMITER_CHR);

// This is a static variable that holds a strictly incrementing number used for generating unique
// temporary file names.
std::atomic_uint_fast32_t s_tmp_name_number;

int get_process_id() {
#ifdef _WIN32
  return static_cast<int>(GetCurrentProcessId());
#else
  return static_cast<int>(getpid());
#endif
}

std::string::size_type get_last_path_separator_pos(const std::string& path) {
#if defined(_WIN32)
  const auto pos1 = path.rfind("/");
  const auto pos2 = path.rfind("\\");
  std::string::size_type pos;
  if (pos1 == std::string::npos) {
    pos = pos2;
  } else if (pos2 == std::string::npos) {
    pos = pos1;
  } else {
    pos = std::max(pos1, pos2);
  }
#else
  const auto pos = path.rfind(PATH_SEPARATOR);
#endif
  return pos;
}

#ifdef _WIN32
int64_t two_dwords_to_int64(const DWORD low, const DWORD high) {
  return static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(low)) |
                              (static_cast<uint64_t>(static_cast<uint32_t>(high)) << 32));
}

file_info_t::time_t win32_filetime_as_unix_epoch(const FILETIME& file_time) {
  // The FILETIME represents the number of 100-nanosecond intervals since January 1, 1601 (UTC).
  // I.e. the Windows epoch starts 11644473600 seconds before the UNIX epoch.
  const auto t64 = two_dwords_to_int64(file_time.dwLowDateTime, file_time.dwHighDateTime);
  return static_cast<file_info_t::time_t>((t64 / 10000000L) - 11644473600L);
}
#endif

void remove_dir_internal(const std::string& path, const bool ignore_errors) {
#ifdef _WIN32
  const auto success = (_wrmdir(utf8_to_ucs2(path).c_str()) == 0);
#else
  const auto success = (rmdir(path.c_str()) == 0);
#endif
  if ((!success) && (!ignore_errors)) {
    throw std::runtime_error("Unable to remove dir.");
  }
}
}  // namespace

tmp_file_t::tmp_file_t(const std::string& dir, const std::string& extension) {
  // Get unique identifiers for this file.
  const auto pid = get_process_id();
  const auto number = ++s_tmp_name_number;

  // Generate a file name from the unique identifiers.
  std::ostringstream ss;
  ss << "bcache" << pid << "_" << number;
  std::string file_name = ss.str();

  // Concatenate base dir, file name and extension into the full path.
  m_path = append_path(dir, file_name + extension);
}

tmp_file_t::~tmp_file_t() {
  if (file_exists(m_path)) {
    try {
      remove_file(m_path);
    } catch (const std::exception& e) {
      debug::log(debug::ERROR) << e.what();
    }
  }
}

file_info_t::file_info_t(const std::string& path,
                         const file_info_t::time_t modify_time,
                         const file_info_t::time_t access_time,
                         const int64_t size,
                         const bool is_dir)
    : m_path(path),
      m_modify_time(modify_time),
      m_access_time(access_time),
      m_size(size),
      m_is_dir(is_dir) {
}

std::string append_path(const std::string& path, const std::string& append) {
  return path + PATH_SEPARATOR + append;
}

std::string append_path(const std::string& path, const char* append) {
  return append_path(path, std::string(append));
}

std::string get_extension(const std::string& path) {
  const auto pos = path.rfind(".");

  // Check that we did not pick up an extension before the last path separator.
  const auto sep_pos = get_last_path_separator_pos(path);
  if ((pos != std::string::npos) && (sep_pos != std::string::npos) && (pos < sep_pos)) {
    return std::string();
  }

  return (pos != std::string::npos) ? path.substr(pos) : std::string();
}

std::string change_extension(const std::string& path, const std::string& new_ext) {
  const auto pos = path.rfind(".");

  // Check that we did not pick up an extension before the last path separator.
  const auto sep_pos = get_last_path_separator_pos(path);
  if ((pos != std::string::npos) && (sep_pos != std::string::npos) && (pos < sep_pos)) {
    return path;
  }

  return (pos != std::string::npos) ? (path.substr(0, pos) + new_ext) : path;
}

std::string get_file_part(const std::string& path, const bool include_ext) {
  const auto pos = get_last_path_separator_pos(path);
  const auto file_name = (pos != std::string::npos) ? path.substr(pos + 1) : path;
  const auto ext_pos = file_name.rfind(".");
  return (include_ext || (ext_pos == std::string::npos) || (ext_pos == 0))
             ? file_name
             : file_name.substr(0, ext_pos);
}

std::string get_dir_part(const std::string& path) {
  const auto pos = get_last_path_separator_pos(path);
  return (pos != std::string::npos) ? path.substr(0, pos) : path;
}

std::string get_temp_dir() {
#if defined(_WIN32)
  WCHAR buf[MAX_PATH + 1] = {0};
  DWORD path_len = GetTempPathW(MAX_PATH + 1, buf);
  if (path_len > 0) {
    return ucs2_to_utf8(std::wstring(buf, path_len));
  }
  return std::string();
#else
  return std::string("/tmp");
#endif
}

std::string get_user_home_dir() {
#if defined(_WIN32)
#if 0
  // TODO(m): We should use SHGetKnownFolderPath() for Vista and later, but this fails to build in
  // older MinGW so we skip it a.t.m.
  std::string local_app_data;
  PWSTR path = nullptr;
  try {
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &path))) {
      local_app_data = ucs2_to_utf8(std::wstring(path));
    }
  } finally {
    if (path != nullptr) {
      CoTaskMemFree(path);
    }
  }
  return local_app_data;
#else
  std::string user_home;
  HANDLE token = nullptr;
  if (SUCCEEDED(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))) {
    // Query the necessary buffer size and allocate memory for it.
    DWORD buf_size = 0;
    GetUserProfileDirectoryW(token, nullptr, &buf_size);
    std::vector<WCHAR> buf(buf_size);

    // Get the actual path.
    if (SUCCEEDED(GetUserProfileDirectoryW(token, buf.data(), &buf_size))) {
      user_home = ucs2_to_utf8(std::wstring(buf.data(), buf.size() - 1));
    }
    CloseHandle(token);
  }
  return user_home;
#endif
#else
  const auto* home_env = getenv("HOME");
  return (home_env != nullptr) ? std::string(home_env) : std::string();
#endif
}

bool is_absolute_path(const std::string& path) {
#ifdef _WIN32
  const bool is_abs_drive =
      (path.size() >= 3) && (path[1] == ':') && ((path[2] == '\\') || (path[2] == '/'));
  const bool is_abs_net = (path.size() >= 2) && (path[0] == '\\') && (path[1] == '\\');
  return is_abs_drive || is_abs_net;
#else
  return (path.size() >= 1) && (path[0] == PATH_SEPARATOR_CHR);
#endif
}

std::string resolve_path(const std::string& path) {
#if defined(_WIN32)
  // TODO(m): Implement me!
  return path;
#else
  auto* char_ptr = realpath(path.c_str(), nullptr);
  if (char_ptr != nullptr) {
    const auto result = std::string(char_ptr);
    std::free(char_ptr);
    return result;
  }
  return std::string();
#endif
}

std::string find_executable(const std::string& path, const std::string& exclude) {
  auto file_to_find = path;

  // TODO(m): For Windows we should also look for files with ".exe" endings (e.g. if you have only
  // specified "gcc", we should find "gcc.exe"). Furthermore we should also prepend the current
  // working directory to the PATH, as Windows searches the CWD before searching the PATH.

  // Handle absolute and relative paths. Examples:
  //  - "C:\Foo\foo.exe"
  //  - "somedir/../mysubdir/foo"
  if (is_absolute_path(file_to_find) ||
      (get_last_path_separator_pos(file_to_find) != std::string::npos)) {
    // Return the full path unless it points to the excluded executable.
    const auto true_path = resolve_path(file_to_find);
    if (true_path.empty()) {
      throw std::runtime_error("Could not resolve absolute path for the executable file.");
    }
    if (lower_case(get_file_part(true_path, false)) != exclude) {
      debug::log(debug::DEBUG) << "Found exe: " << true_path << " (looked for " << path << ")";
      return true_path;
    }

    // ...otherwise search for the named file (which should be a symlink) in the PATH.
    file_to_find = get_file_part(file_to_find);
  }

  // Get the PATH environment variable.
  const auto* path_env = getenv("PATH");
  if (!path_env) {
    return std::string();
  }
  const auto search_path = string_list_t(std::string(path_env), PATH_DELIMITER);

  // Iterate the path from start to end and see if we can find the executable file.
  for (const auto& base_path : search_path) {
    const auto true_path = resolve_path(append_path(base_path, file_to_find));
    if ((!true_path.empty()) && file_exists(true_path)) {
      // Check that this is not the excluded file name.
      if (lower_case(get_file_part(true_path, false)) != exclude) {
        debug::log(debug::DEBUG) << "Found exe: " << true_path << " (looked for " << file_to_find
                                 << ")";
        return true_path;
      }
    }
  }

  throw std::runtime_error("Could not find the executable file.");
}

void create_dir(const std::string& path) {
#ifdef _WIN32
  const auto success = (_wmkdir(utf8_to_ucs2(path).c_str()) == 0);
#else
  const auto success = (mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == 0);
#endif
  if (!success) {
    throw std::runtime_error("Unable to create directory " + path);
  }
}

void create_dir_with_parents(const std::string& path) {
  // Recursively create parent directories if necessary.
  const auto parent = get_dir_part(path);
  if (parent.size() < path.size() && !parent.empty() && !dir_exists(parent)) {
    create_dir_with_parents(parent);
  }

  // Create the requested directory unless it already exists.
  if (!dir_exists(path)) {
    create_dir(path);
  }
}

void remove_file(const std::string& path, const bool ignore_errors) {
#ifdef _WIN32
  const auto success = (_wunlink(utf8_to_ucs2(path).c_str()) == 0);
#else
  const auto success = (unlink(path.c_str()) == 0);
#endif
  if ((!success) && (!ignore_errors)) {
    throw std::runtime_error("Unable to remove file.");
  }
}

void remove_dir(const std::string& path, const bool ignore_errors) {
  const auto files = walk_directory(path);
  for (const auto& file : files) {
    if (file.is_dir()) {
      remove_dir_internal(file.path(), ignore_errors);
    } else {
      remove_file(file.path(), ignore_errors);
    }
  }
  remove_dir_internal(path, ignore_errors);
}

bool dir_exists(const std::string& path) {
#ifdef _WIN32
  struct __stat64 buffer;
  const auto success = (_wstat64(utf8_to_ucs2(path).c_str(), &buffer) == 0);
  return success && S_ISDIR(buffer.st_mode);
#else
  struct stat buffer;
  const auto success = (stat(path.c_str(), &buffer) == 0);
  return success && S_ISDIR(buffer.st_mode);
#endif
}

bool file_exists(const std::string& path) {
#ifdef _WIN32
  struct __stat64 buffer;
  const auto success = (_wstat64(utf8_to_ucs2(path).c_str(), &buffer) == 0);
  return success && S_ISREG(buffer.st_mode);
#else
  struct stat buffer;
  const auto success = (stat(path.c_str(), &buffer) == 0);
  return success && S_ISREG(buffer.st_mode);
#endif
}

void move(const std::string& from_path, const std::string& to_path) {
  // First remove the old target file, if any (otherwise the rename will fail).
  if (file_exists(to_path)) {
    remove_file(to_path);
  }

  // Rename the file.
#ifdef _WIN32
  const auto success =
      (_wrename(utf8_to_ucs2(from_path).c_str(), utf8_to_ucs2(to_path).c_str()) == 0);
#else
  const auto success = (std::rename(from_path.c_str(), to_path.c_str()) == 0);
#endif

  if (!success) {
    throw std::runtime_error("Unable to move file.");
  }
}

void copy(const std::string& from_path, const std::string& to_path) {
  // Copy to a temporary file first and once the copy has succeeded rename it to the target file.
  // This should prevent half-finished copies if the process is terminated prematurely (e.g.
  // CTRL+C).
  const auto base_path = get_dir_part(to_path);
  auto tmp_file = tmp_file_t(base_path, ".tmp");

#ifdef _WIN32
  // TODO(m): We could handle paths longer than MAX_PATH, e.g. by prepending strings with "\\?\"?
  bool success =
      (CopyFileW(utf8_to_ucs2(from_path).c_str(), utf8_to_ucs2(tmp_file.path()).c_str(), FALSE) !=
       0);
#else
  // For non-Windows systems we use a classic buffered read-write loop.
  bool success = false;
  auto* from_file = std::fopen(from_path.c_str(), "rb");
  if (from_file != nullptr) {
    auto* to_file = std::fopen(tmp_file.path().c_str(), "wb");
    if (to_file != nullptr) {
      // We use a buffer size that typically fits in an L1 cache.
      static const int BUFFER_SIZE = 8192;
      std::vector<std::uint8_t> buf(BUFFER_SIZE);
      success = true;
      while (!std::feof(from_file)) {
        const auto bytes_read = std::fread(buf.data(), 1, buf.size(), from_file);
        if (bytes_read == 0u) {
          break;
        }
        const auto bytes_written = std::fwrite(buf.data(), 1, bytes_read, to_file);
        if (bytes_written != bytes_read) {
          success = false;
          break;
        }
      }
      std::fclose(to_file);
    }
    std::fclose(from_file);
  }
#endif

  if (!success) {
    // Note: At this point the temporary file (if any) will be deleted.
    throw std::runtime_error("Unable to copy file.");
  }

  // Move the temporary file to its target name.
  move(tmp_file.path(), to_path);
}

void link_or_copy(const std::string& from_path, const std::string& to_path) {
  // First remove the old file, if any (otherwise the hard link will fail).
  if (file_exists(to_path)) {
    remove_file(to_path);
  }

  // First try to make a hard link. However this may fail if the files are on different volumes for
  // instance.
  bool success;
#ifdef _WIN32
  success = (CreateHardLinkW(
                 utf8_to_ucs2(to_path).c_str(), utf8_to_ucs2(from_path).c_str(), nullptr) != 0);
#else
  success = (link(from_path.c_str(), to_path.c_str()) == 0);
#endif

  // Touch the file to update the modification time.
  if (success) {
#ifdef _WIN32
    success = (_wutime64(utf8_to_ucs2(to_path).c_str(), nullptr) == 0);
#else
    success = (utime(to_path.c_str(), nullptr) == 0);
#endif
  }

  // If the hard link failed, make a full copy instead.
  if (!success) {
    debug::log(debug::DEBUG) << "Hard link failed - copying instead.";
    copy(from_path, to_path);
  }
}

std::string read(const std::string& path) {
  FILE* f;

// Open the file.
#ifdef _WIN32
  const auto err = _wfopen_s(&f, utf8_to_ucs2(path).c_str(), L"rb");
  if (err != 0) {
    throw std::runtime_error("Unable to open the file.");
  }
#else
  f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    throw std::runtime_error("Unable to open the file.");
  }
#endif

  // Get file size.
  std::fseek(f, 0, SEEK_END);
  const auto file_size = static_cast<size_t>(std::ftell(f));
  std::fseek(f, 0, SEEK_SET);

  // Read the data into a string.
  std::string str;
  str.resize(static_cast<std::string::size_type>(file_size));
  auto bytes_left = file_size;
  while ((bytes_left != 0u) && !std::feof(f)) {
    auto* ptr = &str[file_size - bytes_left];
    const auto bytes_read = std::fread(ptr, 1, bytes_left, f);
    bytes_left -= bytes_read;
  }

  // Close the file.
  std::fclose(f);

  if (bytes_left != 0u) {
    throw std::runtime_error("Unable to read the file.");
  }

  return str;
}

void write(const std::string& data, const std::string& path) {
  FILE* f;

// Open the file.
#ifdef _WIN32
  const auto err = _wfopen_s(&f, utf8_to_ucs2(path).c_str(), L"wb");
  if (err != 0) {
    throw std::runtime_error("Unable to open the file.");
  }
#else
  f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    throw std::runtime_error("Unable to open the file.");
  }
#endif

  // Get file size.
  const auto file_size = data.size();

  // Write the data to the file.
  auto bytes_left = file_size;
  while ((bytes_left != 0u) && !std::ferror(f)) {
    const auto* ptr = &data[file_size - bytes_left];
    const auto bytes_written = std::fwrite(ptr, 1, bytes_left, f);
    bytes_left -= bytes_written;
  }

  // Close the file.
  std::fclose(f);

  if (bytes_left != 0u) {
    throw std::runtime_error("Unable to write the file.");
  }
}

file_info_t get_file_info(const std::string& path) {
  // TODO(m): This is pretty much copy-paste from walk_directory(). Refactor.
#ifdef _WIN32
  WIN32_FIND_DATAW find_data;
  auto find_handle = FindFirstFileW(utf8_to_ucs2(path).c_str(), &find_data);
  if (find_handle != INVALID_HANDLE_VALUE) {
    const auto name = ucs2_to_utf8(std::wstring(&find_data.cFileName[0]));
    const auto file_path = append_path(path, name);
    file_info_t::time_t modify_time = 0;
    file_info_t::time_t access_time = 0;
    int64_t size = 0;
    bool is_dir = ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    if (!is_dir) {
      size = two_dwords_to_int64(find_data.nFileSizeLow, find_data.nFileSizeHigh);
      modify_time = win32_filetime_as_unix_epoch(find_data.ftLastWriteTime);
      access_time = win32_filetime_as_unix_epoch(find_data.ftLastAccessTime);
    }
    return file_info_t(file_path, modify_time, access_time, size, is_dir);
  }
#else
  struct stat file_stat;
  const bool stat_ok = (stat(path.c_str(), &file_stat) == 0);
  if (stat_ok) {
    file_info_t::time_t modify_time = 0;
    file_info_t::time_t access_time = 0;
    int64_t size = 0;
    const bool is_dir = S_ISDIR(file_stat.st_mode);
    const bool is_file = S_ISREG(file_stat.st_mode);
    if (is_file) {
      size = static_cast<int64_t>(file_stat.st_size);
#ifdef __APPLE__
      modify_time = static_cast<file_info_t::time_t>(file_stat.st_mtimespec.tv_sec);
      access_time = static_cast<file_info_t::time_t>(file_stat.st_atimespec.tv_sec);
#else
      modify_time = static_cast<file_info_t::time_t>(file_stat.st_mtim.tv_sec);
      access_time = static_cast<file_info_t::time_t>(file_stat.st_atim.tv_sec);
#endif
    }
    return file_info_t(path, modify_time, access_time, size, is_dir);
  }
#endif

  throw std::runtime_error("Unable to get file information.");
}

std::string human_readable_size(const int64_t byte_size) {
  static const char* SUFFIX[6] = {"bytes", "KiB", "MiB", "GiB", "TiB", "PiB"};
  static const int MAX_SUFFIX_IDX = (sizeof(SUFFIX) / sizeof(SUFFIX[0])) - 1;

  double scaled_size = static_cast<double>(byte_size);
  int suffix_idx = 0;
  for (; scaled_size >= 1024.0 && suffix_idx < MAX_SUFFIX_IDX; ++suffix_idx) {
    scaled_size /= 1024.0;
  }

  char buf[20];
  if (suffix_idx >= 1) {
    std::snprintf(buf, sizeof(buf), "%.1f %s", scaled_size, SUFFIX[suffix_idx]);
  } else {
    std::snprintf(buf, sizeof(buf), "%d %s", static_cast<int>(byte_size), SUFFIX[suffix_idx]);
  }
  return std::string(buf);
}

std::vector<file_info_t> walk_directory(const std::string& path) {
  std::vector<file_info_t> files;

#ifdef _WIN32
  const auto search_str = utf8_to_ucs2(append_path(path, "*"));
  WIN32_FIND_DATAW find_data;
  auto find_handle = FindFirstFileW(search_str.c_str(), &find_data);
  if (find_handle == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Unable to walk the directory.");
  }
  do {
    const auto name = ucs2_to_utf8(std::wstring(&find_data.cFileName[0]));
    if ((name != ".") && (name != "..")) {
      const auto file_path = append_path(path, name);
      file_info_t::time_t modify_time = 0;
      file_info_t::time_t access_time = 0;
      int64_t size = 0;
      bool is_dir = false;
      if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        auto subdir_files = walk_directory(file_path);
        for (const auto& entry : subdir_files) {
          files.emplace_back(entry);
          size += entry.size();
          modify_time = std::max(modify_time, entry.modify_time());
          access_time = std::max(access_time, entry.access_time());
        }
        is_dir = true;
      } else {
        size = two_dwords_to_int64(find_data.nFileSizeLow, find_data.nFileSizeHigh);
        modify_time = win32_filetime_as_unix_epoch(find_data.ftLastWriteTime);
        access_time = win32_filetime_as_unix_epoch(find_data.ftLastAccessTime);
      }
      files.emplace_back(file_info_t(file_path, modify_time, access_time, size, is_dir));
    }
  } while (FindNextFileW(find_handle, &find_data) != 0);

  FindClose(find_handle);

  const auto find_error = GetLastError();
  if (find_error != ERROR_NO_MORE_FILES) {
    throw std::runtime_error("Failed to walk the directory.");
  }

#else
  auto* dir = opendir(path.c_str());
  if (dir == nullptr) {
    throw std::runtime_error("Unable to walk the directory.");
  }

  auto* entity = readdir(dir);
  while (entity != nullptr) {
    const auto name = std::string(entity->d_name);
    if ((name != ".") && (name != "..")) {
      const auto file_path = append_path(path, name);
      struct stat file_stat;
      if (stat(file_path.c_str(), &file_stat) == 0) {
        file_info_t::time_t modify_time = 0;
        file_info_t::time_t access_time = 0;
        int64_t size = 0;
        bool is_dir = false;
        if (entity->d_type == DT_DIR) {
          auto subdir_files = walk_directory(file_path);
          for (const auto& entry : subdir_files) {
            files.emplace_back(entry);
            size += entry.size();
            modify_time = std::max(modify_time, entry.modify_time());
            access_time = std::max(access_time, entry.access_time());
          }
          is_dir = true;
        } else if (entity->d_type == DT_REG) {
          size = static_cast<int64_t>(file_stat.st_size);
#ifdef __APPLE__
          modify_time = static_cast<file_info_t::time_t>(file_stat.st_mtimespec.tv_sec);
          access_time = static_cast<file_info_t::time_t>(file_stat.st_atimespec.tv_sec);
#else
          modify_time = static_cast<file_info_t::time_t>(file_stat.st_mtim.tv_sec);
          access_time = static_cast<file_info_t::time_t>(file_stat.st_atim.tv_sec);
#endif
        }
        files.emplace_back(file_info_t(file_path, modify_time, access_time, size, is_dir));
      }
    }
    entity = readdir(dir);
  }

  closedir(dir);
#endif

  return files;
}

}  // namespace file
}  // namespace bcache
