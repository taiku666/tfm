#!/bin/sh
# Runs the GUI test binary ($1) under GTK's headless broadway backend, with
# $HOME and every XDG dir in a throwaway directory, so the tests never
# touch the user's desktop, config, trash or a running tfm-gui. Skips
# (exit 0) when gtk4-broadwayd isn't installed, unless TFM_REQUIRE_GUI_TESTS
# is set (CI), so a missing package can't turn the tests off silently.
set -eu

test_bin=$1

# For an ASan build: GTK and fontconfig keep process-lifetime allocations
# that LeakSanitizer would report by the thousand.
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}

broadwayd=$(command -v gtk4-broadwayd || true)
if [ -z "$broadwayd" ]; then
    if [ -n "${TFM_REQUIRE_GUI_TESTS:-}" ]; then
        echo "gtk4-broadwayd not installed, but TFM_REQUIRE_GUI_TESTS is set."
        exit 1
    fi
    echo "gtk4-broadwayd not installed, skipping GUI tests."
    exit 0
fi

root=$(mktemp -d /tmp/tfm-gui-test-XXXXXX)
bwd_pid=
cleanup() {
    if [ -n "$bwd_pid" ]; then
        kill "$bwd_pid" 2>/dev/null || true
        wait "$bwd_pid" 2>/dev/null || true
    fi
    chmod -R u+rwx "$root" 2>/dev/null || true
    rm -rf "$root"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

mkdir -p "$root/home" "$root/runtime"
chmod 700 "$root/runtime"

# exec, and called in a subshell: then $! below is broadwayd's own PID,
# not a wrapper shell's that would leave it running when killed.
run_isolated() {
    exec env -i \
        PATH="$PATH" \
        HOME="$root/home" \
        XDG_CONFIG_HOME="$root/home/.config" \
        XDG_DATA_HOME="$root/home/.local/share" \
        XDG_STATE_HOME="$root/home/.local/state" \
        XDG_CACHE_HOME="$root/home/.cache" \
        XDG_RUNTIME_DIR="$root/runtime" \
        GSETTINGS_BACKEND=memory \
        GTK_A11Y=none \
        NO_AT_BRIDGE=1 \
        ${ASAN_OPTIONS:+ASAN_OPTIONS="$ASAN_OPTIONS"} \
        ${UBSAN_OPTIONS:+UBSAN_OPTIONS="$UBSAN_OPTIONS"} \
        "$@"
}

# broadwayd also serves the display over HTTP on port 8080+display: bound
# to localhost only, and on the next display if that port is taken.
start_broadwayd() {
    (run_isolated "$broadwayd" --address 127.0.0.1 ":$display") >"$root/broadwayd.log" 2>&1 &
    bwd_pid=$!
    i=0
    while [ ! -S "$root/runtime/broadway$((display + 1)).socket" ]; do
        if ! kill -0 "$bwd_pid" 2>/dev/null || [ "$i" -gt 100 ]; then
            kill "$bwd_pid" 2>/dev/null || true
            bwd_pid=
            return 1
        fi
        i=$((i + 1))
        sleep 0.05
    done
}

display=$(( $$ % 50 + 40 ))
tries=0
until start_broadwayd; do
    tries=$((tries + 1))
    if [ "$tries" -ge 10 ]; then
        echo "broadwayd failed to start:"
        cat "$root/broadwayd.log"
        exit 1
    fi
    display=$((display + 1))
done

(run_isolated GDK_BACKEND=broadway BROADWAY_DISPLAY=":$display" "$test_bin")
