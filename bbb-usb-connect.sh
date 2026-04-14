#!/bin/bash
#
# Host-side setup for SSH-over-USB to the BeagleBone Black.
#
# Prereq: the BBB must be running a HILGLEBONE_DEV=1 image (so the
# usb-gadget-net recipe is installed) and connected to this machine
# via the micro-USB cable on J1.
#
# Run AFTER plugging in the cable. The kernel creates a new interface
# (usually enx<mac>) a second or two after enumeration; this script
# finds it by its pinned MAC, assigns 192.168.7.1/24, brings it up,
# clears any stale host key for 192.168.7.2, and prints the ssh command.
#
# Safe to re-run: ip addr add / ip link set are idempotent, and the
# ssh-keygen -R is a no-op if there's nothing to remove.

set -u

TARGET="192.168.7.2"
HOST_IP="192.168.7.1/24"

# The BBB's g_ether module is configured (via /etc/modprobe.d/g_ether.conf
# on the BBB, sourced from yocto/meta-hilglebone/recipes-bsp/usb-gadget-net/
# files/g_ether.options) to hand the HOST side this exact MAC. Because it
# is pinned, the laptop's predictable interface name is stable across
# reboots of the BBB -- so we can search for it deterministically instead
# of guessing by name. If you change host_addr= in g_ether.options, update
# this value too.
GADGET_MAC="02:bb:be:e0:01:01"

# Scan /sys/class/net/*/address -- canonical place to read per-interface
# MAC. Retry for a few seconds because USB enumeration lags slightly
# after plug-in.
IFACE=""
for _ in 1 2 3 4 5; do
    for addr_file in /sys/class/net/*/address; do
        [ -e "$addr_file" ] || continue
        if [ "$(cat "$addr_file")" = "$GADGET_MAC" ]; then
            IFACE=$(basename "$(dirname "$addr_file")")
            break 2
        fi
    done
    sleep 1
done

if [ -z "$IFACE" ]; then
    echo "   No interface with MAC $GADGET_MAC found."
    echo "   Is the BBB plugged in and booted with HILGLEBONE_DEV=1?"
    echo "   Check: dmesg | tail   and   ip -o link show"
    exit 1
fi

echo "[*] Using interface: $IFACE (MAC $GADGET_MAC)"

# Address add returns non-zero if the address is already assigned -- that
# is fine on a re-run, so we swallow the error instead of bailing out.
sudo ip addr add "$HOST_IP" dev "$IFACE" 2>/dev/null || true
sudo ip link set "$IFACE" up

# The BBB regenerates SSH host keys on first boot after a fresh flash,
# so the known_hosts entry for 192.168.7.2 goes stale. Drop it
# preemptively; ssh will prompt to trust the new key on first connect.
ssh-keygen -f "$HOME/.ssh/known_hosts" -R "$TARGET" >/dev/null 2>&1 || true

echo "[*] Ready. Connect with: ssh root@$TARGET"
echo "    (password is blank -- debug-tweaks in the dev image)"
