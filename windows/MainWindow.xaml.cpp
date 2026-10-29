#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "GeneratedBackend.hpp"
#include "HostHelpers.h"

#include <cmath>
#include <stdexcept>

namespace winrt::RivetHost::implementation {
namespace {

using namespace payback::host;
using payback::strings::Strings;

using Windows::Data::Json::JsonObject;
using Windows::Data::Json::JsonValue;

// Badge backdrop hue follows the device category, mirroring the mac
// DeviceIconBadge; "other" (and anything unknown) wears the accent soft tint.
winrt::Microsoft::UI::Xaml::Media::Brush category_brush(
    winrt::Microsoft::UI::Xaml::Controls::Grid const& contentRoot,
    std::wstring const& category) {
  wchar_t const* key = L"PaybackAccentSoftBrush";
  if (category == L"computer") {
    key = L"PaybackCatComputerBrush";
  } else if (category == L"phone") {
    key = L"PaybackCatPhoneBrush";
  } else if (category == L"tablet") {
    key = L"PaybackCatTabletBrush";
  } else if (category == L"watch") {
    key = L"PaybackCatWatchBrush";
  } else if (category == L"audio") {
    key = L"PaybackCatAudioBrush";
  } else if (category == L"camera") {
    key = L"PaybackCatCameraBrush";
  } else if (category == L"gaming") {
    key = L"PaybackCatGamingBrush";
  } else if (category == L"appliance") {
    key = L"PaybackCatApplianceBrush";
  } else if (category == L"accessory") {
    key = L"PaybackCatAccessoryBrush";
  }
  return theme_brush(contentRoot, key);
}

// Rounded emoji tile (mac DeviceIconBadge): the corner radius tracks the
// size so small sidebar badges and the large detail badge keep one shape.
winrt::Microsoft::UI::Xaml::Controls::Border make_badge(
    winrt::Microsoft::UI::Xaml::Controls::Grid const& contentRoot,
    std::wstring const& icon, std::wstring const& category, double size) {
  winrt::Microsoft::UI::Xaml::Controls::Border badge;
  badge.Width(size);
  badge.Height(size);
  double const corner = size * 0.26;
  badge.CornerRadius(
      winrt::Microsoft::UI::Xaml::CornerRadius{corner, corner, corner, corner});
  badge.Background(category_brush(contentRoot, category));
  winrt::Microsoft::UI::Xaml::Controls::TextBlock glyph;
  glyph.Text(icon);
  glyph.FontSize(size * 0.55);
  glyph.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);
  glyph.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
  badge.Child(glyph);
  return badge;
}

}  // namespace

// ---------- construction ----------------------------------------------------

MainWindow::MainWindow() {
  payback::host::load_language_override();
  InitializeComponent();
  Title(L"Payback");
  // Same floor as the macOS host; below it the summary grid and the toolbar
  // start crowding each other instead of reflowing.
  if (auto const presenter =
          AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>()) {
    presenter.PreferredMinimumWidth(980);
    presenter.PreferredMinimumHeight(600);
  }
  StatusMessage().Text(strings().starting_backend());
  ApplyLanguage();
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

// ---------- language ----------------------------------------------------------

// Re-apply every static label; called on language change and at startup (the
// persisted override must win over the XAML's zh defaults). The device list
// re-renders from the in-memory document (no backend round trip).
void MainWindow::ApplyLanguage() {
  Strings const& s = strings();
  TotalSpentLabel().Text(s.total_spent());
  EarnedBackLabel().Text(s.earned_back());
  OverallDailyLabel().Text(s.overall_daily());
  DeviceCountLabel().Text(s.device_count());
  // borderless toolbar icons: the string acts as both label and tooltip
  auto const label_button = [](auto const& button, std::wstring const& text) {
    button.Label(winrt::hstring(text));
    Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
        button, winrt::box_value(winrt::hstring(text)));
  };
  label_button(AddButton(), s.add_device());
  label_button(EditButton(), s.edit());
  label_button(DeleteButton(), s.remove());
  label_button(SettingsButton(), s.settings_title());
  label_button(HelpButton(), s.help_menu());
  MenuCheckUpdates().Text(winrt::hstring(s.check_updates_menu()));
  MenuAbout().Text(winrt::hstring(s.about_menu()));
  SortBox().Items().GetAt(0).as<Microsoft::UI::Xaml::Controls::ComboBoxItem>()
      .Content(winrt::box_value(winrt::hstring(s.sort_added())));
  SortBox().Items().GetAt(1).as<Microsoft::UI::Xaml::Controls::ComboBoxItem>()
      .Content(winrt::box_value(winrt::hstring(s.sort_daily_cost())));
  SortBox().Items().GetAt(2).as<Microsoft::UI::Xaml::Controls::ComboBoxItem>()
      .Content(winrt::box_value(winrt::hstring(s.sort_payback())));
  EmptyTitleText().Text(s.empty_title());
  EmptyHintText().Text(s.empty_hint());
  EmptyAddButton().Content(winrt::box_value(winrt::hstring(s.add_device())));
  ApplyQuip();
  RenderDocumentFromCache();
}

void MainWindow::ApplyQuip() {
  if (QuipText() == nullptr) {
    return;
  }
  if (document_.devices.empty()) {
    QuipText().Text(L"");
    return;
  }
  QuipText().Text(L"✦ " + daily_quip(quip_day_));
}

void MainWindow::SetReadyUi() {
  // "backend ready" is not news worth a permanent strip: the bar only
  // exists while it carries something actionable (errors, update progress,
  // the daily digest), then it gives the row back to the device list.
  StatusBar().IsOpen(false);
  AddButton().IsEnabled(true);
  EmptyAddButton().IsEnabled(true);
  UpdateToolbarEnabled();
  StartSyncPolling();
}

