// Data & Sync settings dialog and the sync poll loop for the Payback
// window. Split from MainWindow.xaml.cpp; mirrors the macOS SettingsView
// (docs/sync-design.md M1): probe common cloud folders before the picker,
// confirm-don't-type enable flow, per-machine rename, and a 5 s sync-state
// poll that reloads the merged view when the revision moves.

#include "pch.h"
#include "MainWindow.xaml.h"
#include "GeneratedBackend.hpp"
#include "HostHelpers.h"

#include <shellapi.h>
#include <cwctype>

#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>

namespace winrt::RivetHost::implementation {

// Lives only inside MainWindow.Sync.cpp; the window keeps the panel alive
// through shared_ptr while async completions may still want to paint it.
struct MainWindow::SettingsPanel {
  Microsoft::UI::Xaml::Controls::StackPanel root{nullptr};
  Microsoft::UI::Xaml::Controls::StackPanel main_section{nullptr};
  Microsoft::UI::Xaml::Controls::StackPanel prompt_section{nullptr};
  Microsoft::UI::Xaml::Controls::TextBlock prompt_title{nullptr};
  Microsoft::UI::Xaml::Controls::TextBlock prompt_body{nullptr};
  Microsoft::UI::Xaml::Controls::StackPanel prompt_actions{nullptr};
  Microsoft::UI::Xaml::Controls::TextBlock status_text{nullptr};
  Microsoft::UI::Xaml::Controls::Button status_button{nullptr};
  Microsoft::UI::Xaml::Controls::StackPanel enabled_section{nullptr};
  Microsoft::UI::Xaml::Controls::TextBlock folder_path{nullptr};
  Microsoft::UI::Xaml::Controls::TextBox machine_name_box{nullptr};
  Microsoft::UI::Xaml::Controls::Button machine_name_save{nullptr};
  Microsoft::UI::Xaml::Controls::StackPanel machines_list{nullptr};
  Microsoft::UI::Xaml::Controls::TextBlock last_merge_text{nullptr};
  Microsoft::UI::Xaml::Controls::InfoBar ignored_bar{nullptr};
  Microsoft::UI::Xaml::Controls::ComboBox currency_box{nullptr};
};

namespace {

using namespace payback::host;
using payback::strings::Strings;

using Windows::Data::Json::JsonObject;
using Windows::Data::Json::JsonValue;

// The default currency list, mac SettingsView parity.
std::vector<std::wstring> const k_currency_codes = {
    L"CNY", L"USD", L"EUR", L"JPY", L"GBP", L"HKD",
    L"TWD", L"KRW", L"SGD", L"AUD", L"CAD", L"CHF"};

std::wstring env_path(wchar_t const* name) {
  std::wstring buffer(32768, L'\0');
  auto const length = ::GetEnvironmentVariableW(name, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return {};
  }
  buffer.resize(length);
  return buffer;
}

// Common sync homes probed before the folder picker (mac parity): the
// cloud-drive mounts Windows exposes plus the classic home-folder layouts.
std::vector<std::wstring> detect_sync_folders() {
  std::wstring const profile = env_path(L"USERPROFILE");
  std::vector<std::wstring> candidates;
  auto const probe = [&](std::wstring const& base) {
    if (base.empty()) {
      return;
    }
    std::wstring const path = base + L"\\Payback";
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
      candidates.push_back(path);
    }
  };
  probe(profile + L"\\iCloudDrive");
  probe(env_path(L"OneDrive"));
  probe(env_path(L"OneDriveConsumer"));
  probe(profile + L"\\Dropbox");
  probe(profile + L"\\Nutstore");
  probe(profile + L"\\坚果云");
  // OneDrive and OneDriveConsumer often resolve to the same path
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  return candidates;
}

// WinUI has no middle text trimming; the sync folder path shows head…tail.
std::wstring middle_truncate(std::wstring const& text, std::size_t max_chars) {
  if (text.size() <= max_chars) {
    return text;
  }
  std::size_t const head = (max_chars - 1) / 2;
  std::size_t const tail = max_chars - 1 - head;
  return text.substr(0, head) + L"…" + text.substr(text.size() - tail);
}

std::wstring trim_copy(std::wstring const& text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && iswspace(text[begin])) {
    ++begin;
  }
  while (end > begin && iswspace(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

}  // namespace

// ---------- sync state --------------------------------------------------------

// Decode the sync-state document every sync RPC returns; also records the
// revision so the poll loop does not fire a redundant reload right after a
// configure/rename.
void MainWindow::ApplySyncState(std::vector<std::uint8_t> const& payload) {
  auto const utf8_text = std::string(payload.begin(), payload.end());
  auto const state = JsonObject::Parse(to_utf8_as_wide(utf8_text));

  sync_state_.enabled = as_bool(field(state, L"enabled"));
  sync_state_.dir = as_string(field(state, L"dir"));
  auto const machine = field(state, L"machine");
  sync_state_.machine_id = as_string(field(machine, L"id"));
  sync_state_.machine_name = as_string(field(machine, L"name"));
  sync_state_.machines.clear();
  auto const machines = field(state, L"machines");
  if (machines.ValueType() == Windows::Data::Json::JsonValueType::Array) {
    for (auto const& entry : machines.GetArray()) {
      sync_state_.machines.emplace_back(as_string(field(entry, L"id")),
                                        as_string(field(entry, L"name")));
    }
  }
  sync_state_.last_merge_at = as_string(field(state, L"lastMergeAt"));
  sync_state_.ignored_files.clear();
  auto const ignored = field(state, L"ignoredFiles");
  if (ignored.ValueType() == Windows::Data::Json::JsonValueType::Array) {
    for (auto const& entry : ignored.GetArray()) {
      sync_state_.ignored_files.push_back(as_string(entry));
    }
  }
  sync_state_.revision = as_int(field(state, L"revision"));
  sync_revision_ = sync_state_.revision;
  sync_revision_seen_ = true;
}

// ---------- settings dialog ----------------------------------------------------

void MainWindow::OpenSettingsDialog() {
  Strings const& s = strings();
  auto const contentRoot = Content().as<Microsoft::UI::Xaml::Controls::Grid>();
  auto panel = std::make_shared<SettingsPanel>();

  auto const make_caption = [](std::wstring const& text) {
    Microsoft::UI::Xaml::Controls::TextBlock caption;
    caption.Text(text);
    caption.FontSize(12);
    caption.Opacity(0.65);
    caption.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    return caption;
  };

  auto root = Microsoft::UI::Xaml::Controls::StackPanel();
  root.Spacing(12);
  root.Width(480);
  panel->root = root;

  auto main_section = Microsoft::UI::Xaml::Controls::StackPanel();
  main_section.Spacing(12);
  panel->main_section = main_section;

  // ----- Data & Sync -----
  main_section.Children().Append(make_caption(s.sync_how_it_works()));

  auto status_row = Microsoft::UI::Xaml::Controls::Grid();
  status_row.ColumnDefinitions().Append(star_column());
  status_row.ColumnDefinitions().Append(auto_column());
  auto status_text = Microsoft::UI::Xaml::Controls::TextBlock();
  status_text.FontSize(13);
  status_text.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
  panel->status_text = status_text;
  Microsoft::UI::Xaml::Controls::Grid::SetColumn(status_text, 0);
  status_row.Children().Append(status_text);
  auto status_button = Microsoft::UI::Xaml::Controls::Button();
  status_button.Padding({14, 5, 14, 5});
  panel->status_button = status_button;
  Microsoft::UI::Xaml::Controls::Grid::SetColumn(status_button, 1);
  status_row.Children().Append(status_button);
  main_section.Children().Append(status_row);
  auto const weak = get_weak();
  status_button.Click([weak, panel](auto const&, auto const&) {
    if (auto window = weak.get()) {
      if (window->sync_state_.enabled) {
        window->ShowDisableSyncPrompt(panel);
      } else {
        window->BeginEnableSync(panel);
      }
    }
  });

  // details visible only while sync is on
  auto enabled_section = Microsoft::UI::Xaml::Controls::StackPanel();
  enabled_section.Spacing(10);
  panel->enabled_section = enabled_section;

  auto folder_row = Microsoft::UI::Xaml::Controls::Grid();
  folder_row.ColumnDefinitions().Append(star_column());
  folder_row.ColumnDefinitions().Append(auto_column());
  auto folder_stack = Microsoft::UI::Xaml::Controls::StackPanel();
  folder_stack.Spacing(2);
  folder_stack.Children().Append(make_caption(s.sync_folder_label()));
  auto folder_path = Microsoft::UI::Xaml::Controls::TextBlock();
  folder_path.FontSize(13);
  panel->folder_path = folder_path;
  folder_stack.Children().Append(folder_path);
  Microsoft::UI::Xaml::Controls::Grid::SetColumn(folder_stack, 0);
  folder_row.Children().Append(folder_stack);
  auto open_folder_button = Microsoft::UI::Xaml::Controls::Button();
  open_folder_button.Content(winrt::box_value(winrt::hstring(s.sync_open_folder())));
  open_folder_button.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
  open_folder_button.Click([weak](auto const&, auto const&) {
    if (auto window = weak.get()) {
      window->OpenFolderInExplorer(window->sync_state_.dir);
    }
  });
  Microsoft::UI::Xaml::Controls::Grid::SetColumn(open_folder_button, 1);
  folder_row.Children().Append(open_folder_button);
  enabled_section.Children().Append(folder_row);

  auto machine_row = Microsoft::UI::Xaml::Controls::Grid();
  machine_row.ColumnDefinitions().Append(star_column());
  machine_row.ColumnDefinitions().Append(auto_column());
  auto machine_box = Microsoft::UI::Xaml::Controls::TextBox();
  machine_box.Header(winrt::box_value(winrt::hstring(s.sync_machine_name_label())));
  panel->machine_name_box = machine_box;
  Microsoft::UI::Xaml::Controls::Grid::SetColumn(machine_box, 0);
  machine_row.Children().Append(machine_box);
  auto machine_save = Microsoft::UI::Xaml::Controls::Button();
  machine_save.Content(winrt::box_value(winrt::hstring(s.save())));
  machine_save.IsEnabled(false);
  machine_save.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Bottom);
  panel->machine_name_save = machine_save;
  Microsoft::UI::Xaml::Controls::Grid::SetColumn(machine_save, 1);
  machine_row.Children().Append(machine_save);
  machine_save.Click([weak, panel](auto const&, auto const&) {
    if (auto window = weak.get()) {
      window->RenameMachineAsync(
          trim_copy(std::wstring(panel->machine_name_box.Text())), panel);
    }
  });
  machine_box.TextChanged([panel](auto const& sender, auto const&) {
    auto const box = sender.as<Microsoft::UI::Xaml::Controls::TextBox>();
    panel->machine_name_save.IsEnabled(
        !trim_copy(std::wstring(box.Text())).empty());
  });
  enabled_section.Children().Append(machine_row);

  enabled_section.Children().Append(make_caption(s.sync_machines_label()));
  auto machines_list = Microsoft::UI::Xaml::Controls::StackPanel();
  machines_list.Spacing(4);
  panel->machines_list = machines_list;
  enabled_section.Children().Append(machines_list);

  auto last_merge = Microsoft::UI::Xaml::Controls::TextBlock();
  last_merge.FontSize(12);
  last_merge.Opacity(0.65);
  panel->last_merge_text = last_merge;
  enabled_section.Children().Append(last_merge);

  auto ignored_bar = Microsoft::UI::Xaml::Controls::InfoBar();
  ignored_bar.Severity(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
  ignored_bar.IsClosable(false);
  ignored_bar.IsOpen(false);
  panel->ignored_bar = ignored_bar;
  enabled_section.Children().Append(ignored_bar);

  main_section.Children().Append(enabled_section);

  // ----- currency -----
  auto separator = Microsoft::UI::Xaml::Controls::Border();
  separator.Height(1);
  separator.Background(theme_brush(contentRoot, L"PaybackLineBrush"));
  separator.Margin({0, 4, 0, 4});
  main_section.Children().Append(separator);

  auto currency_title = Microsoft::UI::Xaml::Controls::TextBlock();
  currency_title.Text(s.currency());
  currency_title.FontSize(14);
  currency_title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
  main_section.Children().Append(currency_title);

  auto currency_box = Microsoft::UI::Xaml::Controls::ComboBox();
  currency_box.Width(220);
  for (auto const& code : k_currency_codes) {
    Microsoft::UI::Xaml::Controls::ComboBoxItem item;
    item.Content(winrt::box_value(winrt::hstring(code)));
    item.Tag(winrt::box_value(winrt::hstring(code)));
    currency_box.Items().Append(item);
  }
  panel->currency_box = currency_box;
  currency_box.SelectionChanged([weak, panel](auto const&, auto const&) {
    if (auto window = weak.get()) {
      if (window->suppress_currency_changed_) {
        return;
      }
      auto const selectable = panel->currency_box.SelectedItem();
      if (!selectable) {
        return;
      }
      auto const item = selectable.as<Microsoft::UI::Xaml::Controls::ComboBoxItem>();
      auto const code = winrt::unbox_value_or<winrt::hstring>(item.Tag(), {});
      if (!code.empty()) {
        window->SaveCurrencyAsync(std::wstring(code.c_str()));
      }
    }
  });
  main_section.Children().Append(currency_box);
  main_section.Children().Append(make_caption(s.currency_hint()));

  // ----- prompt surface: Windows stacks one ContentDialog at a time, so
  // the enable/disable confirmations swap into the dialog content instead
  // of stacking a second dialog the way macOS sheets can -----
  auto prompt_section = Microsoft::UI::Xaml::Controls::StackPanel();
  prompt_section.Spacing(10);
  prompt_section.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
  panel->prompt_section = prompt_section;
  auto prompt_title = Microsoft::UI::Xaml::Controls::TextBlock();
  prompt_title.FontSize(16);
  prompt_title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
  prompt_title.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
  panel->prompt_title = prompt_title;
  prompt_section.Children().Append(prompt_title);
  auto prompt_body = Microsoft::UI::Xaml::Controls::TextBlock();
  prompt_body.FontSize(13);
  prompt_body.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
  panel->prompt_body = prompt_body;
  prompt_section.Children().Append(prompt_body);
  auto prompt_actions = Microsoft::UI::Xaml::Controls::StackPanel();
  prompt_actions.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
  prompt_actions.Spacing(8);
  prompt_actions.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Right);
  panel->prompt_actions = prompt_actions;
  prompt_section.Children().Append(prompt_actions);

  root.Children().Append(main_section);
  root.Children().Append(prompt_section);

  Microsoft::UI::Xaml::Controls::ContentDialog dialog;
  dialog.Title(winrt::box_value(s.settings_title()));
  dialog.Content(root);
  dialog.CloseButtonText(s.close());
  dialog.DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Close);
  dialog.XamlRoot(Content().XamlRoot());

