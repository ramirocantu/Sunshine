/**
 * @file src/platform/windows/wgc_helper.cpp
 * @brief Capture-helper subcommand. Runs Windows.Graphics.Capture in the logged-on
 *        user's session (where WGC works) on behalf of the SYSTEM service process, and
 *        publishes captured frames into shared D3D11 keyed-mutex textures that the SYSTEM
 *        process consumes (see wgc_ipc.h for the protocol).
 */
// standard includes
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "display.h"
#include "misc.h"
#include "utf_utils.h"
#include "wgc_helper.h"
#include "wgc_ipc.h"
#include "src/logging.h"

namespace platf::dxgi {
  using namespace std::literals;

  namespace {
    const char *find_arg(int argc, char **argv, std::string_view key) {
      for (int i = 0; i + 1 < argc; ++i) {
        if (key == argv[i]) {
          return argv[i + 1];
        }
      }
      return nullptr;
    }

    HANDLE connect_pipe(const char *name) {
      // The SYSTEM process creates the pipe before launching us, but there is still a
      // small startup race; retry briefly.
      for (int i = 0; i < 100; ++i) {
        HANDLE p = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (p != INVALID_HANDLE_VALUE) {
          DWORD mode = PIPE_READMODE_MESSAGE;
          SetNamedPipeHandleState(p, &mode, nullptr, nullptr);
          return p;
        }
        DWORD e = GetLastError();
        if (e != ERROR_PIPE_BUSY && e != ERROR_FILE_NOT_FOUND) {
          break;
        }
        Sleep(100);
      }
      return INVALID_HANDLE_VALUE;
    }

    bool send_msg(HANDLE pipe, wgc_msg_type_e type, const void *payload, uint32_t len) {
      uint8_t buf[WGC_IPC_MAX_MSG];
      auto *h = reinterpret_cast<wgc_msg_header_t *>(buf);
      h->magic = WGC_IPC_MAGIC;
      h->type = static_cast<uint32_t>(type);
      h->payload_len = len;
      if (len) {
        std::memcpy(buf + sizeof(*h), payload, len);
      }
      DWORD total = sizeof(*h) + len, written = 0;
      return WriteFile(pipe, buf, total, &written, nullptr) && written == total;
    }

  }  // namespace

  int run_wgc_capture_helper(int argc, char **argv) {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error &) {
      BOOST_LOG(warning) << "WGC HELPER: COM apartment already initialized in another mode; continuing"sv;
    }

    const char *output_name = find_arg(argc, argv, "--output-name");
    const char *luid_hi_s = find_arg(argc, argv, "--luid-hi");
    const char *luid_lo_s = find_arg(argc, argv, "--luid-lo");
    const char *dynamic_range_s = find_arg(argc, argv, "--dynamic-range");
    const char *framerate_s = find_arg(argc, argv, "--framerate");
    const char *pipe_name = find_arg(argc, argv, "--pipe-name");
    const char *slot_count_s = find_arg(argc, argv, "--slot-count");

    if (!pipe_name) {
      BOOST_LOG(error) << "WGC HELPER: missing --pipe-name"sv;
      return 1;
    }
    const uint32_t slot_count = slot_count_s ? (uint32_t) std::atoi(slot_count_s) : WGC_SLOT_COUNT;

    LUID target_luid {};
    const bool have_luid = luid_hi_s && luid_lo_s;
    if (have_luid) {
      target_luid.HighPart = (LONG) std::strtol(luid_hi_s, nullptr, 10);
      target_luid.LowPart = (DWORD) std::strtoul(luid_lo_s, nullptr, 10);
    }

    ::video::config_t config {};
    config.framerate = framerate_s ? std::atoi(framerate_s) : 60;
    config.dynamicRange = dynamic_range_s ? std::atoi(dynamic_range_s) : 0;

    BOOST_LOG(info) << "WGC HELPER: starting (output="sv << (output_name ? output_name : "<primary>")
                    << ", dynamicRange="sv << config.dynamicRange << ", framerate="sv << config.framerate
                    << ", slots="sv << slot_count << ')';

