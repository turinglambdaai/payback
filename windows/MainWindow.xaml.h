#pragma once

#include "pch.h"
#include "MainWindow.g.h"

namespace winrt::RivetHost::implementation {

struct MainWindow : MainWindowT<MainWindow> {
  MainWindow();

  void SortBox_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
  void NavView_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args);
  void AddButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                       Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void EditButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                        Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void DeleteButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                          Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void SettingsButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                            Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void CheckUpdates_Click(winrt::Windows::Foundation::IInspectable const& sender,
                          Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void LanguageBox_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                    winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
  void ProButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                       Microsoft::UI::Xaml::RoutedEventArgs const& args);
  winrt::fire_and_forget AboutButton_Click(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void ApplyLanguage();
  void RenderDocumentFromCache();

 private:
  winrt::fire_and_forget InitializeBackendAsync();
  void SetReadyUi();
  void SetErrorUi(std::string const& message);
  void ShowError(std::wstring const& message);
  void ShowSuccess(std::wstring const& message);
  void OpenActivationDialog();

  void LoadAllAsync();
  void RenderDocument(std::vector<std::uint8_t> const& payload);
  void RenderDeviceList();
  void RenderDetail();
  void UpdateToolbarEnabled();
  void SaveDeviceAsync(bool update, std::wstring id);
  void DeleteDeviceAsync(std::wstring id);
  void ShowDeleteConfirm(std::wstring const& id);

  // emotional layer (parity with the macOS host)
  void ApplyQuip();
  void RunDailyDigest();
  void ShowCelebrations();

  // online updates; every install step requires explicit consent
  void CheckUpdatesAsync();
  void AutoCheckUpdatesAsync();
  void RunUpdateCheck(bool force, bool silent);
  void ShowUpdateConsent(std::wstring const& version, double size_bytes);
  void StartDownloadAsync();
  void HandleUpdatePoll(std::vector<std::uint8_t> const& payload);
  void ShowInstallConsent(std::wstring const& path, std::wstring const& version);
  void InstallDownloadedUpdate(std::wstring const& path);
  void ReportFailedInstall();

  // Data & Sync settings + default currency (docs/sync-design.md M1);
  // mirrors the macOS SettingsView: probe common folders before the picker,
  // rename through the backend, refresh the merged view on revision bumps.
  struct SettingsPanel;
  struct PromptAction {
    std::wstring label;
    bool destructive = false;
    std::function<void()> run;
  };
  struct SyncStateData {
    bool enabled = false;
    std::wstring dir;
    std::wstring machine_id;
    std::wstring machine_name;
    std::vector<std::pair<std::wstring, std::wstring>> machines;
    std::wstring last_merge_at;  // empty = never merged
    std::int64_t revision = 0;
    std::vector<std::wstring> ignored_files;
  };
  void OpenSettingsDialog();
  void RefreshSyncState(std::shared_ptr<SettingsPanel> const& panel);
  void ApplySyncStateToPanel(std::shared_ptr<SettingsPanel> const& panel);
  // Windows stacks one ContentDialog at a time, so the enable/disable
  // confirmations swap into the settings dialog instead of stacking.
  void ShowSettingsPrompt(std::shared_ptr<SettingsPanel> const& panel,
                          std::wstring const& title, std::wstring const& body,
                          std::vector<PromptAction> const& actions);
  void RestoreSettingsMain(std::shared_ptr<SettingsPanel> const& panel);
  void BeginEnableSync(std::shared_ptr<SettingsPanel> const& panel);
  void ShowDetectedSyncPrompt(std::shared_ptr<SettingsPanel> const& panel,
                              std::vector<std::wstring> const& candidates);
  void ShowDisableSyncPrompt(std::shared_ptr<SettingsPanel> const& panel);
  void OpenFolderPicker();
  void ConfigureSyncAsync(std::wstring const& dir,
                          std::shared_ptr<SettingsPanel> const& panel,
                          bool reopen_dialog = false);
  void RenameMachineAsync(std::wstring const& name,
                          std::shared_ptr<SettingsPanel> const& panel);
  void SaveCurrencyAsync(std::wstring const& code);
  void OpenFolderInExplorer(std::wstring const& path);
  void ApplySyncState(std::vector<std::uint8_t> const& payload);
  void StartSyncPolling();
  void HandleSyncPoll(std::vector<std::uint8_t> const& payload);

  std::shared_ptr<rivet::windows::Backend> backend_;

  // parsed subset of the load-all document the UI needs
  struct Milestone {
    std::wstring key;
    bool achieved = false;
    // true only on the load-all that first observes the achievement
    bool is_new = false;
  };
  struct Computed {
    std::int64_t days_held = 1;
    double cost_per_day_minor = 0.0;
    bool willing_set = false;
    double earned_minor = 0.0;
    bool has_earned = false;
    double payback_progress = 0.0;
    bool has_progress = false;
    std::wstring payback_eta;
    bool paid_back = false;
    std::vector<Milestone> milestones;
  };
  struct DeviceRow {
    std::wstring id;
    std::wstring name;
    std::wstring icon;
    std::wstring category;
    std::int64_t price_minor = 0;
    std::wstring currency;
    std::wstring purchase_date;
    std::wstring created_at;
    std::int64_t willing_per_day_minor = 0;
    bool has_willing = false;
    std::wstring notes;
    Computed computed;
  };
  struct Document {
    std::wstring currency = L"CNY";
    bool update_auto_check = true;
    std::vector<DeviceRow> devices;
    std::int64_t total_spent_minor = 0;
    double earned_total_minor = 0.0;
    double avg_cost_per_day_minor = 0.0;
    int device_count = 0;
  };

  Document document_;
  int sort_mode_ = 0;  // 0 added, 1 daily cost, 2 payback progress
  int quip_day_ = 1;
  bool celebration_dialog_open_ = false;
  bool auto_check_done_ = false;

  // sidebar selection: which device drives the detail pane (empty = none
  // yet); a stale id falls back to the first device on every render
  std::wstring selected_device_id_;
  bool suppress_selection_changed_ = false;

  // cost-curve geometry for the visible detail card; recomputed on layout
  struct CurveState {
    std::int64_t price_minor = 0;
    std::int64_t days_held = 1;
    Microsoft::UI::Xaml::Shapes::Polyline line{nullptr};
    Microsoft::UI::Xaml::Shapes::Polygon fill{nullptr};
    Microsoft::UI::Xaml::Shapes::Ellipse dot{nullptr};
    Microsoft::UI::Xaml::Controls::Canvas canvas{nullptr};
  };
  void UpdateCurveGeometry(CurveState& curve);
  std::shared_ptr<CurveState> curve_;

  // sync settings state (docs/sync-design.md M1)
  SyncStateData sync_state_;
  bool sync_revision_seen_ = false;
  std::int64_t sync_revision_ = 0;
  std::shared_ptr<Microsoft::UI::Xaml::DispatcherTimer> sync_timer_;
  bool suppress_currency_changed_ = false;
  std::shared_ptr<Windows::Foundation::IAsyncOperation<
      Microsoft::UI::Xaml::Controls::ContentDialogResult>>
      settings_dialog_operation_;

  // a launch-time update offer can race the milestone celebration dialog
  // (only one ContentDialog may be open); the offer waits here and is
  // re-shown when the celebration closes
  bool update_consent_pending_ = false;
  std::wstring update_consent_version_;
  double update_consent_size_bytes_ = 0.0;

  // update download UI state; owned here so lambdas never have to thread
  // them through two nesting levels
  std::shared_ptr<Microsoft::UI::Xaml::Controls::ContentDialog> update_dialog_;
  std::shared_ptr<Windows::Foundation::IAsyncOperation<
      Microsoft::UI::Xaml::Controls::ContentDialogResult>>
      update_dialog_operation_;
  std::shared_ptr<Microsoft::UI::Xaml::Controls::ProgressBar> update_percent_;
  std::shared_ptr<Microsoft::UI::Xaml::DispatcherTimer> update_timer_;
};

}  // namespace winrt::RivetHost::implementation

namespace winrt::RivetHost::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::RivetHost::factory_implementation
