// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// i2c-boot-progress: HMC I2C boot telemetry and command daemon.
// Replaces i2c-boot-progress.sh + i2c-slave-config.sh.
//
// Usage: i2c-boot-progress [ro_bus [rw_bus]]
//   Standard HMC:   ro_bus=3,  rw_bus=0  (EEPROMs: 3-104f, 0-1045)
//   AST2700-a1 HMC: ro_bus=12, rw_bus=9  (EEPROMs: 12-104f, 9-1045)
//
// Telemetry EEPROM (addr 0x4f, {ro_bus}-104f/slave-eeprom):
//   Byte 0:      0x00 = HMC Boot Complete flag
//   Bytes 1-5:   SW version (git hash nibbles from VERSION_ID)
//   Byte 6:      USB online (0=off, 1=online)
//   Bytes 7-10:  USB IP address (4 octets)
//   Byte 11:     Service status bitmask
//   Byte 12:     RWFS usage %
//   Byte 13:     USB hardware bits (bit0=UDC configured, bit1=g_ether,
//   bit2=usb0 up) Byte 14:     Filesystem recovery status Byte 15: Additional
//   service status bitmask Bytes 16-19: HMC uptime, BE32 seconds Byte 24: App
//   version (0x05)
//
// Control EEPROM (addr 0x45, {rw_bus}-1045/slave-eeprom):
//   Byte 0: Command opcode written by FPGA; cleared to 0xff after dispatch

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/reboot.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <thread>
#include <variant>

namespace fs = std::filesystem;
using LogParams = std::map<std::string, std::string>;

// ── Bus / address configuration
// ───────────────────────────────────────────────
static unsigned RO_BUS = 3; // telemetry EEPROM bus (ast2700-a1: 12)
static unsigned RW_BUS = 0; // control EEPROM bus   (ast2700-a1: 9)

static constexpr uint8_t RO_ADDR = 0x4f;
static constexpr uint8_t RW_ADDR = 0x45;
static constexpr auto RO_TYPE = "slave-24c512ro";
static constexpr auto RW_TYPE = "slave-24c02";
static constexpr uint8_t APP_VER = 0x05;
static constexpr int APP_VER_OFF = 24;
static constexpr int UPTIME_OFF = 16;
static constexpr int LOOP_S = 5;
static constexpr int EVENT_INTERVAL = 120; // seconds between event checks
static constexpr int FS_CHECK_DELAY =
    600; // seconds before first FS health check
static constexpr int MAX_RWFS_FULL = 3;
static constexpr int MAX_RECOVERY = 3;

static const std::string BMC_IP = "172.31.13.241";
static const std::string HMC_IP = "172.31.13.251";
static const std::string HMC_READY =
    "/sys/class/leds/hmc_ready_noled/brightness";
static const std::string SIG_FILE = "/var/check_signature";

// Kernel MCTP transport config, cached at startup
static bool g_mctp_kernel = false;
static bool g_mctp_usb_kernel = false;
static bool g_mctp_spi_kernel = false;

// ── EEPROM path helpers
// ───────────────────────────────────────────────────────
static std::string ro_eeprom()
{
    return std::format("/sys/bus/i2c/devices/{}-10{:02x}/slave-eeprom", RO_BUS,
                       RO_ADDR);
}
static std::string rw_eeprom()
{
    return std::format("/sys/bus/i2c/devices/{}-10{:02x}/slave-eeprom", RW_BUS,
                       RW_ADDR);
}

// ── EEPROM I/O
// ────────────────────────────────────────────────────────────────
static void eeprom_write(const std::string& path, std::size_t offset,
                         const uint8_t* data, std::size_t len)
{
    std::ofstream f{path, std::ios::binary | std::ios::in | std::ios::out};
    if (!f)
    {
        std::cerr << std::format("i2c-boot-progress: cannot open {}\n", path);
        return;
    }
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(reinterpret_cast<const char*>(data),
            static_cast<std::streamsize>(len));
}

