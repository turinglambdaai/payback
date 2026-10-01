// Update flow, daily digest, and milestone celebrations for the Payback
// window. Split from MainWindow.xaml.cpp; the emotional layer mirrors the
// macOS host (docs/updates.md, docs/design.md).

#include "pch.h"
#include "MainWindow.xaml.h"
#include "GeneratedBackend.hpp"
#include "HostHelpers.h"

#include <shellapi.h>
#include <stdexcept>

namespace winrt::RivetHost::implementation {
namespace {

using namespace payback::host;
using payback::strings::Strings;

using Windows::Data::Json::JsonObject;

}  // namespace

// ---------- language ----------------------------------------------------------

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
  if (mapped == payback::host::language_override()) {
    return;  // no-op selection (e.g. the constructor's menu sync)
  }
  payback::host::set_language_override(mapped);
  ApplyLanguage();
}

// ---------- online updates ----------------------------------------------------

void MainWindow::CheckUpdates_Click(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  CheckUpdatesAsync();
}

void MainWindow::CheckUpdatesAsync() {
  RunUpdateCheck(true, /*silent=*/false);
}

void MainWindow::AutoCheckUpdatesAsync() {
  RunUpdateCheck(false, /*silent=*/true);
}

// A manual check surfaces every outcome; a silent (launch-time) check never
// nags — it only reports an available update through the consent dialog.
void MainWindow::RunUpdateCheck(bool force, bool silent) {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;
  if (backend == nullptr) {
    return;
  }
  if (!silent) {
    Strings const& s = strings();
    StatusBar().Severity(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational);
    StatusBar().Message(s.update_check_title());
  }
  try {
    rivet_app::API api(*backend);
    (void)api.check_updates_async(
        force,
        [dispatcher, weak, silent](rivet_app::Result<rivet::Bytes> result) {
          std::vector<std::uint8_t> payload;
          std::string failure;
          try {
            payload = result.get();
          } catch (std::exception const& e) {
            failure = e.what();
          }
          dispatcher.TryEnqueue([weak, payload = std::move(payload),
                                 failure = std::move(failure), silent] {
            if (auto window = weak.get()) {
              if (!failure.empty()) {
                if (!silent) {
                  window->SetErrorUi(failure);
                }
                return;
              }
              auto const utf8_text = std::string(payload.begin(), payload.end());
              auto const check = JsonObject::Parse(to_utf8_as_wide(utf8_text));
              auto const status = as_string(field(check, L"status"));
              if (status == L"available") {
                window->ShowUpdateConsent(
                    as_string(field(check, L"availableVersion")),
                    as_double(field(check, L"sizeBytes")));
              } else if (!silent) {
                Strings const& s = strings();
                Microsoft::UI::Xaml::Controls::ContentDialog dialog;
                dialog.Title(winrt::box_value(winrt::hstring(s.updates())));
                dialog.Content(winrt::box_value(winrt::hstring(
                    status == L"up-to-date" ? s.up_to_date()
                                            : s.update_error())));
                dialog.CloseButtonText(s.close());
                dialog.XamlRoot(window->Content().XamlRoot());
                try {
                  (void)dialog.ShowAsync();
                } catch (...) {
                  // a dialog is already up; try again after closing it
                }
              }
            }
          });
        });
  } catch (std::exception const& e) {
    if (!silent) {
      SetErrorUi(e.what());
    }
  }
}

// Nothing downloads before the user picks "Download" — an update is an
// offer, never an ambient side effect.
void MainWindow::ShowUpdateConsent(std::wstring const& version, double size_bytes) {
  Strings const& s = strings();
  std::wstring body = s.update_available_body();
  body = replace_all(body, L"{version}", version);
  body = replace_all(body, L"{size}", format_size(size_bytes));

  Microsoft::UI::Xaml::Controls::ContentDialog dialog;
  dialog.Title(winrt::box_value(s.update_available()));
  dialog.Content(winrt::box_value(winrt::hstring(body)));
  dialog.PrimaryButtonText(s.download_update());
  dialog.CloseButtonText(s.cancel());
  dialog.DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Primary);
  dialog.XamlRoot(Content().XamlRoot());

  auto const weak = get_weak();
  try {
    auto operation = dialog.ShowAsync();
    operation.Completed([weak](auto const& async, auto&&) {
      if (auto window = weak.get()) {
        if (async.GetResults() ==
            Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary) {
          window->StartDownloadAsync();
        }
      }
    });
  } catch (...) {
    // another dialog is already up; the check can be repeated from the toolbar
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
    ShowInstallConsent(as_string(field(state, L"downloadedPath")),
                       as_string(field(state, L"availableVersion")));
  } else if (phase == L"error") {
    if (update_timer_) update_timer_->Stop();
    if (update_dialog_operation_) update_dialog_operation_->Cancel();
    update_dialog_ = nullptr;
    ShowError(as_string(field(state, L"message")));
  }
}