void MainWindow::SetErrorUi(std::string const& message) {
  StatusBar().IsOpen(true);
  StatusBar().Severity(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
  StatusMessage().Text(winrt::to_hstring(message));
}

void MainWindow::ShowSuccess(std::wstring const& message) {
  StatusBar().IsOpen(true);
  StatusBar().Severity(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success);
  StatusMessage().Text(message);
}

void MainWindow::ShowError(std::wstring const& message) {
  SetErrorUi(wide_to_utf8(message));
}

// ---------- data loading -----------------------------------------------------

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
  document_.currency_groups.clear();
  auto const groups = field(summary, L"currencyGroups");
  if (groups.ValueType() == Windows::Data::Json::JsonValueType::Array) {
    for (auto const& item : groups.GetArray()) {
      if (item.ValueType() != Windows::Data::Json::JsonValueType::Object) {
        continue;
      }
      document_.currency_groups.push_back(MainWindow::CurrencyGroup{
          as_string(field(item, L"currency")),
          as_int(field(item, L"totalSpentMinor")),
          as_double(field(item, L"earnedTotalMinor")),
          as_double(field(item, L"avgCostPerDayMinor"))});
    }
  }

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
      row.created_at = as_string(field(item, L"createdAt"));
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
          milestone.is_new = as_bool(field(entry, L"new"));
          row.computed.milestones.push_back(milestone);
        }
      }
      document_.devices.push_back(row);
    }
  }

  TotalSpentText().Text(document_.mixed_currency()
                            ? joined_spent()
                            : money(static_cast<double>(document_.total_spent_minor),
                                    document_.currency));
  EarnedBackText().Text(document_.mixed_currency()
                            ? joined_earned()
                            : money(document_.earned_total_minor,
                                    document_.currency));
  TintEarnedBack();
  OverallDailyText().Text(document_.mixed_currency()
                              ? joined_daily()
                              : per_day(document_.avg_cost_per_day_minor,
                                        document_.currency));
  DeviceCountText().Text(std::to_wstring(document_.device_count));

  RenderDeviceList();
  ApplyQuip();
  ShowCelebrations();

  ReportFailedInstall();

  // the backend throttles checks to once a day; a launch-time check stays
  // silent unless an update is available (then the consent dialog shows)
  if (!auto_check_done_) {
    auto_check_done_ = true;
    if (document_.update_auto_check) {
      AutoCheckUpdatesAsync();
    }
  }
  RunDailyDigest();
}

void MainWindow::RenderDocumentFromCache() {
  TotalSpentText().Text(document_.mixed_currency()
                            ? joined_spent()
                            : money(static_cast<double>(document_.total_spent_minor),
                                    document_.currency));
  EarnedBackText().Text(document_.mixed_currency()
                            ? joined_earned()
                            : money(document_.earned_total_minor,
                                    document_.currency));
  TintEarnedBack();
  OverallDailyText().Text(document_.mixed_currency()
                              ? joined_daily()
                              : per_day(document_.avg_cost_per_day_minor,
                                        document_.currency));
  DeviceCountText().Text(std::to_wstring(document_.device_count));
  RenderDeviceList();
}

// the earn green appears exactly when value has been earned back (mac
// StatTile tint rule, docs/design.md) — never on a zero summary
void MainWindow::TintEarnedBack() {
  auto const contentRoot = Content().as<Microsoft::UI::Xaml::Controls::Grid>();
  EarnedBackText().Foreground(document_.earned_total_minor > 0
                                  ? theme_brush(contentRoot, L"PaybackEarnBrush")
                                  : Microsoft::UI::Xaml::Media::Brush{nullptr});
}

// Mixed portfolios never show one combined money value: the backend nulls
// the combined scalars and the tiles join the per-currency groups (#23).
std::wstring MainWindow::joined_spent() const {
  std::wstring joined;
  for (auto const& group : document_.currency_groups) {
    if (!joined.empty()) joined += L" · ";
    joined += money(static_cast<double>(group.total_spent_minor), group.currency);
  }
  return joined;
}

std::wstring MainWindow::joined_earned() const {
  std::wstring joined;
  for (auto const& group : document_.currency_groups) {
    if (!joined.empty()) joined += L" · ";
    joined += money(group.earned_total_minor, group.currency);
  }
  return joined;
}

std::wstring MainWindow::joined_daily() const {
  std::wstring joined;
  for (auto const& group : document_.currency_groups) {
    if (!joined.empty()) joined += L" · ";
    joined += per_day(group.avg_cost_per_day_minor, group.currency);
  }
  return joined;
}

// ---------- sidebar: device rows ----------------------------------------------

