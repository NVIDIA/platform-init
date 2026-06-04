// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-redfish-log.h"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus.hpp>

#include <map>
#include <string>

namespace emmc::redfish
{

namespace
{

constexpr const char* kLoggingService = "xyz.openbmc_project.Logging";
constexpr const char* kLoggingPath = "/xyz/openbmc_project/logging";
constexpr const char* kLoggingInterface = "xyz.openbmc_project.Logging.Create";

const char* severityString(Severity s)
{
    switch (s)
    {
        case Severity::Warning:
            return "xyz.openbmc_project.Logging.Entry.Level.Warning";
        case Severity::Critical:
            return "xyz.openbmc_project.Logging.Entry.Level.Critical";
        case Severity::Informational:
        default:
            return "xyz.openbmc_project.Logging.Entry.Level.Informational";
    }
}

} // namespace

void emit(std::string_view message_id, Severity severity,
          std::string_view message_args)
{
    try
    {
        auto bus = sdbusplus::bus::new_default();
        auto m = bus.new_method_call(kLoggingService, kLoggingPath,
                                     kLoggingInterface, "Create");
        std::map<std::string, std::string> additional_data{
            {"REDFISH_MESSAGE_ID", std::string(message_id)},
            {"REDFISH_MESSAGE_ARGS", std::string(message_args)},
        };
        m.append(std::string(message_id), severityString(severity),
                 additional_data);
        bus.call(m);
    }
    catch (const std::exception& e)
    {
        // phosphor-logging unavailable → journald only, non-fatal.
        lg2::warning("redfish-log emit failed for {ID}: {ERR}", "ID",
                     std::string(message_id), "ERR", e.what());
    }
}

void emitResourceErrorsDetected(std::string_view args)
{
    emit("ResourceEvent.1.0.ResourceErrorsDetected", Severity::Critical, args);
}

void emitResourceWarningThresholdExceeded(std::string_view args)
{
    emit("ResourceEvent.1.0.ResourceWarningThresholdExceeded",
         Severity::Warning, args);
}

void emitResourceErrorsCorrected(std::string_view args)
{
    emit("ResourceEvent.1.0.ResourceErrorsCorrected",
         Severity::Informational, args);
}

void emitBmcRebootReason(std::string_view args)
{
    emit("OpenBMC.0.4.BMCRebootReason", Severity::Informational, args);
}

} // namespace emmc::redfish
