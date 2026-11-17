// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "switch_cartridge.hpp"

#include "i2c.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace nvidia
{

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
    if (off == 0)
    {
        std::cerr << "Cartridge: chassis info area missing\n";
        return false;
    }
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
    // Slot, tray and topology are at +2, +3 and +4 of the custom data.
    if ((b[0] & 0x3fU) < 5)
    {
        std::cerr << std::format("Cartridge: chassis custom info too short "
                                 "(len={})\n",
                                 b[0] & 0x3fU);
        return false;
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

void copy_cartridge_data(const std::string& path)
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

} // namespace nvidia