// Apple's answer to "browse a collection, inspect one item" is the nonmodal
// split (mac NavigationSplitView parity): each device is a selectable pane
// row, selection drives the detail pane in place, and the rich surface
// (curve, progress, milestones) lives one selection away in the detail.
void MainWindow::RenderDeviceList() {
  // The SortBox raises SelectionChanged while InitializeComponent is still
  // binding x:Name members, so the pane may not exist yet; the real render
  // happens once the backend document arrives.
  if (!NavView()) {
    return;
  }
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
              // "yyyy-MM-dd HH:mm:ss" compares chronologically as text
              return a.created_at > b.created_at;
            });

  bool const empty = rows.empty();
  // the empty state replaces the whole split, the way the mac host does
  NavView().Visibility(empty ? Microsoft::UI::Xaml::Visibility::Collapsed
                             : Microsoft::UI::Xaml::Visibility::Visible);
  EmptyState().Visibility(empty ? Microsoft::UI::Xaml::Visibility::Visible
                                : Microsoft::UI::Xaml::Visibility::Collapsed);

  auto const contentRoot = Content().as<Microsoft::UI::Xaml::Controls::Grid>();
  auto items = NavView().MenuItems();
  suppress_selection_changed_ = true;
  items.Clear();

  for (auto const& row : rows) {
    auto item = Microsoft::UI::Xaml::Controls::NavigationViewItem();

    auto content = Microsoft::UI::Xaml::Controls::Grid();
    content.Padding({0, 2, 0, 2});
    content.ColumnDefinitions().Append(pixel_column(40));
    content.ColumnDefinitions().Append(star_column());
    content.ColumnDefinitions().Append(auto_column());

    auto badge = make_badge(contentRoot, row.icon, row.category, 34);
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(badge, 0);
    badge.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
    content.Children().Append(badge);

    auto text = Microsoft::UI::Xaml::Controls::StackPanel();
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(text, 1);
    text.Margin({8, 0, 0, 0});
    text.Spacing(2);
    text.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
    auto name = Microsoft::UI::Xaml::Controls::TextBlock();
    name.Text(row.name);
    name.FontSize(14);
    name.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    name.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
    text.Children().Append(name);
    auto cost = Microsoft::UI::Xaml::Controls::TextBlock();
    cost.Text(per_day(row.computed.cost_per_day_minor, row.currency));
    cost.FontSize(12);
    cost.Opacity(0.65);
    text.Children().Append(cost);
    content.Children().Append(text);

    if (row.has_willing) {
      auto ring = Microsoft::UI::Xaml::Controls::ProgressRing();
      ring.Width(26);
      ring.Height(26);
      ring.IsIndeterminate(false);
      ring.Value(std::min(std::max(row.computed.payback_progress, 0.0), 1.0) * 100);
      ring.Foreground(row.computed.paid_back
                          ? theme_brush(contentRoot, L"PaybackEarnBrush")
                          : theme_brush(contentRoot, L"PaybackAccentBrush"));
      ring.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
      ring.Margin({8, 0, 0, 0});
      Microsoft::UI::Xaml::Controls::Grid::SetColumn(ring, 2);
      content.Children().Append(ring);
    } else {
      // the last two achieved milestones, mac SidebarDeviceRow parity
      std::vector<std::wstring> achieved;
      for (auto const& milestone : row.computed.milestones) {
        if (milestone.achieved) {
          achieved.push_back(milestone_emoji(milestone.key));
        }
      }
      std::wstring badges;
      for (std::size_t i = achieved.size() > 2 ? achieved.size() - 2 : 0;
           i < achieved.size(); ++i) {
        badges += achieved[i];
      }
      if (!badges.empty()) {
        auto emoji = Microsoft::UI::Xaml::Controls::TextBlock();
        emoji.Text(badges);
        emoji.FontSize(13);
        emoji.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
        emoji.Margin({8, 0, 0, 0});
        Microsoft::UI::Xaml::Controls::Grid::SetColumn(emoji, 2);
        content.Children().Append(emoji);
      }
    }

    // right-tap row menu: edit / delete, delete painted destructive
    auto const id = row.id;
    auto const weak = get_weak();
    auto menu = Microsoft::UI::Xaml::Controls::MenuFlyout();
    auto edit_item = Microsoft::UI::Xaml::Controls::MenuFlyoutItem();
    edit_item.Text(s.edit());
    edit_item.Click([weak, id](auto const&, auto const&) {
      if (auto window = weak.get()) {
        window->SaveDeviceAsync(true, id);
      }
    });
    auto remove_item = Microsoft::UI::Xaml::Controls::MenuFlyoutItem();
    remove_item.Text(s.remove());
    remove_item.Foreground(destructive_brush());
    remove_item.Click([weak, id](auto const&, auto const&) {
      if (auto window = weak.get()) {
        window->ShowDeleteConfirm(id);
      }
    });
    menu.Items().Append(edit_item);
    menu.Items().Append(remove_item);
    item.ContextFlyout(menu);

    item.Content(content);
    item.Tag(winrt::box_value(winrt::hstring(id)));
    items.Append(item);
  }

  // selection survives re-renders; a stale or fresh selection falls back to
  // the first device so the detail pane is never blank while devices exist
  std::wstring next;
  for (auto const& row : rows) {
    if (row.id == selected_device_id_) {
      next = row.id;
      break;
    }
  }
  if (next.empty() && !rows.empty()) {
    next = rows.front().id;
  }
  selected_device_id_ = next;
  if (next.empty()) {
    NavView().SelectedItem(nullptr);
  } else {
    for (std::uint32_t i = 0; i < items.Size(); ++i) {
      auto const candidate =
          items.GetAt(i).try_as<Microsoft::UI::Xaml::Controls::NavigationViewItem>();
      auto const tag = winrt::unbox_value_or<winrt::hstring>(candidate.Tag(), {});
      if (std::wstring(tag.c_str()) == next) {
        NavView().SelectedItem(candidate);
        break;
      }
    }
  }
  suppress_selection_changed_ = false;

  RenderDetail();
}

void MainWindow::NavView_SelectionChanged(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args) {
  if (suppress_selection_changed_) {
    return;
  }
  auto const item =
      args.SelectedItem().try_as<Microsoft::UI::Xaml::Controls::NavigationViewItem>();
  if (!item) {
    return;
  }
  auto const tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), {});
  std::wstring const id(tag.c_str());
  if (id == selected_device_id_) {
    return;
  }
  selected_device_id_ = id;
  RenderDetail();
}

// ---------- detail pane -------------------------------------------------------

