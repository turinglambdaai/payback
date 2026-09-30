#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "GeneratedBackend.hpp"
#include "Strings.h"

#include <algorithm>
#include <cmath>
#include <shellapi.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace winrt::RivetHost::implementation {
namespace {

using payback::strings::Strings;

// language_override_: -1 follow system, 0 zh, 1 en (set by LanguageBox)
static std::atomic<int> g_language_override{-1};

payback::strings::Strings strings() {
  int override_value = g_language_override.load(std::memory_order_relaxed);
  if (override_value == -1) {
    return payback::strings::strings();  // follows the system language
  }
  return payback::strings::Strings(override_value == 0);
}

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


// ---------- localization (generated from shared/strings) ----------

// ---------- money and dates -----------------------------------------------

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

// yyyy-MM-dd <-> DatePicker DateTime (WinUI dates are local-midnight FILETIME)
std::wstring format_date(Windows::Foundation::DateTime const& date_time) {
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

Windows::Foundation::DateTime parse_date(std::wstring const& text) {
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
  Windows::Foundation::DateTime result{Windows::Foundation::TimeSpan{
      static_cast<std::int64_t>((static_cast<std::int64_t>(utc.dwHighDateTime)
                                 << 32) |
                                utc.dwLowDateTime)}};
  return result;
}

std::wstring to_utf8_as_wide(std::string const& text) {
  return std::wstring(winrt::to_hstring(text).c_str());
}

std::string wide_to_utf8(std::wstring const& text) {
  return winrt::to_string(text);
}

// ---------- JSON helpers (Windows.Data.Json) -------------------------------

using Windows::Data::Json::IJsonValue;
using Windows::Data::Json::JsonArray;
using Windows::Data::Json::JsonObject;
using Windows::Data::Json::JsonValue;

IJsonValue field(IJsonValue const& object, wchar_t const* key) {
  if (object.ValueType() == Windows::Data::Json::JsonValueType::Object) {
    return object.GetObjectW().GetNamedValue(key, JsonValue::CreateNullValue());
  }
  return JsonValue::CreateNullValue();
}

std::wstring as_string(IJsonValue const& value, std::wstring const& fallback = L"") {
  if (value.ValueType() == Windows::Data::Json::JsonValueType::String) {
    return std::wstring(value.GetString().c_str());
  }
  return fallback;
}

std::int64_t as_int(IJsonValue const& value, std::int64_t fallback = 0) {
  if (value.ValueType() == Windows::Data::Json::JsonValueType::Number) {
    return static_cast<std::int64_t>(value.GetNumber());
  }
  return fallback;
}

double as_double(IJsonValue const& value, double fallback = 0.0) {
  if (value.ValueType() == Windows::Data::Json::JsonValueType::Number) {
    return value.GetNumber();
  }
  return fallback;
}

bool as_bool(IJsonValue const& value, bool fallback = false) {
  if (value.ValueType() == Windows::Data::Json::JsonValueType::Boolean) {
    return value.GetBoolean();
  }
  return fallback;
}

std::vector<std::uint8_t> to_bytes(std::wstring const& json) {
  auto const utf8 = wide_to_utf8(json);
  return std::vector<std::uint8_t>(utf8.begin(), utf8.end());
}

// ---------- milestone presentation -----------------------------------------

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
  Strings const& s = strings();
  if (key == L"days-100") return s.zh ? L"百日纪念" : L"100 days together";
  if (key == L"days-365") return s.zh ? L"陪伴一整年" : L"A full year";
  if (key == L"days-1000") return s.zh ? L"一千天老友" : L"1000-day friend";
  if (key == L"cpd-10") return s.zh ? L"日均低于 10 元" : L"Under 10/day";
  if (key == L"cpd-5") return s.zh ? L"日均低于 5 元" : L"Under 5/day";
  if (key == L"cpd-2") return s.zh ? L"日均低于 2 元" : L"Under 2/day";
  if (key == L"cpd-1") return s.zh ? L"日均低于 1 元" : L"Under 1/day";
  if (key == L"cpd-05") return s.zh ? L"日均低于 5 毛" : L"Under 0.50/day";
  if (key == L"paid-back") return s.paid_back();
  return key;
}

}  // namespace

// ---------- construction ----------------------------------------------------

