#!/usr/bin/env bash
#
# rb_lobby.sh — N processes, one lobby: create/join a room, launch a rollback
# match from it, drain, soft-return to the waiting room, and (rounds > 1)
# rematch from there -- headless, through the SAME callback table recomp-ui's
# launcher drives (runner/src/netplay/nes_host_lobby.c, NES_LOBBY_SELFTEST).
# Ported from n64lle tools/rb_lobby.sh.
#
#   tools/rb_lobby.sh <exe> <rom> <online|lan> [rounds] [ticks] [-- <args for every peer>]
#
#   online  a local recomp-net-server (RB_LOBBY_SERVER=<binary>, required): the
#           host creates a room of RB_LOBBY_SEATS seats (2..4), the guests find
#           it in the list and join, all ready, the host starts, the server
#           launches everyone onto its UDP input relay.
#   lan     a LAN / Direct IP room (recomp-ui's rnet_lan_* modules, no server):
#           two seats, the delay settled and nothing else.
#   rounds  matches to play (default 1). Every match drains at `ticks`
#           (NES_NET_MATCH_TICKS, default 1200), every peer goes back to the
#           room, the room prints what it would show (seated? last_error?),
#           and the next round readies and starts again: a REMATCH, a cold boot
#           in the same process with the fresh session id the room hands out.
#
# THE SESSION CONFIGURATION comes from the lobby, not from the environment: the
# host's OFFLINE mod selection is written to host/mods/state.toml
# (RB_LOBBY_COOP=N enables Simultaneous Co-op for N players, default = seats
# when seats > 2, else off); the room publishes the host's offer in its match
# caps and every guest applies it at launch. Each peer's "started ...
# session_config=" line says what it ran.
#
# ROLES. The lobby HOST is seat 0 (Mario, port 1, Start). The validation
# injector (NES_RB_FORCE_MISPREDICT, RB_LOBBY_MISPREDICT, default 45) runs on
# guest 1, whose corruptions land on the host's rows. Every other peer has the
# validation knobs pinned OFF as a group. Every peer plays a scripted pad
# (NES_NET_TEST_PAD=<peer index>): the host presses Start, everyone runs and
# jumps on their own period.
#
# Knobs:
#   RB_LOBBY_OUT=dir            logs, per-peer exe copies, screenshots (default ./rb_lobby)
#   RB_LOBBY_SERVER=path        recomp-net-server binary (online)
#   RB_LOBBY_SEATS=n            online seats, 2..4 (default 2; LAN is always 2)
#   RB_LOBBY_PORTS=ws,relay     the server's WebSocket and input-relay ports (18865,18877)
#   RB_LOBBY_LAN_PORT=n         the LAN room's port (17790; the guest binds n+1)
#   RB_LOBBY_MISPREDICT=n       guest 1's injector interval (0 = off)
#   RB_LOBBY_GUEST_ENV="K=V .." extra environment for guest 1 only (e.g. a forced
#                               boot fork: NES_RB_FORCE_BOOT_FORK=1)
#   RB_LOBBY_THEN_OFFLINE=1     after the last match every peer takes Offline Play
#                               in the same process (NES_RUN_FRAMES=ticks); its
#                               RUN_DONE line must equal a fresh process's own
#                               offline run of the same length, which this
#                               script then makes.
#   RB_LOBBY_SPECTATORS=n       online only: n more processes take gallery seats
#                               (the host opens the gallery); each must launch as
#                               a spectator, contribute no row, stay in sync and
#                               confirm the players' digest
#   RB_LOBBY_WALL=secs          kill everything after this long
#
# The verdict per match: every peer launched with the same session id, the
# boot digest agreed, 0 forks, every peer drained ("RB quiesced"), episodes > 0
# when the injector ran, everyone back in a room. Across matches: a fresh
# session id each time and every match's tick-0 boot parts equal to the first
# match's (a rematch that did not boot cold shows up there first). Exit 0 =
# all of that held. A forced boot fork instead expects every peer to refuse
# (boot_digest_mismatch), return to the room with last_error set, and play
# nothing.
set -u

