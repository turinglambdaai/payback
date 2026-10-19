#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)";output="$repo/.rivet/acceptance/macos";home="$output/home-$$";data="$home/Library/Application Support/Payback"
app="$(find "$repo/dist" -type f -path '*/Contents/MacOS/*' -perm -u+x -print -quit)";driver="$output/macos-ax";mkdir -p "$output" "$data"
printf '%s\n' '{"version":1,"devices":[{"id":"acceptance-device","name":"Codex Acceptance Device","icon":"box","category":"computer","priceMinor":1299900,"currency":"CNY","purchaseDate":"2024-06-01","willingPerDayMinor":3000,"notes":"","createdAt":"2026-01-01 00:00:00"}],"settings":{"currency":"CNY","updateAutoCheck":false,"updateBaseUrl":null,"lastUpdateCheckAt":null,"rolloutBucket":null,"seenMilestones":{"acceptance-device":["days-100","days-365","paid-back"]}}}' > "$data/payback.json"
[[ -n "$app" && -x "$app" ]];swiftc "$repo/scripts/e2e/macos-ax.swift" -o "$driver" -framework AppKit -framework ApplicationServices
HOME="$home" "$app" >"$output/application.log" 2>&1 & app_pid=$!
cleanup(){ status=$?;trap - EXIT;kill -TERM "$app_pid" 2>/dev/null||true;wait "$app_pid" 2>/dev/null||true;exit "$status";};trap cleanup EXIT
"$driver" --pid "$app_pid" --output "$output";for _ in $(seq 1 200);do kill -0 "$app_pid" 2>/dev/null||break;sleep .05;done
kill -0 "$app_pid" 2>/dev/null&&{ echo 'Payback did not exit after native termination' >&2;exit 1;}||true;wait "$app_pid";trap - EXIT
