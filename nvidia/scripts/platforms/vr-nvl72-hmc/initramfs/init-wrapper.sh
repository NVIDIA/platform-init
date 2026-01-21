#!/bin/sh
#
# init-wrapper.sh - Initramfs init wrapper for vr-nvl72-hmc
#
# This script runs as /init in initramfs before the main obmc-init.sh.
# It mounts necessary filesystems and runs standby_power_wait.sh to ensure
# standby power is available before continuing boot.
#

# Mount essential filesystems needed for standby power detection
# obmc-init.sh checks if these are mounted and skips re-mounting
mkdir -p /dev /sys /proc /run
mount -t devtmpfs devtmpfs /dev
mount -t sysfs sysfs /sys
mount -t proc proc /proc
mount -t tmpfs tmpfs /run -o mode=755,nodev

# Wait for standby power devices to appear on I2C
# On timeout, this creates /run/standby_power_failure marker
# Logs are written to /run/standby_power_wait.log
/standby_power_wait.sh

# Continue with main init script, passing through any arguments
exec /obmc-init.sh "$@"

