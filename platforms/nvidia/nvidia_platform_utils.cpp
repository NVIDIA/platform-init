// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

#include "nvidia_platform_utils.hpp"

#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/rtc.h>

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

bool iox_wait(unsigned bus, unsigned addr, std::chrono::seconds timeout)
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

int notify_ready_and_wait()
{
    gpio::release_all();
    sd_notify(0, "READY=1");
    pause();
    return EXIT_SUCCESS;
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

    // A non-numeric flag restarts the budget rather than throwing.
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
        std::cerr << std::format("filesystem_check: RWFS recovered after {} "
                                 "reboot(s)\n",
                                 prev_count);
        uboot_env_set("rwfs_recovery", "0");
    }

    if (!detail::is_mounted(rwfs_mount))
    {
        std::cerr << std::format("filesystem_check: RWFS not mounted at {}\n",
                                 rwfs_mount);

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
        std::cerr << std::format(
            "filesystem_check: reboot to recover RWFS (attempt {}/{})\n", next,
            max_retries);
        uboot_env_set("rwfs_recovery", std::to_string(next));

        std::this_thread::sleep_for(5s);
        ::sync();
        ::reboot(RB_AUTOBOOT);
        return EXIT_FAILURE; // unreachable
    }

    // ROFS: no space to create the work dir.
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
                         std::string_view mtd_device, bool clear_recovery_flag)
{
    using namespace std::chrono_literals;

    // A daemon that may log reports and clears the flag where there is one
    // (hmc-mailbox on the VR HMCs); elsewhere it is cleared here.
    if (uboot_env_get("dataflashrecovery") == "yes" &&
        detail::is_mounted(rwfs_mount))
    {
        std::cerr << "filesystem_check_mtd: RWFS recovered after a flash "
                     "erase\n";
        if (clear_recovery_flag)
        {
            uboot_env_set("dataflashrecovery");
        }
    }

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

        // Let the U-Boot env write reach flash before rebooting.
        std::this_thread::sleep_for(5s);
        ::sync();
        ::reboot(RB_AUTOBOOT);
        return EXIT_FAILURE; // unreachable
    }

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

bool check_rofs()
{
    const std::filesystem::path work_dir{"/run/initramfs/rw/work"};
    if (!std::filesystem::is_directory(work_dir))
    {
        std::cerr << "Could not find work directory, /run/initramfs/rw/work\n";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directory(work_dir / "work", ec);
    if (ec)
    {
        std::cerr << "[ERROR] No space left on rwfs. Management Controller "
                     "booted in read only mode. Perform Factory reset to "
                     "recover read write functionality.\n";
        return false;
    }
    return true;
}

bool check_rw_filesystems()
{
    if (detail::is_mounted("/run/initramfs/rw"))
        return true;
    std::cerr << "[ERROR] Mount failure at /run/initramfs/rw\n";
    return false;
}

bool set_system_clock(std::time_t epoch)
{
    const timespec ts{.tv_sec = epoch, .tv_nsec = 0};
    if (clock_settime(CLOCK_REALTIME, &ts) == 0)
        return true;
    std::cerr << std::format("cannot set the clock to {}: {}\n", epoch,
                             std::strerror(errno));
    return false;
}

int rtc_write_system_time(int fd)
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    const rtc_time tm{
        .tm_sec = utc.tm_sec,
        .tm_min = utc.tm_min,
        .tm_hour = utc.tm_hour,
        .tm_mday = utc.tm_mday,
        .tm_mon = utc.tm_mon,
        .tm_year = utc.tm_year,
        .tm_wday = utc.tm_wday,
        .tm_yday = utc.tm_yday,
        .tm_isdst = 0};
    return ioctl(fd, RTC_SET_TIME, &tm) == 0 ? 0 : errno;
}

namespace
{

// i2ctransfer -y -f <bus> w0@<addr>, then w1@<addr> <reg> r1.
int forced_read(unsigned bus, uint8_t addr, uint8_t reg)
{
    int fd = open(std::format("/dev/i2c-{}", bus).c_str(), O_RDWR);
    i2c_msg msg{.addr = addr, .flags = 0, .len = 0, .buf = nullptr};
    i2c_rdwr_ioctl_data data{.msgs = &msg, .nmsgs = 1};
    bool ack = fd >= 0 && ioctl(fd, I2C_RDWR, &data) >= 0;
    if (fd >= 0)
        close(fd);
    std::array<uint8_t, 1> id{};
    if (!ack || i2c::raw_transfer(bus, addr, std::array{reg}, id))
        return -1;
    return id[0];
}

int smbus_read(unsigned bus, uint8_t addr, uint8_t reg)
{
    // A DTS-bound driver makes I2C_SLAVE fail with EBUSY; skip the probe.
    if (std::filesystem::exists(
            std::format("/sys/bus/i2c/devices/{}-{:04x}/driver", bus, addr)))
    {
        std::cerr << std::format(
            "rtc_detect: bus={} addr={:#x}: already bound to kernel driver\n",
            bus, addr);
        return -2;
    }
    try
    {
        i2c::RawDevice dev{bus, addr};
        auto id = dev.read_byte(reg);
        return id ? *id : -1;
    }
    catch (const std::exception& e)
    {
        std::cerr << std::format("rtc_detect: bus={}: {}\n", bus, e.what());
    }
    return -1;
}

} // namespace

std::optional<RtcFound> detect_rtc(std::span<const unsigned> buses,
                                   std::span<const RtcId> table, bool forced)
{
    for (unsigned bus : buses)
    {
        int id = forced ? -1 : smbus_read(bus, table[0].addr, table[0].reg);
        if (id == -2)
            return std::nullopt;
        for (size_t i = 0; i < table.size(); i++)
        {
            if (forced)
                id = forced_read(bus, table[i].addr, table[i].reg);
            if (id >= 0 && id == table[i].id)
                return RtcFound{bus, i};
        }
    }
    if (!forced)
        std::cerr << "rtc_detect: no RTC found on any bus\n";
    return std::nullopt;
}

} // namespace nvidia

namespace nvidia::compute
{

void create_state_dir()
{
    std::error_code ec;
    std::filesystem::create_directories("/run/bmc-state", ec);
}

} // namespace nvidia::compute
