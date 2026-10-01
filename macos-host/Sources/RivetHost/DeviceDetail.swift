import SwiftUI

// Detail sheet: payback math, the milestone ladder, the cost curve, and
// destructive actions — themed with the Payback design language.

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
                DeviceIconBadge(icon: device.icon, category: device.category)
                VStack(alignment: .leading, spacing: 2) {
                    Text(device.name).font(.title2.bold())
                    Text("\(Money.major(device.priceMinor, code: device.currency)) · \(device.purchaseDate)")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                if device.computed.paidBack {
                    HStack(spacing: 4) {
                        Text("🎉")
                        Text(L10n.t(.paidBack)).bold()
                    }
                    .font(.callout)
                    .foregroundStyle(PaybackTheme.earn)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .background(PaybackTheme.earnSoft)
                    .clipShape(Capsule())
                }
            }

            HStack(spacing: 10) {
                StatTile(label: L10n.t(.dailyCost),
                         value: Money.perDay(device.computed.costPerDayMinor, code: device.currency),
                         tint: device.computed.paidBack ? PaybackTheme.earn : nil)
                StatTile(label: L10n.t(.heldFor),
                         value: "\(device.computed.daysHeld) \(L10n.t(.days))")
                if device.computed.willingSet {
                    StatTile(label: L10n.t(.willingPerDay),
                             value: Money.major(Double(device.willingPerDayMinor ?? 0),
                                                code: device.currency))
                }
            }

            CostCurve(priceMinor: device.priceMinor,
                      daysHeld: Int(device.computed.daysHeld),
                      currency: currency)
                .padding(14)
                .background(
                    RoundedRectangle(cornerRadius: PaybackTheme.cardCorner, style: .continuous)
                        .fill(PaybackTheme.card)
                )

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
        .frame(width: 480)
        .background(PaybackTheme.paper)
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
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    HStack(spacing: 10) {
                        PaybackRingView(progress: computed.paybackProgress ?? 0,
                                        paidBack: computed.paidBack,
                                        size: 38)
                        Text(L10n.t(.paybackProgress))
                            .font(.callout.weight(.medium))
                    }
                    Spacer()
                    Text(computed.paidBack
                         ? L10n.t(.paidBack)
                         : String(format: "%.0f%%", (computed.paybackProgress ?? 0) * 100))
                        .font(.callout.bold())
                        .monospacedDigit()
                        .foregroundStyle(computed.paidBack ? PaybackTheme.earn : PaybackTheme.accent)
                }
                ProgressView(value: min(computed.paybackProgress ?? 0, 1))
                    .tint(computed.paidBack ? PaybackTheme.earn : PaybackTheme.accent)

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
            .background(
                RoundedRectangle(cornerRadius: PaybackTheme.cardCorner, style: .continuous)
                    .fill(computed.paidBack ? PaybackTheme.earnSoft : PaybackTheme.card)
            )
        } else {
            HStack(spacing: 10) {
                Text("💡")
                Text(L10n.t(.willingHint))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(
                RoundedRectangle(cornerRadius: PaybackTheme.cardCorner, style: .continuous)
                    .fill(PaybackTheme.accentSoft)
            )
        }
    }

    private var milestoneLadder: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(L10n.t(.milestones))
                .font(.callout.bold())
            ForEach(device.computed.milestones) { milestone in
                HStack(spacing: 10) {
                    Text(MilestoneBadge.emoji(for: milestone.key))
                        .font(.callout)
                        .opacity(milestone.achieved ? 1 : 0.3)
                    Text(MilestoneBadge.label(for: milestone.key))
                        .font(.callout)
                        .foregroundStyle(milestone.achieved ? .primary : .secondary)
                    Spacer()
                    if milestone.achieved {
                        Image(systemName: "checkmark.circle.fill")
                            .foregroundStyle(PaybackTheme.earn)
                    }
                }
            }
        }
        .padding(14)
        .background(
            RoundedRectangle(cornerRadius: PaybackTheme.cardCorner, style: .continuous)
                .fill(PaybackTheme.card)
        )
    }
}

// The daily-cost curve: price/day for every day since purchase, with a soft
// gradient fill. It only ever goes down — that is the whole product.
struct CostCurve: View {
    let priceMinor: Int64
    let daysHeld: Int
    let currency: String

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(L10n.t(.dailyCost))
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.secondary)
                Spacer()
                Text(Money.perDay(Double(priceMinor) / Double(max(daysHeld, 1)),
                                  code: currency))
                    .font(.caption.bold())
                    .monospacedDigit()
                    .foregroundStyle(PaybackTheme.accent)
            }
            GeometryReader { geometry in
                ZStack {
                    curveFill(width: geometry.size.width, height: geometry.size.height)
                    curveLine(width: geometry.size.width, height: geometry.size.height)
                    if let last = curvePoints(width: geometry.size.width,
                                              height: geometry.size.height).last {
                        Circle()
                            .fill(PaybackTheme.accent)
                            .frame(width: 7, height: 7)
                            .position(last)
                    }
                }
            }
            .frame(height: 64)
            HStack {
                Text(L10n.t(.curveDayLabel)
                    .replacingOccurrences(of: "{day}", with: "1"))
                Spacer()
                Text(L10n.t(.curveDayLabel)
                    .replacingOccurrences(of: "{day}", with: "\(daysHeld)"))
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

    private func curvePath(width: CGFloat, height: CGFloat) -> Path {
        var path = Path()
        let points = curvePoints(width: width, height: height)
        for (index, point) in points.enumerated() {
            if index == 0 {
                path.move(to: point)
            } else {
                path.addLine(to: point)
            }
        }
        return path
    }

    private func curveLine(width: CGFloat, height: CGFloat) -> some View {
        curvePath(width: width, height: height)
            .stroke(
                LinearGradient(colors: [PaybackTheme.accent.opacity(0.5), PaybackTheme.accent],
                               startPoint: .leading, endPoint: .trailing),
                style: StrokeStyle(lineWidth: 2.5, lineCap: .round)
            )
    }

    private func curveFill(width: CGFloat, height: CGFloat) -> some View {
        var path = curvePath(width: width, height: height)
        path.addLine(to: CGPoint(x: width, y: height))
        path.addLine(to: CGPoint(x: 0, y: height))
        path.closeSubpath()
        return path
            .fill(
                LinearGradient(colors: [PaybackTheme.accent.opacity(0.22),
                                        PaybackTheme.accent.opacity(0.02)],
                               startPoint: .top, endPoint: .bottom)
            )
    }
}
