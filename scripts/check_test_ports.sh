#!/usr/bin/env bash
# CI: tests must not dial a fixed loopback port. Every test listener binds an OS-assigned port (setOsAssignedPortsForTest),
# and a test dials the port that was bound. Use port 1 for an address that must have nothing listening.
set -euo pipefail
cd "$(dirname "$0")/.."

found=$(grep -nE '(127\.0\.0\.1|localhost):[0-9]{2,}|htons\([1-9][0-9]+\)' tests/*.cpp tests/*.hpp 2>/dev/null | grep -v 'fixed-port-ok' || true)
if [ -n "$found" ]; then
    echo "Fixed ports in tests (use port 0 and dial the bound port, or mark the line // fixed-port-ok: <reason>):"
    echo "$found"
    exit 1
fi
