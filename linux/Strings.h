// GENERATED from shared/strings/strings.json by scripts/gen-strings.js.
// Do not edit by hand: change strings.json and re-run the generator.
#pragma once

#include <string>

// UTF-8 narrow strings for the GTK host; the caller decides zh (in-app
// override, then the system language) and constructs Strings with it.
namespace payback::linux_strings {

struct Strings {
  bool zh;

  explicit Strings(bool chinese) : zh(chinese) {}

  std::string starting_backend() const { return zh ? "正在启动 Racket 引擎…" : "Starting Racket engine…"; }
  std::string backend_ready() const { return zh ? "Racket CS 就绪" : "Racket CS ready"; }
  std::string backend_error() const { return zh ? "后端错误" : "Backend error"; }
  std::string config_error() const { return zh ? "配置错误" : "Configuration error"; }
  std::string app_name() const { return zh ? "Payback 回本" : "Payback"; }
  std::string tagline() const { return zh ? "用得越久，越接近回本" : "The longer you keep it, the more it pays back"; }
  std::string total_spent() const { return zh ? "累计投入" : "Total spent"; }
  std::string earned_back() const { return zh ? "已赚回" : "Earned back"; }
  std::string device_count() const { return zh ? "设备" : "devices"; }
  std::string overall_daily() const { return zh ? "整体日均" : "Overall daily"; }
  std::string add_device() const { return zh ? "添加设备" : "Add Device"; }
  std::string check_updates_menu() const { return zh ? "检查更新…" : "Check for Updates…"; }
  std::string empty_title() const { return zh ? "还没有设备" : "No devices yet"; }
  std::string empty_hint() const { return zh ? "添加你的第一台设备，看着它的每日成本一天天降下去。" : "Add your first device and watch its daily cost drop day by day."; }
  std::string sort_by() const { return zh ? "排序" : "Sort"; }
  std::string sort_added() const { return zh ? "加入时间" : "Date added"; }
  std::string sort_daily_cost() const { return zh ? "日均成本" : "Daily cost"; }
  std::string sort_payback() const { return zh ? "回本进度" : "Payback progress"; }
  std::string daily_cost() const { return zh ? "每日成本" : "Daily cost"; }
  std::string per_day() const { return zh ? "/天" : "/day"; }
  std::string held_for() const { return zh ? "已持有" : "Held for"; }
  std::string days() const { return zh ? "天" : "days"; }
  std::string paid_back() const { return zh ? "已回本" : "Paid back"; }
  std::string payback_progress() const { return zh ? "回本进度" : "Payback progress"; }
  std::string payback_eta() const { return zh ? "预计回本" : "Break-even expected"; }
  std::string earned_so_far() const { return zh ? "按你的心理价位，已经多用出" : "Value gained beyond your price"; }
  std::string still_to_earn() const { return zh ? "还差" : "Remaining"; }
  std::string willing_per_day() const { return zh ? "每天愿意付" : "Willing to pay"; }
  std::string not_set() const { return zh ? "未设置" : "Not set"; }
  std::string milestones() const { return zh ? "里程碑" : "Milestones"; }
  std::string edit() const { return zh ? "编辑" : "Edit"; }
  std::string remove() const { return zh ? "删除" : "Delete"; }
  std::string delete_confirm_title() const { return zh ? "删除这台设备？" : "Delete this device?"; }
  std::string delete_confirm_text() const { return zh ? "记录删除后无法恢复。" : "This cannot be undone."; }
  std::string cancel() const { return zh ? "取消" : "Cancel"; }
  std::string save() const { return zh ? "保存" : "Save"; }
  std::string name() const { return zh ? "名称" : "Name"; }
  std::string icon() const { return zh ? "图标" : "Icon"; }
  std::string category() const { return zh ? "分类" : "Category"; }
  std::string price() const { return zh ? "购买价格" : "Price"; }
  std::string purchase_date() const { return zh ? "购买日期" : "Purchase date"; }
  std::string notes() const { return zh ? "备注" : "Notes"; }
  std::string willing_hint() const { return zh ? "设置“每天愿意付多少钱”，就能看到回本进度。" : "Set what a day with this device is worth to you, and watch it pay itself back."; }
  std::string willing_label() const { return zh ? "心理价位（每天）" : "Worth per day"; }
  std::string category_computer() const { return zh ? "电脑" : "Computer"; }
  std::string category_phone() const { return zh ? "手机" : "Phone"; }
  std::string category_tablet() const { return zh ? "平板" : "Tablet"; }
  std::string category_audio() const { return zh ? "音频" : "Audio"; }
  std::string category_camera() const { return zh ? "相机" : "Camera"; }
  std::string category_gaming() const { return zh ? "游戏" : "Gaming"; }
  std::string category_appliance() const { return zh ? "家电" : "Appliance"; }
  std::string category_accessory() const { return zh ? "配件" : "Accessory"; }
  std::string category_other() const { return zh ? "其他" : "Other"; }
  std::string milestone_days100() const { return zh ? "百日纪念" : "100 days together"; }
  std::string milestone_days365() const { return zh ? "陪伴一整年" : "A full year"; }
  std::string milestone_days1000() const { return zh ? "一千天老友" : "1000-day friend"; }
  std::string milestone_cpd10() const { return zh ? "日均低于 10 元" : "Under 10/day"; }
  std::string milestone_cpd5() const { return zh ? "日均低于 5 元" : "Under 5/day"; }
  std::string milestone_cpd2() const { return zh ? "日均低于 2 元" : "Under 2/day"; }
  std::string milestone_cpd1() const { return zh ? "日均低于 1 元" : "Under 1/day"; }
  std::string milestone_cpd05() const { return zh ? "日均低于 5 毛" : "Under 0.50/day"; }
  std::string milestone_paid_back() const { return zh ? "完全回本" : "Fully paid back"; }
  std::string updates() const { return zh ? "软件更新" : "Software Update"; }
  std::string update_check_title() const { return zh ? "正在检查更新…" : "Checking for updates…"; }
  std::string up_to_date() const { return zh ? "已是最新版本" : "You're up to date"; }
  std::string update_available() const { return zh ? "发现新版本" : "Update available"; }
  std::string download_update() const { return zh ? "下载更新" : "Download Update"; }
  std::string downloading() const { return zh ? "正在下载…" : "Downloading…"; }
  std::string install_now() const { return zh ? "退出并安装" : "Quit and Install"; }
  std::string install_failed() const { return zh ? "安装失败" : "Install failed"; }
  std::string update_error() const { return zh ? "更新失败" : "Update failed"; }
  std::string retry() const { return zh ? "重试" : "Retry"; }
  std::string close() const { return zh ? "关闭" : "Close"; }
  std::string currency() const { return zh ? "货币" : "Currency"; }
  std::string language_auto() const { return zh ? "跟随系统" : "Auto"; }
  std::string language_title() const { return zh ? "语言" : "Language"; }
  std::string digest_title() const { return zh ? "💰 回本快报" : "💰 Payback digest"; }
  std::string celebrate_title() const { return zh ? "值得庆祝的时刻" : "A moment worth celebrating"; }
  std::string celebrate_keep() const { return zh ? "继续用，继续赚" : "Keep using, keep earning"; }
  std::string quip1() const { return zh ? "每一次打开，都是在赚回当初的决定。" : "Every open is another step toward payback."; }
  std::string quip2() const { return zh ? "时间是你最便宜的合伙人。" : "Time is your cheapest business partner."; }
  std::string quip3() const { return zh ? "今天不用，它也在帮你回本——不，还是用用它吧。" : "It pays back even idle — but go use it anyway."; }
  std::string quip4() const { return zh ? "冲动是魔鬼，折旧是天使。" : "Impulse is the devil; depreciation is the angel."; }
  std::string quip5() const { return zh ? "好的购买，是用得越久越便宜。" : "A good purchase gets cheaper every day you use it."; }
  std::string quip6() const { return zh ? "别数钱了——好吧，再数一次。" : "Stop counting the money — okay, one more look."; }
  std::string curve_day_label() const { return zh ? "第 {day} 天" : "day {day}"; }
  std::string update_available_body() const { return zh ? "发现新版本 {version}（{size}），要现在下载吗？" : "Payback {version} ({size}) is available. Download it now?"; }
  std::string update_ready_body() const { return zh ? "新版本 {version} 已下载完成，退出并安装？" : "Payback {version} is downloaded. Quit and install?"; }
  std::string update_install_failed_body() const { return zh ? "上次更新安装失败（msiexec 错误 {code}）。当前版本不受影响，可稍后在「检查更新」重试，或到 GitHub Releases 手动下载安装包。" : "The last update did not install (msiexec error {code}). This version is unaffected — retry from Check Updates later, or download the installer from GitHub Releases."; }
  std::string update_install_note() const { return zh ? "下载已完成并通过签名校验。用新版 tar.gz 覆盖应用目录即可完成更新。" : "The download is verified. Extract the new tar.gz over the app directory to finish the update."; }
  std::string update_open_folder() const { return zh ? "打开所在文件夹" : "Open folder"; }
  std::string digest_body() const { return zh ? "已赚回 {earned}，继续用，继续赚。" : "Earned back {earned}. Keep using, keep earning."; }
  std::string digest_body_best() const { return zh ? "{name} 今天只要 {cost}" : "{name} costs just {cost} today"; }
  std::string activate_pro_menu() const { return zh ? "激活 Payback Pro…" : "Activate Payback Pro…"; }
  std::string license_key_label() const { return zh ? "授权码" : "License key"; }
  std::string activate_button() const { return zh ? "激活" : "Activate"; }
  std::string activation_failed() const { return zh ? "激活失败" : "Activation failed"; }
  std::string pro_active_title() const { return zh ? "Payback Pro 已激活" : "Payback Pro activated"; }
  std::string pro_licensed_to() const { return zh ? "Pro · 授权给 {name}" : "Pro · licensed to {name}"; }
  std::string free_device_limit_note() const { return zh ? "免费版最多记录 10 台设备" : "The free version holds up to 10 devices"; }
  std::string license_hint() const { return zh ? "在购买确认邮件里找到 PB1 开头的授权码，粘贴到这里。离线验证，不上传任何信息。" : "Paste the PB1 token from your purchase email. Verification is offline — nothing is uploaded."; }
};

}  // namespace payback::linux_strings