// The detail pane is pure content (mac DeviceDetail parity): identity header,
// stat tiles, cost curve, payback progress, milestone ladder. Its actions
// live in the toolbar and the row context menu, keeping this surface clean.
void MainWindow::RenderDetail() {
  if (!DetailPanel()) {
    return;
  }
  DetailPanel().Children().Clear();
  curve_ = nullptr;

  // nonmodal fallback: a stale or fresh selection reads as the first device,
  // so the pane is never blank while devices exist
  DeviceRow const* row = nullptr;
  for (auto const& candidate : document_.devices) {
    if (candidate.id == selected_device_id_) {
      row = &candidate;
      break;
    }
  }
  if (row == nullptr && !document_.devices.empty()) {
    row = &document_.devices.front();
  }
  UpdateToolbarEnabled();
  if (row == nullptr) {
    return;
  }

  Strings const& s = strings();
  auto const contentRoot = Content().as<Microsoft::UI::Xaml::Controls::Grid>();
  auto children = DetailPanel().Children();

  auto const make_card = [&contentRoot]() {
    Microsoft::UI::Xaml::Controls::Border card;
    card.CornerRadius({16, 16, 16, 16});
    card.Background(theme_brush(contentRoot, L"PaybackCardBrush"));
    card.BorderBrush(theme_brush(contentRoot, L"PaybackLineBrush"));
    card.BorderThickness({1, 1, 1, 1});
    card.Padding({14, 14, 14, 14});
    return card;
  };

  // ----- header: badge + name + price · date + paid-back capsule -----
  auto header = Microsoft::UI::Xaml::Controls::Grid();
  header.ColumnDefinitions().Append(star_column());
  header.ColumnDefinitions().Append(auto_column());

  auto identity = Microsoft::UI::Xaml::Controls::StackPanel();
  identity.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
  identity.Spacing(14);
  identity.Children().Append(make_badge(contentRoot, row->icon, row->category, 54));
  auto names = Microsoft::UI::Xaml::Controls::StackPanel();
  names.Spacing(2);
  names.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
  auto title = Microsoft::UI::Xaml::Controls::TextBlock();
  title.Text(row->name);
  title.FontSize(22);
  title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
  title.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
  names.Children().Append(title);
  auto facts = Microsoft::UI::Xaml::Controls::TextBlock();
  facts.Text(money(static_cast<double>(row->price_minor), row->currency) + L" · " +
             row->purchase_date);
  facts.FontSize(13);
  facts.Opacity(0.7);
  names.Children().Append(facts);
  identity.Children().Append(names);
  Microsoft::UI::Xaml::Controls::Grid::SetColumn(identity, 0);
  header.Children().Append(identity);

  if (row->computed.paid_back) {
    auto capsule = Microsoft::UI::Xaml::Controls::Border();
    capsule.CornerRadius({16, 16, 16, 16});
    capsule.Background(theme_brush(contentRoot, L"PaybackEarnSoftBrush"));
    capsule.Padding({12, 6, 12, 6});
    capsule.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
    auto capsule_content = Microsoft::UI::Xaml::Controls::StackPanel();
    capsule_content.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
    capsule_content.Spacing(4);
    auto party = Microsoft::UI::Xaml::Controls::TextBlock();
    party.Text(L"🎉");
    auto paid = Microsoft::UI::Xaml::Controls::TextBlock();
    paid.Text(s.paid_back());
    paid.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    paid.Foreground(theme_brush(contentRoot, L"PaybackEarnBrush"));
    capsule_content.Children().Append(party);
    capsule_content.Children().Append(paid);
    capsule.Child(capsule_content);
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(capsule, 1);
    header.Children().Append(capsule);
  }
  children.Append(header);

  // ----- stat tiles: daily cost / days held / willingness -----
  auto tiles = Microsoft::UI::Xaml::Controls::Grid();
  tiles.ColumnSpacing(10);
  std::size_t tile_index = 0;
  auto const add_tile = [&](std::wstring const& label, std::wstring const& value,
                            winrt::Microsoft::UI::Xaml::Media::Brush const& tint = {nullptr}) {
    auto tile = Microsoft::UI::Xaml::Controls::Border();
    tile.CornerRadius({14, 14, 14, 14});
    tile.Background(theme_brush(contentRoot, L"PaybackCardBrush"));
    tile.BorderBrush(theme_brush(contentRoot, L"PaybackLineBrush"));
    tile.BorderThickness({1, 1, 1, 1});
    tile.Padding({16, 12, 16, 12});
    auto stack = Microsoft::UI::Xaml::Controls::StackPanel();
    stack.Spacing(3);
    auto caption = Microsoft::UI::Xaml::Controls::TextBlock();
    caption.Text(label);
    caption.FontSize(12);
    caption.Opacity(0.65);
    stack.Children().Append(caption);
    auto number = Microsoft::UI::Xaml::Controls::TextBlock();
    number.Text(value);
    number.FontSize(20);
    number.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    number.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
    if (tint) {
      number.Foreground(tint);
    }
    stack.Children().Append(number);
    tile.Child(stack);
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(
        tile, static_cast<std::int32_t>(tile_index));
    tiles.ColumnDefinitions().Append(star_column());
    tiles.Children().Append(tile);
    ++tile_index;
  };
  add_tile(s.daily_cost(), per_day(row->computed.cost_per_day_minor, row->currency),
           row->computed.paid_back
               ? theme_brush(contentRoot, L"PaybackEarnBrush")
               : winrt::Microsoft::UI::Xaml::Media::Brush{nullptr});
  add_tile(s.held_for(), std::to_wstring(row->computed.days_held) + L" " + s.days());
  if (row->has_willing) {
    add_tile(s.willing_per_day(),
             money(static_cast<double>(row->willing_per_day_minor), row->currency));
  }
  children.Append(tiles);

  auto const accent_color = theme_brush(contentRoot, L"PaybackAccentBrush")
                                .as<winrt::Microsoft::UI::Xaml::Media::SolidColorBrush>()
                                .Color();
  auto const accent_brush = [&accent_color]() {
    return winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{accent_color};
  };
  auto const with_alpha = [&accent_color](std::uint8_t alpha) {
    winrt::Windows::UI::Color color = accent_color;
    color.A = alpha;
    return color;
  };

  // ----- cost curve card: a daily-cost line that only ever falls (mac
  // CostCurve) — gradient stroke over a soft area fill -----
  auto curve_card = make_card();
  auto curve_stack = Microsoft::UI::Xaml::Controls::StackPanel();
  curve_stack.Spacing(6);
  auto curve_head = Microsoft::UI::Xaml::Controls::Grid();
  curve_head.ColumnDefinitions().Append(star_column());
  curve_head.ColumnDefinitions().Append(auto_column());
  auto curve_label = Microsoft::UI::Xaml::Controls::TextBlock();
  curve_label.Text(s.daily_cost());
  curve_label.FontSize(12);
  curve_label.Opacity(0.65);
  Microsoft::UI::Xaml::Controls::Grid::SetColumn(curve_label, 0);
  curve_head.Children().Append(curve_label);
  auto curve_value = Microsoft::UI::Xaml::Controls::TextBlock();
  curve_value.Text(per_day(row->computed.cost_per_day_minor, row->currency));
  curve_value.FontSize(12);
  curve_value.FontWeight(Microsoft::UI::Text::FontWeights::Bold());
  curve_value.Foreground(accent_brush());
  Microsoft::UI::Xaml::Controls::Grid::SetColumn(curve_value, 1);
  curve_head.Children().Append(curve_value);
  curve_stack.Children().Append(curve_head);

  auto canvas = Microsoft::UI::Xaml::Controls::Canvas();
  canvas.Height(64);
  curve_ = std::make_shared<CurveState>();
  curve_->price_minor = row->price_minor;
  curve_->days_held = row->computed.days_held;
  curve_->canvas = canvas;

  auto fill_shape = Microsoft::UI::Xaml::Shapes::Polygon();
  auto fill_brush = winrt::Microsoft::UI::Xaml::Media::LinearGradientBrush();
  fill_brush.StartPoint({0, 0});
  fill_brush.EndPoint({0, 1});
  winrt::Microsoft::UI::Xaml::Media::GradientStop fill_top;
  fill_top.Color(with_alpha(0x38));
  fill_top.Offset(0.0);
  winrt::Microsoft::UI::Xaml::Media::GradientStop fill_bottom;
  fill_bottom.Color(with_alpha(0x05));
  fill_bottom.Offset(1.0);
  fill_brush.GradientStops().Append(fill_top);
  fill_brush.GradientStops().Append(fill_bottom);
  fill_shape.Fill(fill_brush);

  auto line_shape = Microsoft::UI::Xaml::Shapes::Polyline();
  auto stroke_brush = winrt::Microsoft::UI::Xaml::Media::LinearGradientBrush();
  stroke_brush.StartPoint({0, 0});
  stroke_brush.EndPoint({1, 0});
  winrt::Microsoft::UI::Xaml::Media::GradientStop stroke_start;
  stroke_start.Color(with_alpha(0x80));
  stroke_start.Offset(0.0);
  winrt::Microsoft::UI::Xaml::Media::GradientStop stroke_end;
  stroke_end.Color(with_alpha(0xFF));
  stroke_end.Offset(1.0);
  stroke_brush.GradientStops().Append(stroke_start);
  stroke_brush.GradientStops().Append(stroke_end);
  line_shape.Stroke(stroke_brush);
  line_shape.StrokeThickness(2.5);
  line_shape.StrokeStartLineCap(Microsoft::UI::Xaml::Media::PenLineCap::Round);
  line_shape.StrokeEndLineCap(Microsoft::UI::Xaml::Media::PenLineCap::Round);
  line_shape.StrokeLineJoin(Microsoft::UI::Xaml::Media::PenLineJoin::Round);

  auto dot = Microsoft::UI::Xaml::Shapes::Ellipse();
  dot.Width(7);
  dot.Height(7);
  dot.Fill(accent_brush());

  curve_->fill = fill_shape;
  curve_->line = line_shape;
  curve_->dot = dot;
  canvas.Children().Append(fill_shape);
  canvas.Children().Append(line_shape);
  canvas.Children().Append(dot);
  canvas.SizeChanged([weak = get_weak(), curve = curve_](auto&&, auto&&) {
    if (auto window = weak.get()) {
      window->UpdateCurveGeometry(*curve);
    }
  });
  curve_stack.Children().Append(canvas);

  auto day_labels = Microsoft::UI::Xaml::Controls::Grid();
  day_labels.ColumnDefinitions().Append(star_column());
  day_labels.ColumnDefinitions().Append(auto_column());
  auto day_first = Microsoft::UI::Xaml::Controls::TextBlock();
  day_first.Text(replace_all(s.curve_day_label(), L"{day}", L"1"));
  day_first.FontSize(11);
  day_first.Opacity(0.65);
  Microsoft::UI::Xaml::Controls::Grid::SetColumn(day_first, 0);
  day_labels.Children().Append(day_first);
  auto day_last = Microsoft::UI::Xaml::Controls::TextBlock();
  day_last.Text(replace_all(s.curve_day_label(), L"{day}",
                            std::to_wstring(row->computed.days_held)));
  day_last.FontSize(11);
  day_last.Opacity(0.65);
  Microsoft::UI::Xaml::Controls::Grid::SetColumn(day_last, 1);
  day_labels.Children().Append(day_last);
  curve_stack.Children().Append(day_labels);
  curve_card.Child(curve_stack);
  children.Append(curve_card);

  // ----- payback progress card (or the willing hint card) -----
  if (row->has_willing) {
    double const progress =
        std::min(std::max(row->computed.payback_progress, 0.0), 1.0);
    auto progress_card = make_card();
    if (row->computed.paid_back) {
      progress_card.Background(theme_brush(contentRoot, L"PaybackEarnSoftBrush"));
    }
    auto stack = Microsoft::UI::Xaml::Controls::StackPanel();
    stack.Spacing(8);

    auto head = Microsoft::UI::Xaml::Controls::Grid();
    head.ColumnDefinitions().Append(star_column());
    head.ColumnDefinitions().Append(auto_column());
    auto head_left = Microsoft::UI::Xaml::Controls::StackPanel();
    head_left.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
    head_left.Spacing(10);
    auto ring = Microsoft::UI::Xaml::Controls::ProgressRing();
    ring.Width(38);
    ring.Height(38);
    ring.IsIndeterminate(false);
    ring.Value(progress * 100);
    ring.Foreground(row->computed.paid_back
                        ? theme_brush(contentRoot, L"PaybackEarnBrush")
                        : accent_brush());
    head_left.Children().Append(ring);
    auto ring_label = Microsoft::UI::Xaml::Controls::TextBlock();
    ring_label.Text(s.payback_progress());
    ring_label.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
    head_left.Children().Append(ring_label);
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(head_left, 0);
    head.Children().Append(head_left);
    auto head_right = Microsoft::UI::Xaml::Controls::TextBlock();
    head_right.Text(row->computed.paid_back
                        ? s.paid_back()
                        : std::to_wstring(static_cast<int>(progress * 100 + 0.5)) + L"%");
    head_right.FontWeight(Microsoft::UI::Text::FontWeights::Bold());
    head_right.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
    head_right.Foreground(row->computed.paid_back
                              ? theme_brush(contentRoot, L"PaybackEarnBrush")
                              : accent_brush());
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(head_right, 1);
    head.Children().Append(head_right);
    stack.Children().Append(head);

    auto bar = Microsoft::UI::Xaml::Controls::ProgressBar();
    bar.Maximum(100);
    bar.Value(progress * 100);
    bar.Foreground(row->computed.paid_back
                       ? theme_brush(contentRoot, L"PaybackEarnBrush")
                       : accent_brush());
    stack.Children().Append(bar);

    std::wstring caption;
    if (row->computed.paid_back) {
      if (row->computed.has_earned) {
        caption = s.earned_so_far() + L" " +
                  money(row->computed.earned_minor, row->currency);
      }
    } else if (!row->computed.payback_eta.empty()) {
      caption = s.payback_eta() + L": " + row->computed.payback_eta;
    }
    if (!caption.empty()) {
      auto caption_block = Microsoft::UI::Xaml::Controls::TextBlock();
      caption_block.Text(caption);
      caption_block.FontSize(12);
      caption_block.Opacity(0.65);
      caption_block.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
      stack.Children().Append(caption_block);
    }
    progress_card.Child(stack);
    children.Append(progress_card);
  } else {
    auto hint_card = Microsoft::UI::Xaml::Controls::Border();
    hint_card.CornerRadius({16, 16, 16, 16});
    hint_card.Background(theme_brush(contentRoot, L"PaybackAccentSoftBrush"));
    hint_card.Padding({12, 12, 12, 12});
    auto hint_row = Microsoft::UI::Xaml::Controls::StackPanel();
    hint_row.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
    hint_row.Spacing(10);
    auto bulb = Microsoft::UI::Xaml::Controls::TextBlock();
    bulb.Text(L"💡");
    auto hint = Microsoft::UI::Xaml::Controls::TextBlock();
    hint.Text(s.willing_hint());
    hint.FontSize(12);
    hint.Opacity(0.75);
    hint.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    hint.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
    hint_row.Children().Append(bulb);
    hint_row.Children().Append(hint);
    hint_card.Child(hint_row);
    children.Append(hint_card);
  }

  // ----- milestone ladder -----
  auto ladder = make_card();
  auto ladder_stack = Microsoft::UI::Xaml::Controls::StackPanel();
  ladder_stack.Spacing(8);
  auto ladder_title = Microsoft::UI::Xaml::Controls::TextBlock();
  ladder_title.Text(s.milestones());
  ladder_title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
  ladder_stack.Children().Append(ladder_title);
  for (auto const& milestone : row->computed.milestones) {
    auto line = Microsoft::UI::Xaml::Controls::Grid();
    line.ColumnDefinitions().Append(star_column());
    line.ColumnDefinitions().Append(auto_column());
    auto left = Microsoft::UI::Xaml::Controls::StackPanel();
    left.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
    left.Spacing(10);
    auto emoji = Microsoft::UI::Xaml::Controls::TextBlock();
    emoji.Text(milestone_emoji(milestone.key));
    emoji.Opacity(milestone.achieved ? 1.0 : 0.3);
    left.Children().Append(emoji);
    auto label = Microsoft::UI::Xaml::Controls::TextBlock();
    label.Text(milestone_label(milestone.key));
    label.FontSize(13);
    label.Opacity(milestone.achieved ? 1.0 : 0.55);
    left.Children().Append(label);
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(left, 0);
    line.Children().Append(left);
    if (milestone.achieved) {
      auto check = Microsoft::UI::Xaml::Controls::TextBlock();
      check.Text(L"✓");
      check.FontWeight(Microsoft::UI::Text::FontWeights::Bold());
      check.Foreground(theme_brush(contentRoot, L"PaybackEarnBrush"));
      Microsoft::UI::Xaml::Controls::Grid::SetColumn(check, 1);
      line.Children().Append(check);
    }
    ladder_stack.Children().Append(line);
  }
  ladder.Child(ladder_stack);
  children.Append(ladder);
}

