#include "workflow.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cassert>
#include <chrono>
#include <thread>

namespace omron::workflow {

namespace {

constexpr int kConnectAttempts = 3;
constexpr auto kConnectRetryDelay = std::chrono::seconds(2);
constexpr auto kAdvertisementWait = std::chrono::seconds(15);
constexpr auto kPollInterval = std::chrono::milliseconds(200);

std::wstring Widen(const char* text) {
    return storage::FromUtf8(text);
}

// Returns once the monitor has been heard advertising, or throws NotFound after `timeout`.
void WaitForAdvertisement(uint64_t address, std::chrono::milliseconds timeout, const Log& log) {
    std::atomic<bool> heard{false};
    ble::Scanner scanner([&](const ble::FoundDevice& found) {
        if (found.address == address) heard = true;
    });
    scanner.Start();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const long long max_polls = timeout.count() / kPollInterval.count() + 1;
    for (long long i = 0; i < max_polls && !heard; ++i) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(kPollInterval);
    }
    scanner.Stop();
    if (!heard) {
        throw ble::NotFound("the monitor is not advertising - press its Bluetooth button so the symbol shows, then try again");
    }
    log(ble::Level::Debug, L"monitor is advertising");
}

void ReleaseOthers(const std::vector<uint64_t>& others, uint64_t keep, const Log& log) {
    assert(others.size() <= storage::kMaxDevices);
    for (uint64_t address : others) {
        if (address != 0 && address != keep) ble::ReleaseBond(address, log);
    }
}

std::vector<models::Reading> SplitRecords(const models::DeviceLayout& layout, size_t user, const std::vector<uint8_t>& region, const Log& log) {
    assert(region.size() % layout.record_size == 0);
    const size_t slots = region.size() / layout.record_size;
    assert(slots == static_cast<size_t>(layout.records_per_user[user]));
    std::vector<models::Reading> readings;
    readings.reserve(slots);
    for (size_t slot = 0; slot < slots; ++slot) {
        const uint8_t* raw = region.data() + slot * layout.record_size;
        try {
            const std::optional<models::Reading> r = models::ParseRecord(layout, raw, layout.record_size);
            if (r) readings.push_back(*r);
        } catch (const std::exception& exc) {
            log(ble::Level::Info, L"user " + std::to_wstring(user + 1) + L" slot " + std::to_wstring(slot) + L" skipped: " + Widen(exc.what()));
        }
    }
    std::sort(readings.begin(), readings.end(), [](const models::Reading& a, const models::Reading& b) { return a.when.ToEpoch() < b.when.ToEpoch(); });
    return readings;
}

// Reads the clock record, reports the drift and, when allowed and the layout is verified, writes PC time.
ClockStatus CheckClock(ble::Monitor& monitor, const models::DeviceLayout& layout, bool sync, const Log& log) {
    ClockStatus status;
    if (layout.clock == nullptr) return status;
    const models::ClockLayout& clock = *layout.clock;
    status.known = true;
    const std::vector<uint8_t> record = monitor.ReadEeprom(clock.read_address, clock.size, clock.size);
    status.verified = models::ClockChecksumOk(clock, record.data(), record.size());
    const std::optional<models::Timestamp> monitor_time = models::ParseClock(clock, record.data(), record.size());
    if (!monitor_time) {
        log(ble::Level::Info, L"monitor clock unreadable");
        return status;
    }
    status.readable = true;
    status.monitor_time = *monitor_time;
    const models::Timestamp now = storage::LocalNow();
    status.drift_seconds = static_cast<long>(monitor_time->ToEpoch() - now.ToEpoch());
    log(ble::Level::Info, L"monitor clock " + storage::FromUtf8(monitor_time->ToString()) + L", PC " + storage::FromUtf8(now.ToString()) + L" (" +
                              (status.drift_seconds >= 0 ? L"+" : L"") + std::to_wstring(status.drift_seconds) + L" s)");
    if (std::labs(status.drift_seconds) <= kClockToleranceSeconds) return status;
    if (!status.verified) {
        log(ble::Level::Info, L"clock record checksum does not match the known layout - not correcting the clock");
        return status;
    }
    if (!sync) return status;
    std::array<uint8_t, protocol::kMaxReadSize> out{};
    const models::Timestamp target = storage::LocalNow();
    models::EncodeClock(clock, record.data(), record.size(), target, out.data());
    monitor.WriteEeprom(clock.write_address, out.data(), clock.size);
    status.corrected = true;
    log(ble::Level::Info, L"monitor clock set to " + storage::FromUtf8(target.ToString()) + L" (applied when the session ends)");
    return status;
}

}  // namespace

