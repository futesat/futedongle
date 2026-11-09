#include <stdio.h>
#include <chrono>

#include "common.h"
#include "bluetoothHandler.h"
#include "bluetoothProfiles.h"
#include "bluetoothAdvertisement.h"

static int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

static constexpr const char* BLUEZ_BUS_NAME = "org.bluez";
static constexpr const char* BLUEZ_ROOT_OBJECT_PATH = "/";
static constexpr const char* BLUEZ_OBJECT_PATH = "/org/bluez";

static constexpr const char* INTERFACE_BLUEZ_ADAPTER = "org.bluez.Adapter1";
static constexpr const char* INTERFACE_BLUEZ_LE_ADVERTISING_MANAGER = "org.bluez.LEAdvertisingManager1";

static constexpr const char* INTERFACE_BLUEZ_DEVICE = "org.bluez.Device1";
static constexpr const char* INTERFACE_BLUEZ_PROFILE_MANAGER = "org.bluez.ProfileManager1";

static constexpr const char* LE_ADVERTISEMENT_OBJECT_PATH = "/com/aawgd/bluetooth/advertisement";

static constexpr const char* AAWG_PROFILE_OBJECT_PATH = "/com/aawgd/bluetooth/aawg";
static constexpr const char* AAWG_PROFILE_UUID = "4de17a00-52cb-11e6-bdf4-0800200c9a66";

static constexpr const char* HSP_HS_PROFILE_OBJECT_PATH = "/com/aawgd/bluetooth/hsp";
static constexpr const char* HSP_AG_UUID = "00001112-0000-1000-8000-00805f9b34fb";
static constexpr const char* HSP_HS_UUID = "00001108-0000-1000-8000-00805f9b34fb";


class BluezAdapterProxy: private DBus::ObjectProxy {
    BluezAdapterProxy(std::shared_ptr<DBus::Connection> conn, DBus::Path path): DBus::ObjectProxy(conn, BLUEZ_BUS_NAME, path) {
        alias = this->create_property<std::string>(INTERFACE_BLUEZ_ADAPTER, "Alias");
        powered = this->create_property<bool>(INTERFACE_BLUEZ_ADAPTER, "Powered");
        discoverable = this->create_property<bool>(INTERFACE_BLUEZ_ADAPTER, "Discoverable");
        pairable = this->create_property<bool>(INTERFACE_BLUEZ_ADAPTER, "Pairable");

        registerAdvertisement = this->create_method<void(DBus::Path, DBus::Properties)>(INTERFACE_BLUEZ_LE_ADVERTISING_MANAGER, "RegisterAdvertisement");
        unregisterAdvertisement = this->create_method<void(DBus::Path)>(INTERFACE_BLUEZ_LE_ADVERTISING_MANAGER, "UnregisterAdvertisement");
    }

public:
    static std::shared_ptr<BluezAdapterProxy> create(std::shared_ptr<DBus::Connection> conn, DBus::Path path)
    {
      return std::shared_ptr<BluezAdapterProxy>(new BluezAdapterProxy(conn, path));
    }

    std::shared_ptr<DBus::PropertyProxy<std::string>> alias;
    std::shared_ptr<DBus::PropertyProxy<bool>> powered;
    std::shared_ptr<DBus::PropertyProxy<bool>> discoverable;
    std::shared_ptr<DBus::PropertyProxy<bool>> pairable;

    std::shared_ptr<DBus::MethodProxy<void(DBus::Path, DBus::Properties)>> registerAdvertisement;
    std::shared_ptr<DBus::MethodProxy<void(DBus::Path)>> unregisterAdvertisement;
};


BluetoothHandler& BluetoothHandler::instance() {
    static BluetoothHandler instance;
    return instance;
}

DBus::ManagedObjects BluetoothHandler::getBluezObjects() {
    std::shared_ptr<DBus::ObjectProxy> m_bluezRootObject = m_connection->create_object_proxy(BLUEZ_BUS_NAME, BLUEZ_ROOT_OBJECT_PATH);
    DBus::MethodProxy getManagedObjects = *(m_bluezRootObject->create_method<DBus::ManagedObjects(void)>("org.freedesktop.DBus.ObjectManager", "GetManagedObjects"));

    return getManagedObjects();
}

