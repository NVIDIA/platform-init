// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Shared utilities called from inside platform init functions.
// These are not subcommands — they are steps within platform init.

#pragma once

#include "gpio.hpp"
#include "i2c.hpp"
#include "utilities.hpp"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace nvidia
{

// Best-effort D-Bus call to phosphor-logging's Logging.Create.
// Used to publish Redfish-routed events from the init path. The logging
// service may not be up yet at very early boot — failures are swallowed
// and reported to stderr so init never aborts on logging issues.
inline void log_redfish(
    std::string_view message_id, std::string_view args,
    std::string_view severity =
        "xyz.openbmc_project.Logging.Entry.Level.Informational")
{
    try
    {
        auto bus = sdbusplus::bus::new_default_system();
        auto method = bus.new_method_call(
            "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
            "xyz.openbmc_project.Logging.Create", "Create");
        const std::map<std::string, std::string> data{
            {"REDFISH_MESSAGE_ID", std::string{message_id}},
            {"REDFISH_MESSAGE_ARGS", std::string{args}},
        };
        method.append(std::string{message_id}, std::string{severity}, data);
        bus.call_noreply(method);
    }
    catch (const std::exception& e)
    {
        std::cerr << "log_redfish: " << e.what() << "\n";
    }
}

// ── EEPROM init ───────────────────────────────────────────────────────────

struct EepromDevice
{
    unsigned int bus;
    unsigned int address;
    std::string_view type; // e.g. "24c128"
    std::string_view description;
    bool poweron_only = false;
};

inline void eeprom_init(std::span<const EepromDevice> devices)
{
    for (const auto& dev : devices)
    {
        const auto path = std::format("/sys/bus/i2c/drivers/at24/{}-{:04x}",
                                      dev.bus, dev.address);
        if (std::filesystem::exists(path))
        {
            continue;
        }
        std::cerr << std::format("eeprom_init: {} bus={} addr={:#x} type={}\n",
                                 dev.description, dev.bus, dev.address,
                                 dev.type);
        i2c::new_device(dev.bus, dev.address, dev.type);
    }
}

inline void eeprom_remove_poweron(std::span<const EepromDevice> devices)
{
    for (const auto& dev : devices)
    {
        if (!dev.poweron_only)
        {
            continue;
        }
        const auto path = std::format("/sys/bus/i2c/drivers/at24/{}-{:04x}",
                                      dev.bus, dev.address);
        if (!std::filesystem::exists(path))
        {
            continue;
        }
        const auto del = std::format(
            "/sys/class/i2c-dev/i2c-{}/device/delete_device", dev.bus);
        if (auto f = std::ofstream{del}; f)
        {
            f << std::format("{:#x}\n", dev.address);
        }
    }
}

// ── GPIO level validation ─────────────────────────────────────────────────

struct GpioLevel
{
    const char* name;
    int expected;
};

inline int gpio_check(std::span<const GpioLevel> checks)
{
    bool all_pass = true;
    for (const auto& g : checks)
    {
        try
        {
            const int actual = gpio::get(g.name);
            if (actual != g.expected)
            {
                std::cerr << std::format(
                    "gpio_check: FAIL {} expected={} actual={}\n", g.name,
                    g.expected, actual);
                all_pass = false;
            }
        }
        catch (...)
        {
            std::cerr << std::format("gpio_check: error reading {}\n", g.name);
            all_pass = false;
        }
    }
    std::cerr << (all_pass ? "gpio_check: PASS\n" : "gpio_check: FAIL\n");
    return all_pass ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── EEPROM content verification ───────────────────────────────────────────
// Mirrors eeprom-crc-verifier.sh.
// Reads the golden sample from the read-only overlay lowerdir (not through the
// writable upper layer) to prevent a tampered upperdir from masking a bad
// EEPROM. Non-fatal: logs pass/fail to stderr and to Redfish via
// phosphor-logging; init continues regardless.

inline void eeprom_crc_verify(
    unsigned bus, unsigned addr, std::string_view type,
    std::string_view golden_rel_path =
        "/usr/share/eeprom-crc-verifier/golden_eeprom.bin")
{
    using namespace std::chrono_literals;
    const auto tag =
        std::format("eeprom_crc_verify: bus={} addr={:#x}", bus, addr);

    // ── Step 1: Ensure EEPROM driver is bound ─────────────────────────────
    const auto dev_sysfs =
        std::format("/sys/bus/i2c/devices/{}-{:04x}", bus, addr);
    if (!std::filesystem::exists(dev_sysfs))
    {
        std::cerr << std::format("{}: binding driver {}\n", tag, type);
        i2c::new_device(bus, addr, type);
        std::this_thread::sleep_for(100ms); // wait for sysfs entry
    }
    const auto eeprom_path = dev_sysfs + "/eeprom";
    if (!std::filesystem::exists(eeprom_path))
    {
        std::cerr << std::format("{}: eeprom sysfs not found at {}\n", tag,
                                 eeprom_path);
        return;
    }

    // ── Step 2: Resolve golden sample path via overlay lowerdir ──────────
    // Reading from the lowerdir directly (not through the overlay merge)
    // prevents a write to the upperdir from shadowing the golden and producing
    // a false pass.
    std::string golden_path{golden_rel_path};
    {
        std::ifstream mounts{"/proc/mounts"};
        std::string line;
        while (std::getline(mounts, line))
        {
            std::istringstream ss{line};
            std::string dev, mnt, fstype, opts;
            ss >> dev >> mnt >> fstype >> opts;
            if (fstype != "overlay" || mnt != "/")
                continue;
            const auto pos = opts.find("lowerdir=");
            if (pos == std::string::npos)
                break;
            const auto start = pos + 9;
            const auto comma = opts.find(',', start);
            const auto lowerdir = opts.substr(
                start, comma == std::string::npos ? comma : comma - start);
            if (!lowerdir.empty())
                golden_path = lowerdir + std::string{golden_rel_path};
            break;
        }
    }

    // ── Step 3: Read golden sample ────────────────────────────────────────
    std::vector<uint8_t> golden;
    {
        std::ifstream f{golden_path, std::ios::binary};
        if (!f)
        {
            std::cerr << std::format("{}: golden sample not found at {}\n", tag,
                                     golden_path);
            return;
        }
        golden.assign(std::istreambuf_iterator<char>{f}, {});
    }
    if (golden.empty())
    {
        std::cerr << std::format("{}: golden sample is empty\n", tag);
        return;
    }

    // ── Step 4: Read EEPROM content (exactly golden.size() bytes) ────────
    std::vector<uint8_t> actual(golden.size());
    {
        std::ifstream f{eeprom_path, std::ios::binary};
        if (!f || !f.read(reinterpret_cast<char*>(actual.data()),
                          static_cast<std::streamsize>(golden.size())))
        {
            std::cerr << std::format("{}: failed to read {}\n", tag,
                                     eeprom_path);
            return;
        }
    }

    // ── Step 5: Compare ───────────────────────────────────────────────────
    const auto args = std::format("EEPROM bus={} addr={:#x}", bus, addr);
    if (actual == golden)
    {
        std::cerr << std::format("{}: PASS — content matches golden\n", tag);
        log_redfish("ResourceEvent.1.3.ResourceStatusChangedOK", args);
    }
    else
    {
        std::cerr << std::format("{}: FAIL — content does NOT match golden\n",
                                 tag);
        log_redfish("ResourceEvent.1.3.ResourceErrorsDetected", args,
                    "xyz.openbmc_project.Logging.Entry.Level.Warning");
    }
    // Non-fatal: result is informational only
}

// Overload: golden data supplied by the caller as a constexpr byte array.
// Skips file I/O and overlay lowerdir resolution entirely.
// Use this when the golden bytes are embedded in the platform .cpp file.
inline void eeprom_crc_verify(unsigned bus, unsigned addr,
                              std::string_view type,
                              std::span<const uint8_t> golden_bytes)
{
    using namespace std::chrono_literals;
    const auto tag =
        std::format("eeprom_crc_verify: bus={} addr={:#x}", bus, addr);

    // ── Step 1: Ensure EEPROM driver is bound ─────────────────────────────
    const auto dev_sysfs =
        std::format("/sys/bus/i2c/devices/{}-{:04x}", bus, addr);
    const auto eeprom_path = dev_sysfs + "/eeprom";
    if (!std::filesystem::exists(dev_sysfs))
    {
        std::cerr << std::format("{}: binding driver {}\n", tag, type);
        i2c::new_device(bus, addr, type);
        std::this_thread::sleep_for(100ms);
    }
    if (!std::filesystem::exists(eeprom_path))
    {
        std::cerr << std::format("{}: eeprom sysfs not found at {}\n", tag,
                                 eeprom_path);
        return;
    }

    // ── Step 2: Read EEPROM content ───────────────────────────────────────
    std::vector<uint8_t> actual(golden_bytes.size());
    {
        std::ifstream f{eeprom_path, std::ios::binary};
        if (!f || !f.read(reinterpret_cast<char*>(actual.data()),
                          static_cast<std::streamsize>(golden_bytes.size())))
        {
            std::cerr << std::format("{}: failed to read {}\n", tag,
                                     eeprom_path);
            return;
        }
    }

    // ── Step 3: Compare ───────────────────────────────────────────────────
    const auto golden_vec =
        std::vector<uint8_t>(golden_bytes.begin(), golden_bytes.end());
    const auto args = std::format("EEPROM bus={} addr={:#x}", bus, addr);
    if (actual == golden_vec)
    {
        std::cerr << std::format("{}: PASS — content matches golden\n", tag);
        log_redfish("ResourceEvent.1.3.ResourceStatusChangedOK", args);
    }
    else
    {
        std::cerr << std::format("{}: FAIL — content does NOT match golden\n",
                                 tag);
        log_redfish("ResourceEvent.1.3.ResourceErrorsDetected", args,
                    "xyz.openbmc_project.Logging.Entry.Level.Warning");
    }
}

// ── NVMe CPLD MUX probe ───────────────────────────────────────────────────
// Mirrors nvme_cpld_probe.sh + nvme_lib.sh.
// The NVMe CPLD MUX (pca954x) is powered by RUN_POWER; if the BMC boots while
// the host is off, the kernel's initial probe fails and the MUX is bound
// without creating channel-0. Unbind + rebind forces a fresh probe. If
// channel-0 already exists the MUX is healthy — leaves it alone. Non-fatal;
// call for each (bus, addr) pair that has an E1.S MUX.
inline void nvme_cpld_probe(unsigned bus, unsigned addr = 0x77)
{
    const auto key = std::format("{}-{:04x}", bus, addr);
    const auto dev = "/sys/bus/i2c/devices/" + key;
    const auto drv = "/sys/bus/i2c/drivers/pca954x/" + key;
    const auto channel0 = dev + "/channel-0";

    bool do_bind = true;
    if (std::filesystem::exists(dev))
    {
        if (std::filesystem::exists(channel0))
        {
            std::cerr << std::format("nvme_cpld_probe: {}: healthy\n", key);
            return;
        }
        if (std::filesystem::exists(drv))
        {
            if (std::ofstream f{"/sys/bus/i2c/drivers/pca954x/unbind"}; f)
                f << key;
            else
            {
                std::cerr << std::format("nvme_cpld_probe: {}: unbind failed\n",
                                         key);
                do_bind = false;
            }
        }
    }

    if (do_bind && !std::filesystem::exists(channel0))
    {
        if (std::ofstream f{"/sys/bus/i2c/drivers/pca954x/bind"}; f)
        {
            f << key;
            std::cerr << std::format("nvme_cpld_probe: {}: bound to pca954x\n",
                                     key);
        }
        else
            std::cerr << std::format("nvme_cpld_probe: {}: bind failed\n", key);
    }
}

// ── RTC device detection ──────────────────────────────────────────────────
// Mirrors rtc-detection.sh.
// Probes each bus at `addr` for a supported RTC by reading vendor register 0x10
// and comparing the device ID. On match, binds the driver via new_device.
// Non-fatal: logs if no RTC found.
// buses: platform-specific list (e.g. {5, 19} for nvl/c2/e5010, {6, 19} for
// quark)

struct RtcVariant
{
    uint8_t device_id;
    std::string_view driver;
};

static constexpr std::array<RtcVariant, 2> rtc_variants{{
    {0x5c, "nct3018y"},
    {0x4e, "pcf85053a"},
}};

inline void rtc_detect(std::span<const unsigned> buses, uint8_t addr = 0x6f)
{
    for (unsigned bus : buses)
    {
        // If the kernel RTC driver already claimed this device from DTS,
        // I2C_SLAVE ioctl will fail with EBUSY. Check sysfs first — if a
        // driver is already bound the RTC is functional; skip raw I2C probe.
        const auto sysfs_dev =
            std::format("/sys/bus/i2c/devices/{}-{:04x}", bus, addr);
        if (std::filesystem::exists(sysfs_dev + "/driver"))
        {
            std::cerr << std::format(
                "rtc_detect: bus={} addr={:#x}: already bound to kernel driver\n",
                bus, addr);
            return;
        }

        try
        {
            i2c::RawDevice dev{bus, addr};
            auto id_r = dev.read_byte(0x10);
            if (!id_r)
                continue;
            uint8_t id = *id_r;
            for (const auto& rtc : rtc_variants)
            {
                if (id != rtc.device_id)
                    continue;
                std::cerr << std::format(
                    "rtc_detect: bus={} addr={:#x}: found {} (id={:#x})\n", bus,
                    addr, rtc.driver, id);
                i2c::new_device(bus, addr, rtc.driver);
                return;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format("rtc_detect: bus={}: {}\n", bus, e.what());
        }
    }
    std::cerr << "rtc_detect: no RTC found on any bus\n";
}

// ── ERoT hardware checkout ────────────────────────────────────────────────
// Mirrors hw_checkout.sh: issues MCTP VDM query to each ERoT endpoint and
// reports pass/fail. Non-fatal — called after sd_notify(READY=1).

struct ErotEndpoint
{
    unsigned int eid;
    std::string_view description;
};

inline int hw_checkout(std::span<const ErotEndpoint> endpoints, int retries = 5,
                       std::chrono::seconds retry_delay = std::chrono::seconds{
                           3})
{
    if (endpoints.empty())
    {
        std::cerr << "hw_checkout: no ERoT endpoints configured\n";
        return EXIT_FAILURE;
    }

    bool all_pass = true;
    for (const auto& ep : endpoints)
    {
        std::cerr << std::format("hw_checkout: checking EID={} ({})\n", ep.eid,
                                 ep.description);
        const auto cmd = std::format(
            "mctp-vdm-util -t {} -c query_boot_status 2>&1", ep.eid);

        int rc = -1;
        for (int attempt = 0; attempt <= retries; ++attempt)
        {
            rc = std::system(cmd.c_str()); // NOLINT(cert-env33-c)
            if (rc == 0)
                break;
            if (attempt < retries)
            {
                std::cerr << std::format(
                    "hw_checkout: EID={} attempt {}/{} failed (rc={}) — retrying in {}s\n",
                    ep.eid, attempt + 1, retries, rc, retry_delay.count());
                std::this_thread::sleep_for(retry_delay);
            }
        }

        if (rc != 0)
        {
            std::cerr << std::format("hw_checkout: FAIL EID={} rc={}\n", ep.eid,
                                     rc);
            all_pass = false;
        }
        else
        {
            std::cerr << std::format("hw_checkout: PASS EID={}\n", ep.eid);
        }
    }

    std::cerr << (all_pass ? "hw_checkout: all PASS\n"
                           : "hw_checkout: some FAIL\n");
    return all_pass ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── Power-sequence and hardware-binding helpers ───────────────────────────

// ── 2a. IO expander helpers ───────────────────────────────────────────────
// Source: bind_gpio_expanders() / check_iox_N_20_present() in bmc_ready.sh
// After standby power comes up, PCA953x expanders on downstream power domains
// are not auto-detected by the kernel. These functions manually bind them.

// Returns true if the expander is already registered in sysfs.
inline bool iox_present(unsigned bus, unsigned addr)
{
    return std::filesystem::exists(
        std::format("/sys/bus/i2c/drivers/pca953x/{}-{:04x}", bus, addr));
}

// Bind a PCA953x GPIO expander. No-op if already bound. Non-fatal on error.
// Source: `echo {bus}-{addr:04x} > /sys/bus/i2c/drivers/pca953x/bind`
inline void iox_bind(unsigned bus, unsigned addr)
{
    if (iox_present(bus, addr))
        return;
    if (std::ofstream f{"/sys/bus/i2c/drivers/pca953x/bind"}; f)
    {
        f << std::format("{}-{:04x}", bus, addr);
        std::cerr << std::format("iox_bind: {}-{:04x} bound\n", bus, addr);
    }
    else
        std::cerr << std::format("iox_bind: {}-{:04x} FAILED\n", bus, addr);
}

// Poll until the expander appears in sysfs (i.e. standby power is up).
// Source: check_iox_N_20_present() — 30 s timeout, 1 s poll.
inline bool iox_wait(unsigned bus, unsigned addr,
                     std::chrono::seconds timeout = std::chrono::seconds{30})
{
    using namespace std::chrono_literals;
    const auto path =
        std::format("/sys/bus/i2c/drivers/pca953x/{}-{:04x}", bus, addr);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (std::filesystem::exists(path))
            return true;
        std::this_thread::sleep_for(1s);
    }
    std::cerr << std::format("iox_wait: {}-{:04x} not present after {}s\n", bus,
                             addr, timeout.count());
    return false;
}

// ── 2b. I2C mux binding ───────────────────────────────────────────────────
// Source: bind_i2c_mux_driver() in bmc_ready.sh
// Binds a PCA954x I2C mux. Used on C2 after B0_M1_I2C_SOCAMM_UPHY_SEL-O is set.
inline void i2c_mux_bind(unsigned bus, unsigned addr)
{
    const auto path =
        std::format("/sys/bus/i2c/drivers/pca954x/{}-{:04x}", bus, addr);
    if (std::filesystem::exists(path))
        return;
    if (std::ofstream f{"/sys/bus/i2c/drivers/pca954x/bind"}; f)
    {
        f << std::format("{}-{:04x}", bus, addr);
        std::cerr << std::format("i2c_mux_bind: {}-{:04x} bound\n", bus, addr);
    }
    else
        std::cerr << std::format("i2c_mux_bind: {}-{:04x} FAILED\n", bus, addr);
}

// ── 2c. HMC I2C mux select ────────────────────────────────────────────────
// Source: hmc_bypass() in bmc_ready.sh
// Routes I2C_MGMT1 through or around the HMC depending on whether the HMC is
// present. HMC present → HIGH (dedicated path). HMC absent → LOW (bypass).
// GPIO names may differ across platforms; override defaults as needed.
inline void hmc_mux_select(const char* prsnt_gpio = "HMC_PRSNT_R-I",
                           const char* sel_gpio = "BMC_HMC_MUX_SEL-O")
{
    const int present = gpio::get(prsnt_gpio);
    if (present == 1)
    {
        std::cerr << std::format(
            "hmc_mux_select: HMC present — disabling I2C_MGMT1 bypass\n");
        gpio::set(sel_gpio, 1);
    }
    else
    {
        std::cerr << std::format(
            "hmc_mux_select: HMC absent — enabling I2C_MGMT1 bypass\n");
        gpio::set(sel_gpio, 0);
    }
}

// ── 2d. GPIO poll helper ──────────────────────────────────────────────────
// Source: wait_gpio_assert() in bmc_ready.sh
// Polls a GPIO until it goes HIGH. Returns true on success, false on timeout.
// Default: 100 retries × 100 ms = 10 s (matches script default for MCU/CPLD
// waits). For standby power waits use retries=200 (20 s), delay=100 ms.
inline bool wait_gpio_high(
    const char* name, int retries = 100,
    std::chrono::milliseconds delay = std::chrono::milliseconds{100})
{
    std::cerr << std::format("wait_gpio_high: waiting for {}\n", name);
    for (int i = 0; i < retries; ++i)
    {
        if (gpio::get(name) != 0)
        {
            std::cerr << std::format("wait_gpio_high: {} asserted\n", name);
            return true;
        }
        std::this_thread::sleep_for(delay);
    }
    std::cerr << std::format("wait_gpio_high: TIMEOUT {} after {} × {}ms\n",
                             name, retries, delay.count());
    return false;
}

// ── 2e. Boot banner ───────────────────────────────────────────────────────
// Source: bmc_boot_banner() in bmc_ready.sh
// Prints OS version and network info to UART at 115200 baud.
// uart_dev: platform UART console (default /dev/ttyS0, override for platforms
//           that use a different console, e.g. /dev/ttyAMA0).
inline void bmc_boot_banner(std::string_view uart_dev = "/dev/ttyS0")
{
    std::ofstream uart{uart_dev.data()};
    if (!uart)
        return;

    if (std::ifstream art{"/usr/bin/banner_art.txt"}; art)
        uart << art.rdbuf() << "\n\n";

    uart << "+---------------------+\n"
            "| VERSION INFORMATION |\n"
            "+---------------------+\n";
    if (std::ifstream rel{"/etc/os-release"}; rel)
        uart << rel.rdbuf() << "\n";

    uart << "+---------------------+\n"
            "| NETWORK INFORMATION |\n"
            "+---------------------+\n";
    if (FILE* p = ::popen("ip a 2>&1", "r"); p)
    {
        char buf[256];
        while (::fgets(buf, sizeof(buf), p))
            uart << buf;
        ::pclose(p);
    }
    uart << "\n";
}

// ── eMMC partition layout ─────────────────────────────────────────────────
// Default VR BMC partition layout — shared by vr-nvl-bmc, vr-c2-bmc,
// vr-e5010-bmc, vr-quark-bmc, and vr200-nvl72-gl10-bmc.
// Platforms with a different layout define their own constexpr array and pass
// it to emmc_partition() / partition_check() instead of using the default.

enum class EmmcPartType : uint8_t
{
    Linux = 0x83,
    Extended = 0x05,
};

struct EmmcPartSpec
{
    int num;
    EmmcPartType type;
    uint64_t size_bytes; // 0 = fill to end of disk (Extended only)
};

//   p1   64 MiB  Config data   (primary)
//   p2    2 GiB  FDR           (primary)
//   p3    1 GiB  User log      (primary)
//   p4    rest   Extended      (container)
//   p5    2 GiB  IST           (logical)
//   p6    1 MiB  U-boot env    (logical)
//   p7  1.9 GiB  Miscellaneous (logical)
static constexpr std::array<EmmcPartSpec, 7> vr_bmc_emmc_layout{{
    {1, EmmcPartType::Linux, 64ULL << 20},   //   64 MiB
    {2, EmmcPartType::Linux, 2048ULL << 20}, //    2 GiB
    {3, EmmcPartType::Linux, 1024ULL << 20}, //    1 GiB
    {4, EmmcPartType::Extended, 0},          //  fills rest
    {5, EmmcPartType::Linux, 2048ULL << 20}, //    2 GiB
    {6, EmmcPartType::Linux, 1ULL << 20},    //    1 MiB
    {7, EmmcPartType::Linux, 1900ULL << 20}, //  1.9 GiB
}};

// ── eMMC partition creation ───────────────────────────────────────────────
// Replaces create-partition.sh.
// Writes an MBR partition table directly via ioctl; no external tools.
// Idempotent: skips if p1 already exists in sysfs.
// layout: partition specs (default: vr_bmc_emmc_layout). Platforms with a
// different
//         layout pass their own constexpr array via std::span.

inline int emmc_partition(
    std::string_view device,
    std::span<const EmmcPartSpec> layout = vr_bmc_emmc_layout)
{
    // Idempotent guard
    const auto sysfs_p1 = std::format("/sys/block/{0}/{0}p1/size", device);
    if (std::filesystem::exists(sysfs_p1))
    {
        std::cerr << std::format("emmc_partition: {} already partitioned\n",
                                 device);
        return EXIT_SUCCESS;
    }

    const auto dev_path = std::format("/dev/{}", device);
    std::cerr << std::format("emmc_partition: partitioning {}\n", dev_path);

    const int fd = ::open(dev_path.c_str(), O_RDWR | O_SYNC);
    if (fd < 0)
    {
        std::cerr << std::format("emmc_partition: open {}: {}\n", dev_path,
                                 std::strerror(errno));
        return EXIT_FAILURE;
    }
    struct Guard
    {
        int fd;
        ~Guard()
        {
            ::close(fd);
        }
    } guard{fd};

    uint64_t total_bytes = 0;
    if (::ioctl(fd, BLKGETSIZE64, &total_bytes) < 0)
    {
        std::cerr << std::format("emmc_partition: BLKGETSIZE64: {}\n",
                                 std::strerror(errno));
        return EXIT_FAILURE;
    }

    constexpr uint32_t SECTOR = 512;
    constexpr uint32_t ALIGN =
        2048; // 1 MiB boundary in sectors (fdisk default)
    const uint64_t total_sects = total_bytes / SECTOR;

    auto align_up = [](uint64_t lba) -> uint64_t {
        return ((lba + ALIGN - 1) / ALIGN) * ALIGN;
    };

    // CHS encoding using fdisk's LBA geometry (255 heads × 63 spt).
    // CHS is not used for LBA-mode access but must be set for tool
    // compatibility.
    constexpr uint32_t HEADS = 255;
    constexpr uint32_t SPT = 63;

    auto encode_chs = [&](uint8_t* out, uint64_t abs_lba) {
        uint32_t c = static_cast<uint32_t>(abs_lba / (HEADS * SPT));
        uint32_t h = static_cast<uint32_t>((abs_lba % (HEADS * SPT)) / SPT);
        uint32_t s = static_cast<uint32_t>((abs_lba % SPT) + 1);
        if (c > 1023)
        {
            c = 1023;
            h = 254;
            s = 63;
        } // cap at max CHS
        out[0] = static_cast<uint8_t>(h);
        out[1] = static_cast<uint8_t>((s & 0x3F) | ((c >> 2) & 0xC0));
        out[2] = static_cast<uint8_t>(c & 0xFF);
    };

    // Write a 16-byte MBR/EBR partition entry.
    // abs_lba: absolute disk LBA (used only for CHS encoding)
    // rel_lba: LBA value written to the entry (may be relative to a base)
    auto fill_entry = [&](uint8_t* e, uint8_t type, uint64_t abs_lba,
                          uint64_t rel_lba, uint64_t size) {
        e[0] = 0x00; // not bootable
        encode_chs(e + 1, abs_lba);
        e[4] = type;
        encode_chs(e + 5, abs_lba + (size > 0 ? size - 1 : 0));
        const auto r = static_cast<uint32_t>(rel_lba);
        const auto n = static_cast<uint32_t>(size);
        e[8] = r & 0xFF;
        e[9] = (r >> 8) & 0xFF;
        e[10] = (r >> 16) & 0xFF;
        e[11] = (r >> 24) & 0xFF;
        e[12] = n & 0xFF;
        e[13] = (n >> 8) & 0xFF;
        e[14] = (n >> 16) & 0xFF;
        e[15] = (n >> 24) & 0xFF;
    };

    // Primary partition LBA ranges (from layout; index matches num-1)
    const uint64_t p1_sects = layout[0].size_bytes / SECTOR;
    const uint64_t p2_sects = layout[1].size_bytes / SECTOR;
    const uint64_t p3_sects = layout[2].size_bytes / SECTOR;
    const uint64_t p5_sects = layout[4].size_bytes / SECTOR;
    const uint64_t p6_sects = layout[5].size_bytes / SECTOR;
    const uint64_t p7_sects = layout[6].size_bytes / SECTOR;

    const uint64_t p1_start = ALIGN;
    const uint64_t p2_start = align_up(p1_start + p1_sects);
    const uint64_t p3_start = align_up(p2_start + p2_sects);
    const uint64_t p4_start = align_up(p3_start + p3_sects);
    const uint64_t p4_sects = total_sects - p4_start; // fills to end of disk

    // Logical partitions: each preceded by a 1-sector EBR at a 1 MiB boundary,
    // with the data starting ALIGN sectors after the EBR.
    const uint64_t p5_ebr = p4_start;
    const uint64_t p5_start = p5_ebr + ALIGN;

    const uint64_t p6_ebr = align_up(p5_start + p5_sects);
    const uint64_t p6_start = p6_ebr + ALIGN;

    // No p7_start: the pX_start values exist only to position the *next*
    // EBR, and p7 is last in the chain. write_ebr() derives each partition's
    // own data offset as ebr_lba + ALIGN.
    const uint64_t p7_ebr = align_up(p6_start + p6_sects);

    // ── MBR: sector 0 ────────────────────────────────────────────────────
    std::array<uint8_t, 512> mbr{};
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    // Primary entries at offset 446; for primary partitions abs == rel
    fill_entry(mbr.data() + 446, 0x83, p1_start, p1_start, p1_sects);
    fill_entry(mbr.data() + 446 + 16, 0x83, p2_start, p2_start, p2_sects);
    fill_entry(mbr.data() + 446 + 32, 0x83, p3_start, p3_start, p3_sects);
    fill_entry(mbr.data() + 446 + 48, 0x05, p4_start, p4_start, p4_sects);

    if (::pwrite(fd, mbr.data(), 512, 0) != 512)
    {
        std::cerr << std::format("emmc_partition: MBR write: {}\n",
                                 std::strerror(errno));
        return EXIT_FAILURE;
    }

    // ── EBRs for logical partitions ───────────────────────────────────────
    // EBR entry[0]: this logical partition (lba_start relative to EBR sector)
    // EBR entry[1]: next EBR           (lba_start relative to p4_start), 0 if
    // last
    //               lba_size           = ALIGN + next_partition_size

    auto write_ebr = [&](uint64_t ebr_lba, uint64_t data_sects,
                         uint64_t next_ebr_lba, uint64_t next_sects) -> bool {
        std::array<uint8_t, 512> ebr{};
        ebr[510] = 0x55;
        ebr[511] = 0xAA;

        // Entry 0: current logical (CHS=absolute, LBA=relative to EBR)
        fill_entry(ebr.data() + 446, 0x83,
                   ebr_lba + ALIGN, // abs_lba for CHS
                   ALIGN,           // rel_lba (offset from EBR)
                   data_sects);

        // Entry 1: next EBR (CHS=absolute, LBA=relative to extended start)
        if (next_ebr_lba != 0)
        {
            fill_entry(ebr.data() + 462, 0x05,
                       next_ebr_lba,            // abs_lba for CHS
                       next_ebr_lba - p4_start, // rel_lba (offset from p4)
                       ALIGN + next_sects);     // slot = EBR gap + data
        }

        const auto offset = static_cast<off_t>(ebr_lba) * SECTOR;
        return ::pwrite(fd, ebr.data(), 512, offset) == 512;
    };

    if (!write_ebr(p5_ebr, p5_sects, p6_ebr, p6_sects) ||
        !write_ebr(p6_ebr, p6_sects, p7_ebr, p7_sects) ||
        !write_ebr(p7_ebr, p7_sects, 0, 0))
    {
        std::cerr << std::format("emmc_partition: EBR write: {}\n",
                                 std::strerror(errno));
        return EXIT_FAILURE;
    }

    if (::fsync(fd) < 0)
    {
        std::cerr << std::format("emmc_partition: fsync: {}\n",
                                 std::strerror(errno));
        return EXIT_FAILURE;
    }

    // Ask kernel to re-read the new partition table
    if (::ioctl(fd, BLKRRPART) < 0)
    {
        std::cerr << std::format("emmc_partition: BLKRRPART: {} "
                                 "(reboot will apply table)\n",
                                 std::strerror(errno));
        // Non-fatal: the table is on disk; kernel will pick it up after reboot
    }

    std::cerr << std::format("emmc_partition: {} partitioned successfully\n",
                             device);
    return EXIT_SUCCESS;
}

// ── Partition layout check ────────────────────────────────────────────────
// Replaces repartition_check.sh.
// Validates sysfs sector counts against the provided layout.
// layout: partition specs (default: vr_bmc_emmc_layout). Must match what was
// written
//         by emmc_partition().

inline int partition_check(
    std::string_view device,
    std::span<const EmmcPartSpec> layout = vr_bmc_emmc_layout)
{
    constexpr int tolerance_pct = 2;
    constexpr long sector_bytes = 512;

    int missing = 0;
    int failed = 0;

    // Count non-extended entries (those we can validate via sysfs)
    int total = 0;
    for (const auto& spec : layout)
        if (spec.type != EmmcPartType::Extended)
            ++total;

    for (const auto& spec : layout)
    {
        if (spec.type == EmmcPartType::Extended)
            continue; // variable size; nothing to compare

        const long expected_sects =
            static_cast<long>(spec.size_bytes / sector_bytes);
        const auto sysfs =
            std::format("/sys/block/{0}/{0}p{1}/size", device, spec.num);

        long actual_sects = -1;
        if (std::ifstream f{sysfs}; f)
            f >> actual_sects;

        if (actual_sects < 0)
        {
            std::cerr << std::format("partition_check: {}p{} not in sysfs\n",
                                     device, spec.num);
            ++missing;
            continue;
        }

        const long diff = std::abs(actual_sects - expected_sects);
        const long diff_pct =
            expected_sects > 0 ? diff * 100L / expected_sects : 100L;

        if (diff_pct <= tolerance_pct)
            std::cerr << std::format("partition_check: {}p{} OK ({}%)\n",
                                     device, spec.num, diff_pct);
        else
        {
            std::cerr << std::format("partition_check: {}p{} MISMATCH "
                                     "(actual={} expected=~{} diff={}%)\n",
                                     device, spec.num, actual_sects,
                                     expected_sects, diff_pct);
            ++failed;
        }
    }

    if (missing == total)
    {
        std::cerr << std::format(
            "partition_check: no partitions on {}; skipping\n", device);
        return EXIT_SUCCESS;
    }

    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── eMMC mount table ─────────────────────────────────────────────────────
// Maps partition numbers to mount points. Replaces emmc-mount.conf.
// Shared by vr-nvl-bmc, vr-c2-bmc, vr-e5010-bmc, vr-quark-bmc,
// and vr200-nvl72-gl10-bmc (same layout as vr_bmc_emmc_layout).
// Extended partition (p4) is a container — not mounted directly.

struct EmmcMount
{
    int part_num;
    const char* mount_point;
};

static constexpr std::array<EmmcMount, 6> vr_bmc_emmc_mounts{{
    {1, "/var/emmc/bmc-config"},
    {2, "/var/emmc/fdr"},
    {3, "/var/emmc/user-logs"},
    {5, "/var/emmc/ist"},
    {6, "/var/emmc/uboot-env"},
    {7, "/var/emmc/misc"},
}};

// ── eMMC format and mount ─────────────────────────────────────────────────
// Replaces emmc-partition-format.sh mount logic.
// For each entry in the mount table:
//   1. Creates the mount-point directory if absent.
//   2. Skips silently if the partition is already mounted (idempotent).
//   3. Tries to mount as ext4; if the superblock is absent (EINVAL / ENOENT)
//      runs mkfs.ext4 -F to format, then mounts.
// Non-fatal: logs failures and continues to the next partition.

inline void emmc_format_and_mount(
    std::string_view device,
    std::span<const EmmcMount> mounts = vr_bmc_emmc_mounts)
{
    // Helper: check if a path is already a mount point via /proc/mounts
    auto is_mounted = [](std::string_view mpt) -> bool {
        std::ifstream f{"/proc/mounts"};
        std::string line;
        while (std::getline(f, line))
        {
            std::istringstream ss{line};
            std::string dev, target;
            ss >> dev >> target;
            if (target == mpt)
                return true;
        }
        return false;
    };

    for (const auto& m : mounts)
    {
        const auto part = std::format("/dev/{}p{}", device, m.part_num);
        const auto mpt = std::string{m.mount_point};

        // 1. Create mount point directory
        std::error_code ec;
        std::filesystem::create_directories(mpt, ec);
        if (ec)
        {
            std::cerr << std::format("emmc_mount: mkdir {} failed: {}\n", mpt,
                                     ec.message());
            continue;
        }

        // 2. Already mounted — skip
        if (is_mounted(mpt))
        {
            std::cerr << std::format("emmc_mount: {} already mounted\n", mpt);
            continue;
        }

        // 3. Try to mount
        if (::mount(part.c_str(), mpt.c_str(), "ext4", 0, nullptr) == 0)
        {
            std::cerr << std::format("emmc_mount: {} mounted at {}\n", part,
                                     mpt);
            continue;
        }

        const int err = errno;
        // EINVAL = no valid superblock; ENOENT = partition node not yet in /dev
        if (err != EINVAL && err != ENOENT)
        {
            std::cerr << std::format("emmc_mount: {} mount failed: {}\n", part,
                                     std::strerror(err));
            continue;
        }

        // 4. Format with mkfs.ext4 then mount
        std::cerr << std::format("emmc_mount: {} needs formatting\n", part);
        const auto cmd = std::format("mkfs.ext4 -F {} >/dev/null 2>&1", part);
        if (std::system(cmd.c_str()) != 0)
        {
            std::cerr << std::format("emmc_mount: mkfs.ext4 {} failed\n", part);
            continue;
        }

        if (::mount(part.c_str(), mpt.c_str(), "ext4", 0, nullptr) == 0)
            std::cerr << std::format("emmc_mount: {} formatted+mounted at {}\n",
                                     part, mpt);
        else
            std::cerr << std::format(
                "emmc_mount: {} post-format mount failed: {}\n", part,
                std::strerror(errno));
    }
}

// ── hw-management runtime tree ────────────────────────────────────────────
// The Mellanox hw-management framework publishes its runtime state under
// /var/run/hw-management. The switch bmc_ready scripts reached it through
// $config_path / $system_path from hw-management-helpers.sh; these are the
// same two directories.

inline constexpr std::string_view hw_mgmt_config_path =
    "/var/run/hw-management/config";
inline constexpr std::string_view hw_mgmt_system_path =
    "/var/run/hw-management/system";

// ── Log severity levels ───────────────────────────────────────────────────
// Severity string, from nvidia_event_logging.sh's sevInfo. Only the one the
// recovery-succeeded Redfish entry needs: the error paths write the console
// instead, because nothing they run before can reach the Logging service.
inline constexpr std::string_view log_level_informational =
    "xyz.openbmc_project.Logging.Entry.Level.Informational";

// ── Internal helpers ──────────────────────────────────────────────────────
// Implementation details of the helpers below, not called from platform files.

namespace detail
{

// ── sysfs, procfs and file access ─────────────────────────────────────────

// Write to a sysfs attribute, returning 0 or a positive errno. ofstream
// cannot report why a sysfs write failed, and the GPIO helpers must tell
// EBUSY ("already exported", benign) apart from a real error.
inline int sysfs_write(const std::string& path, std::string_view data)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    const ssize_t written = ::write(fd, data.data(), data.size());
    const int err = written < 0 ? errno : 0;
    ::close(fd);
    return err;
}

// First line of a sysfs attribute, without its newline; nullopt if unreadable.
inline std::optional<std::string> sysfs_read(const std::string& path)
{
    std::ifstream f{path};
    if (!f)
        return std::nullopt;
    std::string value;
    std::getline(f, value);
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
        value.pop_back();
    return value;
}

inline std::vector<uint8_t> read_file_range(const std::string& path,
                                            size_t offset, size_t length)
{
    std::ifstream f{path, std::ios::binary};
    if (!f)
        return {};
    f.seekg(static_cast<std::streamoff>(offset));
    std::vector<uint8_t> buf(length);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(length));
    buf.resize(static_cast<size_t>(f.gcount()));
    return buf;
}

inline bool is_mounted(std::string_view path)
{
    std::ifstream f{"/proc/mounts"};
    std::string line;
    while (std::getline(f, line))
    {
        std::istringstream ss{line};
        std::string dev, mnt;
        ss >> dev >> mnt;
        if (mnt == path)
            return true;
    }
    return false;
}

// ── Raw-byte text helpers ─────────────────────────────────────────────────

inline bool is_whitespace(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// What `echo $var` does to an unquoted expansion of raw EEPROM bytes: the
// shell drops NULs in command substitution, then word splitting collapses
// each run of whitespace to one space and trims both ends.
inline std::string shell_word_join(std::span<const uint8_t> raw)
{
    std::string out;
    bool separator_pending = false;
    for (const uint8_t c : raw)
    {
        if (c == 0x00)
            continue;
        if (is_whitespace(c))
        {
            separator_pending = !out.empty();
            continue;
        }
        if (separator_pending)
        {
            out.push_back(' ');
            separator_pending = false;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// First whitespace-separated word, NULs dropped — the `IFS=$' ' read -r -a
// item_array` + `$item_array` idiom the BOM assembly uses.
inline std::string first_word(std::span<const uint8_t> raw)
{
    std::string word;
    for (const uint8_t c : raw)
    {
        if (c == 0x00)
            continue;
        if (is_whitespace(c))
        {
            if (!word.empty())
                break;
            continue;
        }
        word.push_back(static_cast<char>(c));
    }
    return word;
}

// ── phosphor-logging ──────────────────────────────────────────────────────

// The Logging.Create call behind log_redfish_status(). Waits for the reply so
// the caller can report whether the entry was accepted; log_prefix names that
// caller in the failure message.
inline bool log_create(std::string_view message_id, std::string_view severity,
                       const std::map<std::string, std::string>& data,
                       std::string_view log_prefix)
{
    try
    {
        auto bus = sdbusplus::bus::new_default_system();
        auto m = bus.new_method_call(
            "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
            "xyz.openbmc_project.Logging.Create", "Create");
        m.append(std::string{message_id}, std::string{severity}, data);
        bus.call(m);
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << std::format("{}: {}\n", log_prefix, e.what());
        return false;
    }
}

// Same wire format as log_redfish() above, but reports whether the call
// reached the Logging service. filesystem_check_mtd() needs that: the MTD
// script clears dataflashrecovery only if the busctl call succeeded.
//
// log_redfish() keeps its own implementation rather than forwarding here: it
// publishes with call_noreply(), while reporting status means waiting for the
// reply and so blocking for the D-Bus timeout whenever the Logging service is
// not up yet. Its early-boot callers must not start paying that.
inline bool log_redfish_status(std::string_view message_id,
                               std::string_view args, std::string_view severity)
{
    return log_create(message_id, severity,
                      {
                          {"REDFISH_MESSAGE_ID", std::string{message_id}},
                          {"REDFISH_MESSAGE_ARGS", std::string{args}},
                      },
                      "log_redfish_status");
}

} // namespace detail

// ── U-Boot environment access ─────────────────────────────────────────────
// Source: filesystem_check.sh (fw_printenv / fw_setenv) and
//         bmc_ready_common.sh:bmc_init_bootargs
// Promoted out of filesystem_check(), which held these as private lambdas —
// bmc_init_bootargs and filesystem_check_mtd need the same two operations.
//
// Shell-out to fw_printenv / fw_setenv: they read the U-Boot env in flash at
// offsets from /etc/fw_env.config, with no stable C API for that short of
// adding libubootenv as a dependency.

inline std::string uboot_env_get(std::string_view key)
{
    const auto cmd = std::format("fw_printenv -n {} 2>/dev/null", key);
    auto* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe)
        return {};
    char buf[256]{};
    std::string result;
    while (::fgets(buf, sizeof(buf), pipe))
        result += buf;
    ::pclose(pipe);
    while (!result.empty() && result.back() == '\n')
        result.pop_back();
    return result;
}

// An empty value unsets the variable, matching `fw_setenv <key>`.
inline void uboot_env_set(std::string_view key, std::string_view value = {})
{
    const auto cmd =
        value.empty() ? std::format("fw_setenv {} 2>/dev/null", key)
                      : std::format("fw_setenv {} {} 2>/dev/null", key, value);
    // Best-effort, but a non-zero status means the value did not persist,
    // which for a recovery flag would let the reboot loop repeat. Log it
    // rather than discard the result.
    const int rc = ::system(cmd.c_str()); // NOLINT(cert-env33-c)
    if (rc != 0)
    {
        std::cerr << std::format("uboot_env_set: '{}' failed (status {})\n",
                                 cmd, rc);
    }
}

// Source: bmc_init_bootargs, byte-identical in the meta-rosalind
//         (bmc_ready_common.sh:33-46) and meta-spc6-ast2600
//         (bmc_ready_common.sh:85-98) shell libraries. The meta-ast2700 copy
//         is a different function and is ported locally in
//         switch_ast2700_bmc.cpp instead.
// The ttyS2 + host MAC pair is the "already rewritten" marker, so a BMC that
// has been through this once does not rewrite the U-Boot env on every boot.
inline void bmc_init_bootargs()
{
    // Standalone BMC system, no system EEPROM.
    if (!std::filesystem::is_directory("/sys/class/net/eth1"))
    {
        uboot_env_set("bootargs", "console=ttyS4,115200n8 root=/dev/ram rw");
    }

    const auto bootargs = uboot_env_get("bootargs");
    if (bootargs.contains("ttyS2") && bootargs.contains("46:44:8a:c8:7f:bf"))
    {
        return;
    }

    uboot_env_set("bootargs", "console=ttyS2,115200n8 root=/dev/ram rw "
                              "g_ether.host_addr=46:44:8a:c8:7f:bf "
                              "g_ether.dev_addr=46:44:8a:c8:7f:bd");
}

// ── Logged shell-out ──────────────────────────────────────────────────────
// Source: the `calling <cmd>` line and the failure report the switch
// bmc_ready.sh scripts wrote around every external command.
//
// Takes a view but materialises the buffer: std::system() needs a
// NUL-terminated string, and callers pass both literals and std::format()
// results.
inline void shell_out(std::string_view cmd, std::string_view log_prefix)
{
    const std::string command{cmd};
    std::cerr << std::format("{}: calling {}\n", log_prefix, command);
    if (const int rc = std::system(command.c_str());
        rc != 0) // NOLINT(cert-env33-c)
    {
        std::cerr << std::format("{}: '{}' failed (status {})\n", log_prefix,
                                 command, rc);
    }
}

// ── hw-management sysfs access ────────────────────────────────────────────

// Resolve the `hwmon*` glob the shell used to spell the $BMC_CPU_PWR_ON family
// of mlxreg-io paths. The hwmon instance number is assigned at probe time and
// is not stable across boots, hence the glob rather than a fixed path.
// hwmon_dir is the per-platform .../mlxreg-io/hwmon directory; returns the
// resolved attribute path, or an empty string when no instance exposes it.
inline std::string hw_mgmt_hwmon_attr(std::string_view hwmon_dir,
                                      std::string_view attribute,
                                      std::string_view log_prefix)
{
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{hwmon_dir, ec})
    {
        if (!entry.path().filename().string().starts_with("hwmon"))
        {
            continue;
        }
        const auto path = (entry.path() / attribute).string();
        if (std::filesystem::exists(path))
        {
            return path;
        }
    }
    std::cerr << std::format("{}: no mlxreg-io hwmon instance exposing {}\n",
                             log_prefix, attribute);
    return {};
}

// Source: bmc_init_main's `CPU_OFF_CMD=$(< $BMC_CPU_PWR_ON)` reads.
// The path is a hw-management attribute, not a GPIO line name, so gpio::get()
// cannot address it.
inline std::optional<uint32_t> hw_mgmt_read_u32(std::string_view path)
{
    const auto raw = detail::sysfs_read(std::string{path});
    if (!raw || raw->empty())
    {
        std::cerr << std::format("hw_mgmt_read_u32: cannot read {}\n", path);
        return std::nullopt;
    }
    try
    {
        return static_cast<uint32_t>(std::stoul(*raw, nullptr, 0));
    }
    catch (const std::exception&)
    {
        std::cerr << std::format(
            "hw_mgmt_read_u32: {}: unparsable value '{}'\n", path, *raw);
        return std::nullopt;
    }
}

// Source: bmc_init_main's `echo 1 > $BMC_TO_CPU_CTRL`.
inline bool hw_mgmt_write(std::string_view path, std::string_view value)
{
    if (const int err = detail::sysfs_write(std::string{path}, value); err != 0)
    {
        std::cerr << std::format("hw_mgmt_write: {} <- '{}': {}\n", path, value,
                                 std::strerror(err));
        return false;
    }
    return true;
}

// Source: get_cpu_type / get_system_hw_id / get_system_hw_bom, all of which
// end in `echo <value> > $config_path/<name>`. The trailing newline echo adds
// is part of the file format the hw-management consumers expect.
inline bool hw_mgmt_write_config(std::string_view name, std::string_view value)
{
    std::error_code ec;
    std::filesystem::create_directories(hw_mgmt_config_path, ec);
    const auto path = std::format("{}/{}", hw_mgmt_config_path, name);
    std::ofstream f{path, std::ios::trunc};
    if (!f)
    {
        std::cerr << std::format("hw_mgmt_write_config: cannot open {}\n",
                                 path);
        return false;
    }
    f << value << "\n";
    return f.good();
}

// Source: get_cpu_type — `grep -m1 "CPU part" /proc/cpuinfo | awk '{print $4}'`
//         then `cut -c 3- | tr a-z A-Z`, re-prefixed with a lower-case 0x.
// hw-management's check_cpu_type compares the file byte for byte against
// ARMv7_CPU / ARMv8_CPU, so that exact spelling is the file format. On a CPU
// with no "CPU part" line the shell wrote a bare "0x", which no consumer
// accepts; write nothing instead.
inline void write_cpu_type(std::string_view log_prefix)
{
    std::ifstream cpuinfo{"/proc/cpuinfo"};
    for (std::string line; std::getline(cpuinfo, line);)
    {
        if (!line.contains("CPU part"))
        {
            continue;
        }

        std::istringstream fields{line};
        std::string word, fourth;
        for (int i = 0; i < 4 && fields >> word; ++i)
        {
            fourth = word;
        }
        if (fourth.size() <= 2)
        {
            break;
        }

        std::string digits = fourth.substr(2);
        for (auto& c : digits)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        const auto cpu_type = "0x" + digits;
        std::cerr << std::format("{}: cpu type {}\n", log_prefix, cpu_type);
        hw_mgmt_write_config("cpu_type", cpu_type);
        return;
    }
    std::cerr << std::format("{}: no \"CPU part\" line in /proc/cpuinfo; "
                             "cpu_type not written\n",
                             log_prefix);
}

// Source: bmc_init_eth (hw-management-helpers-common.sh:557) — ~120 lines of
// Marvell/Realtek MDIO register pokes and link checks, kept in shell. It is a
// shell function rather than a script, so it has to be sourced before it can be
// called. Every switch platform invokes it with the same command.
inline void hw_mgmt_init_eth(std::string_view log_prefix)
{
    shell_out("bash -c '. /usr/bin/hw-management-helpers-common.sh"
              " && bmc_init_eth'",
              log_prefix);
}

// Source: bmc_init_main — either the commanded power-down or the power-button
// halt means the host is not driving the I2C tree, so the BMC may take it.
// Returns whether the CPU is off, which a caller may also need in order to
// decide whether board data can be reprogrammed.
inline bool hw_mgmt_take_i2c_when_cpu_off(std::string_view hwmon_dir,
                                          std::string_view log_prefix)
{
    const auto pwr_down =
        hw_mgmt_read_u32(hw_mgmt_hwmon_attr(hwmon_dir, "pwr_down", log_prefix));
    const auto pwr_button_halt = hw_mgmt_read_u32(
        hw_mgmt_hwmon_attr(hwmon_dir, "pwr_button_halt", log_prefix));
    const bool cpu_off = pwr_down.value_or(0) || pwr_button_halt.value_or(0);
    if (cpu_off)
    {
        hw_mgmt_write(
            hw_mgmt_hwmon_attr(hwmon_dir, "bmc_to_cpu_ctrl", log_prefix), "1");
    }
    return cpu_off;
}

// Source: the guard every switch platform runs before touching the
// hw-management tree.
//
// Deliberately fails open, exactly as the shell did: log and carry on. The
// symlink tree and every later hw-management consumer are then broken, but the
// BMC still reaches standby and can power the host.
inline void hw_mgmt_wait_system_tree(
    std::string_view log_prefix,
    std::chrono::seconds timeout = std::chrono::seconds{20})
{
    wait_for_path_to_exist(hw_mgmt_system_path, timeout);
    if (!std::filesystem::is_directory(hw_mgmt_system_path))
    {
        std::cerr << std::format("{}: timed out waiting for {} to "
                                 "become available\n",
                                 log_prefix, hw_mgmt_system_path);
    }
}

// ── Legacy sysfs GPIO ─────────────────────────────────────────────────────
// Source: switch_gpio_set.sh:gpio_export / gpio_dir / gpio_set and
//         rosalind_bmc_ready.sh:bmc_init_sysfs_gpio
//
// These helpers drive /sys/class/gpio directly and must NOT reach into the
// gpio:: (libgpiod) namespace. hw-management consumes the exports through the
// /var/run/hw-management/system/<NAME> symlink tree and needs them to outlive
// init, while gpio::release_all() drops every line gpio::set() acquired. The
// two interfaces are mutually exclusive per line, so a line handled here must
// never also be passed to gpio::set/get/Event/set_raw.

enum class SysfsGpioDir : uint8_t
{
    In,
    Out,
    // Export and name the line without disturbing the direction or value its
    // controller retained — the JSON config's "preserve".
    Preserve,
};

struct SysfsGpioPin
{
    int number; // absolute sysfs GPIO number (chip base + offset)
    SysfsGpioDir direction = SysfsGpioDir::Preserve;
    std::optional<int> value = std::nullopt; // applied to outputs only
    std::string symlink;                     // name under system_path, or ""
};

inline std::string sysfs_gpio_value_path(int number)
{
    return std::format("/sys/class/gpio/gpio{}/value", number);
}

// Export each line, then set direction and (for outputs carrying a value) the
// output level. An already-exported line is not an error: the export write
// fails with EBUSY, which is logged and skipped so a manual
// `systemctl restart platform_init` is safe.
inline void sysfs_gpio_apply(std::span<const SysfsGpioPin> pins)
{
    for (const auto& pin : pins)
    {
        const auto dir = std::format("/sys/class/gpio/gpio{}", pin.number);
        if (!std::filesystem::exists(dir))
        {
            const int err = detail::sysfs_write("/sys/class/gpio/export",
                                                std::to_string(pin.number));
            if (err == EBUSY)
            {
                std::cerr << std::format(
                    "sysfs_gpio_apply: gpio{} already exported\n", pin.number);
            }
            else if (err != 0)
            {
                std::cerr << std::format(
                    "sysfs_gpio_apply: export gpio{}: {}\n", pin.number,
                    std::strerror(err));
                continue;
            }
        }

        if (pin.direction == SysfsGpioDir::Preserve)
            continue;

        const auto* direction =
            pin.direction == SysfsGpioDir::Out ? "out" : "in";
        if (const int err = detail::sysfs_write(dir + "/direction", direction);
            err != 0)
        {
            // As in the shell (`gpio_dir ... || true`), a direction failure
            // does not stop the value write: on some expanders the line is
            // usable even when the direction attribute is not writable.
            std::cerr << std::format(
                "sysfs_gpio_apply: gpio{} direction={}: {}\n", pin.number,
                direction, std::strerror(err));
        }

        if (pin.direction != SysfsGpioDir::Out || !pin.value)
            continue;
        if (const int err =
                detail::sysfs_write(dir + "/value", std::to_string(*pin.value));
            err != 0)
        {
            std::cerr << std::format("sysfs_gpio_apply: gpio{} value={}: {}\n",
                                     pin.number, *pin.value,
                                     std::strerror(err));
        }
    }
}

// Source: hw-management-helpers.sh:check_n_link, called once per named line
//         from bmc_init_sysfs_gpio.
// Publishes each pin's value attribute as <system_path>/<symlink>. Mirrors
// check_n_link: a pin whose value attribute does not exist is skipped, and an
// existing link is replaced (`ln -sf`), so a re-run is safe.
inline void hw_mgmt_link_gpio(
    std::span<const SysfsGpioPin> pins,
    std::string_view system_path = hw_mgmt_system_path)
{
    for (const auto& pin : pins)
    {
        if (pin.symlink.empty())
            continue;
        const auto source = sysfs_gpio_value_path(pin.number);
        if (!std::filesystem::exists(source))
        {
            std::cerr << std::format(
                "hw_mgmt_link_gpio: {} absent; not linking {}\n", source,
                pin.symlink);
            continue;
        }
        const auto link = std::format("{}/{}", system_path, pin.symlink);
        std::error_code ec;
        std::filesystem::remove(link, ec);
        std::filesystem::create_symlink(source, link, ec);
        if (ec)
        {
            std::cerr << std::format("hw_mgmt_link_gpio: link {} -> {}: {}\n",
                                     link, source, ec.message());
        }
    }
}

// ── sysfs gpiochip base discovery ─────────────────────────────────────────
// Source: switch_gpio_set.sh:gpiochip_base_by_ngpio / gpiochip_base_aspeed /
//         gpiochip_base_by_i2c
// These return the sysfs *base* number. gpio::find_chip_idx_from_dir()
// returns a libgpiod chip index, which is a different number — the two are
// not interchangeable.

namespace detail
{

inline std::optional<int> gpiochip_attr_int(const std::filesystem::path& chip,
                                            const char* attr)
{
    const auto raw = sysfs_read((chip / attr).string());
    if (!raw || raw->empty())
        return std::nullopt;
    try
    {
        return std::stoi(*raw);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

// /sys/class/gpio/gpiochip* in lexicographic order, matching the order the
// shell's glob expansion produced.
inline std::vector<std::filesystem::path> gpiochip_dirs()
{
    std::vector<std::filesystem::path> chips;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator{"/sys/class/gpio", ec})
    {
        if (entry.path().filename().string().starts_with("gpiochip"))
            chips.push_back(entry.path());
    }
    std::ranges::sort(chips);
    return chips;
}

} // namespace detail

// quiet suppresses the not-found diagnostic, for probes that have a fallback.
inline std::optional<int> sysfs_gpiochip_base_by_ngpio(int ngpio,
                                                       bool quiet = false)
{
    for (const auto& chip : detail::gpiochip_dirs())
    {
        if (detail::gpiochip_attr_int(chip, "ngpio") != ngpio)
            continue;
        if (const auto base = detail::gpiochip_attr_int(chip, "base"); base)
            return base;
    }
    if (!quiet)
    {
        std::cerr << std::format(
            "sysfs_gpiochip_base_by_ngpio: no gpiochip with ngpio={}\n", ngpio);
    }
    return std::nullopt;
}

// AST2600 exposes 208 lines and AST2700 216; some kernels report a different
// count for the SoC bank, hence the 200–230 fallback scan.
inline std::optional<int> sysfs_gpiochip_base_aspeed()
{
    if (const auto base = sysfs_gpiochip_base_by_ngpio(208, true); base)
        return base;
    if (const auto base = sysfs_gpiochip_base_by_ngpio(216, true); base)
        return base;
    for (const auto& chip : detail::gpiochip_dirs())
    {
        const auto ngpio = detail::gpiochip_attr_int(chip, "ngpio");
        if (!ngpio || *ngpio < 200 || *ngpio > 230)
            continue;
        if (const auto base = detail::gpiochip_attr_int(chip, "base"); base)
        {
            std::cerr << std::format(
                "sysfs_gpiochip_base_aspeed: using {} ngpio={} base={} "
                "(fallback)\n",
                chip.filename().string(), *ngpio, *base);
            return base;
        }
    }
    std::cerr << "sysfs_gpiochip_base_aspeed: no Aspeed GPIO chip "
                 "(tried ngpio=208,216 and 200<=ngpio<=230)\n";
    return std::nullopt;
}

// i2c_id is the sysfs client name, "<bus>-<addr>" (e.g. "5-0020"). Needed on
// boards carrying two identical expanders — Magnetar and Rosalind V3000 each
// have a PCAL6524 on i2c-5 and another on i2c-7, so ngpio cannot tell them
// apart.
inline std::optional<int> sysfs_gpiochip_base_by_i2c(std::string_view i2c_id)
{
    if (i2c_id.empty())
    {
        std::cerr << "sysfs_gpiochip_base_by_i2c: empty i2c id\n";
        return std::nullopt;
    }
    for (const auto& chip : detail::gpiochip_dirs())
    {
        std::error_code ec;
        // gpiochipN/device resolves to the i2c client that registered the
        // gpio_chip; its basename is "<bus>-<addr>".
        const auto device = std::filesystem::canonical(chip / "device", ec);
        if (ec || device.filename().string() != i2c_id)
            continue;
        if (const auto base = detail::gpiochip_attr_int(chip, "base"); base)
            return base;
    }
    std::cerr << std::format(
        "sysfs_gpiochip_base_by_i2c: no gpiochip for i2c device {}\n", i2c_id);
    return std::nullopt;
}

// Overload taking the bus and address separately, formatted the way sysfs
// names the client.
inline std::optional<int> sysfs_gpiochip_base_by_i2c(unsigned bus,
                                                     unsigned address)
{
    return sysfs_gpiochip_base_by_i2c(std::format("{}-{:04x}", bus, address));
}

// Source: switch_gpio_set.sh:gpio_base_for_chip_id — resolves the logical
// chip id used by the pins JSON. Its caller is the pins-JSON reader in
// switch_ast2700_bmc.cpp; it stays here beside the three resolvers it
// dispatches to, two of which the platform files call directly.
inline std::optional<int> sysfs_gpiochip_base_for_chip_id(std::string_view chip)
{
    if (chip == "aspeed")
        return sysfs_gpiochip_base_aspeed();
    if (chip == "expander24")
        return sysfs_gpiochip_base_by_ngpio(24);
    if (chip.starts_with("i2c:"))
        return sysfs_gpiochip_base_by_i2c(chip.substr(4));
    std::cerr << std::format(
        "sysfs_gpiochip_base_for_chip_id: unknown chip id '{}'\n", chip);
    return std::nullopt;
}

// ── sysfs value poll ──────────────────────────────────────────────────────
// Source: wait_bmc_standby_ready (10 s timeout, 1 s interval, on
//         BMC_STBY_READY).
// wait_gpio_high() cannot serve this: it takes a libgpiod line name, whereas
// the path here is computed at runtime from a discovered gpiochip base or
// from the pins JSON.
inline bool wait_sysfs_value(
    std::string_view path, std::string_view expected, int retries = 10,
    std::chrono::milliseconds delay = std::chrono::milliseconds{1000})
{
    std::cerr << std::format("wait_sysfs_value: waiting for {} == {}\n", path,
                             expected);
    for (int attempt = 0; attempt <= retries; ++attempt)
    {
        if (const auto value = detail::sysfs_read(std::string{path});
            value && *value == expected)
        {
            std::cerr << std::format("wait_sysfs_value: {} == {}\n", path,
                                     expected);
            return true;
        }
        if (attempt < retries)
            std::this_thread::sleep_for(delay);
    }
    std::cerr << std::format(
        "wait_sysfs_value: TIMEOUT {} != {} after {} x {}ms\n", path, expected,
        retries, delay.count());
    return false;
}

// ── VPD EEPROM fields ─────────────────────────────────────────────────────

// Source: get_system_hw_id — `dd if=<eeprom> bs=1 skip=<offset> count=<len>`.
inline std::string vpd_read_field(std::string_view eeprom_path, size_t offset,
                                  size_t length)
{
    const auto raw =
        detail::read_file_range(std::string{eeprom_path}, offset, length);
    if (raw.empty())
    {
        std::cerr << std::format("vpd_read_field: {} offset {} len {}: "
                                 "no data\n",
                                 eeprom_path, offset, length);
        return {};
    }
    return detail::shell_word_join(raw);
}

// Source: get_system_hw_bom's `strings -a -n 3 -t d | awk 'match($2,
//         /V[0-9]-/) { print $1 + RSTART - 1; exit }'` offset probe.
// The awk expression reports the absolute offset of the first "V<digit>-" in a
// printable run, and those three characters are themselves printable, so a
// direct search over the leading bytes is equivalent.
inline std::optional<size_t> vpd_find_bom_offset(std::string_view eeprom_path,
                                                 size_t scan_length = 128)
{
    const auto head =
        detail::read_file_range(std::string{eeprom_path}, 0, scan_length);
    for (size_t i = 0; i + 2 < head.size(); ++i)
    {
        if (head[i] == 'V' && head[i + 1] >= '0' && head[i + 1] <= '9' &&
            head[i + 2] == '-')
        {
            return i;
        }
    }
    return std::nullopt;
}

// Source: get_system_hw_bom (all three platforms).
// Reads the BOM record, splits it on 0xff and concatenates the first word of
// each field. Rosalind passes the fixed offset 118; AST2600 and AST2700 leave
// it unset so it is discovered with vpd_find_bom_offset(). Returns an empty
// string when the HID is outside the platform's allow-list, when the record
// offset cannot be found, or when the EEPROM cannot be read — in each case the
// shell wrote no bom file either.
inline std::string vpd_read_bom(
    std::string_view eeprom_path, std::string_view hid,
    std::span<const std::string_view> allowed_hids,
    std::optional<size_t> offset = std::nullopt, size_t length = 192)
{
    if (std::ranges::find(allowed_hids, hid) == allowed_hids.end())
    {
        std::cerr << std::format(
            "vpd_read_bom: hid {} has no BOM record; skipping\n", hid);
        return {};
    }

    const auto record_offset =
        offset ? offset : vpd_find_bom_offset(eeprom_path);
    if (!record_offset)
    {
        std::cerr << std::format(
            "vpd_read_bom: no 'V<digit>-' BOM marker in {}\n", eeprom_path);
        return {};
    }

    const auto record = detail::read_file_range(std::string{eeprom_path},
                                                *record_offset, length);
    if (record.empty())
    {
        std::cerr << std::format("vpd_read_bom: cannot read {} at offset {}\n",
                                 eeprom_path, *record_offset);
        return {};
    }

    std::string bom;
    for (size_t pos = 0; pos < record.size();)
    {
        size_t end = pos;
        while (end < record.size() && record[end] != 0xff)
            ++end;
        bom += detail::first_word(
            std::span<const uint8_t>{record}.subspan(pos, end - pos));
        pos = end + 1; // step over the 0xff field separator
    }
    return bom;
}

// Source: fru_rev_id_from_eeprom (hw-management-helpers-common.sh)
// Parse IPMI FRU Board Extra fields. RevId is the two custom extras immediately
// before the HID field (e.g. "18" + "A1" -> "18a1").
//
// One sequential 2KiB read: seeking on at24 sysfs re-reads from offset 0 and
// races the parallel ftgmac100 probe (bmc-eth-driver TimeoutStartSec=60).
inline std::string fru_rev_id_from_eeprom(std::string_view eeprom_path)
{
    constexpr size_t fru_prefix = 2048;
    const auto buf =
        detail::read_file_range(std::string{eeprom_path}, 0, fru_prefix);
    if (buf.size() < 8)
    {
        return {};
    }

    const size_t board_off = static_cast<size_t>(buf[3]) * 8;
    if (board_off + 1 >= buf.size())
    {
        return {};
    }
    const size_t board_len = static_cast<size_t>(buf[board_off + 1]) * 8;
    if (board_len < 8 || board_off + board_len > buf.size())
    {
        return {};
    }
    const size_t area_end = board_off + board_len;

    const auto field_text = [](std::span<const uint8_t> raw) {
        std::string text;
        text.reserve(raw.size());
        for (const uint8_t c : raw)
        {
            if (c == 0x00 || c == '\r' || c == '\n')
            {
                continue;
            }
            text.push_back(static_cast<char>(c));
        }
        size_t begin = 0;
        while (begin < text.size() &&
               detail::is_whitespace(static_cast<uint8_t>(text[begin])))
        {
            ++begin;
        }
        size_t end = text.size();
        while (end > begin &&
               detail::is_whitespace(static_cast<uint8_t>(text[end - 1])))
        {
            --end;
        }
        return text.substr(begin, end - begin);
    };

    const auto is_hid_extra = [](std::string_view field) {
        if (field.size() != 5 && field.size() != 6)
        {
            return false;
        }
        if (std::toupper(static_cast<unsigned char>(field[0])) != 'H' ||
            std::toupper(static_cast<unsigned char>(field[1])) != 'I')
        {
            return false;
        }
        size_t digit0 = 2;
        if (field.size() == 6)
        {
            if (std::toupper(static_cast<unsigned char>(field[2])) != 'D')
            {
                return false;
            }
            digit0 = 3;
        }
        return std::ranges::all_of(field.substr(digit0), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });
    };

    std::vector<std::string> extras;
    int field_idx = 0;
    size_t pos = board_off + 6;
    while (pos + 1 < area_end)
    {
        const uint8_t tl = buf[pos];
        // 0xC1 end-of-fields; 0xFF padding/end used by Rosalind VPD.
        if (tl == 0xC1 || tl == 0xFF)
        {
            break;
        }
        const size_t len = tl & 0x3f;
        ++pos;
        if (pos + len > area_end)
        {
            break;
        }
        if (field_idx >= 5)
        {
            // Keep empty extras: dropping them compresses FRU field order,
            // so a blank extra before HID would pick the wrong pair or miss
            // the two extras immediately before the HID field.
            extras.push_back(
                field_text(std::span<const uint8_t>{buf}.subspan(pos, len)));
        }
        pos += len;
        ++field_idx;
    }

    int hid_i = -1;
    for (size_t i = 0; i < extras.size(); ++i)
    {
        if (is_hid_extra(extras[i]))
        {
            hid_i = static_cast<int>(i);
            break;
        }
    }
    if (hid_i < 2)
    {
        return {};
    }

    std::string rev = extras[static_cast<size_t>(hid_i - 2)] +
                      extras[static_cast<size_t>(hid_i - 1)];
    std::string out;
    out.reserve(rev.size());
    for (const unsigned char c : rev)
    {
        if (detail::is_whitespace(c))
        {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

// Source: get_system_hw_rev (hw-management-helpers-common.sh)
// Write $config_path/rev_id from SYS_FRU Board Extra (e.g. 18a1). HID
// allow-list matches SKUs that already parse VPD BOM from EEPROM. Unknown
// HIDs are a silent no-op, matching the shell case statement.
inline void write_system_hw_rev(
    std::string_view eeprom_path, std::string_view hid,
    std::span<const std::string_view> allowed_hids, std::string_view log_prefix)
{
    if (eeprom_path.empty() ||
        std::ranges::find(allowed_hids, hid) == allowed_hids.end())
    {
        return;
    }

    const auto rev = fru_rev_id_from_eeprom(eeprom_path);
    if (rev.empty())
    {
        std::cerr << std::format("{}: [WARN] Could not parse system revision "
                                 "Id from VPD EEPROM\n",
                                 log_prefix);
        return;
    }
    std::cerr << std::format("{}: system hardware revision Id is {}\n",
                             log_prefix, rev);
    hw_mgmt_write_config("rev_id", rev);
}

// ── Module loading ────────────────────────────────────────────────────────

// Source: bmc_ready_common.sh:bmc_init_usb_smbus_bridge
//         (`timeout -s KILL 5 modprobe hid_cp2112`) and
//         meta-spc6-ast2600 bmc_ready_common.sh:bind_i3c_controller
//         (`modprobe -i <module>`).
// Returns the exit status: 0 on success, 137 (128 + SIGKILL) when the timeout
// fired, otherwise modprobe's own status. Callers must not fail init on a
// non-zero result — a missing bridge only degrades ASIC firmware recovery.
inline int modprobe_with_timeout(
    std::string_view module,
    std::chrono::seconds timeout = std::chrono::seconds{5},
    bool ignore_install = false)
{
    // BusyBox timeout syntax: timeout [-s SIG] SECS PROG ARGS. -s KILL sends
    // SIGKILL straight away, with no graceful SIGTERM first.
    const auto cmd =
        std::format("timeout -s KILL {} modprobe {}{}", timeout.count(),
                    ignore_install ? "-i " : "", module);
    const int raw = std::system(cmd.c_str()); // NOLINT(cert-env33-c)
    int status = raw;
    if (raw != -1)
    {
        if (WIFEXITED(raw))
            status = WEXITSTATUS(raw);
        else if (WIFSIGNALED(raw))
            status = 128 + WTERMSIG(raw);
    }

    if (status == 0)
        std::cerr << std::format("modprobe_with_timeout: {} loaded\n", module);
    else if (status == 137)
        std::cerr << std::format(
            "modprobe_with_timeout: {} timed out after {}s\n", module,
            timeout.count());
    else
        std::cerr << std::format(
            "modprobe_with_timeout: {} failed (status {})\n", module, status);
    return status;
}

// Source: bind_i3c_controller's `lsmod | grep mipi-i3c-hci`.
// /proc/modules — which is what lsmod prints — renders module names with
// underscores, so the shell's hyphenated grep could never match and the
// early-return never fired. Comparing the normalised name makes the check work
// as intended, which is also what makes a repeat invocation cheap (B3).
inline bool module_loaded(std::string_view module)
{
    const auto normalise = [](std::string_view name) {
        std::string out{name};
        std::ranges::replace(out, '-', '_');
        return out;
    };
    const auto wanted = normalise(module);

    std::ifstream f{"/proc/modules"};
    std::string line;
    while (std::getline(f, line))
    {
        const auto space = line.find(' ');
        const auto name =
            space == std::string::npos ? line : line.substr(0, space);
        if (normalise(name) == wanted)
            return true;
    }
    return false;
}

// ── NOS fixup hook ────────────────────────────────────────────────────────
// Source: rosalind_bmc_ready.sh:run_hook
// The NOS may drop a fixup script in to work around a field issue until a
// firmware release carries the fix. Copy it aside so the NOS can tell which
// one last ran, execute it for this phase, and publish its status.
inline void run_nos_hook(
    std::string_view phase,
    std::string_view hook_file = "/usr/local/bin/hw-management-bmc-fixup.sh")
{
    const std::filesystem::path hook{hook_file};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(hook, ec))
    {
        std::cerr << std::format(
            "run_nos_hook: no hook file, {} init hooks are not performed\n",
            phase);
        return;
    }

    if (::access(hook.c_str(), X_OK) != 0)
    {
        std::cerr << std::format(
            "run_nos_hook: '{}' is not executable, changing permissions\n",
            hook_file);
        std::filesystem::permissions(hook,
                                     std::filesystem::perms::owner_exec |
                                         std::filesystem::perms::group_exec |
                                         std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add, ec);
        if (ec)
        {
            std::cerr << std::format("run_nos_hook: chmod +x {}: {}\n",
                                     hook_file, ec.message());
        }
    }

    std::filesystem::create_directories(hw_mgmt_config_path, ec);
    std::filesystem::copy_file(
        hook, std::format("{}/last-executed-fixup.sh", hw_mgmt_config_path),
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        std::cerr << std::format("run_nos_hook: copy {} aside failed: {}\n",
                                 hook_file, ec.message());
    }

    const auto cmd = std::format("{} {}", hook_file, phase);
    const int raw = std::system(cmd.c_str()); // NOLINT(cert-env33-c)
    const int retval = (raw != -1 && WIFEXITED(raw)) ? WEXITSTATUS(raw) : raw;

    hw_mgmt_write_config(std::format("fixup-status-{}", phase),
                         std::to_string(retval));

    if (retval == 0)
    {
        std::cerr << std::format(
            "run_nos_hook: '{}' executed successfully with parameter {}\n",
            hook_file, phase);
    }
    else
    {
        std::cerr << std::format("run_nos_hook: execution of '{}' with "
                                 "parameter {} failed with return value {}\n",
                                 hook_file, phase, retval);
    }
}

// Source: rosalind_bmc_ready.sh:bmc_init_exit_handler — the
//         `trap bmc_init_exit_handler EXIT` installed inside the bmc_init_main
//         subshell, so the post hook runs however init leaves it, including
//         every early `return 1`.
//
// Scope this guard to the block that mirrors bmc_init_main, not to the whole
// init function: every platform init function ends in pause(), so a
// function-scope guard would never be destroyed on the success path.
struct NosHookGuard
{
    NosHookGuard() = default;
    NosHookGuard(const NosHookGuard&) = delete;
    NosHookGuard& operator=(const NosHookGuard&) = delete;
    NosHookGuard(NosHookGuard&&) = delete;
    NosHookGuard& operator=(NosHookGuard&&) = delete;
    ~NosHookGuard()
    {
        run_nos_hook("post");
    }
};

// ── Platform feature flags ────────────────────────────────────────────────
// Source: hw-management-helpers-common.sh:PLATFORM_FEATURES_FILE, the same file
// read by enable_write_protect_on_boot.sh and set_unset_write_protect.sh.
// bmc-post-boot-cfg delivers /etc/platform_features.conf per machine, and on
// AST2700 per HID through /run/plat_active. It carries WRITE_PROTECT_MODE and
// LEAK_DETECTION_SOURCE today.
//
// The shell consumers `source` it; this reads it as key=value instead, since
// the file is configuration and executing it is a capability no consumer needs.
// A missing file, key or value yields the empty string, which callers must
// treat as the most conservative mode. Later assignments win, as when sourcing.
inline std::string platform_feature(
    std::string_view key, std::string_view path = "/etc/platform_features.conf")
{
    std::ifstream conf{std::string{path}};
    if (!conf)
    {
        return {};
    }

    const auto trim = [](std::string_view s) {
        while (!s.empty() &&
               detail::is_whitespace(static_cast<uint8_t>(s.front())))
            s.remove_prefix(1);
        while (!s.empty() &&
               detail::is_whitespace(static_cast<uint8_t>(s.back())))
            s.remove_suffix(1);
        return s;
    };

    std::string value;
    std::string line;
    while (std::getline(conf, line))
    {
        const auto stripped = trim(line);
        if (stripped.empty() || stripped.front() == '#')
        {
            continue;
        }

        const auto eq = stripped.find('=');
        if (eq == std::string_view::npos || trim(stripped.substr(0, eq)) != key)
        {
            continue;
        }

        auto raw = trim(stripped.substr(eq + 1));
        if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == '\'') &&
            raw.back() == raw.front())
        {
            raw = raw.substr(1, raw.size() - 2);
        }
        value = raw;
    }
    return value;
}

// ── Software write protect ────────────────────────────────────────────────
// Source: hw-management-helpers-common.sh:unset_write_protect_on_boot, which
// bmc_init_main called on boards shipping in unset_on_boot mode. Write protect
// is dropped once here and never enabled again, because
// set_unset_write_protect.sh refuses to enable it in that mode. Platforms in
// supported mode leave that to enable_write_protect_on_boot.service, and
// unsupported ones never touch the line. The SetWriteProtectInit(true) call and
// one-second sleep the shell did first are not reproduced: writeprotect is not
// installed on the SPC6 images, so no server answered that call there.
inline void unset_write_protect_on_boot(
    std::string_view system_path = hw_mgmt_system_path)
{
    if (platform_feature("WRITE_PROTECT_MODE") != "unset_on_boot")
    {
        return;
    }

    const auto wp_sysfs = std::format("{}/GP_BMC_WP_CTRL_GPIO_L", system_path);
    if (!std::filesystem::exists(wp_sysfs))
    {
        std::cerr << "Write protect sysfs node is missing, not disabling "
                     "write protect\n";
        return;
    }

    std::cerr << "Disabling write protect, platform write protect mode is "
                 "unset_on_boot.\n";
    hw_mgmt_write(wp_sysfs, "1");
}

// ── Milestone logging ─────────────────────────────────────────────────────
// Source: nvidia_event_logging.sh:phosphor_log +
//         hw-management-helpers-common.sh:log_event_console
// One emitter for the two sinks the switch reached from two separate call
// sites: the journal and the serial console. The D-Bus Logging entry the shell
// also sent is deliberately dropped — platform_init.service is ordered before
// phosphor-log-manager, so Logging.Create cannot be served this early in boot.
inline void phosphor_log_all(std::string_view message)
{
    // Journal, via the unit's SyslogIdentifier.
    std::cerr << message << "\n";

    // log_event_console: printf '\n%s\n\n', guarded on the console being
    // writable so a headless boot does not fail here.
    if (::access("/dev/console", W_OK) == 0)
    {
        if (std::ofstream console{"/dev/console"}; console)
            console << std::format("\n{}\n\n", message);
    }
}

// ── I3C controller bind ───────────────────────────────────────────────────
// Source: meta-spc6-ast2600 bmc_ready_common.sh:bind_i3c_controller (16-83)
// The I3C pull-ups are not powered during kernel init, so the drivers ship as
// blacklisted modules and are loaded only once the CPLD has powered the ASICs.
// Two entry points: inline from AST2600 init when the CPU is
// already up, and the bind-i3c/ module, which builds the same code into
// platform-init-i3c-bind and ships platform-init-i3c-bind.service for
// hw-management-events.sh to start on CPU_RESET_DEASSERT.

// Source: `i2cdetect -l | grep mux` — the mux adapters carry "mux" in their
//         adapter name.
inline bool i2c_mux_present()
{
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator{"/sys/class/i2c-dev", ec})
    {
        const auto name = detail::sysfs_read((entry.path() / "name").string());
        if (name && name->find("mux") != std::string::npos)
            return true;
    }
    return false;
}

inline int bind_i3c_controller()
{
    using namespace std::chrono_literals;

    if (module_loaded("mipi-i3c-hci"))
    {
        std::cerr << "bind_i3c_controller: mipi-i3c-hci module already "
                     "loaded\n";
        return EXIT_SUCCESS;
    }

    // Wait for the i2c mux driver first, so loading I3C cannot perturb i2c bus
    // enumeration.
    constexpr int mux_timeout_ms = 20000;
    for (int elapsed_ms = 0;; elapsed_ms += 100)
    {
        if (i2c_mux_present())
        {
            // The mux interfaces can be visible before the kernel has finished
            // adding all of them.
            std::this_thread::sleep_for(300ms);
            break;
        }
        if (elapsed_ms >= mux_timeout_ms)
        {
            std::cerr << "bind_i3c_controller: timed out waiting for the i2c "
                         "mux driver to complete initialization, loading the "
                         "i3c drivers anyway\n";
            break;
        }
        if (elapsed_ms % 1000 == 0)
        {
            std::cerr << std::format("bind_i3c_controller: waiting for the i2c "
                                     "mux driver, {} ms passed\n",
                                     elapsed_ms);
        }
        std::this_thread::sleep_for(100ms);
    }

    // Load order matters: the controller drivers must precede the HCI and the
    // character/MCTP devices.
    static constexpr std::array<std::string_view, 6> i3c_modules{
        "i3c",          "dw_i3c_master", "ast2600_i3c_master",
        "mipi-i3c-hci", "i3cdev",        "mctp-i3c",
    };
    for (const auto module : i3c_modules)
    {
        if (modprobe_with_timeout(module, std::chrono::seconds{20}, true) != 0)
        {
            std::cerr << std::format("bind_i3c_controller: failed to load {} "
                                     "module\n",
                                     module);
            return EXIT_FAILURE;
        }
    }

    std::cerr << "bind_i3c_controller: I3C driver modules loaded\n";
    return EXIT_SUCCESS;
}

// ── Filesystem health check ───────────────────────────────────────────────
// Mirrors filesystem_check.sh.
// Verifies the read-write overlay is mounted. On failure, logs a Redfish
// event via D-Bus and triggers a forced reboot after recording a recovery
// counter in U-Boot env so the recovery can be logged on the next boot.
// Also detects ROFS (no space on rwfs) and logs an error.
// rwfs_mount / work_path: OpenBMC overlay paths. Override for platforms that
//                         use a non-standard rootfs overlay location.

inline int filesystem_check(
    std::string_view rwfs_mount = "/run/initramfs/rw",
    std::string_view work_path = "/run/initramfs/rw/work")
{
    using namespace std::chrono_literals;

    // The /proc/mounts scan and the U-Boot env accessors this used to hold as
    // private lambdas are now detail::is_mounted() and
    // uboot_env_get() / uboot_env_set(), shared with filesystem_check_mtd()
    // and the platform inits. Same implementations, same call sites.

    // Log a Redfish event via D-Bus
    auto log_redfish = [](const std::string& message_id,
                          const std::string& severity,
                          const std::string& args) {
        try
        {
            auto bus = sdbusplus::bus::new_default();
            auto m = bus.new_method_call(
                "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
                "xyz.openbmc_project.Logging.Create", "Create");
            m.append(message_id, severity,
                     std::vector<std::pair<std::string, std::string>>{
                         {"REDFISH_MESSAGE_ID", message_id},
                         {"REDFISH_MESSAGE_ARGS", args},
                     });
            bus.call(m);
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format("filesystem_check: D-Bus log failed: {}\n",
                                     e.what());
        }
    };

    static constexpr auto level_info =
        "xyz.openbmc_project.Logging.Entry.Level.Informational";
    static constexpr auto level_err =
        "xyz.openbmc_project.Logging.Entry.Level.Error";
    static constexpr auto msg_corrected =
        "ResourceEvent.1.0.ResourceErrorsCorrected";
    static constexpr auto msg_detected =
        "ResourceEvent.1.0.ResourceErrorsDetected";
    static constexpr auto msg_reboot = "OpenBMC.0.4.BMCRebootReason";

    // If a previous recovery succeeded, log the correction and clear the flag.
    // The flag is raw flash-backed text: anything that is not a whole
    // non-negative number is no counter at all, so restart the budget rather
    // than throw out of the boot path.
    const auto prev = uboot_env_get("rwfs_recovery");
    int prev_count = 0;
    if (!prev.empty())
    {
        const char* begin = prev.data();
        const char* end = begin + prev.size();
        const auto [ptr, ec] = std::from_chars(begin, end, prev_count);
        if (ec != std::errc{} || ptr != end || prev_count < 0)
        {
            std::cerr << std::format(
                "filesystem_check: bad rwfs_recovery value '{}'\n", prev);
            prev_count = 0;
        }
    }
    if (prev_count > 0 && detail::is_mounted(rwfs_mount))
    {
        std::this_thread::sleep_for(10s); // wait for logging service
        log_redfish(msg_corrected, level_info, "BMC EMMC,Data Corruption");
        uboot_env_set("rwfs_recovery", "0");
    }

    // Check RWFS mount
    if (!detail::is_mounted(rwfs_mount))
    {
        std::cerr << std::format("filesystem_check: RWFS not mounted at {}\n",
                                 rwfs_mount);
        log_redfish(msg_detected, level_err, "BMC EMMC,Data Corruption");

        constexpr int max_retries = 3;
        if (prev_count >= max_retries)
        {
            std::cerr << std::format(
                "filesystem_check: failed after {} attempts; "
                "manual intervention required\n",
                max_retries);
            return EXIT_FAILURE;
        }

        const int next = prev_count + 1;
        const auto reason = std::format(
            "Reboot to recover RWFS (attempt {}/{})", next, max_retries);
        log_redfish(msg_reboot, level_info, reason);
        uboot_env_set("rwfs_recovery", std::to_string(next));

        std::this_thread::sleep_for(5s);
        ::sync();
        ::reboot(RB_AUTOBOOT);
        return EXIT_FAILURE; // unreachable
    }

    // Check for ROFS (no space — cannot create work dir)
    const std::filesystem::path work{work_path};
    if (std::filesystem::is_directory(work))
    {
        std::error_code ec;
        std::filesystem::create_directory(work / "work", ec);
        if (ec)
        {
            std::cerr << "filesystem_check: no space on RWFS — read-only mode; "
                         "factory reset required\n";
            return EXIT_FAILURE;
        }
    }

    std::cerr << "filesystem_check: RWFS OK\n";
    return EXIT_SUCCESS;
}

// ── Filesystem health check, MTD variant ──────────────────────────────────
// Source: meta-prime filesystem_check.sh — check_fs_recovery,
//         confirm_mountpoint, check_rw_filesystems, check_rofs
//
// The MTD sibling of filesystem_check() above, for platforms whose RWFS is a
// JFFS2 volume on a raw MTD partition rather than an eMMC hardware partition.
// A separate function, not a flag, because the two differ in every respect:
//   * recovers by erasing the MTD partition, not by re-formatting eMMC;
//   * carries recovery state in `dataflashrecovery`, not `rwfs_recovery`;
//   * has NO attempt cap — a corrupt RWFS is a board fault here, and the
//     erase-and-reboot loop is the only recovery path;
//   * reports its faults on the console alone, not as a Redfish
//     ResourceErrorsDetected. Every path here runs before sd_notify(READY=1)
//     and so before phosphor-log-manager, and the Logging bus name is D-Bus
//     activatable, so a Create call would pull that service up out of order
//     and block on bus.call() for the activation. Only the
//     recovery-succeeded entry logs, after waiting for the service, because
//     `dataflashrecovery` must not be cleared unless it was recorded.
inline int filesystem_check_mtd(
    std::string_view rwfs_mount = "/run/initramfs/rw",
    std::string_view work_path = "/run/initramfs/rw/work",
    std::string_view mtd_device = "/dev/mtd6")
{
    using namespace std::chrono_literals;

    // check_fs_recovery: a `yes` means the previous boot erased the partition
    // and rebooted. The mount check is a deliberate divergence — the shell
    // reports on the flag alone, so a failed erase claims a false recovery.
    // Clear the flag only once the log call succeeded, or it goes unreported.
    if (uboot_env_get("dataflashrecovery") == "yes" &&
        detail::is_mounted(rwfs_mount))
    {
        std::this_thread::sleep_for(10s); // wait for log service startup
        std::cerr << "filesystem_check_mtd: creating Redfish log for flash "
                     "auto recovery\n";
        if (detail::log_redfish_status(
                "ResourceEvent.1.0.ResourceErrorsCorrected",
                "Management Controller Flash,Data Corruption",
                log_level_informational))
        {
            uboot_env_set("dataflashrecovery");
        }
        else
        {
            std::cerr << "filesystem_check_mtd: failed to create Redfish log "
                         "for auto recovery\n";
        }
    }

    // confirm_mountpoint + check_rw_filesystems
    if (!detail::is_mounted(rwfs_mount))
    {
        std::cerr << std::format("[ERROR] Mount failure at {}\n", rwfs_mount);

        std::cerr << std::format("filesystem_check_mtd: erasing {} partition "
                                 "on detection of corruption\n",
                                 mtd_device);
        const auto cmd = std::format("flash_eraseall -q {}", mtd_device);
        if (const int rc = ::system(cmd.c_str());
            rc != 0) // NOLINT(cert-env33-c)
        {
            std::cerr << std::format(
                "filesystem_check_mtd: '{}' failed (status {})\n", cmd, rc);
        }
        uboot_env_set("dataflashrecovery", "yes");

        // The U-Boot env write goes to flash in the background; give it time
        // to land, or the reboot loses the recovery flag.
        std::this_thread::sleep_for(5s);
        ::sync();
        ::reboot(RB_AUTOBOOT);
        return EXIT_FAILURE; // unreachable
    }

    // check_rofs
    const std::filesystem::path work{work_path};
    if (!std::filesystem::is_directory(work))
    {
        std::cerr << std::format("filesystem_check_mtd: could not find work "
                                 "directory, {}\n",
                                 work_path);
        return EXIT_FAILURE;
    }
    if (!std::filesystem::is_directory(work / "work"))
    {
        std::error_code ec;
        std::filesystem::create_directory(work / "work", ec);
        if (ec)
        {
            std::cerr
                << "[ERROR] No space left on rwfs. Management Controller "
                   "booted in read only mode. Perform Factory reset to "
                   "recover read write functionality. Please recover before "
                   "flashing Management Controller.\n";
            return EXIT_FAILURE;
        }
    }

    std::cerr << "filesystem_check_mtd: RWFS OK\n";
    return EXIT_SUCCESS;
}

// ── D-Bus service availability wait ──────────────────────────────────────
// Polls org.freedesktop.DBus.GetNameOwner until the named service acquires
// a bus name or the timeout elapses. Used by post-ready init steps that
// depend on services starting after sd_notify(READY=1).
inline bool dbus_wait_for_service(
    const char* name, std::chrono::seconds timeout = std::chrono::seconds{60})
{
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        try
        {
            auto bus = sdbusplus::bus::new_default();
            auto m = bus.new_method_call(
                "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "GetNameOwner");
            m.append(name);
            bus.call(m);
            return true;
        }
        catch (...)
        {
            std::this_thread::sleep_for(1s);
        }
    }
    std::cerr << std::format("dbus_wait_for_service: {} not ready after {}s\n",
                             name, timeout.count());
    return false;
}

// ── BMC reset reason check ────────────────────────────────────────────────
// Source: bmc_reset_check.sh / bmc-reset-check.service.
// Waits for xyz.openbmc_project.Logging, reads pstore/journal/fw_printenv to
// determine the reset cause, and logs BMCRebootReason or BMCKernelPanic.
// Non-fatal: called post-ready; failures are logged to stderr only.
inline void bmc_reset_check(std::chrono::seconds timeout = std::chrono::seconds{
                                60})
{
    using namespace std::chrono_literals;

    std::cerr << "bmc_reset_check: waiting for Logging service\n";
    if (!dbus_wait_for_service("xyz.openbmc_project.Logging", timeout))
        return;

    // ── Check kernel WDT timeout via pstore ───────────────────────────────
    bool kernel_wdt = false;
    {
        std::ifstream f{"/var/lib/systemd/pstore/dmesg-ramoops-0"};
        for (std::string line; std::getline(f, line);)
            if (line.find("watchdog pretimeout event") != std::string::npos)
            {
                kernel_wdt = true;
                break;
            }
    }

    // ── Check kernel panic in boot journal ────────────────────────────────
    bool kernel_panic = false;
    if (!kernel_wdt)
    {
        FILE* p = ::popen("journalctl --no-pager -b 2>/dev/null"
                          " | grep -q 'PStore dmesg-ramoops' && echo y",
                          "r");
        if (p)
        {
            char buf[4]{};
            kernel_panic = ::fgets(buf, sizeof(buf), p) && buf[0] == 'y';
            ::pclose(p);
        }
    }

    // ── Read U-Boot reset_reason env var ─────────────────────────────────
    // fw_printenv -n returns just the value (e.g. "Power_On", "WDT2_SOC").
    // Replace '_' with ' ' to match original script: sed 's/_/ /g'.
    std::string wdt, reset_mode, full_reason;
    if (!kernel_wdt && !kernel_panic)
    {
        FILE* p = ::popen("fw_printenv -n reset_reason 2>/dev/null", "r");
        if (p)
        {
            char buf[256]{};
            if (::fgets(buf, sizeof(buf), p))
            {
                std::string raw{buf};
                while (!raw.empty() &&
                       (raw.back() == '\n' || raw.back() == '\r'))
                    raw.pop_back();
                for (auto& c : raw)
                    if (c == '_')
                        c = ' ';
                const auto sp = raw.find(' ');
                wdt = sp != std::string::npos ? raw.substr(0, sp) : raw;
                reset_mode = sp != std::string::npos ? raw.substr(sp + 1) : "";
                full_reason = raw;
            }
            ::pclose(p);
        }
    }

    // ── Log event via xyz.openbmc_project.Logging.Create ─────────────────
    static constexpr auto sev_info =
        "xyz.openbmc_project.Logging.Entry.Level.Informational";
    using KV = std::pair<std::string, std::string>;

    auto log_event = [&](const char* short_id, std::string_view args = {}) {
        try
        {
            const auto full_id = std::format("OpenBMC.0.4.{}", short_id);
            const auto redfish_id = std::format("OpenBMC.0.5.{}", short_id);
            std::vector<KV> data{
                {"REDFISH_MESSAGE_ID", redfish_id},
                {"xyz.openbmc_project.Logging.Entry.Resolution", "None"},
            };
            if (!args.empty())
                data.emplace_back("REDFISH_MESSAGE_ARGS", std::string{args});
            auto bus = sdbusplus::bus::new_default();
            auto m = bus.new_method_call(
                "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
                "xyz.openbmc_project.Logging.Create", "Create");
            m.append(full_id, std::string{sev_info}, data);
            bus.call(m);
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format("bmc_reset_check: log failed: {}\n",
                                     e.what());
        }
    };

    if (kernel_wdt)
    {
        std::cerr << "bmc_reset_check: kernel WDT timeout\n";
        log_event("BMCRebootReason", "BMC kernel watchdog timeout occurred");
    }
    else if (kernel_panic)
    {
        std::cerr << "bmc_reset_check: kernel panic\n";
        log_event("BMCKernelPanic");
    }
    else if (full_reason == "Power On")
    {
        std::cerr << "bmc_reset_check: power-on reset\n";
        log_event("BMCRebootReason", "BMC power-on reset");
    }
    else if (wdt == "WDT1")
    {
        std::cerr << "bmc_reset_check: normal reset (WDT1)\n";
        log_event("BMCRebootReason", "BMC normal reset");
    }
    else if (!wdt.empty())
    {
        const auto msg =
            std::format("BMC reset due to {} {} Reset", wdt, reset_mode);
        std::cerr << std::format("bmc_reset_check: {}\n", msg);
        log_event("BMCRebootReason", msg);
    }
    else
    {
        std::cerr << "bmc_reset_check: unknown reset reason\n";
        log_event("BMCRebootReason", "BMC reset due to Unknown");
    }
}

} // namespace nvidia
