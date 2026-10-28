import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: AppModel
    @State private var sortOrder: SortOrder = .added
    @State private var selectedID: Device.ID?
    @State private var confirmingDelete: Device?

    enum SortOrder: String, CaseIterable, Identifiable {
        case added
        case dailyCost
        case payback

        var id: String { rawValue }

        var label: String {
            switch self {
            case .added: return L10n.t(.sortAdded)
            case .dailyCost: return L10n.t(.sortDailyCost)
            case .payback: return L10n.t(.sortPayback)
            }
        }

        func sort(_ devices: [Device]) -> [Device] {
            switch self {
            case .added:
                return devices.sorted { $0.createdAt > $1.createdAt }
            case .dailyCost:
                return devices.sorted { $0.computed.costPerDayMinor < $1.computed.costPerDayMinor }
            case .payback:
                return devices.sorted {
                    ($0.computed.paybackProgress ?? -1) > ($1.computed.paybackProgress ?? -1)
                }
            }
        }
    }

    var body: some View {
        content
            .background(PaybackTheme.paper)
            .id(model.uiLanguage)   // rebuild the tree so every string re-resolves
            // a fresh add: the model publishes the confirmed id once the
            // reload lands it, and the detail pane follows the new record
            .onChange(of: model.pendingSelection) { _, newValue in
                guard let id = newValue else { return }
                selectedID = id
                model.pendingSelection = nil
            }
            .frame(minWidth: 980, minHeight: 600)
        .toolbar {
            ToolbarItemGroup {
                Picker(L10n.t(.sortBy), selection: $sortOrder) {
                    ForEach(SortOrder.allCases) { order in
                        Text(order.label).tag(order)
                    }
                }
                .pickerStyle(.menu)

                // the detail pane shows only the device; its actions live
                // here as quiet borderless icons, the way Mail/Notes do it
                Button {
                    if let device = selectedDevice {
                        model.beginEditDeviceForm(device)
                    }
                } label: {
                    Image(systemName: "pencil")
                }
                .disabled(selectedDevice == nil)
                .help(L10n.t(.edit))

                Button {
                    confirmingDelete = selectedDevice
                } label: {
                    Image(systemName: "trash")
                }
                .disabled(selectedDevice == nil)
                .help(L10n.t(.delete))

                Button {
                    model.beginNewDeviceForm()
                } label: {
                    Image(systemName: "plus.circle.fill")
                        .foregroundStyle(PaybackTheme.accent)
                }
                .help(L10n.t(.addDevice))
            }
        }
        .confirmationDialog(L10n.t(.deleteConfirmTitle),
                            isPresented: Binding(
                                get: { confirmingDelete != nil },
                                set: { if !$0 { confirmingDelete = nil } }),
                            titleVisibility: .visible,
                            presenting: confirmingDelete) { device in
            Button(L10n.t(.delete), role: .destructive) {
                model.deleteDevice(device)
                confirmingDelete = nil
            }
            Button(L10n.t(.cancel), role: .cancel) {
                confirmingDelete = nil
            }
        } message: { device in
            Text(L10n.t(.deleteConfirmText))
        }
        .sheet(isPresented: $model.showingNewDevice,
               onDismiss: { model.deviceFormDismissed(editing: false) }) {
            DeviceForm(draft: model.rescuedFormDraft() ?? DeviceForm.Draft.new(currency: model.settings.currency),
                       initialPhase: model.rescuedFormPhase(),
                       rescued: model.rescuedFormIsRescue(),
                       onSave: { draft in
                           try await model.addDevice(draft)
                       },
                       onSnapshot: { draft, phase in
                           model.noteDeviceFormSnapshot(draft: draft, phase: phase, editing: nil)
                       })
        }
        // The four secondary presentations live on one anchor view node each:
        // stacked .sheet modifiers on a single SwiftUI view node let any
        // presentation re-evaluation dismiss the active sheet — observed as
        // the add-device form vanishing while the user corrected a field
        // after a failed save on macOS 26 (docs/save-lifecycle.md)
        .background {
            SecondaryPresentations()
        }
        .alert(item: Binding(
            get: { model.errorAlert.map(ErrorText.init) },
            set: { model.errorAlert = $0?.text }
        )) { item in
            Alert(title: Text(L10n.t(.genericError)), message: Text(item.text),
                  dismissButton: .default(Text(L10n.t(.close))))
        }
    }

    @ViewBuilder
    private var content: some View {
        if !model.ready {
            VStack(spacing: 12) {
                ProgressView()
                Text(model.status)
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if model.devices.isEmpty {
            emptyState
        } else {
            deviceList
        }
    }

    private var emptyState: some View {
        VStack(spacing: 16) {
            ZStack {
                Circle()
                    .fill(PaybackTheme.accentSoft)
                    .frame(width: 96, height: 96)
                Text("💰")
                    .font(.system(size: 44))
            }
            Text(L10n.t(.emptyTitle))
                .font(.title2.bold())
            Text(L10n.t(.emptyHint))
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 380)
            Button(L10n.t(.addDevice)) { model.beginNewDeviceForm() }
                .buttonStyle(.borderedProminent)
                .tint(PaybackTheme.accent)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // Apple's answer to "browse a collection, inspect one item" is the
    // nonmodal split: selection-driven detail, no dialog between the user
    // and the next device (HIG: prefer nonmodal design; sheets are for
    // completing tasks, which is what edit/add keep doing).
    private var deviceList: some View {
        NavigationSplitView {
            sidebar
        } detail: {
            detailPane
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(PaybackTheme.paper)
        }
    }

    private var sidebar: some View {
        // the summary describes all devices, so it belongs to the
        // all-devices column: collapsing the sidebar is "focus on one
        // device", and the aggregate steps aside with the list
        VStack(spacing: 0) {
            SummaryHeader(summary: model.summary, currency: model.settings.currency,
                          quip: model.quip, compact: true)
            List(selection: selectionBinding) {
                ForEach(sortOrder.sort(model.devices)) { device in
                    SidebarDeviceRow(device: device)
                        .tag(device.id)
                        .contextMenu {
                            Button(L10n.t(.edit)) { model.beginEditDeviceForm(device) }
                            Button(L10n.t(.delete), role: .destructive) {
                                confirmingDelete = device
                            }
                        }
                }
            }
            .listStyle(.sidebar)
        }
        .navigationSplitViewColumnWidth(min: 300, ideal: 330, max: 400)
    }

    private var selectionBinding: Binding<Device.ID?> {
        Binding(get: { selectedDevice?.id },
                set: { selectedID = $0 })
    }

    // nil id (fresh launch, deleted selection) falls back to the first
    // device so the detail pane is never blank while devices exist
    private var selectedDevice: Device? {
        model.devices.first { $0.id == selectedID } ?? model.devices.first
    }

    @ViewBuilder
    private var detailPane: some View {
        if let device = selectedDevice {
            DeviceDetail(device: device,
                         currency: model.settings.currency)
        }
    }
}

/// Anchor node for the secondary presentations (edit device, update,
/// about, celebration): each `.sheet` modifier sits on its own zero-size
/// view so the add-device form's presentation can never be displaced by
/// a competing sheet modifier on the content node.
private struct SecondaryPresentations: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        Color.clear.frame(width: 0, height: 0)
            .sheet(item: $model.editingDevice,
                   onDismiss: { model.deviceFormDismissed(editing: true) }) { device in
                DeviceForm(draft: model.rescuedFormDraft() ?? DeviceForm.Draft.from(device),
                           initialPhase: model.rescuedFormPhase(),
                           rescued: model.rescuedFormIsRescue(),
                           onSave: { draft in
                               try await model.updateDevice(draft, id: device.id)
                           },
                           onSnapshot: { draft, phase in
                               model.noteDeviceFormSnapshot(draft: draft, phase: phase,
                                                            editing: device)
                           })
            }
        Color.clear.frame(width: 0, height: 0)
            .sheet(isPresented: $model.showUpdateSheet) {
                UpdateView()
            }
        Color.clear.frame(width: 0, height: 0)
            .sheet(isPresented: $model.showAbout) {
                AboutSheet()
            }
        Color.clear.frame(width: 0, height: 0)
            .sheet(item: $model.celebration) { payload in
                CelebrationView(hits: payload.hits) {
                    model.celebration = nil
                }
            }
    }
}

