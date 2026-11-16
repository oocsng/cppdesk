#!/usr/bin/env bash
# CI only: starts a private PipeWire with its PulseAudio-compatible server and a null output, then runs the sound tests
# (capture, playback, prompt stopping) against it. Run inside `dbus-run-session`, with pipewire, wireplumber,
# pipewire-pulse and pulseaudio-utils installed.
set -u
export XDG_RUNTIME_DIR="$(mktemp -d)"
chmod 700 "$XDG_RUNTIME_DIR"
LOGS="$(mktemp -d)"
trap 'for f in "$LOGS"/*.log; do echo "--- $f"; tail -n 40 "$f"; done; kill $(jobs -p) 2>/dev/null' EXIT

pipewire > "$LOGS/pipewire.log" 2>&1 &
sleep 2
wireplumber > "$LOGS/wireplumber.log" 2>&1 &
sleep 3
pipewire-pulse > "$LOGS/pipewire-pulse.log" 2>&1 &
sleep 2

export PULSE_SERVER="unix:$XDG_RUNTIME_DIR/pulse/native"
pactl info > "$LOGS/pactl-info.log" 2>&1 || { echo "the PulseAudio server did not start"; exit 1; }
# Whatever the default output plays comes back from its monitor, which the default input follows.
pactl load-module module-null-sink sink_name=cdtest > "$LOGS/null-sink.log" 2>&1 || { echo "no null output"; exit 1; }
pactl set-default-sink cdtest
pactl set-default-source cdtest.monitor
pactl list short sinks sources > "$LOGS/devices.log" 2>&1
"$(dirname "$0")/../build/CppDeskTests" --pulse