EXE="${1:?usage: rb_lobby.sh <exe> <rom> <online|lan> [rounds] [ticks] [-- args]}"
ROM="${2:?usage}"
MODE="${3:?usage: mode is online or lan}"
shift 3
ROUNDS=1; TICKS=1200
[ $# -gt 0 ] && [ "$1" != "--" ] && { ROUNDS="$1"; shift; }
[ $# -gt 0 ] && [ "$1" != "--" ] && { TICKS="$1"; shift; }
[ $# -gt 0 ] && [ "$1" = "--" ] && shift
EXTRA=("$@")
OUT="${RB_LOBBY_OUT:-$PWD/rb_lobby}"
case "$MODE" in online|lan) ;; *) echo "rb_lobby: mode must be online or lan" >&2; exit 2;; esac
SEATS="${RB_LOBBY_SEATS:-2}"
[ "$MODE" = lan ] && SEATS=2
case "$SEATS" in 2|3|4) ;; *) echo "rb_lobby: RB_LOBBY_SEATS=$SEATS (2..4)" >&2; exit 2;; esac
[ -x "$EXE" ] || { echo "rb_lobby: $EXE is not executable" >&2; exit 2; }
[ -f "$ROM" ] || { echo "rb_lobby: no ROM at $ROM" >&2; exit 2; }
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
rm -rf "$OUT"/host "$OUT"/guest* "$OUT"/spect* "$OUT"/fresh "$OUT"/server "$OUT"/*.log "$OUT"/*.png
IFS=, read -r WS_PORT RELAY_PORT <<<"${RB_LOBBY_PORTS:-18865,18877}"
MISPREDICT="${RB_LOBBY_MISPREDICT:-45}"
WALL="${RB_LOBBY_WALL:-$(( 90 + 45 * ROUNDS + TICKS * ROUNDS / 40 ))}"
COOP="${RB_LOBBY_COOP:-$([ "$SEATS" -gt 2 ] && echo "$SEATS" || echo 0)}"

SPECT="${RB_LOBBY_SPECTATORS:-0}"
[ "$MODE" = lan ] && SPECT=0
ROLES=(host)
for ((i = 1; i < SEATS; i++)); do ROLES+=("guest$i"); done
for ((i = 1; i <= SPECT; i++)); do ROLES+=("spect$i"); done
for role in "${ROLES[@]}"; do
    mkdir -p "$OUT/$role"
    cp "$EXE" "$OUT/$role/"
    [ -d "$(dirname "$EXE")/mods" ] && cp -r "$(dirname "$EXE")/mods" "$OUT/$role/"
    [ -d "$(dirname "$EXE")/assets" ] && cp -r "$(dirname "$EXE")/assets" "$OUT/$role/"
    rm -f "$OUT/$role/mods/state.toml"
done
if [ "$COOP" -ge 2 ] 2>/dev/null; then
    cat > "$OUT/host/mods/state.toml" <<EOF
format_version=1
[[package]]
id="super-mario-bros.gameplay.simultaneous-coop"
version="1.0.0"
[[feature]]
package_id="super-mario-bros.gameplay.simultaneous-coop"
id="coop"
enabled=true
[feature.values]
players="$COOP"
pause="player"
EOF
fi

server_pid=""
common=(SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
        NES_LOBBY_SELFTEST_ROUNDS="$ROUNDS"
        NES_LOBBY_SELFTEST_LOBBY="nes-rb-lobby-$$"
        NES_LOBBY_SELFTEST_SEATS="$SEATS"
        NES_NET_MATCH_TICKS="$TICKS" NES_NET_SHOT_TICK="$(( TICKS * 3 / 4 ))")
[ "${RB_LOBBY_THEN_OFFLINE:-0}" = 1 ] && common+=(NES_LOBBY_SELFTEST_THEN_OFFLINE=1 NES_RUN_FRAMES="$TICKS")
if [ "$MODE" = online ]; then
    SERVER="${RB_LOBBY_SERVER:?rb_lobby: online needs RB_LOBBY_SERVER=<recomp-net-server binary>}"
    mkdir -p "$OUT/server"
    (cd "$OUT/server" && exec env BIND_ADDR=127.0.0.1:$WS_PORT \
        INPUT_RELAY_BIND=127.0.0.1:$RELAY_PORT INPUT_RELAY_ADVERTISE_HOST=127.0.0.1 \
        INPUT_RELAY_ALLOW_LOOPBACK=1 RUST_LOG=info "$SERVER") >"$OUT/server.log" 2>&1 &
    server_pid=$!
    for _ in $(seq 1 100); do
        (exec 3<>/dev/tcp/127.0.0.1/$WS_PORT) 2>/dev/null && break
        sleep 0.1
    done
    sleep 0.3
    kill -0 "$server_pid" 2>/dev/null || { echo "rb_lobby: the server exited (port $WS_PORT in use?) — $OUT/server.log" >&2; exit 2; }
    common+=(RNET_LOBBY_URL=ws://127.0.0.1:$WS_PORT)
else
    common+=(NES_LOBBY_SELFTEST_LAN=1 NES_LOBBY_SELFTEST_LAN_PORT="${RB_LOBBY_LAN_PORT:-17790}")
fi

off=(NES_RB_FORCE_MISPREDICT=0 NES_RB_FORCE_FORK=0 NES_RB_FORCE_BOOT_FORK=0
     NES_RB_FORCE_MOD_MISMATCH=0 NES_RB_FORCE_MODSET=)
read -r -a guest_env <<<"${RB_LOBBY_GUEST_ENV:-}"
declare -A PID
for ((i = 0; i < SEATS + SPECT; i++)); do
    role=${ROLES[$i]}
    if [ "$i" -eq 1 ]; then
        knobs=(NES_RB_FORCE_MISPREDICT="$MISPREDICT" "${guest_env[@]}")
    else
        knobs=("${off[@]}")
    fi
    [ "$i" -eq 0 ] && [ "$SPECT" -gt 0 ] && knobs+=(NES_LOBBY_SELFTEST_ALLOW_SPECTATORS=1 NES_LOBBY_SELFTEST_EXPECT_SPECTATORS="$SPECT")
    [ "$i" -ge "$SEATS" ] && knobs+=(NES_LOBBY_SELFTEST_SPECTATE=1)
    sel=guest; [ "$i" -eq 0 ] && sel=host
    (cd "$OUT/$role" && exec env "${common[@]}" "${knobs[@]}" \
        NES_LOBBY_SELFTEST="$sel" NES_LOBBY_SELFTEST_NAME="$role" \
        NES_NET_TEST_PAD="$i" NES_NET_SHOT_PATH="$OUT/$role.png" \
        NES_NET_SHOT_STATE="$OUT/$role.shot.state" \
        NES_RUN_SCREENSHOT="$OUT/$role.offline.png" \
        "./$(basename "$EXE")" "$ROM" "${EXTRA[@]}") >"$OUT/$role.log" 2>&1 &
    PID[$role]=$!
    [ "$i" -eq 0 ] && sleep 1   # the host's room exists before anyone looks for it
    # Spectators come in after the players have their seats, so the gallery
    # is theirs and not a race for seat numbers.
    [ "$i" -eq $((SEATS - 1)) ] && [ "$SPECT" -gt 0 ] && sleep 4
done

t0=$SECONDS
while :; do
    alive=0
    for r in "${ROLES[@]}"; do kill -0 "${PID[$r]}" 2>/dev/null && alive=1; done
    [ "$alive" -eq 0 ] && break
    if [ $((SECONDS - t0)) -ge "$WALL" ]; then
        echo "rb_lobby: wall clock ($WALL s) — killing" >&2
        for r in "${ROLES[@]}"; do kill -9 "${PID[$r]}" 2>/dev/null; done
        break
    fi
    sleep 0.5
done
for r in "${ROLES[@]}"; do wait "${PID[$r]}" 2>/dev/null; done
[ -n "$server_pid" ] && { kill "$server_pid" 2>/dev/null; wait "$server_pid" 2>/dev/null; }

count() { grep -c "$2" "$OUT/$1.log" 2>/dev/null || true; }
rc=0
forced_fork=0
[[ " ${RB_LOBBY_GUEST_ENV:-} " == *NES_RB_FORCE_BOOT_FORK=1* ]] && forced_fork=1
echo "mode=$MODE seats=$SEATS spectators=$SPECT rounds=$ROUNDS ticks=$TICKS coop=$COOP"
for r in "${ROLES[@]}"; do
    sids=$(grep -oE 'fill_launch=1 slot=[0-9]+ players=[0-9]+ session=[0-9]+' "$OUT/$r.log" | grep -oE 'session=[0-9]+' | cut -d= -f2 | paste -sd,)
    slots=$(grep -oE 'fill_launch=1 slot=[0-9]+' "$OUT/$r.log" | grep -oE '[0-9]+$' | sort -u | paste -sd,)
    cfgs=$(grep -oE 'session_config="[^"]*"' "$OUT/$r.log" | sort -u | paste -sd' ')
    printf '%-7s slot=%-3s sessions=%-12s episodes=%-4s forks=%s agreed=%s refused=%s quiesced=%s back=%s %s\n' \
        "$r" "${slots:-?}" "${sids:-none}" "$(count "$r" 'RESIM episode')" "$(count "$r" 'FORK')" \
        "$(count "$r" 'RB boot digest agreed')" "$(count "$r" 'BOOT DIGEST MISMATCH')" \
        "$(count "$r" 'RB quiesced at')" "$(count "$r" 'back in the room: in_lobby=1')" "$cfgs"
    grep -h '\[lobby-selftest\].*back in the room' "$OUT/$r.log" | sed "s/^/          /"
done
# every peer: same session id per round, a fresh one each round
ref=$(grep -oE 'fill_launch=1 slot=[0-9]+ players=[0-9]+ session=[0-9]+' "$OUT/host.log" | grep -oE 'session=[0-9]+' | cut -d= -f2 | paste -sd,)
for r in "${ROLES[@]}"; do
    s=$(grep -oE 'fill_launch=1 slot=[0-9]+ players=[0-9]+ session=[0-9]+' "$OUT/$r.log" | grep -oE 'session=[0-9]+' | cut -d= -f2 | paste -sd,)
    [ "$s" = "$ref" ] || { echo "FAIL: $r launched sessions [$s], host [$ref]"; rc=1; }
done
nsid=$(echo "$ref" | tr ',' '\n' | grep -c .)
usid=$(echo "$ref" | tr ',' '\n' | sort -u | grep -c .)
[ "$nsid" -eq "$ROUNDS" ] || { echo "FAIL: host launched $nsid of $ROUNDS matches"; rc=1; }
[ "$usid" -eq "$nsid" ] || { echo "FAIL: a session id was reused across matches ($ref)"; rc=1; }
# boot parts: every match boots the same machine
bp=$(grep -h 'RB boot parts' "$OUT"/*.log | sed 's/.*RB boot parts //' | sort -u | grep -c .)
if [ "$forced_fork" -eq 1 ]; then
    for r in "${ROLES[@]}"; do
        [ "$(count "$r" 'BOOT DIGEST MISMATCH\|boot_digest_mismatch')" -gt 0 ] || { echo "FAIL: $r did not refuse the forked boot"; rc=1; }
        grep -q 'last_error="boot_digest_mismatch"' "$OUT/$r.log" || { echo "FAIL: $r's room shows no last_error=boot_digest_mismatch"; rc=1; }
        [ "$(count "$r" 'RESIM episode')" -eq 0 ] || { echo "FAIL: $r played a refused match"; rc=1; }
    done
    [ "$rc" -eq 0 ] && echo "PASS (refused): every peer refused the forked boot and soft-returned with last_error=boot_digest_mismatch"
else
    [ "$bp" -eq 1 ] || { echo "FAIL: $bp distinct tick-0 boot parts across peers/matches (a rematch that did not boot cold)"; rc=1; }
    for r in "${ROLES[@]}"; do
        [ "$(count "$r" 'FORK')" -eq 0 ] || { echo "FAIL: $r forked"; rc=1; }
        [ "$(count "$r" 'RB quiesced at')" -eq "$ROUNDS" ] || { echo "FAIL: $r drained $(count "$r" 'RB quiesced at') of $ROUNDS matches"; rc=1; }
        [ "$(count "$r" 'back in the room: in_lobby=1')" -eq "$ROUNDS" ] || { echo "FAIL: $r was not back in a room after every match"; rc=1; }
        if [[ "$r" == spect* ]]; then
            grep -q 'started transport=[a-z-]* slot=[0-9]* slots=[0-9]* spectator=1' "$OUT/$r.log" \
                || { echo "FAIL: $r did not launch as a spectator"; rc=1; }
        elif [ "$MISPREDICT" != 0 ]; then
            [ "$(count "$r" 'RESIM episode')" -gt 0 ] || { echo "FAIL: $r ran no episode"; rc=1; }
        fi
    done
    # confirmed frame at 3/4 of every match: equal across peers (last match)
    shot=$(( TICKS * 3 / 4 ))
    dd=$(for r in "${ROLES[@]}"; do grep "NET_TICK_DIGEST tick=$shot " "$OUT/$r.log" | tail -1 | grep -oE 'master=[0-9a-f]+'; done | sort -u | grep -c .)
    [ "$dd" -eq 1 ] || { echo "FAIL: peers confirmed $dd different states at tick $shot"; rc=1; }
    echo "confirmed tick $shot: $(for r in "${ROLES[@]}"; do printf '%s=%s ' "$r" "$(grep "NET_TICK_DIGEST tick=$shot " "$OUT/$r.log" | tail -1 | grep -oE 'master=[0-9a-f]+' | cut -d= -f2)"; done)"
fi
if [ "${RB_LOBBY_THEN_OFFLINE:-0}" = 1 ] && [ "$forced_fork" -eq 0 ]; then
    # A fresh process's own offline run of the same length, with the HOST's
    # offline selection (its mods/state.toml), must end on the same machine.
    mkdir -p "$OUT/fresh"
    cp "$EXE" "$OUT/fresh/"; cp -r "$OUT/host/mods" "$OUT/fresh/" 2>/dev/null
    (cd "$OUT/fresh" && env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NES_RUN_FRAMES="$TICKS" \
        NES_RUN_SCREENSHOT="$OUT/fresh.offline.png" \
        "./$(basename "$EXE")" "$ROM" "${EXTRA[@]}") >"$OUT/fresh.log" 2>&1
    fresh=$(grep -m1 '^RUN_DONE' "$OUT/fresh.log")
    after=$(grep -m1 '^RUN_DONE' "$OUT/host.log")
    echo "offline after rematch: ${after:-none}"
    echo "fresh process:         ${fresh:-none}"
    [ -n "$fresh" ] && [ "$fresh" = "$after" ] || { echo "FAIL: offline Play after the matches is not a fresh process's machine"; rc=1; }
fi
[ "$rc" -eq 0 ] && [ "$forced_fork" -eq 0 ] && echo "PASS: $ROUNDS match(es), $SEATS seats, same fresh session per match, cold boots, 0 forks, all drained, all back in a room"
echo "logs: $OUT"
exit $rc
