#!/usr/bin/env bash
# Shared command execution; caller supplies title/profile/package policy.
nes_configure_build() {
    local project="$1" build="$2" target="$3" jobs="$4"; shift 4
    cmake -S "$project" -B "$build" "$@"
    if [ -n "$target" ]; then
        cmake --build "$build" --target "$target" --parallel "$jobs"
    else
        cmake --build "$build" --parallel "$jobs"
    fi
}

nes_fetch_checked_tool() {
    local url="$1" sha="$2" dest="$3"
    if [ ! -f "$dest" ] || [ "$(sha256sum "$dest" | awk '{print $1}')" != "$sha" ]; then
        curl -fL --retry 3 "$url" -o "$dest.tmp"
        printf '%s  %s\n' "$sha" "$dest.tmp" | sha256sum -c - >/dev/null
        mv "$dest.tmp" "$dest"
    fi
    chmod 0755 "$dest"
}
