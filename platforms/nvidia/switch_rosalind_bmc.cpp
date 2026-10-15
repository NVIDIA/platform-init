// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform initialization daemon for meta-switch/meta-rosalind — machines
// nvl144-sw-bmc and nvl144-sw-surrogate-bmc (AST2600).
// Replaces rosalind_bmc_ready.sh plus the meta-rosalind bmc_ready_common.sh it
// sourced. GPIO numbers come from nvidia-rosalind-bmc-core.dtsi.
//
// The switch platforms are built on the Mellanox hw-management framework, so
// every line below is driven through a legacy /sys/class/gpio export that has
// to outlive init: hw-management reaches it through the
// /var/run/hw-management/system/<NAME> symlink tree. No line handled here may
// also be passed to the gpio:: (libgpiod) namespace — the two are separate
// kernel interfaces and are mutually exclusive per line.
//
// Deliberately not ported: the `.bmc-init-done` marker and its
// bmc_cartridge_resync_if_init_done re-entry branch, which guarded a shell
// re-invocation hazard a Type=notify daemon cannot have; `hw-management.sh
// start`, which stays in bmc-boot-complete.service's ExecStartPre because it
// ends in a nested `systemctl start` and an `udevadm settle`; `reset_mcu` and
// `set_bmc_ready $HIGH`, commented out in the shell; and
// `remove_ipmitools_permissions`, which has no caller.

#include "platform_utils.hpp"
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

// ── Platform config ───────────────────────────────────────────────────────
// Source: rosalind_bmc_ready.sh:20-53 and the meta-rosalind
//         hw-management-helpers.sh:64-69 it sources.

static constexpr std::string_view rosalind_vpd_eeprom =
    "/sys/devices/platform/ahb/1e780000.apb/1e780000.apb:bus@1e78a000/"
    "1e78a280.i2c/i2c-4/4-0051/eeprom";

static constexpr size_t rosalind_vpd_hid_offset = 112;
static constexpr size_t rosalind_vpd_hid_size = 5;
static constexpr size_t rosalind_vpd_bom_offset = 118;
static constexpr size_t rosalind_vpd_bom_size = 192;

// $BMC_CPU_PWR_ON / $BMC_CPU_PWR_ON_BUT / $BMC_TO_CPU_CTRL, all of which the
// shell spelled with a `hwmon*` glob because the mlxreg-io hwmon instance
// number is assigned at probe time.
static constexpr std::string_view rosalind_mlxreg_io_hwmon =
    "/sys/devices/platform/ahb/1e780000.apb/1e780000.apb:bus@1e78a000/"
    "1e78a300.i2c/i2c-5/5-0031/mlxreg-io/hwmon";

// Source: get_system_hw_bom's HID case list — the HIDs that carry a BOM
//         record. Any other HID gets no bom file.
static constexpr std::array<std::string_view, 8> rosalind_bom_hids{
    "HI166", "HI167", "HI169", "HI170", "HI176", "HI177", "HI180", "HI185",
};

// Source: bmc_init_sysfs_gpio (rosalind_bmc_ready.sh:128-213). Expander banks
//         0_0..0_7 are 756-763 and 1_0..1_7 are 764-771; the rest are Aspeed
//         SoC lines.
//
// Not exported here, matching the shell's commented-out lines:
//   * 766 (1_2 S_BMC_GPIO_PHY_RST_GPIO_L), 530 (GPIOC2) and 610 (GPIOM2) —
//     driven from the device tree instead;
//   * GP_BMC_MCU{1,2}_{RESET,RECOVERY}_L — fw-status requests these through
//     libgpiod for MCU recovery, and a sysfs export holds the same lines,
//     which makes that fail with "Device or resource busy".
static const std::array<SysfsGpioPin, 20> rosalind_sysfs_gpios{{
    {756, SysfsGpioDir::Out, 1, "GP_USB_HUB_RTS_L_R"}, // resets the USB hub
    {757, SysfsGpioDir::In, std::nullopt, "GP_EROT_RST_IN_L"},
    {758, SysfsGpioDir::In, std::nullopt, "GP_3V3_BMC_PHY_PG"},
    {759, SysfsGpioDir::Out, 1, "GP_BMC_PWR_CYCLE_3V3_MASK_L"},
    {760, SysfsGpioDir::In, std::nullopt, "NC"},
    {761, SysfsGpioDir::In, std::nullopt, "GP_EROT_SOC_RESET_L"},
    {762, SysfsGpioDir::In, std::nullopt, "SE_SOC_EROT_BOOT_COMPLETE"},
    // Direction only: the shell leaves write protect at its reset level here
    // and clears it later through SetWriteProtectInit instead.
    {763, SysfsGpioDir::Out, std::nullopt, "GP_BMC_WP_CTRL_GPIO_L"},
    {764, SysfsGpioDir::In, std::nullopt, "GP_BMC_SOC_FLASH1_SPI_SEL"},
    // Recovery drives this low out-of-band and returns it to input.
    {765, SysfsGpioDir::In, std::nullopt, "GP_BMC_SOC_FLASH0_SPI_SEL"},
    {767, SysfsGpioDir::Out, 1, "GP_JTAG_BMC_CPLD_BURN_EN_L"},
    {768, SysfsGpioDir::In, std::nullopt, "GP_PWR_BANK_PRESENT_L"},
    {769, SysfsGpioDir::In, std::nullopt, "GP_PWR_BANK_PWR_KILL_L"},
    {770, SysfsGpioDir::In, std::nullopt, "GP_PWR_BANK_MCU_RESET_L"},
    {771, SysfsGpioDir::In, std::nullopt, "GP_PWR_BANK_MCU_RECOVERY_L"},
    {539, SysfsGpioDir::Out, 1, "BMC_UART_TX"},          // GPIOD3
    {608, SysfsGpioDir::Out, 1, "BMC_EP_PERST_EN-O"},    // GPIOM0
    {612, SysfsGpioDir::In, std::nullopt, "GP_STBY_PG"}, // GPIOM4
    {735, SysfsGpioDir::Out, 1, "GP_BMC_PWR_CYCLE_1V8"},
    {736, SysfsGpioDir::Out, 0, "GP_BMC_PWR_CYCLE_1V8_MASK_L"},
}};