  try {
    settings_dialog_operation_ =
        std::make_shared<Windows::Foundation::IAsyncOperation<
            Microsoft::UI::Xaml::Controls::ContentDialogResult>>(
            dialog.ShowAsync());
    settings_dialog_operation_->Completed([weak](auto const&, auto&&) {
      if (auto window = weak.get()) {
        window->settings_dialog_operation_ = nullptr;
      }
    });
  } catch (...) {
    // another dialog is already up; the button can be pressed again
    return;
  }

  // paint the cached state at once, then refresh from the backend
  ApplySyncStateToPanel(panel);
  RefreshSyncState(panel);
}

void MainWindow::RefreshSyncState(std::shared_ptr<SettingsPanel> const& panel) {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;
  if (backend == nullptr) {
    return;
  }
  try {
    rivet_app::API api(*backend);
    (void)api.sync_state_async(
        [dispatcher, weak, panel](rivet_app::Result<rivet::Bytes> result) {
          std::vector<std::uint8_t> payload;
          try {
            payload = result.get();
          } catch (...) {
            return;  // the dialog stays on its cached state
          }
          dispatcher.TryEnqueue([weak, panel, payload = std::move(payload)] {
            if (auto window = weak.get()) {
              window->ApplySyncState(payload);
              window->ApplySyncStateToPanel(panel);
            }
          });
        });
  } catch (...) {
  }
}

