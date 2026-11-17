// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-util.h"

#include <systemd/sd-bus.h>

#include <format>
#include <iostream>

namespace emmc::util
{

bool rebootViaSystemd()
{
    sd_bus* bus = nullptr;
    if (sd_bus_default_system(&bus) < 0 || bus == nullptr)
    {
        std::cerr << "emmc-util: sd_bus_default_system failed\n";
        return false;
    }
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* m = nullptr;
    int r = sd_bus_call_method(
        bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager", "Reboot", &err, &m, "");
    if (r < 0)
    {
        std::cerr << std::format("emmc-util: Reboot call failed: {}\n",
                                 err.message ? err.message : "(unknown)");
        sd_bus_error_free(&err);
        sd_bus_unref(bus);
        return false;
    }
    sd_bus_message_unref(m);
    sd_bus_unref(bus);
    return true;
}

} // namespace emmc::util
