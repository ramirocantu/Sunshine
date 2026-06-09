/**
 * @file src/platform/windows/wgc_helper.h
 * @brief Entry point for the hidden `--wgc-capture-helper` subcommand.
 *
 * When Sunshine runs as a Windows service it executes as the SYSTEM account, where
 * Windows.Graphics.Capture cannot be activated. To work around this, the SYSTEM process
 * launches this same executable as the logged-on user with this subcommand; the helper
 * performs the actual WGC capture (which only works under a real user identity).
 *
 * This header is intentionally lightweight (no D3D/WinRT includes) so it can be included
 * from cross-platform translation units such as main.cpp.
 */
#pragma once

namespace platf::dxgi {
  /**
   * @brief Run the WGC capture helper. Parses its own argv (see launch_wgc_helper).
   * @param argc Argument count (the tokens after `--wgc-capture-helper`).
   * @param argv Argument values.
   * @return 0 on success (WGC initialized and a frame was captured), non-zero on failure.
   */
  int run_wgc_capture_helper(int argc, char **argv);
}  // namespace platf::dxgi
