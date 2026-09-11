// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-config.h"
#include "emmc-format.h"
#include "emmc-init.h"
#include "emmc-ioctl.h"
#include "emmc-layout-ondisk.h"
#include "emmc-monitor.h"
#include "emmc-redfish-log.h"
#include "emmc-uenv.h"
#include "emmc-util.h"

#include "emmc-ext-csd.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
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

constexpr double kPartitionSizeTolerance =
    static_cast<double>(emmc::layout::kSizeTolerancePercent) / 100.0;

std::string partSysfsSizePath(int part_num)
{
    return std::format("/sys/block/mmcblk0/mmcblk0p{}/size", part_num);
}

unsigned long long readSysfsUll(std::string_view path)
{
    std::string p(path);
    std::ifstream ifs(p);
    if (!ifs)
    {
        return 0;
    }
    unsigned long long v = 0;
    ifs >> v;
    return v;
}

// Mount→validate→format mount cascade for a single non-rwfs partition.
void mountOrRecover(int part_num, std::string_view mount_point)
{
    std::error_code ec;
    std::filesystem::create_directories(mount_point, ec);

    if (format::isMounted(mount_point))
    {
        return;
    }

    const std::string part = emmc::util::partDevice(kEmmcDevice, part_num);
    if (format::mountFs(part, mount_point))
    {
        return;
    }
    if (format::validateExt4OrRecover(part) &&
        format::mountFs(part, mount_point))
    {
        return;
    }
    if (format::formatPartition(part) && format::mountFs(part, mount_point))
    {
        return;
    }
    std::cerr << std::format(
        "monitor: failed to mount {} at {} after format retry\n", part,
        mount_point);
}

// Same ratio check as runRepartitionCheck(); separated so the audit
// helper can log every partition without aborting on the first hit.
double sizeRatio(unsigned long long actual, unsigned long long expected)
{
    return std::abs(static_cast<double>(actual) -
                    static_cast<double>(expected)) /
           static_cast<double>(expected);
}

void scheduleRwfsRecoveryReboot(int counter)
{
    using namespace std::chrono_literals;
    std::cerr << std::format(
        "monitor: RWFS overlay missing — scheduling rwfs_recovery reboot "
        "(attempt {}/{})\n",
        counter + 1, kMaxRebootRecoveryAttempts);
    uenv::setCounter("rwfs_recovery", counter + 1);
    redfish::emitBmcRebootReason(
        std::format("RWFS overlay missing; reboot attempt {}/{}", counter + 1,
                    kMaxRebootRecoveryAttempts));
    std::this_thread::sleep_for(3s);
    emmc::util::rebootViaSystemd();
}

void scheduleRepartitionReboot(int attempts)
{
    using namespace std::chrono_literals;
    bool ok_inc =
        uenv::setCounter("emmc_reboot_recovery", attempts + 1, kRrEnvConfig);
    std::cerr << std::format(
        "monitor: setCounter(emmc_reboot_recovery, {}) on phram-env {}\n",
        attempts + 1, ok_inc ? "succeeded" : "FAILED");
    bool ok_set = uenv::set("emmc_repartition", "yes");
    std::cerr << std::format(
        "monitor: set(emmc_repartition, yes) on fw_env {}\n",
        ok_set ? "succeeded" : "FAILED");
    redfish::emitBmcRebootReason(
        std::format("Reboot and re-partition eMMC (attempt {}/{})",
                    attempts + 1, kMaxRebootRecoveryAttempts));
    std::cerr << "monitor: requesting reboot via systemd in 3s; on next boot "
                 "nvidia-emmc-init will secure-erase + repartition\n";
    std::this_thread::sleep_for(3s);
    bool ok_reboot = emmc::util::rebootViaSystemd();
    std::cerr << std::format(
        "monitor: emmc::util::rebootViaSystemd() returned {}\n",
        ok_reboot ? "true (Reboot accepted by systemd)"
                  : "FALSE (Reboot call rejected)");
}

// runRepartitionCheck = Mismatch: bump counter, set flag, reboot.
// Budget-exhausted branch logs and clears the flag without rebooting.
int handleLayoutMismatch()
{
    std::cerr << "monitor: runRepartitionCheck reports Mismatch — entering "
                 "repartition recovery flow\n";
    int attempts = uenv::getCounter("emmc_reboot_recovery", kRrEnvConfig);
    std::cerr << std::format(
        "monitor: reboot-recovery counter = {} (limit {})\n", attempts,
        kMaxRebootRecoveryAttempts);
    if (attempts >= kMaxRebootRecoveryAttempts)
    {
        std::cerr << std::format(
            "monitor: emmc_reboot_recovery budget exhausted ({} >= {}); "
            "layout still mismatched — emitting EMMC Corruption event, "
            "clearing emmc_repartition, and giving up (no reboot)\n",
            attempts, kMaxRebootRecoveryAttempts);
        redfish::emitResourceErrorsDetected("BMC EMMC,EMMC Corruption");
        uenv::clear("emmc_repartition");
        return kStartupRepartitionBudget;
    }
    scheduleRepartitionReboot(attempts);
    return kStartupOk;
}

// Clear repartition flag if the layout came back healthy on this boot.
void clearRepartitionFlagsIfHealthy()
{
    auto rp = uenv::get("emmc_repartition");
    if (!rp || rp->empty())
    {
        return;
    }
    std::cerr << "monitor: emmc_repartition was set but layout is now "
                 "healthy — clearing both counters\n";
    uenv::clear("emmc_repartition");
    uenv::clear("emmc_reboot_recovery", kRrEnvConfig);
    redfish::emitResourceErrorsCorrected("BMC EMMC,Re-partition succeeded");
}

