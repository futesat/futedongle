#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "bluetoothCommon.h"
#include "common.h"

namespace {

static constexpr const char* BLUEZ_BUS_NAME = "org.bluez";
static constexpr const char* BLUEZ_ROOT_PATH = "/";
static constexpr const char* INTERFACE_OBJECT_MANAGER = "org.freedesktop.DBus.ObjectManager";
static constexpr const char* INTERFACE_BLUEZ_ADAPTER = "org.bluez.Adapter1";
static constexpr const char* INTERFACE_BLUEZ_DEVICE = "org.bluez.Device1";
static constexpr const char* INTERFACE_GATT_CHRC = "org.bluez.GattCharacteristic1";

static constexpr const char* TELEMETRY_JSON_PATH = "/run/gte_telemetry.json";
static constexpr const char* TELEMETRY_JSON_TMP = "/run/gte_telemetry.json.tmp";
static constexpr const char* LAST_TRIP_JSON_PATH = "/persist/last_gte_trip.json";
static constexpr const char* LAST_TRIP_JSON_TMP = "/persist/last_gte_trip.json.tmp";
static constexpr const char* LIFETIME_STATS_PATH = "/persist/gte_lifetime.txt";
static constexpr const char* LIFETIME_STATS_TMP = "/persist/gte_lifetime.txt.tmp";
static constexpr const char* PERSIST_OBD_MAC_PATH = "/persist/obd_ble_mac";
static constexpr const char* RUN_OBD_MAC_PATH = "/var/run/obd_ble_mac";
static constexpr const char* GARAGE_IN_RANGE_FLAG = "/var/run/garage_in_range";
static constexpr const char* GARAGE_OPENED_FLAG = "/var/run/garage_opened_once";
static constexpr const char* GARAGE_FIRING_FLAG = "/var/run/garage_firing";
static constexpr const char* GARAGE_HOLD_FLAG = "/var/run/garage_hold";
static constexpr const char* GARAGE_INIT_SCRIPT = "/etc/init.d/S38garage_opener";

std::atomic<bool> g_shouldExit{false};

void handleSignal(int) {
    g_shouldExit.store(true);
}

std::string toLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string toUpper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string jsonEscape(const std::string& input) {
    std::ostringstream ss;
    for (unsigned char c : input) {
        switch (c) {
            case '"':  ss << "\\\""; break;
            case '\\': ss << "\\\\"; break;
            case '\n': ss << "\\n";  break;
            case '\r': ss << "\\r";  break;
            case '\t': ss << "\\t";  break;
            default:
                if (c >= 0x20) {
                    ss << static_cast<char>(c);
                }
                break;
        }
    }
    return ss.str();
}

std::string currentUtcIso8601() {
    time_t now = time(nullptr);
    struct tm tmUtc;
    gmtime_r(&now, &tmUtc);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
    return std::string(buf);
}

std::string readFirstLine(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return "";
    }
    std::string line;
    std::getline(f, line);
    return trim(line);
}

void writeTextFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (f.is_open()) {
        f << content << "\n";
    }
}

bool isTcpPortEstablished(uint16_t port) {
    std::ifstream tcpFile("/proc/net/tcp");
    if (!tcpFile.is_open()) {
        return false;
    }
    char hexPort[16];
    snprintf(hexPort, sizeof(hexPort), ":%04X", port);

    std::string line;
    std::getline(tcpFile, line);
    while (std::getline(tcpFile, line)) {
        std::istringstream iss(line);
        std::string sl, localAddr, remAddr, state;
        if (!(iss >> sl >> localAddr >> remAddr >> state)) {
            continue;
        }
        if (localAddr.find(hexPort) != std::string::npos && state == "01") {
            return true;
        }
    }
    return false;
}

struct TelemetryState {
    bool adapterConnected{false};
    std::string adapterName{"Vgate iCar Pro 2S"};
    std::string adapterMac;
    bool ignitionOn{false};
    bool garageTriggeredByObd{false};
    std::string driveMode{"SEARCHING VGATE BLE..."};
    int speedKmh{0};
    int iceRpm{0};
    std::optional<int> iceCoolantC;
    std::optional<int> iceOilC;
    std::optional<int> intakeAirC;
    std::optional<int> ambientC;
    std::optional<int> dsgTempC;
    std::optional<double> iceLoadPct;
    std::optional<double> throttlePct;
    std::optional<double> turboBoostBar;
    std::optional<double> fuelLevelPct;
    std::optional<double> fuelRateLph;
    std::optional<double> fuelL100km;
    std::optional<double> hvSocDisplayPct;
    std::optional<double> hvSocGrossPct;
    std::optional<double> hvVoltageV;
    std::optional<double> hvCurrentA;
    std::optional<double> hvPowerKw;
    std::optional<double> hvTempMinC;
    std::optional<double> hvTempMaxC;
    std::optional<double> hvTempSpreadC;
    std::optional<double> hvCellMinV;
    std::optional<double> hvCellMaxV;
    std::optional<int> hvCellDeltaMv;
    std::optional<double> hvSohPct;
    std::optional<double> hvEnergyRemKwh;
    std::optional<int> evRangeEstKm;
    std::optional<double> fuelLitersRem;
    std::optional<int> fuelRangeEstKm;
    std::optional<int> totalRangeEstKm;
    std::optional<double> aux12vV;
    std::optional<double> ecu12vV;
    std::optional<double> odometerKm;
    std::optional<int> kmSinceDtcClear;
    std::optional<int> kmWithMilOn;
    std::optional<int> emotorRpm;
    std::optional<int> inverterTempC;
    std::optional<int> catTempC;
    std::optional<int> iceRuntimeSec;
    std::optional<int> aux12vSocPct;
    std::optional<int> systemTotalCv;
    std::optional<double> systemTotalKw;
    std::optional<double> fuelRailBar;
    std::optional<double> timingAdvanceDeg;
    std::optional<double> stftPct;
    std::optional<double> ltftPct;
    std::string dsgGear{"P/N"};
    std::string vin;
    std::optional<double> gpsLat;
    std::optional<double> gpsLon;
    int dtcCount{0};
    bool milOn{false};
    std::string dtcCodes{"NONE"};
    std::string iceWarmupStatus{"COLD"};
    double tripDistanceKm{0.0};
    double tripEvKm{0.0};
    double tripIceKm{0.0};
    double tripEvRatioPct{100.0};
    double tripEvKwhUsed{0.0};
    double tripRegenKwh{0.0};
    std::optional<double> tripEvKwh100km;
    double tripPeakDischargeKw{0.0};
    double tripPeakRegenKw{0.0};
    int tripMaxSpeedKmh{0};
    int tripAvgSpeedKmh{0};
    int tripDurationSec{0};
    double lifetimeTotalKm{0.0};
    double lifetimeEvKm{0.0};
    double lifetimeIceKm{0.0};
    double lifetimeEvKwh{0.0};
    double lifetimeRegenKwh{0.0};
    std::string lastTripJson{"null"};
};

void loadLifetimeStats(TelemetryState& st) {
    std::ifstream f(LIFETIME_STATS_PATH);
    if (!f.is_open()) {
        return;
    }
    f >> st.lifetimeTotalKm >> st.lifetimeEvKm >> st.lifetimeIceKm >> st.lifetimeEvKwh >> st.lifetimeRegenKwh;
}

void saveLifetimeStats(const TelemetryState& st) {
    std::ofstream f(LIFETIME_STATS_TMP, std::ios::out | std::ios::trunc);
    if (f.is_open()) {
        f << std::fixed << std::setprecision(3)
          << st.lifetimeTotalKm << " "
          << st.lifetimeEvKm << " "
          << st.lifetimeIceKm << " "
          << st.lifetimeEvKwh << " "
          << st.lifetimeRegenKwh << "\n";
        f.close();
        rename(LIFETIME_STATS_TMP, LIFETIME_STATS_PATH);
    }
}

void saveLastTripSummary(const TelemetryState& st) {
    if (st.tripDistanceKm < 0.05 && st.tripDurationSec < 30) {
        return;
    }
    std::ostringstream out;
    out << std::fixed;
    out << "{"
        << "\"ended_utc\":\"" << currentUtcIso8601() << "\","
        << "\"distance_km\":" << std::setprecision(2) << st.tripDistanceKm << ","
        << "\"ev_km\":" << std::setprecision(2) << st.tripEvKm << ","
        << "\"ice_km\":" << std::setprecision(2) << st.tripIceKm << ","
        << "\"duration_sec\":" << st.tripDurationSec << ","
        << "\"ev_ratio_pct\":" << std::setprecision(1) << st.tripEvRatioPct << ","
        << "\"ev_kwh_used\":" << std::setprecision(2) << st.tripEvKwhUsed << ","
        << "\"regen_kwh\":" << std::setprecision(2) << st.tripRegenKwh << ","
        << "\"ev_kwh_100km\":";
    if (st.tripEvKwh100km.has_value()) {
        out << std::setprecision(1) << *st.tripEvKwh100km;
    } else {
        out << "null";
    }
    out << ",\"max_speed_kmh\":" << st.tripMaxSpeedKmh << ","
        << "\"avg_speed_kmh\":" << st.tripAvgSpeedKmh << ","
        << "\"end_soc_pct\":";
    if (st.hvSocDisplayPct.has_value()) {
        out << std::setprecision(1) << *st.hvSocDisplayPct;
    } else {
        out << "null";
    }
    out << "}";

    std::ofstream f(LAST_TRIP_JSON_TMP, std::ios::out | std::ios::trunc);
    if (f.is_open()) {
        f << out.str() << "\n";
        f.close();
        rename(LAST_TRIP_JSON_TMP, LAST_TRIP_JSON_PATH);
    }
}

