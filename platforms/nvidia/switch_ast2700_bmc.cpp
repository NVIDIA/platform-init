// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for the switch AST2700 BMCs.
//
// On VRU (HID194/199/200) the platform driver is held back until every CPLD
// root-of-trust MCU reports its CPLD streamed; see the CPLD-ready gate below.

#include "i2c.hpp"
#include "switch_common.hpp"
#include "switch_mcu.hpp"
#include "switch_misc.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace nvidia
{

static constexpr std::string_view ast2700_vpd_eeprom =
    "/sys/devices/platform/soc@14000000/soc@14000000:bus@14c0f000/"
    "14c0f600.i2c-bus/i2c-5/5-0051/eeprom";

static constexpr size_t ast2700_vpd_hid_offset = 22;
static constexpr size_t ast2700_vpd_hid_size = 5;
static constexpr size_t ast2700_vpd_bom_size = 192;

// The CPLD is on i2c-14 here, not i2c-5 as on the AST2600 boards.
static constexpr std::string_view ast2700_mlxreg_io_hwmon =
    "/sys/devices/platform/soc@14000000/soc@14000000:bus@14c0f000/"
    "14c0ff00.i2c-bus/i2c-14/14-0031/mlxreg-io/hwmon";

static constexpr std::array<std::string_view, 11> ast2700_bom_hids{
    "HI186", "HI187", "HI188", "HI189", "HI190", "HI191",
    "HI193", "HI194", "HI183", "HI195", "HI197",
};

static constexpr std::string_view ast2700_cartridge_config =
    "/etc/hw-mgmt-bmc-copy-cartridge-data.json";

struct GpioPinConfig
{
    std::string bmc_stby_ready;
    std::vector<SysfsGpioPin> pins;
};

// The path is per-HID via the /etc -> /run/plat_active symlink tree.
static std::optional<GpioPinConfig> load_gpio_pin_config(
    std::string_view path = "/etc/hw-management-bmc-gpio-pins.json")
{
    const std::string file{path};
    if (!std::filesystem::exists(file))
    {
        std::cerr << std::format(
            "load_gpio_pin_config: GPIO pins config missing: {}\n", file);
        return std::nullopt;
    }

    try
    {
        std::ifstream f{file};
        if (!f)
        {
            std::cerr << std::format("load_gpio_pin_config: cannot open {}\n",
                                     file);
            return std::nullopt;
        }
        const auto doc = nlohmann::json::parse(f, nullptr, false);
        if (doc.is_discarded() || !doc.is_object())
        {
            std::cerr << std::format("load_gpio_pin_config: invalid JSON: {}\n",
                                     file);
            return std::nullopt;
        }

        std::map<std::string, std::optional<int>> bases;
        const auto base_for = [&bases](const std::string& chip) {
            const auto [it, inserted] = bases.try_emplace(chip, std::nullopt);
            if (inserted)
                it->second = sysfs_gpiochip_base_for_chip_id(chip);
            return it->second;
        };

        GpioPinConfig config;

        if (const auto stby = doc.find("bmc_stby_ready");
            stby != doc.end() && stby->is_object())
        {
            const auto chip = stby->value("chip", std::string{});
            const auto offset = stby->value("offset", -1);
            if (!chip.empty() && offset >= 0)
            {
                if (const auto base = base_for(chip); base)
                    config.bmc_stby_ready =
                        sysfs_gpio_value_path(*base + offset);
            }
        }

        const auto pins = doc.find("pins");
        if (pins == doc.end() || !pins->is_array())
        {
            std::cerr << std::format("load_gpio_pin_config: no pins array in "
                                     "{}\n",
                                     file);
            return std::nullopt;
        }

        int index = -1;
        for (const auto& pin : *pins)
        {
            ++index;
            if (!pin.is_object())
            {
                std::cerr << std::format(
                    "load_gpio_pin_config: pins[{}] is not an object, skip\n",
                    index);
                continue;
            }
            const auto chip = pin.value("chip", std::string{});
            const auto direction = pin.value("direction", std::string{});
            const auto offset = pin.value("offset", -1);
            if (chip.empty() || direction.empty() || offset < 0)
            {
                std::cerr << std::format("load_gpio_pin_config: pins[{}] "
                                         "missing chip/offset/direction, "
                                         "skip\n",
                                         index);
                continue;
            }
            const auto base = base_for(chip);
            if (!base)
                continue;

            SysfsGpioPin entry{};
            entry.number = *base + offset;
            entry.symlink = pin.value("symlink", std::string{});
            if (direction == "in")
                entry.direction = SysfsGpioDir::In;
            else if (direction == "out")
                entry.direction = SysfsGpioDir::Out;
            else if (direction == "preserve")
                entry.direction = SysfsGpioDir::Preserve;
            else
            {
                std::cerr << std::format("load_gpio_pin_config: pins[{}] has "
                                         "unsupported direction: {}\n",
                                         index, direction);
                entry.direction = SysfsGpioDir::Preserve;
            }
            if (entry.direction == SysfsGpioDir::Out && pin.contains("value"))
                entry.value = pin.value("value", 0);

            config.pins.push_back(std::move(entry));
        }

        std::cerr << std::format("load_gpio_pin_config: {}: {} pins\n", file,
                                 config.pins.size());
        return config;
    }
    catch (const std::exception& e)
    {
        std::cerr << std::format("load_gpio_pin_config: {}: {}\n", file,
                                 e.what());
        return std::nullopt;
    }
}

// Source: hw-management-bmc-copy-cartridge-data.sh. Copies the primary
// cartridge FRU's rack/topology/tray/slot ids into the switch-board CPLD.
struct CartridgeConfig
{
    std::string access;
    unsigned bus = 0;
    uint8_t addr = 0;
    unsigned swb_bus = 0;
    uint8_t swb_addr = 0;
    unsigned msb = 0;
    unsigned lsb = 0;
    unsigned rack_off = 0;
    unsigned topo_off = 0;
    unsigned tray_off = 0;
    unsigned slot_off = 0;
    size_t rack_size = 13;
};

// Quoted "0xNN" or decimal; anything else is 0, as in the shell.
static unsigned cartridge_json_int(const nlohmann::json& doc,
                                   const std::string& key)
{
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_string())
        return 0;
    const auto& v = it->get_ref<const std::string&>();
    const auto b = v.find_first_not_of(" \t");
    if (b == std::string::npos)
        return 0;
    const auto t = v.substr(b, v.find_last_not_of(" \t") + 1 - b);
    const bool hex = t.starts_with("0x") || t.starts_with("0X");
    unsigned out = 0;
    std::from_chars(t.data() + (hex ? 2 : 0), t.data() + t.size(), out,
                    hex ? 16 : 10);
    return out;
}