// Curve geometry mirrors the mac CostCurve math: day 1 carries the whole
// price at the top edge and the line decays downward (never rises), and the
// x axis stretches to at least 30 days so a fresh purchase does not fill
// the whole card.
void MainWindow::UpdateCurveGeometry(CurveState& curve) {
  auto const canvas = curve.canvas;
  if (!canvas) {
    return;
  }
  double const width = canvas.ActualWidth();
  double const height = canvas.ActualHeight();
  if (width <= 0.0 || height <= 0.0) {
    return;
  }
  auto const max_days = std::max<std::int64_t>(curve.days_held, 30);
  auto const max_value =
      static_cast<double>(std::max<std::int64_t>(curve.price_minor, 1));

  auto const points = curve.line.Points();
  points.Clear();
  auto const fill_points = curve.fill.Points();
  fill_points.Clear();
  if (curve.days_held > 1) {
    for (std::int64_t day = 1; day <= curve.days_held; ++day) {
      double const x = static_cast<double>(day - 1) /
                       static_cast<double>(max_days - 1) * width;
      // day 1 carries the whole price at the top edge; the curve only ever
      // decays downward from there (超衰减曲线, design.md — never rises)
      double const y =
          height * (1.0 - (static_cast<double>(curve.price_minor) /
                           static_cast<double>(day)) /
                              max_value);
      auto const point = winrt::Windows::Foundation::Point{
          static_cast<float>(std::min(x, width)), static_cast<float>(y)};
      points.Append(point);
      fill_points.Append(point);
    }
    // close the area fill down to the bottom corners
    fill_points.Append(winrt::Windows::Foundation::Point{
        static_cast<float>(width), static_cast<float>(height)});
    fill_points.Append(
        winrt::Windows::Foundation::Point{0.0f, static_cast<float>(height)});
    auto const last = points.GetAt(points.Size() - 1);
    Microsoft::UI::Xaml::Controls::Canvas::SetLeft(curve.dot, last.X - 3.5);
    Microsoft::UI::Xaml::Controls::Canvas::SetTop(curve.dot, last.Y - 3.5);
  }
  auto const has_curve = curve.days_held > 1
                             ? Microsoft::UI::Xaml::Visibility::Visible
                             : Microsoft::UI::Xaml::Visibility::Collapsed;
  curve.line.Visibility(has_curve);
  curve.fill.Visibility(has_curve);
  curve.dot.Visibility(has_curve);
}