MainWindow::MainWindow() {
  InitializeComponent();
  Title(L"Payback");
  TotalSpentLabel().Text(strings().total_spent());
  EarnedBackLabel().Text(strings().earned_back());
  OverallDailyLabel().Text(strings().overall_daily());
  DeviceCountLabel().Text(strings().device_count());
  AddButton().Content(winrt::box_value(
      winrt::hstring(L"＋ " + strings().add_device())));
  CheckUpdatesButton().Content(winrt::box_value(winrt::hstring(
      strings().zh ? L"检查更新" : L"Check Updates")));
  StatusBar().Message(L"正在启动 Racket 引擎…");
  auto const weak = get_weak();
  // Window is not a FrameworkElement; the theme lives on the content root
  // (a Grid), which exposes ActualTheme/ActualThemeChanged.
  auto contentRoot = Content().as<Microsoft::UI::Xaml::Controls::Grid>();
  contentRoot.ActualThemeChanged([weak](auto&&, auto&&) {
    if (auto window = weak.get()) {
      window->RenderDocumentFromCache();
    }
  });
  InitializeBackendAsync();
}

winrt::fire_and_forget MainWindow::InitializeBackendAsync() {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = std::make_shared<rivet::windows::Backend>(runtime_config());

  try {
    // Booting the embedded runtime can block on file I/O, so only startup is
    // moved off the UI thread. RPC traffic below is completion-driven.
    co_await winrt::resume_background();
    backend->start();

    dispatcher.TryEnqueue([weak, backend = std::move(backend)]() mutable {
      if (auto window = weak.get()) {
        window->backend_ = std::move(backend);
        window->SetReadyUi();
        window->LoadAllAsync();
      } else {
        // Never destroy the last Backend reference on its own reader thread.
        std::thread([backend = std::move(backend)]() mutable {
          backend->stop();
        }).detach();
      }
    });
  } catch (std::exception const& e) {
    auto message = std::string(e.what());
    dispatcher.TryEnqueue([weak, message = std::move(message)] {
      if (auto window = weak.get()) {
        window->SetErrorUi(message);
      }
    });
  }
}

// ---------- data loading -----------------------------------------------------

// Re-apply every static label; called on language change. The device list
// re-renders from the in-memory document (no backend round trip).
void MainWindow::ApplyLanguage() {
  Strings const& s = strings();
  TotalSpentLabel().Text(s.total_spent());
  EarnedBackLabel().Text(s.earned_back());
  OverallDailyLabel().Text(s.overall_daily());
  DeviceCountLabel().Text(s.device_count());
  AddButton().Content(winrt::box_value(
      winrt::hstring(L"＋ " + s.add_device())));
  CheckUpdatesButton().Content(winrt::box_value(
      winrt::hstring(s.zh ? L"检查更新" : L"Check Updates")));
  SortBox().Items().GetAt(0).as<Microsoft::UI::Xaml::Controls::ComboBoxItem>()
      .Content(winrt::box_value(winrt::hstring(
          s.zh ? L"加入时间" : L"Date added")));
  SortBox().Items().GetAt(1).as<Microsoft::UI::Xaml::Controls::ComboBoxItem>()
      .Content(winrt::box_value(winrt::hstring(
          s.zh ? L"日均成本" : L"Daily cost")));
  SortBox().Items().GetAt(2).as<Microsoft::UI::Xaml::Controls::ComboBoxItem>()
      .Content(winrt::box_value(winrt::hstring(
          s.zh ? L"回本进度" : L"Payback progress")));
  RenderDocumentFromCache();
}

void MainWindow::SetReadyUi() {
  StatusBar().Severity(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success);
  StatusBar().Message(L"Racket CS 就绪");
  AddButton().IsEnabled(true);
}

