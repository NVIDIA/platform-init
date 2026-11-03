// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

// Platform init for the hgxr HMC.

#include "nvidia_platform_utils.hpp"

#include <fcntl.h>
#include <linux/rtc.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <iostream>

namespace nvidia
{

// timedated refuses to set the clock while time sync is enabled.
static bool time_sync_enabled()
{
    for (const auto* dir : {"/etc/systemd/system", "/usr/lib/systemd/system"})
        if (std::filesystem::exists(std::format(
                "{}/sysinit.target.wants/systemd-timesyncd.service", dir)))
            return true;
    return false;
}

// As rtc_setup.sh did: an RTC that cannot be read gets 2020-01-01 00:00 UTC,
// which the system clock takes too unless time sync owns it; then the RTC
// takes the system clock.
static void setup_rtc()
{
    const int fd = open("/dev/rtc0", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    rtc_time tm{};
    if (ioctl(fd, RTC_RD_TIME, &tm) == 0)
    {
        close(fd);
        return;
    }
    std::cerr << std::format("hgxr-hmc: RTC unreadable: {}\n",
                             std::strerror(errno));

    if (!time_sync_enabled())
    {
        const timespec epoch{.tv_sec = 1577836800, .tv_nsec = 0};
        if (clock_settime(CLOCK_REALTIME, &epoch) != 0)
            std::cerr << std::format("hgxr-hmc: cannot set the clock: {}\n",
                                     std::strerror(errno));
    }

    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    tm = rtc_time{.tm_sec = utc.tm_sec,
                  .tm_min = utc.tm_min,
                  .tm_hour = utc.tm_hour,
                  .tm_mday = utc.tm_mday,
                  .tm_mon = utc.tm_mon,
                  .tm_year = utc.tm_year,
                  .tm_wday = utc.tm_wday,
                  .tm_yday = utc.tm_yday,
                  .tm_isdst = 0};
    if (ioctl(fd, RTC_SET_TIME, &tm) != 0)
        std::cerr << std::format("hgxr-hmc: cannot set the RTC: {}\n",
                                 std::strerror(errno));
    close(fd);
}

int init_hgxr_hmc()
{
    std::cerr << "hgxr-hmc: platform init start\n";
    setup_rtc();
    std::cerr << "hgxr-hmc: platform init complete\n";
    return notify_ready_and_wait();
}

} // namespace nvidia
