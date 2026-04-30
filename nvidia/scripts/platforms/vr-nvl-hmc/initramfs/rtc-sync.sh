#!/bin/sh
#
# RTC Sync (initramfs) - Run during early boot to set system time from RTC.
#
# Decision: try external I2C RTC only when host power is down, so we don't
# collide with the host on the shared bus. Primary signal: BRD0_RUN_POWER_PG-I
# (1 = host up -> internal RTC only; 0 = host down -> try external then internal).
# Fallback when GPIO unavailable: U-Boot boot reason (reset_reason from MTD u-boot-env).
# We run after standby_power_wait so I2C/GPIO (e.g. CP2112) have time to enumerate.
# We retry reading env and GPIO a few times in case MTD or gpiochip are not ready yet.
# When both GPIO and boot reason are unknown we use internal RTC only (safe default:
# avoids possible I2C collision; downside is wrong timestamps until NTP or next sync).
#
# External RTC: P4105 may be populated with either the Nuvoton NCT3018Y-R or the
# NXP PCF85053A. Both chips sit at I2C address 0x6f and expose a vendor-ID byte at
# register 0x10 (Nuvoton = 0x5c, NXP = 0x4e). We probe that byte to pick the right
# Linux driver (nct3018y vs pcf85053a) and bind it via i2c-dev's new_device. Once
# bound, both expose a standard /dev/rtcN that hwclock can read identically.
#
# Requires: date (BusyBox or coreutils, with `-s @SECONDS` support);
# i2ctransfer (i2c-tools); gpiofind/gpioget (libgpiod-tools); hwclock is
# optional - used only for the best-effort write-back from system clock to
# /dev/rtc0 after a successful external RTC sync. We deliberately do *not*
# use `hwclock -s` to apply RTC time to the system clock: empirically, on
# this image (BusyBox 1.37 hwclock + rtc-aspeed driver), `hwclock -s -f
# /dev/rtc0` always fails with "select() to /dev/rtc0 to wait for clock
# tick timed out" after ~10s. The root cause has not been pinned down -
# the rtc-aspeed driver does not clear RTC_FEATURE_UPDATE_INTERRUPT, so the
# rtc framework's primary UIE path (hrtimer-based, interface.c:598-626)
# should be in use, but evidently is not delivering ticks to /dev/rtc0
# readers within hwclock's timeout. Rather than chase that, we sidestep it
# by reading /sys/class/rtc/rtc<N>/since_epoch and applying with
# `date -u -s "@<sec>"`. Production images strip /dev/mem
# (CONFIG_DEVMEM=n), so the previous devmem-based register-read path is gone.
#
# Kernel I2C core does not retry on bus busy; we retry the external RTC path a
# few times before falling back to internal RTC.
#
# Internal (ASPEED AST2600) RTC: BusyBox `hwclock -s` against /dev/rtc0
# empirically times out after ~10s (see Requires: paragraph above for the
# unresolved why), so we use two hwclock-free paths:
#   1. If the kernel already set the system clock from this RTC at probe
#      (sysfs hctosys=1), we are done; just log and return.
#   2. Otherwise we read /sys/class/rtc/rtc0/since_epoch and apply with
#      `date -u -s "@<epoch>"`. This uses the same kernel ioctl path as
#      hwclock would for the read itself, just without hwclock's tick-wait
#      synchronization that is what's actually timing out.
#

I2C_BUS=9
RTC_EXT_RETRIES=3
RTC_EXT_RETRY_DELAY=2
# Retries for binding RTC driver (I2C/sysfs may be briefly unavailable)
BIND_RTC_RETRIES=3
BIND_RTC_RETRY_DELAY=1
# Retries for GPIO / boot reason when run early (MTD or gpiochip may not be ready)
RTC_READINESS_RETRIES=5
RTC_READINESS_DELAY=2
# Retries waiting for /dev/rtc0 (rtc-aspeed driver may not have probed yet)
INTERNAL_RTC_RETRIES=5
INTERNAL_RTC_DELAY=2
I2C_ADDR=0x6f
VENDOR_REG=0x10
# Supported external RTC chips at I2C_ADDR. Format: "<vendor_id> <driver_name>".
# vendor_id is the byte read from VENDOR_REG (0x10) and must be lowercase to match
# i2ctransfer's output. driver_name is the kernel module / i2c-dev id used by the
# new_device sysfs entry.
RTC_CANDIDATES="0x5c nct3018y
0x4e pcf85053a"
I2C_DEV_PATH="/sys/class/i2c-dev/i2c-${I2C_BUS}/device"
RTC_DEVICE=""
RTC_DEV=""

