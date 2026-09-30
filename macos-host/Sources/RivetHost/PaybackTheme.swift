import SwiftUI

// Payback design language (docs/design.md): warm paper background, ink
// typography, one Crail-orange accent for value, a single green reserved
// for "paid back". Cards are soft-rounded surfaces; numbers are the hero.

enum PaybackTheme {
    // MARK: palette (light + dark adaptive)

    static let paper = Color(nsColor: .windowBackgroundColor)
    static let card = Color(nsColor: .controlBackgroundColor)
    static let ink = Color.primary

    /// the brand orange — value, progress, primary actions
    static let accent = Color(red: 0.757, green: 0.373, blue: 0.235)   // #C15F3C
    static let accentSoft = accent.opacity(0.14)

    /// reserved exclusively for paid-back / earned states
    static let earn = Color(red: 0.220, green: 0.588, blue: 0.302)     // #38964D
    static let earnSoft = earn.opacity(0.14)

    static let hairline = Color(nsColor: .separatorColor)

    // MARK: metrics

    static let cardCorner: CGFloat = 16
    static let statCorner: CGFloat = 14
    static let cardPadding: CGFloat = 16

    // MARK: styles

    static func cardBackground(_ dark: Bool = false) -> some View {
        RoundedRectangle(cornerRadius: cardCorner, style: .continuous)
            .fill(
                LinearGradient(
                    colors: [card, card.opacity(0.92)],
                    startPoint: .top, endPoint: .bottom
                )
            )
            .shadow(color: Color.black.opacity(0.06), radius: 8, x: 0, y: 2)
    }

    /// capsule badge for milestone chips
    static func badge(text: String, emoji: String) -> some View {
        HStack(spacing: 4) {
            Text(emoji)
            Text(text).font(.caption2.weight(.medium))
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 3)
        .background(accentSoft)
        .foregroundStyle(accent)
        .clipShape(Capsule())
    }
}

// The payback progress ring with an accent gradient and a tick at 100%.
struct PaybackRingView: View {
    let progress: Double
    let paidBack: Bool
    var size: CGFloat = 46

    var body: some View {
        ZStack {
            Circle()
                .stroke(Color.secondary.opacity(0.15), lineWidth: 5)
            Circle()
                .trim(from: 0, to: min(CGFloat(progress), 1))
                .stroke(
                    AngularGradient(
                        colors: paidBack
                            ? [PaybackTheme.earn.opacity(0.55), PaybackTheme.earn]
                            : [PaybackTheme.accent.opacity(0.55), PaybackTheme.accent],
                        center: .center,
                        startAngle: .degrees(-90),
                        endAngle: .degrees(270)
                    ),
                    style: StrokeStyle(lineWidth: 5, lineCap: .round)
                )
                .rotationEffect(.degrees(-90))
            centerLabel
        }
        .frame(width: size, height: size)
    }

    @ViewBuilder
    private var centerLabel: some View {
        if paidBack {
            Image(systemName: "checkmark")
                .font(.system(size: size * 0.34, weight: .bold))
                .foregroundStyle(PaybackTheme.earn)
        } else {
            Text("\(Int((progress * 100).rounded()))")
                .font(.system(size: size * 0.28, weight: .bold, design: .rounded))
                .monospacedDigit()
        }
    }
}

/// Progress ring that animates from zero to its value when it appears.
struct AnimatedRing: View {
    let progress: Double
    let paidBack: Bool
    @State private var drawn = false

    var body: some View {
        PaybackRingView(progress: drawn ? progress : 0, paidBack: paidBack)
            .onAppear {
                withAnimation(.easeOut(duration: 0.9)) { drawn = true }
            }
    }
}
