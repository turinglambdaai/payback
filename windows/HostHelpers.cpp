#include "pch.h"
#include "HostHelpers.h"
#include "GeneratedBackend.hpp"

#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace payback::host {

namespace {

// -1 follow system, 0 zh, 1 en (set by LanguageBox)
std::atomic<int> g_language_override{-1};

}  // namespace

std::filesystem::path app_data_dir() {
  // %APPDATA%\Payback — the same directory the backend stores payback.json
  // in; small state files (ui language, update handoff) live here too.
  wchar_t buffer[MAX_PATH]{};
  auto const length = ::GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  std::filesystem::path dir = std::filesystem::path(buffer) / L"Payback";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::wstring language_settings_path() {
  auto const dir = app_data_dir();
  if (dir.empty()) {
    return {};
  }
  return (dir / L"ui-language.txt").wstring();
}

// ---------- localization ----------

int language_override() {
  return g_language_override.load(std::memory_order_relaxed);
}

void set_language_override(int value) {
  g_language_override.store(value, std::memory_order_relaxed);
  std::wstring const path = language_settings_path();
  if (path.empty()) {
    return;
  }
  std::ofstream file(path, std::ios::trunc);
  file << (value == 0 ? "zh" : value == 1 ? "en" : "system") << '\n';
}

void load_language_override() {
  std::wstring const path = language_settings_path();
  if (path.empty()) {
    return;
  }
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  set_language_override(line == "zh" ? 0 : line == "en" ? 1 : -1);
}

payback::strings::Strings strings() {
  int const override_value = language_override();
  if (override_value == -1) {
    return payback::strings::strings();  // follows the system language
  }
  return payback::strings::Strings(override_value == 0);
}

// ---------- paths and runtime ----------

std::filesystem::path executable_path() {
  std::wstring buffer(32768, L'\0');
  auto const length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  if (length == 0 || length == buffer.size()) {
    throw std::runtime_error("GetModuleFileNameW failed");
  }
  buffer.resize(length);
  return std::filesystem::path(buffer);
}

std::string utf8(std::filesystem::path const& path) {
  auto const wide = path.wstring();
  if (wide.empty()) {
    return {};
  }
  auto const size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                          wide.data(),
                                          static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    throw std::runtime_error("WideCharToMultiByte failed");
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                            wide.data(), static_cast<int>(wide.size()),
                            result.data(), size, nullptr, nullptr) != size) {
    throw std::runtime_error("WideCharToMultiByte failed");
  }
  return result;
}

rivet::windows::RacketRuntimeConfig runtime_config() {
  auto const exe = executable_path();
  auto const root = exe.parent_path();
  auto const runtime = root / L"runtime";

  rivet::windows::RacketRuntimeConfig config;
  config.executable_path = utf8(exe);
  config.petite_boot = utf8(runtime / L"petite.boot");
  config.scheme_boot = utf8(runtime / L"scheme.boot");
  config.racket_boot = utf8(runtime / L"racket.boot");
  config.backend_bundle = utf8(root / L"res" / L"core.zo");
  config.module_name = rivet_app::kModuleName;
  config.entry_symbol = rivet_app::kEntryName;
  config.dll_dir = runtime.wstring();
  return config;
}

// ---------- money and dates ----------

namespace {

std::wstring currency_symbol(std::wstring const& code) {
  if (code == L"CNY") return L"¥";
  if (code == L"USD") return L"$";
  if (code == L"EUR") return L"€";
  if (code == L"GBP") return L"£";
  if (code == L"JPY") return L"¥";
  if (code == L"HKD") return L"HK$";
  if (code == L"TWD") return L"NT$";
  if (code == L"KRW") return L"₩";
  return code + L" ";
}

}  // namespace

std::wstring money(double minor, std::wstring const& code) {
  wchar_t buffer[64];
  std::swprintf(buffer, 64, L"%.2f", minor / 100.0);
  return currency_symbol(code) + buffer;
}