void writeTelemetrySnapshot(const TelemetryState& st) {
    std::ostringstream out;
    out << std::fixed;
    out << "{\n";
    out << "  \"timestamp_utc\": \"" << currentUtcIso8601() << "\",\n";
    out << "  \"adapter_connected\": " << (st.adapterConnected ? "true" : "false") << ",\n";
    out << "  \"adapter_name\": \"" << jsonEscape(st.adapterName) << "\",\n";
    out << "  \"adapter_mac\": \"" << jsonEscape(st.adapterMac) << "\",\n";
    out << "  \"ignition_on\": " << (st.ignitionOn ? "true" : "false") << ",\n";
    out << "  \"garage_triggered_by_obd\": " << (st.garageTriggeredByObd ? "true" : "false") << ",\n";
    out << "  \"drive_mode\": \"" << jsonEscape(st.driveMode) << "\",\n";
    out << "  \"speed_kmh\": " << st.speedKmh << ",\n";
    out << "  \"ice_rpm\": " << st.iceRpm << ",\n";
    out << "  \"dtc_count\": " << st.dtcCount << ",\n";
    out << "  \"mil_on\": " << (st.milOn ? "true" : "false") << ",\n";
    out << "  \"dtc_codes\": \"" << jsonEscape(st.dtcCodes) << "\",\n";
    out << "  \"ice_warmup_status\": \"" << jsonEscape(st.iceWarmupStatus) << "\",\n";
    out << "  \"dsg_gear\": \"" << jsonEscape(st.dsgGear) << "\",\n";
    out << "  \"vin\": \"" << jsonEscape(st.vin) << "\",\n";
    out << "  \"trip_distance_km\": " << std::setprecision(2) << st.tripDistanceKm << ",\n";
    out << "  \"trip_ev_km\": " << std::setprecision(2) << st.tripEvKm << ",\n";
    out << "  \"trip_ice_km\": " << std::setprecision(2) << st.tripIceKm << ",\n";
    out << "  \"trip_ev_ratio_pct\": " << std::setprecision(1) << st.tripEvRatioPct << ",\n";
    out << "  \"trip_ev_kwh_used\": " << std::setprecision(2) << st.tripEvKwhUsed << ",\n";
    out << "  \"trip_regen_kwh\": " << std::setprecision(2) << st.tripRegenKwh << ",\n";
    out << "  \"trip_peak_discharge_kw\": " << std::setprecision(1) << st.tripPeakDischargeKw << ",\n";
    out << "  \"trip_peak_regen_kw\": " << std::setprecision(1) << st.tripPeakRegenKw << ",\n";
    out << "  \"trip_max_speed_kmh\": " << st.tripMaxSpeedKmh << ",\n";
    out << "  \"trip_avg_speed_kmh\": " << st.tripAvgSpeedKmh << ",\n";
    out << "  \"trip_duration_sec\": " << st.tripDurationSec << ",\n";
    out << "  \"lifetime_total_km\": " << std::setprecision(1) << st.lifetimeTotalKm << ",\n";
    out << "  \"lifetime_ev_km\": " << std::setprecision(1) << st.lifetimeEvKm << ",\n";
    out << "  \"lifetime_ice_km\": " << std::setprecision(1) << st.lifetimeIceKm << ",\n";
    out << "  \"lifetime_ev_kwh\": " << std::setprecision(1) << st.lifetimeEvKwh << ",\n";
    out << "  \"lifetime_regen_kwh\": " << std::setprecision(1) << st.lifetimeRegenKwh << ",\n";

    auto writeOptInt = [&](const char* key, const std::optional<int>& val) {
        out << "  \"" << key << "\": ";
        if (val.has_value()) {
            out << *val;
        } else {
            out << "null";
        }
        out << ",\n";
    };

    auto writeOptDouble = [&](const char* key, const std::optional<double>& val, int decimals, bool trailingComma = true) {
        out << "  \"" << key << "\": ";
        if (val.has_value()) {
            out << std::setprecision(decimals) << *val;
        } else {
            out << "null";
        }
        out << (trailingComma ? ",\n" : "\n");
    };

    writeOptInt("ice_coolant_c", st.iceCoolantC);
    writeOptInt("ice_oil_c", st.iceOilC);
    writeOptInt("intake_air_c", st.intakeAirC);
    writeOptInt("ambient_c", st.ambientC);
    writeOptInt("dsg_temp_c", st.dsgTempC);
    writeOptInt("hv_cell_delta_mv", st.hvCellDeltaMv);
    writeOptInt("ev_range_est_km", st.evRangeEstKm);
    writeOptInt("fuel_range_est_km", st.fuelRangeEstKm);
    writeOptInt("total_range_est_km", st.totalRangeEstKm);
    writeOptInt("km_since_dtc_clear", st.kmSinceDtcClear);
    writeOptInt("km_with_mil_on", st.kmWithMilOn);
    writeOptInt("emotor_rpm", st.emotorRpm);
    writeOptInt("inverter_temp_c", st.inverterTempC);
    writeOptInt("cat_temp_c", st.catTempC);
    writeOptInt("ice_runtime_sec", st.iceRuntimeSec);
    writeOptInt("aux_12v_soc_pct", st.aux12vSocPct);
    writeOptInt("system_total_cv", st.systemTotalCv);
    writeOptDouble("system_total_kw", st.systemTotalKw, 1);
    writeOptDouble("fuel_rail_bar", st.fuelRailBar, 1);
    writeOptDouble("timing_advance_deg", st.timingAdvanceDeg, 1);
    writeOptDouble("stft_pct", st.stftPct, 1);
    writeOptDouble("ltft_pct", st.ltftPct, 1);
    writeOptDouble("ice_load_pct", st.iceLoadPct, 1);
    writeOptDouble("throttle_pct", st.throttlePct, 1);
    writeOptDouble("turbo_boost_bar", st.turboBoostBar, 2);
    writeOptDouble("fuel_level_pct", st.fuelLevelPct, 1);
    writeOptDouble("fuel_liters_rem", st.fuelLitersRem, 1);
    writeOptDouble("fuel_rate_lph", st.fuelRateLph, 2);
    writeOptDouble("fuel_l100km", st.fuelL100km, 1);
    writeOptDouble("hv_soc_display_pct", st.hvSocDisplayPct, 1);
    writeOptDouble("hv_soc_gross_pct", st.hvSocGrossPct, 1);
    writeOptDouble("hv_voltage_v", st.hvVoltageV, 1);
    writeOptDouble("hv_current_a", st.hvCurrentA, 1);
    writeOptDouble("hv_power_kw", st.hvPowerKw, 2);
    writeOptDouble("hv_temp_min_c", st.hvTempMinC, 1);
    writeOptDouble("hv_temp_max_c", st.hvTempMaxC, 1);
    writeOptDouble("hv_temp_spread_c", st.hvTempSpreadC, 1);
    writeOptDouble("hv_cell_min_v", st.hvCellMinV, 3);
    writeOptDouble("hv_cell_max_v", st.hvCellMaxV, 3);
    writeOptDouble("hv_soh_pct", st.hvSohPct, 1);
    writeOptDouble("hv_energy_rem_kwh", st.hvEnergyRemKwh, 2);
    writeOptDouble("trip_ev_kwh_100km", st.tripEvKwh100km, 1);
    writeOptDouble("ecu_12v_v", st.ecu12vV, 2);
    writeOptDouble("odometer_km", st.odometerKm, 1);
    writeOptDouble("gps_lat", st.gpsLat, 6);
    writeOptDouble("gps_lon", st.gpsLon, 6);
    writeOptDouble("aux_12v_v", st.aux12vV, 2);
    out << "  \"last_trip\": " << (st.lastTripJson.empty() ? "null" : st.lastTripJson) << "\n";
    out << "}\n";

    std::ofstream f(TELEMETRY_JSON_TMP, std::ios::out | std::ios::trunc);
    if (f.is_open()) {
        f << out.str();
        f.close();
        rename(TELEMETRY_JSON_TMP, TELEMETRY_JSON_PATH);
    }
}

std::string getConfiguredObdMac() {
    const char* envMac = getenv("AAWG_OBD_BLE_MAC");
    if (envMac && envMac[0] != '\0') {
        return toUpper(trim(envMac));
    }
    std::string runMac = readFirstLine(RUN_OBD_MAC_PATH);
    if (!runMac.empty()) {
        return toUpper(runMac);
    }
    std::string persistMac = readFirstLine(PERSIST_OBD_MAC_PATH);
    if (!persistMac.empty()) {
        return toUpper(persistMac);
    }
    return "";
}

bool shouldFireGarageOnObdStart() {
    return (access(GARAGE_HOLD_FLAG, F_OK) == 0) &&
           (access(GARAGE_OPENED_FLAG, F_OK) != 0);
}