void MainWindow::UpdateToolbarEnabled() {
  // the detail pane always falls back to a device, so edit/delete are live
  // exactly while a device exists
  bool const has_device = !document_.devices.empty();
  EditButton().IsEnabled(has_device);
  DeleteButton().IsEnabled(has_device);
}

// ---------- toolbar ------------------------------------------------------------

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

void MainWindow::EditButton_Click(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!selected_device_id_.empty()) {
    SaveDeviceAsync(true, selected_device_id_);
  }
}

void MainWindow::DeleteButton_Click(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!selected_device_id_.empty()) {
    ShowDeleteConfirm(selected_device_id_);
  }
}

void MainWindow::SettingsButton_Click(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  OpenSettingsDialog();
}

// About box: version line + product line + license + the two links that
// route feedback (website, issue tracker).
winrt::fire_and_forget MainWindow::AboutButton_Click(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  Strings const& s = strings();
  auto panel = Microsoft::UI::Xaml::Controls::StackPanel();
  panel.Spacing(4);
  auto tagline = Microsoft::UI::Xaml::Controls::TextBlock();
  tagline.Text(s.about_content());
  tagline.TextAlignment(Microsoft::UI::Xaml::TextAlignment::Center);
  tagline.Margin({0, 0, 0, 8});
  auto license = Microsoft::UI::Xaml::Controls::TextBlock();
  license.Text(s.about_license());
  license.TextAlignment(Microsoft::UI::Xaml::TextAlignment::Center);
  license.FontSize(12);
  license.Opacity(0.75);
  auto website = Microsoft::UI::Xaml::Controls::HyperlinkButton();
  website.Content(winrt::box_value(s.about_website()));
  website.NavigateUri(winrt::Windows::Foundation::Uri{L"https://payback.jrtx.site"});
  website.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Center);
  website.Padding({0, 0, 0, 0});
  auto issue = Microsoft::UI::Xaml::Controls::HyperlinkButton();
  issue.Content(winrt::box_value(s.about_report_issue()));
  issue.NavigateUri(winrt::Windows::Foundation::Uri{
      L"https://github.com/turinglambdaai/payback/issues"});
  issue.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Center);
  issue.Padding({0, 0, 0, 0});
  panel.Children().Append(tagline);
  panel.Children().Append(license);
  panel.Children().Append(website);
  panel.Children().Append(issue);

  Microsoft::UI::Xaml::Controls::ContentDialog dialog;
  dialog.Title(winrt::box_value(L"Payback v" +
                                to_utf8_as_wide(std::string(rivet_app::kVersion))));
  dialog.Content(winrt::box_value(panel));
  dialog.CloseButtonText(s.close());
  dialog.XamlRoot(Content().XamlRoot());
  try {
    co_await dialog.ShowAsync();
  } catch (...) {
    // another dialog is already up; the About button can be pressed again
  }
}