void MainWindow::SetErrorUi(std::string const& message) {
  StatusBar().Severity(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
  StatusBar().Message(winrt::to_hstring(message));
}

void MainWindow::ShowError(std::wstring const& message) {
  SetErrorUi(wide_to_utf8(message));
}

void MainWindow::LoadAllAsync() {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;
  if (backend == nullptr) {
    return;
  }
  try {
    rivet_app::API api(*backend);
    (void)api.load_all_async([dispatcher, weak](rivet_app::Result<rivet::Bytes> result) {
      std::vector<std::uint8_t> payload;
      std::string failure;
      try {
        payload = result.get();
      } catch (std::exception const& e) {
        failure = e.what();
      }
      dispatcher.TryEnqueue([weak, payload = std::move(payload),
                             failure = std::move(failure)] {
        if (auto window = weak.get()) {
          if (!failure.empty()) {
            window->SetErrorUi(failure);
          } else {
            window->RenderDocument(payload);
          }
        }
      });
    });
  } catch (std::exception const& e) {
    SetErrorUi(e.what());
  }
}

void MainWindow::RenderDocument(std::vector<std::uint8_t> const& payload) {
  auto const utf8_text = std::string(payload.begin(), payload.end());
  auto const wide = to_utf8_as_wide(utf8_text);
  auto parsed = JsonObject::Parse(wide);

  auto const settings = field(parsed, L"settings");
  auto const summary = field(parsed, L"summary");
  auto const devices = field(parsed, L"devices");

  document_.currency =
      as_string(field(settings, L"currency"), L"CNY");
  document_.update_auto_check = as_bool(field(settings, L"updateAutoCheck"), true);
  document_.total_spent_minor = as_int(field(summary, L"totalSpentMinor"));
  document_.earned_total_minor = as_double(field(summary, L"earnedTotalMinor"));
  document_.avg_cost_per_day_minor =
      as_double(field(summary, L"avgCostPerDayMinor"));
  document_.device_count = static_cast<int>(as_int(field(summary, L"deviceCount")));

  document_.devices.clear();
  if (devices.ValueType() == Windows::Data::Json::JsonValueType::Array) {
    for (auto const& item : devices.GetArray()) {
      DeviceRow row;
      row.id = as_string(field(item, L"id"));
      row.name = as_string(field(item, L"name"));
      row.icon = as_string(field(item, L"icon"), L"📦");
      row.category = as_string(field(item, L"category"));
      row.price_minor = as_int(field(item, L"priceMinor"));
      row.currency = as_string(field(item, L"currency"), document_.currency);
      row.purchase_date = as_string(field(item, L"purchaseDate"));
      auto const willing = field(item, L"willingPerDayMinor");
      if (willing.ValueType() == Windows::Data::Json::JsonValueType::Number) {
        row.has_willing = true;
        row.willing_per_day_minor = as_int(willing);
      }
      row.notes = as_string(field(item, L"notes"));

      auto const computed = field(item, L"computed");
      row.computed.days_held = as_int(field(computed, L"daysHeld"), 1);
      row.computed.cost_per_day_minor = as_double(field(computed, L"costPerDayMinor"));
      row.computed.willing_set = as_bool(field(computed, L"willingSet"));
      auto const earned = field(computed, L"earnedMinor");
      if (earned.ValueType() == Windows::Data::Json::JsonValueType::Number) {
        row.computed.has_earned = true;
        row.computed.earned_minor = earned.GetNumber();
      }
      auto const progress = field(computed, L"paybackProgress");
      if (progress.ValueType() == Windows::Data::Json::JsonValueType::Number) {
        row.computed.has_progress = true;
        row.computed.payback_progress = progress.GetNumber();
      }
      row.computed.payback_eta = as_string(field(computed, L"paybackEta"));
      row.computed.paid_back = as_bool(field(computed, L"paidBack"));
      auto const milestones = field(computed, L"milestones");
      if (milestones.ValueType() == Windows::Data::Json::JsonValueType::Array) {
        for (auto const& entry : milestones.GetArray()) {
          Milestone milestone;
          milestone.key = as_string(field(entry, L"key"));
          milestone.achieved = as_bool(field(entry, L"achieved"));
          row.computed.milestones.push_back(milestone);
        }
      }
      document_.devices.push_back(row);
    }
  }

  TotalSpentText().Text(money(static_cast<double>(document_.total_spent_minor),
                              document_.currency));
  EarnedBackText().Text(money(document_.earned_total_minor, document_.currency));
  OverallDailyText().Text(per_day(document_.avg_cost_per_day_minor,
                                  document_.currency));
  DeviceCountText().Text(std::to_wstring(document_.device_count));

  RenderDeviceList();
}

void MainWindow::RenderDocumentFromCache() {
  TotalSpentText().Text(money(static_cast<double>(document_.total_spent_minor),
                              document_.currency));
  EarnedBackText().Text(money(document_.earned_total_minor, document_.currency));
  OverallDailyText().Text(per_day(document_.avg_cost_per_day_minor,
                                  document_.currency));
  DeviceCountText().Text(std::to_wstring(document_.device_count));
  RenderDeviceList();
}

void MainWindow::RenderDeviceList() {
  // The SortBox raises SelectionChanged while InitializeComponent is still
  // binding x:Name members, so the panel may not exist yet; the real render
  // happens once the backend document arrives.
  if (!DeviceList()) {
    return;
  }
  DeviceList().Children().Clear();
  Strings const& s = strings();

  auto rows = document_.devices;
  std::sort(rows.begin(), rows.end(),
            [this](DeviceRow const& a, DeviceRow const& b) {
              if (sort_mode_ == 1) {
                return a.computed.cost_per_day_minor < b.computed.cost_per_day_minor;
              }
              if (sort_mode_ == 2) {
                return a.computed.payback_progress > b.computed.payback_progress;
              }
              return a.id > b.id;  // added order approximated by id sort
            });

  if (rows.empty()) {
    Microsoft::UI::Xaml::Controls::TextBlock hint;
    hint.Text(s.empty_hint());
    hint.Opacity(0.65);
    hint.Margin({0, 24, 0, 0});
    DeviceList().Children().Append(hint);
    return;
  }

  auto const make_pixel_column = [](double width) {
    Microsoft::UI::Xaml::Controls::ColumnDefinition column;
    column.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
        width, Microsoft::UI::Xaml::GridUnitType::Pixel));
    return column;
  };
  auto const make_star_column = [&]() {
    Microsoft::UI::Xaml::Controls::ColumnDefinition column;
    column.Width(Microsoft::UI::Xaml::GridLengthHelper::FromValueAndType(
        1, Microsoft::UI::Xaml::GridUnitType::Star));
    return column;
  };

  for (auto const& row : rows) {
    auto card = Microsoft::UI::Xaml::Controls::Grid();
    card.ColumnDefinitions().Append(make_pixel_column(56));
    card.ColumnDefinitions().Append(make_star_column());
    card.ColumnDefinitions().Append(make_pixel_column(160));
    card.Padding({14, 12, 14, 12});
    card.CornerRadius({12, 12, 12, 12});
    bool const dark = Content()
                          .as<Microsoft::UI::Xaml::Controls::Grid>()
                          .ActualTheme() == Microsoft::UI::Xaml::ElementTheme::Dark;
    card.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{
        dark ? Microsoft::UI::Colors::Black() : Microsoft::UI::Colors::White()});
    card.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush{
        dark ? Microsoft::UI::Colors::DimGray() : Microsoft::UI::Colors::LightGray()});
    card.BorderThickness({1, 1, 1, 1});

    Microsoft::UI::Xaml::Controls::TextBlock icon;
    icon.Text(row.icon);
    icon.FontSize(28);
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(icon, 0);
    icon.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
    card.Children().Append(icon);

    auto left = Microsoft::UI::Xaml::Controls::StackPanel();
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(left, 1);
    auto title = Microsoft::UI::Xaml::Controls::TextBlock();
    title.Text(row.name);
    title.FontSize(16);
    title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    left.Children().Append(title);

    std::wstring subtitle = s.held_for() + L" " +
                            std::to_wstring(row.computed.days_held) + L" " +
                            s.days() + L" · " +
                            money(static_cast<double>(row.price_minor), row.currency);
    auto meta = Microsoft::UI::Xaml::Controls::TextBlock();
    meta.Text(subtitle);
    meta.FontSize(12);
    meta.Opacity(0.65);
    left.Children().Append(meta);

    std::wstring badges;
    for (auto const& milestone : row.computed.milestones) {
      if (milestone.achieved) {
        badges += milestone_emoji(milestone.key) + L" ";
      }
    }
    if (!badges.empty()) {
      auto badge = Microsoft::UI::Xaml::Controls::TextBlock();
      badge.Text(badges);
      badge.FontSize(12);
      left.Children().Append(badge);
    }
    card.Children().Append(left);

    auto right = Microsoft::UI::Xaml::Controls::StackPanel();
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(right, 2);
    right.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Right);
    right.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
    auto cost = Microsoft::UI::Xaml::Controls::TextBlock();
    cost.Text(per_day(row.computed.cost_per_day_minor, row.currency));
    cost.FontSize(20);
    cost.FontWeight(Microsoft::UI::Text::FontWeights::Bold());
    cost.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Right);
    if (row.computed.paid_back) {
      cost.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
          Microsoft::UI::Colors::Green()});
    }
    right.Children().Append(cost);
    auto cost_label = Microsoft::UI::Xaml::Controls::TextBlock();
    cost_label.Text(s.daily_cost() + L" " + s.per_day());
    cost_label.FontSize(11);
    cost_label.Opacity(0.65);
    cost_label.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Right);
    right.Children().Append(cost_label);
    card.Children().Append(right);

    auto const id = row.id;
    auto const weak = get_weak();
    card.Tapped([weak, id](auto&&, auto&&) {
      if (auto window = weak.get()) {
        window->OpenDetailDialog(id);
      }
    });

    DeviceList().Children().Append(card);
  }
}