# Write directly to /dev/kmsg with `>>` to avoid stdio buffering through the
# init-wrapper's `> /dev/kmsg 2>&1` redirect. BusyBox ash's `echo` uses stdio,
# which fully-buffers when stdout points at a non-tty char device; messages
# only flush on subsequent forks or clean exit, so any log that runs late and
# isn't followed by another fork can be lost if the shell exits abnormally.
# Falls back to fd 2 if /dev/kmsg isn't writable (e.g. running standalone for
# debugging outside initramfs).
log() {
    if ! printf 'rtc-sync: %s\n' "$*" >>/dev/kmsg 2>/dev/null; then
        printf 'rtc-sync: %s\n' "$*" >&2
    fi
}

BRD0_POWER_GPIO_NAME="BRD0_MODULE_PWR_GOOD-I"

# Find MTD partition by name (e.g. u-boot-env). Requires /sys mounted.
findmtd() {
    local m
    m=$(grep -xl "$1" /sys/class/mtd/*/name 2>/dev/null)
    m=${m%/name}
    m=${m##*/}
    echo "$m"
}

# Read a variable from U-Boot environment in MTD. Same logic as obmc-init.sh (CRC + optional flag).
# Requires /dev and /sys. Used to read reset_reason when we run before obmc-init.
get_fw_env_var() {
    local envdev copies
    envdev=$(findmtd u-boot-env)
    [ -z "$envdev" ] && return 1
    copies=2
    cat /dev/"$envdev" 2>/dev/null |
        tr '\n\000' '\r\n' |
        tail -c +5 | tail -c +${copies-1} |
        sed -ne '/^$/,$d' -e "s/^$1=//p"
}

# Get BRD0 power GPIO: 0 = host down (safe to use external RTC), 1 = host up (internal only), 2 = unknown
# Also logs the result.
get_brd0_power_gpio() {
    local gpio_line gpio_state

    if ! command -v gpiofind >/dev/null 2>&1 || ! command -v gpioget >/dev/null 2>&1; then
        log "BRD0 power GPIO: gpiofind/gpioget not available"
        return 2
    fi
    gpio_line=$(gpiofind "${BRD0_POWER_GPIO_NAME}" 2>/dev/null)
    if [ -z "$gpio_line" ]; then
        log "BRD0 power GPIO: ${BRD0_POWER_GPIO_NAME} not found (e.g. CP2112 not enumerated yet)"
        return 2
    fi
    gpio_state=$(gpioget ${gpio_line} 2>/dev/null)
    if [ $? -ne 0 ]; then
        log "BRD0 power GPIO: read failed for ${BRD0_POWER_GPIO_NAME}"
        return 2
    fi
    log "BRD0 power GPIO: ${BRD0_POWER_GPIO_NAME} = ${gpio_state} (1=host up, 0=host down)"
    return "$gpio_state"
}