std::wstring per_day(double minor, std::wstring const& code) {
  wchar_t buffer[64];
  std::swprintf(buffer, 64, L"%.2f", minor / 100.0);
  return currency_symbol(code) + buffer;
}

std::wstring format_date(winrt::Windows::Foundation::DateTime const& date_time) {
  FILETIME file_time{};
  auto const ticks = date_time.time_since_epoch().count();
  file_time.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFF);
  file_time.dwHighDateTime = static_cast<DWORD>((ticks >> 32) & 0xFFFFFFFF);
  FILETIME local{};
  SYSTEMTIME system_time{};
  if (!FileTimeToLocalFileTime(&file_time, &local) ||
      !FileTimeToSystemTime(&local, &system_time)) {
    return L"";
  }
  wchar_t buffer[16];
  std::swprintf(buffer, 16, L"%04d-%02d-%02d", system_time.wYear,
                system_time.wMonth, system_time.wDay);
  return buffer;
}

winrt::Windows::Foundation::DateTime parse_date(std::wstring const& text) {
  int year = 2000, month = 1, day = 1;
  if (text.size() >= 10) {
    year = _wtoi(text.substr(0, 4).c_str());
    month = _wtoi(text.substr(5, 2).c_str());
    day = _wtoi(text.substr(8, 2).c_str());
  }
  SYSTEMTIME system_time{};
  system_time.wYear = static_cast<WORD>(year);
  system_time.wMonth = static_cast<WORD>(month);
  system_time.wDay = static_cast<WORD>(day);
  FILETIME local{};
  SystemTimeToFileTime(&system_time, &local);
  FILETIME utc{};
  LocalFileTimeToFileTime(&local, &utc);
  winrt::Windows::Foundation::DateTime result{winrt::Windows::Foundation::TimeSpan{
      static_cast<std::int64_t>((static_cast<std::int64_t>(utc.dwHighDateTime)
                                 << 32) |
                                utc.dwLowDateTime)}};
  return result;
}

// ---------- encoding ----------

std::wstring to_utf8_as_wide(std::string const& text) {
  return std::wstring(winrt::to_hstring(text).c_str());
}

std::string wide_to_utf8(std::wstring const& text) {
  return winrt::to_string(text);
}

// ---------- JSON helpers (Windows.Data.Json) ----------

winrt::Windows::Data::Json::IJsonValue field(winrt::Windows::Data::Json::IJsonValue const& object,
                                      wchar_t const* key) {
  using namespace winrt::Windows::Data::Json;
  if (object.ValueType() == JsonValueType::Object) {
    return object.GetObjectW().GetNamedValue(key, JsonValue::CreateNullValue());
  }
  return JsonValue::CreateNullValue();
}

std::wstring as_string(winrt::Windows::Data::Json::IJsonValue const& value,
                       std::wstring const& fallback) {
  using namespace winrt::Windows::Data::Json;
  if (value.ValueType() == JsonValueType::String) {
    return std::wstring(value.GetString().c_str());
  }
  return fallback;
}

std::int64_t as_int(winrt::Windows::Data::Json::IJsonValue const& value,
                    std::int64_t fallback) {
  using namespace winrt::Windows::Data::Json;
  if (value.ValueType() == JsonValueType::Number) {
    return static_cast<std::int64_t>(value.GetNumber());
  }
  return fallback;
}

double as_double(winrt::Windows::Data::Json::IJsonValue const& value, double fallback) {
  using namespace winrt::Windows::Data::Json;
  if (value.ValueType() == JsonValueType::Number) {
    return value.GetNumber();
  }
  return fallback;
}

bool as_bool(winrt::Windows::Data::Json::IJsonValue const& value, bool fallback) {
  using namespace winrt::Windows::Data::Json;
  if (value.ValueType() == JsonValueType::Boolean) {
    return value.GetBoolean();
  }
  return fallback;
}

