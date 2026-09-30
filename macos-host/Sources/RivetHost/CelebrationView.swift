import SwiftUI

// Milestone celebration: a confetti burst over the newly-achieved list.
// This is the payoff moment of the whole product — earned, not staged.

struct CelebrationView: View {
    let hits: [MilestoneHit]
    let onContinue: () -> Void

    var body: some View {
        VStack(spacing: 18) {
            ZStack {
                ConfettiBurst().frame(height: 150)
                VStack(spacing: 6) {
                    ForEach(hits.prefix(3)) { hit in
                        Text(MilestoneBadge.emoji(for: hit.key))
                            .font(.system(size: 40))
                    }
                }
            }
            Text(L10n.t(.celebrateTitle))
                .font(.title3.bold())

            VStack(spacing: 8) {
                ForEach(hits) { hit in
                    HStack(spacing: 8) {
                        Text(hit.icon)
                        Text(hit.deviceName).font(.callout.weight(.medium))
                        Text(MilestoneBadge.label(for: hit.key))
                            .font(.callout)
                            .foregroundStyle(PaybackTheme.accent)
                    }
                }
                if hits.count > 3 {
                    Text("+" + String(hits.count - 3))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Text(L10n.t(.quip2))
                .font(.caption)
                .foregroundStyle(.secondary)

            Button(L10n.t(.celebrateKeep), action: onContinue)
                .buttonStyle(.borderedProminent)
                .tint(PaybackTheme.accent)
                .keyboardShortcut(.defaultAction)
        }
        .padding(28)
        .frame(width: 380)
    }
}

// A lightweight one-shot confetti: ~90 particles falling and fading over
// ~2.5 seconds, seeded per presentation.
struct ConfettiBurst: View {
    var body: some View {
        TimelineView(.animation) { timeline in
            Canvas { context, size in
                let t = timeline.date.timeIntervalSinceReferenceDate
                    .truncatingRemainder(dividingBy: 2.5)
                var rng = SeededRandom(seed: 42)
                let colors: [Color] = [.orange, .yellow, .green, .pink,
                                       .blue, .mint, PaybackTheme.accent]
                for index in 0..<90 {
                    let seedA = rng.next()
                    let seedB = rng.next()
                    let seedC = rng.next()
                    let x = size.width * seedA
                    let fall = (t / 2.5 + seedB) * 1.0
                    let y = size.height * fall * (0.6 + seedC * 0.6)
                    let progress = min(t / 2.5, 1)
                    let alpha = Double(1 - progress)
                    guard alpha > 0 else { continue }
                    let rotation = Angle.degrees((seedC * 720 + t * 160)
                        .truncatingRemainder(dividingBy: 360))
                    var ctx = context
                    ctx.translateBy(x: x, y: y)
                    ctx.rotate(by: rotation)
                    ctx.opacity = alpha
                    let w = 4 + seedC * 5
                    let rect = CGRect(x: -w / 2, y: -w / 2, width: w,
                                      height: w * 0.6)
                    ctx.fill(Path(rect), with: .color(colors[index % colors.count]))
                }
            }
        }
        .allowsHitTesting(false)
    }
}

/// tiny deterministic PRNG so the burst looks identical for a given run
struct SeededRandom {
    private var state: UInt64
    init(seed: UInt64) { state = seed &* 2862933555777941757 &+ 3037000493 }
    mutating func next() -> Double {
        state = state &* 2862933555777941757 &+ 3037000493
        return Double(state >> 11) / Double(UInt64.max >> 11)
    }
}
