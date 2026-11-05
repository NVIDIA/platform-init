// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for the Rosalind switch BMCs.

#include "switch_common.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <vector>

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

// raw_transfer, not RawDevice: at24 and mlxreg-io own these devices.

struct FruCartridgeSource
{
    unsigned bus;
    uint8_t address;
    size_t rack_id_size = 13;
};

struct FruCartridgeCpldMap
{
    unsigned bus;
    uint8_t address;
    uint8_t msb_offset;
    uint8_t lsb_offset;
    uint8_t rack_id_offset;
    uint8_t topology_id_offset;
    uint8_t switch_tray_id_offset;
    uint8_t slot_index_offset;
};

struct FruCartridgeIdentity
{
    std::vector<uint8_t> rack_id;
    uint8_t topology_id = 0;
    uint8_t switch_tray_id = 0;
    uint8_t slot_index = 0;
};

static constexpr FruCartridgeSource rosalind_cartridge_1{
    .bus = 33,
    .address = 0x50,
    .rack_id_size = 13,
};

static constexpr FruCartridgeCpldMap rosalind_cartridge_1_cpld{
    .bus = 24,
    .address = 0x31,
    .msb_offset = 0x10,
    .lsb_offset = 0x00,
    .rack_id_offset = 0x00,
    .topology_id_offset = 0x10,
    .switch_tray_id_offset = 0x11,
    .slot_index_offset = 0x12,
};

static std::optional<std::vector<uint8_t>> fru_read(
    unsigned bus, uint8_t address, unsigned offset, size_t length)
{
    if (offset > 0xff)
    {
        std::cerr << std::format(
            "fru_read: offset {} outside the one-byte FRU address range\n",
            offset);
        return std::nullopt;
    }
    const std::array<uint8_t, 1> command{static_cast<uint8_t>(offset)};
    std::vector<uint8_t> out(length);
    if (const int rc = i2c::raw_transfer(bus, address, command, out); rc != 0)
    {
        std::cerr << std::format(
            "fru_read: bus={} addr={:#x} off={:#x} len={}: {}\n", bus, address,
            offset, length, std::strerror(rc));
        return std::nullopt;
    }
    return out;
}

static bool fru_present(unsigned bus, uint8_t address)
{
    std::array<uint8_t, 1> probe{};
    return i2c::raw_transfer(bus, address, std::span<const uint8_t>{}, probe) ==
           0;
}

// The rack id lives in the Board Serial Number field.
static std::optional<unsigned> fru_sn_offset(const FruCartridgeSource& source)
{
    constexpr unsigned pn_length_offset = 3;
    constexpr uint8_t name_length_mask = 0x3f;
    constexpr unsigned type_length_pair = 2;

    const auto pn_length =
        fru_read(source.bus, source.address, pn_length_offset, 1);
    if (!pn_length)
    {
        std::cerr << std::format("fru_sn_offset: PN length read failed "
                                 "(bus={})\n",
                                 source.bus);
        return std::nullopt;
    }
    const unsigned pn_offset = (*pn_length)[0] * 8u + 13u;

    const auto pn_name = fru_read(source.bus, source.address, pn_offset, 1);
    if (!pn_name)
    {
        std::cerr << std::format("fru_sn_offset: PN name offset read failed "
                                 "(bus={} off={})\n",
                                 source.bus, pn_offset);
        return std::nullopt;
    }
    return pn_offset + ((*pn_name)[0] & name_length_mask) + type_length_pair;
}