void MainWindow::ApplySyncStateToPanel(
    std::shared_ptr<SettingsPanel> const& panel) {
  if (!panel || !panel->status_text) {
    return;
  }
  Strings const& s = strings();
  auto const contentRoot = Content().as<Microsoft::UI::Xaml::Controls::Grid>();

  if (sync_state_.enabled) {
    panel->status_text.Text(L"● " + s.sync_status_on());
    panel->status_text.Opacity(1.0);
    panel->status_text.Foreground(theme_brush(contentRoot, L"PaybackEarnBrush"));
    panel->status_button.Content(winrt::box_value(winrt::hstring(s.sync_disable())));
    panel->enabled_section.Visibility(Microsoft::UI::Xaml::Visibility::Visible);

    panel->folder_path.Text(middle_truncate(sync_state_.dir, 46));
    Microsoft::UI::Xaml::ToolTipService::SetToolTip(
        panel->folder_path, winrt::box_value(winrt::hstring(sync_state_.dir)));

    // this machine's row carries its display name in parens, mac parity
    panel->machines_list.Children().Clear();
    for (auto const& [id, name] : sync_state_.machines) {
      auto line = Microsoft::UI::Xaml::Controls::TextBlock();
      line.Text(id == sync_state_.machine_id
                    ? name + L" (" + sync_state_.machine_name + L")"
                    : name);
      line.FontSize(13);
      panel->machines_list.Children().Append(line);
    }

    std::wstring const merged =
        sync_state_.last_merge_at.empty() ? L"—" : sync_state_.last_merge_at;
    panel->last_merge_text.Text(s.sync_last_merge_label() + L": " + merged);

    panel->ignored_bar.IsOpen(!sync_state_.ignored_files.empty());
    if (!sync_state_.ignored_files.empty()) {
      std::wstring body = s.sync_ignored_warning();
      body = replace_all(body, L"{count}",
                         std::to_wstring(sync_state_.ignored_files.size()));
      panel->ignored_bar.Message(winrt::hstring(body));
    }

    // the name draft only follows the backend while the field is idle, so
    // a refresh never stomps on what the user is typing (mac: machineNameLoaded)
    if (panel->machine_name_box.FocusState() ==
        Microsoft::UI::Xaml::FocusState::Unfocused) {
      panel->machine_name_box.Text(winrt::hstring(sync_state_.machine_name));
      panel->machine_name_save.IsEnabled(
          !trim_copy(sync_state_.machine_name).empty());
    }
  } else {
    panel->status_text.Text(L"○ " + s.sync_status_off());
    panel->status_text.Opacity(0.8);
    panel->status_text.Foreground(nullptr);
    panel->status_button.Content(winrt::box_value(winrt::hstring(s.sync_enable())));
    panel->enabled_section.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
  }

  // the currency selection follows the document; a programmatic set must
  // not re-trigger the save round trip
  suppress_currency_changed_ = true;
  auto const items = panel->currency_box.Items();
  for (std::uint32_t i = 0; i < items.Size(); ++i) {
    auto const item =
        items.GetAt(i).as<Microsoft::UI::Xaml::Controls::ComboBoxItem>();
    auto const tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), {});
    if (std::wstring(tag.c_str()) == document_.currency) {
      panel->currency_box.SelectedIndex(static_cast<std::int32_t>(i));
      break;
    }
  }
  suppress_currency_changed_ = false;
}

