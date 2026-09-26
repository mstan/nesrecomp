#!/usr/bin/env bash
#
# rb_sweep.sh — run the rollback matrix unattended and print one table.
#
# rb_loopback.sh answers "is this configuration sound?". This answers "which
# configurations have we ever actually tried?" -- latency, loss, ring depth,
# tip runway, a disconnect, three and four seats -- over loopback. Ported from
# n64lle tools/rb_sweep.sh (from snesrecomp's); the two-seat cells are the same
# so the engines' tables compare, and the NES adds the four-seat co-op cells
# the product needs.
#
#   tools/rb_sweep.sh <exe> <rom> [seconds-per-cell] [-- <args for every peer>]
#
#   RB_SWEEP_OUT=dir            logs (default ./rb_sweep)
#   RB_SWEEP_SESSION2=text      session config for the 2-seat cells (default: 2P co-op)
#   RB_SWEEP_PROBE_SCRIPT=path  an input script the pre-flight probe also runs
#                               (with RB_SWEEP_PROBE_ARGS, e.g. "--coop 4"), so
#                               the probe covers gameplay, not only the attract
#
# It takes a lock ONCE for the whole grid ($XDG_RUNTIME_DIR/nesrecomp-emu.lock):
# every run in it is one instance (or one set of peers) at a time.
# RB_SWEEP_NO_LOCK=1 when the caller already holds it.
#
# Exit status is the verdict over the whole grid: 0 = every gating cell and
# every pre-flight check passed.
set -u

if [ -z "${RB_SWEEP_NO_LOCK:-}" ]; then
    exec env RB_SWEEP_NO_LOCK=1 flock "${XDG_RUNTIME_DIR:-/tmp}/nesrecomp-emu.lock" "$0" "$@"
fi

