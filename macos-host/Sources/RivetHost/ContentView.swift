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
            SummaryHeader(summary: model.summary, currency: model.settings.currency)
            Divider()
            content
        }
        .background(Color(nsColor: .windowBackgroundColor))
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
                    Image(systemName: "plus")
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
        VStack(spacing: 14) {
            Text("💰")
                .font(.system(size: 56))
            Text(L10n.t(.emptyTitle))
                .font(.title2.bold())
            Text(L10n.t(.emptyHint))
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 380)
            Button(L10n.t(.addDevice)) { model.showingNewDevice = true }
                .buttonStyle(.borderedProminent)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var deviceList: some View {
        ScrollView {
            LazyVStack(spacing: 10) {
                ForEach(sortOrder.sort(model.devices)) { device in
                    DeviceCard(device: device, currency: model.settings.currency)
                        .contentShape(Rectangle())
                        .onTapGesture { detailDevice = device }
                }
            }
            .padding(16)
        }
    }
}

private struct ErrorText: Identifiable {
    let text: String
    var id: String { text }
}

// ---------- summary ----------

struct SummaryHeader: View {
    let summary: Summary
    let currency: String

    var body: some View {
        HStack(spacing: 28) {
            Stat(label: L10n.t(.totalSpent),
                 value: Money.major(summary.totalSpentMinor, code: currency))
            Stat(label: L10n.t(.earnedBack),
                 value: Money.major(summary.earnedTotalMinor, code: currency),
                 highlight: summary.earnedTotalMinor > 0)
            Stat(label: L10n.t(.overallDaily),
                 value: Money.perDay(summary.avgCostPerDayMinor, code: currency))
            Stat(label: L10n.t(.deviceCount),
                 value: String(summary.deviceCount))
        }
        .padding(.vertical, 12)
        .padding(.horizontal, 20)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct Stat: View {
    let label: String
    let value: String
    var highlight = false

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label)
                .font(.caption)
                .foregroundStyle(.secondary)
            Text(value)
                .font(.system(.title3, design: .rounded).bold())
                .foregroundStyle(highlight ? Color.green : Color.primary)
        }
    }
}

// ---------- device card ----------

struct DeviceCard: View {
    let device: Device
    let currency: String

    var body: some View {
        HStack(spacing: 14) {
            Text(device.icon)
                .font(.system(size: 34))
                .frame(width: 52, height: 52)
                .background(Color(nsColor: .controlBackgroundColor))
                .clipShape(RoundedRectangle(cornerRadius: 12))

            VStack(alignment: .leading, spacing: 3) {
                Text(device.name)
                    .font(.headline)
                Text("\(L10n.t(.heldFor)) \(device.computed.daysHeld) \(L10n.t(.days)) · \(Money.major(device.priceMinor, code: device.currency))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                milestoneBadges
            }

            Spacer()

            if device.computed.willingSet {
                PaybackRing(progress: device.computed.paybackProgress ?? 0,
                            paidBack: device.computed.paidBack)
            }

            VStack(alignment: .trailing, spacing: 2) {
                Text(Money.perDay(device.computed.costPerDayMinor, code: device.currency))
                    .font(.system(.title2, design: .rounded).bold())
                    .foregroundStyle(device.computed.paidBack ? Color.green : Color.primary)
                Text(L10n.t(.dailyCost) + " " + L10n.t(.perDay))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(14)
        .background(Color(nsColor: .controlBackgroundColor))
        .cornerRadius(12)
    }

    @ViewBuilder
    private var milestoneBadges: some View {
        let achieved = device.computed.achievedMilestones
        if !achieved.isEmpty {
            HStack(spacing: 4) {
                ForEach(achieved.prefix(3)) { milestone in
                    Text(MilestoneBadge.emoji(for: milestone.key))
                        .font(.caption2)
                }
                Text(MilestoneBadge.label(for: achieved[achieved.count - 1].key))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

struct PaybackRing: View {
    let progress: Double
    let paidBack: Bool

    var body: some View {
        ZStack {
            Circle()
                .stroke(Color.secondary.opacity(0.2), lineWidth: 5)
            Circle()
                .trim(from: 0, to: min(CGFloat(progress), 1))
                .stroke(paidBack ? Color.green : Color.accentColor,
                        style: StrokeStyle(lineWidth: 5, lineCap: .round))
                .rotationEffect(.degrees(-90))
            Text(paidBack ? "✓" : "\(Int((progress * 100).rounded()))")
                .font(.system(size: 11, weight: .bold, design: .rounded))
        }
        .frame(width: 42, height: 42)
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
}