// ---------- prompts (in-dialog, one ContentDialog total) -----------------------

void MainWindow::ShowSettingsPrompt(std::shared_ptr<SettingsPanel> const& panel,
                                    std::wstring const& title,
                                    std::wstring const& body,
                                    std::vector<PromptAction> const& actions) {
  if (!panel || !panel->prompt_title) {
    return;
  }
  panel->prompt_title.Text(title);
  panel->prompt_body.Text(body);
  panel->prompt_actions.Children().Clear();
  for (auto const& action : actions) {
    auto button = Microsoft::UI::Xaml::Controls::Button();
    button.Content(winrt::box_value(winrt::hstring(action.label)));
    if (action.destructive) {
      button.Foreground(destructive_brush());
    }
    button.Click([panel, run = action.run](auto const&, auto const&) {
      if (run) {
        run();
      }
    });
    panel->prompt_actions.Children().Append(button);
  }
  panel->main_section.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
  panel->prompt_section.Visibility(Microsoft::UI::Xaml::Visibility::Visible);
}

void MainWindow::RestoreSettingsMain(std::shared_ptr<SettingsPanel> const& panel) {
  if (!panel || !panel->main_section) {
    return;
  }
  panel->prompt_section.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
  panel->main_section.Visibility(Microsoft::UI::Xaml::Visibility::Visible);
}

