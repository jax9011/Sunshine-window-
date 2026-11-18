/**
 * @file src/platform/windows/display_wgc.cpp
 * @brief Definitions for WinRT Windows.Graphics.Capture API
 */
// platform includes
#include <dxgi1_2.h>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

// local includes
#include "display.h"
#include "misc.h"
#include "utf_utils.h"
#include "src/logging.h"

// Gross hack to work around MINGW-packages#22160
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.foundation.metadata.h>
#include <winrt/windows.graphics.directx.direct3d11.h>

namespace platf {
  using namespace std::literals;
}

namespace winrt {
  using namespace Windows::Foundation;
  using namespace Windows::Foundation::Metadata;
  using namespace Windows::Graphics::Capture;
  using namespace Windows::Graphics::DirectX::Direct3D11;

  extern "C" {
    /**
     * @brief Create direct3 D11 device from DXGI device.
     *
     * @param dxgiDevice DXGI device.
     * @param graphicsDevice Graphics device.
     * @return Created direct3 D11 device from DXGI device object or status.
     */
    HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
  }

  /**
   * Windows structures sometimes have compile-time GUIDs. GCC supports this, but in a roundabout way.
   * If WINRT_IMPL_HAS_DECLSPEC_UUID is true, then the compiler supports adding this attribute to a struct. For example, Visual Studio.
   * If not, then MinGW GCC has a workaround to assign a GUID to a structure.
   */
  struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
    __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
    IDirect3DDxgiInterfaceAccess: ::IUnknown {
    /**
     * @brief Retrieve a DXGI interface from a WinRT Direct3D object.
     *
     * @param id COM interface ID requested from the WinRT wrapper.
     * @param object Output pointer that receives the requested COM interface.
     * @return HRESULT from the WinRT object's interface query.
     */
    virtual HRESULT __stdcall GetInterface(REFIID id, void **object) = 0;  // NOSONAR(cpp:S5008): IDirect3DDxgiInterfaceAccess::GetInterface requires a void double pointer in the WinRT ABI.
  };
}  // namespace winrt
#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}
  // compare with __declspec(uuid(...)) for the struct above.
};

/**
 * @brief Return the GUID used to request IDirect3DDxgiInterfaceAccess.
 *
 * @return GUID for the WinRT DXGI interface-access helper.
 */
template<>
constexpr auto __mingw_uuidof<winrt::IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}
#endif

namespace platf::dxgi {
  namespace {
    std::wstring to_lower(std::wstring str) {
      for (auto &c : str) {
        c = (wchar_t) towlower(c);
      }
      return str;
    }

    /**
     * @brief Read a string value from the registry.
     */
    std::wstring read_registry_string(HKEY root, const wchar_t *subkey, const wchar_t *name) {
      HKEY key = nullptr;
      if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
      }

      wchar_t buf[512] = {};
      DWORD size = sizeof(buf) - sizeof(wchar_t);
      DWORD type = 0;
      std::wstring value;
      if (RegQueryValueExW(key, name, nullptr, &type, (LPBYTE) buf, &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        value.assign(buf, size / sizeof(wchar_t));
        while (!value.empty() && value.back() == L'\0') {
          value.pop_back();
        }
      }
      RegCloseKey(key);
      return value;
    }

    /**
     * @brief Get the requested target application (lower case, trimmed), or an empty string.
     *
     * The process environment is checked first. When Sunshine was started by a parent process that
     * does not have the variable (for example a service, or Explorer started before `setx` ran),
     * the user and system environment stored in the registry are used as a fallback.
     */
    std::wstring get_target_app() {
      std::wstring value;
      const wchar_t *source = L"environment";

      wchar_t buf[512];
      DWORD len = GetEnvironmentVariableW(L"SUNSHINE_CAPTURE_APP", buf, 512);
      if (len > 0 && len < 512) {
        value.assign(buf, len);
      }

      if (value.find_first_not_of(L" \t\"") == std::wstring::npos) {
        value = read_registry_string(HKEY_CURRENT_USER, L"Environment", L"SUNSHINE_CAPTURE_APP");
        source = L"user registry";
      }
      if (value.find_first_not_of(L" \t\"") == std::wstring::npos) {
        value = read_registry_string(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", L"SUNSHINE_CAPTURE_APP");
        source = L"system registry";
      }

      value = to_lower(value);
      const wchar_t *trim = L" \t\"";
      auto first = value.find_first_not_of(trim);
      if (first == std::wstring::npos) {
        return {};
      }
      auto last = value.find_last_not_of(trim);
      value = value.substr(first, last - first + 1);

      // log the setting once per value so it is visible in the log
      static std::mutex log_mutex;
      static std::wstring logged;
      {
        std::lock_guard<std::mutex> lock(log_mutex);
        if (logged != value) {
          logged = value;
          BOOST_LOG(info) << "SUNSHINE_CAPTURE_APP = \""sv << utf_utils::to_utf8(value.c_str()) << "\" (from "sv << utf_utils::to_utf8(source) << ')';
        }
      }
      return value;
    }

    using DwmGetWindowAttribute_t = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);

