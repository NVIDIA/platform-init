// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#pragma once

#include "emmc-config.h"

#include <systemd/sd-event.h>

#include <sdbusplus/bus.hpp>

#include <string>
#include <string_view>

namespace emmc::monitor
{

inline constexpr int kStartupOk = 0;
inline constexpr int kStartupEmmcMissing = 1;
inline constexpr int kStartupRepartitionBudget = 8;

enum class RepartitionResult
{
    Healthy = 0,
    Mismatch = 1,
};

int startup();

// One 3-strike sweep across all mountpoints, scheduled by sd_event.
void monitorTick();

// Helpers (also exposed for unit tests).
bool checkOneMountPoint(std::string_view mount_point);
bool triggerAutoRecovery(std::string_view mount_path, std::string_view reason);
RepartitionResult runRepartitionCheck();
void mountPartitions();
bool verifyRwfsOverlay();
bool checkPreEol(std::string_view device);
void auditPartitionLayout();
bool reformatSinglePartition(std::string_view path);
bool verifyEuda(std::string_view device);

// Scheduled timer state used by the monitor binary's main().
// Note: pointers are non-owning; caller retains ownership of event loop and
// bus.
struct MonitorContext
{
    sd_event* event;
    sdbusplus::bus_t* bus;
};

int installMonitorTimer(MonitorContext& ctx);

} // namespace emmc::monitor
