import SwiftUI
import RivetEmbedding
import RivetRuntime
import RivetSystem
import UserNotifications

@main
struct RivetHostApp: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup("Payback") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 680, minHeight: 480)
                .task { model.start() }
        }
        .windowResizability(.contentMinSize)
        .commands {
            CommandGroup(after: .newItem) {
                Button(L10n.t(.checkUpdatesMenu)) { model.checkForUpdates() }
                    .keyboardShortcut("u", modifiers: .command)
                Button(L10n.t(.activateProMenu)) { model.showActivation = true }
            }
        }
    }
}

/// UI language preference, persisted independently of the business data:
/// this is a view concern, so it lives in UserDefaults rather than the
/// payback.json document.
enum AppLanguage: String, CaseIterable, Identifiable {
    case system
    case zh
    case en

    var id: String { rawValue }

    var label: String {
        switch self {
        case .system: return L10n.t(.languageAuto)
        case .zh: return "中文"
        case .en: return "English"
        }
    }
}

@MainActor
final class AppModel: ObservableObject {
    @Published var ready = false
    @Published var status = L10n.t(.startingBackend)
    @Published var devices: [Device] = []
    @Published var settings = Settings()
    @Published var summary = Summary.empty
    @Published var appInfo = AppInfo(version: "0.0.0", build: 0, identifier: "", channel: "stable")
    @Published var updateState: UpdateState = .idle
    @Published var showUpdateSheet = false
    /// metadata of the newest available update (version, size); shown in the sheet
    @Published var updateInfo: UpdateCheckResult?
    @Published var errorAlert: String?
    @Published var celebration: CelebrationPayload?

struct CelebrationPayload: Identifiable {
    let id = UUID()
    let hits: [MilestoneHit]
}
    @Published var quip: String = ""

    @Published var editingDevice: Device?   // presents DeviceForm for an existing row
    @Published var showingNewDevice = false // presents DeviceForm for a new draft
    @Published var showActivation = false   // presents ActivationView

    /// changing this rebuilds the whole view tree, so strings re-resolve
    @Published var uiLanguage: AppLanguage {
        didSet {
            UserDefaults.standard.set(uiLanguage.rawValue, forKey: "payback.uiLanguage")
            L10n.override = uiLanguage == .system ? nil : uiLanguage.rawValue
        }
    }

    init() {
        let stored = UserDefaults.standard.string(forKey: "payback.uiLanguage")
            ?? AppLanguage.system.rawValue
        let language = AppLanguage(rawValue: stored) ?? .system
        self.uiLanguage = language
        L10n.override = language == .system ? nil : language.rawValue
    }

    func setLanguage(_ language: AppLanguage) {
        uiLanguage = language
    }

    private var backend: EmbeddedRacketBackend?
    private var pollTimer: Timer?

    var api: RivetAPI? {
        guard let backend else { return nil }
        return RivetAPI(client: backend.client)
    }

    func start() {
        guard backend == nil else { return }
        UpdaterInstaller.cleanupStaleBackup()
        do {
            let config = try Self.runtimeConfiguration()
            let backend = EmbeddedRacketBackend(configuration: config)
            self.backend = backend
            Task.detached { [backend, weak self] in
                do {
                    try backend.start()
                    FileHandle.standardError.write(
                        Data("payback: backend started\n".utf8))
                    await MainActor.run {
                        self?.ready = true
                        self?.status = ""
                        self?.reload()
                        self?.autoCheckForUpdates()
                        self?.runDailyDigest()
                    }
                } catch {
                    FileHandle.standardError.write(
                        Data("payback: backend failed: \(error)\n".utf8))
                    await MainActor.run {
                        self?.status = L10n.t(.backendError) + ": \(error)"
                    }
                }
            }
        } catch {
            FileHandle.standardError.write(
                Data("payback: config failed: \(error)\n".utf8))
            status = L10n.t(.configError) + ": \(error)"
        }
    }

    // ---------- emotional layer ----------

    /// rotating quip under the summary, changes once a day
    private func refreshQuip() {
        let quips = [L10n.Key.quip1, .quip2, .quip3, .quip4, .quip5, .quip6]
        let day = Calendar.current.ordinality(of: .day, in: .year, for: Date()) ?? 0
        quip = L10n.t(quips[day % quips.count])
    }