// NVL144 GBCDB: slot, tray and topology at +2, +3 and +4 of the custom data.
static std::optional<unsigned> fru_chassis_custom_info_offset(
    const FruCartridgeSource& source)
{
    constexpr uint8_t field_length_mask = 0x3f;

    const auto header = fru_read(source.bus, source.address, 0x00, 8);
    if (!header)
    {
        std::cerr << "fru_chassis_custom_info_offset: FRU header read failed\n";
        return std::nullopt;
    }
    const unsigned chassis_offset = (*header)[2] * 8u;
    if (chassis_offset == 0)
    {
        std::cerr << "fru_chassis_custom_info_offset: chassis info area "
                     "missing (offset 0)\n";
        return std::nullopt;
    }
    if (!fru_read(source.bus, source.address, chassis_offset, 3))
    {
        std::cerr << "fru_chassis_custom_info_offset: chassis header read "
                     "failed\n";
        return std::nullopt;
    }

    unsigned offset = chassis_offset + 3;
    for (int field = 1; field <= 2; ++field)
    {
        const auto type_length =
            fru_read(source.bus, source.address, offset, 1);
        if (!type_length)
        {
            std::cerr << std::format("fru_chassis_custom_info_offset: field {} "
                                     "read failed\n",
                                     field);
            return std::nullopt;
        }
        offset += 1u + ((*type_length)[0] & field_length_mask);
    }

    const auto custom = fru_read(source.bus, source.address, offset, 1);
    if (!custom)
    {
        std::cerr << "fru_chassis_custom_info_offset: custom type/length read "
                     "failed\n";
        return std::nullopt;
    }
    const unsigned custom_length = (*custom)[0] & field_length_mask;
    if (custom_length < 5)
    {
        std::cerr << std::format("fru_chassis_custom_info_offset: custom info "
                                 "too short (len={}, need >= 5)\n",
                                 custom_length);
        return std::nullopt;
    }
    return offset + 1;
}

static std::string_view fru_topology_name(uint8_t topology_id)
{
    switch (topology_id)
    {
        case 0x80:
            return "18x1RU";
        case 0x81:
            return "9x2RU";
        case 0x00:
            return "Loopback/empty";
        default:
            return "unknown";
    }
}

static std::optional<FruCartridgeIdentity> fru_read_cartridge_identity(
    const FruCartridgeSource& source)
{
    if (source.rack_id_size < 1 || source.rack_id_size > 64)
    {
        std::cerr << std::format("fru_read_cartridge_identity: invalid rack id "
                                 "size {}\n",
                                 source.rack_id_size);
        return std::nullopt;
    }

    const auto sn_offset = fru_sn_offset(source);
    if (!sn_offset)
        return std::nullopt;
    const auto custom_offset = fru_chassis_custom_info_offset(source);
    if (!custom_offset)
        return std::nullopt;

    const auto rack_id =
        fru_read(source.bus, source.address, *sn_offset, source.rack_id_size);
    const auto topology_id =
        fru_read(source.bus, source.address, *custom_offset + 4, 1);
    const auto switch_tray_id =
        fru_read(source.bus, source.address, *custom_offset + 3, 1);
    const auto slot_index =
        fru_read(source.bus, source.address, *custom_offset + 2, 1);
    if (!rack_id || !topology_id || !switch_tray_id || !slot_index)
    {
        std::cerr << std::format("fru_read_cartridge_identity: cartridge "
                                 "EEPROM read failed (bus={})\n",
                                 source.bus);
        return std::nullopt;
    }

    return FruCartridgeIdentity{
        .rack_id = *rack_id,
        .topology_id = (*topology_id)[0],
        .switch_tray_id = (*switch_tray_id)[0],
        .slot_index = (*slot_index)[0],
    };
}

static std::optional<std::vector<uint8_t>> fru_cpld_read(
    const FruCartridgeCpldMap& cpld, uint8_t field_offset, size_t length)
{
    const std::array<uint8_t, 2> command{
        cpld.msb_offset, static_cast<uint8_t>(cpld.lsb_offset + field_offset)};
    std::vector<uint8_t> out(length);
    if (const int rc = i2c::raw_transfer(cpld.bus, cpld.address, command, out);
        rc != 0)
    {
        std::cerr << std::format("fru_cpld_read: bus={} off={:#04x} len={}: "
                                 "{}\n",
                                 cpld.bus, cpld.lsb_offset + field_offset,
                                 length, std::strerror(rc));
        return std::nullopt;
    }
    return out;
}