// ---------- add / edit / delete ----------------------------------------------

void MainWindow::SortBox_SelectionChanged(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  if (SortBox() == nullptr) {
    return;
  }
  auto const index = SortBox().SelectedIndex();
  if (index >= 0) {
    sort_mode_ = index;
    RenderDeviceList();
  }
}

void MainWindow::AddButton_Click(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  SaveDeviceAsync(false, L"");
}

// One form dialog serves both add and edit (update == true).
void MainWindow::SaveDeviceAsync(bool update, std::wstring id) {
  auto existing = std::make_shared<DeviceRow>();
  if (update) {
    bool found = false;
    for (auto const& row : document_.devices) {
      if (row.id == id) {
        existing = std::make_shared<DeviceRow>(row);
        found = true;
      }
    }
    if (!found) {
      return;
    }
  }

  Strings const& s = strings();
  auto panel = Microsoft::UI::Xaml::Controls::StackPanel();
  panel.Spacing(10);
  panel.Width(360);

  auto name_box = Microsoft::UI::Xaml::Controls::TextBox();
  name_box.Header(winrt::box_value(s.name()));
  name_box.Text(existing->name);
  panel.Children().Append(name_box);

  auto icon_box = Microsoft::UI::Xaml::Controls::TextBox();
  icon_box.Header(winrt::box_value(s.icon() + L" (💻📱🎧…)"));
  icon_box.Text(existing->icon);
  panel.Children().Append(icon_box);

  auto category_box = Microsoft::UI::Xaml::Controls::ComboBox();
  category_box.Header(winrt::box_value(s.category()));
  std::vector<std::wstring> const categories = {
      L"computer", L"phone", L"tablet", L"audio", L"camera",
      L"gaming", L"appliance", L"accessory", L"other"};
  int selected = 8;
  int index = 0;
  for (auto const& category : categories) {
    Microsoft::UI::Xaml::Controls::ComboBoxItem item;
    item.Content(winrt::box_value(category));
    category_box.Items().Append(item);
    if (category == existing->category) {
      selected = index;
    }
    ++index;
  }
  category_box.SelectedIndex(selected);
  panel.Children().Append(category_box);

  auto price_box = Microsoft::UI::Xaml::Controls::TextBox();
  price_box.Header(winrt::box_value(s.price() + L" (" + document_.currency + L")"));
  if (update) {
    wchar_t buffer[32];
    std::swprintf(buffer, 32, L"%.2f", existing->price_minor / 100.0);
    price_box.Text(buffer);
  }
  panel.Children().Append(price_box);

  auto date_picker = Microsoft::UI::Xaml::Controls::DatePicker();
  date_picker.Header(winrt::box_value(s.purchase_date()));
  if (update) {
    date_picker.Date(parse_date(existing->purchase_date));
  }
  panel.Children().Append(date_picker);

  auto willing_toggle = Microsoft::UI::Xaml::Controls::ToggleSwitch();
  willing_toggle.Header(winrt::box_value(s.willing_label()));
  willing_toggle.IsOn(existing->has_willing);
  panel.Children().Append(willing_toggle);

  auto willing_box = Microsoft::UI::Xaml::Controls::TextBox();
  if (update && existing->has_willing) {
    wchar_t buffer[32];
    std::swprintf(buffer, 32, L"%.2f", existing->willing_per_day_minor / 100.0);
    willing_box.Text(buffer);
  }
  panel.Children().Append(willing_box);

  auto hint = Microsoft::UI::Xaml::Controls::TextBlock();
  hint.Text(s.willing_hint());
  hint.FontSize(11);
  hint.Opacity(0.65);
  hint.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
  panel.Children().Append(hint);

  auto notes_box = Microsoft::UI::Xaml::Controls::TextBox();
  notes_box.Header(winrt::box_value(s.notes()));
  notes_box.Text(existing->notes);
  panel.Children().Append(notes_box);

  Microsoft::UI::Xaml::Controls::ContentDialog dialog;
  dialog.Title(winrt::box_value(update ? s.edit() : s.add_device()));
  dialog.Content(panel);
  dialog.PrimaryButtonText(s.save());
  dialog.CloseButtonText(s.cancel());
  dialog.DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Primary);
  dialog.XamlRoot(Content().XamlRoot());

  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;

  auto operation = dialog.ShowAsync();
  operation.Completed(
      [dispatcher, weak, backend, update, id, name_box, icon_box, category_box,
       price_box, date_picker, willing_toggle, willing_box, notes_box,
       existing](auto const& async, auto&&) {
        if (async.GetResults() !=
            Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary) {
          return;
        }
        // build the payload off the UI thread values we already hold
        auto const name = name_box.Text();
        auto const icon = icon_box.Text();
        auto const category = [&]() -> std::wstring {
          auto item = category_box.SelectedItem();
          if (item) {
            auto const value = winrt::unbox_value<winrt::hstring>(
                item.as<Microsoft::UI::Xaml::Controls::ComboBoxItem>()
                    .Content());
            return std::wstring(value.c_str());
          }
          return L"other";
        }();
        auto const price_text = price_box.Text();
        auto const purchase_date = format_date(date_picker.Date());
        auto const willing_on = willing_toggle.IsOn();
        auto const willing_text = willing_box.Text();
        auto const notes = notes_box.Text();

        auto const price_minor = static_cast<std::int64_t>(
            std::wcstod(price_text.c_str(), nullptr) * 100.0 + 0.5);
        auto const willing_minor = static_cast<std::int64_t>(
            std::wcstod(willing_text.c_str(), nullptr) * 100.0 + 0.5);

        std::wstring json = L"{";
        json += L"\"name\":" + JsonValue::CreateStringValue(name).Stringify() + L",";
        json += L"\"icon\":" + JsonValue::CreateStringValue(icon).Stringify() + L",";
        json += L"\"category\":" + JsonValue::CreateStringValue(category).Stringify() + L",";
        json += L"\"priceMinor\":" + std::to_wstring(price_minor) + L",";
        json += L"\"currency\":" +
                JsonValue::CreateStringValue(existing->currency).Stringify() + L",";
        json += L"\"purchaseDate\":" +
                JsonValue::CreateStringValue(purchase_date).Stringify() + L",";
        if (willing_on && willing_minor > 0) {
          json += L"\"willingPerDayMinor\":" + std::to_wstring(willing_minor) + L",";
        } else {
          json += L"\"willingPerDayMinor\":null,";
        }
        json += L"\"notes\":" + JsonValue::CreateStringValue(notes).Stringify();
        if (update) {
          json += L",\"id\":" + JsonValue::CreateStringValue(id).Stringify();
        }
        json += L"}";

        auto const payload = to_bytes(json);
        dispatcher.TryEnqueue([weak, backend, update, payload] {
          if (auto window = weak.get()) {
            if (backend == nullptr) {
              return;
            }
            try {
              rivet_app::API api(*backend);
              auto const callbackDispatcher = window->DispatcherQueue();
              auto const callbackWeak = window->get_weak();
              auto handler = [callbackDispatcher,
                              callbackWeak](rivet_app::Result<rivet::Bytes> result) {
                std::string failure;
                try {
                  (void)result.get();
                } catch (std::exception const& e) {
                  failure = e.what();
                }
                callbackDispatcher.TryEnqueue(
                    [callbackWeak, failure = std::move(failure)] {
                      if (auto current = callbackWeak.get()) {
                        if (!failure.empty()) {
                          current->ShowError(std::wstring(winrt::to_hstring(failure).c_str()));
                        } else {
                          current->LoadAllAsync();
                        }
                      }
                    });
              };
              if (update) {
                (void)api.update_device_async(payload, handler);
              } else {
                (void)api.add_device_async(payload, handler);
              }
            } catch (std::exception const& e) {
              window->SetErrorUi(e.what());
            }
          }
        });
      });
}

