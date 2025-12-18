#!/bin/sh
#
# standby_power_wait.sh - Wait for standby power I2C devices before boot
#
# This script runs in initramfs before obmc-init.sh to ensure standby power
# is available by probing for I2C devices that appear when power is ready.
#
# Board 0 (bus 1) is required - timeout waiting for it is a standby power failure.
# Board 1 (bus 7) is optional - absence is noted but not a failure.
#
# On timeout, creates marker files that systemd services can use with
# ConditionPathExists directives.
#

# Log file location
LOG_FILE="/run/standby_power_wait.log"

# Board 0 configuration (required)
BRD0_BUS="1"
BRD0_IOX0_ADDR="0x20"
BRD0_IOX1_ADDR="0x21"

# Board 1 configuration (optional)
BRD1_BUS="7"
BRD1_IOX0_ADDR="0x20"
BRD1_IOX1_ADDR="0x21"

# Timing configuration
POLL_INTERVAL=1
BRD0_TIMEOUT_SECONDS=120
BRD0_MAX_ATTEMPTS=$((BRD0_TIMEOUT_SECONDS / POLL_INTERVAL))

# Board 1 gets shorter timeout (5 seconds) since it's optional and should come up with BRD0
BRD1_MAX_ATTEMPTS=5

# Marker file locations
STANDBY_POWER_FAILURE="/run/standby_power_failure"
BRD1_NOT_PRESENT="/run/brd1_not_present"

# Watchdog device
WATCHDOG_DEV="/dev/watchdog"

#######################################
# Log message to file and console
# Arguments:
#   $@ - Message to log
#######################################
log() {
    echo "standby_power_wait: $*" | tee -a "$LOG_FILE"
}

#######################################
# Log debug message (verbose, only to file)
# Arguments:
#   $@ - Message to log
#######################################
log_debug() {
    echo "standby_power_wait: [DEBUG] $*" >> "$LOG_FILE"
}

#######################################
# Pet the watchdog to prevent system reset
# Writes to /dev/watchdog if it exists
#######################################
pet_watchdog() {
    if [ -c "$WATCHDOG_DEV" ]; then
        echo "." > "$WATCHDOG_DEV" 2>/dev/null
    fi
}

#######################################
# Check if an I2C device is present
# Arguments:
#   $1 - I2C bus number
#   $2 - I2C device address (hex, e.g., 0x20)
# Returns:
#   0 if device responds, 1 otherwise
#######################################
check_i2c_device() {
    bus="$1"
    addr="$2"

    # Use i2cdetect to probe the specific address
    # -y: disable interactive mode
    # Using default quick write probe (not -r read byte which fails for tca9535)
    # Pattern: space before hex digits to match detected device, not row labels like "20:"
    result=$(i2cdetect -y "$bus" "$addr" "$addr" 2>&1)
    log_debug "i2cdetect -y $bus $addr $addr output: $result"
    
    if echo "$result" | grep -q " [0-9a-f][0-9a-f]"; then
        log_debug "Device found at bus $bus addr $addr"
        return 0
    fi
    log_debug "Device NOT found at bus $bus addr $addr"
    return 1
}

#######################################
# Bind I2C device driver via sysfs
# Arguments:
#   $1 - I2C bus number
#   $2 - Device type (e.g., tca9535)
#   $3 - I2C device address (hex, e.g., 0x20)
#######################################
bind_i2c_device() {
    bus="$1"
    dev_type="$2"
    addr="$3"

    log "Binding $dev_type at bus $bus address $addr"
    bind_result=$(echo "$dev_type $addr" > /sys/bus/i2c/devices/i2c-${bus}/new_device 2>&1)
    if [ $? -eq 0 ]; then
        log "Successfully bound $dev_type at bus $bus address $addr"
        return 0
    else
        log "WARNING - Failed to bind $dev_type at bus $bus address $addr: $bind_result"
        return 1
    fi
}

#######################################
# Wait for board and bind its IO expanders
# Arguments:
#   $1 - Board name (for logging)
#   $2 - I2C bus number
#   $3 - IO expander 0 address
#   $4 - IO expander 1 address
#   $5 - Max poll attempts
# Returns:
#   0 if board found and bound, 1 if timeout
#######################################
wait_and_bind_board() {
    board_name="$1"
    bus="$2"
    iox0_addr="$3"
    iox1_addr="$4"
    max_attempts="$5"

    log "Waiting for $board_name (bus $bus)..."
    
    attempt=0
    while [ "$attempt" -lt "$max_attempts" ]; do
        attempt=$((attempt + 1))

        # Pet the watchdog to prevent reset during long waits
        pet_watchdog

        if check_i2c_device "$bus" "$iox0_addr"; then
            log "$board_name detected after $((attempt * POLL_INTERVAL))s"
            
            # Bind both IO expanders for this board
            bind_i2c_device "$bus" "tca9535" "$iox0_addr"
            bind_i2c_device "$bus" "tca9535" "$iox1_addr"
            
            return 0
        fi

        log "$board_name attempt $attempt/$max_attempts - not found, waiting ${POLL_INTERVAL}s..."
        sleep "$POLL_INTERVAL"
    done

    return 1
}

#######################################
# Main
#######################################

# Initialize log file
echo "=== standby_power_wait.sh started at $(date 2>/dev/null || echo 'unknown') ===" > "$LOG_FILE"

# Log system state for debugging
log_debug "Checking filesystem mounts..."
log_debug "$(mount 2>&1)"
log_debug "Checking /dev contents..."
log_debug "$(ls -la /dev/i2c* 2>&1)"
log_debug "Checking i2cdetect availability..."
log_debug "$(which i2cdetect 2>&1)"

log "Starting standby power detection..."
log "BRD0 (required): bus $BRD0_BUS, timeout ${BRD0_TIMEOUT_SECONDS}s"
log "BRD1 (optional): bus $BRD1_BUS, timeout $((BRD1_MAX_ATTEMPTS * POLL_INTERVAL))s"

# Wait for Board 0 (required)
if wait_and_bind_board "BRD0" "$BRD0_BUS" "$BRD0_IOX0_ADDR" "$BRD0_IOX1_ADDR" "$BRD0_MAX_ATTEMPTS"; then
    log "BRD0 ready, checking for BRD1..."
    
    # Board 0 is up, now check for Board 1 (optional, shorter timeout)
    if wait_and_bind_board "BRD1" "$BRD1_BUS" "$BRD1_IOX0_ADDR" "$BRD1_IOX1_ADDR" "$BRD1_MAX_ATTEMPTS"; then
        log "BRD1 ready"
    else
        log "BRD1 not present (single-board configuration)"
        touch "$BRD1_NOT_PRESENT"
    fi
    
    log "Standby power initialization complete"
else
    # Board 0 timeout - this is a standby power failure
    log "FAILURE - BRD0 not detected after ${BRD0_TIMEOUT_SECONDS}s"
    log "Creating failure marker: $STANDBY_POWER_FAILURE"
    touch "$STANDBY_POWER_FAILURE"
fi

log "Script finished"
echo "=== standby_power_wait.sh finished ===" >> "$LOG_FILE"

# Exit 0 to allow boot to continue (systemd services will check markers)
exit 0
