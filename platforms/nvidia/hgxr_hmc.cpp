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
        set_system_clock(1577836800);

    if (const int err = rtc_write_system_time(fd); err != 0)
        std::cerr << std::format("hgxr-hmc: cannot set the RTC: {}\n",
                                 std::strerror(err));
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
