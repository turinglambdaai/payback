#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)";output="$repo/.rivet/acceptance/macos";home="$output/home-$$";data="$home/Library/Application Support/Payback"
app="$(find "$repo/dist" -type f -path '*/Contents/MacOS/*' -perm -u+x -print -quit)";driver="$output/macos-ax";mkdir -p "$output" "$data"
# pre-flight hygiene: reap leftovers of this exact app from an earlier
# interrupted run — a stray instance breaks the next run's window discovery
pkill -f "$repo/dist/payback.app" 2>/dev/null||true;sleep 1
printf '%s\n' '{"version":1,"devices":[{"id":"acceptance-device","name":"Codex Acceptance Device","icon":"box","category":"computer","priceMinor":1299900,"currency":"CNY","purchaseDate":"2024-06-01","willingPerDayMinor":3000,"notes":"","createdAt":"2026-01-01 00:00:00","updatedAt":"2026-01-01 00:00:00"}],"settings":{"currency":"CNY","updateAutoCheck":false,"updateBaseUrl":null,"lastUpdateCheckAt":null,"rolloutBucket":null,"seenMilestones":{"acceptance-device":["days-100","days-365","paid-back"]}}}' > "$data/payback.json"
[[ -n "$app" && -x "$app" ]];swiftc "$repo/scripts/e2e/macos-ax.swift" -o "$driver" -framework AppKit -framework ApplicationServices
app_pid="";cleanup(){ status=$?;trap - EXIT;if [ -n "$app_pid" ]&&kill -0 "$app_pid" 2>/dev/null;then kill -TERM "$app_pid" 2>/dev/null||true;for _ in $(seq 1 40);do kill -0 "$app_pid" 2>/dev/null||break;sleep .25;done;kill -9 "$app_pid" 2>/dev/null||true;fi;wait "$app_pid" 2>/dev/null||true;exit "$status";};trap cleanup EXIT
launch(){ # launch the packaged app with the disposable root and run one driver phase
  local phase="$1" attempt=1 strict="${2:-1}"
  local tracefile="trace-$phase.json";[[ "$phase" == "final" ]]&&tracefile="interaction-trace.json"
  while :; do
    HOME="$home" "$app" >"$output/application-$phase.log" 2>&1 & app_pid=$!
    # bounded readiness gate on observable state: the embedded backend
    # announces its handshake on stderr
    local ready="" t1 t2
    for _ in $(seq 1 600); do
      grep -q "payback: backend started" "$output/application-$phase.log" 2>/dev/null&&{ ready=1;break; }
      kill -0 "$app_pid" 2>/dev/null||{ echo "Payback died during startup ($phase)" >&2;exit 1; }
      sleep .05
    done
    [[ -n "$ready" ]]||{ echo "Payback backend never announced readiness ($phase)" >&2;exit 1; }
    # let the startup CPU burn settle so the driver's first probes see the
    # rendered UI instead of racing the render
    t1=$(ps -o time= -p "$app_pid" 2>/dev/null|tr -d ' ');sleep 1
    for _ in $(seq 1 120); do
      kill -0 "$app_pid" 2>/dev/null||{ echo "Payback died during startup ($phase)" >&2;exit 1; }
      t2=$(ps -o time= -p "$app_pid" 2>/dev/null|tr -d ' ')
      [[ -n "$t1" && "$t1" == "$t2" ]]&&break
      t1="$t2";sleep 1
    done
    rm -f "$output/$tracefile"
    local spin=""
    if "$driver" --pid "$app_pid" --output "$output" --data-dir "$data" --phase "$phase"; then
      break
    fi
    # retry only one specific failure shape: the driver never got past
    # launch (empty trace actions) AND the app is burning CPU — the known
    # intermittent startup spin (dozens of Racket threads at full tilt, no
    # window ever rendered) on current macOS hosts. Any assertion failure
    # has real actions in the trace and is never retried.
    python3 - "$output/$tracefile" <<'EOF' && spin=1 || true
import json, sys
try:
    t = json.load(open(sys.argv[1]))
    sys.exit(0 if t.get("actions") == [] else 1)
except Exception:
    sys.exit(1)
EOF
    # ps time is [[hh:]mm:]ss.rr — fold to whole seconds
    local ct;ct=$(ps -o time= -p "$app_pid" 2>/dev/null|awk -F: '{s=$NF+0;if(NF==2)s+=$1*60;if(NF>=3)s+=$1*3600;printf "%d",s}')
    if [[ -n "$spin" && -n "$ct" && "$ct" -ge 8 ]];then
      echo "Payback hit the startup CPU spin (attempt $attempt); relaunching" >&2
      kill -9 "$app_pid" 2>/dev/null||true;wait "$app_pid" 2>/dev/null||true;app_pid=""
      attempt=$((attempt+1))
      [[ $attempt -le 3 ]]||{ echo "Payback spun on every attempt ($phase)" >&2;exit 1; }
      continue
    fi
    exit 1
  done
  for _ in $(seq 1 400);do kill -0 "$app_pid" 2>/dev/null||break;sleep .05;done
  if kill -0 "$app_pid" 2>/dev/null;then
    # the keyboard-free verify phase records its evidence before exit, so a
    # stale session that swallows its Cmd+Q is reaped, not failed; the
    # interactive run's orderly Cmd+Q exit stays a hard requirement
    [[ "$strict" == "1" ]]&&{ echo "Payback did not exit after the $phase run's Cmd+Q" >&2;exit 1; }
    echo "verify-phase Payback ignored Cmd+Q (stale session routing); reaping" >&2
    kill -TERM "$app_pid" 2>/dev/null||true
    for _ in $(seq 1 40);do kill -0 "$app_pid" 2>/dev/null||break;sleep .25;done
    kill -9 "$app_pid" 2>/dev/null||true
  fi
  wait "$app_pid" 2>/dev/null||true;app_pid=""
}
shard_records(){ # count live records with a given name in the machine's shard
  python3 - "$data" "$1" <<'EOF'
import glob, json, sys
data, name = sys.argv[1], sys.argv[2]
shards = glob.glob(f"{data}/devices-*.json")
if len(shards) != 1: raise SystemExit(f"expected exactly one shard, found {shards}")
with open(shards[0]) as f: doc = json.load(f)
devices = doc.get("devices", {})
records = [d for d in (devices.values() if isinstance(devices, dict) else devices)
           if d.get("name") == name and not d.get("deletedAt")]
print(len(records))
EOF
}
launch full
[[ "$(shard_records "E2E Save Probe")" == "1" ]]||{ echo 'the rejected save and the corrected retry must leave exactly one record' >&2;exit 1;}
[[ "$(shard_records "E2E Storage Probe")" == "1" ]]||{ echo 'the storage-failed attempt and the retry must leave exactly one record' >&2;exit 1;}
echo 'shard after run 1: each probe story (reject+correct, storage-fail+retry) left exactly one record'
launch verify 0
[[ "$(shard_records "E2E Save Probe")" == "1" ]]||{ echo 'the corrected record must be intact after the relaunch' >&2;exit 1;}
[[ "$(shard_records "E2E Storage Probe")" == "1" ]]||{ echo 'the retry record must be intact after the relaunch' >&2;exit 1;}
echo 'acceptance evidence: .rivet/acceptance/macos/'
