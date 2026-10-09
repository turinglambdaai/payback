import SwiftUI
import AppKit

// Update sheet driving the signed-manifest updater. The Racket backend
// verifies and downloads; this native adapter owns installation (mount the
// DMG, replace the app, relaunch).

struct UpdateView: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "arrow.down.circle")
                .font(.system(size: 44))
                .foregroundStyle(Color.accentColor)

            switch model.updateState.phase {
            case "checking":
                ProgressView()
                Text(L10n.t(.updateCheckTitle))

            case "downloading":
                Text(L10n.t(.downloading))
                ProgressView(value: Double(model.updateState.percent), total: 100)
                    .frame(width: 260)
                Text("\(model.updateState.percent)%")
                    .font(.caption)
                    .foregroundStyle(.secondary)

            case "available":
                Text(L10n.t(.updateAvailable))
                    .font(.headline)
                if let version = model.updateState.availableVersion {
                    Text("Payback \(version)")
                        .font(.title3)
                }
                if let size = model.updateInfo?.sizeBytes {
                    Text(Money.size(size))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Button(L10n.t(.downloadUpdate)) {
                    model.startDownload()
                }
                .buttonStyle(.borderedProminent)
                .keyboardShortcut(.defaultAction)
                Button(L10n.t(.close)) { dismiss() }

            case "downloaded":
                Text(L10n.t(.updateAvailable))
                    .font(.headline)
                if let version = model.updateState.availableVersion {
                    Text("Payback \(version)")
                        .font(.title3)
                }
                if let size = model.updateInfo?.sizeBytes {
                    Text(Money.size(size))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Button(L10n.t(.installNow)) {
                    model.installUpdate()
                }
                .buttonStyle(.borderedProminent)
                .keyboardShortcut(.defaultAction)

            case "error":
                Text("⚠️ " + L10n.t(.updateError))
                    .font(.headline)
                Text(model.updateState.message ?? "")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: 340)
                    .lineLimit(4)
                HStack {
                    Button(L10n.t(.retry)) { model.checkForUpdates() }
                    Button(L10n.t(.close)) { dismiss() }
                        .keyboardShortcut(.defaultAction)
                }

            default: // idle / up-to-date
                Text("✅ " + L10n.t(.upToDate))
                Button(L10n.t(.close)) { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(28)
        .frame(width: 380)
        .tint(PaybackTheme.accent)
    }
}

// Native installation adapter: replaces the running .app with the freshly
// downloaded one and relaunches. The previous bundle stays beside the new
// one as a rollback copy: restored if the copy fails, and removed on the
// next launch that runs from the target bundle (rollback no longer needed).
enum UpdaterInstaller {
    /// Running from the installation target means the last update worked:
    /// its rollback copy is stale and can go.
    static func cleanupStaleBackup() {
        let targetURL = installationTargetURL()
        guard Bundle.main.bundleURL.standardizedFileURL.path
                == targetURL.standardizedFileURL.path else { return }
        try? FileManager.default.removeItem(at: targetURL.appendingPathExtension("old"))
    }

    static func install(dmgAt dmgURL: URL) throws {
        let fileManager = FileManager.default
        let targetURL = installationTargetURL()
        let mountPoint = fileManager.temporaryDirectory
            .appendingPathComponent("payback-update-\(UUID().uuidString)")

        try fileManager.createDirectory(at: mountPoint, withIntermediateDirectories: true)

        try run("/usr/bin/hdiutil", ["attach", "-nobrowse", "-readonly",
                                     "-mountpoint", mountPoint.path,
                                     dmgURL.path])
        var detachLater = true
        defer { if detachLater { try? run("/usr/bin/hdiutil", ["detach", mountPoint.path, "-force"]) } }

        let contents = try fileManager.contentsOfDirectory(at: mountPoint,
                                                           includingPropertiesForKeys: nil)
        guard let appURL = contents.first(where: { $0.pathExtension == "app" }) else {
            throw InstallerError.noAppFound(mountPoint.path)
        }

        let backupURL = targetURL.appendingPathExtension("old")
        let hadPrevious = fileManager.fileExists(atPath: targetURL.path)
        if hadPrevious {
            try? fileManager.removeItem(at: backupURL)
            try fileManager.moveItem(at: targetURL, to: backupURL)
        }

        do {
            try fileManager.copyItem(at: appURL, to: targetURL)
        } catch {
            // put the previous version back before surfacing the failure
            if hadPrevious {
                try? fileManager.moveItem(at: backupURL, to: targetURL)
            }
            throw error
        }

        detachLater = false
        try? run("/usr/bin/hdiutil", ["detach", mountPoint.path, "-force"])

        // The DMG arrived over the network, so the quarantine xattr rides
        // into the copied bundle and Gatekeeper would block the very update
        // the user just approved. The backend already verified the artifact
        // (Ed25519 manifest + SHA-256) — clearing it here is safe.
        try? run("/usr/bin/xattr", ["-cr", targetURL.path])

        // relaunch from the new bundle, then end the old process
        NSWorkspace.shared.open(targetURL)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            NSApp.terminate(nil)
        }
    }

    private static func installationTargetURL() -> URL {
        let bundle = Bundle.main.bundleURL
        let applications = URL(fileURLWithPath: "/Applications")
        // already installed in /Applications: replace in place
        if bundle.deletingLastPathComponent() == applications {
            return bundle
        }
        return applications.appendingPathComponent("Payback.app")
    }

    @discardableResult
    static func run(_ launchPath: String, _ arguments: [String]) throws -> String {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: launchPath)
        process.arguments = arguments
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        try process.run()
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        guard process.terminationStatus == 0 else {
            let output = String(data: data, encoding: .utf8) ?? ""
            throw InstallerError.commandFailed(launchPath, process.terminationStatus, output)
        }
        return String(data: data, encoding: .utf8) ?? ""
    }

    enum InstallerError: LocalizedError {
        case noAppFound(String)
        case commandFailed(String, Int32, String)

        var errorDescription: String? {
            switch self {
            case .noAppFound(let path):
                return "no .app bundle found in \(path)"
            case .commandFailed(let command, let status, let output):
                return "\(command) exited \(status): \(output)"
            }
        }
    }
}