    DwmGetWindowAttribute_t get_dwm_attribute_fn() {
      static DwmGetWindowAttribute_t fn = []() -> DwmGetWindowAttribute_t {
        auto lib = LoadLibraryW(L"dwmapi.dll");
        return lib ? (DwmGetWindowAttribute_t) GetProcAddress(lib, "DwmGetWindowAttribute") : nullptr;
      }();
      return fn;
    }

    /**
     * @brief Visible window bounds (without invisible resize borders) in virtual desktop coordinates.
     */
    bool get_visible_bounds(HWND hwnd, RECT &rect) {
      constexpr DWORD DWMWA_EXTENDED_FRAME_BOUNDS_ = 9;
      auto fn = get_dwm_attribute_fn();
      if (fn && SUCCEEDED(fn(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS_, &rect, sizeof(rect)))) {
        return true;
      }
      return GetWindowRect(hwnd, &rect) != 0;
    }

    bool is_cloaked(HWND hwnd) {
      constexpr DWORD DWMWA_CLOAKED_ = 14;
      auto fn = get_dwm_attribute_fn();
      DWORD cloaked = 0;
      return fn && SUCCEEDED(fn(hwnd, DWMWA_CLOAKED_, &cloaked, sizeof(cloaked))) && cloaked != 0;
    }

    std::wstring get_title(HWND hwnd) {
      wchar_t title[512] = {};
      int len = GetWindowTextW(hwnd, title, 512);
      return len > 0 ? std::wstring(title, len) : std::wstring();
    }

    std::wstring get_class(HWND hwnd) {
      wchar_t name[256] = {};
      int len = GetClassNameW(hwnd, name, 256);
      return len > 0 ? std::wstring(name, len) : std::wstring();
    }