// $BMC_STBY_READY — GPIOM4, polled by wait_bmc_standby_ready.
static constexpr int rosalind_gpio_stby_pg = 612;

// Source: `echo 24c512 0x51 > /sys/bus/i2c/devices/i2c-4/new_device`
//         (bmc_ready_sequence). Rosalind's bmc-early-i2c-devices.json now
//         creates this device from bmc-early-i2c-init.service as well;
//         eeprom_init() is idempotent and remains as a fallback.
static constexpr std::array<EepromDevice, 1> rosalind_vpd_device{{
    {4, 0x51, "24c512", "BMC VPD", false},
}};

// ── Cartridge FRU EEPROM -> switch-board CPLD ─────────────────────────────
// Source: meta-rosalind bmc_ready_common.sh — find_cartridge_sn_offset,
//         find_cartridge_chassis_custom_info_offset, topology_name,
//         update_cartrigde_data, log_cpld_cartridge_data,
//         sync_cartridge_data_to_cpld
//
// The cartridge carries an IPMI FRU image read at a one-byte offset
// (`i2ctransfer -f -y <bus> w1@<addr> <off> r<len>`); the switch-board CPLD is
// addressed by a two-byte (MSB, LSB) register pair. Both need
// i2c::raw_transfer(), for the reason the shell passed -f: at24 owns the
// cartridge EEPROM and mlxreg-io the CPLD, so i2c::RawDevice fails its
// I2C_SLAVE ioctl with EBUSY before any transfer is attempted. The FRU reads
// are all short enough that an SMBus block read would otherwise have served;
// the CPLD's two-byte register pair has no SMBus equivalent at all.

struct FruCartridgeSource
{
    unsigned bus;             // CARTRIDGE_<n>_I2C_BUS
    uint8_t address;          // CARTRIDGE_I2C_ADDRESS
    size_t rack_id_size = 13; // CARTRIDGE_RACK_ID_SIZE
};

struct FruCartridgeCpldMap
{
    unsigned bus;                  // SWB_I2C_BUS
    uint8_t address;               // SWB_I2C_ADDRESS
    uint8_t msb_offset;            // SWB_CARTRIDGE_MSB_OFFSET
    uint8_t lsb_offset;            // SWB_CARTRIDGE_<n>_LSB_OFFSET
    uint8_t rack_id_offset;        // SWB_CARTRIDGE_RACK_ID_OFFSET
    uint8_t topology_id_offset;    // SWB_TOPOLOGY_ID_OFFSET
    uint8_t switch_tray_id_offset; // SWB_SWITCH_TRAY_ID_OFFSET
    uint8_t slot_index_offset;     // SWB_SLOT_INDEX_OFFSET
};

struct FruCartridgeIdentity
{
    std::vector<uint8_t> rack_id;
    uint8_t topology_id = 0;
    uint8_t switch_tray_id = 0;
    uint8_t slot_index = 0;
};