static std::optional<CartridgeConfig> load_cartridge_config(
    const std::string& path)
{
    std::ifstream f{path};
    const auto doc = nlohmann::json::parse(f, nullptr, false);
    if (doc.is_discarded() || !doc.is_object())
    {
        std::cerr << std::format("cartridge: invalid JSON: {}\n", path);
        return std::nullopt;
    }
    const auto num = [&doc](const char* key) {
        const auto it = doc.find(key);
        return it != doc.end() && it->is_number_unsigned()
                   ? std::optional{it->get<unsigned>()}
                   : std::nullopt;
    };
    CartridgeConfig c;
    c.access = doc.value("PhysicalAccess", std::string{});
    std::ranges::transform(c.access, c.access.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    c.bus = num("PrimaryCartridgeI2cBus")
                .value_or(num("Cartridge1I2cBus").value_or(0));
    c.addr =
        static_cast<uint8_t>(cartridge_json_int(doc, "CartridgeI2cAddress"));
    const auto plsb = doc.value("PrimaryCartridgeSwbLsbOffset", std::string{});
    c.lsb =
        cartridge_json_int(doc, plsb.empty() ? "SwbCartridge1LsbOffset"
                                             : "PrimaryCartridgeSwbLsbOffset");
    c.swb_bus = num("SwbI2cBus").value_or(0);
    c.swb_addr = static_cast<uint8_t>(cartridge_json_int(doc, "SwbI2cAddress"));
    c.msb = cartridge_json_int(doc, "SwbCartridgeMsbOffset");
    c.rack_off = cartridge_json_int(doc, "SwbCartridgeRackIdOffset");
    c.topo_off = cartridge_json_int(doc, "SwbTopologyIdOffset");
    c.tray_off = cartridge_json_int(doc, "SwbSwitchTrayIdOffset");
    c.slot_off = cartridge_json_int(doc, "SwbSlotIndexOffset");
    if (const auto sz = num("CartridgeRackIdSize"); sz && *sz >= 1 && *sz <= 64)
        c.rack_size = *sz;
    return c;
}

// One-byte EEPROM register reads; the shell's i2ctransfer took only
// byte-sized offsets.
static bool cartridge_read(unsigned bus, uint8_t addr, unsigned off,
                           std::span<uint8_t> out)
{
    const std::array<uint8_t, 1> reg{static_cast<uint8_t>(off)};
    return off <= 0xff && i2c::raw_transfer(bus, addr, reg, out) == 0;
}

static bool cpld_read(const CartridgeConfig& c, unsigned off,
                      std::span<uint8_t> out)
{
    const std::array<uint8_t, 2> reg{static_cast<uint8_t>(c.msb),
                                     static_cast<uint8_t>(off)};
    return off <= 0xff &&
           i2c::raw_transfer(c.swb_bus, c.swb_addr, reg, out) == 0;
}

// Write then read back; a failed write shows up as a readback mismatch.
static bool cpld_push(const CartridgeConfig& c, unsigned off, uint8_t val,
                      std::string_view what)
{
    const std::array<uint8_t, 3> wr{static_cast<uint8_t>(c.msb),
                                    static_cast<uint8_t>(off), val};
    if (off <= 0xff)
        i2c::raw_transfer(c.swb_bus, c.swb_addr, wr, {});
    std::array<uint8_t, 1> rb{};
    if (cpld_read(c, off, rb) && rb[0] == val)
        return true;
    std::cerr << std::format("Cartridge: bus {} address {}: {} not pushed\n",
                             c.bus, c.addr, what);
    return false;
}

static void log_cpld_cartridge_data(const CartridgeConfig& c)
{
    std::vector<uint8_t> rack(c.rack_size);
    std::array<uint8_t, 1> topo{}, tray{}, slot{};
    if (!cpld_read(c, c.lsb + c.rack_off, rack) ||
        !cpld_read(c, c.lsb + c.topo_off, topo) ||
        !cpld_read(c, c.lsb + c.tray_off, tray) ||
        !cpld_read(c, c.lsb + c.slot_off, slot))
    {
        std::cerr << std::format("Cartridge CPLD dump: read failed (LSB={})\n",
                                 c.lsb);
        return;
    }
    std::string raw, ascii;
    bool end = false;
    for (const auto b : rack)
    {
        raw += std::format("{}0x{:02x}", raw.empty() ? "" : " ", b);
        end = end || b == 0;
        if (!end)
            ascii += static_cast<char>(b);
    }
    const auto* topo_name = topo[0] == 0x80   ? "18x1RU"
                            : topo[0] == 0x81 ? "9x2RU"
                            : topo[0] == 0    ? "Loopback/empty"
                                              : "unknown";
    std::cerr << std::format(
        "Cartridge CPLD dump (LSB=0x{:02x}): rack_id raw=[{}] ascii=[{}] "
        "topology_id=0x{:02x} ({}) tray_id=0x{:02x} ({}) slot_id=0x{:02x} "
        "({})\n",
        c.lsb, raw, ascii.empty() ? "<empty>" : ascii, topo[0], topo_name,
        tray[0], tray[0], slot[0], slot[0]);
}

static bool update_cartridge_data(const CartridgeConfig& c)
{
    std::array<uint8_t, 8> hdr{};
    if (i2c::raw_transfer(c.bus, c.addr, {}, std::span{hdr}.first(1)) != 0)
    {
        std::cerr << std::format("No cartridge EEPROM found at bus {} address "
                                 "{}\n",
                                 c.bus, c.addr);
        return false;
    }

    // Board serial follows the board part number field.
    std::array<uint8_t, 1> b{};
    if (!cartridge_read(c.bus, c.addr, 3, b))
        return false;
    const unsigned pn_off = b[0] * 8U + 13;
    if (!cartridge_read(c.bus, c.addr, pn_off, b))
        return false;
    const unsigned sn_off = pn_off + (b[0] & 0x3fU) + 2;

    // Chassis custom info: skip part number and serial fields.
    if (!cartridge_read(c.bus, c.addr, 0, hdr))
        return false;
    unsigned off = hdr[2] * 8U;
    std::array<uint8_t, 3> chdr{};
    if (!cartridge_read(c.bus, c.addr, off, chdr))
        return false;
    off += 3;
    for (int i = 0; i < 3; ++i)
    {
        if (!cartridge_read(c.bus, c.addr, off, b))
            return false;
        off += i < 2 ? 1 + (b[0] & 0x3fU) : 1;
    }

    std::vector<uint8_t> rack(c.rack_size);
    std::array<uint8_t, 1> topo{}, tray{}, slot{};
    if (!cartridge_read(c.bus, c.addr, sn_off, rack) ||
        !cartridge_read(c.bus, c.addr, off + 4, topo) ||
        !cartridge_read(c.bus, c.addr, off + 3, tray) ||
        !cartridge_read(c.bus, c.addr, off + 2, slot))
    {
        std::cerr << std::format("Cartridge: EEPROM read failed (bus={})\n",
                                 c.bus);
        return false;
    }

    if (!cpld_push(c, c.lsb + c.tray_off, tray[0], "Switch tray Id") ||
        !cpld_push(c, c.lsb + c.slot_off, slot[0], "Slot index") ||
        !cpld_push(c, c.lsb + c.topo_off, topo[0], "Topology id"))
        return false;

    // Legacy fixed-width CPLD blocks: pad the transaction to 16 bytes.
    const unsigned rack_reg = c.lsb + c.rack_off;
    std::vector<uint8_t> wr{static_cast<uint8_t>(c.msb),
                            static_cast<uint8_t>(rack_reg)};
    wr.insert(wr.end(), rack.begin(), rack.end());
    wr.resize(std::max<size_t>(wr.size(), 16), 0);
    std::vector<uint8_t> rb(c.rack_size);
    if (rack_reg > 0xff ||
        i2c::raw_transfer(c.swb_bus, c.swb_addr, wr, {}) != 0 ||
        !cpld_read(c, rack_reg, rb) || rb != rack)
    {
        std::cerr << std::format("Cartridge: bus {} address {}: Rack Id not "
                                 "pushed\n",
                                 c.bus, c.addr);
        return false;
    }

    std::cerr << std::format("Cartridge: bus {} EEPROM data pushed to CPLD LSB "
                             "0x{:02x}\n",
                             c.bus, c.lsb);
    log_cpld_cartridge_data(c);
    return true;
}

static void ast2700_copy_cartridge_data(const std::string& path)
{
    using namespace std::chrono_literals;
    constexpr int retries = 5;

    const auto c = load_cartridge_config(path);
    if (!c)
        return;
    if (c->access != "I2C" && !c->access.empty())
    {
        std::cerr << std::format("Cartridge data copy: PhysicalAccess={} not "
                                 "supported, skipping\n",
                                 c->access);
        return;
    }
    for (int attempt = 1; attempt <= retries; ++attempt)
    {
        if (update_cartridge_data(*c))
            return;
        std::cerr << std::format("Cartridge CPLD copy failed, attempt {}/{}\n",
                                 attempt, retries);
        if (attempt < retries)
            std::this_thread::sleep_for(2s);
    }
    std::cerr << std::format("[ERROR] Cartridge CPLD copy failed after {} "
                             "attempts\n",
                             retries);
}

// TEMPORARY HID197 bring-up fallback; remove once its VPD layout is confirmed.
static std::string ast2700_dt_hw_id_fallback()
{
    std::error_code ec;
    const std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator it{
             "/proc/device-tree",
             std::filesystem::directory_options::skip_permission_denied, ec};
         it != end; it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        const auto name = it->path().filename().string();
        if (!name.starts_with("nvsw"))
        {
            continue;
        }
        const auto hid = name.find("hid");
        if (hid == std::string::npos)
        {
            continue;
        }
        size_t digits = hid + 3;
        while (digits < name.size() &&
               std::isdigit(static_cast<unsigned char>(name[digits])) != 0)
        {
            ++digits;
        }
        if (name.substr(hid, digits - hid) == "hid197")
        {
            return "HI197";
        }
    }
    return {};
}

