// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA
//
// nvidia-emmc-monitor — Type=notify systemd service binary.
//   1. startup phase (mount, verify rwfs, audit layout, etc.)
//   2. register com.Nvidia.EmmcManager D-Bus service
//   3. sd_notify(READY=1)
//   4. install 120 s periodic timer
//   5. sd_event_loop

#include "emmc-config.h"
#include "emmc-dbus.h"
#include "emmc-monitor.h"
#include "emmc-redfish-log.h"
#include "emmc-uenv.h"

#include <systemd/sd-daemon.h>
#include <systemd/sd-event.h>

#include <sdbusplus/bus.hpp>

#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>

int main(int /*argc*/, char** /*argv*/)
{
    int rc = emmc::monitor::startup();
    if (rc == emmc::monitor::kStartupEmmcMissing)
    {
        sd_notify(0, "READY=1");
        return EXIT_FAILURE;
    }

    // One-time probe — operations in the loop re-open the env per call.
    if (!emmc::uenv::isAvailable(emmc::kRrEnvConfig))
    {
        std::cerr << std::format(
            "nvidia-emmc-monitor: phram-env ({}) is not available; reboot "
            "budget tracking will degrade gracefully\n",
            emmc::kRrEnvConfig);
    }

    sd_event* event = nullptr;
    if (sd_event_default(&event) < 0)
    {
        std::cerr << "nvidia-emmc-monitor: sd_event_default failed\n";
        return EXIT_FAILURE;
    }

    auto bus = sdbusplus::bus::new_default();
    bus.attach_event(event, SD_EVENT_PRIORITY_NORMAL);

    auto svc = std::make_unique<emmc::dbus::EmmcDbusService>(
        bus, emmc::dbus::kObjectPath);
    bus.request_name(emmc::dbus::kServiceName);

    emmc::monitor::MonitorContext ctx{event, &bus};
    if (emmc::monitor::installMonitorTimer(ctx) < 0)
    {
        std::cerr << "nvidia-emmc-monitor: failed to install monitor timer\n";
        sd_event_unref(event);
        return EXIT_FAILURE;
    }

    sd_notify(0, "READY=1");

    int loop_rc = sd_event_loop(event);
    if (loop_rc < 0)
    {
        std::cerr << std::format("nvidia-emmc-monitor: sd_event_loop rc={}\n",
                                 loop_rc);
    }
    sd_event_unref(event);
    return loop_rc < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
