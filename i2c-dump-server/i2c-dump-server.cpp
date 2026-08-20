// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// i2c-dump-server: HMC on-demand dump command server.
// The FPGA writes a request byte into the control I2C slave EEPROM;
// this daemon polls every 1s, dispatches the command via D-Bus,
// and writes results back to the EEPROM.
// Replaces i2c-dump-server.sh.
//
// Usage: i2c-dump-server <ctrl_bus> <data_bus>
//   Standard HMC:   ctrl_bus=0, data_bus=3   (EEPROMs: 0-1045, 3-104f)
//   AST2700-a1 HMC: ctrl_bus=9, data_bus=12  (EEPROMs: 9-1045, 12-104f)
//
// Control EEPROM (addr 0x45, path {bus}-1045/slave-eeprom):
//   Bytes  1-17: opcode (byte 1) + args (bytes 2-17)
//   Byte   18:   request flag — FPGA writes 0x01 to trigger command
//   Byte   19:   execution status — 0x01=in-progress, 0x00=done
//   Byte   20:   return code
//   Bytes 21-26: result (entry_id 2B BE  /  exec_status+pct+size 6B)
//
// Data EEPROM (addr 0x4f, path {bus}-104f/slave-eeprom):
//   Bytes 1280-1283: block size (4B big-endian)
//   Bytes 1284+:     block data (up to 8192 bytes)

#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

static constexpr uint32_t MAX_BLOCK_SIZE = 8192;
static constexpr uint8_t CTRL_ADDR = 0x45; // I2C slave backend: 0x1045
static constexpr uint8_t DATA_ADDR = 0x4f; // I2C slave backend: 0x104f

// ── EEPROM I/O ────────────────────────────────────────────────────────────

static bool eeprom_write(const std::string& path, std::size_t offset,
                         const uint8_t* data, std::size_t len)
{
    std::ofstream f{path, std::ios::binary | std::ios::in | std::ios::out};
    if (!f)
    {
        std::cerr << std::format("i2c-dump-server: cannot open {}\n", path);
        return false;
    }
    f.seekp(static_cast<std::streamoff>(offset));
    if (!f)
    {
        std::cerr << std::format("i2c-dump-server: cannot seek {} to {}\n",
                                 path, offset);
        return false;
    }
    f.write(reinterpret_cast<const char*>(data),
            static_cast<std::streamsize>(len));
    if (!f)
    {
        std::cerr << std::format("i2c-dump-server: cannot write {} bytes to "
                                 "{} at {}\n",
                                 len, path, offset);
        return false;
    }
    return true;
}

static uint8_t eeprom_read_byte(const std::string& path, std::size_t offset)
{
    std::ifstream f{path, std::ios::binary};
    if (!f)
        return 0;
    f.seekg(static_cast<std::streamoff>(offset));
    uint8_t b = 0;
    f.read(reinterpret_cast<char*>(&b), 1);
    return b;
}

static std::vector<uint8_t> eeprom_read(const std::string& path,
                                        std::size_t offset, std::size_t len)
{
    std::vector<uint8_t> buf(len, 0);
    std::ifstream f{path, std::ios::binary};
    if (!f)
        return buf;
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(len));
    return buf;
}

static void write_be16(const std::string& path, std::size_t offset,
                       uint16_t val)
{
    uint8_t b[2] = {static_cast<uint8_t>(val >> 8), static_cast<uint8_t>(val)};
    eeprom_write(path, offset, b, 2);
}

static void write_be32(const std::string& path, std::size_t offset,
                       uint32_t val)
{
    uint8_t b[4] = {static_cast<uint8_t>(val >> 24),
                    static_cast<uint8_t>(val >> 16),
                    static_cast<uint8_t>(val >> 8), static_cast<uint8_t>(val)};
    eeprom_write(path, offset, b, 4);
}

// ── Opcode 1: trigger dump creation ──────────────────────────────────────

using DumpParams = std::map<std::string, std::variant<std::string>>;