void PairMonitor(uint64_t address, const models::DeviceLayout& layout, const std::vector<uint64_t>& others, const Log& log) {
    assert(address != 0 && log);
    ReleaseOthers(others, address, log);
    log(ble::Level::Info, L"waiting for the monitor ...");
    WaitForAdvertisement(address, kAdvertisementWait, log);
    for (int attempt = 1; attempt <= kConnectAttempts; ++attempt) {
        try {
            ble::Monitor monitor(address, log);
            log(ble::Level::Info, L"bonding ...");
            monitor.EnsureBonded();
            log(ble::Level::Info, L"programming the pairing key ...");
            monitor.ProgramKey(protocol::kDefaultKey);
            // A freshly paired monitor expects one session before it sleeps cleanly.
            monitor.StartSession();
            monitor.EndSession();
            log(ble::Level::Info, std::wstring(L"paired ") + layout.retail);
            return;
        } catch (const ble::NotFound&) {
            throw;
        } catch (const ble::BleError& exc) {
            log(ble::Level::Info, L"attempt " + std::to_wstring(attempt) + L" failed: " + Widen(exc.what()));
            if (attempt == kConnectAttempts) throw;
        } catch (const winrt::hresult_error& exc) {
            log(ble::Level::Info, L"attempt " + std::to_wstring(attempt) + L" failed: " + std::wstring(exc.message()));
            if (attempt == kConnectAttempts) throw ble::BleError("Bluetooth error: " + storage::ToUtf8(std::wstring(exc.message())));
        }
        std::this_thread::sleep_for(kConnectRetryDelay);
    }
}

DownloadResult Download(const storage::KnownDevice& device, const std::vector<uint64_t>& others, const Paths& paths, bool sync_clock, const Log& log) {
    assert(log);
    const models::DeviceLayout* layout = models::FindLayout(device.model);
    if (layout == nullptr) throw ble::BleError("unknown model " + storage::ToUtf8(device.model));
    const uint64_t address = device.AddressValue();
    if (address == 0) throw ble::BleError("bad Bluetooth address in devices.json");

    ReleaseOthers(others, address, log);
    log(ble::Level::Info, L"waiting for " + device.name + L" ...");
    WaitForAdvertisement(address, kAdvertisementWait, log);

    DownloadResult result;
    for (int attempt = 1; attempt <= kConnectAttempts; ++attempt) {
        try {
            ble::Monitor monitor(address, log);
            monitor.EnsureBonded();
            monitor.Unlock(protocol::kDefaultKey);
            monitor.StartSession();
            result.per_user.clear();
            try {
                for (size_t user = 0; user < layout->user_count; ++user) {
                    const size_t size = static_cast<size_t>(layout->records_per_user[user]) * layout->record_size;
                    log(ble::Level::Info, L"reading user " + std::to_wstring(user + 1) + L" ...");
                    const std::vector<uint8_t> region = monitor.ReadEeprom(layout->user_start[user], size, layout->read_block_size);
                    result.per_user.push_back(SplitRecords(*layout, user, region, log));
                }
                result.clock = CheckClock(monitor, *layout, sync_clock, log);
            } catch (...) {
                try {
                    monitor.EndSession();  // otherwise the monitor shows "Err"
                } catch (...) {
                }
                throw;
            }
            monitor.EndSession();
            break;
        } catch (const ble::NotFound&) {
            throw;
        } catch (const ble::BleError& exc) {
            log(ble::Level::Info, L"attempt " + std::to_wstring(attempt) + L" failed: " + Widen(exc.what()));
            if (attempt == kConnectAttempts) throw;
            std::this_thread::sleep_for(kConnectRetryDelay);
        } catch (const winrt::hresult_error& exc) {
            log(ble::Level::Info, L"attempt " + std::to_wstring(attempt) + L" failed: " + std::wstring(exc.message()));
            if (attempt == kConnectAttempts) throw ble::BleError("Bluetooth error: " + storage::ToUtf8(std::wstring(exc.message())));
            std::this_thread::sleep_for(kConnectRetryDelay);
        }
    }
    assert(result.per_user.size() == layout->user_count);
    result.appended = storage::AppendReadings(paths.csv, device.name, device.model, result.per_user);
    return result;
}

}  // namespace omron::workflow
