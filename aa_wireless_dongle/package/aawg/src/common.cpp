#include <cstdlib>
#include <cstdarg>
#include <sstream>
#include <fstream>
#include <syslog.h>

#include "common.h"
#include "proto/WifiInfoResponse.pb.h"

#pragma region Config
/*static*/ Config* Config::instance() {
    static Config s_instance;
    return &s_instance;
}

int32_t Config::getenv(std::string name, int32_t defaultValue) {
    char* envValue = std::getenv(name.c_str());
    try {
        return envValue != nullptr ? std::stoi(envValue) : defaultValue;
    }
    catch(...) {
        return defaultValue;
    }
}

std::string Config::getenv(std::string name, std::string defaultValue) {
    char* envValue = std::getenv(name.c_str());
    return envValue != nullptr ? envValue : defaultValue;
}

std::string Config::getMacAddress(std::string interface) {
    std::ifstream addressFile("/sys/class/net/" + interface + "/address");

    std::string macAddress;
    getline(addressFile, macAddress);

    return macAddress;
}

std::string Config::getUniqueSuffix() {
    std::string uniqueSuffix = getenv("AAWG_UNIQUE_NAME_SUFFIX", "");
    if (!uniqueSuffix.empty()) {
        return uniqueSuffix;
    }

    std::ifstream serialNumberFile("/sys/firmware/devicetree/base/serial-number");

    std::string serialNumber;
    getline(serialNumberFile, serialNumber);

    // Removing trailing null from serialNumber, pad at the beginning
    serialNumber = std::string("00000000") + serialNumber.c_str();

    return serialNumber.substr(serialNumber.size() - 6);
}

std::string Config::getBluetoothName() {
    std::string btName = getenv("AAWG_BLUETOOTH_NAME", "futedongle");
    if (btName.empty()) {
        btName = "futedongle";
    }

    std::string uniqueSuffix = getenv("AAWG_UNIQUE_NAME_SUFFIX", "");
    if (!uniqueSuffix.empty()) {
        btName += "-" + uniqueSuffix;
    }

    if (getConnectionStrategy() == ConnectionStrategy::DONGLE_MODE) {
        const std::string donglePrefix = "AndroidAuto-";
        if (btName.rfind(donglePrefix, 0) != 0) {
            btName = donglePrefix + btName;
        }
    }

    return btName;
}

WifiInfo Config::getWifiInfo() {
    std::string wifiPassword = getenv("AAWG_WIFI_PASSWORD", "");
    if (wifiPassword.empty()) {
        std::ifstream pwFile("/var/run/aawgd_wifi_password");
        if (pwFile.is_open()) {
            getline(pwFile, wifiPassword);
        }
    }
    if (wifiPassword.empty()) {
        wifiPassword = "ConnectAAWirelessDongle";
    }

    return {
        getenv("AAWG_WIFI_SSID", "futedongle"),
        wifiPassword,
        getenv("AAWG_WIFI_BSSID", getMacAddress("wlan0")),
        SecurityMode::WPA2_PERSONAL,
        AccessPointType::DYNAMIC,
        getenv("AAWG_PROXY_IP_ADDRESS", "10.0.0.1"),
        getenv("AAWG_PROXY_PORT", 5288),
    };
}

ConnectionStrategy Config::getConnectionStrategy() {
    if (!connectionStrategy.has_value()) {
        const int32_t connectionStrategyEnv = getenv("AAWG_CONNECTION_STRATEGY", 1);

        switch (connectionStrategyEnv) {
            case 0:
                connectionStrategy = ConnectionStrategy::DONGLE_MODE;
                break;
            case 1:
                connectionStrategy = ConnectionStrategy::PHONE_FIRST;
                break;
            case 2:
                connectionStrategy = ConnectionStrategy::USB_FIRST;
                break;
            default:
                connectionStrategy = ConnectionStrategy::PHONE_FIRST;
                break;
        }
    }

    return connectionStrategy.value();
}

bool Config::getEarlyUsbVersionExchange() {
    if (!earlyUsbVersionExchange.has_value()) {
        earlyUsbVersionExchange = getenv("AAWG_EARLY_USB_VERSION_EXCHANGE", 1) != 0;
    }

    return earlyUsbVersionExchange.value();
}
#pragma endregion Config

#pragma region Logger
/*static*/ Logger* Logger::instance() {
    static Logger s_instance;
    return &s_instance;
}

Logger::Logger() {
    openlog(nullptr, LOG_PERROR | LOG_PID, LOG_USER);
}

Logger::~Logger() {
    closelog();
}

void Logger::info(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsyslog(LOG_INFO, format, args);
    va_end(args);
}
#pragma endregion Logger