void MainWindow::DeleteDeviceAsync(std::wstring id) {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;
  if (backend == nullptr) {
    return;
  }
  try {
    rivet_app::API api(*backend);
    (void)api.delete_device_async(
        wide_to_utf8(id),
        [dispatcher, weak](rivet_app::Result<void> result) {
          std::string failure;
          try {
            result.get();
          } catch (std::exception const& e) {
            failure = e.what();
          }
          dispatcher.TryEnqueue([weak, failure = std::move(failure)] {
            if (auto window = weak.get()) {
              if (!failure.empty()) {
                window->SetErrorUi(failure);
              } else {
                window->LoadAllAsync();
              }
            }
          });
        });
  } catch (std::exception const& e) {
    SetErrorUi(e.what());
  }
}

void MainWindow::OpenDetailDialog(std::wstring id) {
  DeviceRow row;
  bool found = false;
  for (auto const& candidate : document_.devices) {
    if (candidate.id == id) {
      row = candidate;
      found = true;
    }
  }
  if (!found) {
    return;
  }

  Strings const& s = strings();
  auto panel = Microsoft::UI::Xaml::Controls::StackPanel();
  panel.Spacing(8);
  panel.Width(380);

  auto title = Microsoft::UI::Xaml::Controls::TextBlock();
  title.Text(row.icon + L" " + row.name);
  title.FontSize(20);
  title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
  panel.Children().Append(title);

  auto facts = Microsoft::UI::Xaml::Controls::TextBlock();
  facts.Text(money(static_cast<double>(row.price_minor), row.currency) + L" · " +
             row.purchase_date + L" · " + s.held_for() + L" " +
             std::to_wstring(row.computed.days_held) + L" " + s.days());
  facts.Opacity(0.7);
  facts.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
  panel.Children().Append(facts);

  if (row.computed.willing_set) {
    std::wstring line = s.payback_progress() + L": ";
    line += row.computed.paid_back
                ? L"✓ " + s.paid_back()
                : std::to_wstring(static_cast<int>(
                      row.computed.payback_progress * 100 + 0.5)) + L"%";
    if (!row.computed.paid_back && !row.computed.payback_eta.empty()) {
      line += L" · " + s.payback_eta() + L" " + row.computed.payback_eta;
    }
    auto progress = Microsoft::UI::Xaml::Controls::TextBlock();
    progress.Text(line);
    progress.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    panel.Children().Append(progress);
  }

  auto ladder_title = Microsoft::UI::Xaml::Controls::TextBlock();
  ladder_title.Text(s.milestones());
  ladder_title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
  ladder_title.Margin({0, 8, 0, 0});
  panel.Children().Append(ladder_title);

  for (auto const& milestone : row.computed.milestones) {
    auto line = Microsoft::UI::Xaml::Controls::TextBlock();
    line.Text(milestone_emoji(milestone.key) + L" " +
              milestone_label(milestone.key) +
              (milestone.achieved ? L"  ✓" : L""));
    line.Opacity(milestone.achieved ? 1.0 : 0.4);
    panel.Children().Append(line);
  }

  Microsoft::UI::Xaml::Controls::ContentDialog dialog;
  dialog.Title(winrt::box_value(row.name));
  dialog.Content(panel);
  dialog.PrimaryButtonText(s.edit());
  dialog.SecondaryButtonText(s.remove());
  dialog.CloseButtonText(s.close());
  dialog.DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Close);
  dialog.XamlRoot(Content().XamlRoot());

  auto const weak = get_weak();
  auto operation = dialog.ShowAsync();
  operation.Completed([weak, id](auto const& async, auto&&) {
    if (auto window = weak.get()) {
      auto const result = async.GetResults();
      if (result == Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary) {
        window->SaveDeviceAsync(true, id);
      } else if (result ==
                 Microsoft::UI::Xaml::Controls::ContentDialogResult::Secondary) {
        window->DeleteDeviceAsync(id);
      }
    }
  });
}

