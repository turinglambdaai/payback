import SwiftUI

// The macOS settings window: Data & Sync (docs/sync-design.md M1) plus the
// default currency. Sync is deliberately a one-time "confirm, don't type"
// flow — common sync folders are probed first, the folder picker is the
// fallback, and turning it off is one confirmation away.

struct SettingsView: View {
    @EnvironmentObject private var model: AppModel
    @State private var machineNameDraft = ""
    @State private var machineNameLoaded = false
    @State private var confirmingDisable = false
    @State private var detectedFolder: URL?
    @State private var showingPicker = false

    var body: some View {
        Form {
            Section {
                Text(L10n.t(.syncHowItWorks))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                syncStatusRow
                if let state = model.syncState, state.enabled {
                    folderRow(state)
                    machineNameRow(state)
                    machinesRow(state)
                    if !state.ignoredFiles.isEmpty {
                        Label(
                            L10n.t(.syncIgnoredWarning)
                                .replacingOccurrences(of: "{count}",
                                                      with: String(state.ignoredFiles.count)),
                            systemImage: "exclamationmark.triangle")
                            .font(.callout)
                            .foregroundStyle(.orange)
                    }
                }
            } header: {
                Text(L10n.t(.dataSyncSection))
            }

            Section {
                Picker(L10n.t(.currency), selection: currencyBinding) {
                    ForEach(SettingsView.commonCurrencies, id: \.self) { code in
                        Text("\(code) — \(currencyName(code))").tag(code)
                    }
                }
                Text(L10n.t(.currencyHint))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(L10n.t(.currency))
            }
        }
        .formStyle(.grouped)
        .frame(minHeight: 460)
        .confirmationDialog(L10n.t(.syncDisableConfirmTitle),
                            isPresented: $confirmingDisable,
                            titleVisibility: .visible) {
            Button(L10n.t(.syncDisable), role: .destructive) {
                model.configureSync(toFolder: nil)
            }
            Button(L10n.t(.cancel), role: .cancel) {}
        } message: {
            Text(L10n.t(.syncDisableConfirmBody))
        }
        .alert(L10n.t(.syncDetectedTitle),
               isPresented: Binding(
                   get: { detectedFolder != nil },
                   set: { if !$0 { detectedFolder = nil } })) {
            Button(L10n.t(.syncUseDetected)) {
                if let folder = detectedFolder {
                    model.configureSync(toFolder: folder)
                }
                detectedFolder = nil
            }
            Button(L10n.t(.syncChooseFolder), role: .cancel) {
                detectedFolder = nil
                showingPicker = true
            }
            Button(L10n.t(.cancel), role: .cancel) {
                detectedFolder = nil
            }
        } message: {
            Text(L10n.t(.syncDetectedBody)
                .replacingOccurrences(of: "{folder}",
                                      with: detectedFolder?.lastPathComponent ?? ""))
        }
        .fileImporter(isPresented: $showingPicker,
                      allowedContentTypes: [.folder],
                      allowsMultipleSelection: false) { result in
            if case .success(let urls) = result, let folder = urls.first {
                let scoped = folder.startAccessingSecurityScopedResource()
                model.configureSync(toFolder: folder)
                if scoped { folder.stopAccessingSecurityScopedResource() }
            }
        }
        .onAppear {
            model.refreshSyncState()
        }
        .onChange(of: model.syncState?.machine.name) { _, newName in
            // load the draft once the first snapshot lands; user edits win
            if !machineNameLoaded, let newName {
                machineNameDraft = newName
                machineNameLoaded = true
            }
        }
    }

    @ViewBuilder
    private var syncStatusRow: some View {
        if let state = model.syncState {
            if state.enabled {
                HStack {
                    Label(L10n.t(.syncStatusOn), systemImage: "arrow.triangle.2.circlepath")
                        .foregroundStyle(PaybackTheme.earn)
                    Spacer()
                    Button(L10n.t(.syncDisable), role: .destructive) {
                        confirmingDisable = true
                    }
                    .controlSize(.small)
                }
            } else {
                HStack {
                    Label(L10n.t(.syncStatusOff), systemImage: "tray")
                        .foregroundStyle(.secondary)
                    Spacer()
                    Button(L10n.t(.syncEnable)) {
                        beginEnable()
                    }
                    .controlSize(.small)
                }
            }
        } else {
            HStack { ProgressView().controlSize(.small); Spacer() }
        }
    }

