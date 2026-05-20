// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-config.h"
#include "emmc-format.h"
#include "emmc-monitor.h"
#include "emmc-redfish-log.h"
#include "emmc-uenv.h"
#include "emmc-util.h"

#include <sys/statvfs.h>
#include <systemd/sd-event.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace emmc::monitor
{

namespace
{

// "/var/emmc/fdr" -> "var_emmc_fdr"
std::string sanitise(std::string_view mp)
{
    std::string out;
    out.reserve(mp.size());
    for (char c : mp)
    {
        out.push_back(c == '/' ? '_' : c);
    }
    if (!out.empty() && out.front() == '_')
    {
        out.erase(out.begin());
    }
    return out;
}

std::string counterPath(std::string_view mp, std::string_view kind)
{
    std::error_code ec;
    std::filesystem::create_directories(kMonitorStateDir, ec);
    return std::format("{}/{}_{}", kMonitorStateDir, sanitise(mp), kind);
}

int readCounter(std::string_view path)
{
    std::ifstream ifs{std::string(path)};
    if (!ifs)
    {
        return 0;
    }
    int v = 0;
    ifs >> v;
    return v;
}

void writeCounter(std::string_view path, int value)
{
    std::string p(path);
    std::ofstream ofs(p, std::ios::trunc);
    if (!ofs)
    {
        std::cerr << std::format("monitor: write counter {} failed\n", p);
        return;
    }
    ofs << value;
}

void resetCounter(std::string_view path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

bool capacityOver99(std::string_view mp)
{
    std::string p(mp);
    struct statvfs st{};
    if (::statvfs(p.c_str(), &st) != 0 || st.f_blocks == 0)
    {
        return false;
    }
    // f_bavail excludes the ext4 reserved-for-root pool (5% by default),
    // matching df's Use%. f_bfree would mask a user-visible "100%" full fs.
    unsigned long long used = st.f_blocks - st.f_bavail;
    return (used * 100ULL) >= (st.f_blocks * 99ULL);
}

// Write-then-read probe with cleanup. False on any I/O error.
bool rwHealthCheck(std::string_view mp)
{
    const std::string probe =
        std::format("{}/.nvidia-emmc-monitor-health", mp);
    std::error_code ec;
    {
        std::ofstream ofs(probe);
        if (!ofs)
        {
            return false;
        }
        ofs << "ok";
    }
    bool ok = false;
    {
        std::ifstream ifs(probe);
        std::string s;
        if (ifs && (ifs >> s) && s == "ok")
        {
            ok = true;
        }
    }
    std::filesystem::remove(probe, ec);
    return ok && !ec;
}

// 3-strike debouncer. Returns true iff healthy *this tick*.
bool tickStrike(std::string_view mp, std::string_view kind, bool failing,
                std::string_view redfish_id, std::string_view reason)
{
    std::string cf = counterPath(mp, kind);
    if (!failing)
    {
        resetCounter(cf);
        return true;
    }
    int n = readCounter(cf) + 1;
    writeCounter(cf, n);
    if (n >= kStrikeThreshold)
    {
        if (redfish_id == "ResourceWarningThresholdExceeded")
        {
            redfish::emitResourceWarningThresholdExceeded(reason);
        }
        else
        {
            redfish::emitResourceErrorsDetected(reason);
        }
        triggerAutoRecovery(mp, reason);
        resetCounter(cf);
        return false;
    }
    return false;
}

} // namespace

bool checkOneMountPoint(std::string_view mount_point)
{
    bool unmounted = !format::isMounted(mount_point);
    bool capacity_full = !unmounted && capacityOver99(mount_point);
    bool rw_fail = !unmounted && !capacity_full && !rwHealthCheck(mount_point);

    bool ok1 = tickStrike(mount_point, "unmounted", unmounted,
                          "ResourceErrorsDetected",
                          std::format("BMC EMMC,{} unmounted", mount_point));
    bool ok2 = tickStrike(
        mount_point, "capacity", capacity_full,
        "ResourceWarningThresholdExceeded",
        std::format("BMC EMMC,{} capacity > 99%", mount_point));
    bool ok3 = tickStrike(
        mount_point, "health_fail", rw_fail, "ResourceErrorsDetected",
        std::format("BMC EMMC,{} R/W health failed", mount_point));

    return ok1 && ok2 && ok3;
}

bool triggerAutoRecovery(std::string_view mount_path, std::string_view reason)
{
    using namespace std::chrono_literals;

    int attempts = uenv::getCounter("emmc_reboot_recovery", kRrEnvConfig);
    if (attempts >= kMaxRebootRecoveryAttempts)
    {
        std::cerr << std::format(
            "monitor: emmc_reboot_recovery budget exhausted ({} >= {}); "
            "suppressing reboot for {} ({})\n",
            attempts, kMaxRebootRecoveryAttempts, mount_path, reason);
        return false;
    }
    if (uenv::getCounter("rwfs_recovery") != 0)
    {
        std::cerr << "monitor: RWFS recovery already in flight; suppressing "
                     "duplicate reboot\n";
        return false;
    }

    if (!uenv::setCounter("emmc_reboot_recovery", attempts + 1, kRrEnvConfig))
    {
        std::cerr << std::format(
            "monitor: setCounter(emmc_reboot_recovery, {}) on phram-env "
            "FAILED — reboot budget tracking may be incorrect this cycle\n",
            attempts + 1);
    }
    redfish::emitBmcRebootReason(std::format(
        "Auto-recovery reboot for {} ({}) — attempt {}/{}", mount_path,
        reason, attempts + 1, kMaxRebootRecoveryAttempts));

    bool ok_flag = false;
    if (mount_path == kRwfsMount)
    {
        ok_flag = uenv::set("rwfs_recovery", "1");
        if (!ok_flag)
        {
            std::cerr << "monitor: set(rwfs_recovery, 1) on fw_env FAILED — "
                         "next boot will not see the recovery flag\n";
        }
    }
    else
    {
        ok_flag = uenv::set("emmc_secure_erase_partition", mount_path);
        if (!ok_flag)
        {
            std::cerr << std::format(
                "monitor: set(emmc_secure_erase_partition, {}) on fw_env "
                "FAILED — next boot will not reformat the failed partition\n",
                mount_path);
        }
    }
    std::this_thread::sleep_for(5s);
    return emmc::util::rebootViaSystemd();
}

void monitorTick()
{
    bool all_healthy = true;
    for (auto const& p : emmc::layout::kPartitions)
    {
        if (p.mount == nullptr)
        {
            continue;
        }
        if (!checkOneMountPoint(p.mount))
        {
            all_healthy = false;
        }
    }
    if (!checkOneMountPoint(kRwfsMount))
    {
        all_healthy = false;
    }
    else if (uenv::getCounter("rwfs_recovery") > 0)
    {
        uenv::clear("rwfs_recovery");
        redfish::emitResourceErrorsCorrected(
            "BMC EMMC,RWFS recovery succeeded");
    }
    if (all_healthy
        && uenv::getCounter("emmc_reboot_recovery", kRrEnvConfig) > 0)
    {
        uenv::clear("emmc_reboot_recovery", kRrEnvConfig);
        redfish::emitResourceErrorsCorrected(
            "BMC EMMC,All partitions healthy");
    }
}

namespace
{

int onTimer(sd_event_source* source, uint64_t /*usec*/, void* /*userdata*/)
{
    monitorTick();
    uint64_t next = 0;
    if (sd_event_now(sd_event_source_get_event(source), CLOCK_MONOTONIC,
                     &next) >= 0)
    {
        next += static_cast<uint64_t>(kMonitorTickSeconds) * 1000000ULL;
        sd_event_source_set_time(source, next);
        sd_event_source_set_enabled(source, SD_EVENT_ON);
    }
    return 0;
}

} // namespace

int installMonitorTimer(MonitorContext& ctx)
{
    sd_event_source* src = nullptr;
    uint64_t now = 0;
    if (sd_event_now(ctx.event, CLOCK_MONOTONIC, &now) < 0)
    {
        return -1;
    }
    uint64_t when =
        now + static_cast<uint64_t>(kMonitorTickSeconds) * 1000000ULL;
    int rc = sd_event_add_time(ctx.event, &src, CLOCK_MONOTONIC, when,
                               1000000ULL, onTimer, nullptr);
    if (rc < 0)
    {
        return rc;
    }
    sd_event_source_set_enabled(src, SD_EVENT_ON);
    return 0;
}

} // namespace emmc::monitor