# Return 0 if we should try external RTC, 1 if internal only.
# Prefer BRD0 power GPIO (0=host down -> external, 1=host up -> internal).
# When GPIO unknown, use boot reason; retry both a few times (MTD/gpiochip may not be ready yet).
# When both unknown after retries, use internal RTC only (safe default).
should_try_external_rtc() {
    local reason attempt=1
    while [ "$attempt" -le "$RTC_READINESS_RETRIES" ]; do
        get_brd0_power_gpio
        case $? in
            0) log "Host power down - trying external RTC"; return 0 ;;
            1) log "Host power up - using internal RTC only"; return 1 ;;
            2) ;;  # GPIO not available, try boot reason
            *) return 1 ;;
        esac
        reason=$(get_fw_env_var reset_reason 2>/dev/null)
        reason="${reason:-${RTC_SYNC_BOOT_REASON:-}}"
        if [ -n "$reason" ]; then
            log "Boot reason: '${reason}' (GPIO unavailable, attempt ${attempt}/${RTC_READINESS_RETRIES})"
            case "$reason" in
                Power_On) log "Boot reason Power_On - trying external RTC"; return 0 ;;
                WDT*)     log "Boot reason warm reboot - using internal RTC only"; return 1 ;;
                *)       log "Unknown state - using internal RTC only (safe default)"; return 1 ;;
            esac
        fi
        if [ "$attempt" -lt "$RTC_READINESS_RETRIES" ]; then
            log "Boot reason and GPIO not ready (attempt ${attempt}/${RTC_READINESS_RETRIES}), waiting ${RTC_READINESS_DELAY}s..."
            sleep "$RTC_READINESS_DELAY"
        fi
        attempt=$((attempt + 1))
    done
    log "Boot reason: '<unset>' (GPIO and env unavailable after ${RTC_READINESS_RETRIES} attempts - using internal RTC only)"
    return 1
}

detect_rtc_device() {
    local dev_id cand_id cand_drv expected oldifs
    log "Probing I2C bus ${I2C_BUS} at address ${I2C_ADDR}..."
    if ! command -v i2ctransfer >/dev/null 2>&1; then
        log "ERROR - i2ctransfer not available"
        return 1
    fi
    dev_id=$(i2ctransfer -f -y ${I2C_BUS} w1@${I2C_ADDR} ${VENDOR_REG} r1 2>/dev/null | awk '{print $1}')
    if [ -z "$dev_id" ]; then
        log "ERROR - Failed to read device ID from I2C ${I2C_BUS} at ${I2C_ADDR}"
        return 1
    fi
    log "Vendor ID at reg ${VENDOR_REG}: ${dev_id}"
    # Walk candidates table (newline-separated "<id> <driver>" rows). First match wins.
    # Splitting on newline with IFS keeps the for loop in this shell so RTC_DEVICE persists.
    RTC_DEVICE=""
    expected=""
    oldifs="$IFS"
    IFS='
'
    for row in $RTC_CANDIDATES; do
        IFS="$oldifs"
        cand_id=$(echo "$row" | awk '{print $1}')
        cand_drv=$(echo "$row" | awk '{print $2}')
        [ -z "$cand_id" ] && continue
        expected="${expected}${cand_id} "
        if [ "$dev_id" = "$cand_id" ]; then
            RTC_DEVICE="$cand_drv"
            break
        fi
    done
    IFS="$oldifs"
    if [ -n "$RTC_DEVICE" ]; then
        log "Matched external RTC: ${RTC_DEVICE} (vendor ${dev_id})"
        return 0
    fi
    log "ERROR - Unrecognized RTC vendor ID ${dev_id} (expected one of: ${expected})"
    return 1
}

bind_rtc_driver() {
    if [ -z "$RTC_DEVICE" ]; then
        log "ERROR - RTC device type not set"
        return 1
    fi
    if [ ! -d "${I2C_DEV_PATH}" ]; then
        log "ERROR - I2C path ${I2C_DEV_PATH} does not exist"
        return 1
    fi
    log "Binding ${RTC_DEVICE} to ${I2C_ADDR} on bus ${I2C_BUS}..."
    echo "${RTC_DEVICE} ${I2C_ADDR}" > "${I2C_DEV_PATH}/new_device" 2>/dev/null
    if [ $? -ne 0 ]; then
        log "ERROR - Failed to bind driver"
        return 1
    fi
    sleep 1
    RTC_DEV=$(ls -1t /sys/class/rtc/ 2>/dev/null | grep -v "^rtc0$" | head -n 1)
    if [ -z "$RTC_DEV" ]; then
        log "ERROR - No RTC device found after bind"
        return 1
    fi
    log "Bound RTC device: /dev/${RTC_DEV}"
    return 0
}

