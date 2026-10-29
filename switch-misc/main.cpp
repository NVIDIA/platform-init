// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

// Switch BMC unit entry points that replace shell scripts one for one.

#include "switch_misc.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char** argv)
{
    const std::string_view mode = argc == 2 ? argv[1] : "";
    if (mode == "i2c-slave-setup")
        return nvidia::switch_misc::i2c_slave_setup();
    std::cerr << "usage: platform-init-switch-misc "
                 "i2c-slave-setup\n";
    return 2;
}