std::vector<std::uint8_t> to_bytes(std::wstring const& json) {
  auto const utf8_text = wide_to_utf8(json);
  return std::vector<std::uint8_t>(utf8_text.begin(), utf8_text.end());
}

// ---------- strings and presentation ----------

std::wstring replace_all(std::wstring text, std::wstring const& from,
                         std::wstring const& to) {
  std::size_t position = 0;
  while ((position = text.find(from, position)) != std::wstring::npos) {
    text.replace(position, from.size(), to);
    position += to.size();
  }
  return text;
}

int day_of_year() {
  SYSTEMTIME system_time{};
  ::GetLocalTime(&system_time);
  static const int cumulative[] = {0,   31,  59,  90,  120, 151,
                                   181, 212, 243, 273, 304, 334};
  bool const leap =
      (system_time.wYear % 4 == 0 && system_time.wYear % 100 != 0) ||
      system_time.wYear % 400 == 0;
  int const extra = (leap && system_time.wMonth > 2) ? 1 : 0;
  return cumulative[system_time.wMonth - 1] + system_time.wDay + extra;
}

std::wstring milestone_emoji(std::wstring const& key) {
  if (key == L"days-100") return L"🌱";
  if (key == L"days-365") return L"📅";
  if (key == L"days-1000") return L"🏆";
  if (key == L"cpd-10") return L"💸";
  if (key == L"cpd-5") return L"🌤";
  if (key == L"cpd-2") return L"🍃";
  if (key == L"cpd-1") return L"🪶";
  if (key == L"cpd-05") return L"✨";
  if (key == L"paid-back") return L"🎉";
  return L"🏅";
}

std::wstring milestone_label(std::wstring const& key) {
  payback::strings::Strings const& s = strings();
  if (key == L"days-100") return s.milestone_days100();
  if (key == L"days-365") return s.milestone_days365();
  if (key == L"days-1000") return s.milestone_days1000();
  if (key == L"cpd-10") return s.milestone_cpd10();
  if (key == L"cpd-5") return s.milestone_cpd5();
  if (key == L"cpd-2") return s.milestone_cpd2();
  if (key == L"cpd-1") return s.milestone_cpd1();
  if (key == L"cpd-05") return s.milestone_cpd05();
  if (key == L"paid-back") return s.milestone_paid_back();
  return key;
}

std::wstring daily_quip(int day_index) {
  payback::strings::Strings const& s = strings();
  std::wstring const quips[] = {s.quip1(), s.quip2(), s.quip3(),
                                s.quip4(), s.quip5(), s.quip6()};
  int const index = day_index % 6;
  return quips[index < 0 ? index + 6 : index];
}

std::wstring format_size(double bytes) {
  wchar_t buffer[32];
  std::swprintf(buffer, 32, L"%.1f MB", bytes / (1024.0 * 1024.0));
  return buffer;
}

winrt::Microsoft::UI::Xaml::Media::Brush theme_brush(
    winrt::Microsoft::UI::Xaml::Controls::Grid const& contentRoot, wchar_t const* key) {
  auto const dictionaries = contentRoot.Resources().ThemeDictionaries();
  auto const theme_key =
      contentRoot.ActualTheme() == winrt::Microsoft::UI::Xaml::ElementTheme::Dark
          ? winrt::box_value(winrt::hstring(L"Dark"))
          : winrt::box_value(winrt::hstring(L"Light"));
  if (auto const dictionary =
          dictionaries.TryLookup(theme_key)
              .try_as<winrt::Microsoft::UI::Xaml::ResourceDictionary>()) {
    if (auto const brush =
            dictionary.TryLookup(winrt::box_value(winrt::hstring(key)))
                .try_as<winrt::Microsoft::UI::Xaml::Media::Brush>()) {
      return brush;
    }
  }
  // unreachable with the shipped XAML; a neutral surface beats a crash
  return winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
      winrt::Microsoft::UI::Colors::Gray()};
}

}  // namespace payback::host
