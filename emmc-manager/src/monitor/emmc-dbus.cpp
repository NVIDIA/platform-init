// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-dbus.h"

#include "emmc-uenv.h"

#include <xyz/openbmc_project/Common/error.hpp>

#include <chrono>
#include <iostream>
#include <thread>

namespace emmc::dbus
{

namespace
{

// Delay after a successful env write so the caller (Redfish, busctl…)
// can't race a reboot against the libubootenv flush. Mirrors the
// phosphor-bmc-code-mgmt InventoryManager::emmcSecureErase pattern
// (commit 5e25199b).
constexpr auto kPostWriteSettle = std::chrono::seconds(3);

void triggerErase(const char* method, const char* uenv_key)
{
    std::cerr << "EmmcDbusService: " << method << " invoked — setting "
              << uenv_key << "=yes\n";
    if (!emmc::uenv::set(uenv_key, "yes"))
    {
        throw sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure();
    }
    std::this_thread::sleep_for(kPostWriteSettle);
    std::cerr << "EmmcDbusService: " << method
              << " will take effect upon reboot.\n";
}

} // namespace

EmmcDbusService::EmmcDbusService(sdbusplus::bus_t& bus, const char* path) :
    sdbusplus::server::object_t<
        sdbusplus::com::nvidia::Common::server::EmmcSecureErase,
        sdbusplus::com::nvidia::Common::server::EmmcFullSecureErase>(bus,
                                                                     path)
{}

void EmmcDbusService::emmcSecureErase()
{
    triggerErase("EmmcSecureErase", "emmc_secure_erase");
}

void EmmcDbusService::emmcFullSecureErase()
{
    triggerErase("EmmcFullSecureErase", "emmc_full_secure_erase");
}

} // namespace emmc::dbus