struct Ast2700HwId
{
    std::string value;
    bool from_dt_fallback = false;
};

// On the HID197 DT fallback, VPD rev_id is not authoritative.
static Ast2700HwId ast2700_read_hw_id()
{
    const auto is_valid = [](std::string_view hid) {
        return hid.size() == 5 && hid.starts_with("HI") &&
               std::ranges::all_of(hid.substr(2), [](unsigned char c) {
                   return std::isdigit(c) != 0;
               });
    };

    auto vpd = vpd_read_field(ast2700_vpd_eeprom, ast2700_vpd_hid_offset,
                              ast2700_vpd_hid_size);

    // TEMPORARY HID197 bring-up fallback.
    const auto dt_hw_id = ast2700_dt_hw_id_fallback();
    if (!is_valid(vpd) || (!dt_hw_id.empty() && vpd != dt_hw_id))
    {
        if (dt_hw_id.empty())
        {
            std::cerr << std::format(
                "switch-ast2700-bmc: invalid HID read from "
                "VPD {} offset {}: '{}'\n",
                ast2700_vpd_eeprom, ast2700_vpd_hid_offset, vpd);
            return {};
        }
        std::cerr << std::format(
            "switch-ast2700-bmc: TEMPORARY HID fallback: VPD {} "
            "offset {} returned '{}'; using device-tree HID {}. Verify the "
            "HID197 VPD layout during bring-up and remove this fallback.\n",
            ast2700_vpd_eeprom, ast2700_vpd_hid_offset, vpd, dt_hw_id);
        return {.value = dt_hw_id, .from_dt_fallback = true};
    }
    // END TEMPORARY HID197 bring-up fallback.

    return {.value = std::move(vpd)};
}