sync_system_clock() {
    local sysfs_date sysfs_time sysfs_epoch
    if [ -z "$RTC_DEV" ]; then
        log "ERROR - RTC device not set"
        return 1
    fi
    # Log sysfs view of the freshly-bound external RTC for breadcrumb.
    if [ -d "/sys/class/rtc/${RTC_DEV}" ]; then
        sysfs_date=$(cat "/sys/class/rtc/${RTC_DEV}/date" 2>/dev/null || echo '<unreadable>')
        sysfs_time=$(cat "/sys/class/rtc/${RTC_DEV}/time" 2>/dev/null || echo '<unreadable>')
        sysfs_epoch=$(cat "/sys/class/rtc/${RTC_DEV}/since_epoch" 2>/dev/null || echo '<unreadable>')
        log "  ${RTC_DEV} sysfs reads: ${sysfs_date} ${sysfs_time} (since_epoch=${sysfs_epoch})"
    else
        log "  /sys/class/rtc/${RTC_DEV} missing"
        return 1
    fi
    # Apply via sysfs+date instead of hwclock -s (which empirically times out
    # on /dev/rtc0 - see header). Use the same hwclock-free path uniformly so
    # external (nct3018y/pcf85053a) behaviour matches internal.
    if ! set_system_time_from_sysfs "$RTC_DEV"; then
        log "ERROR - failed to set system clock from external RTC ${RTC_DEV}"
        return 1
    fi
    log "System clock set from external RTC ${RTC_DEV} (epoch applied)"
    log "calling sync_aspeed_rtc (write-back to /dev/rtc0)"
    sync_aspeed_rtc
    log "sync_aspeed_rtc returned, sync_system_clock done"
    return 0
}

sync_aspeed_rtc() {
    local hw_out hw_ret
    if [ ! -c /dev/rtc0 ]; then
        log "  sync_aspeed_rtc: /dev/rtc0 not present, skipping write-back to internal RTC"
        return 0
    fi
    hw_out=$(hwclock -w -f /dev/rtc0 2>&1)
    hw_ret=$?
    if [ "$hw_ret" -eq 0 ]; then
        log "  sync_aspeed_rtc: wrote system time back to /dev/rtc0"
    else
        log "  sync_aspeed_rtc: hwclock -w failed (exit ${hw_ret}): ${hw_out}"
        log_internal_rtc_state "sync_aspeed_rtc failure"
    fi
    return 0
}

unbind_rtc_driver() {
    local dev_dir
    if [ -z "$RTC_DEVICE" ]; then
        return 0
    fi
    dev_dir="${I2C_DEV_PATH}/${I2C_BUS}-$(printf '%04x' ${I2C_ADDR})"
    [ -d "$dev_dir" ] || dev_dir="${I2C_DEV_PATH}/${I2C_BUS}-$(printf '%02x' ${I2C_ADDR})"
    if [ -d "$dev_dir" ]; then
        echo "${I2C_ADDR}" > "${I2C_DEV_PATH}/delete_device" 2>/dev/null || true
    fi
    return 0
}

# Dump diagnostic state about the internal RTC for debugging hwclock failures.
# Output goes to the rtc-sync log (init-wrapper redirects rtc-sync.sh to
# /dev/kmsg, so this also lands in dmesg). Sysfs RTC reads use the same kernel
# code path as hwclock, so they're useful as a cross-check: if sysfs shows a
# valid time but hwclock fails, the bug is likely in user-space (busybox/util-
# linux hwclock); if sysfs fails too, the kernel driver itself is the culprit.
log_internal_rtc_state() {
    local prefix="${1:-rtc state}"
    log "  ${prefix}: /sys/class/rtc entries: $(ls /sys/class/rtc/ 2>/dev/null | tr '\n' ' ')"
    if [ -d /sys/class/rtc/rtc0 ]; then
        log "  ${prefix}: rtc0 name: $(cat /sys/class/rtc/rtc0/name 2>/dev/null || echo '<unreadable>')"
        log "  ${prefix}: rtc0 hctosys: $(cat /sys/class/rtc/rtc0/hctosys 2>/dev/null || echo '<unreadable>')"
        log "  ${prefix}: rtc0 since_epoch: $(cat /sys/class/rtc/rtc0/since_epoch 2>/dev/null || echo '<unreadable>')"
        log "  ${prefix}: rtc0 date+time (sysfs): $(cat /sys/class/rtc/rtc0/date 2>/dev/null || echo '<unreadable>') $(cat /sys/class/rtc/rtc0/time 2>/dev/null || echo '<unreadable>')"
    fi
    # Single line; tr collapses ls's multi-line output so it stays one log entry.
    log "  ${prefix}: /dev/rtc* nodes: $(ls -la /dev/rtc* 2>&1 | tr '\n' '|')"
}

