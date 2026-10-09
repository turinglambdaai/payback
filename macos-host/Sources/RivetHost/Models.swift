import SwiftUI

// Wire model mirroring docs/data-format.md. Field names match the JSON
// keys exactly; Racket emits 'null for absent optionals which decodes to
// Swift nil.

struct LoadAllDocument: Codable {
    let app: AppInfo
    let settings: Settings
    let devices: [Device]
    let summary: Summary
}

struct AppInfo: Codable {
    let version: String
    let build: Int64
    let identifier: String
    let channel: String
}

struct Settings: Codable {
    var currency: String
    var updateAutoCheck: Bool
    var updateBaseUrl: String?
    var lastUpdateCheckAt: Int64?
    var rolloutBucket: Int64?
    /// the stored PB1 token; null on the free tier
    var licenseKey: String?

    init(currency: String = "CNY",
         updateAutoCheck: Bool = true,
         updateBaseUrl: String? = nil,
         lastUpdateCheckAt: Int64? = nil,
         rolloutBucket: Int64? = nil,
         licenseKey: String? = nil) {
        self.currency = currency
        self.updateAutoCheck = updateAutoCheck
        self.updateBaseUrl = updateBaseUrl
        self.lastUpdateCheckAt = lastUpdateCheckAt
        self.rolloutBucket = rolloutBucket
        self.licenseKey = licenseKey
    }
}

/// Result of activate-license / license-state (app/license.rkt).
struct LicenseState: Codable {
    let licensed: Bool
    let type: String?
    let subject: String?
    let expiry: String?
    let deviceLimit: Int?
    let reason: String?
}

struct Device: Codable, Identifiable {
    let id: String
    let name: String
    let icon: String
    let category: String
    let priceMinor: Int64
    let currency: String
    let purchaseDate: String
    let willingPerDayMinor: Int64?
    let notes: String?
    let createdAt: String
    let updatedAt: String
    let computed: Computed
}

struct Computed: Codable {
    let daysHeld: Int64
    let costPerDayMinor: Double
    let willingSet: Bool
    let earnedMinor: Double?
    let paybackProgress: Double?
    let paybackEta: String?
    let paidBack: Bool
    let milestones: [Milestone]

    var achievedMilestones: [Milestone] {
        milestones.filter(\.achieved)
    }
}

struct Milestone: Codable, Identifiable {
    let key: String
    let achieved: Bool
    /// true only on the load-all that first observes the achievement
    let new: Bool?
    var id: String { key }
}

/// A milestone worth celebrating this launch.
struct MilestoneHit: Identifiable {
    let deviceName: String
    let icon: String
    let key: String
    var id: String { deviceName + "/" + key }
}

struct DigestResult: Codable {
    let status: String
    let earnedTotalMinor: Double?
    let deviceCount: Int?
    let bestDeviceName: String?
    let bestDeviceCostPerDayMinor: Double?
}

struct Summary: Codable {
    let deviceCount: Int
    let totalSpentMinor: Int64
    let totalDaysHeld: Int64
    let avgCostPerDayMinor: Double
    let earnedTotalMinor: Double
    let paidBackCount: Int
    let bestDeviceId: String?
    let toughestDeviceId: String?

    static let empty = Summary(deviceCount: 0, totalSpentMinor: 0,
                               totalDaysHeld: 0, avgCostPerDayMinor: 0,
                               earnedTotalMinor: 0, paidBackCount: 0,
                               bestDeviceId: nil, toughestDeviceId: nil)
}

// Outgoing payloads. Optional properties encode as absent keys, which the
// backend treats exactly like JSON null.

struct DeviceDraft: Encodable {
    var name: String
    var icon: String
    var category: String
    var priceMinor: Int64
    var currency: String
    var purchaseDate: String
    var willingPerDayMinor: Int64?
    var notes: String
}

struct SettingsPatch: Encodable {
    var currency: String?
    var updateAutoCheck: Bool?
    var updateBaseUrl: String?
}

struct UpdateCheckResult: Codable {
    let status: String
    let message: String?
    let currentVersion: String?
    let availableVersion: String?
    let publishedAt: String?
    let installer: String?
    let sizeBytes: Int64?
}

struct UpdateState: Codable {
     let phase: String        // available | idle | checking | downloading | downloaded | error
    let percent: Int
    let message: String?
    let downloadedPath: String?
    let availableVersion: String?

    static let idle = UpdateState(phase: "idle", percent: 0, message: nil,
                                  downloadedPath: nil, availableVersion: nil)

    static func downloaded(_ path: String) -> UpdateState {
        UpdateState(phase: "downloaded", percent: 100, message: nil,
                    downloadedPath: path, availableVersion: nil)
    }

    static func error(_ message: String) -> UpdateState {
        UpdateState(phase: "error", percent: 0, message: message,
                    downloadedPath: nil, availableVersion: nil)
    }

    static let upToDate = UpdateState.idle
}