// ---------- online updates ----------------------------------------------------

void MainWindow::LanguageBox_SelectionChanged(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  if (LanguageBox() == nullptr) {
    return;
  }
  auto const index = LanguageBox().SelectedIndex();
  if (index < 0) {
    return;
  }
  // 0 system, 1 zh, 2 en -> -1 / 0 / 1
  int const mapped = (index == 0) ? -1 : (index - 1);
  g_language_override.store(mapped, std::memory_order_relaxed);
  ApplyLanguage();
}

void MainWindow::CheckUpdates_Click(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  CheckUpdatesAsync();
}

void MainWindow::CheckUpdatesAsync() {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;
  if (backend == nullptr) {
    return;
  }
  StatusBar().Severity(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational);
  StatusBar().Message(L"正在检查更新…");
  try {
    rivet_app::API api(*backend);
    (void)api.check_updates_async(
        true,
        [dispatcher, weak](rivet_app::Result<rivet::Bytes> result) {
          std::vector<std::uint8_t> payload;
          std::string failure;
          try {
            payload = result.get();
          } catch (std::exception const& e) {
            failure = e.what();
          }
          dispatcher.TryEnqueue([weak, payload = std::move(payload),
                                 failure = std::move(failure)] {
            if (auto window = weak.get()) {
              if (!failure.empty()) {
                window->SetErrorUi(failure);
                return;
              }
              auto const utf8_text = std::string(payload.begin(), payload.end());
              auto const check = JsonObject::Parse(to_utf8_as_wide(utf8_text));
              auto const status = as_string(field(check, L"status"));
              if (status == L"available") {
                window->StartDownloadAsync();
              } else {
                Strings const& s = strings();
                Microsoft::UI::Xaml::Controls::ContentDialog dialog;
                dialog.Title(winrt::box_value(winrt::hstring(s.updates())));
                dialog.Content(winrt::box_value(winrt::hstring(
                    status == L"up-to-date" ? s.up_to_date()
                                            : s.update_error())));
                dialog.CloseButtonText(s.close());
                dialog.XamlRoot(window->Content().XamlRoot());
                (void)dialog.ShowAsync();
              }
            }
          });
        });
  } catch (std::exception const& e) {
    SetErrorUi(e.what());
  }
}

