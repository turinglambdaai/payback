import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var model: AppModel
    @State private var sortOrder: SortOrder = .added
    @State private var detailDevice: Device?

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
        VStack(spacing: 0) {
            SummaryHeader(summary: model.summary, currency: model.settings.currency,
                          quip: model.quip)
            content
        }
        .background(PaybackTheme.paper)
        .id(model.uiLanguage)   // rebuild the tree so every string re-resolves
        .toolbar {
            ToolbarItemGroup {
                Picker(L10n.t(.sortBy), selection: $sortOrder) {
                    ForEach(SortOrder.allCases) { order in
                        Text(order.label).tag(order)
                    }
                }
                .pickerStyle(.menu)


                Button {
                    model.showingNewDevice = true
                } label: {
                    Image(systemName: "plus.circle.fill")
                        .foregroundStyle(PaybackTheme.accent)
                }
                .help(L10n.t(.addDevice))
            }
        }
        .sheet(isPresented: $model.showingNewDevice) {
            DeviceForm(draft: DeviceForm.Draft.new(currency: model.settings.currency)) { draft in
                model.addDevice(draft)
            }
        }
        .sheet(item: $model.editingDevice) { device in
            DeviceForm(draft: DeviceForm.Draft.from(device)) { draft in
                model.updateDevice(draft, id: device.id)
            }
        }
        .sheet(item: $detailDevice) { device in
            DeviceDetail(device: device,
                         currency: model.settings.currency,
                         onEdit: {
                            detailDevice = nil
                            model.editingDevice = device
                         },
                         onDelete: {
                            detailDevice = nil
                            model.deleteDevice(device)
                         })
        }
        .sheet(isPresented: $model.showUpdateSheet) {
            UpdateView()
        }
        .sheet(isPresented: $model.showActivation) {
            ActivationView()
        }
        .sheet(item: $model.celebration) { payload in
            CelebrationView(hits: payload.hits) {
                model.celebration = nil
            }
        }
        .alert(item: Binding(
            get: { model.errorAlert.map(ErrorText.init) },
            set: { model.errorAlert = $0?.text }
        )) { item in
            Alert(title: Text(L10n.t(.updateError)), message: Text(item.text),
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
            Button(L10n.t(.addDevice)) { model.showingNewDevice = true }
                .buttonStyle(.borderedProminent)
                .tint(PaybackTheme.accent)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var deviceList: some View {
        ScrollView {
            LazyVStack(spacing: 12) {
                ForEach(sortOrder.sort(model.devices)) { device in
                    DeviceCard(device: device, currency: model.settings.currency)
                        .contentShape(Rectangle())
                        .onTapGesture { detailDevice = device }
                }
            }
            .padding(20)
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
struct SummaryHeader: View {
    let summary: Summary
    let currency: String
    var quip: String = ""

    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 12) {
                StatTile(label: L10n.t(.totalSpent),
                         value: Money.major(summary.totalSpentMinor, code: currency))
                StatTile(label: L10n.t(.earnedBack),
                         value: Money.major(summary.earnedTotalMinor, code: currency),
                         tint: summary.earnedTotalMinor > 0 ? PaybackTheme.earn : nil,
                         hero: summary.earnedTotalMinor > 0)
                StatTile(label: L10n.t(.overallDaily),
                         value: Money.perDay(summary.avgCostPerDayMinor, code: currency))
                StatTile(label: L10n.t(.deviceCount),
                         value: String(summary.deviceCount))
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
        .padding(.horizontal, 20)
        .padding(.vertical, 14)
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

// ---------- device card ----------

struct DeviceCard: View {
    let device: Device
    let currency: String

    var body: some View {
        HStack(spacing: 16) {
            DeviceIconBadge(icon: device.icon, category: device.category)

            VStack(alignment: .leading, spacing: 4) {
                Text(device.name)
                    .font(.headline)
                Text("\(L10n.t(.heldFor)) \(device.computed.daysHeld) \(L10n.t(.days)) · \(Money.major(device.priceMinor, code: device.currency))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                milestoneBadges
            }

            Spacer(minLength: 12)

            if device.computed.willingSet {
                AnimatedRing(progress: device.computed.paybackProgress ?? 0,
                             paidBack: device.computed.paidBack)
            }

            VStack(alignment: .trailing, spacing: 2) {
                Text(Money.perDay(device.computed.costPerDayMinor, code: device.currency))
                    .font(.system(.title2, design: .rounded).bold())
                    .monospacedDigit()
                    .foregroundStyle(device.computed.paidBack ? PaybackTheme.earn : PaybackTheme.ink)
                Text(L10n.t(.dailyCost) + " " + L10n.t(.perDay))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(PaybackTheme.cardPadding)
        .background(PaybackTheme.cardBackground())
    }

    @ViewBuilder
    private var milestoneBadges: some View {
        let achieved = device.computed.achievedMilestones
        if !achieved.isEmpty {
            HStack(spacing: 6) {
                ForEach(achieved.suffix(2)) { milestone in
                    PaybackTheme.badge(text: MilestoneBadge.shortLabel(for: milestone.key),
                                       emoji: MilestoneBadge.emoji(for: milestone.key))
                }
                if achieved.count > 2 {
                    Text("+\(achieved.count - 2)")
                        .font(.caption2.weight(.medium))
                        .foregroundStyle(.secondary)
                }
            }
        }
    }
}

/// Rounded emoji tile whose backdrop hue follows the device category.
struct DeviceIconBadge: View {
    let icon: String
    let category: String

    private var backdrop: Color {
        switch category {
        case "computer": return Color.blue.opacity(0.14)
        case "phone": return Color.teal.opacity(0.14)
        case "tablet": return Color.indigo.opacity(0.14)
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
            .font(.system(size: 30))
            .frame(width: 54, height: 54)
            .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(backdrop))
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
