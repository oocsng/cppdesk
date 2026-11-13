#!/usr/bin/env bash
# CI only: starts a private PipeWire with a test video source (solid blue, 320x240) and runs the video receiver's test
# against it. Run inside `dbus-run-session`, with PipeWire, WirePlumber, GStreamer's PipeWire plugin and jq installed.
set -u
export XDG_RUNTIME_DIR="$(mktemp -d)"
chmod 700 "$XDG_RUNTIME_DIR"
LOGS="$(mktemp -d)"
trap 'for f in "$LOGS"/*.log; do echo "--- $f"; tail -n 40 "$f"; done; kill $(jobs -p) 2>/dev/null' EXIT

pipewire > "$LOGS/pipewire.log" 2>&1 &
sleep 2
wireplumber > "$LOGS/wireplumber.log" 2>&1 &
sleep 3
gst-launch-1.0 videotestsrc is-live=true pattern=blue \
  ! video/x-raw,format=BGRx,width=320,height=240,framerate=30/1 \
  ! pipewiresink stream-properties="props,media.class=Video/Source,node.name=cppdesk-test" > "$LOGS/gst.log" 2>&1 &
sleep 4

pw-dump > "$LOGS/dump.log" 2>&1
NODE="$(pw-dump | jq '[.[] | select(.info.props["node.name"] == "cppdesk-test")][0].id')"
echo "test source node: $NODE"
[ -n "$NODE" ] && [ "$NODE" != "null" ] || { echo "the test source did not appear"; exit 1; }
CPPDESK_PIPEWIRE_TEST_NODE="$NODE" "$(dirname "$0")/../build/CppDeskTests" --pipewire
