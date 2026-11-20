#include "app/file_association.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace wax::app {

#ifdef _WIN32

namespace {

const wchar_t* const kProgId = L"Wax.Song";
const wchar_t* const kExtensionKey = L"Software\\Classes\\.wax";
const wchar_t* const kProgIdKey = L"Software\\Classes\\Wax.Song";
const wchar_t* const kCommandKey = L"Software\\Classes\\Wax.Song\\shell\\open\\command";
const wchar_t* const kIconKey = L"Software\\Classes\\Wax.Song\\DefaultIcon";

std::string utf8Of(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring runningProgramPath() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {   // (a long path does not fit MAX_PATH: the call fills the buffer, so grow it until it does not)
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) return {};
    if (length < path.size()) { path.resize(length); return path; }
    path.resize(path.size() * 2);
  }
}

// Sets the default value (the unnamed one) of a key, creating the key.
bool setDefault(const wchar_t* key, const std::wstring& value) {
  HKEY handle = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
  const LONG result = RegSetValueExW(handle, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(handle);
  return result == ERROR_SUCCESS;
}

std::wstring readDefault(const wchar_t* key) {
  HKEY handle = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS) return {};
  wchar_t buffer[2048];
  DWORD size = sizeof buffer;
  DWORD type = 0;
  const LONG result = RegQueryValueExW(handle, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size);
  RegCloseKey(handle);
  if (result != ERROR_SUCCESS || type != REG_SZ) return {};
  return std::wstring(buffer, wcsnlen(buffer, sizeof buffer / sizeof(wchar_t)));
}

}  // namespace

bool fileAssociationSupported() { return true; }

bool isOpenerOfWaxFiles() {
  const std::wstring program = runningProgramPath();
  return !program.empty() && readDefault(kExtensionKey) == kProgId && readDefault(kCommandKey) == L"\"" + program + L"\" \"%1\"";
}

std::string registerAsOpenerOfWaxFiles() {
  const std::wstring program = runningProgramPath();
  if (program.empty()) return "Windows would not say where this program is.";
  const bool wrote = setDefault(kExtensionKey, kProgId) && setDefault(kProgIdKey, L"Wax song") && setDefault(kIconKey, L"\"" + program + L"\",0") &&
                     setDefault(kCommandKey, L"\"" + program + L"\" \"%1\"");
  if (!wrote) return "Windows would not let the registry be written.";
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);   // the file manager redraws its icons and learns the new opener
  return {};
}

std::vector<std::string> commandLineUtf8() {
  int count = 0;
  wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
  std::vector<std::string> args;
  if (!wide) return args;
  for (int i = 0; i < count; i++) args.push_back(utf8Of(wide[i]));
  LocalFree(wide);
  return args;
}

#else

bool fileAssociationSupported() { return false; }
bool isOpenerOfWaxFiles() { return false; }
std::string registerAsOpenerOfWaxFiles() { return "Only Windows is set up for this: use the file manager's \"Open with\" on a .wax file."; }
std::vector<std::string> commandLineUtf8() { return {}; }

#endif

}  // namespace wax::app
