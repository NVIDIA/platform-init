/**
 * cpu-diag-status: CPU diagnostic heartbeat monitor
 *
 * Reads xyz.openbmc_project.Control.Diag::DiagStatus every 30 s and drives
 * the following state machine (matching the original shell implementation):
 *
 *   Idle (0)        – no heartbeat; increment counter; if counter >= 3 → Error
 *   InProgress (1)  – test running; acknowledge: reset counter, set → Idle
 *   Finished (2)    – test done; reset counter
 *   Error (3)       – set by this daemon when heartbeat is lost
 *   NotStarted (4)  – reset counter
 *
 * Unlike the shell version the counter is kept in memory (no tmp file).
 */

#include <systemd/sd-daemon.h>
#include <systemd/sd-event.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <variant>

namespace
{
constexpr auto busName = "xyz.openbmc_project.Settings";
constexpr auto objPath = "/xyz/openbmc_project/Control/Diag";
constexpr auto ifaceName = "xyz.openbmc_project.Control.Diag";
constexpr auto propName = "DiagStatus";
constexpr uint64_t intervalUs = 30ULL * 1'000'000; // 30 seconds

enum class DiagStatus : uint8_t
{
    Idle = 0,
    InProgress = 1,
    Finished = 2,
    Error = 3,
    NotStarted = 4,
};
} // namespace

struct DiagMonitor
{
    sdbusplus::bus_t bus{sdbusplus::bus::new_system()};
    int heartbeatCount{0};

    DiagStatus getStatus()
    {
        auto m = bus.new_method_call(busName, objPath,
                                     "org.freedesktop.DBus.Properties", "Get");
        m.append(ifaceName, propName);
        auto reply = bus.call(m);
        std::variant<uint8_t> val;
        reply.read(val);
        return static_cast<DiagStatus>(std::get<uint8_t>(val));
    }

    void setStatus(DiagStatus s)
    {
        auto m = bus.new_method_call(busName, objPath,
                                     "org.freedesktop.DBus.Properties", "Set");
        m.append(ifaceName, propName,
                 std::variant<uint8_t>(static_cast<uint8_t>(s)));
        bus.call_noreply(m);
    }

    void tick()
    {
        try
        {
            switch (getStatus())
            {
                case DiagStatus::Idle:
                    ++heartbeatCount;
                    if (heartbeatCount >= 3)
                    {
                        setStatus(DiagStatus::Error);
                        std::cerr
                            << "cpu-diag-status: no heartbeat after 3 checks"
                               " — setting Error\n";
                        heartbeatCount = 0;
                    }
                    break;

                case DiagStatus::InProgress:
                    setStatus(DiagStatus::Idle);
                    heartbeatCount = 0;
                    std::cout << "cpu-diag-status: test in progress\n";
                    break;

                case DiagStatus::Finished:
                    heartbeatCount = 0;
                    std::cout << "cpu-diag-status: test finished\n";
                    break;

                case DiagStatus::NotStarted:
                    heartbeatCount = 0;
                    std::cout << "cpu-diag-status: test not started\n";
                    break;

                default:
                    break;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "cpu-diag-status: " << e.what() << "\n";
        }
    }
};

static int onTimer(sd_event_source* src, uint64_t /*usec*/, void* data)
{
    auto* monitor = static_cast<DiagMonitor*>(data);
    monitor->tick();

    // Re-arm for the next interval
    uint64_t now{};
    sd_event_now(sd_event_source_get_event(src), CLOCK_MONOTONIC, &now);
    sd_event_source_set_time(src, now + intervalUs);
    return 0;
}

int main()
{
    DiagMonitor monitor;

    sd_event* event{};
    if (sd_event_default(&event) < 0)
    {
        std::cerr << "cpu-diag-status: failed to get sd_event\n";
        return 1;
    }

    monitor.bus.attach_event(event, SD_EVENT_PRIORITY_NORMAL);

    uint64_t now{};
    sd_event_now(event, CLOCK_MONOTONIC, &now);

    sd_event_source* src{};
    sd_event_add_time(event, &src, CLOCK_MONOTONIC, now + intervalUs, 0,
                      onTimer, &monitor);
    sd_event_source_set_enabled(src, SD_EVENT_ON);

    sd_notify(0, "READY=1");
    sd_event_loop(event);

    sd_event_source_unref(src);
    sd_event_unref(event);
    return 0;
}