    private func folderRow(_ state: SyncState) -> some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(L10n.t(.syncFolderLabel))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(state.dir)
                    .font(.caption.monospaced())
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .textSelection(.enabled)
            }
            Spacer()
            Button(L10n.t(.syncOpenFolder)) {
                NSWorkspace.shared.open(URL(fileURLWithPath: state.dir))
            }
            .controlSize(.small)
        }
    }

    private func machineNameRow(_ state: SyncState) -> some View {
        HStack {
            Text(L10n.t(.syncMachineNameLabel))
            Spacer()
            TextField(state.machine.name, text: $machineNameDraft)
                .frame(maxWidth: 200)
                .multilineTextAlignment(.trailing)
                .onSubmit { model.renameMachine(machineNameDraft) }
            Button(L10n.t(.save)) {
                model.renameMachine(machineNameDraft)
            }
            .controlSize(.small)
            .disabled(machineNameDraft.trimmingCharacters(in: .whitespaces).isEmpty)
        }
    }

    private func machinesRow(_ state: SyncState) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(L10n.t(.syncMachinesLabel))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                if let last = state.lastMergeAt {
                    Text(L10n.t(.syncLastMergeLabel) + ": " + last)
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
            }
            ForEach(state.machines) { machine in
                HStack(spacing: 8) {
                    Image(systemName: machine.id == state.machine.id
                          ? "laptopcomputer.and.iphone"
                          : "laptopcomputer")
                        .foregroundStyle(.secondary)
                    Text(machine.name)
                    if machine.id == state.machine.id {
                        Text("(\(state.machine.name))")
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                }
                .font(.callout)
            }
        }
        .padding(.vertical, 2)
    }

    // ---------- enable flow ----------

    /// confirm-first: probe the common sync homes; the folder picker is the
    /// fallback when nothing familiar shows up
    private func beginEnable() {
        let candidates = Self.detectSyncFolders()
        if let first = candidates.first {
            detectedFolder = first
        } else {
            showingPicker = true
        }
    }

    static let commonCurrencies = ["CNY", "USD", "EUR", "JPY", "GBP", "HKD",
                                   "TWD", "KRW", "SGD", "AUD", "CAD", "CHF"]

    /// iCloud Drive first (the Apple-ecosystem default), then the cloud
    /// storage mounts File Provider exposes, then classic home folders
    static func detectSyncFolders() -> [URL] {
        let home = FileManager.default.homeDirectoryForCurrentUser
        var candidates: [URL] = []
        let icloud = home.appendingPathComponent(
            "Library/Mobile Documents/com~apple~CloudDocs/Payback")
        if FileManager.default.fileExists(atPath: icloud.path) {
            candidates.append(icloud)
        }
        if let cloudStorage = try? FileManager.default.contentsOfDirectory(
            at: home.appendingPathComponent("Library/CloudStorage"),
            includingPropertiesForKeys: nil) {
            for mount in cloudStorage.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
                let payback = mount.appendingPathComponent("Payback")
                if FileManager.default.fileExists(atPath: payback.path) {
                    candidates.append(payback)
                }
            }
        }
        for name in ["Dropbox", "OneDrive", "Nutstore Files", "坚果云"] {
            let payback = home.appendingPathComponent("\(name)/Payback")
            if FileManager.default.fileExists(atPath: payback.path) {
                candidates.append(payback)
            }
        }
        return candidates
    }

    private func currencyName(_ code: String) -> String {
        Locale.current.localizedString(forCurrencyCode: code) ?? code
    }

    private var currencyBinding: Binding<String> {
        Binding(get: { model.settings.currency },
                set: { model.saveCurrency($0) })
    }
}