/// Toolbar globe menu: in-app language switch, applied live.
struct LanguageMenu: View {
    @EnvironmentObject private var model: AppModel

    private var binding: Binding<AppLanguage> {
        Binding(get: { model.uiLanguage }, set: { model.setLanguage($0) })
    }

    var body: some View {
        Menu {
            Picker(L10n.t(.languageTitle), selection: binding) {
                ForEach(AppLanguage.allCases) { language in
                    Text(language.label).tag(language)
                }
            }
            .pickerStyle(.inline)
        } label: {
            Image(systemName: "globe")
        }
        .menuStyle(.button)
        .help(L10n.t(.languageTitle))
    }
}

private struct ErrorText: Identifiable {
    let text: String
    var id: String { text }
}

// ---------- summary ----------

/// The portfolio strip: four stat tiles, with 「已赚回」 as the hero number.
/// Full-width row in a classic layout, 2×2 grid inside the split view's
/// sidebar (`compact`).
struct SummaryHeader: View {
    let summary: Summary
    let currency: String
    var quip: String = ""
    var compact: Bool = false

    var body: some View {
        VStack(spacing: 8) {
            if compact {
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())],
                          spacing: 8) {
                    tiles
                }
            } else {
                HStack(spacing: 12) {
                    tiles
                }
            }
            if !quip.isEmpty {
                HStack(spacing: 6) {
                    Text("✦").foregroundStyle(PaybackTheme.accent)
                    Text(quip)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 4)
            }
        }
        .padding(.horizontal, compact ? 14 : 20)
        .padding(.vertical, compact ? 12 : 14)
    }

    @ViewBuilder
    private var tiles: some View {
        StatTile(label: L10n.t(.totalSpent),
                 value: spentText)
        StatTile(label: L10n.t(.earnedBack),
                 value: earnedText,
                 tint: earnedPositive ? PaybackTheme.earn : nil,
                 hero: earnedPositive)
        StatTile(label: L10n.t(.overallDaily),
                 value: dailyText)
        StatTile(label: L10n.t(.deviceCount),
                 value: String(summary.deviceCount))
    }

    // Mixed portfolios never show one combined money value: the backend
    // nulls the scalars and the tiles join the per-currency groups (#23).
    private var groups: [CurrencyGroup]? {
        guard let groups = summary.currencyGroups, groups.count > 1 else {
            return nil
        }
        return groups
    }

    private var spentText: String {
        if let groups {
            return groups
                .map { group -> String in
                    Money.major(Double(group.totalSpentMinor),
                                code: group.currency)
                }
                .joined(separator: " · ")
        }
        return Money.major(summary.totalSpentMinor ?? 0, code: currency)
    }

    private var earnedText: String {
        if let groups {
            return groups
                .map { group -> String in
                    Money.major(group.earnedTotalMinor, code: group.currency)
                }
                .joined(separator: " · ")
        }
        return Money.major(summary.earnedTotalMinor ?? 0, code: currency)
    }

    private var earnedPositive: Bool { summary.earnedTotalMinor.map { $0 > 0 } ?? false }

    private var dailyText: String {
        if let groups {
            return groups
                .map { group -> String in
                    Money.perDay(group.avgCostPerDayMinor, code: group.currency)
                }
                .joined(separator: " · ")
        }
        return Money.perDay(summary.avgCostPerDayMinor ?? 0, code: currency)
    }
}