    /**
     * @brief Lower case executable file name (e.g. `notepad.exe`) of the process that owns the window.
     */
    std::wstring get_exe_name(HWND hwnd, DWORD *pid_out = nullptr) {
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      if (pid_out) {
        *pid_out = pid;
      }
      std::wstring exe;
      if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
          exe = to_lower(std::wstring(path, size));
          auto slash = exe.find_last_of(L"\\/");
          if (slash != std::wstring::npos) {
            exe = exe.substr(slash + 1);
          }
        }
        CloseHandle(proc);
      }
      return exe;
    }

    bool is_internal_exe(const std::wstring &exe) {
      static const wchar_t *internal[] = {
        L"startmenuexperiencehost.exe",
        L"applicationframehost.exe",
        L"peopleexperiencehost.exe",
        L"shellexperiencehost.exe",
        L"systemsettings.exe",
        L"textinputhost.exe",
        L"searchapp.exe",
        L"searchui.exe",
        L"lockapp.exe",
        L"gamebar.exe",
        L"tabtip.exe",
      };
      for (auto name : internal) {
        if (exe == name) {
          return true;
        }
      }
      return exe.rfind(L"windowsinternal", 0) == 0;
    }

    /**
     * @brief UWP apps live in an `ApplicationFrameWindow`; the real window is a child owned by another process.
     */
    HWND get_uwp_actual_window(HWND parent) {
      auto cls = get_class(parent);
      if (cls != L"ApplicationFrameWindow" && cls != L"WinUIDesktopWin32WindowClass") {
        return nullptr;
      }
      DWORD parent_pid = 0;
      GetWindowThreadProcessId(parent, &parent_pid);
      for (HWND child = FindWindowExW(parent, nullptr, nullptr, nullptr); child; child = FindWindowExW(parent, child, nullptr, nullptr)) {
        DWORD child_pid = 0;
        GetWindowThreadProcessId(child, &child_pid);
        if (child_pid != parent_pid) {
          return child;
        }
      }
      return nullptr;
    }

    struct window_candidate_t {
      HWND hwnd;  // window handed to Windows.Graphics.Capture
      std::wstring exe;
      std::wstring title;
      long long area;
    };

    BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lparam) {
      auto *list = (std::vector<window_candidate_t> *) lparam;

      if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || is_cloaked(hwnd)) {
        return TRUE;
      }
      if (GetWindow(hwnd, GW_OWNER) != nullptr || (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) {
        return TRUE;
      }

      RECT client {};
      if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0) {
        return TRUE;
      }

      RECT wr {};
      if (!GetWindowRect(hwnd, &wr)) {
        return TRUE;
      }

      HWND capture_hwnd = hwnd;
      if (HWND actual = get_uwp_actual_window(hwnd)) {
        capture_hwnd = actual;
      }

      DWORD pid = 0;
      auto exe = get_exe_name(capture_hwnd, &pid);
      if (pid == GetCurrentProcessId() || is_internal_exe(exe)) {
        return TRUE;
      }

      auto title = get_title(hwnd);
      if (exe == L"explorer.exe" && title.empty()) {
        return TRUE;
      }

      list->push_back({capture_hwnd, exe, title, (long long) (wr.right - wr.left) * (wr.bottom - wr.top)});
      return TRUE;
    }

    std::vector<window_candidate_t> list_windows() {
      std::vector<window_candidate_t> list;
      EnumWindows(enum_windows_cb, (LPARAM) &list);
      return list;
    }
  }  // namespace

  bool window_capture_requested() {
    return !get_target_app().empty();
  }

  HWND find_capture_window() {
    auto needle = get_target_app();
    if (needle.empty()) {
      return nullptr;
    }

    auto windows = list_windows();

    // rank: 3 = exact executable name, 2 = executable name contains text, 1 = title contains text
    const window_candidate_t *best = nullptr;
    int best_rank = 0;
    for (const auto &w : windows) {
      int rank = 0;
      if (w.exe == needle || w.exe == needle + L".exe") {
        rank = 3;
      } else if (!w.exe.empty() && w.exe.find(needle) != std::wstring::npos) {
        rank = 2;
      } else if (to_lower(w.title).find(needle) != std::wstring::npos) {
        rank = 1;
      }
      if (rank == 0) {
        continue;
      }
      if (!best || rank > best_rank || (rank == best_rank && w.area > best->area)) {
        best = &w;
        best_rank = rank;
      }
    }

    if (best) {
      BOOST_LOG(info) << "Window capture target: ["sv << utf_utils::to_utf8(best->exe.c_str()) << "] "sv << utf_utils::to_utf8(best->title.c_str());
      return best->hwnd;
    }

    BOOST_LOG(warning) << "No window matched SUNSHINE_CAPTURE_APP=\""sv << utf_utils::to_utf8(needle.c_str()) << "\". Visible windows:"sv;
    for (const auto &w : windows) {
      BOOST_LOG(warning) << "  ["sv << utf_utils::to_utf8(w.exe.c_str()) << "] "sv << utf_utils::to_utf8(w.title.c_str());
    }
    return nullptr;
  }

  wgc_capture_t::wgc_capture_t() {
    InitializeConditionVariable(&frame_present_cv);
  }

  wgc_capture_t::~wgc_capture_t() {
    if (item && closed_token.value) {
      try {
        item.Closed(closed_token);
      } catch (...) {
      }
    }
    if (capture_session) {
      capture_session.Close();
    }
    if (frame_pool) {
      frame_pool.Close();
    }
    item = nullptr;
    capture_session = nullptr;
    frame_pool = nullptr;
  }

  /**
   * @brief Initialize the Windows.Graphics.Capture backend.
   * @return 0 on success, -1 on failure.
   */
  int wgc_capture_t::init(display_base_t *display, const ::video::config_t &config) {
    HRESULT status;
    dxgi::dxgi_t dxgi;
    winrt::com_ptr<::IInspectable> d3d_comhandle;
    try {
      if (!winrt::GraphicsCaptureSession::IsSupported()) {
        BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows!"sv;
        return -1;
      }
      if (FAILED(status = display->device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi))) {
        BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      if (FAILED(status = winrt::CreateDirect3D11DeviceFromDXGIDevice(*&dxgi, d3d_comhandle.put()))) {
        BOOST_LOG(error) << "Failed to query WinRT DirectX interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire device: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    DXGI_OUTPUT_DESC output_desc;
    uwp_device = d3d_comhandle.as<winrt::IDirect3DDevice>();
    display->output->GetDesc(&output_desc);

    auto monitor_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (monitor_factory == nullptr) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: no capture interop factory"sv;
      return -1;
    }

    if (window_capture_requested()) {
      // Capture a single application window instead of the whole monitor
      HWND target = find_capture_window();
      if (target == nullptr) {
        BOOST_LOG(error) << "SUNSHINE_CAPTURE_APP is set but no matching visible window was found. Is the application running and not minimized?"sv;
        return -1;
      }

      if (FAILED(status = monitor_factory->CreateForWindow(target, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
        BOOST_LOG(error) << "Failed to capture the target window [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      try {
        auto size = item.Size();
        if (size.Width <= 0 || size.Height <= 0) {
          BOOST_LOG(error) << "Target window has an invalid size"sv;
          return -1;
        }

        display->width = size.Width;
        display->height = size.Height;
        display->width_before_rotation = size.Width;
        display->height_before_rotation = size.Height;
        display->display_rotation = DXGI_MODE_ROTATION_IDENTITY;

        // Map absolute mouse input onto the window instead of the monitor
        RECT bounds;
        if (get_visible_bounds(target, bounds)) {
          display->offset_x = bounds.left - GetSystemMetrics(SM_XVIRTUALSCREEN);
          display->offset_y = bounds.top - GetSystemMetrics(SM_YVIRTUALSCREEN);
        }

        target_window = target;
        last_size = size;
        item_closed = false;
        closed_token = item.Closed([this](winrt::GraphicsCaptureItem const &, winrt::IInspectable const &) {
          item_closed = true;
        });

        BOOST_LOG(info) << "Capturing window "sv << size.Width << 'x' << size.Height;
      } catch (winrt::hresult_error &e) {
        BOOST_LOG(error) << "Failed to set up window capture: [0x"sv << util::hex(e.code()).to_string_view() << ']';
        return -1;
      }
    } else if (FAILED(status = monitor_factory->CreateForMonitor(output_desc.Monitor, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire display: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    if (config.dynamicRange) {
      display->capture_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    } else {
      display->capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    try {
      frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(uwp_device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(display->capture_format), 2, item.Size());
      capture_session = frame_pool.CreateCaptureSession(item);
      frame_pool.FrameArrived({this, &wgc_capture_t::on_frame_arrived});
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to create capture session: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
        capture_session.IsBorderRequired(false);
      } else {
        BOOST_LOG(warning) << "Can't disable colored border around capture area on this version of Windows";
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may not be fully supported on this device for this release of Windows: failed to disable border around capture area: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
        capture_session.MinUpdateInterval(4ms);  // 250Hz
      } else {
        BOOST_LOG(warning) << "Can't set MinUpdateInterval on this version of Windows";
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may be capped to 60fps on this device for this release of Windows: failed to set MinUpdateInterval: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      capture_session.StartCapture();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to start capture: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    return 0;
  }

  /**
   * This function runs in a separate thread spawned by the frame pool and is a producer of frames.
   * To maintain parity with the original display interface, this frame will be consumed by the capture thread.
   * Acquire a read-write lock, make the produced frame available to the capture thread, then wake the capture thread.
   */
  void wgc_capture_t::on_frame_arrived(winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame {nullptr};
    try {
      frame = sender.TryGetNextFrame();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Failed to capture frame: "sv << e.code();
      return;
    }
    if (frame != nullptr) {
      AcquireSRWLockExclusive(&frame_lock);
      if (produced_frame) {
        produced_frame.Close();
      }

      produced_frame = frame;
      ReleaseSRWLockExclusive(&frame_lock);
      WakeConditionVariable(&frame_present_cv);
    }
  }

  /**
   * @brief Get the next frame from the producer thread.
   * If not available, the capture thread blocks until one is, or the wait times out.
   */
  capture_e wgc_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time) {
    // this CONSUMER runs in the capture thread
    release_frame();

    if (target_window) {
      // The captured window went away or changed size: rebuild the capture so the
      // frame pool and the stream geometry follow the window.
      try {
        if (item_closed.load() || !IsWindow(target_window)) {
          BOOST_LOG(warning) << "Captured window was closed"sv;
          return capture_e::reinit;
        }
        if (IsIconic(target_window)) {
          return capture_e::timeout;  // minimized: no frames until it is restored
        }
        auto size = item.Size();
        if (size.Width != last_size.Width || size.Height != last_size.Height) {
          BOOST_LOG(info) << "Captured window resized to "sv << size.Width << 'x' << size.Height;
          return capture_e::reinit;
        }
      } catch (winrt::hresult_error &) {
        return capture_e::reinit;
      }
    }

    AcquireSRWLockExclusive(&frame_lock);
    if (produced_frame == nullptr && SleepConditionVariableSRW(&frame_present_cv, &frame_lock, timeout.count(), 0) == 0) {
      ReleaseSRWLockExclusive(&frame_lock);
      if (GetLastError() == ERROR_TIMEOUT) {
        return capture_e::timeout;
      } else {
        return capture_e::capture_error;
      }
    }
    if (produced_frame) {
      consumed_frame = produced_frame;
      produced_frame = nullptr;
    }
    ReleaseSRWLockExclusive(&frame_lock);
    if (consumed_frame == nullptr) {  // spurious wakeup
      return capture_e::timeout;
    }

    auto capture_access = consumed_frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
    if (capture_access == nullptr) {
      return capture_e::capture_error;
    }
    capture_access->GetInterface(IID_ID3D11Texture2D, (void **) out);
    out_time = consumed_frame.SystemRelativeTime().count();  // raw ticks from query performance counter
    return capture_e::ok;
  }

  capture_e wgc_capture_t::release_frame() {
    if (consumed_frame != nullptr) {
      consumed_frame.Close();
      consumed_frame = nullptr;
    }
    return capture_e::ok;
  }

  int wgc_capture_t::set_cursor_visible(bool x) {
    try {
      if (capture_session.IsCursorCaptureEnabled() != x) {
        capture_session.IsCursorCaptureEnabled(x);
      }
      return 0;
    } catch (winrt::hresult_error &) {
      return -1;
    }
  }

  int display_wgc_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    texture.reset();
    return 0;
  }

  /**
   * @brief Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   */
  capture_e display_wgc_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // Create the staging texture if it doesn't exist. It should match the source in size and format.
    if (texture == nullptr) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';

      D3D11_TEXTURE2D_DESC t {};
      t.Width = width;
      t.Height = height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_STAGING;
      t.Format = capture_format;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto texture_status = device->CreateTexture2D(&t, nullptr, &texture);

      if (FAILED(texture_status)) {
        BOOST_LOG(error) << "Failed to create staging texture [0x"sv << util::hex(texture_status).to_string_view() << ']';
        return capture_e::capture_error;
      }
    }

    // It's possible for our display enumeration to race with mode changes and result in
    // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
    if (desc.Width != width || desc.Height != height) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }
    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    // Copy from GPU to CPU
    device_ctx->CopyResource(texture.get(), src.get());

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }
    auto img = (img_t *) img_out.get();

    // Map the staging texture for CPU access (making it inaccessible for the GPU)
    if (FAILED(status = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info))) {
      BOOST_LOG(error) << "Failed to map texture [0x"sv << util::hex(status).to_string_view() << ']';

      return capture_e::capture_error;
    }

    // Now that we know the capture format, we can finish creating the image
    if (complete_img(img, false)) {
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
      return capture_e::capture_error;
    }

    // WGC already supplies pixels in desktop orientation.
    copy_mapped_image(img_info, *img, DXGI_MODE_ROTATION_IDENTITY);

    // Unmap the staging texture to allow GPU access again
    device_ctx->Unmap(texture.get(), 0);
    img_info.pData = nullptr;

    if (img) {
      img->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_wgc_ram_t::release_snapshot() {
    return dup.release_frame();
  }
}  // namespace platf::dxgi