# Apply system clock from the kernel's view of an RTC by reading sysfs
# /sys/class/rtc/${rtc_name}/since_epoch and calling `date -u -s "@<sec>"`.
# Used in place of `hwclock -s -f /dev/<rtc_name>` because BusyBox hwclock's
# -s synchronises to a RTC tick via select() and empirically times out after
# ~10s on /dev/rtc0 (see header for details). The sysfs read uses the same
# kernel ioctl as hwclock would for the actual read, just without the tick
# wait that is what's actually failing.
#
# Returns 0 on success, 1 on read or apply failure.
set_system_time_from_sysfs() {
    local rtc_name="$1" epoch
    if [ ! -d "/sys/class/rtc/${rtc_name}" ]; then
        log "  /sys/class/rtc/${rtc_name} missing"
        return 1
    fi
    epoch=$(cat "/sys/class/rtc/${rtc_name}/since_epoch" 2>/dev/null)
    if [ -z "$epoch" ]; then
        log "  /sys/class/rtc/${rtc_name}/since_epoch unreadable"
        return 1
    fi
    # Reject obviously bogus times (e.g. RTC reads back 1970 because never set).
    # The kernel itself rejects similarly: on invalid reads it returns -EINVAL
    # and refuses to set hctosys, but a freshly-cold RTC can still read 0.
    if [ "$epoch" -lt 1700000000 ] 2>/dev/null; then
        log "  /sys/class/rtc/${rtc_name}/since_epoch=${epoch} predates Nov 2023; refusing to set system time backward"
        return 1
    fi
    if ! date -u -s "@${epoch}" >/dev/null 2>&1; then
        log "  date -u -s \"@${epoch}\" failed"
        return 1
    fi
    log "  date -u -s \"@${epoch}\" applied (rtc=${rtc_name})"
    return 0
}