static int create_new_dump(sdbusplus::bus_t& bus, const std::string& ctrl,
                           uint8_t dump_type, uint8_t dump_sub_type)
{
    std::string obj_path;
    DumpParams params;

    if (dump_type == 1)
    {
        obj_path = "/xyz/openbmc_project/dump/bmc";
    }
    else if (dump_type == 2)
    {
        obj_path = "/xyz/openbmc_project/dump/system";
        if (dump_sub_type == 1)
            params["DiagnosticType"] = "FPGA";
        else if (dump_sub_type == 2)
            params["DiagnosticType"] = "EROT";
        else
        {
            std::cerr << "i2c-dump-server: invalid dump_sub_type\n";
            return 1;
        }
    }
    else
    {
        std::cerr << "i2c-dump-server: invalid dump_type\n";
        return 1;
    }

    std::cerr << std::format("i2c-dump-server: CreateDump type={} sub={}\n",
                             dump_type, dump_sub_type);
    try
    {
        auto m = bus.new_method_call(
            "xyz.openbmc_project.Dump.Manager", obj_path.c_str(),
            "xyz.openbmc_project.Dump.Create", "CreateDump");
        m.append(params);
        auto reply = bus.call(m);

        sdbusplus::message::object_path entry;
        reply.read(entry);
        const std::string& s = entry.str;
        uint16_t entry_id =
            static_cast<uint16_t>(std::stoul(s.substr(s.rfind('/') + 1)));

        write_be16(ctrl, 21, entry_id);
        std::cerr << std::format("i2c-dump-server: dump {} initiated\n",
                                 entry_id);
        return 0;
    }
    catch (const sdbusplus::exception_t& e)
    {
        std::string_view msg = e.what();
        std::cerr << std::format("i2c-dump-server: CreateDump failed: {}\n",
                                 msg);
        if (msg.find("not allowed") != std::string_view::npos ||
            msg.find("temporarily unavailable") != std::string_view::npos)
            return 2; // dump already in progress
        return 1;
    }
}

// ── Opcode 2: check dump progress ────────────────────────────────────────

// True when a D-Bus error name means "the object does not expose that
// property", as opposed to a failure reaching the service at all.
static bool prop_absent(const char* err_name)
{
    if (err_name == nullptr)
    {
        return false;
    }
    std::string_view name{err_name};
    return name == "org.freedesktop.DBus.Error.UnknownProperty" ||
           name == "org.freedesktop.DBus.Error.UnknownInterface" ||
           name == "org.freedesktop.DBus.Error.InvalidArgs";
}

static int check_dump_progress(sdbusplus::bus_t& bus, const std::string& ctrl,
                               uint8_t dump_type, uint16_t entry_id)
{
    std::string type_str = (dump_type == 1) ? "bmc" : "system";
    std::string entry_path = std::format(
        "/xyz/openbmc_project/dump/{}/entry/{}", type_str, entry_id);

    std::cerr << std::format("i2c-dump-server: check progress {} id={}\n",
                             type_str, entry_id);

    auto get_prop = [&](const std::string& iface, const std::string& prop) {
        auto m = bus.new_method_call("xyz.openbmc_project.Dump.Manager",
                                     entry_path.c_str(),
                                     "org.freedesktop.DBus.Properties", "Get");
        m.append(iface, prop);
        return bus.call(m);
    };

    try
    {
        auto sr = get_prop("xyz.openbmc_project.Common.Progress", "Status");
        std::variant<std::string> sv;
        sr.read(sv);
        std::string full = std::get<std::string>(sv);
        std::string status = full.substr(full.rfind('.') + 1);

        uint8_t exec_status = 0x02; // Failed by default
        uint64_t dump_size = 0;

        if (status == "InProgress")
        {
            exec_status = 0x00;
        }
        else if (status == "Completed")
        {
            exec_status = 0x01;
            auto szr = get_prop("xyz.openbmc_project.Dump.Entry", "Size");
            std::variant<uint64_t> szv;
            szr.read(szv);
            dump_size = std::get<uint64_t>(szv);
        }

        uint8_t pct = 0;
        try
        {
            auto pr =
                get_prop("xyz.openbmc_project.Common.Progress", "Progress");
            std::variant<uint8_t> pv;
            pr.read(pv);
            pct = std::get<uint8_t>(pv);
        }
        catch (const sdbusplus::exception_t& e)
        {
            // Progress is optional: a dump manager may implement Status
            // without it, and 0% is the right answer there. Anything else
            // (transport down, access denied) is a real failure and must
            // reach the outer handler rather than report success.
            if (!prop_absent(e.name()))
            {
                throw;
            }
        }

        // Write 6 bytes at ctrl offset 21: [exec_status, pct, size(4B BE)]
        uint8_t result[6] = {exec_status,
                             pct,
                             static_cast<uint8_t>(dump_size >> 24),
                             static_cast<uint8_t>(dump_size >> 16),
                             static_cast<uint8_t>(dump_size >> 8),
                             static_cast<uint8_t>(dump_size)};
        eeprom_write(ctrl, 21, result, 6);

        std::cerr << std::format("i2c-dump-server: dump {} {} {}% {} B\n",
                                 entry_id, status, pct, dump_size);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << std::format("i2c-dump-server: check_progress error: {}\n",
                                 e.what());
        return 1;
    }
}

// ── Opcode 3: stream block of dump file into data EEPROM ─────────────────