void fireGarageOpenerProcess() {
    if (access(GARAGE_INIT_SCRIPT, X_OK) != 0) {
        Logger::instance()->info("OBD Telemetry: %s not executable, skipping garage trigger\n", GARAGE_INIT_SCRIPT);
        return;
    }

    Logger::instance()->info("OBD Telemetry: Firing %s fire on Car START detection\n", GARAGE_INIT_SCRIPT);
    pid_t pid = fork();
    if (pid == 0) {
        int maxFd = static_cast<int>(sysconf(_SC_OPEN_MAX));
        if (maxFd < 0 || maxFd > 1024) {
            maxFd = 256;
        }
        for (int fd = 3; fd < maxFd; ++fd) {
            close(fd);
        }
        execl(GARAGE_INIT_SCRIPT, GARAGE_INIT_SCRIPT, "fire", static_cast<char*>(nullptr));
        _exit(127);
    } else if (pid > 0) {
        int status = 0;
        for (int i = 0; i < 160; ++i) {
            pid_t ret = waitpid(pid, &status, WNOHANG);
            if (ret == pid || ret < 0) {
                Logger::instance()->info("OBD Telemetry: Garage opener completed (status=%d)\n", status);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        Logger::instance()->info("OBD Telemetry: Garage opener timed out after 16s; killing child pid %d\n", static_cast<int>(pid));
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

struct DiscoveredObdDevice {
    std::string objectPath;
    std::string address;
    std::string name;
    int score{0};
};

// Inspect BlueZ ManagedObjects to find the Vgate iCar Pro 2S BLE endpoint ("IOS-Vlink" / 18f0 / fff0)
std::optional<DiscoveredObdDevice> findObdBleDeviceInObjects(
    const DBus::ManagedObjects& objects,
    const std::string& configuredMac) {

    std::optional<DiscoveredObdDevice> best;

    for (auto const& [path, interfaces] : objects) {
        auto devIt = interfaces.find(INTERFACE_BLUEZ_DEVICE);
        if (devIt == interfaces.end()) {
            continue;
        }
        const DBus::Properties& props = devIt->second;

        std::string addr;
        if (auto it = props.find("Address"); it != props.end()) {
            try { addr = toUpper(it->second.to_string()); } catch (...) {}
        }
        std::string name;
        if (auto it = props.find("Name"); it != props.end()) {
            try { name = it->second.to_string(); } catch (...) {}
        }
        if (name.empty()) {
            if (auto it = props.find("Alias"); it != props.end()) {
                try { name = it->second.to_string(); } catch (...) {}
            }
        }

        std::vector<std::string> uuids;
        if (auto it = props.find("UUIDs"); it != props.end()) {
            try {
                DBus::Variant v = it->second;
                uuids = v.to_vector<std::string>();
            } catch (...) {}
        }

        std::string lowerName = toLower(name);

        // Never match the user's phone or audio/AA devices unless explicitly configured by MAC
        bool isPhoneOrAudio = false;
        bool hasBleObdUuid = false;
        for (const std::string& u : uuids) {
            std::string lu = toLower(u);
            if (lu.find("4de17a00-52cb-11e6-bdf4-0800200c9a66") != std::string::npos ||
                lu.find("0000110a-") != std::string::npos ||
                lu.find("0000110b-") != std::string::npos ||
                lu.find("00001112-") != std::string::npos ||
                lu.find("0000111e-") != std::string::npos ||
                lu.find("0000111f-") != std::string::npos) {
                isPhoneOrAudio = true;
            }
            if (lu.find("000018f0-") != std::string::npos ||
                lu.find("0000fff0-") != std::string::npos ||
                lu.find("e7810a71-73ae-499d-8c15-faa9aef0c3f2") != std::string::npos) {
                hasBleObdUuid = true;
            }
        }

        int score = 0;
        if (!configuredMac.empty() && addr == configuredMac) {
            score = 1000;
        } else if (!isPhoneOrAudio) {
            // Vgate iCar Pro 2S dual-mode broadcasts "IOS-Vlink" for BLE GATT (no PIN)
            // and "Android-Vlink" for Classic SPP (requires PIN 1234). Always prefer BLE!
            if (lowerName.find("android-vlink") != std::string::npos && !hasBleObdUuid) {
                continue;
            }
            if (lowerName.find("ios-vlink") != std::string::npos ||
                lowerName.find("ios_vlink") != std::string::npos) {
                score += 100;
            } else if (lowerName.find("vlink") != std::string::npos ||
                       lowerName.find("v-link") != std::string::npos ||
                       lowerName.find("vgate") != std::string::npos ||
                       lowerName.find("icar") != std::string::npos ||
                       lowerName.find("obd") != std::string::npos) {
                score += 60;
            }
            if (hasBleObdUuid) {
                score += 80;
            }
        }

        if (score > 0 && (!best.has_value() || score > best->score)) {
            DiscoveredObdDevice cand;
            cand.objectPath = path;
            cand.address = addr;
            cand.name = name.empty() ? "Vgate iCar Pro 2S" : name;
            cand.score = score;
            best = cand;
        }
    }

    return best;
}

struct GattEndpoints {
    std::string rxPath;
    std::string txPath;
    std::string rxUuid;
    std::string txUuid;
};

std::optional<GattEndpoints> findGattEndpoints(
    const DBus::ManagedObjects& objects,
    const std::string& devicePath) {

    std::string bestRxPath, bestTxPath, bestRxUuid, bestTxUuid;
    int bestRxScore = 0;
    int bestTxScore = 0;

    const std::string prefix = devicePath + "/";

    for (auto const& [path, interfaces] : objects) {
        if (path.rfind(prefix, 0) != 0) {
            continue;
        }
        auto chrcIt = interfaces.find(INTERFACE_GATT_CHRC);
        if (chrcIt == interfaces.end()) {
            continue;
        }
        const DBus::Properties& props = chrcIt->second;

        std::string uuid;
        if (auto it = props.find("UUID"); it != props.end()) {
            try { uuid = toLower(it->second.to_string()); } catch (...) {}
        }

        std::vector<std::string> flags;
        if (auto it = props.find("Flags"); it != props.end()) {
            try {
                DBus::Variant v = it->second;
                flags = v.to_vector<std::string>();
            } catch (...) {}
        }

        bool canNotify = false;
        bool canWrite = false;
        for (const std::string& f : flags) {
            std::string lf = toLower(f);
            if (lf == "notify" || lf == "indicate") {
                canNotify = true;
            }
            if (lf == "write" || lf == "write-without-response") {
                canWrite = true;
            }
        }

        // Skip standard GAP (2a00..2a05) and GATT (2a05) system characteristics
        if (uuid.find("00002a00-") == 0 ||
            uuid.find("00002a01-") == 0 ||
            uuid.find("00002a04-") == 0 ||
            uuid.find("00002a05-") == 0) {
            continue;
        }

        if (canNotify) {
            int rxScore = 10;
            if (uuid.find("00002af0-") == 0) {
                rxScore = 100; // Vgate iCar Pro 2S / 4.0 primary RX notify characteristic
            } else if (uuid.find("0000fff1-") == 0) {
                rxScore = 80;
            } else if (uuid.find("bef8d6c9-") == 0) {
                rxScore = 70;
            }
            if (rxScore > bestRxScore) {
                bestRxScore = rxScore;
                bestRxPath = path;
                bestRxUuid = uuid;
            }
        }

        if (canWrite) {
            int txScore = 10;
            if (uuid.find("00002af1-") == 0) {
                txScore = 100; // Vgate iCar Pro 2S / 4.0 primary TX write characteristic
            } else if (uuid.find("0000fff2-") == 0) {
                txScore = 80;
            } else if (uuid.find("00002af0-") == 0 || uuid.find("0000fff1-") == 0) {
                txScore = 40; // Combined RX/TX characteristic fallback
            }
            if (txScore > bestTxScore) {
                bestTxScore = txScore;
                bestTxPath = path;
                bestTxUuid = uuid;
            }
        }
    }

    if (!bestRxPath.empty() && !bestTxPath.empty()) {
        GattEndpoints ep;
        ep.rxPath = bestRxPath;
        ep.txPath = bestTxPath;
        ep.rxUuid = bestRxUuid;
        ep.txUuid = bestTxUuid;
        return ep;
    }
    return std::nullopt;
}

class BleElmSession {
public:
    BleElmSession(std::shared_ptr<DBus::Connection> conn, const GattEndpoints& ep)
        : m_conn(conn), m_ep(ep) {}

    ~BleElmSession() {
        closeSession();
    }

    bool openSession() {
        m_rxObj = m_conn->create_object_proxy(BLUEZ_BUS_NAME, m_ep.rxPath);
        m_txObj = m_conn->create_object_proxy(BLUEZ_BUS_NAME, m_ep.txPath);
        m_writeValue = m_txObj->create_method<void(std::vector<uint8_t>, DBus::Properties)>(
            INTERFACE_GATT_CHRC, "WriteValue");

        // Try AcquireNotify first for direct low-overhead socketpair reads
        try {
            auto acquireNotify = m_rxObj->create_method<
                DBus::MultipleReturn<std::shared_ptr<DBus::FileDescriptor>, uint16_t>(DBus::Properties)>(
                INTERFACE_GATT_CHRC, "AcquireNotify");
            auto ret = (*acquireNotify)(DBus::Properties{});
            m_notifyFdObj = std::get<0>(ret.m_data);
            if (m_notifyFdObj && m_notifyFdObj->descriptor() >= 0) {
                m_notifyFd = m_notifyFdObj->descriptor();
                Logger::instance()->info("OBD Telemetry: Acquired GATT Notify fd=%d on %s\n",
                                         m_notifyFd, m_ep.rxUuid.c_str());
                return true;
            }
        } catch (DBus::Error& e) {
            Logger::instance()->info("OBD Telemetry: AcquireNotify unavailable (%s), using StartNotify signal fallback\n",
                                     e.what());
        }

        // Fallback to StartNotify + PropertyProxy("Value") change signal
        try {
            m_startNotify = m_rxObj->create_method<void()>(INTERFACE_GATT_CHRC, "StartNotify");
            m_stopNotify = m_rxObj->create_method<void()>(INTERFACE_GATT_CHRC, "StopNotify");
            m_rxValueProp = m_rxObj->create_property<std::vector<uint8_t>>(INTERFACE_GATT_CHRC, "Value");
            m_rxValueProp->signal_property_changed().connect(
                sigc::mem_fun(*this, &BleElmSession::onRxValueChanged));
            (*m_startNotify)();
            m_usingStartNotify = true;
            Logger::instance()->info("OBD Telemetry: Started GATT Notify on %s\n", m_ep.rxUuid.c_str());
            return true;
        } catch (DBus::Error& e) {
            Logger::instance()->info("OBD Telemetry: StartNotify failed on %s: %s\n",
                                     m_ep.rxUuid.c_str(), e.what());
            return false;
        }
    }

    void onRxValueChanged(std::vector<uint8_t> bytes) {
        std::lock_guard<std::mutex> lock(m_rxMutex);
        m_rxBuffer.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        m_rxCv.notify_all();
    }

    void closeSession() {
        if (m_usingStartNotify && m_stopNotify) {
            try { (*m_stopNotify)(); } catch (...) {}
            m_usingStartNotify = false;
        }
        m_notifyFd = -1;
        m_notifyFdObj.reset();
    }

    std::string sendCommand(const std::string& cmd, int timeoutMs = 1200) {
        if (m_notifyFd >= 0) {
            // Drain any stale bytes before sending new command
            struct pollfd pfd = {m_notifyFd, POLLIN, 0};
            char drain[256];
            while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                if (read(m_notifyFd, drain, sizeof(drain)) <= 0) {
                    break;
                }
            }
        } else {
            std::lock_guard<std::mutex> lock(m_rxMutex);
            m_rxBuffer.clear();
        }

        std::string payload = cmd + "\r";
        std::vector<uint8_t> bytes(payload.begin(), payload.end());
        try {
            (*m_writeValue)(bytes, DBus::Properties{});
        } catch (DBus::Error& e) {
            Logger::instance()->info("OBD Telemetry: WriteValue('%s') failed: %s\n", cmd.c_str(), e.what());
            return "";
        }

        std::string response;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

        if (m_notifyFd >= 0) {
            while (std::chrono::steady_clock::now() < deadline && !g_shouldExit.load()) {
                auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remainingMs <= 0) {
                    break;
                }
                struct pollfd pfd = {m_notifyFd, POLLIN, 0};
                int pr = poll(&pfd, 1, static_cast<int>(remainingMs));
                if (pr > 0 && (pfd.revents & POLLIN)) {
                    char buf[256];
                    ssize_t n = read(m_notifyFd, buf, sizeof(buf));
                    if (n <= 0) {
                        break;
                    }
                    response.append(buf, static_cast<size_t>(n));
                    if (response.find('>') != std::string::npos) {
                        break;
                    }
                } else if (pr <= 0) {
                    break;
                }
            }
        } else {
            std::unique_lock<std::mutex> lock(m_rxMutex);
            while (std::chrono::steady_clock::now() < deadline && !g_shouldExit.load()) {
                if (m_rxBuffer.find('>') != std::string::npos) {
                    break;
                }
                if (m_rxCv.wait_until(lock, deadline) == std::cv_status::timeout) {
                    break;
                }
            }
            response = m_rxBuffer;
            m_rxBuffer.clear();
        }

        // Normalize response: strip prompt '>', CR, LF
        for (char& c : response) {
            if (c == '>' || c == '\r' || c == '\n') {
                c = ' ';
            }
        }
        return trim(response);
    }

private:
    std::shared_ptr<DBus::Connection> m_conn;
    GattEndpoints m_ep;
    std::shared_ptr<DBus::ObjectProxy> m_rxObj;
    std::shared_ptr<DBus::ObjectProxy> m_txObj;
    std::shared_ptr<DBus::MethodProxy<void(std::vector<uint8_t>, DBus::Properties)>> m_writeValue;
    std::shared_ptr<DBus::MethodProxy<void()>> m_startNotify;
    std::shared_ptr<DBus::MethodProxy<void()>> m_stopNotify;
    std::shared_ptr<DBus::PropertyProxy<std::vector<uint8_t>>> m_rxValueProp;
    std::shared_ptr<DBus::FileDescriptor> m_notifyFdObj;
    int m_notifyFd{-1};
    bool m_usingStartNotify{false};
    std::mutex m_rxMutex;
    std::condition_variable m_rxCv;
    std::string m_rxBuffer;
};

// Extract contiguous uppercase hex digits after a given response prefix (e.g. "410C" or "621E3B")
std::optional<std::string> extractHexPayload(const std::string& raw, const std::string& prefix) {
    std::string compact;
    compact.reserve(raw.size());
    for (unsigned char c : raw) {
        if (isxdigit(c)) {
            compact.push_back(static_cast<char>(toupper(c)));
        }
    }
    size_t pos = compact.find(prefix);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    return compact.substr(pos + prefix.size());
}

std::optional<int> parseHexByte(const std::string& hexStr, size_t byteIndex) {
    size_t offset = byteIndex * 2;
    if (hexStr.size() < offset + 2) {
        return std::nullopt;
    }
    char buf[3] = {hexStr[offset], hexStr[offset + 1], '\0'};
    if (!isxdigit(static_cast<unsigned char>(buf[0])) ||
        !isxdigit(static_cast<unsigned char>(buf[1]))) {
        return std::nullopt;
    }
    return static_cast<int>(strtol(buf, nullptr, 16));
}

std::optional<int> parseHexWordU16(const std::string& hexStr, size_t byteIndex = 0) {
    auto b0 = parseHexByte(hexStr, byteIndex);
    auto b1 = parseHexByte(hexStr, byteIndex + 1);
    if (!b0 || !b1) {
        return std::nullopt;
    }
    return (*b0 << 8) | *b1;
}

std::optional<double> parseAtrvVoltage(const std::string& raw) {
    std::string cleaned;
    for (unsigned char c : raw) {
        if (isdigit(c) || c == '.') {
            cleaned.push_back(static_cast<char>(c));
        } else if (!cleaned.empty()) {
            break;
        }
    }
    if (cleaned.empty()) {
        return std::nullopt;
    }
    double v = atof(cleaned.c_str());
    if (v >= 6.0 && v <= 20.0) {
        return v;
    }
    return std::nullopt;
}

// Decode Mode 03 response ("43 ...") into standard ISO 15031-6 DTC strings (P0xxx, C0xxx, B0xxx, U0xxx)
std::string decodeMode03Dtcs(const std::string& raw) {
    auto hex = extractHexPayload(raw, "43");
    if (!hex.has_value() || hex->size() < 4) {
        return "NONE";
    }
    static const char kSysPrefix[4] = {'P', 'C', 'B', 'U'};
    std::vector<std::string> codes;
    // Skip byte 0 if it is a DTC count byte (ISO 15765-4 CAN Mode 03 format: 43 <count> <DTC1_H> <DTC1_L>...)
    size_t offset = ((hex->size() % 4) == 2) ? 1 : 0;
    while (offset * 2 + 4 <= hex->size()) {
        auto b0 = parseHexByte(*hex, offset);
        auto b1 = parseHexByte(*hex, offset + 1);
        offset += 2;
        if (!b0 || !b1 || (*b0 == 0 && *b1 == 0)) {
            continue;
        }
        char code[8];
        char sys = kSysPrefix[(*b0 >> 6) & 0x03];
        int d1 = (*b0 >> 4) & 0x03;
        int d2 = (*b0) & 0x0F;
        snprintf(code, sizeof(code), "%c%d%X%02X", sys, d1, d2, *b1);
        codes.push_back(code);
        if (codes.size() >= 5) {
            break;
        }
    }
    if (codes.empty()) {
        return "NONE";
    }
    std::ostringstream ss;
    for (size_t i = 0; i < codes.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << codes[i];
    }
    return ss.str();
}

// Decode Mode 09 PID 02 response ("49 02 ...") into 17-character ASCII VIN (e.g. WVWZZZ3CZ...)
std::string decodeMode09Vin(const std::string& raw) {
    auto hex = extractHexPayload(raw, "4902");
    if (!hex.has_value()) {
        return "";
    }
    std::string vin;
    for (size_t i = 0; i * 2 + 2 <= hex->size(); ++i) {
        auto b = parseHexByte(*hex, i);
        if (!b) continue;
        char c = static_cast<char>(*b);
        if (isalnum(static_cast<unsigned char>(c)) && c != 'I' && c != 'O' && c != 'Q') {
            vin.push_back(static_cast<char>(toupper(static_cast<unsigned char>(c))));
        }
    }
    if (vin.size() >= 17) {
        return vin.substr(vin.size() - 17);
    }
    return "";
}

// Estimate active DQ400e 6-speed DSG gear (D1..D6) from vehicle speed and shaft RPM
std::string estimateDq400eGear(int speedKmh, int rpm) {
    if (speedKmh <= 1) {
        return (rpm > 0) ? "P/N (IDLE)" : "READY (STILL)";
    }
    int refRpm = (rpm > 400) ? rpm : (speedKmh * 42);
    double kmhPer1000Rpm = (static_cast<double>(speedKmh) * 1000.0) / std::max(500, refRpm);
    if (kmhPer1000Rpm < 11.5) return "D1";
    if (kmhPer1000Rpm < 18.5) return "D2";
    if (kmhPer1000Rpm < 26.5) return "D3";
    if (kmhPer1000Rpm < 35.0) return "D4";
    if (kmhPer1000Rpm < 43.5) return "D5";
    return "D6";
}

} // namespace

int main() {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, handleSignal);
    signal(SIGINT, handleSignal);

    Logger::instance()->info("OBD Telemetry: Starting Passat GTE OBD-II BLE daemon (gte_telemetryd)\n");

    TelemetryState state;
    state.adapterMac = getConfiguredObdMac();
    loadLifetimeStats(state);
    std::string savedLastTrip = readFirstLine(LAST_TRIP_JSON_PATH);
    if (!savedLastTrip.empty() && savedLastTrip.front() == '{') {
        state.lastTripJson = savedLastTrip;
    }
    state.driveMode = "WAITING FOR BLUETOOTH...";
    writeTelemetrySnapshot(state);

    auto dispatcher = DBus::StandaloneDispatcher::create();
    auto conn = dispatcher->create_connection(DBus::BusType::SYSTEM);

    auto rootObj = conn->create_object_proxy(BLUEZ_BUS_NAME, BLUEZ_ROOT_PATH);
    auto getManagedObjects = rootObj->create_method<DBus::ManagedObjects(void)>(
        INTERFACE_OBJECT_MANAGER, "GetManagedObjects");

    // Wait briefly at boot so aawgd can register its profiles first; if we are in the
    // garage waiting for the car START button, begin looking after 3s (or immediately
    // once TCP :5288 is active).
    // While S43garage_gate holds the boot, aawgd is not running yet: start immediately.
    for (int i = 0; i < 6 && !g_shouldExit.load(); ++i) {
        if (isTcpPortEstablished(5288) || access(GARAGE_HOLD_FLAG, F_OK) == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    while (!g_shouldExit.load()) {
        // Do not contend with 2.4 GHz Wi-Fi while S38garage_opener fire is running
        if (access(GARAGE_FIRING_FLAG, F_OK) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        std::string configuredMac = getConfiguredObdMac();
        state.adapterConnected = false;
        state.adapterMac = configuredMac;
        state.driveMode = configuredMac.empty() ? "SCANNING FOR VGATE BLE..." : "CONNECTING TO VGATE BLE...";
        writeTelemetrySnapshot(state);

        DBus::ManagedObjects objects;
        try {
            objects = (*getManagedObjects)();
        } catch (DBus::Error& e) {
            Logger::instance()->info("OBD Telemetry: GetManagedObjects failed: %s\n", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        // Locate hci0 adapter
        std::string adapterPath;
        for (auto const& [path, interfaces] : objects) {
            if (interfaces.find(INTERFACE_BLUEZ_ADAPTER) != interfaces.end()) {
                adapterPath = path;
                break;
            }
        }
        if (adapterPath.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        auto adapterObj = conn->create_object_proxy(BLUEZ_BUS_NAME, adapterPath);
        auto adapterPowered = adapterObj->create_property<bool>(INTERFACE_BLUEZ_ADAPTER, "Powered");
        try {
            if (adapterPowered && !adapterPowered->value()) {
                adapterPowered->set_value(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        } catch (...) {}

        auto obdDev = findObdBleDeviceInObjects(objects, configuredMac);
        if (!obdDev.has_value()) {
            // Run a bounded 4-second BLE-only ("Transport": "le") scan to discover IOS-Vlink
            try {
                auto setDiscoveryFilter = adapterObj->create_method<void(DBus::Properties)>(
                    INTERFACE_BLUEZ_ADAPTER, "SetDiscoveryFilter");
                auto startDiscovery = adapterObj->create_method<void()>(
                    INTERFACE_BLUEZ_ADAPTER, "StartDiscovery");
                auto stopDiscovery = adapterObj->create_method<void()>(
                    INTERFACE_BLUEZ_ADAPTER, "StopDiscovery");

                DBus::Properties filter;
                filter["Transport"] = DBus::Variant(std::string("le"));
                (*setDiscoveryFilter)(filter);
                (*startDiscovery)();

                for (int tick = 0; tick < 8 && !g_shouldExit.load(); ++tick) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    objects = (*getManagedObjects)();
                    obdDev = findObdBleDeviceInObjects(objects, configuredMac);
                    if (obdDev.has_value()) {
                        break;
                    }
                }
                try { (*stopDiscovery)(); } catch (...) {}
            } catch (DBus::Error& e) {
                Logger::instance()->info("OBD Telemetry: BLE discovery error: %s\n", e.what());
            }
        }

        if (!obdDev.has_value()) {
            state.driveMode = "VGATE BLE NOT IN RANGE";
            writeTelemetrySnapshot(state);
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        // Persist discovered BLE MAC so future boots connect directly without scanning
        state.adapterName = obdDev->name;
        state.adapterMac = obdDev->address;
        if (!obdDev->address.empty()) {
            writeTextFile(RUN_OBD_MAC_PATH, obdDev->address);
            if (readFirstLine(PERSIST_OBD_MAC_PATH) != obdDev->address) {
                writeTextFile(PERSIST_OBD_MAC_PATH, obdDev->address);
                Logger::instance()->info("OBD Telemetry: Cached OBD BLE adapter %s (%s) to %s\n",
                                         obdDev->name.c_str(), obdDev->address.c_str(), PERSIST_OBD_MAC_PATH);
            }
        }

        auto devObj = conn->create_object_proxy(BLUEZ_BUS_NAME, obdDev->objectPath);
        auto devConnect = devObj->create_method<void()>(INTERFACE_BLUEZ_DEVICE, "Connect");
        auto devDisconnect = devObj->create_method<void()>(INTERFACE_BLUEZ_DEVICE, "Disconnect");
        auto devConnected = devObj->create_property<bool>(INTERFACE_BLUEZ_DEVICE, "Connected");
        auto devServicesResolved = devObj->create_property<bool>(INTERFACE_BLUEZ_DEVICE, "ServicesResolved");
        auto devTrusted = devObj->create_property<bool>(INTERFACE_BLUEZ_DEVICE, "Trusted");

        try {
            if (devTrusted && !devTrusted->value()) {
                devTrusted->set_value(true);
            }
            if (!devConnected || !devConnected->value()) {
                Logger::instance()->info("OBD Telemetry: Connecting to %s (%s) at %s\n",
                                         obdDev->name.c_str(), obdDev->address.c_str(), obdDev->objectPath.c_str());
                (*devConnect)();
            }
        } catch (DBus::Error& e) {
            Logger::instance()->info("OBD Telemetry: Connect to %s failed: %s\n",
                                     obdDev->address.c_str(), e.what());
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        // Wait up to 8s for GATT ServicesResolved
        std::optional<GattEndpoints> gattEp;
        for (int waitTick = 0; waitTick < 32 && !g_shouldExit.load(); ++waitTick) {
            try {
                objects = (*getManagedObjects)();
                gattEp = findGattEndpoints(objects, obdDev->objectPath);
                if (gattEp.has_value()) {
                    break;
                }
                if (devServicesResolved && devServicesResolved->value()) {
                    gattEp = findGattEndpoints(objects, obdDev->objectPath);
                    break;
                }
            } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        if (!gattEp.has_value()) {
            Logger::instance()->info("OBD Telemetry: GATT characteristics (2af0/2af1 or fff1/fff2) not found on %s\n",
                                     obdDev->objectPath.c_str());
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        BleElmSession elm(conn, *gattEp);
        if (!elm.openSession()) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        // ELM327 initialization sequence for Passat B8 GTE (ISO 15765-4 CAN 11-bit 500 kbps)
        elm.sendCommand("ATZ", 1500);
        elm.sendCommand("ATE0", 600);
        elm.sendCommand("ATL0", 600);
        elm.sendCommand("ATS0", 600);
        elm.sendCommand("ATH0", 600);
        elm.sendCommand("ATSP6", 600);
        elm.sendCommand("ATAT1", 600);
        elm.sendCommand("ATST32", 600);

        state.adapterConnected = true;
        state.driveMode = "STANDBY (PRESS START)";
        writeTelemetrySnapshot(state);
        Logger::instance()->info("OBD Telemetry: Connected and initialized ELM327 on %s (%s)\n",
                                 state.adapterName.c_str(), state.adapterMac.c_str());

        std::optional<double> baselineResting12v;
        std::optional<int> baroKpa;
        int consecutiveNoDataAfterRun = 0;
        bool wasIgnitionOnThisSession = false;
        uint64_t cycleCount = 0;
        uint64_t evSamples = 0;
        uint64_t totalActiveSamples = 0;
        auto lastSampleTime = std::chrono::steady_clock::now();
        auto tripStartTime = std::chrono::steady_clock::now();

        while (!g_shouldExit.load()) {
            if (access(GARAGE_FIRING_FLAG, F_OK) == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            try {
                if (devConnected && !devConnected->value()) {
                    Logger::instance()->info("OBD Telemetry: BLE link disconnected\n");
                    break;
                }
            } catch (...) {
                break;
            }

            // 1. Poll 12V bus voltage (ATRV)
            std::string atrvRaw = elm.sendCommand("ATRV", 800);
            if (atrvRaw.empty()) {
                // Communication lost with BLE GATT endpoint
                break;
            }
            state.aux12vV = parseAtrvVoltage(atrvRaw);

            // 2. Poll Mode 01 PID 0C (Engine RPM & Terminal 15 Ignition ON detection)
            // On VW MQB (Passat B8 GTE), when the car is only unlocked with the keyfob
            // (Klemme 30G), Terminal 15 (Klemme 15) is OFF and 010C returns "NO DATA".
            // The instant the driver presses the START button (entering Ignition ON or
            // GTE READY in E-Mode/Hybrid), Terminal 15 turns ON and 010C immediately
            // responds with "410Cxxxx" (410C0000 in 100% electric E-Mode, >0 if 1.4 TSI runs).
            std::string rpmRaw = elm.sendCommand("010C", 900);
            auto rpmHex = extractHexPayload(rpmRaw, "410C");

            bool ignitionDetectedNow = false;
            if (rpmHex.has_value()) {
                if (auto rawRpm = parseHexWordU16(*rpmHex, 0)) {
                    state.iceRpm = *rawRpm / 4;
                    ignitionDetectedNow = true;
                }
            } else {
                state.iceRpm = 0;
                // Record resting 12V battery voltage while Terminal 15 is OFF
                if (!baselineResting12v.has_value() && state.aux12vV.has_value() && *state.aux12vV < 13.0) {
                    baselineResting12v = *state.aux12vV;
                }
                // Secondary fallback: if HV-to-12V DC-DC converter jumped the 12V rail
                // to >= 13.5V (or +0.65V above resting baseline), the GTE HV contactor is READY
                if (state.aux12vV.has_value() &&
                    (*state.aux12vV >= 13.55 ||
                     (baselineResting12v.has_value() && *state.aux12vV >= *baselineResting12v + 0.65 && *state.aux12vV >= 13.3))) {
                    ignitionDetectedNow = true;
                }
            }

            state.ignitionOn = ignitionDetectedNow;

            // 3. Fire Garage Door Opener immediately when START button is detected in garage!
            if (state.ignitionOn && !state.garageTriggeredByObd && shouldFireGarageOnObdStart()) {
                state.garageTriggeredByObd = true;
                state.driveMode = (state.iceRpm > 0) ? "HYBRID (1.4 TSI)" : "READY (E-MODE)";
                writeTelemetrySnapshot(state);

                Logger::instance()->info(
                    "OBD Telemetry: Car START detected in garage (12V=%.2fV, RPM=%d); triggering garage door opener!\n",
                    state.aux12vV.value_or(0.0), state.iceRpm);

                // Pause BLE traffic while S38garage_opener fire uses the shared 2.4 GHz radio
                fireGarageOpenerProcess();
            }

            if (state.ignitionOn) {
                auto now = std::chrono::steady_clock::now();
                if (!wasIgnitionOnThisSession) {
                    tripStartTime = now;
                    lastSampleTime = now;
                }
                double dtHours = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSampleTime).count() / 3600000.0;
                lastSampleTime = now;
                state.tripDurationSec = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::seconds>(now - tripStartTime).count());

                wasIgnitionOnThisSession = true;
                consecutiveNoDataAfterRun = 0;

                // Poll Vehicle Speed (010D)
                if (auto spdHex = extractHexPayload(elm.sendCommand("010D", 700), "410D")) {
                    if (auto spd = parseHexByte(*spdHex, 0)) {
                        state.speedKmh = *spd;
                        if (state.speedKmh > state.tripMaxSpeedKmh) {
                            state.tripMaxSpeedKmh = state.speedKmh;
                        }
                    }
                }

                // Poll Throttle Pedal Position (0111) & Engine Load (0104)
                if (auto thrHex = extractHexPayload(elm.sendCommand("0111", 600), "4111")) {
                    if (auto b = parseHexByte(*thrHex, 0)) {
                        state.throttlePct = (*b * 100.0) / 255.0;
                    }
                }
                if (state.iceRpm > 0) {
                    if (auto loadHex = extractHexPayload(elm.sendCommand("0104", 600), "4104")) {
                        if (auto b = parseHexByte(*loadHex, 0)) {
                            state.iceLoadPct = (*b * 100.0) / 255.0;
                        }
                    }
                    // Poll Manifold Absolute Pressure (010B) -> Turbo Boost bar
                    if (auto mapHex = extractHexPayload(elm.sendCommand("010B", 600), "410B")) {
                        if (auto mapKpa = parseHexByte(*mapHex, 0)) {
                            int refBaro = baroKpa.value_or(100);
                            state.turboBoostBar = (*mapKpa - refBaro) / 100.0;
                        }
                    }
                    // Poll Instantaneous Fuel Rate (015E)
                    if (auto frHex = extractHexPayload(elm.sendCommand("015E", 600), "415E")) {
                        if (auto w = parseHexWordU16(*frHex, 0)) {
                            double lph = *w / 20.0;
                            state.fuelRateLph = lph;
                            if (state.speedKmh > 3) {
                                state.fuelL100km = (lph * 100.0) / state.speedKmh;
                            } else {
                                state.fuelL100km = std::nullopt;
                            }
                        }
                    }
                } else {
                    state.iceLoadPct = 0.0;
                    state.turboBoostBar = 0.0;
                    state.fuelRateLph = 0.0;
                    state.fuelL100km = 0.0;
                }

                // Poll Displayed HV Battery SoC (015B)
                if (auto socHex = extractHexPayload(elm.sendCommand("015B", 700), "415B")) {
                    if (auto soc = parseHexByte(*socHex, 0)) {
                        state.hvSocDisplayPct = (*soc * 100.0) / 255.0;
                    }
                }

                // Compute DQ400e gear, E-machine RPM (HEM80), and combined GTE system output (up to 160 kW / 218 CV)
                state.dsgGear = estimateDq400eGear(state.speedKmh, state.iceRpm);
                if (state.iceRpm > 0) {
                    state.emotorRpm = state.iceRpm;
                } else {
                    state.emotorRpm = (state.speedKmh > 0) ? (state.speedKmh * 48) : 0;
                }
                {
                    double evKw = 0.0;
                    if (state.hvPowerKw.has_value() && *state.hvPowerKw < 0.0) {
                        evKw = std::min(85.0, (-*state.hvPowerKw) * 0.93);
                    }
                    double iceKw = 0.0;
                    if (state.iceRpm > 500) {
                        double loadFrac = state.iceLoadPct.value_or(20.0) / 100.0;
                        double rpmFrac = std::min(1.0, state.iceRpm / 5500.0);
                        iceKw = std::min(115.0, 115.0 * loadFrac * rpmFrac);
                    }
                    double combKw = std::min(160.0, evKw + iceKw);
                    state.systemTotalKw = combKw;
                    state.systemTotalCv = static_cast<int>(std::round(combKw * 1.35962));
                }

                // Integrate trip & lifetime distance (split by Electric E-Mode vs Hybrid ICE) and energy
                if (dtHours > 0.0 && dtHours < 0.02) {
                    double dKm = state.speedKmh * dtHours;
                    state.tripDistanceKm += dKm;
                    state.lifetimeTotalKm += dKm;
                    if (state.iceRpm == 0) {
                        state.tripEvKm += dKm;
                        state.lifetimeEvKm += dKm;
                    } else {
                        state.tripIceKm += dKm;
                        state.lifetimeIceKm += dKm;
                    }
                    if (state.hvPowerKw.has_value()) {
                        if (*state.hvPowerKw < 0.0) {
                            double dKwh = (-*state.hvPowerKw) * dtHours;
                            state.tripEvKwhUsed += dKwh;
                            state.lifetimeEvKwh += dKwh;
                        } else {
                            double dRegen = (*state.hvPowerKw) * dtHours;
                            state.tripRegenKwh += dRegen;
                            state.lifetimeRegenKwh += dRegen;
                        }
                    }
                }
                if (state.tripDurationSec > 5) {
                    state.tripAvgSpeedKmh = static_cast<int>(
                        std::round((state.tripDistanceKm * 3600.0) / state.tripDurationSec));
                }
                if (state.tripDistanceKm >= 0.05) {
                    state.tripEvRatioPct = (state.tripEvKm * 100.0) / state.tripDistanceKm;
                } else {
                    ++totalActiveSamples;
                    if (state.iceRpm == 0) {
                        ++evSamples;
                    }
                    if (totalActiveSamples > 0) {
                        state.tripEvRatioPct = (static_cast<double>(evSamples) * 100.0) / static_cast<double>(totalActiveSamples);
                    }
                }
                if (state.tripEvKm >= 0.20) {
                    double netKwh = std::max(0.0, state.tripEvKwhUsed - state.tripRegenKwh);
                    state.tripEvKwh100km = (netKwh * 100.0) / state.tripEvKm;
                }

                // Estimate remaining usable HV energy (kWh) and EV / Gasoline / Total range (km)
                if (state.hvSocDisplayPct.has_value()) {
                    double usablePackKwh = 10.4 * (state.hvSohPct.value_or(94.0) / 100.0);
                    state.hvEnergyRemKwh = usablePackKwh * (*state.hvSocDisplayPct / 100.0);
                    double refKwh100km = state.tripEvKwh100km.value_or(18.5);
                    if (refKwh100km < 10.0 || refKwh100km > 40.0) {
                        refKwh100km = 18.5;
                    }
                    state.evRangeEstKm = static_cast<int>(std::round((*state.hvEnergyRemKwh * 100.0) / refKwh100km));
                }
                if (state.fuelLevelPct.has_value()) {
                    // Passat B8 GTE fuel tank is 50.0 liters
                    state.fuelLitersRem = 50.0 * (*state.fuelLevelPct / 100.0);
                    state.fuelRangeEstKm = static_cast<int>(std::round((*state.fuelLitersRem * 100.0) / 6.2));
                    state.totalRangeEstKm = state.fuelRangeEstKm.value_or(0) + state.evRangeEstKm.value_or(0);
                }

                // Medium loop (every 3rd active cycle): Thermal, Fuel & MQB GTE HV BMC (7E5) UDS PIDs
                if ((cycleCount % 3) == 0) {
                    if (auto coolHex = extractHexPayload(elm.sendCommand("0105", 700), "4105")) {
                        if (auto b = parseHexByte(*coolHex, 0)) {
                            state.iceCoolantC = *b - 40;
                        }
                    }
                    if (auto oilHex = extractHexPayload(elm.sendCommand("015C", 700), "415C")) {
                        if (auto b = parseHexByte(*oilHex, 0)) {
                            state.iceOilC = *b - 40;
                        }
                    }
                    int refEngineTemp = state.iceOilC.value_or(state.iceCoolantC.value_or(20));
                    if (refEngineTemp >= 80) {
                        state.iceWarmupStatus = "OPTIMAL (GTE READY)";
                    } else if (refEngineTemp >= 60) {
                        state.iceWarmupStatus = "WARMING UP";
                    } else {
                        state.iceWarmupStatus = "COLD (AVOID KICKDOWN)";
                    }
                    if (auto iatHex = extractHexPayload(elm.sendCommand("010F", 600), "410F")) {
                        if (auto b = parseHexByte(*iatHex, 0)) {
                            state.intakeAirC = *b - 40;
                        }
                    }
                    if (auto ambHex = extractHexPayload(elm.sendCommand("0146", 700), "4146")) {
                        if (auto b = parseHexByte(*ambHex, 0)) {
                            state.ambientC = *b - 40;
                        }
                    }
                    if (auto fuelHex = extractHexPayload(elm.sendCommand("012F", 700), "412F")) {
                        if (auto b = parseHexByte(*fuelHex, 0)) {
                            state.fuelLevelPct = (*b * 100.0) / 255.0;
                        }
                    }
                    if (auto ecuVHex = extractHexPayload(elm.sendCommand("0142", 600), "4142")) {
                        if (auto w = parseHexWordU16(*ecuVHex, 0)) {
                            state.ecu12vV = *w / 1000.0;
                        }
                    }
                    // Poll 1.4 TSI GDI High-Pressure Fuel Rail (0123), Catalyst Temp (013C),
                    // Spark Timing Advance (010E), Fuel Trims STFT/LTFT (0106/0107), and ICE Runtime (011F)
                    if (state.iceRpm > 0) {
                        if (auto frpHex = extractHexPayload(elm.sendCommand("0123", 600), "4123")) {
                            if (auto w = parseHexWordU16(*frpHex, 0)) {
                                // PID 0123 is 10 kPa per bit = 0.1 bar per bit
                                state.fuelRailBar = *w * 0.1;
                            }
                        }
                        if (auto ignHex = extractHexPayload(elm.sendCommand("010E", 600), "410E")) {
                            if (auto b = parseHexByte(*ignHex, 0)) {
                                state.timingAdvanceDeg = (*b / 2.0) - 64.0;
                            }
                        }
                        if (auto stftHex = extractHexPayload(elm.sendCommand("0106", 600), "4106")) {
                            if (auto b = parseHexByte(*stftHex, 0)) {
                                state.stftPct = ((*b - 128) * 100.0) / 128.0;
                            }
                        }
                        if (auto ltftHex = extractHexPayload(elm.sendCommand("0107", 600), "4107")) {
                            if (auto b = parseHexByte(*ltftHex, 0)) {
                                state.ltftPct = ((*b - 128) * 100.0) / 128.0;
                            }
                        }
                    }
                    if ((cycleCount % 6) == 0) {
                        if (auto catHex = extractHexPayload(elm.sendCommand("013C", 600), "413C")) {
                            if (auto w = parseHexWordU16(*catHex, 0)) {
                                int tc = static_cast<int>(std::round((*w / 10.0) - 40.0));
                                if (tc >= -20 && tc <= 1100) {
                                    state.catTempC = tc;
                                }
                            }
                        }
                        if (auto runHex = extractHexPayload(elm.sendCommand("011F", 600), "411F")) {
                            if (auto w = parseHexWordU16(*runHex, 0)) {
                                state.iceRuntimeSec = *w;
                            }
                        }
                    }
                    if (!baroKpa.has_value()) {
                        if (auto baroHex = extractHexPayload(elm.sendCommand("0133", 600), "4133")) {
                            baroKpa = parseHexByte(*baroHex, 0);
                        }
                    }
                    if ((cycleCount % 12) == 0) {
                        // Poll MIL / DTC count (0101), stored DTC codes (03), and Odometer (01A6)
                        if (auto dtcHex = extractHexPayload(elm.sendCommand("0101", 600), "4101")) {
                            if (auto b0 = parseHexByte(*dtcHex, 0)) {
                                state.milOn = ((*b0 & 0x80) != 0);
                                state.dtcCount = (*b0 & 0x7F);
                            }
                        }
                        if (state.dtcCount > 0 || state.milOn) {
                            state.dtcCodes = decodeMode03Dtcs(elm.sendCommand("03", 800));
                        } else {
                            state.dtcCodes = "NONE";
                        }
                        if (auto odoHex = extractHexPayload(elm.sendCommand("01A6", 700), "41A6")) {
                            auto b0 = parseHexByte(*odoHex, 0);
                            auto b1 = parseHexByte(*odoHex, 1);
                            auto b2 = parseHexByte(*odoHex, 2);
                            auto b3 = parseHexByte(*odoHex, 3);
                            if (b0 && b1 && b2 && b3) {
                                uint32_t rawOdo = (static_cast<uint32_t>(*b0) << 24) |
                                                  (static_cast<uint32_t>(*b1) << 16) |
                                                  (static_cast<uint32_t>(*b2) << 8) |
                                                  static_cast<uint32_t>(*b3);
                                state.odometerKm = rawOdo / 10.0;
                            }
                        }
                        // Poll Distance since DTC clear (0131) and Distance with MIL on (0121)
                        if (auto clrHex = extractHexPayload(elm.sendCommand("0131", 600), "4131")) {
                            if (auto w = parseHexWordU16(*clrHex, 0)) {
                                state.kmSinceDtcClear = *w;
                            }
                        }
                        if (auto milKmHex = extractHexPayload(elm.sendCommand("0121", 600), "4121")) {
                            if (auto w = parseHexWordU16(*milKmHex, 0)) {
                                state.kmWithMilOn = *w;
                            }
                        }
                        if (state.vin.empty()) {
                            state.vin = decodeMode09Vin(elm.sendCommand("0902", 900));
                        }
                        // Query MQB Gateway 19 (J533: TX 710, RX 77A) for 12V AGM Battery SoC (222B01)
                        elm.sendCommand("ATSH710", 500);
                        elm.sendCommand("ATCRA77A", 500);
                        if (auto bdmHex = extractHexPayload(elm.sendCommand("222B01", 700), "622B01")) {
                            if (auto b = parseHexByte(*bdmHex, 0)) {
                                if (*b > 0 && *b <= 100) {
                                    state.aux12vSocPct = *b;
                                }
                            }
                        }
                        if (!state.aux12vSocPct.has_value() && state.aux12vV.has_value()) {
                            // Estimate 12V AGM state of charge from resting/regulated voltage
                            double vRef = baselineResting12v.value_or(*state.aux12vV);
                            int estSoc = static_cast<int>(std::round(std::clamp((vRef - 11.8) / (12.85 - 11.8), 0.15, 1.0) * 100.0));
                            state.aux12vSocPct = estSoc;
                        }
                        // Query GTE Electric Drive / Power Electronics 51 (J841: TX 7E4, RX 7EC) for Inverter Temp
                        elm.sendCommand("ATSH7E4", 500);
                        elm.sendCommand("ATCRA7EC", 500);
                        if (auto invHex = extractHexPayload(elm.sendCommand("221E12", 700), "621E12")) {
                            if (auto b = parseHexByte(*invHex, 0)) {
                                int tc = *b - 40;
                                if (tc >= -30 && tc <= 150) {
                                    state.inverterTempC = tc;
                                }
                            }
                        }
                        // Fallback odometer read from VW MQB Instrument Cluster 17 (TX 714, RX 77E, DID 2203)
                        if (!state.odometerKm.has_value()) {
                            elm.sendCommand("ATSH714", 500);
                            elm.sendCommand("ATCRA77E", 500);
                            if (auto clOdoHex = extractHexPayload(elm.sendCommand("222203", 700), "622203")) {
                                auto b0 = parseHexByte(*clOdoHex, 0);
                                auto b1 = parseHexByte(*clOdoHex, 1);
                                auto b2 = parseHexByte(*clOdoHex, 2);
                                if (b0 && b1 && b2) {
                                    uint32_t km = (static_cast<uint32_t>(*b0) << 16) |
                                                  (static_cast<uint32_t>(*b1) << 8) |
                                                  static_cast<uint32_t>(*b2);
                                    if (km > 0 && km < 1500000) {
                                        state.odometerKm = static_cast<double>(km);
                                    }
                                }
                            }
                        }
                        saveLastTripSummary(state);
                        saveLifetimeStats(state);
                    }

                    // Query Passat GTE Hybrid Battery Management (J840 / BMC: TX 7E5, RX 7ED)
                    elm.sendCommand("ATSH7E5", 500);
                    elm.sendCommand("ATCRA7ED", 500);

                    if (auto gSocHex = extractHexPayload(elm.sendCommand("22028C", 800), "62028C")) {
                        if (auto w = parseHexWordU16(*gSocHex, 0)) {
                            double val = *w / 10.0;
                            if (val >= 0.0 && val <= 100.0) {
                                state.hvSocGrossPct = val;
                            }
                        } else if (auto b = parseHexByte(*gSocHex, 0)) {
                            state.hvSocGrossPct = (*b * 100.0) / 255.0;
                        }
                    }

                    if (auto hvVHex = extractHexPayload(elm.sendCommand("221E3B", 800), "621E3B")) {
                        if (auto w = parseHexWordU16(*hvVHex, 0)) {
                            // MQB BMC reports HV pack voltage in 0.1V or 0.01V steps; normalize to 200..450V range
                            double v = (*w > 10000) ? (*w / 100.0) : (*w / 10.0);
                            if (v >= 150.0 && v <= 460.0) {
                                state.hvVoltageV = v;
                                // Derive mean 96s cell voltage if individual min/max cell DIDs are not yet read
                                if (!state.hvCellMinV.has_value()) {
                                    state.hvCellMinV = v / 96.0;
                                    state.hvCellMaxV = v / 96.0;
                                    state.hvCellDeltaMv = 0;
                                }
                            }
                        }
                    }

                    if (auto hvIHex = extractHexPayload(elm.sendCommand("221E3D", 800), "621E3D")) {
                        if (auto w = parseHexWordU16(*hvIHex, 0)) {
                            int16_t signedRaw = static_cast<int16_t>(*w);
                            double amps = signedRaw / 10.0;
                            if (fabs(amps) <= 500.0) {
                                state.hvCurrentA = amps;
                            }
                        }
                    }

                    if (state.hvVoltageV.has_value() && state.hvCurrentA.has_value()) {
                        state.hvPowerKw = (*state.hvVoltageV * *state.hvCurrentA) / 1000.0;
                        if (*state.hvPowerKw < state.tripPeakDischargeKw) {
                            state.tripPeakDischargeKw = *state.hvPowerKw;
                        }
                        if (*state.hvPowerKw > state.tripPeakRegenKw) {
                            state.tripPeakRegenKw = *state.hvPowerKw;
                        }
                    }

                    // Query HV Cell Min/Max Voltage (221E33 / 221E34) & Usable Capacity SOH (221E32) every 6th cycle
                    if ((cycleCount % 6) == 0) {
                        if (auto cMinHex = extractHexPayload(elm.sendCommand("221E33", 700), "621E33")) {
                            if (auto w = parseHexWordU16(*cMinHex, 0)) {
                                double cv = (*w > 1000) ? (*w / 1000.0) : (*w / 100.0);
                                if (cv >= 2.5 && cv <= 4.35) {
                                    state.hvCellMinV = cv;
                                }
                            }
                        }
                        if (auto cMaxHex = extractHexPayload(elm.sendCommand("221E34", 700), "621E34")) {
                            if (auto w = parseHexWordU16(*cMaxHex, 0)) {
                                double cv = (*w > 1000) ? (*w / 1000.0) : (*w / 100.0);
                                if (cv >= 2.5 && cv <= 4.35) {
                                    state.hvCellMaxV = cv;
                                }
                            }
                        }
                        if (state.hvCellMinV.has_value() && state.hvCellMaxV.has_value() &&
                            *state.hvCellMaxV >= *state.hvCellMinV) {
                            state.hvCellDeltaMv = static_cast<int>(
                                std::round((*state.hvCellMaxV - *state.hvCellMinV) * 1000.0));
                        }
                        if (auto capHex = extractHexPayload(elm.sendCommand("221E32", 700), "621E32")) {
                            if (auto w = parseHexWordU16(*capHex, 0)) {
                                // Nominal Passat B8 GTE cell capacity is 37.0 Ah (or 10400 Wh net)
                                double ah = (*w > 1000) ? (*w / 100.0) : (*w / 10.0);
                                if (ah >= 15.0 && ah <= 42.0) {
                                    state.hvSohPct = std::min(100.0, (ah / 37.0) * 100.0);
                                }
                            }
                        }
                    }

                    if (auto tMinHex = extractHexPayload(elm.sendCommand("221E0E", 800), "621E0E")) {
                        if (auto w = parseHexWordU16(*tMinHex, 0)) {
                            int16_t sr = static_cast<int16_t>(*w);
                            double tc = sr / 10.0;
                            if (tc >= -40.0 && tc <= 100.0) {
                                state.hvTempMinC = tc;
                            }
                        } else if (auto b = parseHexByte(*tMinHex, 0)) {
                            state.hvTempMinC = *b - 40.0;
                        }
                    }

                    if (auto tMaxHex = extractHexPayload(elm.sendCommand("222A0B", 800), "622A0B")) {
                        if (auto w = parseHexWordU16(*tMaxHex, 0)) {
                            int16_t sr = static_cast<int16_t>(*w);
                            double tc = sr / 10.0;
                            if (tc >= -40.0 && tc <= 100.0) {
                                state.hvTempMaxC = tc;
                            }
                        } else if (auto b = parseHexByte(*tMaxHex, 0)) {
                            state.hvTempMaxC = *b - 40.0;
                        }
                    }

                    if (state.hvTempMinC.has_value() && state.hvTempMaxC.has_value()) {
                        state.hvTempSpreadC = *state.hvTempMaxC - *state.hvTempMinC;
                    }

                    // Query DQ400e 6-speed DSG Transmission Oil Temp (J743: TX 7E1, RX 7E9) every 6th cycle
                    if ((cycleCount % 6) == 0) {
                        elm.sendCommand("ATSH7E1", 500);
                        elm.sendCommand("ATCRA7E9", 500);
                        if (auto dsgHex = extractHexPayload(elm.sendCommand("22F405", 700), "62F405")) {
                            if (auto b = parseHexByte(*dsgHex, 0)) {
                                int tc = *b - 40;
                                if (tc >= -30 && tc <= 160) {
                                    state.dsgTempC = tc;
                                }
                            }
                        }
                    }

                    // Always restore standard broadcast OBD-II header (7DF) and automatic RX filter
                    elm.sendCommand("ATSH7DF", 500);
                    elm.sendCommand("ATAR", 500);
                }

                // Update Drive Mode badge
                if (state.iceRpm == 0) {
                    state.driveMode = (state.speedKmh > 0) ? "E-MODE (100% EV)" : "READY (E-MODE)";
                } else if (state.hvPowerKw.has_value() && *state.hvPowerKw < -1.5) {
                    state.driveMode = "GTE / BOOST (ICE + EV)";
                } else {
                    state.driveMode = "HYBRID (1.4 TSI)";
                }

                ++cycleCount;
                writeTelemetrySnapshot(state);
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            } else {
                state.speedKmh = 0;
                state.driveMode = shouldFireGarageOnObdStart()
                    ? "ARMED (PRESS CAR START)"
                    : "STANDBY (IGN OFF)";
                writeTelemetrySnapshot(state);

                // Anti-drain protection: if the car was previously running and ignition
                // has now been turned OFF for 3 consecutive checks with 12V < 12.85V,
                // put the ELM327 to sleep and pause polling for 20s so J533 can sleep.
                if (wasIgnitionOnThisSession && state.aux12vV.value_or(13.0) < 12.85) {
                    ++consecutiveNoDataAfterRun;
                    if (consecutiveNoDataAfterRun >= 3) {
                        Logger::instance()->info("OBD Telemetry: Ignition OFF after drive; saving trip summary and entering low-power standby\n");
                        saveLastTripSummary(state);
                        saveLifetimeStats(state);
                        std::string saved = readFirstLine(LAST_TRIP_JSON_PATH);
                        if (!saved.empty() && saved.front() == '{') {
                            state.lastTripJson = saved;
                        }
                        elm.sendCommand("ATLP", 400);
                        elm.closeSession();
                        try { (*devDisconnect)(); } catch (...) {}
                        wasIgnitionOnThisSession = false;
                        consecutiveNoDataAfterRun = 0;
                        for (int s = 0; s < 20 && !g_shouldExit.load(); ++s) {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                        }
                        break;
                    }
                }

                // When waiting in the garage for the driver to press START, poll every 1s
                // for instant garage door response; otherwise poll every 2.5s.
                int waitMs = shouldFireGarageOnObdStart() ? 900 : 2500;
                std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
            }
        }
    }

    Logger::instance()->info("OBD Telemetry: Daemon exiting\n");
    return 0;
}