static uint8_t eeprom_read_byte(const std::string& path, std::size_t offset)
{
    std::ifstream f{path, std::ios::binary};
    if (!f)
        return 0xff;
    f.seekg(static_cast<std::streamoff>(offset));
    uint8_t b = 0xff;
    f.read(reinterpret_cast<char*>(&b), 1);
    return b;
}

// ── sysfs helpers
// ─────────────────────────────────────────────────────────────
static std::string sysfs_read(const fs::path& path)
{
    std::ifstream f(path);
    if (!f)
        return {};
    std::string s;
    std::getline(f, s);
    return s;
}

static void sysfs_write(const std::string& path, const std::string& val)
{
    std::ofstream f(path);
    if (f)
        f << val;
}

// ── D-Bus: systemd unit helpers
// ───────────────────────────────────────────────
static bool is_unit_active(sdbusplus::bus_t& bus, const std::string& unit)
{
    try
    {
        auto m = bus.new_method_call(
            "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", "GetUnit");
        m.append(unit);
        auto r = bus.call(m);
        sdbusplus::message::object_path path;
        r.read(path);

        auto m2 =
            bus.new_method_call("org.freedesktop.systemd1", path.str.c_str(),
                                "org.freedesktop.DBus.Properties", "Get");
        m2.append(std::string("org.freedesktop.systemd1.Unit"),
                  std::string("ActiveState"));
        auto r2 = bus.call(m2);
        std::variant<std::string> state;
        r2.read(state);
        return std::get<std::string>(state) == "active";
    }
    catch (...)
    {
        return false;
    }
}

static void systemd_unit(sdbusplus::bus_t& bus, const std::string& method,
                         const std::string& unit)
{
    try
    {
        auto m = bus.new_method_call(
            "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", method.c_str());
        m.append(unit, std::string("replace"));
        bus.call_noreply(m);
    }
    catch (const std::exception& e)
    {
        std::cerr << std::format("i2c-boot-progress: {} {} failed: {}\n",
                                 method, unit, e.what());
    }
}

// ── D-Bus: Redfish event logging
// ──────────────────────────────────────────────
static void log_event(sdbusplus::bus_t& bus, const std::string& msg_id,
                      const std::string& severity, const LogParams& params)
{
    try
    {
        auto m = bus.new_method_call(
            "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
            "xyz.openbmc_project.Logging.Create", "Create");
        m.append(msg_id, severity, params);
        bus.call_noreply(m);
    }
    catch (...)
    {}
}

static void log_resource_event(sdbusplus::bus_t& bus,
                               const std::string& resource,
                               const std::string& error, bool critical = true)
{
    const auto sev =
        critical ? "xyz.openbmc_project.Logging.Entry.Level.Critical"
                 : "xyz.openbmc_project.Logging.Entry.Level.Informational";
    const auto msg = critical ? "ResourceEvent.1.0.ResourceErrorsDetected"
                              : "ResourceEvent.1.0.ResourceErrorsCorrected";
    LogParams p{{"REDFISH_MESSAGE_ID", msg},
                {"REDFISH_MESSAGE_ARGS", "HMC " + resource + "," + error}};
    if (critical)
        p["xyz.openbmc_project.Logging.Entry.Resolution"] =
            "If problem persists, try restarting HMC";
    log_event(bus, msg, sev, p);
}

static void log_reboot_event(sdbusplus::bus_t& bus, const std::string& reason)
{
    log_event(bus, "OpenBMC.0.4.BMCRebootReason",
              "xyz.openbmc_project.Logging.Entry.Level.Informational",
              {{"REDFISH_MESSAGE_ID", "OpenBMC.0.4.BMCRebootReason"},
               {"REDFISH_MESSAGE_ARGS", reason}});
    std::this_thread::sleep_for(std::chrono::seconds{2});
}

