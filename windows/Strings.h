// GENERATED from shared/strings/strings.json by scripts/gen-strings.js.
// Do not edit by hand: change strings.json and re-run the generator.
#pragma once

#include <windows.h>
#include <string>

namespace payback::strings {

inline bool chinese_ui() {
  // WinAppSDK's cppwinrt projection does not carry Windows.System.Profile;
  // the plain Win32 language query covers zh-Hans and zh-Hant alike.
  return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE;
}

// zh is the primary audience; English is selected automatically for
// non-Chinese systems.
struct Strings {
  bool zh;

  explicit Strings(bool chinese) : zh(chinese) {}

  std::wstring starting_backend() const { return zh ? L"正在启动 Racket 引擎…" : L"Starting Racket engine…"; }
  std::wstring backend_error() const { return zh ? L"后端错误" : L"Backend error"; }
  std::wstring config_error() const { return zh ? L"配置错误" : L"Configuration error"; }
  std::wstring app_name() const { return zh ? L"Payback 回本" : L"Payback"; }
  std::wstring tagline() const { return zh ? L"用得越久，越接近回本" : L"The longer you keep it, the more it pays back"; }
  std::wstring total_spent() const { return zh ? L"累计投入" : L"Total spent"; }
  std::wstring earned_back() const { return zh ? L"已赚回" : L"Earned back"; }
  std::wstring device_count() const { return zh ? L"设备" : L"devices"; }
  std::wstring overall_daily() const { return zh ? L"整体日均" : L"Overall daily"; }
  std::wstring add_device() const { return zh ? L"添加设备" : L"Add Device"; }
  std::wstring check_updates_menu() const { return zh ? L"检查更新…" : L"Check for Updates…"; }
  std::wstring empty_title() const { return zh ? L"还没有设备" : L"No devices yet"; }
  std::wstring empty_hint() const { return zh ? L"添加你的第一台设备，看着它的每日成本一天天降下去。" : L"Add your first device and watch its daily cost drop day by day."; }
  std::wstring sort_by() const { return zh ? L"排序" : L"Sort"; }
  std::wstring sort_added() const { return zh ? L"加入时间" : L"Date added"; }
  std::wstring sort_daily_cost() const { return zh ? L"日均成本" : L"Daily cost"; }
  std::wstring sort_payback() const { return zh ? L"回本进度" : L"Payback progress"; }
  std::wstring daily_cost() const { return zh ? L"每日成本" : L"Daily cost"; }
  std::wstring per_day() const { return zh ? L"/天" : L"/day"; }
  std::wstring held_for() const { return zh ? L"已持有" : L"Held for"; }
  std::wstring days() const { return zh ? L"天" : L"days"; }
  std::wstring paid_back() const { return zh ? L"已回本" : L"Paid back"; }
  std::wstring payback_progress() const { return zh ? L"回本进度" : L"Payback progress"; }
  std::wstring payback_eta() const { return zh ? L"预计回本" : L"Break-even expected"; }
  std::wstring earned_so_far() const { return zh ? L"按你的心理价位，已经多用出" : L"Value gained beyond your price"; }
  std::wstring still_to_earn() const { return zh ? L"还差" : L"Remaining"; }
  std::wstring willing_per_day() const { return zh ? L"每天愿意付" : L"Willing to pay"; }
  std::wstring not_set() const { return zh ? L"未设置" : L"Not set"; }
  std::wstring milestones() const { return zh ? L"里程碑" : L"Milestones"; }
  std::wstring edit() const { return zh ? L"编辑" : L"Edit"; }
  std::wstring remove() const { return zh ? L"删除" : L"Delete"; }
  std::wstring delete_confirm_title() const { return zh ? L"删除这台设备？" : L"Delete this device?"; }
  std::wstring delete_confirm_text() const { return zh ? L"记录删除后无法恢复。" : L"This cannot be undone."; }
  std::wstring cancel() const { return zh ? L"取消" : L"Cancel"; }
  std::wstring save() const { return zh ? L"保存" : L"Save"; }
  std::wstring name() const { return zh ? L"名称" : L"Name"; }
  std::wstring icon() const { return zh ? L"图标" : L"Icon"; }
  std::wstring category() const { return zh ? L"分类" : L"Category"; }
  std::wstring price() const { return zh ? L"购买价格" : L"Price"; }
  std::wstring purchase_date() const { return zh ? L"购买日期" : L"Purchase date"; }
  std::wstring notes() const { return zh ? L"备注" : L"Notes"; }
  std::wstring willing_hint() const { return zh ? L"设置“每天愿意付多少钱”，就能看到回本进度。" : L"Set what a day with this device is worth to you, and watch it pay itself back."; }
  std::wstring willing_label() const { return zh ? L"心理价位（每天）" : L"Worth per day"; }
  std::wstring category_computer() const { return zh ? L"电脑" : L"Computer"; }
  std::wstring category_phone() const { return zh ? L"手机" : L"Phone"; }
  std::wstring category_tablet() const { return zh ? L"平板" : L"Tablet"; }
  std::wstring category_audio() const { return zh ? L"音频" : L"Audio"; }
  std::wstring category_camera() const { return zh ? L"相机" : L"Camera"; }
  std::wstring category_gaming() const { return zh ? L"游戏" : L"Gaming"; }
  std::wstring category_appliance() const { return zh ? L"家电" : L"Appliance"; }
  std::wstring category_accessory() const { return zh ? L"配件" : L"Accessory"; }
  std::wstring category_other() const { return zh ? L"其他" : L"Other"; }
  std::wstring milestone_days100() const { return zh ? L"百日纪念" : L"100 days together"; }
  std::wstring milestone_days365() const { return zh ? L"陪伴一整年" : L"A full year"; }
  std::wstring milestone_days1000() const { return zh ? L"一千天老友" : L"1000-day friend"; }
  std::wstring milestone_cpd10() const { return zh ? L"日均低于 10 元" : L"Under 10/day"; }
  std::wstring milestone_cpd5() const { return zh ? L"日均低于 5 元" : L"Under 5/day"; }
  std::wstring milestone_cpd2() const { return zh ? L"日均低于 2 元" : L"Under 2/day"; }
  std::wstring milestone_cpd1() const { return zh ? L"日均低于 1 元" : L"Under 1/day"; }
  std::wstring milestone_cpd05() const { return zh ? L"日均低于 5 毛" : L"Under 0.50/day"; }
  std::wstring milestone_paid_back() const { return zh ? L"完全回本" : L"Fully paid back"; }
  std::wstring updates() const { return zh ? L"软件更新" : L"Software Update"; }
  std::wstring update_check_title() const { return zh ? L"正在检查更新…" : L"Checking for updates…"; }
  std::wstring up_to_date() const { return zh ? L"已是最新版本" : L"You're up to date"; }
  std::wstring update_available() const { return zh ? L"发现新版本" : L"Update available"; }
  std::wstring download_update() const { return zh ? L"下载更新" : L"Download Update"; }
  std::wstring downloading() const { return zh ? L"正在下载…" : L"Downloading…"; }
  std::wstring install_now() const { return zh ? L"退出并安装" : L"Quit and Install"; }
  std::wstring install_failed() const { return zh ? L"安装失败" : L"Install failed"; }
  std::wstring update_error() const { return zh ? L"更新失败" : L"Update failed"; }
  std::wstring retry() const { return zh ? L"重试" : L"Retry"; }
  std::wstring close() const { return zh ? L"关闭" : L"Close"; }
  std::wstring currency() const { return zh ? L"货币" : L"Currency"; }
  std::wstring language_auto() const { return zh ? L"跟随系统" : L"Auto"; }
  std::wstring language_title() const { return zh ? L"语言" : L"Language"; }
  std::wstring digest_title() const { return zh ? L"💰 回本快报" : L"💰 Payback digest"; }
  std::wstring celebrate_title() const { return zh ? L"值得庆祝的时刻" : L"A moment worth celebrating"; }
  std::wstring celebrate_keep() const { return zh ? L"继续用，继续赚" : L"Keep using, keep earning"; }
  std::wstring quip1() const { return zh ? L"每一次打开，都是在赚回当初的决定。" : L"Every open is another step toward payback."; }
  std::wstring quip2() const { return zh ? L"时间是你最便宜的合伙人。" : L"Time is your cheapest business partner."; }
  std::wstring quip3() const { return zh ? L"今天不用，它也在帮你回本——不，还是用用它吧。" : L"It pays back even idle — but go use it anyway."; }
  std::wstring quip4() const { return zh ? L"冲动是魔鬼，折旧是天使。" : L"Impulse is the devil; depreciation is the angel."; }
  std::wstring quip5() const { return zh ? L"好的购买，是用得越久越便宜。" : L"A good purchase gets cheaper every day you use it."; }
  std::wstring quip6() const { return zh ? L"别数钱了——好吧，再数一次。" : L"Stop counting the money — okay, one more look."; }
  std::wstring digest_body() const { return zh ? L"已赚回 {earned}，继续用，继续赚。" : L"Earned back {earned}. Keep using, keep earning."; }
  std::wstring digest_body_best() const { return zh ? L"{name} 今天只要 {cost}" : L"{name} costs just {cost} today"; }
};

inline Strings const& strings() {
  static Strings const value{chinese_ui()};
  return value;
}

}  // namespace payback::strings
