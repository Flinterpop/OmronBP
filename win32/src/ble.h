// Bluetooth LE access to an OMRON monitor via C++/WinRT.
//
// All Monitor methods block (they wait on WinRT async operations) and must be
// called from a worker thread that has called InitWorkerThread(), never from
// the UI thread.
#pragma once

#include <winrt/base.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "protocol.h"

namespace omron::ble {

enum class Level { Info, Debug };
using LogFn = std::function<void(Level, const std::wstring&)>;

class BleError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The monitor is not advertising (not woken, or its window has closed).
class NotFound : public BleError {
public:
    using BleError::BleError;
};

struct FoundDevice {
    uint64_t address = 0;
    std::wstring name;
    int rssi = 0;
};

void InitWorkerThread();  // winrt::init_apartment(multi_threaded)

bool IsOmronName(const std::wstring& name);  // "BLEsmart_..." (case-insensitive)

// Live advertisement watcher.  The callback runs on a WinRT thread; it must
// not touch UI directly (post a message instead).
class Scanner {
public:
    explicit Scanner(std::function<void(const FoundDevice&)> on_found);
    ~Scanner();
    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;
    void Start();
    void Stop();

private:
    std::function<void(const FoundDevice&)> on_found_;
    winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher watcher_;
    winrt::event_token token_{};
    bool running_ = false;
};

// Forget the Windows bond for a monitor.  Returns true if one was removed.
bool ReleaseBond(uint64_t address, const LogFn& log);

// The EEPROM operations the clock logic needs; Monitor implements them over Bluetooth,
// tests implement them over a byte array.
class EepromIo {
public:
    virtual ~EepromIo() = default;
    virtual std::vector<uint8_t> ReadEeprom(uint16_t address, size_t size, size_t block_size) = 0;
    virtual void WriteEeprom(uint16_t address, const uint8_t* data, size_t size) = 0;
};

// One connection to a monitor.
class Monitor : public EepromIo {
public:
    Monitor(uint64_t address, LogFn log);  // connects and resolves the OMRON service
    ~Monitor();
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    bool IsBonded();
    void EnsureBonded();  // bonds if needed (Windows "Just Works", encryption level)

    void Unlock(const std::array<uint8_t, protocol::kKeySize>& key);
    void ProgramKey(const std::array<uint8_t, protocol::kKeySize>& key);  // monitor must be in pairing mode
    void StartSession();
    void EndSession();
    std::vector<uint8_t> ReadEeprom(uint16_t address, size_t size, size_t block_size) override;
    void WriteEeprom(uint16_t address, const uint8_t* data, size_t size) override;  // one block, at most 0x38 bytes

private:
    using Characteristic = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic;

    void ResolveCharacteristics();
    void EnableNotifications();
    void OnRx(size_t channel, const uint8_t* data, size_t size);
    void OnUnlock(const uint8_t* data, size_t size);
    void WriteChunks(const protocol::Bytes& command);
    protocol::Packet Send(const protocol::Bytes& command);
    std::vector<uint8_t> ReadBlock(uint16_t address, uint8_t size);
    std::array<uint8_t, 20> UnlockExchange(uint8_t op, const std::array<uint8_t, protocol::kKeySize>& key);

    uint64_t address_;
    LogFn log_;
    winrt::Windows::Devices::Bluetooth::BluetoothLEDevice device_{nullptr};
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSession session_{nullptr};
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceService service_{nullptr};
    std::array<Characteristic, protocol::kChannelCount> rx_{nullptr, nullptr, nullptr, nullptr};
    std::array<Characteristic, protocol::kChannelCount> tx_{nullptr, nullptr, nullptr, nullptr};
    Characteristic unlock_{nullptr};
    std::array<Characteristic::ValueChanged_revoker, protocol::kChannelCount> rx_revokers_{};
    Characteristic::ValueChanged_revoker unlock_revoker_{};

    std::mutex mutex_;
    std::condition_variable cv_;
    protocol::ChannelAssembler assembler_;
    std::optional<protocol::Packet> reply_;
    std::optional<std::array<uint8_t, 20>> unlock_reply_;
};

}  // namespace omron::ble