// ── D-Bus: rsyslog config
// ─────────────────────────────────────────────────────
static bool is_rsyslog_active(sdbusplus::bus_t& bus)
{
    try
    {
        auto get = [&](const std::string& iface, const std::string& prop) {
            auto m = bus.new_method_call(
                "xyz.openbmc_project.Syslog.Config",
                "/xyz/openbmc_project/logging/config/remote",
                "org.freedesktop.DBus.Properties", "Get");
            m.append(iface, prop);
            return bus.call(m);
        };
        auto ar = get("xyz.openbmc_project.Network.Client", "Address");
        std::variant<std::string> av;
        ar.read(av);
        auto pr = get("xyz.openbmc_project.Network.Client", "Port");
        std::variant<uint16_t> pv;
        pr.read(pv);
        return std::get<std::string>(av) == BMC_IP &&
               std::get<uint16_t>(pv) == 6514;
    }
    catch (...)
    {
        return false;
    }
}

static void control_remote_logging(sdbusplus::bus_t& bus, bool enable)
{
    auto set = [&](const std::string& iface, const std::string& prop,
                   auto val) {
        auto m =
            bus.new_method_call("xyz.openbmc_project.Syslog.Config",
                                "/xyz/openbmc_project/logging/config/remote",
                                "org.freedesktop.DBus.Properties", "Set");
        m.append(iface, prop, std::variant<decltype(val)>(val));
        bus.call_noreply(m);
    };
    if (enable)
    {
        set("xyz.openbmc_project.Logging.RsyslogClient", "Enabled", true);
        set("xyz.openbmc_project.Logging.RsyslogClient", "Severity",
            std::string(
                "xyz.openbmc_project.Logging.RsyslogClient.SeverityType.All"));
        set("xyz.openbmc_project.Network.Client", "Port", uint16_t(6514));
        set("xyz.openbmc_project.Network.Client", "Address",
            std::string(BMC_IP));
    }
    else
    {
        set("xyz.openbmc_project.Logging.RsyslogClient", "Enabled", false);
        set("xyz.openbmc_project.Network.Client", "Port", uint16_t(0));
        set("xyz.openbmc_project.Network.Client", "Address", std::string(""));
    }
}

// ── External tool wrappers
// ────────────────────────────────────────────────────
static std::string fw_getenv(const std::string& key)
{
    auto cmd = std::format("fw_printenv -n {} 2>/dev/null", key);
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp)
        return {};
    char buf[256] = {};
    fgets(buf, sizeof(buf), fp);
    pclose(fp);
    std::string s(buf);
    if (!s.empty() && s.back() == '\n')
        s.pop_back();
    return s;
}

static void fw_setenv(const std::string& key, const std::string& val = {})
{
    auto cmd = val.empty() ? std::format("fw_setenv {}", key)
                           : std::format("fw_setenv {} {}", key, val);
    std::system(cmd.c_str());
}

static std::string find_mtd(const std::string& name)
{
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/mtd", ec))
        if (sysfs_read(e.path() / "name") == name)
            return e.path().filename().string(); // e.g. "mtd6"
    return {};
}

static bool kernel_has(const std::string& cfg)
{
    auto cmd =
        std::format("zcat /proc/config.gz 2>/dev/null | grep -q '{}=y'", cfg);
    return std::system(cmd.c_str()) == 0;
}

static bool is_mounted(const std::string& path)
{
    std::ifstream f("/proc/mounts");
    std::string line;
    while (std::getline(f, line))
        if (line.find(' ' + path + ' ') != std::string::npos)
            return true;
    return false;
}

// ── I2C slave creation
// ────────────────────────────────────────────────────────
static void create_i2c_slaves()
{
    auto check_and_create = [](unsigned bus, uint8_t addr, const char* type) {
        auto slave = std::format("/sys/bus/i2c/devices/i2c-{}/{}-10{:02x}/name",
                                 bus, bus, addr);
        if (!fs::exists(slave))
            sysfs_write(std::format("/sys/bus/i2c/devices/i2c-{}/new_device",
                                    bus),
                        std::format("{} 0x10{:02x}\n", type, addr));
    };
    check_and_create(RO_BUS, RO_ADDR, RO_TYPE);
    check_and_create(RW_BUS, RW_ADDR, RW_TYPE);
}

