# vr-nvl72-hmc Initramfs Scripts

## Overview

This directory contains initramfs scripts for the vr-nvl72-hmc platform that run
before the main OpenBMC init to ensure standby power is available and bind I2C
IO expander drivers.

## Scripts

### `init-wrapper.sh`

Entry point (`/init`) in initramfs. Mounts `/run` tmpfs and calls
`standby_power_wait.sh`, then `rtc-sync.sh` if the initramfs has one, before
executing the main `obmc-init.sh`. Platforms with no external RTC omit it.

### `standby_power_wait.sh`

Polls I2C buses for IO expander devices that indicate standby power is ready,
then binds the drivers via sysfs. Handles two boards independently:

- **BRD0 (bus 1)**: Required - timeout is a standby power failure
- **BRD1 (bus 7)**: Optional - absence is noted but not a failure

## Platform Overrides

`standby_power_wait.sh` sources `/etc/standby_power_wait.conf` if the initramfs
has one, so a platform can override the bus numbers, addresses and timeouts, or
set `REBIND_DEVICES=0` when the kernel binds the devices on its own:

```sh
BRD0_BUS="80"
BRD1_BUS="81"
BRD1_MAX_ATTEMPTS=30
REBIND_DEVICES=0
```

## Boot Flow

```text
1. Poll BRD0 (bus 1, addr 0x20) for up to 2 minutes
   ├── Found → Bind tca9535 drivers for BRD0 (0x20, 0x21)
   │           Poll BRD1 (bus 7, addr 0x20) for 5 seconds
   │           ├── Found → Bind tca9535 drivers for BRD1 (0x20, 0x21)
   │           └── Not found → Create /run/brd1_not_present, continue
   │
   └── Timeout → Create /run/standby_power_failure, continue
```

## Marker Files

| File                         | Meaning                                              |
| ---------------------------- | ---------------------------------------------------- |
| `/run/standby_power_failure` | BRD0 timed out - standby power issue (critical)      |
| `/run/brd1_not_present`      | BRD1 not found - single-board config (informational) |

## Systemd Integration

Services that should NOT start when standby power failed:

```ini
[Unit]
ConditionPathExists=!/run/standby_power_failure
```

Services that require both boards:

```ini
[Unit]
ConditionPathExists=!/run/standby_power_failure
ConditionPathExists=!/run/brd1_not_present
```

### Creating a drop-in override

For existing services, create a drop-in file:

```text
/etc/systemd/system/<service-name>.service.d/standby-power-check.conf
```

Or in a BitBake recipe, install to:

```text
${D}${systemd_system_unitdir}/<service-name>.service.d/standby-power-check.conf
```

## Device Tree Requirements

The IO expanders must be set to `status = "disabled"` in the device tree so the
kernel doesn't try to probe them during boot (before standby power is ready).
This script binds them via sysfs after detecting standby power.

Devices to disable in DT:

- `exp0_brd0: tca9535@20` on i2c1
- `exp1_brd0: tca9535@21` on i2c1
- `exp0_brd1: tca9535@20` on i2c7
- `exp1_brd1: tca9535@21` on i2c7

## Timeout Configuration

| Board | Timeout                       | Behavior on timeout                 |
| ----- | ----------------------------- | ----------------------------------- |
| BRD0  | 2 minutes (120 attempts × 1s) | Create `/run/standby_power_failure` |
| BRD1  | 5 seconds (5 attempts × 1s)   | Create `/run/brd1_not_present`      |