void MainWindow::StartDownloadAsync() {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;
  if (backend == nullptr) {
    return;
  }

  Strings const& s = strings();
  auto panel = Microsoft::UI::Xaml::Controls::StackPanel();
  panel.Spacing(8);
  auto percent = std::make_shared<Microsoft::UI::Xaml::Controls::ProgressBar>();
  percent->Width(300);
  panel.Children().Append(*percent);
  auto label = Microsoft::UI::Xaml::Controls::TextBlock();
  label.Text(s.downloading());
  panel.Children().Append(label);

  update_dialog_ = std::make_shared<Microsoft::UI::Xaml::Controls::ContentDialog>();
  update_dialog_->Title(winrt::box_value(s.update_available()));
  update_dialog_->Content(panel);
  update_dialog_->CloseButtonText(s.cancel());
  update_dialog_->XamlRoot(Content().XamlRoot());
  update_percent_ = percent;
  update_dialog_operation_ =
      std::make_shared<Windows::Foundation::IAsyncOperation<
          Microsoft::UI::Xaml::Controls::ContentDialogResult>>(
          update_dialog_->ShowAsync());

  try {
    rivet_app::API api(*backend);
    (void)api.start_download_async([dispatcher, weak](rivet_app::Result<void> result) {
      std::string failure;
      try {
        result.get();
      } catch (std::exception const& e) {
        failure = e.what();
      }
      if (!failure.empty()) {
        dispatcher.TryEnqueue([weak, failure] {
          if (auto window = weak.get()) {
            window->SetErrorUi(failure);
          }
        });
      }
    });
  } catch (std::exception const& e) {
    SetErrorUi(e.what());
    return;
  }

  // poll update-state while the background thread downloads
  update_timer_ = std::make_shared<Microsoft::UI::Xaml::DispatcherTimer>();
  update_timer_->Interval(std::chrono::milliseconds{400});
  update_timer_->Tick([weak, dispatcher, backend](auto&&, auto&&) {
    try {
      rivet_app::API api(*backend);
      (void)api.update_state_async(
          [dispatcher, weak](rivet_app::Result<rivet::Bytes> result) {
            std::vector<std::uint8_t> payload;
            try {
              payload = result.get();
            } catch (...) {
            }
            dispatcher.TryEnqueue([weak, payload = std::move(payload)] {
              if (auto window = weak.get()) {
                window->HandleUpdatePoll(payload);
              }
            });
          });
    } catch (...) {
    }
  });
  update_timer_->Start();
}

