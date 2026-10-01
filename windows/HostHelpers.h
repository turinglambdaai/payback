#pragma once

// Shared host utilities for the Payback WinUI 3 window: localization
// override handling, money/date formatting, JSON accessors, and milestone
// presentation. Extracted from MainWindow.xaml.cpp so the window code stays
// about UI flow (docs/design.md).

#include "pch.h"
#include "Strings.h"

#include <filesystem>
#include <string>
#include <vector>

namespace payback::host {

// ---------- localization ----------

// -1 follow system, 0 zh, 1 en; kept in a process atomic plus a small file
// under %APPDATA%\Payback so the choice survives relaunches.
int language_override();
void set_language_override(int value);
void load_language_override();

payback::strings::Strings strings();

// ---------- paths and runtime ----------

std::filesystem::path executable_path();
std::string utf8(std::filesystem::path const& path);
rivet::windows::RacketRuntimeConfig runtime_config();

// ---------- money and dates ----------

std::wstring money(double minor, std::wstring const& code);
std::wstring per_day(double minor, std::wstring const& code);
// yyyy-MM-dd <-> DatePicker DateTime (WinUI dates are local-midnight FILETIME)
std::wstring format_date(Windows::Foundation::DateTime const& date_time);
Windows::Foundation::DateTime parse_date(std::wstring const& text);

// ---------- encoding ----------

std::wstring to_utf8_as_wide(std::string const& text);
std::string wide_to_utf8(std::wstring const& text);

// ---------- JSON helpers (Windows.Data.Json) ----------

Windows::Data::Json::IJsonValue field(Windows::Data::Json::IJsonValue const& object,
                                      wchar_t const* key);
std::wstring as_string(Windows::Data::Json::IJsonValue const& value,
                       std::wstring const& fallback = L"");
std::int64_t as_int(Windows::Data::Json::IJsonValue const& value,
                    std::int64_t fallback = 0);
double as_double(Windows::Data::Json::IJsonValue const& value, double fallback = 0.0);
bool as_bool(Windows::Data::Json::IJsonValue const& value, bool fallback = false);
std::vector<std::uint8_t> to_bytes(std::wstring const& json);

// ---------- strings and presentation ----------

std::wstring replace_all(std::wstring text, std::wstring const& from,
                         std::wstring const& to);
int day_of_year();
std::wstring milestone_emoji(std::wstring const& key);
std::wstring milestone_label(std::wstring const& key);
std::wstring daily_quip(int day_index);
std::wstring format_size(double bytes);

// Theme-aware brushes resolved from the content root's ThemeDictionaries so
// cards follow the shipped light/dark palette instead of hardcoded colors.
Microsoft::UI::Xaml::Media::Brush theme_brush(
    Microsoft::UI::Xaml::Controls::Grid const& contentRoot, wchar_t const* key);

}  // namespace payback::host