// ── SW version parsing
// ────────────────────────────────────────────────────────
// VERSION_ID=core-25.04-rc1-23-g1a2b3c4d5.build → extracts bytes from hash
static std::array<uint8_t, 5> parse_sw_version()
{
    std::array<uint8_t, 5> ver{};
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("VERSION_ID=", 0) != 0)
            continue;
        auto s = line.substr(11);
        auto dirty = s.find("-dirty");
        if (dirty != std::string::npos)
            s.erase(dirty, 6);

        // Split by '-', take last segment
        std::string last;
        for (std::size_t p = 0, q;; p = q + 1)
        {
            q = s.find('-', p);
            last =
                s.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (q == std::string::npos)
                break;
        }
        // last = "g1a2b3c4d5.builddate" — strip after '.'
        auto dot = last.find('.');
        std::string hash =
            (dot != std::string::npos) ? last.substr(0, dot) : last;

        auto hex = [&](std::size_t pos, std::size_t len) -> uint8_t {
            if (hash.size() <= pos)
                return 0;
            try
            {
                return static_cast<uint8_t>(
                    std::stoul(hash.substr(pos, len), nullptr, 16));
            }
            catch (...)
            {
                return 0;
            }
        };
        // hash starts with 'g'; extract nibble at [1], bytes at [2],[4],[6],[8]
        ver[0] = hex(1, 1);
        ver[1] = hex(2, 2);
        ver[2] = hex(4, 2);
        ver[3] = hex(6, 2);
        ver[4] = hex(8, 2);
        break;
    }
    return ver;
}

// ── Telemetry write
// ───────────────────────────────────────────────────────────
struct Telemetry
{
    std::array<uint8_t, 5> ver{};
    uint8_t usb_online = 0;
    uint8_t usb_ip[4] = {};
    uint8_t svc_status = 0;
    uint8_t rwfs_usage = 0;
    uint8_t usb_hw_bits = 0;
    uint8_t fs_recovery = 0;
    uint8_t svc_status2 = 0;
};

static void write_telemetry(const Telemetry& t)
{
    // Bytes 0-15 only: byte 16 is the MSB of the BE32 uptime at UPTIME_OFF,
    // which write_uptime() owns. Writing past byte 15 here zeroes it on
    // every loop.
    uint8_t buf[UPTIME_OFF] = {};
    buf[0] = 0x00; // HMC Boot Complete flag
    for (int i = 0; i < 5; ++i)
        buf[1 + i] = t.ver[i];
    buf[6] = t.usb_online;
    buf[7] = t.usb_ip[0];
    buf[8] = t.usb_ip[1];
    buf[9] = t.usb_ip[2];
    buf[10] = t.usb_ip[3];
    buf[11] = t.svc_status;
    buf[12] = t.rwfs_usage;
    buf[13] = t.usb_hw_bits;
    buf[14] = t.fs_recovery;
    buf[15] = t.svc_status2;
    eeprom_write(ro_eeprom(), 0, buf, sizeof(buf));
}

static void write_uptime()
{
    double s = 0;
    {
        std::ifstream f("/proc/uptime");
        f >> s;
    }
    auto u = static_cast<uint32_t>(s);
    uint8_t b[4] = {uint8_t(u >> 24), uint8_t(u >> 16), uint8_t(u >> 8),
                    uint8_t(u)};
    eeprom_write(ro_eeprom(), UPTIME_OFF, b, 4);
}

