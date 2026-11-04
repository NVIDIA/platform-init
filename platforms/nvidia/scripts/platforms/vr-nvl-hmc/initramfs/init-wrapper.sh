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

# Wait for standby power devices on I2C first so GPIO (e.g. CP2112) may be
# enumerated before we read BRD0 power and U-Boot env for RTC sync.
# On timeout, creates /run/standby_power_failure; logs to /run/standby_power_wait.log
[ -x /standby_power_wait.sh ] && /standby_power_wait.sh

# Platforms with no external RTC leave rtc-sync.sh out of the initramfs.
if [ -x /rtc-sync.sh ]; then
    /rtc-sync.sh > /dev/kmsg 2>&1
fi

# Continue with main init script, passing through any arguments
exec /obmc-init.sh "$@"

