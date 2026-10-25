import SwiftUI

// GENERATED from shared/strings/strings.json by scripts/gen-strings.js.
// Do not edit by hand: change strings.json and re-run the generator.
// Language resolution: an in-app override ("zh" / "en") wins; otherwise
// the system language decides, with zh as the fallback.

enum L10n {
    /// in-app language override; nil = follow the system. UI-set on the main
    /// actor only; nonisolated(unsafe) keeps the generated type usable from
    /// any view body without concurrency annotations.
    nonisolated(unsafe) static var override: String?

    static var prefersChinese: Bool {
        switch override {
        case "zh": return true
        case "en": return false
        default: return Locale.preferredLanguages.first?.hasPrefix("zh") ?? true
        }
    }

    static func t(_ key: Key) -> String {
        prefersChinese ? key.zh : key.en
    }

    enum Key {
        case startingBackend
        case backendReady
        case backendError
        case configError
        case appName
        case tagline
        case totalSpent
        case earnedBack
        case deviceCount
        case overallDaily
        case addDevice
        case checkUpdatesMenu
        case helpMenu
        case emptyTitle
        case emptyHint
        case sortBy
        case sortAdded
        case sortDailyCost
        case sortPayback
        case dailyCost
        case perDay
        case heldFor
        case days
        case paidBack
        case paybackProgress
        case paybackEta
        case earnedSoFar
        case stillToEarn
        case willingPerDay
        case notSet
        case milestones
        case edit
        case delete
        case deleteConfirmTitle
        case deleteConfirmText
        case cancel
        case save
        case saving
        case saveFailed
        case genericError
        case name
        case icon
        case category
        case price
        case priceRequiredHint
        case purchaseDate
        case yearField
        case monthField
        case jumpToToday
        case dateInvalidHint
        case notes
        case willingHint
        case willingLabel
        case categoryComputer
        case categoryPhone
        case categoryTablet
        case categoryWatch
        case categoryAudio
        case categoryCamera
        case categoryGaming
        case categoryAppliance
        case categoryAccessory
        case categoryOther
        case milestoneDays100
        case milestoneDays365
        case milestoneDays1000
        case milestoneCpd10
        case milestoneCpd5
        case milestoneCpd2
        case milestoneCpd1
        case milestoneCpd05
        case milestonePaidBack
        case updates
        case updateCheckTitle
        case upToDate
        case updateAvailable
        case downloadUpdate
        case downloading
        case installNow
        case installFailed
        case updateError
        case retry
        case close
        case currency
        case languageAuto
        case languageTitle
        case digestTitle
        case celebrateTitle
        case celebrateKeep
        case quip1
        case quip2
        case quip3
        case quip4
        case quip5
        case quip6
        case curveDayLabel
        case updateAvailableBody
        case updateReadyBody
        case updateInstallFailedBody
        case updateInstallNote
        case updateOpenFolder
        case digestBody
        case digestBodyBest
        case aboutMenu
        case aboutContent
        case aboutLicense
        case aboutWebsite
        case aboutReportIssue
        case settingsTitle
        case dataSyncSection
        case syncHowItWorks
        case syncStatusOff
        case syncStatusOn
        case syncEnable
        case syncDisable
        case syncDisableConfirmTitle
        case syncDisableConfirmBody
        case syncFolderLabel
        case syncChooseFolder
        case syncPickFolderTitle
        case syncDetectedTitle
        case syncDetectedBody
        case syncUseDetected
        case syncMachinesLabel
        case syncMachineNameLabel
        case syncLastMergeLabel
        case syncIgnoredWarning
        case syncOpenFolder
        case syncEnableFailed
        case currencyHint