EXE="${1:?usage: rb_sweep.sh <exe> <rom> [seconds-per-cell] [-- args]}"
ROM="${2:?usage: rb_sweep.sh <exe> <rom> [seconds-per-cell] [-- args]}"
shift 2
SECS=45
[ $# -gt 0 ] && [ "$1" != "--" ] && { SECS="$1"; shift; }
[ $# -gt 0 ] && [ "$1" = "--" ] && shift
EXTRA=("$@")
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="${RB_SWEEP_OUT:-$PWD/rb_sweep}"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
S2="${RB_SWEEP_SESSION2:-nes-session/1;coop=2:player;widescreen=0;}"
S3='nes-session/1;coop=3:player;widescreen=0;'
S4='nes-session/1;coop=4:player;widescreen=0;'

fails=0
pre_fails=0
cells=0

# ── pre-flight ────────────────────────────────────────────────────────────
# 1. The determinism probe (src/rollback/nes_rb_probe.c) through the same
#    snapshot/load/replay path an episode uses, with the rollback continuation
#    restart active: 0 divergences and 0 symmetry failures, else FAIL.
# 2. Digest transparency: a run that serializes + digests the snapshot domain
#    at every tick (NES_RB_PROBE=digest) must produce the same frame hashes as
#    the plain run.
single() { # name env... -- args
    local name=$1; shift
    local envs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    ( cd "$(dirname "$EXE")" && timeout 900 env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        "${envs[@]}" "$EXE" "$ROM" "$@" ) >"$OUT/pre_$name.log" 2>&1
}
probe_check() { # name
    local pl pn pd ps
    pl=$(grep -m1 '^RB_PROBE_SUMMARY' "$OUT/pre_$1.log")
    pn=$(echo "$pl" | grep -oE 'probes=[0-9]+' | cut -d= -f2)
    pd=$(echo "$pl" | grep -oE 'diverged=[0-9]+' | cut -d= -f2)
    ps=$(echo "$pl" | grep -oE 'symmetry_fail=[0-9]+' | cut -d= -f2)
    if [ -n "$pn" ] && [ "${pn:-0}" -gt 0 ] && [ "${pd:-1}" -eq 0 ] && [ "${ps:-1}" -eq 0 ]; then
        printf '  %-24s PASS  (%s probes, 0 diverged, 0 symmetry failures)\n' "probe $1" "$pn"
    else
        printf '  %-24s FAIL  (%s -- %s)\n' "probe $1" "${pl:-no RB_PROBE_SUMMARY}" "$OUT/pre_$1.log"
        pre_fails=$((pre_fails+1))
    fi
}
echo "pre-flight"
single attract NES_RB_PROBE=60:45:30:15 -- --smoke 3200 --smoke-interval 100000 "${EXTRA[@]}"
probe_check attract
if [ -n "${RB_SWEEP_PROBE_SCRIPT:-}" ]; then
    # shellcheck disable=SC2086
    single gameplay NES_RB_PROBE=100:45:60:7 -- ${RB_SWEEP_PROBE_ARGS:-} \
        --script "$RB_SWEEP_PROBE_SCRIPT" --smoke 1000000 --smoke-interval 100000 "${EXTRA[@]}"
    probe_check gameplay
fi
single dig_off -- --smoke 1500 --smoke-interval 25 --smoke-output "$OUT/pre_dig_off.json" "${EXTRA[@]}"
single dig_on NES_RB_PROBE=digest -- --smoke 1500 --smoke-interval 25 --smoke-output "$OUT/pre_dig_on.json" "${EXTRA[@]}"
if [ -s "$OUT/pre_dig_off.json" ] && cmp -s "$OUT/pre_dig_off.json" "$OUT/pre_dig_on.json"; then
    echo "  rollback digest          PASS  (digested every tick = undigested: 60 frame hashes equal; $(grep -m1 -oE 'digest_ms_p50=[0-9.]+' "$OUT/pre_dig_on.log"))"
else
    echo "  rollback digest          FAIL  (frame hashes differ: $OUT/pre_dig_off.json vs pre_dig_on.json)"
    pre_fails=$((pre_fails+1))
fi
echo

# name | env assignments | forced-mispredict interval (default 45)
grid=(
  "baseline                |RB_LOOPBACK_SESSION=$S2"
  "rtt 60ms                |RB_LOOPBACK_SESSION=$S2 RNET_SIM_LATENCY_MS=30 RNET_SIM_JITTER_MS=8"
  "rtt 200ms               |RB_LOOPBACK_SESSION=$S2 RNET_SIM_LATENCY_MS=100 RNET_SIM_JITTER_MS=25"
  "rtt 300ms               |RB_LOOPBACK_SESSION=$S2 RNET_SIM_LATENCY_MS=150 RNET_SIM_JITTER_MS=40"
  "loss 2%, fast link      |RB_LOOPBACK_SESSION=$S2 RNET_SIM_LOSS_PCT=2"
  "loss 5%, fast link      |RB_LOOPBACK_SESSION=$S2 RNET_SIM_LOSS_PCT=5"
  "loss 2% + rtt 200ms     |RB_LOOPBACK_SESSION=$S2 RNET_SIM_LATENCY_MS=100 RNET_SIM_JITTER_MS=25 RNET_SIM_LOSS_PCT=2"
  "min ring (depth 16)     |RB_LOOPBACK_SESSION=$S2 NES_RB_SNAP_DEPTH=16 RNET_SIM_LATENCY_MS=100"
  "deep ring (depth 240)   |RB_LOOPBACK_SESSION=$S2 NES_RB_SNAP_DEPTH=240 RNET_SIM_LATENCY_MS=100"
  "runway 4 (below rtt)    |RB_LOOPBACK_SESSION=$S2 NES_RB_TIP_RUNWAY=4 RNET_SIM_LATENCY_MS=100"
  "runway 24 (above rtt)   |RB_LOOPBACK_SESSION=$S2 NES_RB_TIP_RUNWAY=24 RNET_SIM_LATENCY_MS=100"
  "disconnect mid-match    |RB_LOOPBACK_SESSION=$S2 RB_LOOPBACK_KILL_AT=20 RNET_SIM_LATENCY_MS=30"
  # Organic only: no injector, every seat's scripted pad, one-way latency
  # above D (8 ticks = 133 ms), so the peers mispredict each other's real
  # input edges and the scene reads them.
  "organic only, rtt 300ms |RB_LOOPBACK_SESSION=$S2 RNET_SIM_LATENCY_MS=150 RNET_SIM_JITTER_MS=40|0"
  # NES: the product -- three and four seats of simultaneous co-op.
  "3 seats co-op           |RB_LOOPBACK_SEATS=3 RB_LOOPBACK_SESSION=$S3"
  "4 seats co-op           |RB_LOOPBACK_SEATS=4 RB_LOOPBACK_SESSION=$S4"
  "4 seats rtt 200ms       |RB_LOOPBACK_SEATS=4 RB_LOOPBACK_SESSION=$S4 RNET_SIM_LATENCY_MS=100 RNET_SIM_JITTER_MS=25"
  "4 seats loss 2%         |RB_LOOPBACK_SEATS=4 RB_LOOPBACK_SESSION=$S4 RNET_SIM_LOSS_PCT=2"
  "4 seats organic rtt 300 |RB_LOOPBACK_SEATS=4 RB_LOOPBACK_SESSION=$S4 RNET_SIM_LATENCY_MS=150 RNET_SIM_JITTER_MS=40|0"
  # Non-gating, as on snesrecomp and n64lle: an injected edge every 6 ticks is
  # far past anything real play produces.
  "STRESS tip-extend       |RB_LOOPBACK_SESSION=$S2 NES_RB_TIP_RUNWAY=24 RNET_SIM_LATENCY_MS=100 RNET_SIM_JITTER_MS=25|6"
)

printf '%-24s %-9s %2s %5s %4s %4s %4s %3s %4s %5s %6s %10s %4s %5s %11s %11s\n' \
  cell verdict rc Ep Res Ab NACK WD Ext Stall runway 'tiphold' unop Chg 'live p50/99' 'repl p50/99'
printf '%-24s %-9s %2s %5s %4s %4s %4s %3s %4s %5s %6s %10s %4s %5s %11s %11s\n' \
  '' '' '' '' '' '' '' '' '' '' '' 'n/mean' '' '' 'ms' 'ms'
printf '%.0s─' {1..142}; echo

for row in "${grid[@]}"; do
    name="${row%%|*}"; name="${name%"${name##*[![:space:]]}"}"
    rest="${row#*|}"
    envs="${rest%%|*}"
    mis="${rest#*|}"; [ "$mis" = "$rest" ] && mis=45
    [ -n "$mis" ] || mis=45
    [ "$mis" = 0 ] && mis=0
    slug=$(echo "$name" | tr -c 'a-zA-Z0-9' '_')
    cells=$((cells + 1))
    # shellcheck disable=SC2086
    out=$(env $envs RB_LOOPBACK_OUT="$OUT/$slug" \
          bash "$HERE/rb_loopback.sh" "$EXE" "$ROM" "$SECS" "$mis" -- "${EXTRA[@]}" 2>&1)
    rc=$?
    echo "$out" > "$OUT/$slug.verdict.txt"
    verdict=$(echo "$out" | grep -oE '^(PASS|FAIL)[^:]*' | head -1)
    [ -n "$verdict" ] || verdict="NO-RUN"
    case "$name" in
        STRESS*) [ "$rc" -eq 0 ] || verdict="FLAKY" ;;
        *)       [ "$rc" -eq 0 ] || fails=$((fails + 1)) ;;
    esac

    logs="$OUT/$slug"/*.log
    ep=$(cat $logs 2>/dev/null | grep -c 'RESIM episode' || true)
    ab=$(cat $logs 2>/dev/null | grep -c 'RB abort' || true)
    nk=$(cat $logs 2>/dev/null | grep -c 'RB follow refused' || true)
    wd=$(cat $logs 2>/dev/null | grep -c 'timed out waiting' || true)
    ex=$(cat $logs 2>/dev/null | grep -c 'RB tip-extend epoch' || true)
    st=$(cat $logs 2>/dev/null | grep -c 'RB chain stall' || true)
    res=$(echo "$out" | grep -m1 '^ledger' | grep -oE 'residual=-?[0-9]+' | cut -d= -f2)
    rw=$(grep -h -m1 'ROLLBACK start' "$OUT/$slug"/initiator.log "$OUT/$slug"/follower.log \
         2>/dev/null | grep -oE 'tip_runway=[0-9]+' | cut -d= -f2 | sort -u | paste -sd/)
    thn=$(cat $logs 2>/dev/null | grep -c 'tip-hold for' || true)
    thm=$(cat $logs 2>/dev/null | sed -n 's/.*RB tip-hold ended .* held=\([0-9]*\) ticks.*/\1/p' \
          | awk '{s+=$1; n++} END {if (n) printf "%.1f", s/n; else print "-"}')
    uo=$(cat $logs 2>/dev/null | grep -c 'RB drain: correction not opened' || true)
    # field cost, both peers pooled by mean of their p50/p99 (ms)
    chg=$(grep -h '^NETPLAY_DRIVER' $logs 2>/dev/null | grep -oE 'replays_changed=[0-9]+' \
          | cut -d= -f2 | awk '{s+=$1} END {print s+0}')
    lv=$(grep -h '^NETPLAY_FIELDS' $logs 2>/dev/null \
         | sed -n 's/.*live_us p50=\([0-9]*\) p99=\([0-9]*\).*/\1 \2/p' \
         | awk '{a+=$1; b+=$2; n++} END {if (n) printf "%.1f/%.1f", a/n/1000, b/n/1000; else print "-"}')
    rp=$(grep -h '^NETPLAY_FIELDS' $logs 2>/dev/null \
         | sed -n 's/.*replay=\([0-9]*\) replay_us p50=\([0-9]*\) p99=\([0-9]*\).*/\1 \2 \3/p' \
         | awk '$1>0 {a+=$2; b+=$3; n++} END {if (n) printf "%.1f/%.1f", a/n/1000, b/n/1000; else print "-"}')
    printf '%-24s %-9s %2s %5s %4s %4s %4s %3s %4s %5s %6s %10s %4s %5s %11s %11s\n' \
      "$name" "${verdict:0:9}" "$rc" "$ep" "${res:--}" "$ab" "$nk" "$wd" "$ex" "$st" "${rw:--}" \
      "$thn/$thm" "$uo" "$chg" "$lv" "$rp"
done

echo
echo "Ep = RESIM episode lines, both peers (an episode logs once on each side);"
echo "Res = ledger residual (initiated - followed - refused); Ab = aborts; NACK ="
echo "BEGINs refused; WD = stage watchdogs ('timed out waiting'); Ext = tip-extends;"
echo "Stall = advisory chain stalls; tiphold = entries / mean ticks held; unop ="
echo "corrections not opened while draining; Chg = replayed fields, both peers,"
echo "that ended on a different machine than their mispredicted live run (the"
echo "correction was guest-visible); field cost = mean over the two peers."
if [ "$pre_fails" -gt 0 ] && [ "$fails" -gt 0 ]; then
    echo "SWEEP FAIL: $pre_fails pre-flight check(s) and $fails of $cells cells" \
         "failed — logs under $OUT"
elif [ "$pre_fails" -gt 0 ]; then
    echo "SWEEP FAIL: $pre_fails pre-flight check(s) failed; all $cells cells" \
         "clean — logs under $OUT"
elif [ "$fails" -gt 0 ]; then
    echo "SWEEP FAIL: $fails of $cells cells failed — logs under $OUT"
else
    echo "SWEEP PASS: pre-flight clean, $cells cells, every gating cell clean"
fi
exit $(( (fails + pre_fails) > 0 ))