// ── Telemetry update functions
// ────────────────────────────────────────────────
static void update_usb(Telemetry& t)
{
    t.usb_online = 0;
    std::fill(std::begin(t.usb_ip), std::end(t.usb_ip), uint8_t(0));

    struct ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
        return;

    for (auto* ifa = list; ifa; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (std::string_view(ifa->ifa_name) != "usb0")
            continue;
        auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        uint32_t addr = ntohl(sin->sin_addr.s_addr);
        uint8_t b0 = addr >> 24, b1 = (addr >> 16) & 0xff;
        uint8_t b2 = (addr >> 8) & 0xff, b3 = addr & 0xff;

        if (b0 == 169 && b1 == 254)
        {
            // Link-local only — add static IP
            std::cerr
                << "i2c-boot-progress: link-local on usb0, adding static IP\n";
            std::system(("ip addr replace " + HMC_IP + "/24 dev usb0").c_str());
        }
        else
        {
            t.usb_online = 1;
            t.usb_ip[0] = b0;
            t.usb_ip[1] = b1;
            t.usb_ip[2] = b2;
            t.usb_ip[3] = b3;
        }
        break;
    }
    freeifaddrs(list);
}

static void update_services(sdbusplus::bus_t& bus, Telemetry& t)
{
    // Bit 6 always set: indicates remote logging is supported
    uint8_t s = 0x40;
    if (is_unit_active(bus, "bmcweb.service"))
        s |= (1 << 0);
    if (is_unit_active(bus, "pldmd.service"))
        s |= (1 << 1);
    if (is_unit_active(bus, "nsmd.service"))
        s |= (1 << 2);
    if (g_mctp_kernel)
    {
        if (is_unit_active(bus, "mctpd.service"))
            s |= (1 << 3);
        if (is_unit_active(bus, "xyz.openbmc_project.mctpreactor.service"))
            s |= (1 << 4);
    }
    else
    {
        if (is_unit_active(bus, "mctp-usb-ctrl.service"))
            s |= (1 << 3);
        if (is_unit_active(bus, "mctp-spi0-ctrl.service"))
            s |= (1 << 4);
    }
    if (is_rsyslog_active(bus))
        s |= (1 << 7);
    t.svc_status = s;

    uint8_t s2 = 0;
    if (is_unit_active(bus, "xyz.openbmc_project.Dump.Manager.service"))
        s2 |= (1 << 0);
    if (!g_mctp_usb_kernel && is_unit_active(bus, "mctp-usb-demux.service"))
        s2 |= (1 << 1);
    if (!g_mctp_spi_kernel && is_unit_active(bus, "mctp-spi0-demux.service"))
        s2 |= (1 << 2);
    t.svc_status2 = s2;
}

static void update_diagnostics(Telemetry& t, bool& rwfs_full)
{
    // RWFS usage via statvfs on the mount point
    struct statvfs sv;
    if (statvfs("/run/initramfs/rw", &sv) == 0 && sv.f_blocks > 0)
    {
        uint64_t used = sv.f_blocks - sv.f_bfree;
        t.rwfs_usage = static_cast<uint8_t>((used * 100) / sv.f_blocks);
        rwfs_full = (t.rwfs_usage >= 99);
    }

    // USB hardware bits
    uint8_t hw = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/udc", ec))
    {
        if (sysfs_read(e.path() / "state").find("configured") !=
            std::string::npos)
            hw |= 1;
        if (sysfs_read(e.path() / "uevent").find("g_ether") !=
            std::string::npos)
            hw |= 2;
    }
    if (sysfs_read("/sys/class/net/usb0/operstate").find("up") !=
        std::string::npos)
        hw |= 4;
    t.usb_hw_bits = hw;
}

// ── Command dispatch
// ───────────────────────────────────────────────────────────
static void reset_network_config()
{
    static const std::string src = "/run/initramfs/rw/cow/etc/systemd/network/";
    static const std::string ro = "/run/initramfs/ro/etc/systemd/network/";
    static const std::string dest = "/etc/systemd/network/";
    std::error_code ec;
    fs::create_directories(dest, ec);
    for (const auto& e : fs::directory_iterator(src, ec))
    {
        auto ro_file = ro + e.path().filename().string();
        if (fs::exists(ro_file, ec))
            fs::copy_file(ro_file, dest + e.path().filename().string(),
                          fs::copy_options::overwrite_existing, ec);
    }
}