// confirm-first: probe the common sync homes; the folder picker is the
// fallback when nothing familiar shows up (mac beginEnable parity)
void MainWindow::BeginEnableSync(std::shared_ptr<SettingsPanel> const& panel) {
  auto const candidates = detect_sync_folders();
  if (!candidates.empty()) {
    ShowDetectedSyncPrompt(panel, candidates);
  } else {
    OpenFolderPicker();
  }
}

void MainWindow::ShowDetectedSyncPrompt(
    std::shared_ptr<SettingsPanel> const& panel,
    std::vector<std::wstring> const& candidates) {
  Strings const& s = strings();
  // the prompt names the folder, not the full path (mac parity)
  std::wstring const folder_name =
      std::filesystem::path(candidates.front()).filename().wstring();
  std::wstring body = s.sync_detected_body();
  body = replace_all(body, L"{folder}", folder_name);

  auto const weak = get_weak();
  std::vector<PromptAction> actions;
  actions.push_back(PromptAction{
      s.sync_use_detected(), false, [weak, panel, first = candidates.front()]() {
        if (auto window = weak.get()) {
          window->ConfigureSyncAsync(first, panel);
          window->RestoreSettingsMain(panel);
        }
      }});
  actions.push_back(PromptAction{s.sync_choose_folder(), false,
                                 [weak, panel]() {
                                   if (auto window = weak.get()) {
                                     window->RestoreSettingsMain(panel);
                                     window->OpenFolderPicker();
                                   }
                                 }});
  actions.push_back(PromptAction{s.cancel(), false, [weak, panel]() {
                                  if (auto window = weak.get()) {
                                    window->RestoreSettingsMain(panel);
                                  }
                                }});
  ShowSettingsPrompt(panel, s.sync_detected_title(), body, actions);
}

