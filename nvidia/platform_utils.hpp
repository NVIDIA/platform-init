// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Shared utilities called from inside platform init functions.
// These are not subcommands — they are steps within platform init.

#pragma once

#include "gpio.hpp"
#include "i2c.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <linux/fs.h>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <thread>
#include <unistd.h>
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
    std::string_view type;        // e.g. "24c128"
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
                                 dev.description, dev.bus, dev.address, dev.type);
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
    int         expected;
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
                std::cerr << std::format("gpio_check: FAIL {} expected={} actual={}\n",
                                         g.name, g.expected, actual);
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
// writable upper layer) to prevent a tampered upperdir from masking a bad EEPROM.
// Non-fatal: logs pass/fail to stderr and to Redfish via phosphor-logging;
// init continues regardless.

inline void eeprom_crc_verify(
    unsigned bus, unsigned addr, std::string_view type,
    std::string_view golden_rel_path =
        "/usr/share/eeprom-crc-verifier/golden_eeprom.bin")
{
    using namespace std::chrono_literals;
    const auto tag = std::format("eeprom_crc_verify: bus={} addr={:#x}", bus, addr);

    // ── Step 1: Ensure EEPROM driver is bound ─────────────────────────────
    const auto dev_sysfs = std::format("/sys/bus/i2c/devices/{}-{:04x}", bus, addr);
    if (!std::filesystem::exists(dev_sysfs))
    {
        std::cerr << std::format("{}: binding driver {}\n", tag, type);
        i2c::new_device(bus, addr, type);
        std::this_thread::sleep_for(100ms); // wait for sysfs entry
    }
    const auto eeprom_path = dev_sysfs + "/eeprom";
    if (!std::filesystem::exists(eeprom_path))
    {
        std::cerr << std::format("{}: eeprom sysfs not found at {}\n",
                                 tag, eeprom_path);
        return;
    }

    // ── Step 2: Resolve golden sample path via overlay lowerdir ──────────
    // Reading from the lowerdir directly (not through the overlay merge) prevents
    // a write to the upperdir from shadowing the golden and producing a false pass.
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
            std::cerr << std::format("{}: golden sample not found at {}\n",
                                     tag, golden_path);
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
            std::cerr << std::format("{}: failed to read {}\n", tag, eeprom_path);
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
        std::cerr << std::format("{}: FAIL — content does NOT match golden\n", tag);
        log_redfish("ResourceEvent.1.3.ResourceErrorsDetected", args,
                    "xyz.openbmc_project.Logging.Entry.Level.Warning");
    }
    // Non-fatal: result is informational only
}

// Overload: golden data supplied by the caller as a constexpr byte array.
// Skips file I/O and overlay lowerdir resolution entirely.
// Use this when the golden bytes are embedded in the platform .cpp file.
inline void eeprom_crc_verify(
    unsigned bus, unsigned addr, std::string_view type,
    std::span<const uint8_t> golden_bytes)
{
    using namespace std::chrono_literals;
    const auto tag = std::format("eeprom_crc_verify: bus={} addr={:#x}", bus, addr);

    // ── Step 1: Ensure EEPROM driver is bound ─────────────────────────────
    const auto dev_sysfs  = std::format("/sys/bus/i2c/devices/{}-{:04x}", bus, addr);
    const auto eeprom_path = dev_sysfs + "/eeprom";
    if (!std::filesystem::exists(dev_sysfs))
    {
        std::cerr << std::format("{}: binding driver {}\n", tag, type);
        i2c::new_device(bus, addr, type);
        std::this_thread::sleep_for(100ms);
    }
    if (!std::filesystem::exists(eeprom_path))
    {
        std::cerr << std::format("{}: eeprom sysfs not found at {}\n",
                                 tag, eeprom_path);
        return;
    }

    // ── Step 2: Read EEPROM content ───────────────────────────────────────
    std::vector<uint8_t> actual(golden_bytes.size());
    {
        std::ifstream f{eeprom_path, std::ios::binary};
        if (!f || !f.read(reinterpret_cast<char*>(actual.data()),
                          static_cast<std::streamsize>(golden_bytes.size())))
        {
            std::cerr << std::format("{}: failed to read {}\n", tag, eeprom_path);
            return;
        }
    }

    // ── Step 3: Compare ───────────────────────────────────────────────────
    const auto golden_vec = std::vector<uint8_t>(golden_bytes.begin(),
                                                  golden_bytes.end());
    const auto args = std::format("EEPROM bus={} addr={:#x}", bus, addr);
    if (actual == golden_vec)
    {
        std::cerr << std::format("{}: PASS — content matches golden\n", tag);
        log_redfish("ResourceEvent.1.3.ResourceStatusChangedOK", args);
    }
    else
    {
        std::cerr << std::format("{}: FAIL — content does NOT match golden\n", tag);
        log_redfish("ResourceEvent.1.3.ResourceErrorsDetected", args,
                    "xyz.openbmc_project.Logging.Entry.Level.Warning");
    }
}