static void process_command(sdbusplus::bus_t& bus)
{
    uint8_t opcode = eeprom_read_byte(rw_eeprom(), 0);
    if (opcode == 0xff)
        return;

    std::cerr << std::format("i2c-boot-progress: opcode 0x{:02x}\n", opcode);
    switch (opcode)
    {
        case 0x01:
            log_reboot_event(bus,
                             "Reboot triggered by external command over I2C");
            ::reboot(RB_AUTOBOOT);
            break;
        case 0x02:
            systemd_unit(bus, "StopUnit", "systemd-networkd.service");
            reset_network_config();
            systemd_unit(bus, "StartUnit", "systemd-networkd.service");
            break;
        case 0x03:
            control_remote_logging(bus, true);
            break;
        case 0x04:
            control_remote_logging(bus, false);
            break;
        case 0x05:
            systemd_unit(bus, "StartUnit", "i2c-dump-server.service");
            break;
        case 0x06:
            systemd_unit(bus, "StopUnit", "i2c-dump-server.service");
            break;
        case 0x0e:
            systemd_unit(bus, "StopUnit",
                         "xyz.openbmc_project.Logging.service");
            systemd_unit(bus, "StopUnit",
                         "xyz.openbmc_project.Dump.Manager.service");
            std::system("umount /dev/mtdblock6 2>/dev/null");
            std::system("umount /var/lib/logging  2>/dev/null");
            std::system("flash_eraseall -q /dev/mtd6");
            fw_setenv("openbmclog", "dataflash-erase");
            fw_setenv("emmc_secure_erase_partition", "/var/emmc/user-logs");
            std::this_thread::sleep_for(std::chrono::seconds{5});
            ::reboot(RB_AUTOBOOT);
            break;
        case 0x0f:
            try
            {
                auto m = bus.new_method_call(
                    "xyz.openbmc_project.Software.BMC.Inventory",
                    "/xyz/openbmc_project/software/bmc",
                    "xyz.openbmc_project.Common.FactoryReset", "Reset");
                bus.call_noreply(m);
            }
            catch (...)
            {}
            log_reboot_event(
                bus, "Factory Reset triggered by external command over I2C");
            ::reboot(RB_AUTOBOOT);
            break;
        case 0xd0:
        {
            std::ofstream f(SIG_FILE);
        } // touch
        break;
        default:
            std::cerr << std::format(
                "i2c-boot-progress: unknown opcode 0x{:02x}\n", opcode);
    }

    uint8_t done = 0xff;
    eeprom_write(rw_eeprom(), 0, &done, 1);
}