static bool fru_cpld_write_verify(
    const FruCartridgeCpldMap& cpld, uint8_t field_offset,
    std::span<const uint8_t> payload, size_t verify_length,
    std::string_view field_name)
{
    std::vector<uint8_t> command{
        cpld.msb_offset, static_cast<uint8_t>(cpld.lsb_offset + field_offset)};
    command.insert(command.end(), payload.begin(), payload.end());
    if (const int rc = i2c::raw_transfer(cpld.bus, cpld.address, command,
                                         std::span<uint8_t>{});
        rc != 0)
    {
        std::cerr << std::format(
            "fru_cpld_write_verify: CPLD {} write failed "
            "(bus={} off={:#04x}): {}\n",
            field_name, cpld.bus, cpld.lsb_offset + field_offset,
            std::strerror(rc));
        return false;
    }

    const auto readback = fru_cpld_read(cpld, field_offset, verify_length);
    if (!readback)
    {
        std::cerr << std::format("fru_cpld_write_verify: CPLD {} read-back "
                                 "failed\n",
                                 field_name);
        return false;
    }
    if (!std::ranges::equal(*readback, payload.first(verify_length)))
    {
        std::cerr << std::format("fru_cpld_write_verify: {} not pushed "
                                 "(bus={} off={:#04x})\n",
                                 field_name, cpld.bus,
                                 cpld.lsb_offset + field_offset);
        return false;
    }
    return true;
}

static void fru_cpld_log_cartridge_data(const FruCartridgeCpldMap& cpld,
                                        size_t rack_id_size)
{
    const auto rack_id = fru_cpld_read(cpld, cpld.rack_id_offset, rack_id_size);
    const auto topology_id = fru_cpld_read(cpld, cpld.topology_id_offset, 1);
    const auto switch_tray_id =
        fru_cpld_read(cpld, cpld.switch_tray_id_offset, 1);
    const auto slot_index = fru_cpld_read(cpld, cpld.slot_index_offset, 1);
    if (!rack_id || !topology_id || !switch_tray_id || !slot_index)
    {
        std::cerr << std::format("fru_cpld_log_cartridge_data: read failed "
                                 "(LSB={:#04x})\n",
                                 cpld.lsb_offset);
        return;
    }

    std::string raw;
    for (const uint8_t byte : *rack_id)
        raw += std::format("{}0x{:02x}", raw.empty() ? "" : " ", byte);

    std::string ascii;
    for (const uint8_t byte : *rack_id)
    {
        if (byte == 0x00)
            break;
        ascii.push_back(static_cast<char>(byte));
    }

    std::cerr << std::format(
        "fru_cpld_log_cartridge_data: cartridge CPLD dump (LSB={:#04x}): "
        "rack_id raw=[{}] ascii=[{}] topology_id={:#04x} ({}) "
        "tray_id={:#04x} ({}) slot_id={:#04x} ({})\n",
        cpld.lsb_offset, raw, ascii.empty() ? "<empty>" : ascii,
        (*topology_id)[0], fru_topology_name((*topology_id)[0]),
        (*switch_tray_id)[0], (*switch_tray_id)[0], (*slot_index)[0],
        (*slot_index)[0]);
}