// ── NVMe CPLD MUX probe ───────────────────────────────────────────────────
// Mirrors nvme_cpld_probe.sh + nvme_lib.sh.
// The NVMe CPLD MUX (pca954x) is powered by RUN_POWER; if the BMC boots while
// the host is off, the kernel's initial probe fails and the MUX is bound without
// creating channel-0. Unbind + rebind forces a fresh probe.
// If channel-0 already exists the MUX is healthy — leaves it alone.
// Non-fatal; call for each (bus, addr) pair that has an E1.S MUX.
inline void nvme_cpld_probe(unsigned bus, unsigned addr = 0x77)
{
    const auto key      = std::format("{}-{:04x}", bus, addr);
    const auto dev      = "/sys/bus/i2c/devices/" + key;
    const auto drv      = "/sys/bus/i2c/drivers/pca954x/" + key;
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
                std::cerr << std::format("nvme_cpld_probe: {}: unbind failed\n", key);
                do_bind = false;
            }
        }
    }

    if (do_bind && !std::filesystem::exists(channel0))
    {
        if (std::ofstream f{"/sys/bus/i2c/drivers/pca954x/bind"}; f)
        {
            f << key;
            std::cerr << std::format("nvme_cpld_probe: {}: bound to pca954x\n", key);
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
// buses: platform-specific list (e.g. {5, 19} for nvl/c2/e5010, {6, 19} for quark)

struct RtcVariant
{
    uint8_t          device_id;
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
        const auto sysfs_dev = std::format("/sys/bus/i2c/devices/{}-{:04x}", bus, addr);
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
            uint8_t id = 0;
            if (dev.read_byte(0x10, id) != 0)
                continue;
            for (const auto& rtc : rtc_variants)
            {
                if (id != rtc.device_id)
                    continue;
                std::cerr << std::format(
                    "rtc_detect: bus={} addr={:#x}: found {} (id={:#x})\n",
                    bus, addr, rtc.driver, id);
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
    unsigned int     eid;
    std::string_view description;
};

inline int hw_checkout(std::span<const ErotEndpoint> endpoints,
                       int retries = 5,
                       std::chrono::seconds retry_delay = std::chrono::seconds{3})
{
    if (endpoints.empty())
    {
        std::cerr << "hw_checkout: no ERoT endpoints configured\n";
        return EXIT_FAILURE;
    }

    bool all_pass = true;
    for (const auto& ep : endpoints)
    {
        std::cerr << std::format("hw_checkout: checking EID={} ({})\n",
                                 ep.eid, ep.description);
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
            std::cerr << std::format("hw_checkout: FAIL EID={} rc={}\n",
                                     ep.eid, rc);
            all_pass = false;
        }
        else
        {
            std::cerr << std::format("hw_checkout: PASS EID={}\n", ep.eid);
        }
    }

    std::cerr << (all_pass ? "hw_checkout: all PASS\n" : "hw_checkout: some FAIL\n");
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
    const auto path = std::format("/sys/bus/i2c/drivers/pca953x/{}-{:04x}", bus, addr);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (std::filesystem::exists(path))
            return true;
        std::this_thread::sleep_for(1s);
    }
    std::cerr << std::format("iox_wait: {}-{:04x} not present after {}s\n",
                             bus, addr, timeout.count());
    return false;
}

// ── 2b. I2C mux binding ───────────────────────────────────────────────────
// Source: bind_i2c_mux_driver() in bmc_ready.sh
// Binds a PCA954x I2C mux. Used on C2 after B0_M1_I2C_SOCAMM_UPHY_SEL-O is set.
inline void i2c_mux_bind(unsigned bus, unsigned addr)
{
    const auto path = std::format("/sys/bus/i2c/drivers/pca954x/{}-{:04x}", bus, addr);
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
                           const char* sel_gpio   = "BMC_HMC_MUX_SEL-O")
{
    const int present = gpio::get(prsnt_gpio);
    if (present == 1)
    {
        std::cerr << std::format("hmc_mux_select: HMC present — disabling I2C_MGMT1 bypass\n");
        gpio::set(sel_gpio, 1);
    }
    else
    {
        std::cerr << std::format("hmc_mux_select: HMC absent — enabling I2C_MGMT1 bypass\n");
        gpio::set(sel_gpio, 0);
    }
}

// ── 2d. GPIO poll helper ──────────────────────────────────────────────────
// Source: wait_gpio_assert() in bmc_ready.sh
// Polls a GPIO until it goes HIGH. Returns true on success, false on timeout.
// Default: 100 retries × 100 ms = 10 s (matches script default for MCU/CPLD waits).
// For standby power waits use retries=200 (20 s), delay=100 ms.
inline bool wait_gpio_high(const char* name,
                           int retries = 100,
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
    Linux    = 0x83,
    Extended = 0x05,
};

struct EmmcPartSpec
{
    int          num;
    EmmcPartType type;
    uint64_t     size_bytes; // 0 = fill to end of disk (Extended only)
};

//   p1   64 MiB  Config data   (primary)
//   p2    2 GiB  FDR           (primary)
//   p3    1 GiB  User log      (primary)
//   p4    rest   Extended      (container)
//   p5    2 GiB  IST           (logical)
//   p6    1 MiB  U-boot env    (logical)
//   p7  1.9 GiB  Miscellaneous (logical)
static constexpr std::array<EmmcPartSpec, 7> vr_bmc_emmc_layout{{
    {1, EmmcPartType::Linux,      64ULL << 20},  //   64 MiB
    {2, EmmcPartType::Linux,    2048ULL << 20},  //    2 GiB
    {3, EmmcPartType::Linux,    1024ULL << 20},  //    1 GiB
    {4, EmmcPartType::Extended,            0},   //  fills rest
    {5, EmmcPartType::Linux,    2048ULL << 20},  //    2 GiB
    {6, EmmcPartType::Linux,       1ULL << 20},  //    1 MiB
    {7, EmmcPartType::Linux,    1900ULL << 20},  //  1.9 GiB
}};

// ── eMMC partition creation ───────────────────────────────────────────────
// Replaces create-partition.sh.
// Writes an MBR partition table directly via ioctl; no external tools.
// Idempotent: skips if p1 already exists in sysfs.
// layout: partition specs (default: vr_bmc_emmc_layout). Platforms with a different
//         layout pass their own constexpr array via std::span.

inline int emmc_partition(std::string_view device,
                          std::span<const EmmcPartSpec> layout = vr_bmc_emmc_layout)
{
    // Idempotent guard
    const auto sysfs_p1 = std::format("/sys/block/{0}/{0}p1/size", device);
    if (std::filesystem::exists(sysfs_p1))
    {
        std::cerr << std::format("emmc_partition: {} already partitioned\n", device);
        return EXIT_SUCCESS;
    }

    const auto dev_path = std::format("/dev/{}", device);
    std::cerr << std::format("emmc_partition: partitioning {}\n", dev_path);

    const int fd = ::open(dev_path.c_str(), O_RDWR | O_SYNC);
    if (fd < 0)
    {
        std::cerr << std::format("emmc_partition: open {}: {}\n",
                                 dev_path, std::strerror(errno));
        return EXIT_FAILURE;
    }
    struct Guard { int fd; ~Guard() { ::close(fd); } } guard{fd};

    uint64_t total_bytes = 0;
    if (::ioctl(fd, BLKGETSIZE64, &total_bytes) < 0)
    {
        std::cerr << std::format("emmc_partition: BLKGETSIZE64: {}\n",
                                 std::strerror(errno));
        return EXIT_FAILURE;
    }

    constexpr uint32_t SECTOR = 512;
    constexpr uint32_t ALIGN  = 2048; // 1 MiB boundary in sectors (fdisk default)
    const uint64_t total_sects = total_bytes / SECTOR;

    auto align_up = [](uint64_t lba) -> uint64_t {
        return ((lba + ALIGN - 1) / ALIGN) * ALIGN;
    };

    // CHS encoding using fdisk's LBA geometry (255 heads × 63 spt).
    // CHS is not used for LBA-mode access but must be set for tool compatibility.
    constexpr uint32_t HEADS = 255;
    constexpr uint32_t SPT   = 63;

    auto encode_chs = [&](uint8_t* out, uint64_t abs_lba) {
        uint32_t c = static_cast<uint32_t>(abs_lba / (HEADS * SPT));
        uint32_t h = static_cast<uint32_t>((abs_lba % (HEADS * SPT)) / SPT);
        uint32_t s = static_cast<uint32_t>((abs_lba % SPT) + 1);
        if (c > 1023) { c = 1023; h = 254; s = 63; } // cap at max CHS
        out[0] = static_cast<uint8_t>(h);
        out[1] = static_cast<uint8_t>((s & 0x3F) | ((c >> 2) & 0xC0));
        out[2] = static_cast<uint8_t>(c & 0xFF);
    };

    // Write a 16-byte MBR/EBR partition entry.
    // abs_lba: absolute disk LBA (used only for CHS encoding)
    // rel_lba: LBA value written to the entry (may be relative to a base)
    auto fill_entry = [&](uint8_t* e, uint8_t type,
                          uint64_t abs_lba, uint64_t rel_lba, uint64_t size) {
        e[0] = 0x00; // not bootable
        encode_chs(e + 1, abs_lba);
        e[4] = type;
        encode_chs(e + 5, abs_lba + (size > 0 ? size - 1 : 0));
        const auto r = static_cast<uint32_t>(rel_lba);
        const auto n = static_cast<uint32_t>(size);
        e[8]  = r & 0xFF;        e[9]  = (r >> 8) & 0xFF;
        e[10] = (r >> 16) & 0xFF; e[11] = (r >> 24) & 0xFF;
        e[12] = n & 0xFF;        e[13] = (n >> 8) & 0xFF;
        e[14] = (n >> 16) & 0xFF; e[15] = (n >> 24) & 0xFF;
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
    const uint64_t p5_ebr   = p4_start;
    const uint64_t p5_start = p5_ebr + ALIGN;

    const uint64_t p6_ebr   = align_up(p5_start + p5_sects);
    const uint64_t p6_start = p6_ebr + ALIGN;

    const uint64_t p7_ebr   = align_up(p6_start + p6_sects);
    const uint64_t p7_start = p7_ebr + ALIGN;

    // ── MBR: sector 0 ────────────────────────────────────────────────────
    std::array<uint8_t, 512> mbr{};
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    // Primary entries at offset 446; for primary partitions abs == rel
    fill_entry(mbr.data() + 446,      0x83, p1_start, p1_start, p1_sects);
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
    // EBR entry[1]: next EBR           (lba_start relative to p4_start), 0 if last
    //               lba_size           = ALIGN + next_partition_size

    auto write_ebr = [&](uint64_t ebr_lba, uint64_t data_sects,
                         uint64_t next_ebr_lba, uint64_t next_sects) -> bool {
        std::array<uint8_t, 512> ebr{};
        ebr[510] = 0x55;
        ebr[511] = 0xAA;

        // Entry 0: current logical (CHS=absolute, LBA=relative to EBR)
        fill_entry(ebr.data() + 446, 0x83,
                   ebr_lba + ALIGN,  // abs_lba for CHS
                   ALIGN,            // rel_lba (offset from EBR)
                   data_sects);

        // Entry 1: next EBR (CHS=absolute, LBA=relative to extended start)
        if (next_ebr_lba != 0)
        {
            fill_entry(ebr.data() + 462, 0x05,
                       next_ebr_lba,              // abs_lba for CHS
                       next_ebr_lba - p4_start,   // rel_lba (offset from p4)
                       ALIGN + next_sects);        // slot = EBR gap + data
        }

        const auto offset = static_cast<off_t>(ebr_lba) * SECTOR;
        return ::pwrite(fd, ebr.data(), 512, offset) == 512;
    };

    if (!write_ebr(p5_ebr, p5_sects, p6_ebr, p6_sects) ||
        !write_ebr(p6_ebr, p6_sects, p7_ebr, p7_sects) ||
        !write_ebr(p7_ebr, p7_sects, 0,      0))
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

    std::cerr << std::format("emmc_partition: {} partitioned successfully\n", device);
    return EXIT_SUCCESS;
}

// ── Partition layout check ────────────────────────────────────────────────
// Replaces repartition_check.sh.
// Validates sysfs sector counts against the provided layout.
// layout: partition specs (default: vr_bmc_emmc_layout). Must match what was written
//         by emmc_partition().

inline int partition_check(std::string_view device,
                           std::span<const EmmcPartSpec> layout = vr_bmc_emmc_layout)
{
    constexpr int  tolerance_pct = 2;
    constexpr long sector_bytes  = 512;

    int missing = 0;
    int failed  = 0;

    // Count non-extended entries (those we can validate via sysfs)
    int total = 0;
    for (const auto& spec : layout)
        if (spec.type != EmmcPartType::Extended)
            ++total;

    for (const auto& spec : layout)
    {
        if (spec.type == EmmcPartType::Extended)
            continue; // variable size; nothing to compare

        const long expected_sects = static_cast<long>(spec.size_bytes / sector_bytes);
        const auto sysfs = std::format("/sys/block/{0}/{0}p{1}/size",
                                       device, spec.num);

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

        const long diff     = std::abs(actual_sects - expected_sects);
        const long diff_pct = expected_sects > 0
                            ? diff * 100L / expected_sects : 100L;

        if (diff_pct <= tolerance_pct)
            std::cerr << std::format("partition_check: {}p{} OK ({}%)\n",
                                     device, spec.num, diff_pct);
        else
        {
            std::cerr << std::format(
                "partition_check: {}p{} MISMATCH "
                "(actual={} expected=~{} diff={}%)\n",
                device, spec.num, actual_sects, expected_sects, diff_pct);
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
    int         part_num;
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
        const auto mpt  = std::string{m.mount_point};

        // 1. Create mount point directory
        std::error_code ec;
        std::filesystem::create_directories(mpt, ec);
        if (ec)
        {
            std::cerr << std::format("emmc_mount: mkdir {} failed: {}\n",
                                     mpt, ec.message());
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
            std::cerr << std::format("emmc_mount: {} mounted at {}\n", part, mpt);
            continue;
        }

        const int err = errno;
        // EINVAL = no valid superblock; ENOENT = partition node not yet in /dev
        if (err != EINVAL && err != ENOENT)
        {
            std::cerr << std::format("emmc_mount: {} mount failed: {}\n",
                                     part, std::strerror(err));
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
            std::cerr << std::format("emmc_mount: {} post-format mount failed: {}\n",
                                     part, std::strerror(errno));
    }
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
    std::string_view work_path  = "/run/initramfs/rw/work")
{
    using namespace std::chrono_literals;

    // Check /proc/mounts for the RWFS overlay mount
    auto is_mounted = [](std::string_view path) -> bool {
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
    };

    // Shell-out to fw_printenv / fw_setenv: they access U-Boot env in flash
    // at offsets from /etc/fw_env.config. There is no stable C API for this
    // without adding libubootenv as a new dependency.
    auto fw_getenv = [](std::string_view key) -> std::string {
        const auto cmd = std::format("fw_printenv -n {} 2>/dev/null", key);
        auto* pipe = ::popen(cmd.data(), "r");
        if (!pipe) return {};
        char buf[256]{};
        std::string result;
        while (::fgets(buf, sizeof(buf), pipe))
            result += buf;
        ::pclose(pipe);
        while (!result.empty() && result.back() == '\n')
            result.pop_back();
        return result;
    };

    auto fw_setenv = [](std::string_view key, std::string_view value = {}) {
        const auto cmd = value.empty()
            ? std::format("fw_setenv {} 2>/dev/null", key)
            : std::format("fw_setenv {} {} 2>/dev/null", key, value);
        ::system(cmd.c_str()); // NOLINT(cert-env33-c)
    };

    // Log a Redfish event via D-Bus
    auto log_redfish = [](const std::string& message_id,
                          const std::string& severity,
                          const std::string& args) {
        try
        {
            auto bus = sdbusplus::bus::new_default();
            auto m = bus.new_method_call(
                "xyz.openbmc_project.Logging",
                "/xyz/openbmc_project/logging",
                "xyz.openbmc_project.Logging.Create", "Create");
            m.append(message_id, severity,
                     std::vector<std::pair<std::string, std::string>>{
                         {"REDFISH_MESSAGE_ID",   message_id},
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
    static constexpr auto level_err  =
        "xyz.openbmc_project.Logging.Entry.Level.Error";
    static constexpr auto msg_corrected =
        "ResourceEvent.1.0.ResourceErrorsCorrected";
    static constexpr auto msg_detected  =
        "ResourceEvent.1.0.ResourceErrorsDetected";
    static constexpr auto msg_reboot    = "OpenBMC.0.4.BMCRebootReason";

    // If a previous recovery succeeded, log the correction and clear the flag
    const auto prev = fw_getenv("rwfs_recovery");
    const int  prev_count = prev.empty() ? 0 : std::stoi(prev);
    if (prev_count > 0 && is_mounted(rwfs_mount))
    {
        std::this_thread::sleep_for(10s); // wait for logging service
        log_redfish(msg_corrected, level_info, "BMC EMMC,Data Corruption");
        fw_setenv("rwfs_recovery", "0");
    }

    // Check RWFS mount
    if (!is_mounted(rwfs_mount))
    {
        std::cerr << std::format(
            "filesystem_check: RWFS not mounted at {}\n", rwfs_mount);
        log_redfish(msg_detected, level_err, "BMC EMMC,Data Corruption");

        constexpr int max_retries = 3;
        if (prev_count >= max_retries)
        {
            std::cerr << std::format(
                "filesystem_check: failed after {} attempts; "
                "manual intervention required\n", max_retries);
            return EXIT_FAILURE;
        }

        const int next = prev_count + 1;
        const auto reason = std::format(
            "Reboot to recover RWFS (attempt {}/{})", next, max_retries);
        log_redfish(msg_reboot, level_info, reason);
        fw_setenv("rwfs_recovery", std::to_string(next));

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

// ── D-Bus service availability wait ──────────────────────────────────────
// Polls org.freedesktop.DBus.GetNameOwner until the named service acquires
// a bus name or the timeout elapses. Used by post-ready init steps that
// depend on services starting after sd_notify(READY=1).
inline bool dbus_wait_for_service(const char* name,
                                   std::chrono::seconds timeout = std::chrono::seconds{60})
{
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        try
        {
            auto bus = sdbusplus::bus::new_default();
            auto m   = bus.new_method_call("org.freedesktop.DBus",
                                            "/org/freedesktop/DBus",
                                            "org.freedesktop.DBus", "GetNameOwner");
            m.append(name);
            bus.call(m);
            return true;
        }
        catch (...) { std::this_thread::sleep_for(1s); }
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
inline void bmc_reset_check(std::chrono::seconds timeout = std::chrono::seconds{60})
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
                { kernel_wdt = true; break; }
    }

    // ── Check kernel panic in boot journal ────────────────────────────────
    bool kernel_panic = false;
    if (!kernel_wdt)
    {
        FILE* p = ::popen(
            "journalctl --no-pager -b 2>/dev/null"
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
                while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r'))
                    raw.pop_back();
                for (auto& c : raw) if (c == '_') c = ' ';
                const auto sp = raw.find(' ');
                wdt         = sp != std::string::npos ? raw.substr(0, sp) : raw;
                reset_mode  = sp != std::string::npos ? raw.substr(sp + 1) : "";
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
            const auto full_id    = std::format("OpenBMC.0.4.{}", short_id);
            const auto redfish_id = std::format("OpenBMC.0.5.{}", short_id);
            std::vector<KV> data{
                {"REDFISH_MESSAGE_ID", redfish_id},
                {"xyz.openbmc_project.Logging.Entry.Resolution", "None"},
            };
            if (!args.empty())
                data.emplace_back("REDFISH_MESSAGE_ARGS", std::string{args});
            auto bus = sdbusplus::bus::new_default();
            auto m   = bus.new_method_call(
                "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
                "xyz.openbmc_project.Logging.Create", "Create");
            m.append(full_id, std::string{sev_info}, data);
            bus.call(m);
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format("bmc_reset_check: log failed: {}\n", e.what());
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
        const auto msg = std::format("BMC reset due to {} {} Reset", wdt, reset_mode);
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