void BluetoothHandler::initAdapter() {
    DBus::ManagedObjects objects = getBluezObjects();

    std::string adapter_path;
    for (auto const& [path, interfaces]: objects) {
        for (auto const& [interface, properties]: interfaces) {
            if (interface == INTERFACE_BLUEZ_ADAPTER) {
                adapter_path = path;
                Logger::instance()->info("Using bluetooth adapter at path: %s\n", path.c_str());
                break;
            }
        }
        if (!adapter_path.empty()) {
            break;
        }
    }

    if (adapter_path.empty()) {
        Logger::instance()->info("Did not find any bluetooth adapters\n");
    }
    else {
        m_adapter = BluezAdapterProxy::create(m_connection, adapter_path);
        try {
            m_adapter->alias->set_value(m_adapterAlias);
            Logger::instance()->info("Bluetooth adapter alias: %s\n", m_adapterAlias.c_str());
        } catch (DBus::Error& e) {
            Logger::instance()->info("Failed to set bluetooth adapter alias: %s\n", e.what());
        }
    }
}

void BluetoothHandler::setPower(bool on) {
    if (!m_adapter) {
        return;
    }

    try {
        m_adapter->powered->set_value(on);
        Logger::instance()->info("Bluetooth adapter was powered %s\n", on ? "on" : "off");
    } catch (DBus::Error& e) {
        Logger::instance()->info("Failed to set bluetooth power %s: %s\n", on ? "on" : "off", e.what());
    }
}

void BluetoothHandler::setPairable(bool pairable) {
    if (!m_adapter) {
        return;
    }

    try {
        m_adapter->discoverable->set_value(pairable);
        m_adapter->pairable->set_value(pairable);
        Logger::instance()->info("Bluetooth adapter is now discoverable and pairable\n");
    } catch (DBus::Error& e) {
        Logger::instance()->info("Failed to set bluetooth discoverable/pairable: %s\n", e.what());
    }
}

void BluetoothHandler::exportProfiles() {
    std::shared_ptr<DBus::ObjectProxy> bluezObject = m_connection->create_object_proxy(BLUEZ_BUS_NAME, BLUEZ_OBJECT_PATH);
    DBus::MethodProxy registerProfile = *(bluezObject->create_method<void(DBus::Path, std::string, DBus::Properties)>(INTERFACE_BLUEZ_PROFILE_MANAGER, "RegisterProfile"));

    // Register AA Wireless Profile
    m_aawProfile = AAWirelessProfile::create(AAWG_PROFILE_OBJECT_PATH);
    if (m_connection->register_object(m_aawProfile, DBus::ThreadForCalling::DispatcherThread) != DBus::RegistrationStatus::Success) {
        Logger::instance()->info("Failed to register AA Wireless profile\n");
    }

    registerProfile(AAWG_PROFILE_OBJECT_PATH, AAWG_PROFILE_UUID, {
        {"Name", DBus::Variant("AA Wireless")},
        {"Role", DBus::Variant("server")},
        {"Channel", DBus::Variant(uint16_t(8))},
        {"RequireAuthentication", DBus::Variant(false)},
        {"RequireAuthorization", DBus::Variant(false)},
        {"AutoConnect", DBus::Variant(true)},
    });
    Logger::instance()->info("Bluetooth AA Wireless profile active\n");

    if (Config::instance()->getConnectionStrategy() != ConnectionStrategy::DONGLE_MODE) {
        // Register HSP Handset profile
        m_hspProfile = HSPHSProfile::create(HSP_HS_PROFILE_OBJECT_PATH);
        if (m_connection->register_object(m_hspProfile, DBus::ThreadForCalling::DispatcherThread) != DBus::RegistrationStatus::Success) {
            Logger::instance()->info("Failed to register HSP Handset profile\n");
        }
        registerProfile(HSP_HS_PROFILE_OBJECT_PATH, HSP_HS_UUID, {
            {"Name", DBus::Variant("HSP HS")},
            {"RequireAuthorization", DBus::Variant(false)},
            {"AutoConnect", DBus::Variant(true)},
        });
        Logger::instance()->info("HSP Handset profile active\n");
    }
}