// ── FS health check
// ───────────────────────────────────────────────────────────
static void run_fs_health_check(sdbusplus::bus_t& bus, Telemetry& t,
                                bool rwfs_full, int& rwfs_full_cnt,
                                int& fs_delay_cnt, bool& fs_checked)
{
    if (rwfs_full)
        ++rwfs_full_cnt;
    else
        rwfs_full_cnt = 0;
    if (rwfs_full_cnt >= MAX_RWFS_FULL)
    {
        std::system("umount /dev/mtdblock6 2>/dev/null");
        std::system("flash_eraseall -q /dev/mtd6");
        log_resource_event(bus, "Rwfs", "Resource Exhaustion", false);
        std::this_thread::sleep_for(std::chrono::seconds{5});
        log_reboot_event(bus, "Auto Recovery");
        ::reboot(RB_AUTOBOOT);
    }

    fs_delay_cnt += LOOP_S;
    if (fs_delay_cnt <= FS_CHECK_DELAY || fs_checked)
        return;
    fs_delay_cnt = 0;

    bool rw_ok = is_mounted("/run/initramfs/rw");
    bool log_ok = is_mounted("/var/lib/logging");

    uint8_t bits = 0;
    if (!rw_ok)
        bits |= 0x81;
    if (!log_ok)
        bits |= 0x82;
    t.fs_recovery = bits;
    write_telemetry(t);

    bool recovered = false;
    if (!rw_ok)
    {
        std::system("flash_eraseall -q /dev/mtd6");
        recovered = true;
    }
    if (!log_ok)
    {
        auto log = find_mtd("log");
        if (!log.empty())
            std::system(("flash_eraseall -q /dev/" + log).c_str());
        fw_setenv("emmc_secure_erase_partition", "/var/emmc/user-logs");
        recovered = true;
    }

    // fw_printenv output is not trusted. The whole string must parse - a
    // numeric prefix followed by junk is a bad value, not a count - and a
    // negative would sit below MAX_RECOVERY and hand out extra reboot
    // cycles. fw_getenv() has already stripped the trailing newline, so
    // anything left over is real junk. Treat any invalid value as "counter
    // unavailable" and fall through to the no-counter path.
    auto count_str = fw_getenv("rwfs_recovery_count");
    bool can_count = !count_str.empty();
    int count = 0;
    if (can_count)
    {
        const char* begin = count_str.data();
        const char* end = begin + count_str.size();
        auto [ptr, ec] = std::from_chars(begin, end, count);
        if (ec != std::errc{} || ptr != end || count < 0)
        {
            std::cerr << std::format(
                "i2c-boot-progress: bad rwfs_recovery_count '{}'\n", count_str);
            can_count = false;
            count = 0;
        }
    }

    if (recovered && can_count && count < MAX_RECOVERY)
    {
        fw_setenv("dataflashrecovery", "yes");
        fw_setenv("rwfs_recovery_count", std::to_string(count + 1));
        std::this_thread::sleep_for(std::chrono::seconds{5});
        std::system("systemctl reboot --force");
        return;
    }

    fs_checked = true;

    if (can_count && count > 0 && !recovered)
    {
        log_resource_event(
            bus, "Rwfs",
            "Filesystem is healthy after previous recovery attempts resetting counter",
            false);
        fw_setenv("rwfs_recovery_count", "0");
        return;
    }
    if (!can_count && recovered)
    {
        log_resource_event(bus, "Rwfs",
                           "System has recovered using the recovery filesystem",
                           true);
    }
    else if (can_count && count >= MAX_RECOVERY)
    {
        log_resource_event(
            bus, "Rwfs",
            "Recovery failed and stopped due to attempts reaching the limit",
            true);
    }
}