        var zh: String {
            switch self {
            case .startingBackend: return "正在启动 Racket 引擎…"
            case .backendReady: return "Racket CS 就绪"
            case .backendError: return "后端错误"
            case .configError: return "配置错误"
            case .appName: return "Payback 回本"
            case .tagline: return "用得越久，越接近回本"
            case .totalSpent: return "累计投入"
            case .earnedBack: return "已赚回"
            case .deviceCount: return "设备"
            case .overallDaily: return "整体日均"
            case .addDevice: return "添加设备"
            case .checkUpdatesMenu: return "检查更新…"
            case .helpMenu: return "帮助"
            case .emptyTitle: return "还没有设备"
            case .emptyHint: return "添加你的第一台设备，看着它的每日成本一天天降下去。"
            case .sortBy: return "排序"
            case .sortAdded: return "加入时间"
            case .sortDailyCost: return "日均成本"
            case .sortPayback: return "回本进度"
            case .dailyCost: return "每日成本"
            case .perDay: return "/天"
            case .heldFor: return "已持有"
            case .days: return "天"
            case .paidBack: return "已回本"
            case .paybackProgress: return "回本进度"
            case .paybackEta: return "预计回本"
            case .earnedSoFar: return "按你的心理价位，已经多用出"
            case .stillToEarn: return "还差"
            case .willingPerDay: return "每天愿意付"
            case .notSet: return "未设置"
            case .milestones: return "里程碑"
            case .edit: return "编辑"
            case .delete: return "删除"
            case .deleteConfirmTitle: return "删除这台设备？"
            case .deleteConfirmText: return "记录删除后无法恢复。"
            case .cancel: return "取消"
            case .save: return "保存"
            case .saving: return "保存中…"
            case .saveFailed: return "保存失败"
            case .genericError: return "出了点问题"
            case .name: return "名称"
            case .icon: return "图标"
            case .category: return "分类"
            case .price: return "购买价格"
            case .priceRequiredHint: return "请输入大于 0 的金额"
            case .purchaseDate: return "购买日期"
            case .yearField: return "年"
            case .monthField: return "月"
            case .jumpToToday: return "回到今天"
            case .dateInvalidHint: return "按 年-月-日 填写，例如 2026-10-15"
            case .notes: return "备注"
            case .willingHint: return "设置“每天愿意付多少钱”，就能看到回本进度。"
            case .willingLabel: return "心理价位（每天）"
            case .categoryComputer: return "电脑"
            case .categoryPhone: return "手机"
            case .categoryTablet: return "平板"
            case .categoryWatch: return "手表"
            case .categoryAudio: return "音频"
            case .categoryCamera: return "相机"
            case .categoryGaming: return "游戏"
            case .categoryAppliance: return "家电"
            case .categoryAccessory: return "配件"
            case .categoryOther: return "其他"
            case .milestoneDays100: return "百日纪念"
            case .milestoneDays365: return "陪伴一整年"
            case .milestoneDays1000: return "一千天老友"
            case .milestoneCpd10: return "日均低于 10 元"
            case .milestoneCpd5: return "日均低于 5 元"
            case .milestoneCpd2: return "日均低于 2 元"
            case .milestoneCpd1: return "日均低于 1 元"
            case .milestoneCpd05: return "日均低于 5 毛"
            case .milestonePaidBack: return "完全回本"
            case .updates: return "软件更新"
            case .updateCheckTitle: return "正在检查更新…"
            case .upToDate: return "已是最新版本"
            case .updateAvailable: return "发现新版本"
            case .downloadUpdate: return "下载更新"
            case .downloading: return "正在下载…"
            case .installNow: return "退出并安装"
            case .installFailed: return "安装失败"
            case .updateError: return "更新失败"
            case .retry: return "重试"
            case .close: return "关闭"
            case .currency: return "货币"
            case .languageAuto: return "跟随系统"
            case .languageTitle: return "语言"
            case .digestTitle: return "💰 回本快报"
            case .celebrateTitle: return "值得庆祝的时刻"
            case .celebrateKeep: return "继续用，继续赚"
            case .quip1: return "每一次打开，都是在赚回当初的决定。"
            case .quip2: return "时间是你最便宜的合伙人。"
            case .quip3: return "今天不用，它也在帮你回本——不，还是用用它吧。"
            case .quip4: return "冲动是魔鬼，折旧是天使。"
            case .quip5: return "好的购买，是用得越久越便宜。"
            case .quip6: return "别数钱了——好吧，再数一次。"
            case .curveDayLabel: return "第 {day} 天"
            case .updateAvailableBody: return "发现新版本 {version}（{size}），要现在下载吗？"
            case .updateReadyBody: return "新版本 {version} 已下载完成，退出并安装？"
            case .updateInstallFailedBody: return "上次更新安装失败（msiexec 错误 {code}）。当前版本不受影响，可稍后在「检查更新」重试，或到 GitHub Releases 手动下载安装包。"
            case .updateInstallNote: return "下载已完成并通过签名校验。用新版 tar.gz 覆盖应用目录即可完成更新。"
            case .updateOpenFolder: return "打开所在文件夹"
            case .digestBody: return "已赚回 {earned}，继续用，继续赚。"
            case .digestBodyBest: return "{name} 今天只要 {cost}"
            case .aboutMenu: return "关于 Payback"
            case .aboutContent: return "记录电子产品的购买价格，看着每日成本一天天降下去\n用得越久，越接近回本"
            case .aboutLicense: return "以 MIT 许可证授权发布"
            case .aboutWebsite: return "访问官网"
            case .aboutReportIssue: return "问题反馈"
            case .settingsTitle: return "设置"
            case .dataSyncSection: return "数据与同步"
            case .syncHowItWorks: return "把数据放进任意网盘文件夹（iCloud Drive、OneDrive、坚果云、Syncthing…），任一台设备录入，处处可见。"
            case .syncStatusOff: return "未开启同步"
            case .syncStatusOn: return "同步已开启"
            case .syncEnable: return "开启同步…"
            case .syncDisable: return "停止同步"
            case .syncDisableConfirmTitle: return "停止同步？"
            case .syncDisableConfirmBody: return "本机数据保留在本地，不再与其他设备合并。"
            case .syncFolderLabel: return "同步文件夹"
            case .syncChooseFolder: return "选择文件夹…"
            case .syncPickFolderTitle: return "选择用于同步的文件夹"
            case .syncDetectedTitle: return "检测到同步文件夹"
            case .syncDetectedBody: return "检测到 {folder}，用它来同步吗？"
            case .syncUseDetected: return "使用"
            case .syncMachinesLabel: return "参与同步的设备"
            case .syncMachineNameLabel: return "本机名称"
            case .syncLastMergeLabel: return "上次合并"
            case .syncIgnoredWarning: return "{count} 个文件无法读取，已跳过（常见于网盘冲突副本）。"
            case .syncOpenFolder: return "打开文件夹"
            case .syncEnableFailed: return "无法开启同步"
            case .currencyHint: return "新设备的默认货币"
            }
        }