// emmc_secure_erase_partition=<mount> consumer (set by monitorTick's
// 3-strike auto-recovery path on a per-partition failure).
void consumeSecureErasePartition()
{
    auto sep = uenv::get("emmc_secure_erase_partition");
    if (!sep || sep->empty())
    {
        return;
    }
    reformatSinglePartition(*sep);
    uenv::clear("emmc_secure_erase_partition");
}

} // namespace

void mountPartitions()
{
    for (const auto& p : emmc::layout::kPartitions)
    {
        if (p.mount == nullptr)
        {
            continue;
        }
        mountOrRecover(p.num, p.mount);
    }
}

bool verifyRwfsOverlay()
{
    if (format::isMounted(kRwfsMount))
    {
        int counter = uenv::getCounter("rwfs_recovery");
        if (counter > 0)
        {
            redfish::emitResourceErrorsCorrected(
                "BMC EMMC,RWFS recovery succeeded");
            uenv::clear("rwfs_recovery");
        }
        return true;
    }

    int counter = uenv::getCounter("rwfs_recovery");
    if (counter >= kMaxRebootRecoveryAttempts)
    {
        std::cerr << std::format(
            "monitor: rwfs_recovery budget exhausted ({} >= {}); "
            "RWFS overlay still missing — emitting RWFS recovery "
            "exhausted event and giving up (no reboot)\n",
            counter, kMaxRebootRecoveryAttempts);
        redfish::emitResourceErrorsDetected("BMC EMMC,RWFS recovery exhausted");
        return false;
    }
    scheduleRwfsRecoveryReboot(counter);
    return false;
}

bool checkPreEol(std::string_view device)
{
    std::array<uint8_t, 512> ext_csd{};
    if (!ioctl::readExtCsd(device, ext_csd))
    {
        return false;
    }
    if (ext_csd[emmc::extcsd::kPre_Eol_Info] >= emmc::extcsd::kPreEolWarning)
    {
        redfish::emitResourceWarningThresholdExceeded(
            "BMC EMMC,Consumed > 80% of reserved block");
    }
    return true;
}

void auditPartitionLayout()
{
    for (const auto& p : emmc::layout::kPartitions)
    {
        // Skip the Extended container (sectors == 0); audit every other
        // partition including p1 (rwfs) which has no mount but still has
        // an expected size on disk.
        if (p.type == emmc::layout::EmmcPart::Type::Extended)
        {
            continue;
        }
        unsigned long long actual = readSysfsUll(partSysfsSizePath(p.num));
        if (actual == 0)
        {
            std::cerr << std::format(
                "monitor: partition p{} not present in sysfs\n", p.num);
            continue;
        }
        if (sizeRatio(actual, p.sectors) > kPartitionSizeTolerance)
        {
            std::cerr << std::format(
                "monitor: partition p{} size mismatch — actual {} sectors, "
                "expected {} (ratio {:.4f})\n",
                p.num, actual, p.sectors, sizeRatio(actual, p.sectors));
        }
    }
}

// Delegates to the shared check so this daemon and nvidia-emmc-init can
// never disagree about whether the on-disk geometry is acceptable. When
// they did, init skipped the repartition and the monitor kept asking for
// one, and the board rebooted on every boot without ever converging.
RepartitionResult runRepartitionCheck()
{
    return emmc::layout::onDiskState(kEmmcDevice) ==
                   emmc::layout::OnDiskState::Match
               ? RepartitionResult::Healthy
               : RepartitionResult::Mismatch;
}

bool reformatSinglePartition(std::string_view path)
{
    int part_num = 0;
    for (const auto& m : emmc::layout::kPartitions)
    {
        if (m.mount != nullptr && path == m.mount)
        {
            part_num = m.num;
            break;
        }
    }
    if (part_num == 0)
    {
        std::cerr << std::format(
            "monitor: reformatSinglePartition({}) — unknown mount point\n",
            path);
        return false;
    }
    const std::string dev = emmc::util::partDevice(kEmmcDevice, part_num);
    if (format::isMounted(path))
    {
        format::unmount(path);
    }
    return format::formatPartition(dev) && format::mountFs(dev, path);
}

bool verifyEuda(std::string_view device)
{
    if (init::eudaConfig(device, /*recheck=*/true) != init::EudaResult::Ok)
    {
        std::cerr << std::format(
            "monitor: EUDA recheck on {} returned non-Ok — emitting "
            "ResourceErrorsDetected\n",
            device);
        redfish::emitResourceErrorsDetected(
            "BMC EMMC,EUDA configuration failed");
        return false;
    }
    return true;
}

int startup()
{
    if (!std::filesystem::exists(kEmmcDevice))
    {
        redfish::emitResourceErrorsDetected("BMC EMMC,EMMC Missing");
        return kStartupEmmcMissing;
    }

    mountPartitions();
    verifyRwfsOverlay();
    checkPreEol(kEmmcDevice);
    auditPartitionLayout();

    if (runRepartitionCheck() == RepartitionResult::Mismatch)
    {
        return handleLayoutMismatch();
    }

    std::cerr << "monitor: runRepartitionCheck reports Healthy\n";
    clearRepartitionFlagsIfHealthy();
    consumeSecureErasePartition();
    verifyEuda(kEmmcDevice);
    return kStartupOk;
}

} // namespace emmc::monitor