void MainWindow::HandleUpdatePoll(std::vector<std::uint8_t> const& payload) {
  if (payload.empty() || update_dialog_ == nullptr) {
    return;
  }
  auto const utf8_text = std::string(payload.begin(), payload.end());
  auto const state = JsonObject::Parse(to_utf8_as_wide(utf8_text));
  auto const phase = as_string(field(state, L"phase"));
  if (update_percent_ != nullptr) {
    update_percent_->Value(
        static_cast<double>(as_int(field(state, L"percent"))));
  }
  if (phase == L"downloaded") {
    if (update_timer_) update_timer_->Stop();
    if (update_dialog_operation_) update_dialog_operation_->Cancel();
    update_dialog_ = nullptr;
    InstallDownloadedUpdate(as_string(field(state, L"downloadedPath")));
  } else if (phase == L"error") {
    if (update_timer_) update_timer_->Stop();
    if (update_dialog_operation_) update_dialog_operation_->Cancel();
    update_dialog_ = nullptr;
    ShowError(as_string(field(state, L"message")));
  }
}

void MainWindow::InstallDownloadedUpdate(std::wstring path) {
  // The signed MSI supplies transactional rollback; launching it replaces
  // the app (docs/updates.md).
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_DEFAULT;
  info.lpVerb = L"open";
  info.lpFile = L"msiexec.exe";
  info.lpParameters = (L"/i \"" + path + L"\"").c_str();
  info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&info)) {
    ShowError(L"msiexec launch failed");
    return;
  }
  winrt::Microsoft::UI::Xaml::Application::Current().Exit();
}

}  // namespace winrt::RivetHost::implementation
