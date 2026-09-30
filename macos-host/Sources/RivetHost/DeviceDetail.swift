import SwiftUI

// Detail sheet: full payback math, the milestone ladder, the cost curve, and
// destructive actions.

struct DeviceDetail: View {
    let device: Device
    let currency: String
    let onEdit: () -> Void
    let onDelete: () -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var confirmDelete = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 14) {
                Text(device.icon)
                    .font(.system(size: 42))
                VStack(alignment: .leading, spacing: 2) {
                    Text(device.name).font(.title2.bold())
                    Text("\(Money.major(device.priceMinor, code: device.currency)) · \(device.purchaseDate)")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                if device.computed.paidBack {
                    Text("🎉 " + L10n.t(.paidBack))
                        .font(.callout.bold())
                        .foregroundStyle(.green)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 5)
                        .background(Color.green.opacity(0.12))
                        .clipShape(Capsule())
                }
            }

            HStack(spacing: 24) {
                Stat(label: L10n.t(.dailyCost),
                     value: Money.perDay(device.computed.costPerDayMinor, code: device.currency))
                Stat(label: L10n.t(.heldFor),
                     value: "\(device.computed.daysHeld) \(L10n.t(.days))")
                if device.computed.willingSet {
                    Stat(label: L10n.t(.willingPerDay),
                         value: Money.major(Double(device.willingPerDayMinor ?? 0),
                                            code: device.currency))
                }
            }
            .padding(14)
            .background(Color(nsColor: .controlBackgroundColor))
            .cornerRadius(10)

            CostCurve(priceMinor: device.priceMinor,
                      daysHeld: Int(device.computed.daysHeld),
                      currency: currency)
                .frame(height: 90)

            paybackSection

            milestoneLadder

            HStack {
                Button(L10n.t(.edit), action: onEdit)
                Spacer()
                Button(L10n.t(.delete), role: .destructive) { confirmDelete = true }
            }
            .padding(.top, 4)
        }
        .padding(20)
        .frame(width: 460)
        .confirmationDialog(L10n.t(.deleteConfirmTitle),
                            isPresented: $confirmDelete,
                            titleVisibility: .visible) {
            Button(L10n.t(.delete), role: .destructive, action: onDelete)
            Button(L10n.t(.cancel), role: .cancel) {}
        } message: {
            Text(L10n.t(.deleteConfirmText))
        }
    }

    @ViewBuilder
    private var paybackSection: some View {
        let computed = device.computed
        if computed.willingSet {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text(L10n.t(.paybackProgress))
                        .font(.callout)
                    Spacer()
                    Text(computed.paidBack
                         ? L10n.t(.paidBack)
                         : String(format: "%.0f%%", (computed.paybackProgress ?? 0) * 100))
                        .font(.callout.bold())
                        .foregroundStyle(computed.paidBack ? Color.green : Color.primary)
                }
                ProgressView(value: min(computed.paybackProgress ?? 0, 1))
                    .tint(computed.paidBack ? .green : .accentColor)

                if computed.paidBack {
                    if let earned = computed.earnedMinor {
                        Text("\(L10n.t(.earnedSoFar)) \(Money.major(earned, code: device.currency))")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                } else if let eta = computed.paybackEta {
                    Text("\(L10n.t(.paybackEta)): \(eta)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(14)
            .background(Color(nsColor: .controlBackgroundColor))
            .cornerRadius(10)
        } else {
            Text(L10n.t(.willingHint))
                .font(.caption)
                .foregroundStyle(.secondary)
                .padding(10)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color(nsColor: .controlBackgroundColor).opacity(0.6))
                .cornerRadius(10)
        }
    }

    private var milestoneLadder: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(L10n.t(.milestones))
                .font(.callout.bold())
            ForEach(device.computed.milestones) { milestone in
                HStack(spacing: 8) {
                    Text(MilestoneBadge.emoji(for: milestone.key))
                        .font(.callout)
                        .opacity(milestone.achieved ? 1 : 0.25)
                    Text(MilestoneBadge.label(for: milestone.key))
                        .font(.callout)
                        .foregroundStyle(milestone.achieved ? .primary : .secondary)
                    Spacer()
                    if milestone.achieved {
                        Image(systemName: "checkmark.circle.fill")
                            .foregroundStyle(.green)
                    }
                }
            }
        }
        .padding(14)
        .background(Color(nsColor: .controlBackgroundColor))
        .cornerRadius(10)
    }
}

// The daily-cost curve: price/day for every day since purchase. It only ever
// goes down — that is the whole product.
struct CostCurve: View {
    let priceMinor: Int64
    let daysHeld: Int
    let currency: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            GeometryReader { geometry in
                let points = curvePoints(width: geometry.size.width,
                                         height: geometry.size.height)
                ZStack {
                    Path { path in
                        for (index, point) in points.enumerated() {
                            if index == 0 {
                                path.move(to: point)
                            } else {
                                path.addLine(to: point)
                            }
                        }
                    }
                    .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 2, lineCap: .round))

                    if let last = points.last {
                        Circle()
                            .fill(Color.accentColor)
                            .frame(width: 6, height: 6)
                            .position(last)
                    }
                }
            }
            HStack {
                Text("day 1")
                Spacer()
                Text("day \(daysHeld)")
            }
            .font(.caption2)
            .foregroundStyle(.secondary)
        }
    }

    private func curvePoints(width: CGFloat, height: CGFloat) -> [CGPoint] {
        guard daysHeld > 1 else { return [] }
        let maxDays = max(daysHeld, 30)
        let maxValue = Double(priceMinor)  // day 1: the whole price
        return (1...daysHeld).map { day in
            let x = CGFloat(day - 1) / CGFloat(maxDays - 1) * width
            let value = Double(priceMinor) / Double(day)
            let y = CGFloat(1 - value / maxValue) * height
            return CGPoint(x: min(x, width), y: height - y)
        }
    }
}