void MainWindow::ShowDisableSyncPrompt(
    std::shared_ptr<SettingsPanel> const& panel) {
  Strings const& s = strings();
  auto const weak = get_weak();
  std::vector<PromptAction> actions;
  actions.push_back(PromptAction{
      s.sync_disable(), true, [weak, panel]() {
        if (auto window = weak.get()) {
          // an empty dir decodes to {"dir":null} = back to the local folder
          window->ConfigureSyncAsync(L"", panel);
          window->RestoreSettingsMain(panel);
        }
      }});
  actions.push_back(PromptAction{s.cancel(), false, [weak, panel]() {
                                  if (auto window = weak.get()) {
                                    window->RestoreSettingsMain(panel);
                                  }
                                }});
  ShowSettingsPrompt(panel, s.sync_disable_confirm_title(),
                     s.sync_disable_confirm_body(), actions);
}

// ---------- sync RPCs ----------------------------------------------------------

void MainWindow::OpenFolderPicker() {
  // the folder picker is its own OS window: drop the settings dialog first
  // (Windows stacks one ContentDialog), reopen it once the picker lands
  if (settings_dialog_operation_) {
    settings_dialog_operation_->Cancel();
    settings_dialog_operation_ = nullptr;
  }

  winrt::Windows::Storage::Pickers::FolderPicker picker;
  picker.FileTypeFilter().Append(L"*");
  // unpackaged desktop app: the picker must be bound to an owner HWND
  auto const initialize_with_window = picker.as<::IInitializeWithWindow>();
  auto hwnd = ::GetActiveWindow();
  if (hwnd == nullptr) {
    hwnd = ::GetForegroundWindow();
  }
  initialize_with_window->Initialize(hwnd);

  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto operation = picker.PickSingleFolderAsync();
  operation.Completed([dispatcher, weak](auto const& async, auto&&) {
    std::wstring path;
    bool picked = false;
    try {
      if (auto folder = async.GetResults()) {
        path = std::wstring(folder.Path().c_str());
        picked = !path.empty();
      }
    } catch (...) {
      // the picker was cancelled; still goes back to settings below
    }
    dispatcher.TryEnqueue([weak, path = std::move(path), picked] {
      if (auto window = weak.get()) {
        if (picked) {
          window->ConfigureSyncAsync(path, nullptr, /*reopen_dialog=*/true);
        } else {
          window->OpenSettingsDialog();
        }
      }
    });
  });
}