// Returns an explicit code: a bare return here was nvbug 6169868.
static int fru_update_cartridge_data(const FruCartridgeSource& source,
                                     const FruCartridgeCpldMap& cpld,
                                     bool absent_is_ok = false)
{
    if (!fru_present(source.bus, source.address))
    {
        std::cerr << std::format("fru_update_cartridge_data: no cartridge "
                                 "EEPROM at bus {} address {:#x}\n",
                                 source.bus, source.address);
        return absent_is_ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    const auto identity = fru_read_cartridge_identity(source);
    if (!identity)
        return EXIT_FAILURE;

    const std::array<uint8_t, 1> switch_tray_id{identity->switch_tray_id};
    if (!fru_cpld_write_verify(cpld, cpld.switch_tray_id_offset, switch_tray_id,
                               1, "switch tray id"))
        return EXIT_FAILURE;

    const std::array<uint8_t, 1> slot_index{identity->slot_index};
    if (!fru_cpld_write_verify(cpld, cpld.slot_index_offset, slot_index, 1,
                               "slot index"))
        return EXIT_FAILURE;

    const std::array<uint8_t, 1> topology_id{identity->topology_id};
    if (!fru_cpld_write_verify(cpld, cpld.topology_id_offset, topology_id, 1,
                               "topology id"))
        return EXIT_FAILURE;

    // Zero-padded to a 16-byte transfer; only the rack id is verified.
    std::vector<uint8_t> rack_id = identity->rack_id;
    if (const size_t framed = 2 + rack_id.size(); framed < 16)
        rack_id.resize(rack_id.size() + (16 - framed), 0x00);
    if (!fru_cpld_write_verify(cpld, cpld.rack_id_offset, rack_id,
                               identity->rack_id.size(), "rack id"))
        return EXIT_FAILURE;

    std::cerr << std::format("fru_update_cartridge_data: bus {} EEPROM data "
                             "pushed to CPLD LSB {:#04x}\n",
                             source.bus, cpld.lsb_offset);
    fru_cpld_log_cartridge_data(cpld, identity->rack_id.size());
    return EXIT_SUCCESS;
}

// Only while the host CPU is off, i.e. the BMC owns the I2C tree.
static int fru_sync_cartridge_to_cpld(
    const FruCartridgeSource& source, const FruCartridgeCpldMap& cpld,
    int retries = 5, std::chrono::seconds retry_delay = std::chrono::seconds{2})
{
    std::cerr << "fru_sync_cartridge_to_cpld: syncing cartridge EEPROM "
                 "identity to switch-board CPLD\n";
    for (int attempt = 1; attempt <= retries; ++attempt)
    {
        if (const int rc = fru_update_cartridge_data(source, cpld);
            rc == EXIT_SUCCESS)
            return EXIT_SUCCESS;
        std::cerr << std::format("fru_sync_cartridge_to_cpld: cartridge CPLD "
                                 "update failed, attempt {}/{}\n",
                                 attempt, retries);
        if (attempt < retries)
            std::this_thread::sleep_for(retry_delay);
    }
    std::cerr << std::format("fru_sync_cartridge_to_cpld: update cartridge "
                             "CPLD failed after {} attempts\n",
                             retries);
    return EXIT_FAILURE;
}

static bool dbus_set_write_protect_init(std::string_view object_path,
                                        bool value)
{
    try
    {
        auto bus = sdbusplus::bus::new_default_system();
        auto m = bus.new_method_call(
            "xyz.openbmc_project.Software.Settings",
            std::string{object_path}.c_str(),
            "xyz.openbmc_project.Software.Settings", "SetWriteProtectInit");
        m.append(value);
        bus.call(m);
        std::cerr << std::format(
            "dbus_set_write_protect_init: {} SetWriteProtectInit {}\n",
            object_path, value);
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << std::format("dbus_set_write_protect_init: {}: {}\n",
                                 object_path, e.what());
        return false;
    }
}

// Non-fatal: only ASIC firmware recovery over USB-SMBus needs it.
static void rosalind_init_usb_smbus_bridge()
{
    std::cerr << "switch-rosalind-bmc: loading hid_cp2112 for the USB-I2C "
                 "bridge (ASIC firmware recovery)\n";
    if (modprobe_with_timeout("hid_cp2112", std::chrono::seconds{5}) != 0)
    {
        return;
    }
    if (std::filesystem::exists("/dev/hidraw0") &&
        std::filesystem::exists("/dev/hidraw1"))
    {
        std::cerr << "switch-rosalind-bmc: USB-I2C bridge devices created: "
                     "/dev/hidraw0, /dev/hidraw1\n";
    }
    else
    {
        std::cerr << "switch-rosalind-bmc: hid_cp2112 loaded but "
                     "/dev/hidraw{0,1} not found\n";
    }
}

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

    rosalind_init_usb_smbus_bridge();

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

    if (cpu_off &&
        fru_sync_cartridge_to_cpld(rosalind_cartridge_1,
                                   rosalind_cartridge_1_cpld) != EXIT_SUCCESS)
    {
        std::cerr << "switch-rosalind-bmc: cartridge CPLD sync failed; "
                     "continuing to power CPU\n";
    }

    if (filesystem_check_mtd() != EXIT_SUCCESS)
    {
        std::cerr << "switch-rosalind-bmc: FATAL: filesystem check "
                     "failed\n";
        return EXIT_FAILURE;
    }

    // enable_write_protect_on_boot.service sets it again later.
    std::cerr << "switch-rosalind-bmc: disabling write protect to allow "
                 "the background copy\n";
    // Not fatal: the copy then runs with write protect as it was.
    if (!dbus_set_write_protect_init("/xyz/openbmc_project/software/Chassis_0",
                                     false))
    {
        std::cerr << "switch-rosalind-bmc: write protect left unchanged; "
                     "continuing\n";
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
