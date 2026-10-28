// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

#include "nvidia_platform_utils.hpp"

namespace nvidia
{

void eeprom_init(std::span<const EepromDevice> devices)
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

bool iox_present(unsigned bus, unsigned addr)
{
    return std::filesystem::exists(
        std::format("/sys/bus/i2c/drivers/pca953x/{}-{:04x}", bus, addr));
}

void iox_bind(unsigned bus, unsigned addr)
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

namespace detail
{

int sysfs_write(const std::string& path, std::string_view data)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    const ssize_t written = ::write(fd, data.data(), data.size());
    const int err = written < 0 ? errno : 0;
    ::close(fd);
    return err;
}

std::optional<std::string> sysfs_read(const std::string& path)
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

bool is_mounted(std::string_view path)
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

} // namespace detail

constexpr std::string_view log_level_informational =
    "xyz.openbmc_project.Logging.Entry.Level.Informational";

namespace detail
{

// The Logging.Create call behind log_redfish_status(). Waits for the reply so
// the caller can report whether the entry was accepted; log_prefix names that
// caller in the failure message.
static bool log_create(std::string_view message_id, std::string_view severity,
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
static bool log_redfish_status(std::string_view message_id,
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

std::string uboot_env_get(std::string_view key)
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

void uboot_env_set(std::string_view key, std::string_view value)
{
    const auto cmd =
        value.empty() ? std::format("fw_setenv {} 2>/dev/null", key)
                      : std::format("fw_setenv {} {} 2>/dev/null", key, value);
    // A non-zero status means the value did not persist; log it.
    const int rc = ::system(cmd.c_str()); // NOLINT(cert-env33-c)
    if (rc != 0)
    {
        std::cerr << std::format("uboot_env_set: '{}' failed (status {})\n",
                                 cmd, rc);
    }
}

int filesystem_check(std::string_view rwfs_mount, std::string_view work_path)
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

int filesystem_check_mtd(std::string_view rwfs_mount,
                         std::string_view work_path,
                         std::string_view mtd_device)
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

} // namespace nvidia
