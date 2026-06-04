// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#pragma once

#include <com/nvidia/Common/EmmcFullSecureErase/server.hpp>
#include <com/nvidia/Common/EmmcSecureErase/server.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/object.hpp>

namespace emmc::dbus
{

inline constexpr const char* kServiceName = "com.Nvidia.EmmcManager";
inline constexpr const char* kObjectPath = "/xyz/openbmc_project/software";

// Single object at kObjectPath implementing both SecureErase variants —
// each Trigger sets a u-boot env flag consumed by nvidia-emmc-init on
// next boot.
class EmmcDbusService :
    public sdbusplus::server::object_t<
        sdbusplus::com::nvidia::Common::server::EmmcSecureErase,
        sdbusplus::com::nvidia::Common::server::EmmcFullSecureErase>
{
  public:
    EmmcDbusService(sdbusplus::bus_t& bus, const char* path);

    void emmcSecureErase() override;
    void emmcFullSecureErase() override;
};

} // namespace emmc::dbus
