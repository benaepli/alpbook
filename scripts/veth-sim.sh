#!/usr/bin/env bash
#
# Create / tear down a veth pair for testing the real AfXdpSource against the
# avalanche simulator on a single host.
#
#   veth0 (10.0.0.1)  <--->  veth1 (10.0.0.2)
#
# avalanche sends the live multicast feed out of veth0 (--live-interface 10.0.0.1);
# alpdaq binds its AF_XDP socket to veth1. The recovery transports (Glimpse / rewind)
# stay on 127.0.0.1 so they are NOT on the AF_XDP-bound interface and therefore are
# not swallowed by the libxdp full-queue redirect.
#
# Usage:
#   sudo scripts/veth-sim.sh up      # create the pair (default)
#   sudo scripts/veth-sim.sh down    # remove the pair
#
set -euo pipefail

VETH0=veth0
VETH1=veth1
ADDR0=10.0.0.1/24
ADDR1=10.0.0.2/24

cmd="${1:-up}"

if [[ "$EUID" -ne 0 ]]; then
    echo "error: must run as root (network configuration requires it)" >&2
    exit 1
fi

case "$cmd" in
    up)
        if ip link show "$VETH0" >/dev/null 2>&1; then
            echo "$VETH0 already exists; run 'down' first to recreate." >&2
            exit 1
        fi
        ip link add "$VETH0" type veth peer name "$VETH1"
        ip addr add "$ADDR0" dev "$VETH0"
        ip addr add "$ADDR1" dev "$VETH1"
        ip link set "$VETH0" up
        ip link set "$VETH1" up
        echo "Created $VETH0 ($ADDR0) <-> $VETH1 ($ADDR1)."
        echo "  avalanche: --live-interface ${ADDR0%/*}"
        echo "  alpdaq:    interface = \"$VETH1\""
        ;;
    down)
        # Deleting one end removes the pair.
        ip link del "$VETH0" 2>/dev/null || true
        echo "Removed $VETH0 / $VETH1."
        ;;
    *)
        echo "usage: $0 [up|down]" >&2
        exit 1
        ;;
esac