void MainWindow::ConfigureSyncAsync(std::wstring const& dir,
                                    std::shared_ptr<SettingsPanel> const& panel,
                                    bool reopen_dialog) {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;
  if (backend == nullptr) {
    return;
  }
  std::wstring json = L"{\"dir\":";
  json += dir.empty()
              ? L"null"
              : JsonValue::CreateStringValue(winrt::hstring(dir)).Stringify();
  json += L"}";
  try {
    rivet_app::API api(*backend);
    (void)api.configure_sync_async(
        to_bytes(json),
        [dispatcher, weak, panel, reopen_dialog](
            rivet_app::Result<rivet::Bytes> result) {
          std::vector<std::uint8_t> payload;
          std::string failure;
          try {
            payload = result.get();
          } catch (std::exception const& e) {
            failure = e.what();
          }
          dispatcher.TryEnqueue(
              [weak, panel, reopen_dialog, payload = std::move(payload),
               failure = std::move(failure)] {
                if (auto window = weak.get()) {
                  if (!failure.empty()) {
                    Strings const& s = strings();
                    window->ShowError(s.sync_enable_failed() + L": " +
                                      to_utf8_as_wide(failure));
                  } else {
                    window->ApplySyncState(payload);
                    if (panel) {
                      window->ApplySyncStateToPanel(panel);
                    }
                    // the active data directory may have changed wholesale
                    window->LoadAllAsync();
                  }
                  if (reopen_dialog) {
                    window->OpenSettingsDialog();
                  }
                }
              });
        });
  } catch (std::exception const& e) {
    SetErrorUi(e.what());
  }
}

