# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 NVIDIA

executable(
    'platform-init-i3c-bind',
    [
        'bind_i3c.cpp',
        '../platforms/nvidia/nvidia_platform_utils.cpp',
        '../platforms/nvidia/switch_common.cpp',
    ] + util_srcs,
    dependencies: [gpiodcxx_dep, libsystemd_dep, i2c_dep, sdbusplus],
    include_directories: ['..', '../platforms/nvidia'],
    install: true,
    install_dir: get_option('bindir'),
)

install_data(
    'platform-init-i3c-bind.service',
    install_dir: systemd_system_unit_dir,
)
