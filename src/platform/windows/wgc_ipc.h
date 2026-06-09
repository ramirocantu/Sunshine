/**
 * @file src/platform/windows/wgc_ipc.h
 * @brief Shared IPC protocol between the SYSTEM-side WGC helper backend
 *        (display_wgc_helper_vram_t) and the user-session capture helper
 *        (run_wgc_capture_helper).
 *
 * Transport:
 *  - A named pipe (message mode) carries the handshake and control messages below.
 *  - Per-frame signaling uses an auto-reset Event plus a shared-memory block
 *    (wgc_shared_frame_state_t) — both created by the helper and passed to the SYSTEM
 *    process via handle duplication (the helper sends the raw handle values; the SYSTEM
 *    process DuplicateHandle()s them from the helper process, which it owns).
 *
 */
#pragma once

#include <cstdint>

namespace platf::dxgi {
  // 'WGC1' — sanity marker on every pipe message.
  constexpr uint32_t WGC_IPC_MAGIC = 0x57474331;

  // Number of shared textures in the capture ring (double-buffered).
  constexpr uint32_t WGC_SLOT_COUNT = 2;

  // Largest pipe message we ever send/receive (header + biggest payload), with margin.
  constexpr uint32_t WGC_IPC_MAX_MSG = 256;

  enum class wgc_msg_type_e : uint32_t {
    handshake = 1,  ///< helper -> system: capture params + event/mapping handles
    slot_handle = 2,  ///< helper -> system: one per shared texture slot
    set_cursor_visible = 3,  ///< system -> helper: toggle cursor capture
    shutdown = 4,  ///< system -> helper: stop and exit
  };

  struct wgc_msg_header_t {
    uint32_t magic;  ///< WGC_IPC_MAGIC
    uint32_t type;  ///< wgc_msg_type_e
    uint32_t payload_len;  ///< bytes of payload following the header
  };

  struct wgc_handshake_payload_t {
    uint32_t slot_count;  ///< number of slot_handle messages that follow
    uint32_t width;  ///< capture width in pixels
    uint32_t height;  ///< capture height in pixels
    uint32_t dxgi_format;  ///< DXGI_FORMAT of the shared textures
    uint64_t event_handle;  ///< frame-ready Event, value in the helper's process
    uint64_t mapping_handle;  ///< shared-state file mapping, value in the helper's process
  };

  struct wgc_slot_handle_payload_t {
    uint32_t slot_index;  ///< 0 .. slot_count-1
    uint32_t reserved;
    uint64_t texture_handle;  ///< shared NT handle, value in the helper's process
  };

  struct wgc_set_cursor_payload_t {
    uint32_t visible;  ///< 0/1
  };

  /**
   * Shared-memory block (single mapping) updated by the helper after each captured
   * frame and read by the SYSTEM process. Plain integer fields so it needs no Windows
   * headers; the helper uses Interlocked* on the int32/int64 fields.
   */
  struct wgc_shared_frame_state_t {
    volatile int32_t latest_slot;  ///< index of the freshest slot, -1 until first frame
    volatile int32_t producer_alive;  ///< 1 while the helper is actively capturing
    volatile int64_t frame_qpc;  ///< QueryPerformanceCounter ticks of the latest frame
    volatile int64_t frame_generation;  ///< increments once per delivered frame
  };
}  // namespace platf::dxgi