struct StatTile: View {
    let label: String
    let value: String
    var tint: Color?
    var hero = false

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(label)
                .font(.caption.weight(.medium))
                .foregroundStyle(.secondary)
            Text(value)
                .font(.system(hero ? .title2 : .title3, design: .rounded).bold())
                .monospacedDigit()
                .foregroundStyle(tint ?? PaybackTheme.ink)
                .lineLimit(1)
                .minimumScaleFactor(0.6)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
        .background(
            RoundedRectangle(cornerRadius: PaybackTheme.statCorner, style: .continuous)
                .fill(hero ? PaybackTheme.earnSoft : PaybackTheme.card)
        )
    }
}

// ---------- device sidebar row ----------

/// Sidebar row: the light identity strip Apple's split views use — the
/// rich surface (curve, progress, milestones, actions) lives in the detail
/// pane, one selection away.
struct SidebarDeviceRow: View {
    let device: Device

    var body: some View {
        HStack(spacing: 10) {
            DeviceIconBadge(icon: device.icon, category: device.category, size: 34)

            VStack(alignment: .leading, spacing: 2) {
                Text(device.name)
                    .font(.headline)
                    .lineLimit(1)
                Text(Money.perDay(device.computed.costPerDayMinor, code: device.currency))
                    .font(.caption)
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
            }

            Spacer(minLength: 8)

            if device.computed.willingSet {
                // size/lineWidth are real layout values — a scaleEffect hack
                // keeps the 46pt canvas and the selected row's clipping
                // cuts the ring in half
                AnimatedRing(progress: device.computed.paybackProgress ?? 0,
                             paidBack: device.computed.paidBack,
                             size: 26, lineWidth: 3)
            } else {
                milestoneEmojis
            }
        }
        .padding(.vertical, 2)
    }

