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
            CommandGroup(replacing: .appInfo) {
                Button(L10n.t(.aboutMenu)) { model.showAbout = true }
            }
            // Family menu convention: Check for Updates… lives in the App
            // menu directly below About.
            CommandGroup(after: .appInfo) {
                Button(L10n.t(.checkUpdatesMenu)) { model.checkForUpdates() }
                    .keyboardShortcut("u", modifiers: .command)
            }
        }
        // SwiftUI.Settings spelled out: our wire model type is also named
        // Settings and would shadow the scene otherwise
        SwiftUI.Settings {
            SettingsView()
                .environmentObject(model)
                .frame(width: 480)
                .frame(minHeight: 420)
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
    /// device id the list should select after the next reload (a fresh add)
    @Published var pendingSelection: Device.ID?
    @Published var showAbout = false        // presents AboutSheet

    // Issue #30: current macOS hosts can force-dismiss a sheet while the
    // user edits a field on a failed form — the keystroke whose result is
    // valid closes the sheet and loses the typed correction. The form
    // mirrors (draft, phase) up here on every change; when the sheet goes
    // away while the phase is still `.failed` (no Save/Retry/Esc path ran,
    // those all reset the phase first), the dismissal was the platform, not
    // the user, and the form re-presents with everything typed. CI proved
    // the first cut of this defense (re-presenting the failed phase, banner
    // included) loses a war of attrition: those hosts dismiss the failed
    // form again on the continued edit, until any attempt budget is spent.
    // So the re-assertion presents a FRESH idle form carrying the typed
    // draft: idle forms edit stably on these hosts (the acceptance suite
    // types on idle forms throughout), which breaks the trigger while
    // keeping the input; the next failed save re-renders the banner from
    // scratch. Bounded per failed-phase stretch, and idle re-presentations
    // are never re-asserted, so a host dismissing even idle forms simply
    // closes instead of looping.
    struct DeviceFormRescue {
        var draft: DeviceForm.Draft?
        var phase: DeviceForm.SavePhase = .idle
        var editingRef: Device?
        var attempts = 0
    }
    private var formRescue = DeviceFormRescue()

    /// fresh form: the rescue mirror starts clean
    func beginNewDeviceForm() {
        formRescue = DeviceFormRescue()
        showingNewDevice = true
    }

    /// edit form: remember which row is being edited so a forced dismissal
    /// can re-present it (the item binding nils out on any dismissal)
    func beginEditDeviceForm(_ device: Device) {
        formRescue = DeviceFormRescue()
        formRescue.editingRef = device
        editingDevice = device
    }

    /// called by the form after every (draft, phase) change
    func noteDeviceFormSnapshot(draft: DeviceForm.Draft,
                                phase: DeviceForm.SavePhase,
                                editing: Device?) {
        rescueLog("snapshot editing=\(editing != nil) phase=\(phase) draft=\(draft.name)/\(draft.priceText)")
        formRescue.draft = draft
        formRescue.phase = phase
        if let editing { formRescue.editingRef = editing }
        if phase == .idle { formRescue.attempts = 0 }
    }

    /// presentation seed: the rescued state after a forced dismissal, else fresh
    func rescuedFormDraft() -> DeviceForm.Draft? { formRescue.draft }
    func rescuedFormPhase() -> DeviceForm.SavePhase { formRescue.phase }

    /// one stderr line per rescue event: the e2e suite captures the app's
    /// stderr, and #30's trigger cannot be seen from the AX tree alone
    private func rescueLog(_ line: String) {
        FileHandle.standardError.write(Data("payback: rescue: \(line)\n".utf8))
    }

    /// the sheet went away: re-present it only for a platform forced
    /// dismissal (phase still `.failed`); an intended close has already
    /// reset the phase to `.idle`
    func deviceFormDismissed(editing: Bool) {
        rescueLog("dismissed editing=\(editing) phase=\(formRescue.phase) attempts=\(formRescue.attempts)")
        guard case .failed = formRescue.phase,
              formRescue.draft != nil,
              formRescue.attempts < 3 else {
            formRescue = DeviceFormRescue()
            rescueLog("dismissed: no re-assert")
            return
        }
        formRescue.attempts += 1
        // the banner does not survive the re-presentation (see above): the
        // seed phase drops to idle so the form comes back editable and
        // stable; the draft carries the user's typed values
        formRescue.phase = .idle
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
            // outside the dismissal transaction: re-presenting in the same
            // runloop turn is swallowed by SwiftUI's single-sheet rule
            self.rescueLog("re-assert #\(self.formRescue.attempts)")
            if editing, let ref = self.formRescue.editingRef {
                self.editingDevice = ref
            } else {
                self.showingNewDevice = true
            }
        }
    }

    @Published var syncState: SyncState?    // snapshot for the settings pane
    private var syncRevisionSeen: Int64 = -1
    private var syncPollTimer: Timer?

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

    /// Synchronous backend start on the main actor (taskly family pattern):
    /// spawning + handshake is fast, and keeping it here avoids capturing
    /// non-Sendable state across a detached task — older Swift 6 toolchains
    /// (macOS Intel runners) reject that pattern outright.
    func start() {
        guard backend == nil else { return }
        UpdaterInstaller.cleanupStaleBackup()
        do {
            let config = try Self.runtimeConfiguration()
            let backend = EmbeddedRacketBackend(configuration: config)
            self.backend = backend
            do {
                try backend.start()
                FileHandle.standardError.write(
                    Data("payback: backend started\n".utf8))
                ready = true
                status = ""
                reload()
                autoCheckForUpdates()
                runDailyDigest()
                startSyncPolling()
            } catch {
                FileHandle.standardError.write(
                    Data("payback: backend failed: \(error)\n".utf8))
                status = L10n.t(.backendError) + ": \(error)"
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
                // mixed portfolios join per-currency totals; the combined
                // scalar is null then (#23)
                let earnedText: String
                if let total = digest.earnedTotalMinor {
                    earnedText = Money.major(total, code: settings.currency)
                } else if let per = digest.earnedPerCurrency, !per.isEmpty {
                    earnedText = per
                        .map { amount -> String in
                            Money.major(amount.earnedTotalMinor,
                                        code: amount.currency)
                        }
                        .joined(separator: " · ")
                } else {
                    earnedText = Money.major(0.0, code: settings.currency)
                }
                var body = L10n.t(.digestBody)
                    .replacingOccurrences(of: "{earned}", with: earnedText)
                if let name = digest.bestDeviceName {
                    let cost = Money.perDay(digest.bestDeviceCostPerDayMinor ?? 0,
                                            code: digest.bestDeviceCurrency
                                                ?? settings.currency)
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

    /// Persists a new device and returns its id. Failures throw to the
    /// form, which owns the retry — never to the window-level alert (#24).
    @discardableResult
    func addDevice(_ draft: DeviceDraft) async throws -> String {
        guard let api else { throw BackendUnavailable() }
        let payload = try JSONEncoder().encode(draft)
        let created = try await api.add_device(payload: payload)
        showingNewDevice = false
        reload()
        // the detail pane follows the record the user just wrote
        if let id = try? Self.deviceId(from: created) {
            pendingSelection = id
        }
        return (try? Self.deviceId(from: created)) ?? ""
    }

    /// Full replace of an existing device; the id travels in the payload.
    @discardableResult
    func updateDevice(_ draft: DeviceDraft, id: String) async throws -> String {
        guard let api else { throw BackendUnavailable() }
        var payload = try JSONEncoder().encode(draft)
        if var object = try JSONSerialization.jsonObject(with: payload) as? [String: Any] {
            object["id"] = id
            payload = try JSONSerialization.data(withJSONObject: object)
        }
        _ = try await api.update_device(payload: payload)
        editingDevice = nil
        reload()
        return id
    }

    private static func deviceId(from payload: Data) throws -> String {
        guard let object = try JSONSerialization.jsonObject(with: payload) as? [String: Any],
              let id = object["id"] as? String else {
            throw BackendUnavailable()
        }
        return id
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

    // ---------- sync (docs/sync-design.md) ----------

    /// Poll the engine: external shard changes bump the revision, and the
    /// poll answers with a load-all so the whole UI re-renders from the
    /// merged view. Also keeps the settings pane's snapshot warm.
    func startSyncPolling() {
        syncPollTimer?.invalidate()
        let timer = Timer(timeInterval: 5.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.pollSyncState() }
        }
        RunLoop.main.add(timer, forMode: .common)
        syncPollTimer = timer
        pollSyncState()
    }

    func refreshSyncState() {
        pollSyncState()
    }

    private func pollSyncState() {
        guard let api, ready else { return }
        Task {
            do {
                let data = try await api.sync_state()
                let state = try JSONDecoder().decode(SyncState.self, from: data)
                syncState = state
                if state.revision != syncRevisionSeen {
                    let first = syncRevisionSeen == -1
                    syncRevisionSeen = state.revision
                    // the first snapshot primes the pane; later bumps mean
                    // another machine (or this one via another window)
                    // changed the merged view
                    if !first { reload() }
                }
            } catch {
                // silent: the poll is an optimization, never an alarm
            }
        }
    }

    /// dir nil = turn sync off and bring the claims back to the local
    /// directory; a URL = point the engine at that folder
    func configureSync(toFolder folder: URL?) {
        guard let api else { return }
        Task {
            do {
                let dirValue: Any = folder.map { $0.path } ?? NSNull()
                let payload = try JSONSerialization.data(
                    withJSONObject: ["dir": dirValue])
                let data = try await api.configure_sync(payload: payload)
                syncState = try JSONDecoder().decode(SyncState.self, from: data)
                syncRevisionSeen = syncState?.revision ?? syncRevisionSeen
                reload()
            } catch {
                errorAlert = L10n.t(.syncEnableFailed) + ": " + Self.cleanError(error)
            }
        }
    }

    func renameMachine(_ name: String) {
        guard let api else { return }
        Task {
            do {
                let payload = try JSONEncoder().encode(
                    ["name": name.trimmingCharacters(in: .whitespacesAndNewlines)])
                let data = try await api.rename_machine(payload: payload)
                syncState = try JSONDecoder().decode(SyncState.self, from: data)
            } catch {
                errorAlert = Self.cleanError(error)
            }
        }
    }

    /// currency rides the sync whitelist: saving it claims a settings
    /// entry, so one machine's choice becomes every machine's default
    func saveCurrency(_ code: String) {
        guard let api, code != settings.currency else { return }
        Task {
            do {
                let payload = try JSONEncoder().encode(["currency": code])
                let data = try await api.save_settings(payload: payload)
                settings = try JSONDecoder().decode(Settings.self, from: data)
            } catch {
                errorAlert = Self.cleanError(error)
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

/// The backend pipe is not up yet; the device form reports this like any
/// other save failure instead of silently dropping the save (#24).
struct BackendUnavailable: LocalizedError {
    var errorDescription: String? { L10n.t(.startingBackend) }
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
