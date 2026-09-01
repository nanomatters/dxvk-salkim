// Copyright 2026 Erhan Bilgili

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>

#include "../../util/util_env.h"

#ifdef _WIN32
#include "../../util/com/com_include.h"
#endif

#include "dxvk_hud_info.h"

namespace dxvk::hud {

  namespace {

    std::string trim(std::string value) {
      auto isSpace = [] (unsigned char ch) { return std::isspace(ch); };
      auto first = std::find_if_not(value.begin(), value.end(), isSpace);
      auto last = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();

      return first < last ? std::string(first, last) : std::string();
    }


#ifdef _WIN32
    enum class WineDisplayBackend : uint32_t {
      Unknown,
      Wayland,
      X11,
      Xwayland,
    };


    std::string queryCpuName() {
      HKEY key = nullptr;
      WCHAR value[256] = { };
      DWORD type = 0;
      DWORD size = sizeof(value);

      if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return std::string();

      LONG status = ::RegQueryValueExW(key, L"ProcessorNameString", nullptr,
        &type, reinterpret_cast<BYTE*>(value), &size);
      ::RegCloseKey(key);

      if (status != ERROR_SUCCESS || type != REG_SZ)
        return std::string();

      value[std::size(value) - 1] = L'\0';
      return trim(str::fromws(value));
    }


    void queryWineVersion(std::string& version, std::string& build) {
      HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
      if (!ntdll)
        return;

      using GetStringProc = const char* (__cdecl*)();
      auto getVersion = reinterpret_cast<GetStringProc>(
        ::GetProcAddress(ntdll, "wine_get_version"));
      auto getBuild = reinterpret_cast<GetStringProc>(
        ::GetProcAddress(ntdll, "wine_get_build_id"));

      if (getVersion) {
        if (const char* value = getVersion())
          version = trim(value);
      }

      if (getBuild) {
        if (const char* value = getBuild())
          build = trim(value);
      }
    }


    std::string queryDisplayBackend() {
      HMODULE win32u = ::GetModuleHandleW(L"win32u.dll");
      if (!win32u)
        return std::string();

      using GetBackendProc = WineDisplayBackend (WINAPI*)();
      auto getBackend = reinterpret_cast<GetBackendProc>(
        ::GetProcAddress(win32u, "__wine_get_display_backend"));
      if (!getBackend)
        return std::string();

      switch (getBackend()) {
        case WineDisplayBackend::Wayland: return "Wayland";
        case WineDisplayBackend::X11: return "X11";
        case WineDisplayBackend::Xwayland: return "Xwayland";
        default: return std::string();
      }
    }
#endif

  }


  HudSystemInfo::HudSystemInfo()
  : protonBuild(trim(env::getEnvVar("PROTON_BUILD_NAME"))) {
#ifdef _WIN32
    cpuName = queryCpuName();
    queryWineVersion(wineVersion, wineBuild);
    displayBackend = queryDisplayBackend();
#endif
  }


  const HudSystemInfo& HudSystemInfo::get() {
    static const HudSystemInfo info;
    return info;
  }

}