    @ViewBuilder
    private var milestoneEmojis: some View {
        let achieved = device.computed.achievedMilestones
        if !achieved.isEmpty {
            Text(achieved.suffix(2).map { MilestoneBadge.emoji(for: $0.key) }.joined())
                .font(.caption2)
        }
    }
}

/// Rounded emoji tile whose backdrop hue follows the device category.
struct DeviceIconBadge: View {
    let icon: String
    let category: String
    var size: CGFloat = 54

    private var backdrop: Color {
        switch category {
        case "computer": return Color.blue.opacity(0.14)
        case "phone": return Color.teal.opacity(0.14)
        case "tablet": return Color.indigo.opacity(0.14)
        case "watch": return Color.mint.opacity(0.18)
        case "audio": return Color.purple.opacity(0.14)
        case "camera": return Color.orange.opacity(0.16)
        case "gaming": return Color.red.opacity(0.13)
        case "appliance": return Color.cyan.opacity(0.15)
        case "accessory": return Color.mint.opacity(0.16)
        default: return PaybackTheme.accentSoft
        }
    }

    var body: some View {
        Text(icon)
            .font(.system(size: size * 0.55))
            .frame(width: size, height: size)
            .background(RoundedRectangle(cornerRadius: size * 0.26, style: .continuous).fill(backdrop))
    }
}

enum MilestoneBadge {
    static func emoji(for key: String) -> String {
        switch key {
        case "days-100": return "🌱"
        case "days-365": return "📅"
        case "days-1000": return "🏆"
        case "cpd-10": return "💸"
        case "cpd-5": return "🌤"
        case "cpd-2": return "🍃"
        case "cpd-1": return "🪶"
        case "cpd-05": return "✨"
        case "paid-back": return "🎉"
        default: return "🏅"
        }
    }

    static func label(for key: String) -> String {
        switch key {
        case "days-100": return L10n.t(.milestoneDays100)
        case "days-365": return L10n.t(.milestoneDays365)
        case "days-1000": return L10n.t(.milestoneDays1000)
        case "cpd-10": return L10n.t(.milestoneCpd10)
        case "cpd-5": return L10n.t(.milestoneCpd5)
        case "cpd-2": return L10n.t(.milestoneCpd2)
        case "cpd-1": return L10n.t(.milestoneCpd1)
        case "cpd-05": return L10n.t(.milestoneCpd05)
        case "paid-back": return L10n.t(.milestonePaidBack)
        default: return key
        }
    }

    /// compact form for card chips
    static func shortLabel(for key: String) -> String {
        switch key {
        case "days-100": return "100d"
        case "days-365": return "1y"
        case "days-1000": return "1000d"
        case "cpd-10": return "<10"
        case "cpd-5": return "<5"
        case "cpd-2": return "<2"
        case "cpd-1": return "<1"
        case "cpd-05": return "<0.5"
        case "paid-back": return L10n.t(.paidBack)
        default: return key
        }
    }
}