static int request_block_data(const std::string& data_eeprom, uint8_t dump_type,
                              uint16_t entry_id, uint16_t block_number)
{
    std::string type_str = (dump_type == 1) ? "bmc" : "system";
    std::string dump_dir =
        std::format("/var/lib/logging/dumps/{}/{}", type_str, entry_id);

    std::cerr << std::format(
        "i2c-dump-server: block data type={} id={} block={}\n", type_str,
        entry_id, block_number);

    // Find the single dump file in the entry directory
    std::filesystem::path dump_path;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dump_dir, ec))
        if (e.is_regular_file())
        {
            dump_path = e.path();
            break;
        }

    if (dump_path.empty() || ec)
    {
        std::cerr << std::format("i2c-dump-server: dump {} not found\n",
                                 entry_id);
        return 1;
    }

    auto dump_size = std::filesystem::file_size(dump_path, ec);
    if (ec)
    {
        std::cerr << "i2c-dump-server: file_size error\n";
        return 1;
    }

    std::size_t skip = static_cast<std::size_t>(block_number) * MAX_BLOCK_SIZE;
    if (skip >= dump_size)
    {
        std::cerr << "i2c-dump-server: block_number out of range\n";
        return 1;
    }

    std::size_t block_size =
        std::min<std::size_t>(dump_size - skip, MAX_BLOCK_SIZE);

    // Write block_size (BE32) at data EEPROM offset 1280
    write_be32(data_eeprom, 1280, static_cast<uint32_t>(block_size));

    // Write block data at data EEPROM offset 1284
    std::vector<uint8_t> buf(block_size);
    if (std::ifstream f{dump_path, std::ios::binary}; f)
    {
        f.seekg(static_cast<std::streamoff>(skip));
        f.read(reinterpret_cast<char*>(buf.data()),
               static_cast<std::streamsize>(block_size));
    }
    eeprom_write(data_eeprom, 1284, buf.data(), block_size);

    std::cerr << std::format("i2c-dump-server: block {} ready ({} B)\n",
                             block_number, block_size);
    return 0;
}

// ── Command dispatcher ────────────────────────────────────────────────────

static void process_command(sdbusplus::bus_t& bus, const std::string& ctrl,
                            const std::string& data_eeprom)
{
    // Clear request flag (byte 18 = 0) and mark in-progress (byte 19 = 1)
    uint8_t init[2] = {0x00, 0x01};
    eeprom_write(ctrl, 18, init, 2);

    // Read opcode (byte 1) and args (bytes 2-17)
    auto args = eeprom_read(ctrl, 1, 17);
    uint8_t opcode = args[0];

    int rc = 1;
    switch (opcode)
    {
        case 1:
            rc = create_new_dump(bus, ctrl, args[1], args[2]);
            break;
        case 2:
        {
            uint16_t entry_id = (static_cast<uint16_t>(args[2]) << 8) | args[3];
            rc = check_dump_progress(bus, ctrl, args[1], entry_id);
            break;
        }
        case 3:
        {
            uint16_t entry_id = (static_cast<uint16_t>(args[2]) << 8) | args[3];
            uint16_t block_number =
                (static_cast<uint16_t>(args[4]) << 8) | args[5];
            rc = request_block_data(data_eeprom, args[1], entry_id,
                                    block_number);
            break;
        }
        default:
            std::cerr << std::format("i2c-dump-server: unknown opcode {}\n",
                                     opcode);
    }

    // Write return code at byte 20, then clear in-progress at byte 19
    uint8_t rc_byte = static_cast<uint8_t>(rc);
    eeprom_write(ctrl, 20, &rc_byte, 1);
    uint8_t done = 0x00;
    eeprom_write(ctrl, 19, &done, 1);
}

// ── Main ──────────────────────────────────────────────────────────────────

int main(int argc, char** argv)
{
    unsigned ctrl_bus = 0;
    unsigned data_bus = 3;
    if (argc >= 2)
        try
        {
            ctrl_bus = std::stoul(argv[1]);
        }
        catch (...)
        {}
    if (argc >= 3)
        try
        {
            data_bus = std::stoul(argv[2]);
        }
        catch (...)
        {}

    const std::string ctrl = std::format(
        "/sys/bus/i2c/devices/{}-10{:02x}/slave-eeprom", ctrl_bus, CTRL_ADDR);
    const std::string data_eeprom = std::format(
        "/sys/bus/i2c/devices/{}-10{:02x}/slave-eeprom", data_bus, DATA_ADDR);

    // Wait up to 15s for EEPROM files (created by i2c-boot-progress)
    for (int i = 0; i < 3; ++i)
    {
        if (std::filesystem::exists(ctrl) &&
            std::filesystem::exists(data_eeprom))
            break;
        if (i == 2)
        {
            std::cerr << std::format(
                "i2c-dump-server: EEPROM files not ready ({}, {})\n", ctrl,
                data_eeprom);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds{5});
    }

    auto bus = sdbusplus::bus::new_default();
    std::cerr << std::format("i2c-dump-server: ready. ctrl={} data={}\n", ctrl,
                             data_eeprom);

    // Poll every 1s for a command from the FPGA
    while (true)
    {
        if (eeprom_read_byte(ctrl, 18) == 0x01)
            process_command(bus, ctrl, data_eeprom);
        std::this_thread::sleep_for(std::chrono::seconds{1});
    }
}
