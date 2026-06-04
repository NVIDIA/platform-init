// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-dbus.h"

#include <gtest/gtest.h>

// Smoke test: confirm we can construct EmmcDbusService against a private
// bus without throwing. Calling the actual D-Bus methods requires a live
// libubootenv environment — out of scope for unit tests.

TEST(EmmcDbus, ConstructionDoesNotThrow)
{
    auto bus = sdbusplus::bus::new_default();
    EXPECT_NO_THROW({
        emmc::dbus::EmmcDbusService svc(bus, "/xyz/openbmc_project/software");
    });
}

TEST(EmmcDbus, ServiceAndObjectConstantsMatchSpec)
{
    EXPECT_STREQ(emmc::dbus::kServiceName, "com.Nvidia.EmmcManager");
    EXPECT_STREQ(emmc::dbus::kObjectPath, "/xyz/openbmc_project/software");
}
