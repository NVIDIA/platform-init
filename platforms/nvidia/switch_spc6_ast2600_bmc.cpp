// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for spc6-ast2600-bmc.

#include "switch_common.hpp"
#include "switch_misc.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>

#include <array>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace nvidia
{

static constexpr std::string_view spc6_2600_vpd_eeprom =
    "/sys/devices/platform/ahb/1e780000.apb/1e780000.apb:bus@1e78a000/"
    "1e78a280.i2c/i2c-4/4-0051/eeprom";

// The HID lives at offset 22 here, not at Rosalind's 112.
static constexpr size_t spc6_2600_vpd_hid_offset = 22;
static constexpr size_t spc6_2600_vpd_hid_size = 5;
static constexpr size_t spc6_2600_vpd_bom_size = 192;

// The hwmon instance number is assigned at probe time.
static constexpr std::string_view spc6_2600_mlxreg_io_hwmon =
    "/sys/devices/platform/ahb/1e780000.apb/1e780000.apb:bus@1e78a000/"
    "1e78a300.i2c/i2c-5/5-0031/mlxreg-io/hwmon";

static constexpr std::array<std::string_view, 5> spc6_2600_bom_hids{
    "HI187", "HI188", "HI191", "HI193", "HI183",
};

// sysfs, not gpio::, because hw-management symlinks these lines.
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
        // Direction only; unset_write_protect_on_boot() sets the level.
        pins.push_back(
            {e + 7, SysfsGpioDir::Out, std::nullopt, "GP_BMC_WP_CTRL_GPIO_L"});
        // 0 gives JTAG access to the BIOS flashes; 1 only for recovery.
        pins.push_back(
            {e + 8, SysfsGpioDir::Out, 0, "GP_BMC_REC_SPI_MUX1_SEL"});
        // Offset 10 (PHY reset) is driven from the DTS.
        pins.push_back(
            {e + 11, SysfsGpioDir::Out, 1, "GP_JTAG_BMC_CPLD_BURN_EN_L"});

        // On HI188 offset 12 is a mux output with no symlink.
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

        stby_ready = sysfs_gpio_value_path(a + 100);
    }
    else
    {
        std::cerr << "switch-spc6-ast2600-bmc: failed to detect the Aspeed "
                     "GPIO chip base\n";
    }

    return pins;
}

// ARMv7_CPU as spelled in this machine's helpers copy.
constexpr std::array<std::string_view, 1> spc6_2600_decode_cpu_types{"0xC07"};

// Boards whose cable cartridge EEPROMs hw-management.sh start added.
constexpr std::array<CartridgeBoard, 4> spc6_2600_cartridge_boards{{
    {"Juliet", "HI176"},
    {"Rosalind", "HI180"},
    {"Spc6", "HI181"},
    {"Spc6", "HI182"},
}};

int init_switch_spc6_ast2600_bmc()
{
    using namespace std::chrono_literals;

    std::cerr << "switch-spc6-ast2600-bmc: platform init start\n";

    // The recovery slave first: the recovery path needs it most when a later
    // step fails.
    switch_misc::i2c_slave_setup_if_configured();

    // HID first: it picks offset 12's role in the GPIO setup.
    const auto hid = vpd_read_field(
        spc6_2600_vpd_eeprom, spc6_2600_vpd_hid_offset, spc6_2600_vpd_hid_size);
    std::cerr << std::format("switch-spc6-ast2600-bmc: system hardware Id "
                             "is {}\n",
                             hid);
    hw_mgmt_write_config("hid", hid);

    std::string stby_ready;
    const auto pins = spc6_2600_gpio_pins(hid, stby_ready);
    sysfs_gpio_apply(pins);

    hw_mgmt_wait_system_tree("switch-spc6-ast2600-bmc");

    hw_mgmt_link_gpio(pins);
    std::cerr << "switch-spc6-ast2600-bmc: GPIO sysfs initialization "
                 "complete\n";

    const bool cpu_off = hw_mgmt_take_i2c_when_cpu_off(
        spc6_2600_mlxreg_io_hwmon, "switch-spc6-ast2600-bmc");

    // A timeout here is not fatal.
    if (!stby_ready.empty())
    {
        wait_sysfs_value(stby_ready, "1", 10, 1000ms);
    }

    write_cpu_type("switch-spc6-ast2600-bmc");

    // The BOM offset is not fixed; vpd_read_bom() scans for it.
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

    write_system_hw_rev(spc6_2600_vpd_eeprom, hid, spc6_2600_bom_hids,
                        "switch-spc6-ast2600-bmc");

    shell_out("a2d_leakage_config.sh", "switch-spc6-ast2600-bmc");

    if (filesystem_check_mtd() != EXIT_SUCCESS)
    {
        std::cerr << "switch-spc6-ast2600-bmc: FATAL: filesystem check "
                     "failed\n";
        return EXIT_FAILURE;
    }

    // With the CPU off, I3C is bound on CPU reset deassert instead.
    if (!cpu_off)
    {
        bind_i3c_controller();
    }

    unset_write_protect_on_boot();

    hw_mgmt_boot_start(spc6_2600_cartridge_boards, spc6_2600_decode_cpu_types);

    sd_notify(0, "READY=1");

    phosphor_log_all("platform init completed");

    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