void BluetoothHandler::startAdvertising() {
    if (!m_adapter) {
        return;
    }

    // Register Advertisement Object
    m_leAdvertisement = BLEAdvertisement::create(LE_ADVERTISEMENT_OBJECT_PATH);

    m_leAdvertisement->type->set_value("peripheral");
    m_leAdvertisement->serviceUUIDs->set_value(std::vector<std::string>{AAWG_PROFILE_UUID});
    m_leAdvertisement->localName->set_value(m_adapterAlias);

    if (m_connection->register_object(m_leAdvertisement, DBus::ThreadForCalling::DispatcherThread) != DBus::RegistrationStatus::Success) {
        Logger::instance()->info("Failed to register BLE Advertisement\n");
    }

    (*m_adapter->registerAdvertisement)(LE_ADVERTISEMENT_OBJECT_PATH, {});
    Logger::instance()->info("BLE Advertisement started\n");
}

void BluetoothHandler::stopAdvertising() {
    if (!m_adapter) {
        return;
    }

    (*m_adapter->unregisterAdvertisement)(LE_ADVERTISEMENT_OBJECT_PATH);
    Logger::instance()->info("BLE Advertisement stopped\n");
}

void BluetoothHandler::notifyHandoffStarted() {
    m_lastHandoffTimestampMs.store(currentTimeMs());
}

void BluetoothHandler::clearHandoffInProgress() {
    m_lastHandoffTimestampMs.store(0);
}

bool BluetoothHandler::connectDevice() {
    int64_t lastHandoff = m_lastHandoffTimestampMs.load();
    if (lastHandoff > 0 && (currentTimeMs() - lastHandoff) < 10000) {
        Logger::instance()->info("Bluetooth handoff in progress, waiting for Wi-Fi TCP connection\n");
        return true;
    }

    DBus::ManagedObjects objects = getBluezObjects();

    std::vector<std::string> device_paths;
    for (auto const& [path, interfaces]: objects) {
        for (auto const& [interface, properties]: interfaces) {
            if (interface == INTERFACE_BLUEZ_DEVICE) {
                if (auto it = properties.find("Paired"); it != properties.end() && !it->second.to_bool()) {
                    Logger::instance()->info("Skipping unpaired bluetooth device at path: %s\n", path.c_str());
                    continue;
                }
                device_paths.push_back(path);
            }
        }
    }

    if (!device_paths.size()) {
        Logger::instance()->info("Did not find any paired bluetooth device\n");
        return false;
    }

    const bool isDongleMode = (Config::instance()->getConnectionStrategy() == ConnectionStrategy::DONGLE_MODE);
    bool anyConnected = false;

    Logger::instance()->info("Found %d paired bluetooth devices\n", device_paths.size());

    for (const std::string &device_path: device_paths) {
        Logger::instance()->info("Trying to connect bluetooth device at path: %s\n", device_path.c_str());

        std::shared_ptr<DBus::ObjectProxy> bluezDevice = m_connection->create_object_proxy(BLUEZ_BUS_NAME, device_path);
        DBus::MethodProxy connectDeviceAll = *(bluezDevice->create_method<void()>(INTERFACE_BLUEZ_DEVICE, "Connect"));
        DBus::MethodProxy connectProfile = *(bluezDevice->create_method<void(std::string)>(INTERFACE_BLUEZ_DEVICE, "ConnectProfile"));
        DBus::MethodProxy disconnect = *(bluezDevice->create_method<void()>(INTERFACE_BLUEZ_DEVICE, "Disconnect"));

        std::shared_ptr<DBus::PropertyProxy<bool>> deviceConnected = bluezDevice->create_property<bool>(INTERFACE_BLUEZ_DEVICE, "Connected");
        std::shared_ptr<DBus::PropertyProxy<bool>> deviceTrusted = bluezDevice->create_property<bool>(INTERFACE_BLUEZ_DEVICE, "Trusted");

        try {
            if (deviceTrusted && !deviceTrusted->value()) {
                deviceTrusted->set_value(true);
                Logger::instance()->info("Marked bluetooth device as trusted: %s\n", device_path.c_str());
            }

            const bool isConnected = (deviceConnected && deviceConnected->value());
            if (isConnected) {
                if (m_triedDirectConnect) {
                    Logger::instance()->info("Bluetooth device already connected without handoff, cycling connection\n");
                    disconnect();
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                } else {
                    Logger::instance()->info("Bluetooth device already connected, requesting HSP profile without disconnecting\n");
                    m_triedDirectConnect = true;
                }
            } else {
                m_triedDirectConnect = true;
            }

            if (isDongleMode) {
                connectProfile("");
            } else {
                try {
                    connectProfile(HSP_AG_UUID);
                } catch (DBus::Error& hspErr) {
                    std::string errMsg = hspErr.what() ? hspErr.what() : "";
                    if (errMsg.find("page-timeout") != std::string::npos ||
                        errMsg.find("Host is down") != std::string::npos) {
                        throw;
                    }
                    Logger::instance()->info("ConnectProfile(HSP_AG) failed (%s), falling back to Device1.Connect()\n", errMsg.c_str());
                    connectDeviceAll();
                }
            }
            Logger::instance()->info("Bluetooth connected to the device\n");
            anyConnected = true;
            if (!isDongleMode) {
                return true;
            }
        } catch (DBus::Error& e) {
            if (!isDongleMode) {
                Logger::instance()->info("Failed to connect device at path %s: %s\n", device_path.c_str(), e.what());
            }
        }
    }

    if (!isDongleMode) {
        Logger::instance()->info("Failed to connect to any known bluetooth device\n");
    }
    return anyConnected;
}