// ARMv7_CPU and ARMv8_CPU as spelled in this machine's helpers copy.
constexpr std::array<std::string_view, 2> ast2700_decode_cpu_types{
    "0xC07", "0xD04"};

// ── CPLD-ready gate (VRU) ─────────────────────────────────────────────────
// On VRU each MCU is the root of trust of one CPLD: it authenticates the CPLD
// image and streams it over SSPI, and until every CPLD is streamed the register
// space behind the platform driver is incomplete. The driver is therefore
// blacklisted on those HIDs and loaded here, once every MCU listed in
// CPLD_READY_MCUS reports its CPLD component Activated in NSM Get RoT State
// Information (type 6, command 0x01). The MCU reports in-progress and failure
// with the same state, so the only way out of "not done" is the timeout, after
// which the driver is loaded anyway: a backup-image restore can take up to
// 300 s. The MCUs stream in parallel, so they share one timeout.
//
// The MCUs have no EID this early: entity-manager and mctpreactor assign them
// long after platform init. The same static EIDs are assigned here through
// mctpd, which hands back the existing endpoint when mctpreactor repeats the
// call.
//
// nsmtool refuses to send while nsmd owns its bus name, so the VRU
// platform_init.service drop-in orders this unit before nsmd.service.

static constexpr uint16_t cpld_rot_component_class = 0x000a;
static constexpr uint16_t cpld_rot_component_id = 0x0120;
static constexpr std::chrono::seconds cpld_ready_default_timeout{330};
static constexpr std::chrono::seconds cpld_ready_poll_interval{2};
static constexpr std::chrono::seconds nsm_command_timeout{10};
// MCTP reserves EID 0 (null) and 255 (broadcast).
static constexpr unsigned mctp_max_static_eid = 254;
static constexpr size_t shell_read_chunk = 512;
static constexpr size_t nsm_reply_log_limit = 2048;