# Sync system clock from the AST2600 internal RTC. Two paths:
#   1. If sysfs hctosys=1, the kernel already set system time at probe and we
#      have nothing to do - just confirm and return.
#   2. Otherwise apply via set_system_time_from_sysfs (see helper above for why
#      we avoid hwclock -s).
#
# /dev/rtc0 may not exist yet very early in initramfs because the aspeed-rtc
# driver probes ~2.6s after kernel boot; we retry briefly for it to appear.
#
# Note: production builds disable /dev/mem (CONFIG_DEVMEM=n) so we cannot
# fall back to reading the RTC registers via devmem. If neither hctosys nor
# sysfs+date works within the retry budget, we return failure and let NTP
# correct the time later.
read_internal_rtc() {
    local attempt=1 rtc_dev=/dev/rtc0 hctosys sysfs_date sysfs_time sysfs_epoch

    while [ "$attempt" -le "$INTERNAL_RTC_RETRIES" ]; do
        log "Internal RTC attempt ${attempt}/${INTERNAL_RTC_RETRIES}: probing ${rtc_dev}"

        if [ ! -c "$rtc_dev" ]; then
            log "  ${rtc_dev} not present yet (rtc-aspeed driver may not have probed)"
            log_internal_rtc_state "attempt ${attempt}"
        else
            # Sysfs read for log breadcrumb (also informs the hctosys check below).
            sysfs_date=$(cat /sys/class/rtc/rtc0/date 2>/dev/null || echo '<unreadable>')
            sysfs_time=$(cat /sys/class/rtc/rtc0/time 2>/dev/null || echo '<unreadable>')
            sysfs_epoch=$(cat /sys/class/rtc/rtc0/since_epoch 2>/dev/null || echo '<unreadable>')
            hctosys=$(cat /sys/class/rtc/rtc0/hctosys 2>/dev/null || echo '<unreadable>')
            log "  rtc0 sysfs reads: ${sysfs_date} ${sysfs_time} (since_epoch=${sysfs_epoch}, hctosys=${hctosys})"

            # Path 1: kernel already did it for us at rtc-aspeed probe.
            if [ "$hctosys" = "1" ]; then
                log "System clock already set by kernel at rtc-aspeed probe (hctosys=1); current: $(date 2>/dev/null || true)"
                return 0
            fi

            # Path 2: apply via sysfs+date (avoids hwclock UIE timeout).
            if set_system_time_from_sysfs rtc0; then
                log "System clock set from internal RTC sysfs: $(date 2>/dev/null || true)"
                return 0
            fi
            log_internal_rtc_state "attempt ${attempt} after sysfs apply failure"
        fi

        if [ "$attempt" -lt "$INTERNAL_RTC_RETRIES" ]; then
            sleep "$INTERNAL_RTC_DELAY"
        fi
        attempt=$((attempt + 1))
    done

    log "ERROR - Failed to set system clock from internal RTC after ${INTERNAL_RTC_RETRIES} attempts"
    log_internal_rtc_state "final"
    return 1
}

try_external_rtc() {
    local bind_attempt=1
    if ! detect_rtc_device; then
        return 1
    fi
    while [ "$bind_attempt" -le "$BIND_RTC_RETRIES" ]; do
        if bind_rtc_driver; then
            break
        fi
        if [ "$bind_attempt" -lt "$BIND_RTC_RETRIES" ]; then
            log "bind_rtc_driver attempt ${bind_attempt}/${BIND_RTC_RETRIES} failed, retrying in ${BIND_RTC_RETRY_DELAY}s..."
            sleep "$BIND_RTC_RETRY_DELAY"
        else
            log "bind_rtc_driver failed after ${BIND_RTC_RETRIES} attempts"
            return 1
        fi
        bind_attempt=$((bind_attempt + 1))
    done
    if sync_system_clock; then
        unbind_rtc_driver
        return 0
    fi
    unbind_rtc_driver
    return 1
}

main() {
    # We sync the system clock via sysfs+date rather than hwclock -s, which
    # empirically times out after ~10s on /dev/rtc0 (see header). hwclock is
    # now only used by sync_aspeed_rtc for the best-effort write-back to
    # /dev/rtc0 after a successful external RTC sync; absence is logged there.
    if ! command -v date >/dev/null 2>&1; then
        log "ERROR - date not available; cannot apply RTC time to system clock"
        return 1
    fi

    if should_try_external_rtc; then
        log "Trying external RTC first (up to ${RTC_EXT_RETRIES} attempts)"
        attempt=1
        while [ "$attempt" -le "$RTC_EXT_RETRIES" ]; do
            if try_external_rtc; then
                log "Complete (external RTC)"
                return 0
            fi
            if [ "$attempt" -lt "$RTC_EXT_RETRIES" ]; then
                log "External RTC attempt ${attempt} failed, retrying in ${RTC_EXT_RETRY_DELAY}s..."
                sleep "$RTC_EXT_RETRY_DELAY"
            fi
            attempt=$((attempt + 1))
        done
        log "External RTC failed after ${RTC_EXT_RETRIES} attempts, falling back to internal"
        if read_internal_rtc; then
            log "Complete (internal RTC fallback)"
            return 0
        fi
    else
        log "Using internal RTC only"
        if read_internal_rtc; then
            log "Complete (internal RTC)"
            return 0
        fi
    fi

    log "RTC sync failed - system time may be wrong"
    return 1
}

main
exit $?
