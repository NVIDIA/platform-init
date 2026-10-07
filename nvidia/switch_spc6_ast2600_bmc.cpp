// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform initialization daemon for meta-switch/meta-ast2600/meta-spc6-ast2600
// — machine spc6-ast2600-bmc.
// Replaces spc6-ast2600-bmc_ready.sh plus that layer's bmc_ready_common.sh.
//
// Unlike Rosalind, the sysfs GPIO numbers here are not fixed: the Aspeed SoC
// bank and the PCAL6524 expander bases are discovered at runtime, so each bank
// is skipped whole if its base cannot be found. As on every switch platform
// these lines must stay exported for the /var/run/hw-management/system symlink
// tree, so none may be routed through the gpio:: (libgpiod) namespace.
//
// Deliberately not ported: the `.bmc-init-done` marker, a Type=notify daemon
// running once per boot; `hw-management.sh start`, which stays in
// bmc-boot-complete.service's ExecStartPre; `set_bmc_ready $HIGH`, commented
// out in the shell; `remove_ipmitools_permissions`, which has no caller; and
// the VPD EEPROM `new_device` write, done by bmc-early-i2c-init.service here.

#include "platform_utils.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>

#include <array>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace nvidia
{

// ── Platform config ───────────────────────────────────────────────────────
// Source: spc6-ast2600-bmc_ready.sh:25-30 and the meta-spc6-ast2600
//         hw-management-helpers.sh:60-64 it sources.

static constexpr std::string_view spc6_2600_vpd_eeprom =
    "/sys/devices/platform/ahb/1e780000.apb/1e780000.apb:bus@1e78a000/"
    "1e78a280.i2c/i2c-4/4-0051/eeprom";

// The HID lives at offset 22 here, not at Rosalind's 112.
static constexpr size_t spc6_2600_vpd_hid_offset = 22;
static constexpr size_t spc6_2600_vpd_hid_size = 5;
static constexpr size_t spc6_2600_vpd_bom_size = 192;

// $BMC_CPU_PWR_ON / $BMC_CPU_PWR_ON_BUT / $BMC_TO_CPU_CTRL, a `hwmon*` glob in
// the shell because the instance number is assigned at probe time.
static constexpr std::string_view spc6_2600_mlxreg_io_hwmon =
    "/sys/devices/platform/ahb/1e780000.apb/1e780000.apb:bus@1e78a000/"
    "1e78a300.i2c/i2c-5/5-0031/mlxreg-io/hwmon";

// Source: get_system_hw_bom's HID case list.
static constexpr std::array<std::string_view, 5> spc6_2600_bom_hids{
    "HI187", "HI188", "HI191", "HI193", "HI183",
};

// ── GPIO pin config ───────────────────────────────────────────────────────

// Source: bmc_init_sysfs_gpio (spc6-ast2600-bmc_ready.sh:95-257)
// A bank whose base could not be resolved contributes neither pins nor
// symlinks, exactly as the shell's `if [ -n "$..._BASE" ]` blocks. stby_ready
// receives the GP_STBY_PG value path, or stays empty when that bank is missing.
static std::vector<SysfsGpioPin> spc6_2600_gpio_pins(std::string_view hid,
                                                     std::string& stby_ready)
{
    std::vector<SysfsGpioPin> pins;

    // PCAL6524, 24 lines.
    if (const auto base = sysfs_gpiochip_base_by_ngpio(24); base)
    {
        std::cerr << std::format("switch-spc6-ast2600-bmc: expander GPIO base "
                                 "{}\n",
                                 *base);
        const int e = *base;
        pins.push_back(
            {e + 1, SysfsGpioDir::In, std::nullopt, "GP_EROT_RST_IN_L"});
        pins.push_back(
            {e + 2, SysfsGpioDir::In, std::nullopt, "GP_3V3_BMC_PHY_PG"});
        pins.push_back(
            {e + 3, SysfsGpioDir::Out, 1, "GP_BMC_PWR_CYCLE_3V3_MASK_L"});
        pins.push_back({e + 6, SysfsGpioDir::In, std::nullopt,
                        "SE_SOC_EROT_BOOT_COMPLETE"});
        // Direction only: write protect keeps its reset level until
        // unset_write_protect_on_boot() below decides whether to drop it.
        pins.push_back(
            {e + 7, SysfsGpioDir::Out, std::nullopt, "GP_BMC_WP_CTRL_GPIO_L"});
        // BIOS flash recovery: 0 on init to give JTAG access to the flashes,
        // 1 only when recovery is needed.
        pins.push_back(
            {e + 8, SysfsGpioDir::Out, 0, "GP_BMC_REC_SPI_MUX1_SEL"});
        // Offset 10, GPIO_PHY_RST_L, is driven from the DTS — not exported.
        pins.push_back(
            {e + 11, SysfsGpioDir::Out, 1, "GP_JTAG_BMC_CPLD_BURN_EN_L"});

        // Offset 12 has two roles. On HI188 the shell leaves
        // $GPIO_PWR_BANK_PRESENT_L unset, so check_n_link created no symlink —
        // hence the empty name here rather than one for the mux line.
        if (hid == "HI188")
        {
            pins.push_back({e + 12, SysfsGpioDir::Out, 1, ""});
        }
        else
        {
            pins.push_back({e + 12, SysfsGpioDir::In, std::nullopt,
                            "GP_PWR_BANK_PRESENT_L"});
        }

        pins.push_back({e + 19, SysfsGpioDir::Out, 0, "GP_PROD_CS_FLASH0_EN"});
        pins.push_back({e + 21, SysfsGpioDir::Out, 0, "GP_PROD_CS_FLASH1_EN"});
    }
    else
    {
        std::cerr << "switch-spc6-ast2600-bmc: failed to detect the GPIO "
                     "expander base (ngpio=24)\n";
    }

    // AST2600 SoC bank.
    if (const auto base = sysfs_gpiochip_base_aspeed(); base)
    {
        std::cerr << std::format("switch-spc6-ast2600-bmc: Aspeed GPIO base "
                                 "{}\n",
                                 *base);
        const int a = *base;
        pins.push_back({a + 27, SysfsGpioDir::Out, 1, "BMC_UART_TX"}); // GPIOD3
        pins.push_back(
            {a + 96, SysfsGpioDir::Out, 1, "BMC_EP_PERST_EN-O"});      // GPIOM0
        pins.push_back(
            {a + 100, SysfsGpioDir::In, std::nullopt, "GP_STBY_PG"});  // GPIOM4
        pins.push_back({a + 223, SysfsGpioDir::Out, 1, "GP_BMC_PWR_CYCLE_1V8"});
        pins.push_back(
            {a + 224, SysfsGpioDir::Out, 0, "GP_BMC_PWR_CYCLE_1V8_MASK_L"});

        // $BMC_STBY_READY, also derived from the discovered base in the shell.
        stby_ready = sysfs_gpio_value_path(a + 100);
    }
    else
    {
        std::cerr << "switch-spc6-ast2600-bmc: failed to detect the Aspeed "
                     "GPIO chip base\n";
    }

    return pins;
}

// ── Platform init ─────────────────────────────────────────────────────────

int init_switch_spc6_ast2600_bmc()
{
    using namespace std::chrono_literals;

    std::cerr << "switch-spc6-ast2600-bmc: platform init start\n";

    // Source: spc6-ast2600-bmc_ready.sh:426 `run_hook pre`
    run_nos_hook("pre");

    // The block below is bmc_init_main, whose `trap bmc_init_exit_handler
    // EXIT` ran the post hook on every path out of the subshell.
    {
        NosHookGuard nos_hook;

        // Source: get_system_hw_id, which on this platform runs BEFORE
        //         bmc_init_sysfs_gpio because the GPIO code reads
        //         $config_path/hid to pick offset 12's role.
        const auto hid =
            vpd_read_field(spc6_2600_vpd_eeprom, spc6_2600_vpd_hid_offset,
                           spc6_2600_vpd_hid_size);
        std::cerr << std::format("switch-spc6-ast2600-bmc: system hardware Id "
                                 "is {}\n",
                                 hid);
        hw_mgmt_write_config("hid", hid);

        // Source: bmc_init_sysfs_gpio
        std::string stby_ready;
        const auto pins = spc6_2600_gpio_pins(hid, stby_ready);
        sysfs_gpio_apply(pins);

        // Source: wait_platform_drv — hw-management publishes its runtime tree
        //         from a udev event, so it may not exist yet.
        hw_mgmt_wait_system_tree("switch-spc6-ast2600-bmc");

        // Source: the 15 check_n_link calls at the end of bmc_init_sysfs_gpio
        hw_mgmt_link_gpio(pins);
        std::cerr << "switch-spc6-ast2600-bmc: GPIO sysfs initialization "
                     "complete\n";

        hw_mgmt_init_eth("switch-spc6-ast2600-bmc");

        bmc_init_bootargs();

        // Source: bmc_init_main:442-449
        const bool cpu_off = hw_mgmt_take_i2c_when_cpu_off(
            spc6_2600_mlxreg_io_hwmon, "switch-spc6-ast2600-bmc");

        // ── bmc_ready_sequence (spc6-ast2600-bmc_ready.sh:361-394) ─────────

        // Source: `wait_bmc_standby_ready 10 1`. The shell discarded the
        //         result, so a timeout is not fatal.
        if (!stby_ready.empty())
        {
            wait_sysfs_value(stby_ready, "1", 10, 1000ms);
        }

        write_cpu_type("switch-spc6-ast2600-bmc");

        // Source: get_system_hw_bom. The record offset is not fixed here, so
        //         vpd_read_bom() scans for the `V<digit>-` marker.
        if (const auto bom =
                vpd_read_bom(spc6_2600_vpd_eeprom, hid, spc6_2600_bom_hids,
                             std::nullopt, spc6_2600_vpd_bom_size);
            !bom.empty())
        {
            std::cerr << std::format("switch-spc6-ast2600-bmc: system hardware "
                                     "BOM record is {}\n",
                                     bom);
            hw_mgmt_write_config("bom", bom);
        }

        // Source: get_system_hw_rev. Writes config/rev_id from SYS_FRU Board
        //         Extra (the two extras immediately before HID, e.g. 18a1).
        write_system_hw_rev(spc6_2600_vpd_eeprom, hid, spc6_2600_bom_hids,
                            "switch-spc6-ast2600-bmc");

        // Source: `a2d_leakage_config.sh` — 1261 lines of JSON-driven
        //         leak-detector configuration, kept in shell.
        shell_out("a2d_leakage_config.sh", "switch-spc6-ast2600-bmc");

        // Source: check_rw_filesystems + check_rofs. AST2600 is MTD-backed, so
        //         the MTD variant, and the result is fatal.
        if (filesystem_check_mtd() != EXIT_SUCCESS)
        {
            std::cerr << "switch-spc6-ast2600-bmc: FATAL: filesystem check "
                         "failed\n";
            return EXIT_FAILURE;
        }

        // Source: bmc_init_main:460-470. When the CPU is still down,
        //         hw-management-events.sh starts
        //         platform-init-i3c-bind.service on the CPU reset deassert
        //         instead.
        if (!cpu_off)
        {
            bind_i3c_controller();
        }

        // Source: bmc_init_main:472. The action comes from WRITE_PROTECT_MODE
        //         in /etc/platform_features.conf.
        unset_write_protect_on_boot();
    } // NosHookGuard: the post hook runs here, where the subshell exited

    sd_notify(0, "READY=1");

    // A no-op on switch — nothing in this file goes through gpio::set() — but
    // it keeps a future libgpiod line from leaking past init.
    gpio::release_all();

    // Source: `phosphor_log "bmc_ready.sh completed"` plus the separate
    //         log_event_console call, consolidated into one emitter.
    phosphor_log_all("platform init completed");

    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