void MainWindow::RenameMachineAsync(std::wstring const& name,
                                    std::shared_ptr<SettingsPanel> const& panel) {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;
  if (backend == nullptr) {
    return;
  }
  std::wstring json = L"{\"name\":";
  json += JsonValue::CreateStringValue(winrt::hstring(name)).Stringify();
  json += L"}";
  try {
    rivet_app::API api(*backend);
    (void)api.rename_machine_async(
        to_bytes(json),
        [dispatcher, weak, panel](rivet_app::Result<rivet::Bytes> result) {
          std::vector<std::uint8_t> payload;
          std::string failure;
          try {
            payload = result.get();
          } catch (std::exception const& e) {
            failure = e.what();
          }
          dispatcher.TryEnqueue([weak, panel, payload = std::move(payload),
                                 failure = std::move(failure)] {
            if (auto window = weak.get()) {
              if (!failure.empty()) {
                window->ShowError(to_utf8_as_wide(failure));
              } else {
                window->ApplySyncState(payload);
                window->ApplySyncStateToPanel(panel);
              }
            }
          });
        });
  } catch (std::exception const& e) {
    SetErrorUi(e.what());
  }
}

// The backend answers with the merged settings; adopt them and re-render so
// the summary, rows, and detail all switch to the new currency at once.
void MainWindow::SaveCurrencyAsync(std::wstring const& code) {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;
  if (backend == nullptr) {
    return;
  }
  std::wstring json = L"{\"currency\":";
  json += JsonValue::CreateStringValue(winrt::hstring(code)).Stringify();
  json += L"}";
  try {
    rivet_app::API api(*backend);
    (void)api.save_settings_async(
        to_bytes(json),
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
                window->ShowError(to_utf8_as_wide(failure));
                return;
              }
              auto const settings = JsonObject::Parse(
                  to_utf8_as_wide(std::string(payload.begin(), payload.end())));
              window->document_.currency =
                  as_string(field(settings, L"currency"),
                            window->document_.currency);
              window->document_.update_auto_check =
                  as_bool(field(settings, L"updateAutoCheck"), true);
              window->RenderDocumentFromCache();
            }
          });
        });
  } catch (std::exception const& e) {
    SetErrorUi(e.what());
  }
}

void MainWindow::OpenFolderInExplorer(std::wstring const& path) {
  if (path.empty()) {
    return;
  }
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = path.c_str();
  info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&info)) {
    SetErrorUi("ShellExecuteExW failed");
  }
}

// ---------- sync poll loop -----------------------------------------------------

void MainWindow::StartSyncPolling() {
  if (sync_timer_ != nullptr) {
    return;
  }
  // External changes ride the backend's revision counter: a changed revision
  // means the merged view moved under us (another machine, or a conflict
  // copy landing), and the load-all render path picks it up. Errors stay
  // silent — the poll is a listener, never a nag.
  sync_timer_ = std::make_shared<Microsoft::UI::Xaml::DispatcherTimer>();
  sync_timer_->Interval(std::chrono::milliseconds{5000});
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;
  sync_timer_->Tick([dispatcher, weak, backend](auto&&, auto&&) {
    try {
      rivet_app::API api(*backend);
      (void)api.sync_state_async(
          [dispatcher, weak](rivet_app::Result<rivet::Bytes> result) {
            std::vector<std::uint8_t> payload;
            try {
              payload = result.get();
            } catch (...) {
              return;
            }
            dispatcher.TryEnqueue([weak, payload = std::move(payload)] {
              if (auto window = weak.get()) {
                window->HandleSyncPoll(payload);
              }
            });
          });
    } catch (...) {
    }
  });
  sync_timer_->Start();
}

void MainWindow::HandleSyncPoll(std::vector<std::uint8_t> const& payload) {
  try {
    auto const utf8_text = std::string(payload.begin(), payload.end());
    auto const state = JsonObject::Parse(to_utf8_as_wide(utf8_text));
    auto const revision = as_int(field(state, L"revision"));
    if (!sync_revision_seen_) {
      // the first snapshot only calibrates the counter
      sync_revision_ = revision;
      sync_revision_seen_ = true;
      return;
    }
    if (revision != sync_revision_) {
      sync_revision_ = revision;
      LoadAllAsync();
    }
  } catch (...) {
  }
}

}  // namespace winrt::RivetHost::implementation