    /// celebrate milestones achieved since the last launch
    private func detectCelebrations(in devices: [Device]) {
        var hits: [MilestoneHit] = []
        for device in devices {
            for milestone in device.computed.milestones
            where milestone.new == true && milestone.achieved {
                hits.append(MilestoneHit(deviceName: device.name,
                                         icon: device.icon,
                                         key: milestone.key))
            }
        }
        if !hits.isEmpty {
            // let the list land first; celebrate right after
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: 800_000_000)
                self.celebration = CelebrationPayload(hits: hits)
            }
        }
    }

    /// once-per-day system notification: the payback digest. Permission
    /// denial is remembered and never re-prompted; every failure is silent.
    private func runDailyDigest() {
        refreshQuip()
        guard let api else { return }
        Task {
            do {
                let data = try await api.daily_digest()
                let digest = try JSONDecoder().decode(DigestResult.self, from: data)
                guard digest.status == "ok",
                      !UserDefaults.standard.bool(forKey: "payback.digest.denied") else { return }
                // a bare staged binary has no bundle id — UNUserNotificationCenter
                // raises NSException there (Swift cannot catch it); only ask
                // inside a real bundle
                guard Bundle.main.bundleIdentifier != nil else { return }
                let granted = (try? await RivetNotifications.requestAuthorization()) ?? false
                guard granted else {
                    UserDefaults.standard.set(true, forKey: "payback.digest.denied")
                    return
                }
                var body = L10n.t(.digestBody)
                    .replacingOccurrences(
                        of: "{earned}",
                        with: Money.major(digest.earnedTotalMinor ?? 0,
                                          code: settings.currency))
                if let name = digest.bestDeviceName {
                    let cost = Money.perDay(digest.bestDeviceCostPerDayMinor ?? 0,
                                            code: settings.currency)
                    body += "\n" + L10n.t(.digestBodyBest)
                        .replacingOccurrences(of: "{name}", with: name)
                        .replacingOccurrences(of: "{cost}", with: cost)
                }
                try? await RivetNotifications.show(title: L10n.t(.digestTitle),
                                                   body: body)
            } catch {
                // silent: the digest is a delight, never an interruption
                FileHandle.standardError.write(
                    Data("payback: digest error: \(error)\n".utf8))
            }
        }
    }

    // ---------- data ----------

    func reload() {
        guard let api, ready else { return }
        Task {
            do {
                let data = try await api.load_all()
                let document = try JSONDecoder().decode(LoadAllDocument.self, from: data)
                devices = document.devices
                settings = document.settings
                summary = document.summary
                appInfo = document.app
                detectCelebrations(in: document.devices)
            } catch {
                errorAlert = "\(error)"
            }
        }
    }

    func addDevice(_ draft: DeviceDraft) {
        guard let api else { return }
        Task {
            do {
                let payload = try JSONEncoder().encode(draft)
                _ = try await api.add_device(payload: payload)
                showingNewDevice = false
                reload()
            } catch {
                errorAlert = Self.cleanError(error)
            }
        }
    }

    func updateDevice(_ draft: DeviceDraft, id: String) {
        guard let api else { return }
        Task {
            do {
                var payload = try JSONEncoder().encode(draft)
                if var object = try JSONSerialization.jsonObject(with: payload) as? [String: Any] {
                    object["id"] = id
                    payload = try JSONSerialization.data(withJSONObject: object)
                }
                _ = try await api.update_device(payload: payload)
                editingDevice = nil
                reload()
            } catch {
                errorAlert = Self.cleanError(error)
            }
        }
    }

    func deleteDevice(_ device: Device) {
        guard let api else { return }
        Task {
            do {
                _ = try await api.delete_device(id: device.id)
                reload()
            } catch {
                errorAlert = Self.cleanError(error)
            }
        }
    }

    // ---------- license ----------

    var isPro: Bool { settings.licenseKey != nil }

    /// activate-license verifies offline and stores the token; a failure
    /// surfaces the backend's user-facing message
    func activateLicense(_ key: String) {
        guard let api else { return }
        Task {
            do {
                let payload = try JSONEncoder().encode(["key": key.trimmingCharacters(in: .whitespacesAndNewlines)])
                _ = try await api.activate_license(payload: payload)
                reload()
                showActivation = false
            } catch {
                errorAlert = L10n.t(.activationFailed) + ": " + Self.cleanError(error)
            }
        }
    }

    // ---------- updates ----------

    private func autoCheckForUpdates() {
        guard settings.updateAutoCheck else { return }
        Task {
            try? await Task.sleep(nanoseconds: 3_000_000_000)
            // silent on failure: a launch-time check never nags
            checkUpdates(throttled: true, present: false)
        }
    }

    func checkForUpdates() {
        checkUpdates(throttled: false, present: true)
    }

    private func checkUpdates(throttled: Bool, present: Bool) {
        guard let api else { return }
        Task {
            do {
                let data = try await api.check_updates(force: !throttled)
                let result = try JSONDecoder().decode(UpdateCheckResult.self, from: data)
                if result.status == "available" && present {
                    updateInfo = result
                    // an explicit available phase: the sheet offers the
                    // download (consent first, matching the Windows flow) —
                    // idle here rendered as "up to date" and nothing ever
                    // started the download
                    updateState = UpdateState(phase: "available", percent: 0,
                                              message: nil, downloadedPath: nil,
                                              availableVersion: result.availableVersion)
                    showUpdateSheet = true
                } else if result.status == "up-to-date" && present {
                    updateState = .upToDate
                    showUpdateSheet = true
                } else if result.status == "error" && present {
                    updateState = .error(result.message ?? "unknown error")
                    showUpdateSheet = true
                }
            } catch {
                if present {
                    updateState = .error("\(error)")
                    showUpdateSheet = true
                }
            }
        }
    }

    func startDownload() {
        guard let api else { return }
        Task {
            do {
                _ = try await api.start_download()
                startPolling()
            } catch {
                errorAlert = Self.cleanError(error)
            }
        }
    }

    private func startPolling() {
        pollTimer?.invalidate()
        let timer = Timer(timeInterval: 0.4, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.pollUpdateState() }
        }
        RunLoop.main.add(timer, forMode: .common)
        pollTimer = timer
        pollUpdateState()
    }

    private func pollUpdateState() {
        guard let api else { return }
        Task {
            do {
                let data = try await api.update_state()
                let state = try JSONDecoder().decode(UpdateState.self, from: data)
                updateState = state
                if state.phase != "downloading" {
                    pollTimer?.invalidate()
                    pollTimer = nil
                }
            } catch {
                pollTimer?.invalidate()
                pollTimer = nil
            }
        }
    }

    func installUpdate() {
        guard updateState.phase == "downloaded",
              let path = updateState.downloadedPath else { return }
        do {
            try UpdaterInstaller.install(dmgAt: URL(fileURLWithPath: path))
        } catch {
            errorAlert = L10n.t(.installFailed) + ": \(error)"
        }
    }

    // ---------- helpers ----------

    static func cleanError(_ error: Error) -> String {
        let text = "\(error)"
        // RVT1 failures arrive as "...error: <message>"; keep the message
        if let range = text.range(of: "error: ") {
            return String(text[range.upperBound...])
        }
        return text
    }

    private static func runtimeConfiguration() throws -> EmbeddedRacketConfiguration {
        let executable = Bundle.main.executableURL
            ?? URL(fileURLWithPath: CommandLine.arguments[0]).standardizedFileURL

        // Packaged apps keep Racket data in Contents/Resources. `raco rivet
        // dev` runs the staged executable directly, where runtime/res live
        // next to the executable. Pick the first complete layout so both
        // paths use exactly the same host binary.
        let roots = [
            Bundle.main.resourceURL,
            executable.deletingLastPathComponent()
        ].compactMap { $0 }

        for root in roots {
            let runtime = root.appendingPathComponent("runtime", isDirectory: true)
            let core = root.appendingPathComponent("res/core.zo")
            let required = [
                runtime.appendingPathComponent("petite.boot"),
                runtime.appendingPathComponent("scheme.boot"),
                runtime.appendingPathComponent("racket.boot"),
                core
            ]
            if required.allSatisfy({ FileManager.default.fileExists(atPath: $0.path) }) {
                // The embedded runtime resolves its foreign libraries
                // (libgmp and friends) relative to the working directory;
                // packaged apps must therefore run from their Resources
                // directory so "runtime/lib/..." lands inside the bundle
                // (scripts/post-package.sh copies the libraries there).
                FileManager.default.changeCurrentDirectoryPath(root.path)
                return EmbeddedRacketConfiguration(
                    executable: executable,
                    petiteBoot: required[0],
                    schemeBoot: required[1],
                    racketBoot: required[2],
                    core: core,
                    moduleName: RivetGeneratedConfig.moduleName,
                    entryName: RivetGeneratedConfig.entryName
                )
            }
        }

        throw HostError.missingRuntimeLayout(
            roots.map(\.path).joined(separator: ", ")
        )
    }
}

enum HostError: Error, CustomStringConvertible {
    case missingRuntimeLayout(String)

    var description: String {
        switch self {
        case .missingRuntimeLayout(let roots):
            return "missing Rivet runtime/res layout under: \(roots)"
        }
    }
}