// Source: rosalind_bmc_ready.sh:41-53 — cartridge 1's FRU EEPROM and the
//         switch-board CPLD window it is programmed into. Cartridge 2
//         (bus 34, LSB 0x20) has no caller in the shell either.
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

// One-byte-addressed FRU read: i2ctransfer -f -y <bus> w1@<addr> <off> r<len>.
// The FRU walk computes offsets arithmetically, so reject anything the
// one-byte address cannot express rather than truncating it silently —
// i2ctransfer would have refused the same argument.
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

// Source: the `i2cget -f -y <bus> <addr>` presence probe at the top of
//         update_cartrigde_data.
static bool fru_present(unsigned bus, uint8_t address)
{
    std::array<uint8_t, 1> probe{};
    return i2c::raw_transfer(bus, address, std::span<const uint8_t>{}, probe) ==
           0;
}

// Source: find_cartridge_sn_offset
// The rack id lives in the Board Serial Number field: read the product-name
// length, step over the PN area (len * 8 + 13), read that field's type/length
// byte, mask off the name length (0x3f) and skip the type/length pair (+2).
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

// Source: find_cartridge_chassis_custom_info_offset
// FRU common-header byte 2 holds the Chassis Info Area offset in 8-byte
// multiples, and 0 means the area is absent. Inside the area, skip the 3-byte
// header and the two fixed fields, then take the custom field. The NVL144
// GBCDB block puts slot at +2, tray at +3 and topology at +4 of the custom
// data, so a field shorter than 5 bytes is rejected.
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
    return offset + 1; // skip the type/length byte; callers use the data start
}

// Source: topology_name()
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

// Source: the EEPROM read half of update_cartrigde_data.
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

// Source: cpld_read() — i2ctransfer -f -y <bus> w2@<addr> <MSB> <LSB+field>
//         r<len>.
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

// Source: the write-then-read-back-verify blocks in update_cartrigde_data.
// payload is written verbatim — the caller has already padded it — and only
// its first verify_length bytes are compared, which is what the shell does for
// the 16-byte-padded rack id.
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

// Source: log_cpld_cartridge_data — a best-effort read-back of the whole
//         cartridge identity window, for field diagnosis. Never fails init.
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

    // bytes_to_ascii(): stop at the first 0x00.
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

// Source: update_cartrigde_data
// Returns EXIT_SUCCESS on success, or when an absent cartridge is tolerated;
// EXIT_FAILURE on any I2C, parse or read-back-verify failure. The explicit
// return code matters: before nvbug 6169868 this path returned bare and
// reported success.
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

    // The rack id is written as <MSB> <LSB+field> <rack id…>, zero-padded so
    // the whole transfer is 16 bytes; the read-back compares the rack id only.
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

// Source: sync_cartridge_data_to_cpld — 5 attempts, 2 s apart.
// The caller must have established that the host CPU is off, i.e. that the BMC
// owns the I2C tree.
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

// ── Platform-local steps ──────────────────────────────────────────────────

// Source: bmc_init_main's
//   busctl call xyz.openbmc_project.Software.Settings <object_path>
//       xyz.openbmc_project.Software.Settings SetWriteProtectInit b <value>
// Only Rosalind still does this, clearing it on Chassis_0 so the ERoT
// background copy can run. The AST2600 and AST2700 calls that set it went away
// with the platform_features.conf policy above. Separate from
// enable_write_protect_on_boot.service, which stays a unit.
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

// Source: bmc_ready_common.sh:1-31 bmc_init_usb_smbus_bridge
// Never fails init: without the bridge only ASIC firmware recovery over
// USB-SMBus is unavailable.
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

// ── Platform init ─────────────────────────────────────────────────────────