        var en: String {
            switch self {
            case .startingBackend: return "Starting Racket engine…"
            case .backendReady: return "Racket CS ready"
            case .backendError: return "Backend error"
            case .configError: return "Configuration error"
            case .appName: return "Payback"
            case .tagline: return "The longer you keep it, the more it pays back"
            case .totalSpent: return "Total spent"
            case .earnedBack: return "Earned back"
            case .deviceCount: return "devices"
            case .overallDaily: return "Overall daily"
            case .addDevice: return "Add Device"
            case .checkUpdatesMenu: return "Check for Updates…"
            case .helpMenu: return "Help"
            case .emptyTitle: return "No devices yet"
            case .emptyHint: return "Add your first device and watch its daily cost drop day by day."
            case .sortBy: return "Sort"
            case .sortAdded: return "Date added"
            case .sortDailyCost: return "Daily cost"
            case .sortPayback: return "Payback progress"
            case .dailyCost: return "Daily cost"
            case .perDay: return "/day"
            case .heldFor: return "Held for"
            case .days: return "days"
            case .paidBack: return "Paid back"
            case .paybackProgress: return "Payback progress"
            case .paybackEta: return "Break-even expected"
            case .earnedSoFar: return "Value gained beyond your price"
            case .stillToEarn: return "Remaining"
            case .willingPerDay: return "Willing to pay"
            case .notSet: return "Not set"
            case .milestones: return "Milestones"
            case .edit: return "Edit"
            case .delete: return "Delete"
            case .deleteConfirmTitle: return "Delete this device?"
            case .deleteConfirmText: return "This cannot be undone."
            case .cancel: return "Cancel"
            case .save: return "Save"
            case .saving: return "Saving…"
            case .saveFailed: return "Save failed"
            case .genericError: return "Something went wrong"
            case .name: return "Name"
            case .icon: return "Icon"
            case .category: return "Category"
            case .price: return "Price"
            case .priceRequiredHint: return "Enter an amount greater than 0"
            case .purchaseDate: return "Purchase date"
            case .yearField: return "Year"
            case .monthField: return "Month"
            case .jumpToToday: return "Jump to Today"
            case .dateInvalidHint: return "Use year-month-day, e.g. 2026-10-15"
            case .notes: return "Notes"
            case .willingHint: return "Set what a day with this device is worth to you, and watch it pay itself back."
            case .willingLabel: return "Worth per day"
            case .categoryComputer: return "Computer"
            case .categoryPhone: return "Phone"
            case .categoryTablet: return "Tablet"
            case .categoryWatch: return "Watch"
            case .categoryAudio: return "Audio"
            case .categoryCamera: return "Camera"
            case .categoryGaming: return "Gaming"
            case .categoryAppliance: return "Appliance"
            case .categoryAccessory: return "Accessory"
            case .categoryOther: return "Other"
            case .milestoneDays100: return "100 days together"
            case .milestoneDays365: return "A full year"
            case .milestoneDays1000: return "1000-day friend"
            case .milestoneCpd10: return "Under 10/day"
            case .milestoneCpd5: return "Under 5/day"
            case .milestoneCpd2: return "Under 2/day"
            case .milestoneCpd1: return "Under 1/day"
            case .milestoneCpd05: return "Under 0.50/day"
            case .milestonePaidBack: return "Fully paid back"
            case .updates: return "Software Update"
            case .updateCheckTitle: return "Checking for updates…"
            case .upToDate: return "You're up to date"
            case .updateAvailable: return "Update available"
            case .downloadUpdate: return "Download Update"
            case .downloading: return "Downloading…"
            case .installNow: return "Quit and Install"
            case .installFailed: return "Install failed"
            case .updateError: return "Update failed"
            case .retry: return "Retry"
            case .close: return "Close"
            case .currency: return "Currency"
            case .languageAuto: return "Auto"
            case .languageTitle: return "Language"
            case .digestTitle: return "💰 Payback digest"
            case .celebrateTitle: return "A moment worth celebrating"
            case .celebrateKeep: return "Keep using, keep earning"
            case .quip1: return "Every open is another step toward payback."
            case .quip2: return "Time is your cheapest business partner."
            case .quip3: return "It pays back even idle — but go use it anyway."
            case .quip4: return "Impulse is the devil; depreciation is the angel."
            case .quip5: return "A good purchase gets cheaper every day you use it."
            case .quip6: return "Stop counting the money — okay, one more look."
            case .curveDayLabel: return "day {day}"
            case .updateAvailableBody: return "Payback {version} ({size}) is available. Download it now?"
            case .updateReadyBody: return "Payback {version} is downloaded. Quit and install?"
            case .updateInstallFailedBody: return "The last update did not install (msiexec error {code}). This version is unaffected — retry from Check Updates later, or download the installer from GitHub Releases."
            case .updateInstallNote: return "The download is verified. Extract the new tar.gz over the app directory to finish the update."
            case .updateOpenFolder: return "Open folder"
            case .digestBody: return "Earned back {earned}. Keep using, keep earning."
            case .digestBodyBest: return "{name} costs just {cost} today"
            case .aboutMenu: return "About Payback"
            case .aboutContent: return "Log what your devices cost and watch the daily price drop\nThe longer you keep them, the more they pay back"
            case .aboutLicense: return "Released under the MIT License"
            case .aboutWebsite: return "Website"
            case .aboutReportIssue: return "Report an Issue"
            case .settingsTitle: return "Settings"
            case .dataSyncSection: return "Data & Sync"
            case .syncHowItWorks: return "Point Payback at any synced folder (iCloud Drive, OneDrive, Dropbox, Syncthing…). Enter a device once, see it everywhere."
            case .syncStatusOff: return "Sync is off"
            case .syncStatusOn: return "Sync is on"
            case .syncEnable: return "Turn On Sync…"
            case .syncDisable: return "Turn Off Sync"
            case .syncDisableConfirmTitle: return "Turn off sync?"
            case .syncDisableConfirmBody: return "Your data stays on this machine and no longer merges with your other devices."
            case .syncFolderLabel: return "Sync folder"
            case .syncChooseFolder: return "Choose Folder…"
            case .syncPickFolderTitle: return "Choose a Folder for Syncing"
            case .syncDetectedTitle: return "Sync Folder Found"
            case .syncDetectedBody: return "Found {folder}. Use it to sync your devices?"
            case .syncUseDetected: return "Use"
            case .syncMachinesLabel: return "Devices in This Sync"
            case .syncMachineNameLabel: return "This Machine's Name"
            case .syncLastMergeLabel: return "Last merge"
            case .syncIgnoredWarning: return "{count} file(s) could not be read and were skipped — often cloud conflict copies."
            case .syncOpenFolder: return "Open Folder"
            case .syncEnableFailed: return "Couldn't Enable Sync"
            case .currencyHint: return "Default currency for new devices"
            }
        }
    }
}
