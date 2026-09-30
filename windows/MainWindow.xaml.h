#pragma once

#include "pch.h"
#include "MainWindow.g.h"

namespace winrt::RivetHost::implementation {

struct MainWindow : MainWindowT<MainWindow> {
  MainWindow();

  void SortBox_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
  void AddButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                       Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void CheckUpdates_Click(winrt::Windows::Foundation::IInspectable const& sender,
                          Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void LanguageBox_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                    winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
  void ApplyLanguage();
  void RenderDocumentFromCache();
  int LanguageSelection() const { return language_override_; }  // 0 system 1 zh 2 en

 private:
  winrt::fire_and_forget InitializeBackendAsync();
  void SetReadyUi();
  void SetErrorUi(std::string const& message);
  void ShowError(std::wstring const& message);

  void LoadAllAsync();
  void RenderDocument(std::vector<std::uint8_t> const& payload);
  void RenderDeviceList();
  void SaveDeviceAsync(bool update, std::wstring id);
  void DeleteDeviceAsync(std::wstring id);
  void OpenDetailDialog(std::wstring id);
  void CheckUpdatesAsync();
  void StartDownloadAsync();
  void HandleUpdatePoll(std::vector<std::uint8_t> const& payload);
  void InstallDownloadedUpdate(std::wstring path);

  std::shared_ptr<rivet::windows::Backend> backend_;

  // parsed subset of the load-all document the UI needs
  struct Milestone {
    std::wstring key;
    bool achieved = false;
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
  int language_override_ = -1;  // -1 system, 0 zh, 1 en

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