// Installing is a separate, explicit step: the download is complete, the
// hash verified, and only the user's "Quit and Install" closes the app.
void MainWindow::ShowInstallConsent(std::wstring const& path,
                                    std::wstring const& version) {
  Strings const& s = strings();
  std::wstring body = s.update_ready_body();
  body = replace_all(body, L"{version}", version);

  auto dialog = std::make_shared<Microsoft::UI::Xaml::Controls::ContentDialog>();
  dialog->Title(winrt::box_value(s.update_available()));
  dialog->Content(winrt::box_value(winrt::hstring(body)));
  dialog->PrimaryButtonText(s.install_now());
  dialog->CloseButtonText(s.close());
  dialog->DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Primary);
  dialog->XamlRoot(Content().XamlRoot());

  auto const weak = get_weak();
  auto const shared_path = std::make_shared<std::wstring>(path);
  try {
    auto operation = dialog->ShowAsync();
    operation.Completed([weak, shared_path](auto const& async, auto&&) {
      if (auto window = weak.get()) {
        if (async.GetResults() ==
            Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary) {
          window->InstallDownloadedUpdate(*shared_path);
        }
      }
    });
  } catch (...) {
    // another dialog is already up; the install stays available from Check Updates
  }
}

void MainWindow::InstallDownloadedUpdate(std::wstring const& path) {
  // Hand off to a detached script: wait for this process to exit, run the
  // signed MSI passively (transactional rollback), then relaunch the app.
  std::wstring const exe = executable_path().wstring();
  std::wstring const parameters =
      L"/c timeout /t 2 /nobreak >nul & msiexec /i \"" + path +
      L"\" /passive & start \"\" \"" + exe + L"\"";
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = L"cmd.exe";
  info.lpParameters = parameters.c_str();
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info)) {
    ShowError(strings().install_failed());
    return;
  }
  winrt::Microsoft::UI::Xaml::Application::Current().Exit();
}

// ---------- emotional layer ---------------------------------------------------

// Unpacked desktop apps have no package identity, so toast notifications are
// unavailable; the once-a-day digest surfaces in the status bar instead. The
// backend throttles to once per day, and failures stay silent.
void MainWindow::RunDailyDigest() {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;
  if (backend == nullptr) {
    return;
  }
  try {
    rivet_app::API api(*backend);
    (void)api.daily_digest_async(
        [dispatcher, weak](rivet_app::Result<rivet::Bytes> result) {
          std::vector<std::uint8_t> payload;
          try {
            payload = result.get();
          } catch (...) {
            return;
          }
          dispatcher.TryEnqueue([weak, payload = std::move(payload)] {
            if (auto window = weak.get()) {
              try {
                auto const utf8_text = std::string(payload.begin(), payload.end());
                auto const digest = JsonObject::Parse(to_utf8_as_wide(utf8_text));
                if (as_string(field(digest, L"status")) != L"ok") {
                  return;
                }
                Strings const& s = strings();
                std::wstring body = s.digest_body();
                body = replace_all(
                    body, L"{earned}",
                    money(as_double(field(digest, L"earnedTotalMinor")),
                          window->document_.currency));
                auto const best_name = as_string(field(digest, L"bestDeviceName"));
                if (!best_name.empty()) {
                  std::wstring best = s.digest_body_best();
                  best = replace_all(best, L"{name}", best_name);
                  best = replace_all(
                      best, L"{cost}",
                      per_day(as_double(field(digest, L"bestDeviceCostPerDayMinor")),
                              window->document_.currency));
                  body += L"\n" + best;
                }
                window->StatusBar().Severity(
                    Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational);
                window->StatusBar().IsClosable(true);
                window->StatusBar().Message(s.digest_title() + L"  " + body);
              } catch (...) {
                // the digest is a delight, never an interruption
              }
            }
          });
        });
  } catch (...) {
  }
}

// Celebrate milestones flagged new by the backend's ack-on-read: each one
// gets exactly one dialog, on the load-all that first observed it.
void MainWindow::ShowCelebrations() {
  if (celebration_dialog_open_) {
    return;
  }
  Strings const& s = strings();
  auto panel = Microsoft::UI::Xaml::Controls::StackPanel();
  panel.Spacing(6);
  for (auto const& row : document_.devices) {
    for (auto const& milestone : row.computed.milestones) {
      if (milestone.achieved && milestone.is_new) {
        auto line = Microsoft::UI::Xaml::Controls::TextBlock();
        line.Text(milestone_emoji(milestone.key) + L" " + row.name +
                  L" — " + milestone_label(milestone.key));
        panel.Children().Append(line);
      }
    }
  }
  if (panel.Children().Size() == 0) {
    return;
  }

  celebration_dialog_open_ = true;
  auto dialog = std::make_shared<Microsoft::UI::Xaml::Controls::ContentDialog>();
  dialog->Title(winrt::box_value(s.celebrate_title()));
  dialog->Content(panel);
  dialog->CloseButtonText(s.celebrate_keep());
  dialog->XamlRoot(Content().XamlRoot());

  auto const weak = get_weak();
  auto operation = dialog->ShowAsync();
  operation.Completed([weak](auto const&, auto&&) {
    if (auto window = weak.get()) {
      window->celebration_dialog_open_ = false;
    }
  });
}

}  // namespace winrt::RivetHost::implementation