struct ShellCapture
{
    int status = -1;
    std::string output;
};

// Runs cmd through /bin/sh and returns its pclose() status and stdout with
// trailing newlines removed. Callers append "2>&1" to capture stderr too.
// status stays -1 when the shell cannot be started.
//   in:  "echo hello"
//   out: { status = 0, output = "hello" }
//   in:  "false"
//   out: { status = 256, output = "" }   (wait status: exit code 1 << 8)
static ShellCapture ast2700_shell_capture(const std::string& cmd)
{
    ShellCapture result;
    auto* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe)
        return result;
    std::array<char, shell_read_chunk> buf{};
    while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        result.output += buf.data();
    result.status = ::pclose(pipe);
    while (!result.output.empty() && result.output.back() == '\n')
        result.output.pop_back();
    return result;
}

// The value of key in /etc/platform_features.conf as an unsigned integer, or
// nullopt when the key is unset or its value is not a plain decimal number.
//   in:  "CPLD_READY_TIMEOUT"  with  CPLD_READY_TIMEOUT=330   out: 330
//   in:  "CPLD_READY_TIMEOUT"  with  CPLD_READY_TIMEOUT=330s  out: nullopt
//   in:  "CPLD_READY_TIMEOUT"  with the key absent            out: nullopt
static std::optional<unsigned> ast2700_feature_uint(std::string_view key)
{
    const auto value = platform_feature(key);
    unsigned parsed = 0;
    const auto* const end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(value.data(), end, parsed);
    if (value.empty() || ec != std::errc{} || ptr != end)
        return std::nullopt;
    return parsed;
}

// A shell reply on one line for the journal: whitespace runs collapsed to a
// single space and anything past nsm_reply_log_limit cut off. An empty reply
// is described by its exit status instead.
//   in:  { status = 0, output = "{\n  \"Completion code\": 4\n}" }
//   out: "{ \"Completion code\": 4 }"
//   in:  { status = 31744, output = "" }  (timeout killed nsmtool)
//   out: "no output, exit status 124"
static std::string ast2700_reply_for_log(const ShellCapture& reply)
{
    std::string line;
    for (const char c : reply.output)
    {
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            if (!line.empty() && line.back() != ' ')
                line += ' ';
        }
        else
            line += c;
    }
    if (!line.empty() && line.back() == ' ')
        line.pop_back();

    if (line.empty())
    {
        if (reply.status == -1)
            return "no output, shell not started";
        if (WIFEXITED(reply.status))
            return std::format("no output, exit status {}",
                               WEXITSTATUS(reply.status));
        return std::format("no output, wait status {}", reply.status);
    }
    if (line.size() > nsm_reply_log_limit)
    {
        line.resize(nsm_reply_log_limit);
        line += " ...";
    }
    return line;
}

// The "Firmware state" of the active slot in nsmtool's GetRotInformation JSON,
// or empty when the reply is missing, malformed or carries a non-zero
// completion code. nsmtool's stderr is merged in, so the JSON starts at the
// first brace.
//   in:  { "Completion code": 0, "Active Slot": 0,
//          "Slot information": [
//              { "Slot ID": 0, "Firmware state": "Activated", ... },
//              { "Slot ID": 1, "Firmware state": "Unknown", ... } ] }
//   out: "Activated"
//   in:  { "Completion code": 4, ... }               out: ""
//   in:  "nsmtool: failed to open socket"            out: ""
static std::string ast2700_cpld_rot_state(std::string_view nsmtool_output)
{
    const auto start = nsmtool_output.find('{');
    if (start == std::string_view::npos)
        return {};

    try
    {
        const auto doc =
            nlohmann::json::parse(nsmtool_output.substr(start), nullptr, false);
        if (doc.is_discarded() || !doc.is_object() ||
            doc.value("Completion code", -1) != 0)
            return {};

        const auto active = doc.value("Active Slot", -1);
        const auto slots = doc.find("Slot information");
        if (slots == doc.end() || !slots->is_array())
            return {};
        for (const auto& slot : *slots)
        {
            if (slot.is_object() && slot.value("Slot ID", -2) == active)
                return slot.value("Firmware state", std::string{});
        }
    }
    catch (const nlohmann::json::exception& e)
    {
        std::cerr << std::format("switch-ast2700-bmc: unexpected nsmtool "
                                 "reply: {}\n",
                                 e.what());
    }
    return {};
}