    // Per-monitor DPI awareness so monitor coordinates/sizes aren't virtualized.
    if (auto user32 = LoadLibraryA("user32.dll")) {
      typedef BOOL (*SetProcessDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT);
      if (auto f = (SetProcessDpiAwarenessContext_t) GetProcAddress(user32, "SetProcessDpiAwarenessContext")) {
        f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
      }
      FreeLibrary(user32);
    }

    factory1_t factory;
    HRESULT status = CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory);
    if (FAILED(status)) {
      BOOST_LOG(error) << "WGC HELPER: CreateDXGIFactory1 failed [0x"sv << util::hex(status).to_string_view() << ']';
      return 1;
    }

    adapter_t adapter;
    output_t output;
    {
      adapter_t::pointer adapter_p {};
      for (int x = 0; !output && factory->EnumAdapters1(x, &adapter_p) != DXGI_ERROR_NOT_FOUND; ++x) {
        adapter_t adapter_tmp {adapter_p};
        DXGI_ADAPTER_DESC1 adapter_desc;
        adapter_tmp->GetDesc1(&adapter_desc);
        if (have_luid &&
            (adapter_desc.AdapterLuid.HighPart != target_luid.HighPart ||
             adapter_desc.AdapterLuid.LowPart != target_luid.LowPart)) {
          continue;
        }
        output_t::pointer output_p {};
        for (int y = 0; adapter_tmp->EnumOutputs(y, &output_p) != DXGI_ERROR_NOT_FOUND; ++y) {
          output_t output_tmp {output_p};
          DXGI_OUTPUT_DESC output_desc;
          output_tmp->GetDesc(&output_desc);
          if (!output_desc.AttachedToDesktop) {
            continue;
          }
          if (output_name && utf_utils::to_utf8(output_desc.DeviceName) != output_name) {
            continue;
          }
          output = std::move(output_tmp);
          adapter = std::move(adapter_tmp);
          break;
        }
      }
    }
    if (!output) {
      BOOST_LOG(error) << "WGC HELPER: failed to find a matching attached output"sv;
      return 1;
    }

    D3D_FEATURE_LEVEL feature_levels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_2,
      D3D_FEATURE_LEVEL_9_1
    };
    device_t device;
    device_ctx_t device_ctx;
    status = D3D11CreateDevice(
      adapter.get(),
      D3D_DRIVER_TYPE_UNKNOWN,
      nullptr,
      D3D11_CREATE_DEVICE_FLAGS,
      feature_levels,
      ARRAYSIZE(feature_levels),
      D3D11_SDK_VERSION,
      &device,
      nullptr,
      &device_ctx);
    if (FAILED(status)) {
      BOOST_LOG(error) << "WGC HELPER: D3D11CreateDevice failed [0x"sv << util::hex(status).to_string_view() << ']';
      return 1;
    }

    DXGI_FORMAT capture_format = DXGI_FORMAT_UNKNOWN;
    wgc_capture_t dup;
    if (dup.init(device.get(), output.get(), capture_format, config)) {
      BOOST_LOG(error) << "WGC HELPER: wgc_capture_t::init failed (WGC not usable in this context)"sv;
      return 1;
    }

    // Acquire the first frame so we know the exact capture dimensions/format before we
    // size the shared textures. The returned texture lives on `device`.
    texture2d_t frame;
    uint64_t frame_qpc = 0;
    {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      bool got = false;
      while (std::chrono::steady_clock::now() < deadline) {
        auto cs = dup.next_frame(200ms, &frame, frame_qpc);
        if (cs == capture_e::ok && frame) {
          got = true;
          break;
        }
        if (cs == capture_e::error) {
          break;
        }
      }
      if (!got) {
        BOOST_LOG(error) << "WGC HELPER: timed out waiting for the first frame"sv;
        return 2;
      }
    }

    D3D11_TEXTURE2D_DESC frame_desc;
    frame->GetDesc(&frame_desc);
    BOOST_LOG(info) << "WGC HELPER: capturing "sv << frame_desc.Width << 'x' << frame_desc.Height
                    << " (format "sv << (int) frame_desc.Format << ')';

    // Create the shared texture ring.
    std::vector<texture2d_t> slot_tex(slot_count);
    std::vector<keyed_mutex_t> slot_km(slot_count);
    std::vector<HANDLE> slot_handle(slot_count, nullptr);
    for (uint32_t i = 0; i < slot_count; ++i) {
      D3D11_TEXTURE2D_DESC sd {};
      sd.Width = frame_desc.Width;
      sd.Height = frame_desc.Height;
      sd.MipLevels = 1;
      sd.ArraySize = 1;
      sd.SampleDesc.Count = 1;
      sd.Format = frame_desc.Format;
      sd.Usage = D3D11_USAGE_DEFAULT;
      sd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
      sd.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
      if (FAILED(status = device->CreateTexture2D(&sd, nullptr, &slot_tex[i]))) {
        BOOST_LOG(error) << "WGC HELPER: failed to create shared slot texture [0x"sv << util::hex(status).to_string_view() << ']';
        return 1;
      }
      if (FAILED(slot_tex[i]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &slot_km[i]))) {
        BOOST_LOG(error) << "WGC HELPER: failed to query keyed mutex"sv;
        return 1;
      }
      resource1_t resource;
      if (FAILED(slot_tex[i]->QueryInterface(__uuidof(IDXGIResource1), (void **) &resource))) {
        BOOST_LOG(error) << "WGC HELPER: failed to query IDXGIResource1"sv;
        return 1;
      }
      if (FAILED(status = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &slot_handle[i]))) {
        BOOST_LOG(error) << "WGC HELPER: CreateSharedHandle failed [0x"sv << util::hex(status).to_string_view() << ']';
        return 1;
      }
    }

    // Frame-ready event + shared state block.
    HANDLE frame_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    HANDLE state_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(wgc_shared_frame_state_t), nullptr);
    if (!frame_event || !state_mapping) {
      BOOST_LOG(error) << "WGC HELPER: failed to create event/mapping"sv;
      return 1;
    }
    auto *state = (wgc_shared_frame_state_t *) MapViewOfFile(state_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(wgc_shared_frame_state_t));
    if (!state) {
      BOOST_LOG(error) << "WGC HELPER: MapViewOfFile failed"sv;
      return 1;
    }
    state->latest_slot = -1;
    state->producer_alive = 1;
    state->frame_qpc = 0;
    state->frame_generation = 0;

    // Connect to the SYSTEM process and hand it the capture parameters + handles.
    HANDLE pipe = connect_pipe(pipe_name);
    if (pipe == INVALID_HANDLE_VALUE) {
      BOOST_LOG(error) << "WGC HELPER: failed to connect to control pipe"sv;
      return 1;
    }

    wgc_handshake_payload_t hs {};
    hs.slot_count = slot_count;
    hs.width = frame_desc.Width;
    hs.height = frame_desc.Height;
    hs.dxgi_format = (uint32_t) frame_desc.Format;
    hs.event_handle = (uint64_t) (uintptr_t) frame_event;
    hs.mapping_handle = (uint64_t) (uintptr_t) state_mapping;
    if (!send_msg(pipe, wgc_msg_type_e::handshake, &hs, sizeof(hs))) {
      BOOST_LOG(error) << "WGC HELPER: failed to send handshake"sv;
      return 1;
    }
    for (uint32_t i = 0; i < slot_count; ++i) {
      wgc_slot_handle_payload_t sp {};
      sp.slot_index = i;
      sp.texture_handle = (uint64_t) (uintptr_t) slot_handle[i];
      if (!send_msg(pipe, wgc_msg_type_e::slot_handle, &sp, sizeof(sp))) {
        BOOST_LOG(error) << "WGC HELPER: failed to send slot handle"sv;
        return 1;
      }
    }
    BOOST_LOG(info) << "WGC HELPER: handshake complete; publishing frames"sv;

    // Publish a frame into the next slot, synchronized via the slot's keyed mutex.
    uint32_t write_slot = 0;
    auto publish = [&](texture2d_t &src, uint64_t qpc) {
      auto *km = slot_km[write_slot].get();
      // Finite wait so a stalled consumer makes us drop a frame rather than wedge.
      if (km->AcquireSync(0, 1000) != S_OK) {
        return;
      }
      device_ctx->CopyResource(slot_tex[write_slot].get(), src.get());
      km->ReleaseSync(0);
      InterlockedExchange((volatile LONG *) &state->latest_slot, (LONG) write_slot);
      InterlockedExchange64((volatile LONG64 *) &state->frame_qpc, (LONG64) qpc);
      InterlockedIncrement64((volatile LONG64 *) &state->frame_generation);
      SetEvent(frame_event);
      write_slot = (write_slot + 1) % slot_count;
    };

    // Publish the first frame we already captured, then stream until the SYSTEM side
    // closes the pipe (or WGC reports an unrecoverable error).
    publish(frame, frame_qpc);
    frame.reset();

    bool running = true;
    while (running) {
      // Drain any pending control messages from the SYSTEM process. A failed peek means
      // the SYSTEM side closed the pipe (we should exit).
      DWORD avail = 0;
      if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        break;
      }
      while (avail > 0) {
        uint8_t buf[WGC_IPC_MAX_MSG];
        DWORD bytes = 0;
        if (!ReadFile(pipe, buf, sizeof(buf), &bytes, nullptr) || bytes < sizeof(wgc_msg_header_t)) {
          running = false;
          break;
        }
        auto *mh = (wgc_msg_header_t *) buf;
        if (mh->magic == WGC_IPC_MAGIC) {
          if (mh->type == (uint32_t) wgc_msg_type_e::set_cursor_visible && mh->payload_len >= sizeof(wgc_set_cursor_payload_t)) {
            auto *cp = (wgc_set_cursor_payload_t *) (buf + sizeof(*mh));
            dup.set_cursor_visible(cp->visible != 0);
          } else if (mh->type == (uint32_t) wgc_msg_type_e::shutdown) {
            running = false;
            break;
          }
        }
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
          running = false;
          break;
        }
      }
      if (!running) {
        break;
      }

      auto cs = dup.next_frame(200ms, &frame, frame_qpc);
      if (cs == capture_e::ok && frame) {
        // A mid-session resolution/format change invalidates the fixed-size slots; exit
        // so the SYSTEM side reinitializes the whole capture with the new dimensions.
        D3D11_TEXTURE2D_DESC d;
        frame->GetDesc(&d);
        if (d.Width != frame_desc.Width || d.Height != frame_desc.Height || d.Format != frame_desc.Format) {
          BOOST_LOG(info) << "WGC HELPER: capture size/format changed; exiting for reinit"sv;
          frame.reset();
          break;
        }
        publish(frame, frame_qpc);
        frame.reset();
      } else if (cs == capture_e::error) {
        BOOST_LOG(warning) << "WGC HELPER: capture error; exiting"sv;
        break;
      }
      // timeout / reinit: loop and re-check control + capture
    }

    BOOST_LOG(info) << "WGC HELPER: control pipe closed; shutting down"sv;
    InterlockedExchange((volatile LONG *) &state->producer_alive, 0);
    SetEvent(frame_event);  // wake a waiting consumer so it notices we're gone

    for (auto h : slot_handle) {
      if (h) {
        CloseHandle(h);
      }
    }
    UnmapViewOfFile(state);
    CloseHandle(state_mapping);
    CloseHandle(frame_event);
    CloseHandle(pipe);
    return 0;
  }
}  // namespace platf::dxgi
