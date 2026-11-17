// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for the Rosalind switch BMCs.

#include "switch_cartridge.hpp"
#include "switch_common.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>

namespace nvidia
{

static constexpr std::string_view rosalind_vpd_eeprom =
    "/sys/devices/platform/ahb/1e780000.apb/1e780000.apb:bus@1e78a000/"
    "1e78a280.i2c/i2c-4/4-0051/eeprom";

static constexpr size_t rosalind_vpd_hid_offset = 112;
static constexpr size_t rosalind_vpd_hid_size = 5;
static constexpr size_t rosalind_vpd_bom_offset = 118;
static constexpr size_t rosalind_vpd_bom_size = 192;

// The hwmon instance number is assigned at probe time.
static constexpr std::string_view rosalind_mlxreg_io_hwmon =
    "/sys/devices/platform/ahb/1e780000.apb/1e780000.apb:bus@1e78a000/"
    "1e78a300.i2c/i2c-5/5-0031/mlxreg-io/hwmon";

static constexpr std::array<std::string_view, 8> rosalind_bom_hids{
    "HI166", "HI167", "HI169", "HI170", "HI176", "HI177", "HI180", "HI185",
};

// sysfs, not gpio::, because hw-management symlinks these lines.
// MCU reset/recovery lines are left to fw-status, which uses libgpiod.
static const std::array<SysfsGpioPin, 20> rosalind_sysfs_gpios{{
    {756, SysfsGpioDir::Out, 1, "GP_USB_HUB_RTS_L_R"},
    {757, SysfsGpioDir::In, std::nullopt, "GP_EROT_RST_IN_L"},
    {758, SysfsGpioDir::In, std::nullopt, "GP_3V3_BMC_PHY_PG"},
    {759, SysfsGpioDir::Out, 1, "GP_BMC_PWR_CYCLE_3V3_MASK_L"},
    {760, SysfsGpioDir::In, std::nullopt, "NC"},
    {761, SysfsGpioDir::In, std::nullopt, "GP_EROT_SOC_RESET_L"},
    {762, SysfsGpioDir::In, std::nullopt, "SE_SOC_EROT_BOOT_COMPLETE"},
    // Direction only; cleared later through SetWriteProtectInit.
    {763, SysfsGpioDir::Out, std::nullopt, "GP_BMC_WP_CTRL_GPIO_L"},
    {764, SysfsGpioDir::In, std::nullopt, "GP_BMC_SOC_FLASH1_SPI_SEL"},
    // Recovery drives this low out-of-band and returns it to input.
    {765, SysfsGpioDir::In, std::nullopt, "GP_BMC_SOC_FLASH0_SPI_SEL"},
    {767, SysfsGpioDir::Out, 1, "GP_JTAG_BMC_CPLD_BURN_EN_L"},
    {768, SysfsGpioDir::In, std::nullopt, "GP_PWR_BANK_PRESENT_L"},
    {769, SysfsGpioDir::In, std::nullopt, "GP_PWR_BANK_PWR_KILL_L"},
    {770, SysfsGpioDir::In, std::nullopt, "GP_PWR_BANK_MCU_RESET_L"},
    {771, SysfsGpioDir::In, std::nullopt, "GP_PWR_BANK_MCU_RECOVERY_L"},
    {539, SysfsGpioDir::Out, 1, "BMC_UART_TX"},
    {608, SysfsGpioDir::Out, 1, "BMC_EP_PERST_EN-O"},
    {612, SysfsGpioDir::In, std::nullopt, "GP_STBY_PG"},
    {735, SysfsGpioDir::Out, 1, "GP_BMC_PWR_CYCLE_1V8"},
    {736, SysfsGpioDir::Out, 0, "GP_BMC_PWR_CYCLE_1V8_MASK_L"},
}};

static constexpr int rosalind_gpio_stby_pg = 612;

// CPLD leakage_conn_en at 5-0x31: 16-bit register offset, then the value.
static constexpr unsigned rosalind_leak_cpld_bus = 5;
static constexpr uint8_t rosalind_leak_cpld_addr = 0x31;
static constexpr uint16_t rosalind_leakage_conn_en_reg = 0x2539;
static constexpr uint8_t rosalind_leakage_conn_en_on = 0x01;

static constexpr std::string_view rosalind_cartridge_config =
    "/etc/hw-mgmt-bmc-copy-cartridge-data.json";

// ARMv7_CPU as spelled in this machine's helpers copy.
constexpr std::array<std::string_view, 1> rosalind_decode_cpu_types{"0xC07"};

// Boards whose cable cartridge EEPROMs hw-management.sh start added.
constexpr std::array<CartridgeBoard, 5> rosalind_cartridge_boards{{
    {"Juliet", "HI176"},
    {"Rosalind", "HI180"},
    {"Rosalind", "HI185"},
    {"Spc6", "HI181"},
    {"Spc6", "HI182"},
}};

int init_switch_rosalind_bmc()
{
    using namespace std::chrono_literals;

    std::cerr << "switch-rosalind-bmc: platform init start\n";

    sysfs_gpio_apply(rosalind_sysfs_gpios);

    hw_mgmt_wait_system_tree("switch-rosalind-bmc");

    hw_mgmt_link_gpio(rosalind_sysfs_gpios);

    const bool cpu_off = hw_mgmt_take_i2c_when_cpu_off(rosalind_mlxreg_io_hwmon,
                                                       "switch-rosalind-bmc");

    if (uboot_env_get("leak_rj45_en") == "1")
    {
        static constexpr std::array<uint8_t, 3> leakage_conn_en{
            static_cast<uint8_t>(rosalind_leakage_conn_en_reg >> 8),
            static_cast<uint8_t>(rosalind_leakage_conn_en_reg & 0xff),
            rosalind_leakage_conn_en_on};
        if (const int rc = i2c::raw_transfer(
                rosalind_leak_cpld_bus, rosalind_leak_cpld_addr,
                leakage_conn_en, std::span<uint8_t>{});
            rc != 0)
        {
            std::cerr << std::format("switch-rosalind-bmc: leakage_conn_en "
                                     "write failed: {}\n",
                                     std::strerror(rc));
        }
    }

    // A timeout here is not fatal.
    wait_sysfs_value(sysfs_gpio_value_path(rosalind_gpio_stby_pg), "1", 10,
                     1000ms);

    write_cpu_type("switch-rosalind-bmc");

    const auto hid = vpd_read_field(
        rosalind_vpd_eeprom, rosalind_vpd_hid_offset, rosalind_vpd_hid_size);
    std::cerr << std::format("switch-rosalind-bmc: system hardware Id is "
                             "{}\n",
                             hid);
    hw_mgmt_write_config("hid", hid);

    if (const auto bom =
            vpd_read_bom(rosalind_vpd_eeprom, hid, rosalind_bom_hids,
                         rosalind_vpd_bom_offset, rosalind_vpd_bom_size);
        !bom.empty())
    {
        std::cerr << std::format("switch-rosalind-bmc: system hardware BOM "
                                 "record is {}\n",
                                 bom);
        hw_mgmt_write_config("bom", bom);
    }

    write_system_hw_rev(rosalind_vpd_eeprom, hid, rosalind_bom_hids,
                        "switch-rosalind-bmc");

    // Only while the host CPU is not driving the I2C tree.
    if (cpu_off && std::filesystem::exists(rosalind_cartridge_config))
    {
        copy_cartridge_data(std::string{rosalind_cartridge_config});
    }

    if (filesystem_check_mtd() != EXIT_SUCCESS)
    {
        std::cerr << "switch-rosalind-bmc: FATAL: filesystem check "
                     "failed\n";
        return EXIT_FAILURE;
    }

    hw_mgmt_boot_start(rosalind_cartridge_boards, rosalind_decode_cpu_types);

    sd_notify(0, "READY=1");

    phosphor_log_all("platform init completed");

    recover_external_rtc("switch-rosalind-bmc");

    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