int init_switch_rosalind_bmc()
{
    using namespace std::chrono_literals;

    std::cerr << "switch-rosalind-bmc: platform init start\n";

    // Source: rosalind_bmc_ready.sh:420 `run_hook pre`
    // A pre hook may rewrite the U-Boot command line through
    // bmc_set_extra_params.sh and reboot the BMC, so it runs before anything
    // else touches the hardware.
    run_nos_hook("pre");

    // The block below is bmc_init_main, whose `trap bmc_init_exit_handler
    // EXIT` ran the post hook on every path out of the subshell — including
    // each early `return 1`.
    {
        NosHookGuard nos_hook;

        // Source: bmc_init_sysfs_gpio, exports/directions/levels
        sysfs_gpio_apply(rosalind_sysfs_gpios);

        // Source: wait_platform_drv — hw-management publishes its runtime tree
        //         from a udev event, so it may not exist yet.
        hw_mgmt_wait_system_tree("switch-rosalind-bmc");

        // Source: the 20 check_n_link calls at the end of bmc_init_sysfs_gpio
        hw_mgmt_link_gpio(rosalind_sysfs_gpios);

        rosalind_init_usb_smbus_bridge();
        bmc_init_bootargs();

        // Source: bmc_init_main:433-439
        const bool cpu_off = hw_mgmt_take_i2c_when_cpu_off(
            rosalind_mlxreg_io_hwmon, "switch-rosalind-bmc");

        // Source: bmc_init_main:441-444 — set the leakage_conn_en attribute in
        //         the CPLD at 5-0x31, equivalent to
        //         `i2ctransfer -f -y 5 w3@0x31 0x25 0x39 0x01`.
        if (uboot_env_get("leak_rj45_en") == "1")
        {
            static constexpr std::array<uint8_t, 3> leakage_conn_en{
                0x25, 0x39, 0x01};
            if (const int rc = i2c::raw_transfer(5, 0x31, leakage_conn_en,
                                                 std::span<uint8_t>{});
                rc != 0)
            {
                std::cerr << std::format("switch-rosalind-bmc: leakage_conn_en "
                                         "write failed: {}\n",
                                         std::strerror(rc));
            }
        }

        // ── bmc_ready_sequence (rosalind_bmc_ready.sh:346-388) ────────────

        // Source: `wait_bmc_standby_ready 10 1` on GP_STBY_PG. The shell
        //         discarded the result, so a timeout is not fatal.
        wait_sysfs_value(sysfs_gpio_value_path(rosalind_gpio_stby_pg), "1", 10,
                         1000ms);

        write_cpu_type("switch-rosalind-bmc");

        eeprom_init(rosalind_vpd_device);

        // Source: get_system_hw_id. Its HID case statement has empty bodies —
        //         the per-HID blacklist and cartridge JSON selection happens
        //         in the /etc -> /run/plat_active symlink tree instead.
        const auto hid =
            vpd_read_field(rosalind_vpd_eeprom, rosalind_vpd_hid_offset,
                           rosalind_vpd_hid_size);
        std::cerr << std::format("switch-rosalind-bmc: system hardware Id is "
                                 "{}\n",
                                 hid);
        hw_mgmt_write_config("hid", hid);

        // Source: get_system_hw_bom. Rosalind's BOM record sits at a fixed
        //         offset, so no `V<digit>-` marker scan is needed here.
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

        // Source: get_system_hw_rev. Writes config/rev_id from SYS_FRU Board
        //         Extra (the two extras immediately before HID, e.g. 18a1).
        write_system_hw_rev(rosalind_vpd_eeprom, hid, rosalind_bom_hids,
                            "switch-rosalind-bmc");

        // Source: sync_cartridge_data_to_cpld, run only while the host CPU is
        //         down, i.e. while the BMC owns the I2C tree. Non-fatal: the
        //         shell warned and carried on to power the CPU.
        if (cpu_off && fru_sync_cartridge_to_cpld(rosalind_cartridge_1,
                                                  rosalind_cartridge_1_cpld) !=
                           EXIT_SUCCESS)
        {
            std::cerr << "switch-rosalind-bmc: cartridge CPLD sync failed; "
                         "continuing to power CPU\n";
        }

        // Source: `a2d_leakage_config.sh` — 1261 lines of JSON-driven
        //         leak-detector configuration, kept in shell.
        shell_out("a2d_leakage_config.sh", "switch-rosalind-bmc");

        // Source: check_rw_filesystems + check_rofs. Rosalind is MTD-backed,
        //         hence the MTD variant, and the result is fatal: never report
        //         ready on a BMC whose read-write filesystem is gone.
        if (filesystem_check_mtd() != EXIT_SUCCESS)
        {
            std::cerr << "switch-rosalind-bmc: FATAL: filesystem check "
                         "failed\n";
            return EXIT_FAILURE;
        }

        // Source: bmc_init_main:455-457. Rosalind CLEARS write protect so the
        //         background copy can run; enable_write_protect_on_boot.service
        //         sets it again later.
        std::cerr << "switch-rosalind-bmc: disabling write protect to allow "
                     "the background copy\n";
        dbus_set_write_protect_init("/xyz/openbmc_project/software/Chassis_0",
                                    false);
    } // NosHookGuard: the post hook runs here, where the subshell exited

    sd_notify(0, "READY=1");

    // A no-op on switch — nothing in this file goes through gpio::set() — but
    // it keeps a future libgpiod line from leaking past init.
    gpio::release_all();

    // Source: `phosphor_log "bmc_ready.sh completed"` plus the separate
    //         log_event_console call, consolidated into one emitter over the
    //         D-Bus log entry, the journal and the console.
    phosphor_log_all("platform init completed");

    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
