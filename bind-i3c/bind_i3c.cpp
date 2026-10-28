// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

// Standalone entry point for the deferred I3C controller bind on
// spc6-ast2600-bmc. The I3C pull-ups are dead until the CPLD has powered the
// ASICs, so platform init cannot load the drivers while the host CPU is down;
// hw-management-events.sh starts this from the CPU_RESET_DEASSERT udev event.

#include "switch_common.hpp"

int main()
{
    return nvidia::bind_i3c_controller();
}
