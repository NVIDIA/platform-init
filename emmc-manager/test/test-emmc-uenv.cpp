// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-uenv.h"

#include <gtest/gtest.h>

// libubootenv requires /etc/fw_env.config + the underlying MTD device,
// which only exist on the target. The unit-test-side coverage here is
// limited to exercising the API surface against a missing config file
// (every call should fail cleanly without throwing).

TEST(EmmcUenv, MissingConfigFileReturnsCleanFalse)
{
    EXPECT_FALSE(emmc::uenv::isAvailable("/no-such-fw-env.config"));
    auto v = emmc::uenv::get("anything", "/no-such-fw-env.config");
    EXPECT_FALSE(v.has_value());
    EXPECT_FALSE(emmc::uenv::set("anything", "yes", "/no-such-fw-env.config"));
    EXPECT_EQ(emmc::uenv::getCounter("anything", "/no-such-fw-env.config"), 0);
    EXPECT_FALSE(emmc::uenv::clear("anything", "/no-such-fw-env.config"));
    EXPECT_FALSE(
        emmc::uenv::setCounter("anything", 42, "/no-such-fw-env.config"));
}

// Integration test: requires a writable libubootenv environment.
// Out of scope for unit tests.