struct CpldReadyMcu
{
    std::string iface;
    unsigned eid = 0;
    std::string netdev;
    std::string assign_cmd;
    std::string query_cmd;
    bool assigned = false;
    bool ready = false;
    std::string last_state;
    std::string last_reply;
};

// CPLD_READY_MCUS is a whitespace-separated list of <netdev>:<EID> pairs, one
// per CPLD root-of-trust MCU, e.g. "musbmgmt:71 musbswb1:69". A malformed
// entry is logged and dropped. Each returned MCU carries its netdev path and
// the mctpd and nsmtool commands built for it.
//   in:  CPLD_READY_MCUS="musbmgmt:71 musbswb1:69"
//   out: [ { iface = "musbmgmt", eid = 71,
//            netdev = "/sys/class/net/musbmgmt",
//            assign_cmd = "busctl call ... interfaces/musbmgmt ...
//                          AssignEndpointStatic ayyyayay 0 71 0 0 0 2>&1",
//            query_cmd = "timeout 10 nsmtool firmware GetRotInformation
//                         -m 71 -c 0x0a -i 0x0120 -d 0 2>&1" },
//          { iface = "musbswb1", eid = 69, ... } ]
//   in:  CPLD_READY_MCUS="musbmgmt:71 musbswb1 musbswb2:300"
//   out: [ { iface = "musbmgmt", eid = 71, ... } ]  (the other two logged)
//   in:  CPLD_READY_MCUS unset                       out: []
static std::vector<CpldReadyMcu> ast2700_cpld_ready_mcus()
{
    std::vector<CpldReadyMcu> mcus;
    const auto list = platform_feature("CPLD_READY_MCUS");
    std::string_view rest{list};
    while (!rest.empty())
    {
        const auto begin = rest.find_first_not_of(" \t");
        if (begin == std::string_view::npos)
            break;
        rest.remove_prefix(begin);
        const auto entry = rest.substr(0, rest.find_first_of(" \t"));
        rest.remove_prefix(entry.size());

        const auto colon = entry.find(':');
        const auto iface = entry.substr(0, colon);
        const auto eid_str = colon == std::string_view::npos
                                 ? std::string_view{}
                                 : entry.substr(colon + 1);
        unsigned eid = 0;
        const auto* const end = eid_str.data() + eid_str.size();
        const auto [ptr, ec] = std::from_chars(eid_str.data(), end, eid);
        if (iface.empty() || eid_str.empty() || ec != std::errc{} ||
            ptr != end || eid == 0 || eid > mctp_max_static_eid)
        {
            std::cerr << std::format("switch-ast2700-bmc: ignoring malformed "
                                     "CPLD_READY_MCUS entry '{}'\n",
                                     entry);
            continue;
        }

        CpldReadyMcu mcu;
        mcu.iface = iface;
        mcu.eid = eid;
        mcu.netdev = std::format("/sys/class/net/{}", iface);
        mcu.assign_cmd = std::format(
            "busctl call au.com.codeconstruct.MCTP1 "
            "/au/com/codeconstruct/mctp1/interfaces/{} "
            "au.com.codeconstruct.MCTP.BusOwner1 AssignEndpointStatic "
            "ayyyayay 0 {} 0 0 0 2>&1",
            iface, eid);
        mcu.query_cmd = std::format(
            "timeout {} nsmtool firmware GetRotInformation -m {} -c {:#04x} "
            "-i {:#06x} -d 0 2>&1",
            nsm_command_timeout.count(), eid, cpld_rot_component_class,
            cpld_rot_component_id);
        mcus.push_back(std::move(mcu));
    }
    return mcus;
}