// ── Event check (runs every EVENT_INTERVAL seconds) ──────────────────────────
static void run_event_check(sdbusplus::bus_t& bus, int& event_cnt,
                            bool& events_fired, int& fs_prev_usage,
                            bool& usage_warned)
{
    event_cnt += LOOP_S;
    if (event_cnt < EVENT_INTERVAL)
        return;
    event_cnt = 0;

    write_uptime();

    // HMC ready pin check
    bool hmc_ready = sysfs_read(HMC_READY).find('1') != std::string::npos;
    if (!hmc_ready && !events_fired)
    {
        log_resource_event(bus, "Ready", "Service Unavailable");

        if (sysfs_read("/sys/class/net/usb0/operstate").find("up") ==
            std::string::npos)
            log_resource_event(bus, "USB Connection", "Service Unavailable");

        // Check for routable (non-link-local) IP
        bool routable = false;
        struct ifaddrs* list = nullptr;
        if (getifaddrs(&list) == 0)
        {
            for (auto* ifa = list; ifa; ifa = ifa->ifa_next)
            {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                    continue;
                if (std::string_view(ifa->ifa_name) != "usb0")
                    continue;
                auto* sin =
                    reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
                uint32_t addr = ntohl(sin->sin_addr.s_addr);
                uint8_t b0 = addr >> 24, b1 = (addr >> 16) & 0xff;
                routable = !(b0 == 169 && b1 == 254);
                break;
            }
            freeifaddrs(list);
        }
        if (!routable)
            log_resource_event(bus, "USB Network", "Service Unavailable");

        if (!is_unit_active(bus, "bmcweb.service"))
            log_resource_event(bus, "Redfish", "Service Unavailable");

        events_fired = true;
    }
    else if (hmc_ready)
    {
        events_fired = false;
    }

    // Log partition usage (90% threshold, re-arms at 85%)
    struct statvfs sv;
    if (statvfs("/var/lib/logging", &sv) == 0 && sv.f_blocks > 0)
    {
        int cur =
            static_cast<int>((sv.f_blocks - sv.f_bfree) * 100 / sv.f_blocks);
        if (cur >= 90 && fs_prev_usage <= 89 && fs_prev_usage >= 1 &&
            usage_warned)
        {
            LogParams p{{"REDFISH_MESSAGE_ID",
                         "ResourceEvent.1.0.ResourceWarningThresholdExceeded"},
                        {"REDFISH_MESSAGE_ARGS", "HMC Log Storage,90% Usage"}};
            log_event(bus, "ResourceEvent.1.0.ResourceStatusChangedWarning",
                      "xyz.openbmc_project.Logging.Entry.Level.Informational",
                      p);
            usage_warned = false;
        }
        if (cur <= 85)
            usage_warned = true;
        fs_prev_usage = cur;
    }
}

// ── Startup: check previous recovery flag
// ─────────────────────────────────────
static void check_fs_recovery(sdbusplus::bus_t& bus)
{
    if (fw_getenv("dataflashrecovery") != "yes")
        return;
    std::this_thread::sleep_for(std::chrono::seconds{10});
    log_resource_event(bus, "Flash", "Data Corruption", false);
    fw_setenv("dataflashrecovery"); // clear
}

// ── Main
// ──────────────────────────────────────────────────────────────────────
int main(int argc, char** argv)
{
    if (argc >= 2)
        try
        {
            RO_BUS = std::stoul(argv[1]);
        }
        catch (...)
        {}
    if (argc >= 3)
        try
        {
            RW_BUS = std::stoul(argv[2]);
        }
        catch (...)
        {}

    std::this_thread::sleep_for(std::chrono::seconds{5});

    g_mctp_kernel = kernel_has("CONFIG_MCTP");
    g_mctp_usb_kernel = kernel_has("CONFIG_MCTP_TRANSPORT_USB");
    g_mctp_spi_kernel = kernel_has("CONFIG_MCTP_TRANSPORT_SPI");

    create_i2c_slaves();

    auto bus = sdbusplus::bus::new_default();
    check_fs_recovery(bus);

    auto ver = parse_sw_version();
    uint8_t app_ver = APP_VER;
    eeprom_write(ro_eeprom(), APP_VER_OFF, &app_ver, 1);
    fs::remove(SIG_FILE);

    Telemetry t;
    t.ver = ver;

    bool rwfs_full = false;
    int rwfs_full_cnt = 0;
    int fs_delay_cnt = 0;
    bool fs_checked = false;
    int event_cnt = 0;
    bool events_fired = false;
    int fs_prev_usage = 0;
    bool usage_warned = true;

    std::cerr << std::format("i2c-boot-progress: ready. ro_bus={} rw_bus={}\n",
                             RO_BUS, RW_BUS);

    while (true)
    {
        update_usb(t);
        update_services(bus, t);
        update_diagnostics(t, rwfs_full);
        write_telemetry(t);
        process_command(bus);
        run_fs_health_check(bus, t, rwfs_full, rwfs_full_cnt, fs_delay_cnt,
                            fs_checked);
        run_event_check(bus, event_cnt, events_fired, fs_prev_usage,
                        usage_warned);
        std::this_thread::sleep_for(std::chrono::seconds{LOOP_S});
    }
}
