#!/bin/sh
#
# RTC Sync (initramfs) - Run during early boot to set system time from RTC.
#
# Decision: try external I2C RTC (NCT3018Y) only when host power is down, so we
# don't collide with the host on the shared bus. Primary signal: BRD0_RUN_POWER_PG-I
# (1 = host up -> internal RTC only; 0 = host down -> try external then internal).
# Fallback when GPIO unavailable: U-Boot boot reason (reset_reason from MTD u-boot-env).
# We run after standby_power_wait so I2C/GPIO (e.g. CP2112) have time to enumerate.
# We retry reading env and GPIO a few times in case MTD or gpiochip are not ready yet.
# When both GPIO and boot reason are unknown we use internal RTC only (safe default:
# avoids possible I2C collision; downside is wrong timestamps until NTP or next sync).
#
# Requires: for external RTC: hwclock, i2ctransfer (i2c-tools); for internal RTC:
# devmem, date; gpiofind/gpioget (libgpiod-tools).
#
# The kernel rtc-nct3018y driver and I2C core do not retry on bus busy; we retry
# the external RTC path a few times before falling back to internal RTC.
#
# Internal (ASPEED AST2600) RTC: we read registers at 1e781000 (RTC00 offset 0x00:
# SecCnt 5:0, MinuCnt 13:8, HourCnt 20:16, DayCnt 28:24; RTC04 offset 0x04:
# MonCnt 3:0, YearCnt 14:8, CentCnt 20:16) and set system time with date -s.
#

AST2600_RTC_BASE=0x1e781000
I2C_BUS=9
RTC_EXT_RETRIES=3
RTC_EXT_RETRY_DELAY=2
# Retries for binding RTC driver (I2C/sysfs may be briefly unavailable)
BIND_RTC_RETRIES=3
BIND_RTC_RETRY_DELAY=1
# Retries for GPIO / boot reason when run early (MTD or gpiochip may not be ready)
RTC_READINESS_RETRIES=5
RTC_READINESS_DELAY=2
I2C_ADDR=0x6f
VENDOR_REG=0x10
I2C_DEV_PATH="/sys/class/i2c-dev/i2c-${I2C_BUS}/device"
RTC_DEVICE=""
RTC_DEV=""

log() { echo "rtc-sync: $*" >&2; }

BRD0_POWER_GPIO_NAME="BRD0_RUN_POWER_PG-I"

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
    local dev_id
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
    log "Device ID: ${dev_id}"
    if [ "$dev_id" = "0x5c" ]; then
        RTC_DEVICE="nct3018y"
        return 0
    fi
    log "ERROR - Expected NCT3018Y (0x5c), got ${dev_id}"
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
    if [ -z "$RTC_DEV" ]; then
        log "ERROR - RTC device not set"
        return 1
    fi
    if ! hwclock -s -f "/dev/${RTC_DEV}" 2>/dev/null; then
        log "ERROR - Failed to set system clock from /dev/${RTC_DEV}"
        return 1
    fi
    log "System clock set from RTC: $(date 2>/dev/null || true)"
    sync_aspeed_rtc
    return 0
}

sync_aspeed_rtc() {
    if [ ! -c /dev/rtc0 ]; then
        return 0
    fi
    hwclock -w -f /dev/rtc0 2>/dev/null || true
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

# Read AST2600 RTC counter registers (binary, per datasheet) and set system clock.
read_internal_rtc() {
    local reg00 reg04 sec min hour mday mon year cent full_year datestr

    if ! command -v devmem >/dev/null 2>&1; then
        log "ERROR - devmem not available for internal RTC read"
        return 1
    fi
    if ! command -v date >/dev/null 2>&1; then
        log "ERROR - date not available for setting system clock"
        return 1
    fi

    reg00=$(devmem $((AST2600_RTC_BASE + 0x00)) 32 2>/dev/null)
    reg04=$(devmem $((AST2600_RTC_BASE + 0x04)) 32 2>/dev/null)
    if [ -z "$reg00" ] || [ -z "$reg04" ]; then
        log "ERROR - Failed to read ASPEED RTC registers (base ${AST2600_RTC_BASE})"
        return 1
    fi

    # RTC00: SecCnt 5:0, MinuCnt 13:8, HourCnt 20:16, DayCnt 28:24 (binary)
    sec=$((reg00 & 0x3f))
    min=$(((reg00 >> 8) & 0x3f))
    hour=$(((reg00 >> 16) & 0x1f))
    mday=$(((reg00 >> 24) & 0x1f))
    # RTC04: MonCnt 3:0 (1-12), YearCnt 14:8 (0-99), CentCnt 20:16 (0-31)
    mon=$((reg04 & 0x0f))
    year=$(((reg04 >> 8) & 0x7f))
    cent=$(((reg04 >> 16) & 0x1f))
    full_year=$((cent * 100 + year))

    datestr=$(printf "%04d-%02d-%02d %02d:%02d:%02d" "$full_year" "$mon" "$mday" "$hour" "$min" "$sec")
    if ! date -u -s "$datestr" >/dev/null 2>&1; then
        log "ERROR - Failed to set system clock from ASPEED RTC: date -u -s \"$datestr\" failed"
        return 1
    fi
    log "System clock set from internal RTC (regs): $datestr"
    return 0
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
    if should_try_external_rtc; then
        if ! command -v hwclock >/dev/null 2>&1; then
            log "hwclock not available, using internal RTC only"
            if read_internal_rtc; then
                log "Complete (internal RTC)"
                return 0
            fi
            log "RTC sync failed - system time may be wrong"
            return 1
        fi
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