// One poll of one MCU: the active slot's firmware state, or why there is none.
// Assigns the MCU's static EID first when it has none yet, and forgets the
// assignment when the netdev disappears or the MCU gives no valid reply. Every
// nsmtool reply is kept in mcu.last_reply.
//   in:  { iface = "musbmgmt", eid = 71 }, netdev up, MCU streamed its CPLD
//   out: "Activated"                     (mcu.assigned = true)
//   in:  { iface = "musbswb1", eid = 69 }, /sys/class/net/musbswb1 missing
//   out: "musbswb1 absent"               (mcu.assigned = false)
//   in:  { iface = "musbmgmt", eid = 71 }, nsmtool times out
//   out: "no valid NSM reply: no output, exit status 124"
//                                        (mcu.assigned = false)
static std::string ast2700_poll_cpld_mcu(CpldReadyMcu& mcu)
{
    if (!std::filesystem::exists(mcu.netdev))
    {
        mcu.assigned = false;
        return std::format("{} absent", mcu.iface);
    }

    if (!mcu.assigned)
    {
        const auto r = ast2700_shell_capture(mcu.assign_cmd);
        mcu.assigned = r.status == 0;
        if (!mcu.assigned)
            return std::format("EID {} assignment failed: {}", mcu.eid,
                               r.output);
    }

    const auto reply = ast2700_shell_capture(mcu.query_cmd);
    mcu.last_reply = ast2700_reply_for_log(reply);
    auto state = ast2700_cpld_rot_state(reply.output);
    if (state.empty())
    {
        // The MCU may have re-enumerated and lost its EID; the assignment is
        // idempotent, so redo it next round.
        mcu.assigned = false;
        return std::format("no valid NSM reply: {}", mcu.last_reply);
    }
    return state;
}

// No-op unless the active HID sets CPLD_READY_MCUS in
// /etc/platform_features.conf. Polls every listed MCU every 2 s until all
// report Activated or CPLD_READY_TIMEOUT expires, logging each state change.
// Returns in both cases; on timeout each MCU not ready is logged as an error.
//   in:  CPLD_READY_MCUS="musbmgmt:71", MCU reports Activated after 4 s
//   out: returns after about 4 s, logging "MCU EID 71 on musbmgmt reports
//        its CPLD ready after 4 s"
//   in:  CPLD_READY_MCUS="musbmgmt:71 musbswb1:69", musbswb1 never appears
//   out: returns after CPLD_READY_TIMEOUT, logging "MCU EID 69 on musbswb1
//        did not report its CPLD ready within 330 s (last: musbswb1 absent;
//        last nsmtool reply: none)"
static void ast2700_wait_cpld_ready()
{
    auto mcus = ast2700_cpld_ready_mcus();
    if (mcus.empty())
        return;

    const auto timeout =
        ast2700_feature_uint("CPLD_READY_TIMEOUT")
            .transform([](unsigned s) { return std::chrono::seconds{s}; })
            .value_or(cpld_ready_default_timeout);

    for (const auto& mcu : mcus)
    {
        std::cerr << std::format("switch-ast2700-bmc: waiting up to {} s for "
                                 "MCU EID {} on {} to report its CPLD ready\n",
                                 timeout.count(), mcu.eid, mcu.iface);
    }

    const auto start = std::chrono::steady_clock::now();
    const auto elapsed_since_start = [start] {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start);
    };
    for (;;)
    {
        for (auto& mcu : mcus)
        {
            // An unanswered poll takes up to nsm_command_timeout, so stop the
            // round once the budget is spent.
            if (elapsed_since_start() >= timeout)
                break;
            if (mcu.ready)
                continue;

            const auto state = ast2700_poll_cpld_mcu(mcu);
            const auto elapsed = elapsed_since_start();
            if (state == "Activated")
            {
                mcu.ready = true;
                std::cerr << std::format("switch-ast2700-bmc: MCU EID {} on "
                                         "{} reports its CPLD ready after {} "
                                         "s\n",
                                         mcu.eid, mcu.iface, elapsed.count());
            }
            else if (state != mcu.last_state)
            {
                std::cerr << std::format(
                    "switch-ast2700-bmc: MCU EID {} on {}: CPLD not ready "
                    "yet after {} s: {}\n",
                    mcu.eid, mcu.iface, elapsed.count(), state);
                mcu.last_state = state;
            }
        }

        if (std::ranges::all_of(mcus, &CpldReadyMcu::ready))
        {
            std::cerr << "switch-ast2700-bmc: every MCU reports its CPLD "
                         "ready\n";
            return;
        }

        if (elapsed_since_start() >= timeout)
        {
            for (const auto& mcu : mcus)
            {
                if (mcu.ready)
                    continue;
                phosphor_log_all(std::format(
                    "platform init: MCU EID {} on {} did not report its CPLD "
                    "ready within {} s (last: {}; last nsmtool reply: {}); "
                    "loading the platform driver anyway",
                    mcu.eid, mcu.iface, timeout.count(), mcu.last_state,
                    mcu.last_reply.empty() ? "none" : mcu.last_reply));
            }
            return;
        }
        std::this_thread::sleep_for(cpld_ready_poll_interval);
    }
}