// ---------- add / edit / delete ----------------------------------------------

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
  // id paired with its localized display name; the id travels to the backend
  std::vector<std::pair<std::wstring, std::wstring>> const categories = {
      {L"computer", s.category_computer()}, {L"phone", s.category_phone()},
      {L"tablet", s.category_tablet()},     {L"watch", s.category_watch()},
      {L"audio", s.category_audio()},       {L"camera", s.category_camera()},
      {L"gaming", s.category_gaming()},
      {L"appliance", s.category_appliance()},
      {L"accessory", s.category_accessory()},
      {L"other", s.category_other()}};
  int selected = -1;
  int other_index = -1;
  int index = 0;
  for (auto const& [category_id, category_label] : categories) {
    Microsoft::UI::Xaml::Controls::ComboBoxItem item;
    item.Content(winrt::box_value(category_label));
    item.Tag(winrt::box_value(winrt::hstring(category_id)));
    category_box.Items().Append(item);
    if (category_id == existing->category) {
      selected = index;
    }
    if (category_id == L"other") {
      other_index = index;
    }
    ++index;
  }
  if (selected < 0) {
    selected = other_index < 0 ? 0 : other_index;
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

  // say why Save refuses the round trip instead of leaving the error to the
  // backend: the hint tracks the field live, and Closing cancels a save
  // that would carry an invalid amount
  auto price_error = Microsoft::UI::Xaml::Controls::TextBlock();
  price_error.Text(s.price_required_hint());
  price_error.FontSize(11);
  price_error.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
      Microsoft::UI::Colors::Red()});
  auto const price_error_visible = [](Microsoft::UI::Xaml::Controls::TextBox const& box) {
    return parse_amount_minor(box.Text()) <= 0
               ? Microsoft::UI::Xaml::Visibility::Visible
               : Microsoft::UI::Xaml::Visibility::Collapsed;
  };
  price_error.Visibility(price_error_visible(price_box));
  panel.Children().Append(price_error);
  // TextChanged is EventHandler<object>: the sender arrives as IInspectable
  // and must be cast back (Toggled, by contrast, ships a typed sender)
  price_box.TextChanged(
      [price_error, price_error_visible](
          winrt::Windows::Foundation::IInspectable const& sender,
          winrt::Windows::Foundation::IInspectable const&) {
        auto const box = sender.as<Microsoft::UI::Xaml::Controls::TextBox>();
        price_error.Visibility(price_error_visible(box));
      });

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

  // enabled with a clean amount = set; enabled but empty = not set; enabled
  // with garbage would silently drop the field — surface that before save
  auto willing_error = Microsoft::UI::Xaml::Controls::TextBlock();
  willing_error.Text(s.price_required_hint());
  willing_error.FontSize(11);
  willing_error.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
      Microsoft::UI::Colors::Red()});
  auto const willing_error_visible =
      [](Microsoft::UI::Xaml::Controls::ToggleSwitch const& toggle,
         Microsoft::UI::Xaml::Controls::TextBox const& box) {
        return toggle.IsOn() && !box.Text().empty() &&
                       parse_amount_minor(box.Text()) <= 0
                   ? Microsoft::UI::Xaml::Visibility::Visible
                   : Microsoft::UI::Xaml::Visibility::Collapsed;
      };
  willing_error.Visibility(willing_error_visible(willing_toggle, willing_box));
  panel.Children().Append(willing_error);
  // WinUI 3 projects the classic control events as EventHandler<object>:
  // every sender arrives as IInspectable and is cast back to its control
  willing_toggle.Toggled(
      [willing_box, willing_error, willing_error_visible](
          winrt::Windows::Foundation::IInspectable const& sender,
          winrt::Windows::Foundation::IInspectable const&) {
        auto const toggle =
            sender.as<Microsoft::UI::Xaml::Controls::ToggleSwitch>();
        willing_error.Visibility(willing_error_visible(toggle, willing_box));
      });
  willing_box.TextChanged(
      [willing_toggle, willing_error, willing_error_visible](
          winrt::Windows::Foundation::IInspectable const& sender,
          winrt::Windows::Foundation::IInspectable const&) {
        auto const box = sender.as<Microsoft::UI::Xaml::Controls::TextBox>();
        willing_error.Visibility(
            willing_error_visible(willing_toggle, box));
      });

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

  // save failures render here, at the form, so the fields survive for a
  // retry — never in the window-level error bar (#24)
  auto form_error = Microsoft::UI::Xaml::Controls::TextBlock();
  form_error.FontSize(11);
  form_error.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
      Microsoft::UI::Colors::Red()});
  form_error.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
  form_error.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
  panel.Children().Append(form_error);

  Microsoft::UI::Xaml::Controls::ContentDialog dialog;
  dialog.Title(winrt::box_value(update ? s.edit() : s.add_device()));
  dialog.Content(panel);
  dialog.PrimaryButtonText(s.save());
  dialog.CloseButtonText(s.cancel());
  dialog.DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Primary);
  dialog.XamlRoot(Content().XamlRoot());

  // ContentDialog buttons can't be disabled; the close-cancel guard does
  // the state machine instead: an invalid field, a save already in flight
  // and a mid-flight cancel all keep the form open, and the dialog closes
  // only after the backend confirms persistence (#24). Hide() closes it
  // programmatically once the save lands.
  struct FormSaveState {
    bool saving = false;
  };
  auto state = std::make_shared<FormSaveState>();

  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;

  auto const show_form_error = [form_error](std::wstring const& message) {
    form_error.Text(winrt::hstring(message));
    form_error.Visibility(message.empty()
                              ? Microsoft::UI::Xaml::Visibility::Collapsed
                              : Microsoft::UI::Xaml::Visibility::Visible);
  };

  dialog.Closing(
      [state, dialog, dispatcher, weak, backend, update, id, name_box,
       icon_box, category_box, price_box, date_picker, willing_toggle,
       willing_box, notes_box, form_error, show_form_error, existing](
          Microsoft::UI::Xaml::Controls::ContentDialog const&,
          Microsoft::UI::Xaml::Controls::ContentDialogClosingEventArgs const&
              args) {
        if (args.Result() !=
            Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary) {
          // mid-flight cancel would leave the outcome unknown; wait it out
          if (state->saving) {
            args.Cancel(true);
          }
          return;
        }
        if (state->saving) {
          args.Cancel(true);  // double Save must not create a second record
          return;
        }

        auto const category = [&]() -> std::wstring {
          auto item = category_box.SelectedItem();
          if (item) {
            auto const value = winrt::unbox_value<winrt::hstring>(
                item.as<Microsoft::UI::Xaml::Controls::ComboBoxItem>().Tag());
            return std::wstring(value.c_str());
          }
          return L"other";
        }();
        auto const price_minor = parse_amount_minor(price_box.Text());
        auto const willing_minor = parse_amount_minor(willing_box.Text());
        bool const price_bad = price_minor <= 0;
        bool const willing_bad =
            willing_toggle.IsOn() && !willing_box.Text().empty() &&
            willing_minor <= 0;
        if (price_bad || willing_bad) {
          args.Cancel(true);
          show_form_error(L"");
          return;
        }

        // stay open while the save runs; fields stay exactly as typed so
        // a failure can be retried in place
        state->saving = true;
        args.Cancel(true);
        form_error.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);

        std::wstring json = L"{";
        json += L"\"name\":" +
                JsonValue::CreateStringValue(name_box.Text()).Stringify() +
                L",";
        json += L"\"icon\":" +
                JsonValue::CreateStringValue(icon_box.Text()).Stringify() +
                L",";
        json += L"\"category\":" +
                JsonValue::CreateStringValue(category).Stringify() + L",";
        json += L"\"priceMinor\":" + std::to_wstring(price_minor) + L",";
        json += L"\"currency\":" +
                JsonValue::CreateStringValue(existing->currency).Stringify() +
                L",";
        json += L"\"purchaseDate\":" +
                JsonValue::CreateStringValue(format_date(date_picker.Date()))
                    .Stringify() +
                L",";
        if (willing_toggle.IsOn() && willing_minor > 0) {
          json += L"\"willingPerDayMinor\":" + std::to_wstring(willing_minor) +
                  L",";
        } else {
          json += L"\"willingPerDayMinor\":null,";
        }
        json += L"\"notes\":" +
                JsonValue::CreateStringValue(notes_box.Text()).Stringify();
        if (update) {
          json += L",\"id\":" +
                  JsonValue::CreateStringValue(id).Stringify();
        }
        json += L"}";

        auto const payload = to_bytes(json);
        try {
          rivet_app::API api(*backend);
          auto const callbackDispatcher = dispatcher;
          auto handler = [state, dialog, callbackDispatcher, weak,
                          form_error, show_form_error](
                             rivet_app::Result<rivet::Bytes> result) {
            std::string failure;
            try {
              (void)result.get();
            } catch (std::exception const& e) {
              failure = e.what();
            }
            callbackDispatcher.TryEnqueue(
                [state, dialog, weak, failure = std::move(failure),
                 form_error, show_form_error] {
                  if (failure.empty()) {
                    if (auto window = weak.get()) {
                      window->LoadAllAsync();
                    }
                    state->saving = false;  // before Hide(): Closing must allow it
                    dialog.Hide();
                    return;
                  }
                  // keep the form open with its fields; Save acts as Retry
                  state->saving = false;
                  show_form_error(
                      std::wstring(winrt::to_hstring(failure).c_str()));
                });
          };
          if (update) {
            (void)api.update_device_async(payload, handler);
          } else {
            (void)api.add_device_async(payload, handler);
          }
        } catch (std::exception const& e) {
          state->saving = false;
          show_form_error(std::wstring(winrt::to_hstring(e.what()).c_str()));
          args.Cancel(true);
        }
      });

  (void)dialog.ShowAsync();
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

void MainWindow::ShowDeleteConfirm(std::wstring const& id) {
  Strings const& s = strings();
  Microsoft::UI::Xaml::Controls::ContentDialog dialog;
  dialog.Title(winrt::box_value(s.delete_confirm_title()));
  dialog.Content(winrt::box_value(winrt::hstring(s.delete_confirm_text())));
  dialog.PrimaryButtonText(s.remove());
  dialog.CloseButtonText(s.cancel());
  dialog.DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Close);
  dialog.XamlRoot(Content().XamlRoot());

  auto const weak = get_weak();
  auto operation = dialog.ShowAsync();
  operation.Completed([weak, id](auto const& async, auto&&) {
    if (auto window = weak.get()) {
      if (async.GetResults() ==
          Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary) {
        window->DeleteDeviceAsync(id);
      }
    }
  });
}

}  // namespace winrt::RivetHost::implementation
