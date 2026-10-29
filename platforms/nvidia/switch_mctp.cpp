// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

// mctp-interface-setup.sh up to its link loop; the unit then runs the
// script's mctp and ip commands on each mctpusb link.

#include "switch_mctp.hpp"

#include "nvidia_platform_utils.hpp"
#include "switch_common.hpp"
#include "switch_mcu.hpp"

#include <glob.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>
#include <variant>

namespace nvidia
{
namespace
{

void log(std::string_view msg)
{
    std::cerr << msg << '\n';
}

void sleep_s(int s)
{
    std::this_thread::sleep_for(std::chrono::seconds{s});
}

std::string ts()
{
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    tm local{};
    localtime_r(&now.tv_sec, &local);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%F %T", &local);
    std::string mono;
    std::ifstream{"/proc/uptime"} >> mono;
    return std::format("{}.{:03} (mono={}s)", buf, now.tv_nsec / 1000000, mono);
}

// Bash glob: sorted matches, or the pattern itself when nothing matches.
std::vector<std::string> expand(const std::string& pattern)
{
    glob_t g{};
    std::vector<std::string> out;
    if (glob(pattern.c_str(), 0, nullptr, &g) == 0)
        out.assign(g.gl_pathv, g.gl_pathv + g.gl_pathc);
    else
        out.push_back(pattern);
    globfree(&g);
    return out;
}

// Every name `ip link show` prints per link: name, alias and altnames.
std::vector<std::string> link_names()
{
    std::vector<std::string> out;
    const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    struct
    {
        nlmsghdr nh;
        ifinfomsg ifi;
    } req{};
    req.nh.nlmsg_len = sizeof(req);
    req.nh.nlmsg_type = RTM_GETLINK;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    if (fd < 0 || send(fd, &req, sizeof(req), 0) < 0)
    {
        if (fd >= 0)
            close(fd);
        return out;
    }
    alignas(nlmsghdr) char buf[32768];
    for (bool done = false; !done;)
    {
        auto n = recv(fd, buf, sizeof(buf), 0);
        done = n <= 0;
        for (auto* h = reinterpret_cast<nlmsghdr*>(buf);
             !done && NLMSG_OK(h, static_cast<unsigned>(n));
             h = NLMSG_NEXT(h, n))
        {
            done = h->nlmsg_type != RTM_NEWLINK;
            auto* ifi = static_cast<ifinfomsg*>(NLMSG_DATA(h));
            int len = static_cast<int>(IFLA_PAYLOAD(h));
            for (auto* a = IFLA_RTA(ifi); !done && RTA_OK(a, len);
                 a = RTA_NEXT(a, len))
            {
                const auto type = a->rta_type & NLA_TYPE_MASK;
                if (type == IFLA_IFNAME || type == IFLA_IFALIAS)
                    out.emplace_back(static_cast<const char*>(RTA_DATA(a)));
                if (type != IFLA_PROP_LIST)
                    continue;
                int plen = static_cast<int>(RTA_PAYLOAD(a));
                for (auto* p = static_cast<rtattr*>(RTA_DATA(a));
                     RTA_OK(p, plen); p = RTA_NEXT(p, plen))
                    if ((p->rta_type & NLA_TYPE_MASK) == IFLA_ALT_IFNAME)
                        out.emplace_back(static_cast<const char*>(RTA_DATA(p)));
            }
        }
    }
    close(fd);
    return out;
}

// ip link show | grep -c mctpusb
int interface_count()
{
    return static_cast<int>(
        std::ranges::count_if(link_names(), [](const std::string& s) {
            return s.contains("mctpusb");
        }));
}

void snapshot(const MctpPaths& paths, const std::string& pg_glob,
              std::string_view phase)
{
    log(std::format("USB/MCU state [{}] at {}", phase, ts()));
    std::ifstream conf{paths.usb_debug};
    const auto ports = detail::mcu_map_ports(
        std::string{std::istreambuf_iterator<char>{conf}, {}});
    for (const auto& port : ports)
    {
        const auto node = "/sys/bus/usb/devices/" + port;
        if (!std::filesystem::is_directory(node))
        {
            log(std::format("  port {}: ABSENT (not enumerated)", port));
            continue;
        }
        log(std::format("  port {}: present vid={} pid={} devnum={}", port,
                        detail::sysfs_read(node + "/idVendor").value_or(""),
                        detail::sysfs_read(node + "/idProduct").value_or(""),
                        detail::sysfs_read(node + "/devnum").value_or("")));
    }
    if (ports.empty())
        log("  (no USB MCU ports configured)");
    // grep -o 'mctpusb[^:@ ]*' | sort -u
    std::set<std::string> names;
    for (const auto& s : link_names())
        if (auto p = s.find("mctpusb"); p != std::string::npos)
            names.insert(s.substr(p, s.find_first_of(":@ ", p) - p));
    std::string ifaces;
    for (const auto& n : names)
        ifaces += n + " ";
    log(std::format("  mctpusb interfaces: {}",
                    ifaces.empty() ? "none" : ifaces));
    for (const auto& pg : expand(pg_glob))
        if (access(pg.c_str(), R_OK) == 0)
            log(std::format("  power-good {} = {}", pg,
                            detail::sysfs_read(pg).value_or("")));
}

std::string host_state()
{
    try
    {
        auto bus = sdbusplus::bus::new_default_system();
        auto call =
            bus.new_method_call("xyz.openbmc_project.State.Host",
                                "/xyz/openbmc_project/state/host0",
                                "org.freedesktop.DBus.Properties", "Get");
        call.append("xyz.openbmc_project.State.Host", "CurrentHostState");
        std::variant<std::string> value;
        bus.call(call).read(value);
        return std::get<std::string>(value);
    }
    catch (const std::exception&)
    {
        return {};
    }
}

bool host_cpu_intentionally_off()
{
    constexpr int retries = 30;
    std::string state;
    for (int retry = 0; retry < retries; ++retry)
    {
        state = host_state();
        if (!state.empty())
            break;
        if (retry + 1 < retries)
            sleep_s(1);
    }
    if (state.empty())
    {
        log(std::format("Host state unavailable after {} retries; continuing "
                        "with full setup",
                        retries));
        return false;
    }
    log("Read host state: " + state);
    return state.ends_with("HostState.Off") ||
           state.ends_with("TransitioningToOff");
}

bool wait_for_mcu_power_good(const std::string& conf, std::string& pg_glob)
{
    constexpr int max_wait = 25;
    log("Waiting for MCU power good...");
    if (!std::filesystem::is_regular_file(conf))
    {
        log("MCU PG config not found: " + conf);
        return false;
    }
    pg_glob = platform_feature("MCU_PG", conf);
    if (pg_glob.empty())
    {
        log("MCU_PG is not set or empty in " + conf);
        return false;
    }
    for (int elapsed = 0; elapsed < max_wait; ++elapsed)
    {
        const auto pg_file = expand(pg_glob).front();
        if (std::filesystem::is_regular_file(pg_file))
        {
            log("MCU power good file found: " + pg_file);
            if (detail::sysfs_read(pg_file).value_or("") == "0")
            {
                log(std::format("MCU power good detected at {} after {}s", ts(),
                                elapsed));
                return true;
            }
        }
        sleep_s(1);
    }
    log(std::format("Timeout waiting for MCU power good after {} seconds",
                    max_wait));
    return false;
}

void wait_for_interfaces(const MctpPaths& paths, const std::string& pg_glob,
                         int expected)
{
    constexpr int retries = 10;
    log(std::format("Expected number of MCTP interfaces: {} (wait start {})",
                    expected, ts()));
    snapshot(paths, pg_glob, "enter-wait_for_interfaces");
    int prev = -1;
    int count = 0;
    for (int retry = 0; retry < retries; ++retry)
    {
        count = interface_count();
        log(std::format("Retry {}/{}: Found {} interface(s), expecting {}",
                        retry + 1, retries, count, expected));
        if (count != prev)
        {
            log(std::format("Interface count changed {} -> {} at {}", prev,
                            count, ts()));
            prev = count;
        }
        if (count == expected && count > 0)
        {
            log(std::format("All expected MCTP interfaces enumerated at {}",
                            ts()));
            return;
        }
        sleep_s(1);
    }
    log(std::format("WARNING: Interface count mismatch after {} retries. "
                    "Expected: {}, Found: {}",
                    retries, expected, count));
    snapshot(paths, pg_glob, "timeout-wait_for_interfaces");
}

// MCTP_USB_IFACE_COUNT, or IFS_NUM=2 when unset or not a plain number.
int expected_count(const std::string& features)
{
    const auto value = platform_feature("MCTP_USB_IFACE_COUNT", features);
    if (value.empty())
    {
        log(std::format("MCTP_USB_IFACE_COUNT not set in {}; falling back to "
                        "IFS_NUM=2",
                        features));
        return 2;
    }
    if (value.find_first_not_of("0123456789") == std::string::npos &&
        (value == "0" || value.front() != '0') && value.size() < 10)
    {
        log(std::format("Using MCTP_USB_IFACE_COUNT={} from {}", value,
                        features));
        return std::stoi(value);
    }
    log(std::format("MCTP_USB_IFACE_COUNT='{}' in {} is not a valid "
                    "non-negative integer; falling back to IFS_NUM=2",
                    value, features));
    return 2;
}

} // namespace

namespace detail
{

// First column of MCU_MAP="${MCU_MAP:-...}" in usb-enum-debug.conf.
std::vector<std::string> mcu_map_ports(const std::string& text)
{
    const auto pos = text.find("MCU_MAP=\"");
    if (pos == std::string::npos)
        return {};
    auto v = text.substr(pos + 9);
    v = v.substr(0, v.find('"'));
    if (v.starts_with("${MCU_MAP:-"))
    {
        v.erase(0, 11);
        if (v.ends_with('}'))
            v.pop_back();
    }
    for (auto bs = v.find("\\\n"); bs != std::string::npos; bs = v.find("\\\n"))
        v.erase(bs, 2);
    std::vector<std::string> ports;
    std::istringstream lines{v};
    std::string line;
    while (std::getline(lines, line))
    {
        std::string port;
        if (std::istringstream{line} >> port)
            ports.push_back(port);
    }
    return ports;
}

} // namespace detail

int switch_mctp_ready(const MctpPaths& paths)
{
    const auto setup = platform_feature("MCTP_SETUP", paths.features);
    // HIDs whose MCUs bmc_ready already released once: pulsing their reset
    // again only re-enumerates them; mctpd's restart assigns the EIDs.
    if (setup == "none")
    {
        log("MCTP setup: MCUs already released by bmc_ready; no reset");
        return mctp_setup_done;
    }
    // HIDs whose MCUs re-enumerate on reset (udev configures the links).
    if (setup == "mcu-reset")
    {
        log("MCTP setup: resetting on-board MCUs (no host gate, no iface "
            "setup)");
        hw_mgmt_mcu_reset(paths.mcu_devices);
        log("MCTP setup: MCU reset complete");
        return mctp_setup_done;
    }
    log("Starting MCTP interface setup");
    if (host_cpu_intentionally_off())
    {
        log("Host CPU is off or powering down; skipping MCTP interface setup "
            "(success)");
        return mctp_setup_done;
    }
    std::string pg_glob;
    if (!wait_for_mcu_power_good(paths.mcu_pg, pg_glob))
    {
        log("Failed to get MCU power good - exiting");
        return 1;
    }
    log("Setting up MCTP interfaces...");
    const int expected = expected_count(paths.features);
    if (expected == 0)
    {
        log("No MCTP USB devices expected");
        log("MCTP interface setup completed");
        return mctp_setup_done;
    }
    sleep_s(5);
    wait_for_interfaces(paths, pg_glob, expected);
    return 0;
}

} // namespace nvidia
