# platform-init

Platform initialization utilities for OpenBMC-based systems.

## Overview

`platform-init` provides hardware-specific initialization for supported Intel and
NVIDIA platforms. The `platform` executable selects the initialization routine
for the configured platform and integrates with systemd.

The repository also includes platform-specific preparation, RTC detection,
I2C recovery and MCTP readiness helpers, plus eMMC initialization, monitoring,
and partition layout utilities.

## Repository layout

- [`platform.cpp`](platform.cpp): command-line entry point and platform selection.
- [`platforms/`](platforms/): platform implementations and deployment scripts.
- [`emmc-manager/`](emmc-manager/): eMMC initialization and monitoring utilities.
- [`emmc-partition/`](emmc-partition/): eMMC partition layout utilities.
- [`bind-i3c/`](bind-i3c/): platform-specific I3C binding helper.
- [`test/`](test/): platform unit tests; component directories contain additional tests.
- [`subprojects/`](subprojects/): Meson dependency wrap files.

## Building and testing

Build for the target Linux/OpenBMC environment with a C++23-capable toolchain,
Meson 1.7.0 or newer, and Ninja. Dependencies include libgpiod's C++ bindings,
systemd/libsystemd, sdbusplus, CLI11, and libi2c. See
[`meson.build`](meson.build) and the component build files for dependency details.

For a native build in an environment with the required dependencies:

```sh
meson setup build -Dplatform-name=nvidia-gb200
meson compile -C build
```

For cross-compilation, supply the target environment's Meson cross file.
Select the target with `platform-name`; the available build options are defined
in [`meson.options`](meson.options), and supported initialization names are
listed in [`platform.cpp`](platform.cpp).

To require and run unit tests, install GoogleTest and configure a separate build:

```sh
meson setup build-test -Dtests=enabled
meson compile -C build-test
meson test -C build-test --print-errorlogs
```

The default `tests=auto` setting may omit tests when GoogleTest is unavailable.

## Deployment and usage

The generated `platform_init.service` invokes
`/usr/libexec/platform init` with the configured platform name. See
[`platform_init.service.in`](platform_init.service.in) for service integration.
Additional helpers and services are selected by the component build files.

These utilities interact with platform hardware and system state. Deploy and
run them only on the intended target with the matching hardware configuration,
not on a development workstation.

## Development

The `main` branch contains a published snapshot of the project.
Ongoing development is available on the
[`develop` branch](https://github.com/NVIDIA/platform-init/tree/develop).
See [`OWNERS`](OWNERS) for the project's ownership information.

## Security

Follow [`SECURITY.md`](SECURITY.md) to report potential security vulnerabilities
through NVIDIA's private reporting channels. Do not report vulnerabilities in
public GitHub issues or pull requests.

## License

This project is licensed under the Apache License, Version 2.0.
See [`LICENSE.txt`](LICENSE.txt). The develop snapshot's [`LICENSE`](LICENSE)
contains the same license text.
