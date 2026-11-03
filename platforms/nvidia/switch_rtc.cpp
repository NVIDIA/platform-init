// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

#include "switch_rtc.hpp"

#include "i2c.hpp"
#include "nvidia_platform_utils.hpp"
#include "utilities.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>

namespace nvidia
{

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{

template <typename... Args>
void say(std::format_string<Args...> fmt, Args&&... args)
{
    std::cerr << std::format(fmt, std::forward<Args>(args)...) << '\n';
}

constexpr std::string_view aspeed_rtc = "1e781000.rtc";

bool write_sysfs(const std::string& path, std::string_view value)
{
    std::ofstream f(path);
    f << value << std::flush;
    return f.good();
}

void try_unbind_aspeed_rtc()
{
    const std::string dev =
        std::format("/sys/devices/platform/ahb/1e780000.apb/{}", aspeed_rtc);
    const bool dev_exists = fs::is_directory(dev);
    const std::array<std::pair<std::string, std::string_view>, 3> paths{{
        {dev_exists ? dev + "/driver/unbind" : "", ""},
        {"/sys/bus/platform/drivers/aspeed-rtc/unbind", " via aspeed-rtc"},
        {"/sys/bus/platform/drivers/ast2600-rtc/unbind", " via ast2600-rtc"},
    }};
    for (const auto& [path, how] : paths)
    {
        if (fs::exists(path) &&
            write_sysfs(path, std::format("{}\n", aspeed_rtc)))
        {
            say("Unbound Aspeed RTC{}", how);
            sleep_milliseconds(1s);
            return;
        }
    }
    if (dev_exists)
        say("Could not unbind Aspeed RTC; external RTC may register "
            "as rtc1 (will use /dev/rtc symlink)");
}

void remove_i2c_rtc_device(const std::string& bus, unsigned addr)
{
    const std::string name = std::format("{}-{:04x}", bus, addr);
    const std::string unbind =
        "/sys/bus/i2c/devices/" + name + "/driver/unbind";
    const std::string del =
        std::format("/sys/class/i2c-dev/i2c-{}/device/delete_device", bus);
    if (!fs::is_directory("/sys/bus/i2c/devices/" + name))
        return;
    say("Resetting stale I2C RTC client {}", name);
    for (const auto& [path, value] :
         {std::pair{unbind, name + "\n"},
          std::pair{del, std::format("{:#04x}\n", addr)}})
    {
        if (fs::exists(path))
        {
            write_sysfs(path, value);
            sleep_milliseconds(1s);
        }
    }
}

// /dev/rtcN whose sysfs name is <name> or starts with "<name> ".
std::string find_external_rtc(const std::string& name)
{
    std::vector<std::string> dirs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/rtc", ec))
        dirs.push_back(e.path().filename().string());
    std::ranges::sort(dirs);
    for (const auto& d : dirs)
    {
        std::ifstream f(std::format("/sys/class/rtc/{}/name", d));
        std::string line;
        if (d.starts_with("rtc") && std::getline(f, line) &&
            (line == name || line.starts_with(name + " ")))
            return "/dev/" + d;
    }
    return {};
}

// i2cget / i2cset -f -y <bus> <addr> <reg> [<val>]: same transactions.
int reg_io(unsigned bus, uint8_t addr, uint8_t reg, int val = -1)
{
    std::array<uint8_t, 2> w{reg, static_cast<uint8_t>(val)};
    std::array<uint8_t, 1> r{};
    bool rd = val < 0;
    if (i2c::raw_transfer(bus, addr, std::span(w).first(rd ? 1 : 2),
                          std::span(r).first(rd ? 1 : 0)))
        return -1;
    return rd ? r[0] : 0;
}

// Clear OF/RTCF/CIF (reg 0x0b) and the STOP bit (reg 0x0a).
void recover_pcf85053a_flags(unsigned bus, uint8_t addr)
{
    int status = reg_io(bus, addr, 0x0b);
    if (status < 0)
    {
        say("RTC: warning: could not read status register; skipping "
            "flag recovery");
        return;
    }
    if ((status & 0x70) == 0)
    {
        say("RTC: State is healthy (Reg 0x0b = {}).", status);
        return;
    }
    say("RTC: unreliable time flags detected (Reg 0x0b = {}). Clearing...",
        status);
    int ctrl = -1;
    const char* warning = nullptr;
    if (reg_io(bus, addr, 0x0b, status & 0x07) < 0)
        warning = "could not clear status register";
    else if ((ctrl = reg_io(bus, addr, 0x0a)) < 0)
        warning = "could not read control register after status clear";
    else if (ctrl & 0x80)
    {
        say("RTC: STOP bit detected. Restarting oscillator...");
        if (reg_io(bus, addr, 0x0a, ctrl & ~0x80) < 0)
            warning = "could not clear STOP bit";
    }
    std::cerr << (warning ? std::format("RTC: warning: {}\n", warning)
                          : "RTC: Flags cleared.\n");
}

int setup_external_rtc(const std::string& bus, const RtcEntry& e)
{
    auto addr = static_cast<uint8_t>(std::stoul(e.addr, nullptr, 16));
    try_unbind_aspeed_rtc();

    std::string rtc = find_external_rtc(e.name);
    if (!rtc.empty())
        say("{} already registered at {}", e.name, rtc);
    else
    {
        remove_i2c_rtc_device(bus, addr);
        if (!write_sysfs(
                std::format("/sys/class/i2c-dev/i2c-{}/device/new_device", bus),
                std::format("{} {}\n", e.name, e.addr)))
        {
            say("{} rtc driver not bind", e.name);
            return 1;
        }
        sleep_milliseconds(2s);
        rtc = find_external_rtc(e.name);
    }
    if (rtc.empty())
    {
        say("{} rtc device did not appear under /sys/class/rtc", e.name);
        return 1;
    }
    if (rtc == "/dev/rtc0")
        say("{} registered as rtc0", e.name);
    else
    {
        say("{} is {}; redirecting /dev/rtc", e.name, rtc);
        std::error_code ec;
        fs::remove("/dev/rtc", ec);
        fs::create_symlink(rtc.substr(5), "/dev/rtc", ec);
    }
    if (e.name == "pcf85053a")
        recover_pcf85053a_flags(std::stoul(bus), addr);
    // rtc-detection.service runs hwclock --hctosys -u -f <this device>.
    return write_sysfs(std::string(rtc_sync_file), rtc) ? 0 : 1;
}

} // namespace

bool is_i2c_hex_byte(std::string_view s)
{
    return s.size() >= 3 && s.size() <= 4 && s.starts_with("0x") &&
           std::ranges::all_of(s.substr(2), ::isxdigit);
}

RtcConfig parse_rtc_config(std::istream& in)
{
    RtcConfig cfg;
    for (std::string line, key; std::getline(in, line);)
    {
        std::istringstream words(line.substr(0, line.find('#')));
        RtcEntry e;
        words >> key;
        if (key == "bus" && words >> e.addr)
            cfg.buses.push_back(e.addr);
        else if (key == "rtc" &&
                 words >> e.addr >> e.vendor_reg >> e.dev_id >> e.name)
            cfg.devices.push_back(e);
        key.clear();
    }
    return cfg;
}

int switch_rtc_detect()
{
    std::error_code ec;
    fs::remove(rtc_sync_file, ec);
    std::ifstream f{"/etc/rtc-detection.conf"};
    if (!f)
        return 0;
    RtcConfig cfg = parse_rtc_config(f);

    std::vector<unsigned> buses;
    for (const auto& bus : cfg.buses)
        if (std::ranges::all_of(bus, ::isdigit))
            buses.push_back(std::stoul(bus));
    std::vector<RtcEntry> valid;
    std::vector<RtcId> table;
    for (const auto& e : cfg.devices)
    {
        if (!is_i2c_hex_byte(e.addr) || !is_i2c_hex_byte(e.vendor_reg) ||
            !is_i2c_hex_byte(e.dev_id))
        {
            say("rtc-detection: skipping entry with invalid I2C values for {}",
                e.name);
            continue;
        }
        // The script compared the ID as text with i2ctransfer's "0x%02x".
        int id = std::stoi(e.dev_id, nullptr, 16);
        valid.push_back(e);
        table.push_back(
            {static_cast<uint8_t>(std::stoul(e.addr, nullptr, 16)),
             static_cast<uint8_t>(std::stoul(e.vendor_reg, nullptr, 16)),
             std::format("{:#04x}", id) == e.dev_id ? id : -1, e.name});
    }
    if (auto found = detect_rtc(buses, table, true))
    {
        const RtcEntry& e = valid[found->index];
        say("{} rtc device detected on i2c-{}@{}", e.name, found->bus, e.addr);
        return setup_external_rtc(std::to_string(found->bus), e);
    }
    say("rtc device was not found");
    return 0;
}

} // namespace nvidia
