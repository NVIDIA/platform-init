# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 NVIDIA
#
# emmc-manager — produces two C++ binaries:
#   - nvidia-emmc-init    (initramfs target, install_dir: get_option('sbindir'))
#   - nvidia-emmc-monitor (rofs target,      install_dir: get_option('bindir'))
# plus the systemd .service file for the monitor.
#
# See SADD §3.4.2 and the platform-init work order §13/14 for the layout
# this build file implements.

cpp = meson.get_compiler('cpp')

# Common dependencies used by both binaries.
ext2fs_dep = dependency('ext2fs', required: true)  # libext2fs
e2p_dep = dependency('e2p', required: true)  # libe2p
com_err_dep = cpp.find_library('com_err', required: true)  # libcom_err
libubootenv_dep = dependency('libubootenv', required: true)
libsystemd_dep = dependency('libsystemd', required: true)

# nvidia-emmc-init only — libgpiod (C API) for EMMC_RST_L-O toggle.
# Pinned to the v1.x C API surface (gpiod_line_find / gpiod_line_request_output).
# libgpiod v2.x is a breaking rewrite; we will not silently adopt it.
gpiod_dep = dependency('libgpiod', version: ['>=1.0', '<2.0'], required: true)

# nvidia-emmc-monitor only — sdbusplus + phosphor-dbus-interfaces +
# phosphor-logging.
sdbusplus_dep = dependency('sdbusplus', required: true)
phosphor_dbus_dep = dependency('phosphor-dbus-interfaces', required: true)
phosphor_logging_dep = dependency('phosphor-logging', required: true)

# The per-machine eMMC layout lives in include/emmc-layout-machine.h.
emmc_inc = include_directories('include')

emmc_shared = static_library(
    'emmc-shared',
    sources: files(
        'src/init/euda.cpp',
        'src/shared/emmc-format.cpp',
        'src/shared/emmc-ioctl.cpp',
        'src/shared/emmc-uenv.cpp',
        'src/shared/emmc-util.cpp',
    ),
    include_directories: emmc_inc,
    dependencies: [
        ext2fs_dep,
        e2p_dep,
        com_err_dep,
        libubootenv_dep,
        libsystemd_dep,
    ],
)

# nvidia-emmc-init: initramfs binary.
executable(
    'nvidia-emmc-init',
    sources: files(
        'src/init/emmc-init-flow.cpp',
        'src/init/euda-hwreset.cpp',
        'src/init/hwreset.cpp',
        'src/init/main.cpp',
        'src/init/partition.cpp',
        'src/init/secure-erase.cpp',
    ),
    include_directories: emmc_inc,
    link_with: emmc_shared,
    dependencies: [
        ext2fs_dep,
        e2p_dep,
        com_err_dep,
        libubootenv_dep,
        gpiod_dep,
    ],
    install: true,
    install_dir: get_option('sbindir'),
)

# nvidia-emmc-monitor: rofs systemd service binary.
executable(
    'nvidia-emmc-monitor',
    sources: files(
        'src/monitor/emmc-dbus.cpp',
        'src/monitor/emmc-monitor-loop.cpp',
        'src/monitor/emmc-redfish-log.cpp',
        'src/monitor/emmc-startup.cpp',
        'src/monitor/main.cpp',
    ),
    include_directories: emmc_inc,
    link_with: emmc_shared,
    dependencies: [
        ext2fs_dep,
        e2p_dep,
        com_err_dep,
        libubootenv_dep,
        libsystemd_dep,
        sdbusplus_dep,
        phosphor_dbus_dep,
        phosphor_logging_dep,
    ],
    install: true,
    install_dir: get_option('bindir'),
)

# Systemd unit for the monitor. systemd_system_unit_dir is set by the
# top-level meson.build and inherited here.
install_data(
    'service/nvidia-emmc-monitor.service',
    install_dir: systemd_system_unit_dir,
)

# Tests — gated by the existing `tests` option (added to meson.options
# alongside this subdir). Pattern matches the platform-init top-level
# build: tests are off by default and require gtest.
if get_option('tests').allowed()
    # When -Dtests=enabled is explicit, gtest must be available; missing
    # gtest with tests=enabled is a configuration error, not a silent skip.
    # With tests=auto (default), missing gtest skips the test block.
    tests_required = get_option('tests').enabled()
    gtest_main_dep = dependency('gtest_main', required: tests_required)
    gtest_dep = dependency('gtest', required: tests_required)
    if gtest_main_dep.found() and gtest_dep.found()
        test_cases = [
            ['test-emmc-init-flow', files('test/test-emmc-init-flow.cpp')],
            [
                'test-emmc-monitor-loop',
                files('test/test-emmc-monitor-loop.cpp'),
            ],
            ['test-emmc-dbus', files('test/test-emmc-dbus.cpp')],
            ['test-emmc-uenv', files('test/test-emmc-uenv.cpp')],
            [
                'test-emmc-secure-erase',
                files('test/test-emmc-secure-erase.cpp'),
            ],
        ]
        foreach t : test_cases
            t_name = t[0]
            t_srcs = t[1]
            extra_srcs = []
            extra_deps = [gtest_main_dep, gtest_dep]
            if t_name == 'test-emmc-init-flow'
                extra_srcs += files(
                    'src/init/emmc-init-flow.cpp',
                    'src/init/euda-hwreset.cpp',
                    'src/init/hwreset.cpp',
                    'src/init/partition.cpp',
                    'src/init/secure-erase.cpp',
                )
                extra_deps += [
                    ext2fs_dep,
                    e2p_dep,
                    com_err_dep,
                    libubootenv_dep,
                    gpiod_dep,
                ]
            elif t_name == 'test-emmc-monitor-loop'
                # Constants-only test — no extra .cpp deps needed.
            elif t_name == 'test-emmc-dbus'
                extra_srcs += files(
                    'src/monitor/emmc-dbus.cpp',
                )
                extra_deps += [
                    libubootenv_dep,
                    sdbusplus_dep,
                    phosphor_dbus_dep,
                    phosphor_logging_dep,
                ]
            elif t_name == 'test-emmc-uenv'
                extra_deps += [
                    ext2fs_dep,
                    e2p_dep,
                    com_err_dep,
                    libubootenv_dep,
                ]
            endif

            t_exe = executable(
                t_name,
                sources: t_srcs + extra_srcs,
                include_directories: emmc_inc,
                link_with: emmc_shared,
                dependencies: extra_deps,
            )
            test(t_name, t_exe)
        endforeach
    endif
endif