// The driver named by PLATFORM_DRIVER_MODULE is blacklisted on the HIDs that
// set it, so udev never autoloads it and it is loaded only here. A failed
// load is logged as an error and init carries on: the BMC stays reachable
// without the platform driver, only its hw-management tree is missing.
//   in:  PLATFORM_DRIVER_MODULE=nvsw_bmc_hid162
//   runs: modprobe nvsw_bmc_hid162
//   in:  PLATFORM_DRIVER_MODULE unset
//   runs: nothing
static void ast2700_load_platform_driver()
{
    const auto module = platform_feature("PLATFORM_DRIVER_MODULE");
    if (module.empty())
        return;
    const auto cmd = std::format("modprobe {}", module);
    std::cerr << std::format("switch-ast2700-bmc: calling {}\n", cmd);
    if (const int rc = std::system(cmd.c_str()); // NOLINT(cert-env33-c)
        rc != 0)
    {
        phosphor_log_all(std::format(
            "platform init: ERROR: '{}' failed (status {}); the platform "
            "driver is not loaded",
            cmd, rc));
    }
}

// ── Platform init ─────────────────────────────────────────────────────────

int init_switch_ast2700_bmc()
{
    using namespace std::chrono_literals;

    std::cerr << "switch-ast2700-bmc: platform init start\n";

    run_nos_hook("pre");

    // The recovery slave first: the recovery path needs it most when a later
    // step fails.
    switch_misc::i2c_slave_setup_if_configured();

    {
        NosHookGuard nos_hook;

        const auto gpio_config =
            load_gpio_pin_config().value_or(GpioPinConfig{});
        sysfs_gpio_apply(gpio_config.pins);

        // Both no-ops unless the active HID's platform_features.conf asks for
        // them. They must precede the wait below: the system tree appears only
        // once the platform driver binds.
        ast2700_wait_cpld_ready();
        ast2700_load_platform_driver();

        hw_mgmt_wait_system_tree("switch-ast2700-bmc");

        hw_mgmt_link_gpio(gpio_config.pins);
        std::cerr << "switch-ast2700-bmc: GPIO sysfs initialization "
                     "complete\n";

        const bool cpu_off = hw_mgmt_take_i2c_when_cpu_off(
            ast2700_mlxreg_io_hwmon, "switch-ast2700-bmc");

        // A timeout here is not fatal.
        if (!gpio_config.bmc_stby_ready.empty())
        {
            wait_sysfs_value(gpio_config.bmc_stby_ready, "1", 10, 1000ms);
        }

        write_cpu_type("switch-ast2700-bmc");

        const auto hw_id = ast2700_read_hw_id();
        if (!hw_id.value.empty())
        {
            std::cerr << std::format("switch-ast2700-bmc: system hardware Id "
                                     "is {}\n",
                                     hw_id.value);
            hw_mgmt_write_config("hid", hw_id.value);
        }

        if (const auto bom =
                vpd_read_bom(ast2700_vpd_eeprom, hw_id.value, ast2700_bom_hids,
                             std::nullopt, ast2700_vpd_bom_size);
            !bom.empty())
        {
            std::cerr << std::format("switch-ast2700-bmc: system hardware BOM "
                                     "record is {}\n",
                                     bom);
            hw_mgmt_write_config("bom", bom);
        }

        if (hw_id.from_dt_fallback)
        {
            std::cerr << "switch-ast2700-bmc: skipping rev_id; HID came from "
                         "the temporary HID197 device-tree fallback\n";
        }
        else
        {
            write_system_hw_rev(ast2700_vpd_eeprom, hw_id.value,
                                ast2700_bom_hids, "switch-ast2700-bmc");
        }

        // Only while the host CPU is not driving the I2C tree.
        if (cpu_off && std::filesystem::exists(ast2700_cartridge_config))
        {
            ast2700_copy_cartridge_data(std::string{ast2700_cartridge_config});
        }

        // MCU_RESET_ON_BOOT=no keeps the MCUs running: on VRU they control the
        // CPLDs, and a reset would re-stream them under the platform driver.
        if (platform_feature("MCU_RESET_ON_BOOT") == "no")
        {
            std::cerr << "switch-ast2700-bmc: MCU_RESET_ON_BOOT=no, leaving "
                         "the MCUs running\n";
        }
        else
        {
            hw_mgmt_mcu_reset();
        }

        const auto leakSource = platform_feature("LEAK_DETECTION_SOURCE");
        if (leakSource == "mcu")
        {
            std::cerr << "switch-ast2700-bmc: leak detection comes from the "
                         "MCU, skipping A2D configuration\n";
        }
        else if (leakSource == "none")
        {
            std::cerr << "switch-ast2700-bmc: no leak detectors on this "
                         "platform, skipping A2D configuration\n";
        }
        else
        {
            shell_out("a2d_leakage_config.sh", "switch-ast2700-bmc");
        }

        // eMMC-backed, unlike the other switch machines.
        if (filesystem_check() != EXIT_SUCCESS)
        {
            std::cerr << "switch-ast2700-bmc: FATAL: filesystem check "
                         "failed\n";
            return EXIT_FAILURE;
        }

        unset_write_protect_on_boot();
    }

    hw_mgmt_boot_start({}, ast2700_decode_cpu_types);

    sd_notify(0, "READY=1");

    phosphor_log_all("platform init completed");

    recover_external_rtc("switch-ast2700-bmc");

    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