void BluetoothHandler::retryConnectLoop(std::future<void> connectWithRetryFuture) {
    bool should_exit = false;
    m_triedDirectConnect = false;
    int32_t failedAttempts = 0;

    while (!should_exit) {
        const bool connected = connectDevice();

        // When Bluetooth connects, give the phone a full 5s window to open the
        // RFCOMM AAWirelessProfile socket before considering the link stalled.
        // Only use fast 2s retries when Bluetooth connection itself failed.
        int32_t retryIntervalSec = 5;
        if (!connected) {
            retryIntervalSec = (failedAttempts < 3) ? 2 : 5;
            ++failedAttempts;
        } else {
            failedAttempts = 0;
        }

        if (connectWithRetryFuture.wait_for(std::chrono::seconds(retryIntervalSec)) == std::future_status::ready) {
            should_exit = true;
            std::lock_guard<std::mutex> lock(m_retryMutex);
            connectWithRetryPromise = nullptr;
        }
    }

    if (Config::instance()->getConnectionStrategy() != ConnectionStrategy::DONGLE_MODE) {
        BluetoothHandler::instance().powerOff();
    }
}

void BluetoothHandler::init() {
    // DBus::set_logging_function( DBus::log_std_err );
    // DBus::set_log_level( SL_TRACE );

    m_dispatcher = DBus::StandaloneDispatcher::create();
    m_connection = m_dispatcher->create_connection( DBus::BusType::SYSTEM );

    m_adapterAlias = Config::instance()->getBluetoothName();

    initAdapter();
    exportProfiles();
}

void BluetoothHandler::powerOn() {
    if (!m_adapter) {
        initAdapter();
    }
    if (!m_adapter) {
        return;
    }

    bool wasPowered = true;
    try {
        wasPowered = (m_adapter->powered && m_adapter->powered->value());
    } catch (DBus::Error&) {
        wasPowered = false;
    }

    if (!wasPowered) {
        m_lastHandoffTimestampMs.store(0);
    }

    setPower(true);
    setPairable(true);

    if (!wasPowered) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    if (Config::instance()->getConnectionStrategy() == ConnectionStrategy::DONGLE_MODE) {
        startAdvertising();
    }
}

std::optional<std::thread> BluetoothHandler::connectWithRetry() {
    if (!m_adapter) {
        return std::nullopt;
    }

    std::future<void> connectWithRetryFuture;
    {
        std::lock_guard<std::mutex> lock(m_retryMutex);
        connectWithRetryPromise = std::make_shared<std::promise<void>>();
        connectWithRetryFuture = connectWithRetryPromise->get_future();
    }
    return std::thread(&BluetoothHandler::retryConnectLoop, this, std::move(connectWithRetryFuture));
}

void BluetoothHandler::stopConnectWithRetry() {
    m_lastHandoffTimestampMs.store(0);
    std::lock_guard<std::mutex> lock(m_retryMutex);
    if (connectWithRetryPromise) {
        connectWithRetryPromise->set_value();
        connectWithRetryPromise = nullptr;
    }
}

void BluetoothHandler::powerOff() {
    if (!m_adapter) {
        return;
    }

    if (Config::instance()->getConnectionStrategy() == ConnectionStrategy::DONGLE_MODE) {
        stopAdvertising();
    }
    setPower(false);
}