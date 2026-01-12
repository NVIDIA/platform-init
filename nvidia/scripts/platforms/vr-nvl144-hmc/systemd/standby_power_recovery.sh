#!/bin/sh
#
# standby_power_recovery.sh - Recovery service for late standby power
#
# This script runs as a systemd service when standby power was not available
# at boot time (indicated by /run/standby_power_failure marker).
#
# It polls for standby power to become available and reboots the system
# when detected to ensure a clean state.
#

# Board 0 configuration (required for standby power)
BRD0_BUS="1"
BRD0_IOX0_ADDR="0x20"

# Polling configuration
POLL_INTERVAL=10

# Reboot loop tracking
REBOOT_LOOP_COUNT_FILE="/var/RebootLoopCount"
MAX_REBOOT_COUNT=3

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
    # Match either hex address (unbound) or UU (driver bound)
    if i2cdetect -y "$bus" "$addr" "$addr" 2>/dev/null | grep -qE " ([0-9a-f]{2}|UU)"; then
        return 0
    fi
    return 1
}

#######################################
# Initialize or read reboot loop counter
#######################################
init_reboot_counter() {
    if [ ! -f "$REBOOT_LOOP_COUNT_FILE" ]; then
        echo "0" > "$REBOOT_LOOP_COUNT_FILE"
        echo "Created reboot loop counter file"
    fi
    cat "$REBOOT_LOOP_COUNT_FILE"
}

#######################################
# Increment reboot loop counter
#######################################
increment_reboot_counter() {
    count=$(cat "$REBOOT_LOOP_COUNT_FILE" 2>/dev/null || echo "0")
    count=$((count + 1))
    echo "$count" > "$REBOOT_LOOP_COUNT_FILE"
    echo "$count"
}

#######################################
# Main
#######################################

echo "Starting standby power recovery service"
echo "Standby power was not available at boot - polling for recovery..."

# Initialize/read reboot counter
current_count=$(init_reboot_counter)
echo "Current reboot loop count: $current_count (max: $MAX_REBOOT_COUNT)"

# Check if we've exceeded max reboot attempts
if [ "$current_count" -ge "$MAX_REBOOT_COUNT" ]; then
    echo "ERROR: Maximum reboot count ($MAX_REBOOT_COUNT) reached!"
    echo "Giving up on standby power recovery to prevent reboot loop."
    echo "Manual intervention required."
    exit 1
fi

echo "Polling every ${POLL_INTERVAL}s for BRD0 on bus $BRD0_BUS"

while true; do
    if check_i2c_device "$BRD0_BUS" "$BRD0_IOX0_ADDR"; then
        echo "Standby power detected! BRD0 IO expander found at bus $BRD0_BUS addr $BRD0_IOX0_ADDR"
        
        # Increment reboot counter before rebooting
        new_count=$(increment_reboot_counter)
        echo "Reboot loop count incremented to: $new_count"
        
        echo "Initiating reboot for clean system state..."
        
        # Give a moment for log to flush
        sleep 2
        
        # Reboot the system
        reboot
        
        # Should not reach here, but exit just in case
        exit 0
    fi

    sleep "$POLL_INTERVAL"
done

