#include "ble.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cassert>
#include <cwctype>
#include <thread>

namespace omron::ble {

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Storage::Streams;

namespace {

constexpr wchar_t kNamePrefix[] = L"blesmart_";

const guid kServiceUuid{L"ecbe3980-c9a2-11e1-b1bd-0002a5d5c51b"};
const std::array<guid, protocol::kChannelCount> kRxUuids{
    guid{L"49123040-aee8-11e1-a74d-0002a5d5c51b"},
    guid{L"4d0bf320-aee8-11e1-a0d9-0002a5d5c51b"},
    guid{L"5128ce60-aee8-11e1-b84b-0002a5d5c51b"},
    guid{L"560f1420-aee8-11e1-8184-0002a5d5c51b"},
};
const std::array<guid, protocol::kChannelCount> kTxUuids{
    guid{L"db5b55e0-aee7-11e1-965e-0002a5d5c51b"},
    guid{L"e0b8a060-aee7-11e1-92f4-0002a5d5c51b"},
    guid{L"0ae12b00-aee8-11e1-a192-0002a5d5c51b"},
    guid{L"10e1ba60-aee8-11e1-89e5-0002a5d5c51b"},
};
const guid kUnlockUuid{L"b305b680-aee7-11e1-a730-0002a5d5c51b"};

constexpr auto kReplyTimeout = std::chrono::milliseconds(1000);
constexpr int kMaxRetries = 5;
constexpr auto kUnlockTimeout = std::chrono::seconds(10);
constexpr int kPairingAttempts = 10;
constexpr auto kPairingRetryDelay = std::chrono::seconds(1);
constexpr int kBondSettleAttempts = 3;
constexpr int kBondRetryAttempts = 6;
constexpr auto kBondRetryDelay = std::chrono::milliseconds(1500);
constexpr int kServicePollAttempts = 20;

std::wstring Hex(const uint8_t* data, size_t size) {
    static constexpr wchar_t kDigits[] = L"0123456789abcdef";
    std::wstring out;
    for (size_t i = 0; i < size && i < 64; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

IBuffer MakeBuffer(const uint8_t* data, size_t size) {
    DataWriter writer;
    writer.WriteBytes(array_view<const uint8_t>(data, data + size));
    return writer.DetachBuffer();
}

const char* StatusName(GattCommunicationStatus status) {
    switch (status) {
        case GattCommunicationStatus::Success: return "success";
        case GattCommunicationStatus::Unreachable: return "unreachable";
        case GattCommunicationStatus::ProtocolError: return "protocol error";
        case GattCommunicationStatus::AccessDenied: return "access denied";
    }
    return "unknown";
}

DeviceInformation FreshDeviceInformation(const BluetoothLEDevice& device) {
    // The DeviceInformation hanging off the device object does not refresh its pairing state.
    return DeviceInformation::CreateFromIdAsync(device.DeviceInformation().Id()).get();
}

}  // namespace

void InitWorkerThread() {
    init_apartment(apartment_type::multi_threaded);
}

bool IsOmronName(const std::wstring& name) {
    const size_t n = sizeof kNamePrefix / sizeof kNamePrefix[0] - 1;
    if (name.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::towlower(name[i]) != kNamePrefix[i]) return false;
    }
    return true;
}

// --- Scanner -------------------------------------------------------------------------------------

Scanner::Scanner(std::function<void(const FoundDevice&)> on_found) : on_found_(std::move(on_found)) {
    assert(on_found_);
    watcher_.ScanningMode(BluetoothLEScanningMode::Active);
    token_ = watcher_.Received([this](const BluetoothLEAdvertisementWatcher&, const BluetoothLEAdvertisementReceivedEventArgs& args) {
        FoundDevice found;
        found.address = args.BluetoothAddress();
        found.name = std::wstring(args.Advertisement().LocalName());
        found.rssi = args.RawSignalStrengthInDBm();
        on_found_(found);
    });
}

Scanner::~Scanner() {
    Stop();
    watcher_.Received(token_);
}

void Scanner::Start() {
    if (running_) return;
    watcher_.Start();
    running_ = true;
}

void Scanner::Stop() {
    if (!running_) return;
    watcher_.Stop();
    running_ = false;
}

// --- bonds ---------------------------------------------------------------------------------------

bool ReleaseBond(uint64_t address, const LogFn& log) {
    assert(log);
    BluetoothLEDevice device = BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
    if (!device) return false;
    DeviceInformation info = FreshDeviceInformation(device);
    if (!info.Pairing().IsPaired()) return false;
    const DeviceUnpairingResult result = info.Pairing().UnpairAsync().get();
    log(Level::Info, L"released bond with " + std::wstring(device.Name()) + L" (status " +
        std::to_wstring(static_cast<int>(result.Status())) + L")");
    return result.Status() == DeviceUnpairingResultStatus::Unpaired || result.Status() == DeviceUnpairingResultStatus::AlreadyUnpaired;
}

// --- Monitor -------------------------------------------------------------------------------------

Monitor::Monitor(uint64_t address, LogFn log) : address_(address), log_(std::move(log)) {
    assert(address_ != 0 && log_);
    device_ = BluetoothLEDevice::FromBluetoothAddressAsync(address_).get();
    if (!device_) {
        throw NotFound("monitor not found - press its Bluetooth button and try again");
    }
    session_ = GattSession::FromDeviceIdAsync(device_.BluetoothDeviceId()).get();
    session_.MaintainConnection(true);

    // The service can take a moment to appear after the link comes up.
    for (int attempt = 0; attempt < kServicePollAttempts; ++attempt) {
        const GattDeviceServicesResult result = device_.GetGattServicesForUuidAsync(kServiceUuid, BluetoothCacheMode::Uncached).get();
        if (result.Status() == GattCommunicationStatus::Success && result.Services().Size() > 0) {
            service_ = result.Services().GetAt(0);
            break;
        }
        if (result.Status() == GattCommunicationStatus::Unreachable && attempt >= 2) {
            throw NotFound("monitor not reachable - press its Bluetooth button and try again");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!service_) {
        throw BleError("connected, but the OMRON service is missing - is this really an OMRON monitor?");
    }
    ResolveCharacteristics();
    log_(Level::Info, L"connected to " + std::wstring(device_.Name()));
}

Monitor::~Monitor() {
    for (auto& revoker : rx_revokers_) revoker.revoke();
    unlock_revoker_.revoke();
    if (session_) session_.MaintainConnection(false);
    if (service_) service_.Close();
    if (session_) session_.Close();
    if (device_) device_.Close();
}

void Monitor::ResolveCharacteristics() {
    assert(service_ != nullptr);
    auto find = [this](const guid& uuid) -> Characteristic {
        const GattCharacteristicsResult result = service_.GetCharacteristicsForUuidAsync(uuid, BluetoothCacheMode::Uncached).get();
        if (result.Status() != GattCommunicationStatus::Success || result.Characteristics().Size() == 0) {
            throw BleError(std::string("characteristic missing (") + StatusName(result.Status()) + ")");
        }
        return result.Characteristics().GetAt(0);
    };
    for (size_t i = 0; i < protocol::kChannelCount; ++i) {
        rx_[i] = find(kRxUuids[i]);
        tx_[i] = find(kTxUuids[i]);
    }
    unlock_ = find(kUnlockUuid);
    assert(rx_[0] != nullptr && tx_[0] != nullptr && unlock_ != nullptr);
}

void Monitor::OnRx(size_t channel, const uint8_t* data, size_t size) {
    assert(channel < protocol::kChannelCount && data != nullptr && size <= protocol::kChannelWidth + 4);
    std::lock_guard<std::mutex> lock(mutex_);
    log_(Level::Debug, L"rx ch" + std::to_wstring(channel) + L" < " + Hex(data, size));
    try {
        std::optional<protocol::Packet> packet = assembler_.Push(channel, data, size);
        if (packet) {
            assert(packet->raw.size >= protocol::kHeaderSize + protocol::kTrailerSize);
            reply_ = packet;
            cv_.notify_all();
        }
    } catch (const protocol::ProtocolError& exc) {
        log_(Level::Debug, L"discarding corrupt reply: " + std::wstring(exc.what(), exc.what() + std::char_traits<char>::length(exc.what())));
    }
}

void Monitor::OnUnlock(const uint8_t* data, size_t size) {
    assert(data != nullptr && size > 0);
    std::lock_guard<std::mutex> lock(mutex_);
    log_(Level::Debug, L"rx unlock < " + Hex(data, size));
    std::array<uint8_t, 20> reply{};
    for (size_t i = 0; i < size && i < reply.size(); ++i) reply[i] = data[i];
    unlock_reply_ = reply;
    cv_.notify_all();
}

void Monitor::EnableNotifications() {
    assert(unlock_ != nullptr);
    if (rx_revokers_[0]) return;
    for (size_t i = 0; i < protocol::kChannelCount; ++i) {
        rx_revokers_[i] = rx_[i].ValueChanged(auto_revoke, [this, i](const Characteristic&, const GattValueChangedEventArgs& args) {
            const IBuffer buffer = args.CharacteristicValue();
            OnRx(i, buffer.data(), buffer.Length());
        });
        const GattWriteResult result =
            rx_[i].WriteClientCharacteristicConfigurationDescriptorWithResultAsync(GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
        if (result.Status() != GattCommunicationStatus::Success) {
            throw BleError("cannot enable notifications on channel " + std::to_string(i));
        }
    }
    assert(rx_revokers_[protocol::kChannelCount - 1]);
    unlock_revoker_ = unlock_.ValueChanged(auto_revoke, [this](const Characteristic&, const GattValueChangedEventArgs& args) {
        const IBuffer buffer = args.CharacteristicValue();
        OnUnlock(buffer.data(), buffer.Length());
    });
    const GattWriteResult result =
        unlock_.WriteClientCharacteristicConfigurationDescriptorWithResultAsync(GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
    if (result.Status() != GattCommunicationStatus::Success) {
        throw BleError("cannot enable notifications on the unlock channel");
    }
}

bool Monitor::IsBonded() {
    return FreshDeviceInformation(device_).Pairing().IsPaired();
}

void Monitor::EnsureBonded() {
    assert(device_ != nullptr);
    // The monitor may start a bond itself right after connecting; asking at the same moment
    // fails with OperationAlreadyInProgress, so let that settle and retry.
    for (int i = 0; i < kBondSettleAttempts; ++i) {
        if (IsBonded()) return;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    DeviceInformation info = FreshDeviceInformation(device_);
    assert(info != nullptr);
    DeviceInformationCustomPairing custom = info.Pairing().Custom();
    auto revoker = custom.PairingRequested(auto_revoke, [](const DeviceInformationCustomPairing&, const DevicePairingRequestedEventArgs& args) {
        args.Accept();
    });
    for (int attempt = 1; attempt <= kBondRetryAttempts; ++attempt) {
        const DevicePairingResult result = custom.PairAsync(DevicePairingKinds::ConfirmOnly, DevicePairingProtectionLevel::Encryption).get();
        const DevicePairingResultStatus status = result.Status();
        if (status == DevicePairingResultStatus::Paired || status == DevicePairingResultStatus::AlreadyPaired) {
            log_(Level::Info, L"bonded with " + std::wstring(device_.Name()));
            return;
        }
        if (status != DevicePairingResultStatus::OperationAlreadyInProgress) {
            throw BleError("Windows could not bond with the monitor (status " + std::to_string(static_cast<int>(status)) +
                           "). If another OMRON monitor is paired in Windows Settings, remove it there first.");
        }
        log_(Level::Debug, L"bond attempt " + std::to_wstring(attempt) + L": already in progress, retrying");
        std::this_thread::sleep_for(kBondRetryDelay);
    }
    throw BleError("bonding kept reporting 'already in progress'");
}

std::array<uint8_t, 20> Monitor::UnlockExchange(uint8_t op, const std::array<uint8_t, protocol::kKeySize>& key) {
    assert(op <= protocol::kUnlockOpEnterPairing && unlock_ != nullptr);
    std::array<uint8_t, 1 + protocol::kKeySize> packet{};
    packet[0] = op;
    for (size_t i = 0; i < protocol::kKeySize; ++i) packet[1 + i] = key[i];
    {
        std::lock_guard<std::mutex> lock(mutex_);
        unlock_reply_.reset();
    }
    const GattWriteResult result = unlock_.WriteValueWithResultAsync(MakeBuffer(packet.data(), packet.size()), GattWriteOption::WriteWithResponse).get();
    if (result.Status() != GattCommunicationStatus::Success) {
        throw BleError("unlock write failed");
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, kUnlockTimeout, [this] { return unlock_reply_.has_value(); })) {
        throw BleError("no reply on the unlock channel");
    }
    assert(unlock_reply_.has_value());
    return *unlock_reply_;
}

void Monitor::Unlock(const std::array<uint8_t, protocol::kKeySize>& key) {
    EnableNotifications();
    const std::array<uint8_t, 20> reply = UnlockExchange(protocol::kUnlockOpUnlock, key);
    if (reply[0] != protocol::kUnlockRspUnlocked || reply[1] != 0) {
        throw BleError("the monitor rejected the key - pair it again (Bluetooth button until P blinks)");
    }
}

void Monitor::ProgramKey(const std::array<uint8_t, protocol::kKeySize>& key) {
    const std::array<uint8_t, protocol::kKeySize> zero{};  // all-zero key = "enter pairing mode"
    assert(key != zero);
    EnableNotifications();
    bool entered = false;
    for (int attempt = 1; attempt <= kPairingAttempts; ++attempt) {
        const std::array<uint8_t, 20> reply = UnlockExchange(protocol::kUnlockOpEnterPairing, zero);
        if (reply[0] == protocol::kUnlockRspPairingMode && reply[1] == 0) {
            entered = true;
            break;
        }
        log_(Level::Debug, L"pairing-mode attempt " + std::to_wstring(attempt) + L" answered " + Hex(reply.data(), 2));
        std::this_thread::sleep_for(kPairingRetryDelay);
    }
    if (!entered) {
        throw BleError("the monitor did not enter key-programming mode - is it showing the blinking P?");
    }
    assert(entered);
    const std::array<uint8_t, 20> reply = UnlockExchange(protocol::kUnlockOpStoreKey, key);
    if (reply[0] != protocol::kUnlockRspKeyStored || reply[1] != 0) {
        throw BleError("the monitor refused the new key");
    }
    log_(Level::Info, L"key programmed");
}

void Monitor::WriteChunks(const protocol::Bytes& command) {
    assert(command.size > 0 && command.size <= protocol::kMaxPacket);
    const size_t chunks = (command.size + protocol::kChannelWidth - 1) / protocol::kChannelWidth;
    assert(chunks >= 1 && chunks <= protocol::kChannelCount);
    for (size_t i = 0; i < chunks; ++i) {
        const size_t offset = i * protocol::kChannelWidth;
        const size_t size = std::min(protocol::kChannelWidth, command.size - offset);
        log_(Level::Debug, L"tx ch" + std::to_wstring(i) + L" > " + Hex(command.begin() + offset, size));
        const GattWriteResult result = tx_[i].WriteValueWithResultAsync(MakeBuffer(command.begin() + offset, size), GattWriteOption::WriteWithResponse).get();
        if (result.Status() != GattCommunicationStatus::Success) {
            throw BleError("write failed on channel " + std::to_string(i));
        }
    }
}

protocol::Packet Monitor::Send(const protocol::Bytes& command) {
    assert(command.size >= protocol::kHeaderSize + protocol::kTrailerSize && rx_revokers_[0]);
    for (int attempt = 1; attempt <= kMaxRetries; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            assembler_.Reset();
            reply_.reset();
        }
        WriteChunks(command);
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, kReplyTimeout, [this] { return reply_.has_value(); })) {
            assert(reply_.has_value());
            return *reply_;
        }
        log_(Level::Debug, L"no reply (attempt " + std::to_wstring(attempt) + L"/" + std::to_wstring(kMaxRetries) + L")");
    }
    throw BleError("the monitor stopped answering");
}

void Monitor::StartSession() {
    protocol::Bytes cmd;
    std::copy(protocol::kStartCommand.begin(), protocol::kStartCommand.end(), cmd.data.begin());
    cmd.size = protocol::kStartCommand.size();
    const protocol::Packet reply = Send(cmd);
    if (reply.kind != protocol::kRspStart) throw BleError("unexpected reply to session start");
}

void Monitor::EndSession() {
    protocol::Bytes cmd;
    std::copy(protocol::kEndCommand.begin(), protocol::kEndCommand.end(), cmd.data.begin());
    cmd.size = protocol::kEndCommand.size();
    const protocol::Packet reply = Send(cmd);
    if (reply.kind != protocol::kRspEnd) throw BleError("unexpected reply to session end");
    if (reply.raw[protocol::kHeaderSize] != 0) {
        throw BleError("the monitor reported error " + std::to_string(reply.raw[protocol::kHeaderSize]) + " at end of session");
    }
}

std::vector<uint8_t> Monitor::ReadBlock(uint16_t address, uint8_t size) {
    assert(size > 0 && size <= protocol::kMaxReadSize);
    const protocol::Packet reply = Send(protocol::BuildReadCommand(address, size));
    if (reply.kind != protocol::kRspRead) throw BleError("unexpected reply to read");
    if (reply.address != address) throw BleError("reply for the wrong address");
    if (reply.DataSize() == 0 && reply.raw[5] == size) {
        // Header-only reply: an erased region reads as 0xFF (empty slots).
        return std::vector<uint8_t>(size, 0xFF);
    }
    if (reply.DataSize() != size) throw BleError("reply carried the wrong number of bytes");
    assert(reply.DataSize() == size);
    return std::vector<uint8_t>(reply.Data(), reply.Data() + size);
}

void Monitor::WriteEeprom(uint16_t address, const uint8_t* data, size_t size) {
    assert(data != nullptr && size > 0 && size <= protocol::kMaxReadSize);
    const protocol::Packet reply = Send(protocol::BuildWriteCommand(address, data, size));
    if (reply.kind != protocol::kRspWrite) throw BleError("unexpected reply to write");
    if (reply.address != address) throw BleError("write acknowledged for the wrong address");
}

std::vector<uint8_t> Monitor::ReadEeprom(uint16_t address, size_t size, size_t block_size) {
    assert(block_size > 0 && block_size <= protocol::kMaxReadSize);
    assert(size > 0 && size <= 0x10000);
    std::vector<uint8_t> out;
    out.reserve(size);
    const size_t max_blocks = (size + block_size - 1) / block_size;
    for (size_t i = 0; i < max_blocks && out.size() < size; ++i) {
        const size_t chunk = std::min(block_size, size - out.size());
        const std::vector<uint8_t> block = ReadBlock(static_cast<uint16_t>(address + out.size()), static_cast<uint8_t>(chunk));
        out.insert(out.end(), block.begin(), block.end());
    }
    assert(out.size() == size);
    return out;
}

}  // namespace omron::ble
