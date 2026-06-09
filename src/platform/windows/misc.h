/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

// platform includes
#include <Windows.h>
#include <winnt.h>

namespace platf {
  void print_status(const std::string_view &prefix, HRESULT status);
  HDESK syncThreadDesktop();

  /**
   * @brief Check whether the current process is running as the LocalSystem account.
   * @return `true` if running as SYSTEM, `false` otherwise.
   */
  bool is_running_as_system();

  /**
   * @brief Launch this executable's `--wgc-capture-helper` subcommand as the logged-on
   *        user (only valid when running as SYSTEM). The child is placed in a
   *        kill-on-close job object so it dies if this process exits.
   * @param args The argument string appended after `--wgc-capture-helper`.
   * @param job_out Receives the job object handle (caller must CloseHandle it to kill the helper).
   * @return The child process handle (caller owns it), or `nullptr` on failure.
   */
  HANDLE launch_wgc_helper(const std::wstring &args, HANDLE &job_out);

  /**
   * @brief Create the named-pipe server used to control the WGC capture helper, secured
   *        so SYSTEM (this process) and interactive-session users (the helper) can use it.
   * @param pipe_name Full pipe name, e.g. `\\.\pipe\sunshine-wgc-...`.
   * @return The pipe server handle (overlapped, message mode), or `INVALID_HANDLE_VALUE`.
   */
  HANDLE create_wgc_helper_pipe(const std::wstring &pipe_name);

  int64_t qpc_counter();

  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Get file version information from a Windows executable or driver file.
   * @param file_path Path to the file to query.
   * @param version_str Output parameter for version string in format "major.minor.build.revision".
   * @return true if version info was successfully extracted, false otherwise.
   */
  bool getFileVersionInfo(const std::filesystem::path &file_path, std::string &version_str);
}  // namespace platf
