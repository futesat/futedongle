#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr const char* DEFAULT_BIND_IP = "10.0.0.1";
constexpr int DEFAULT_PORT = 80;
constexpr const char* WEB_ROOT_INDEX = "/usr/share/futedongle-web/index.html";
constexpr const char* BOOT_CONF_PATH = "/boot/aawgd.conf";
constexpr const char* ETC_CONF_PATH = "/etc/aawgd.conf";
constexpr const char* BOOT_LOCK_PATH = "/var/run/boot_mount.lock";

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string readFileText(const std::string& path, size_t maxBytes = 256 * 1024) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return "";
    }
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size <= 0) {
        std::ostringstream ss;
        file.clear();
        file.seekg(0, std::ios::beg);
        ss << file.rdbuf();
        return ss.str();
    }
    if (static_cast<size_t>(size) > maxBytes) {
        file.seekg(size - static_cast<std::streamoff>(maxBytes), std::ios::beg);
    } else {
        file.seekg(0, std::ios::beg);
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string readFirstLine(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::string line;
    std::getline(file, line);
    return trim(line);
}

std::string jsonEscape(const std::string& input) {
    std::ostringstream ss;
    for (unsigned char c : input) {
        switch (c) {
            case '"':  ss << "\\\""; break;
            case '\\': ss << "\\\\"; break;
            case '\b': ss << "\\b";  break;
            case '\f': ss << "\\f";  break;
            case '\n': ss << "\\n";  break;
            case '\r': ss << "\\r";  break;
            case '\t': ss << "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    ss << buf;
                } else {
                    ss << static_cast<char>(c);
                }
                break;
        }
    }
    return ss.str();
}

std::string urlDecode(const std::string& encoded) {
    std::string out;
    out.reserve(encoded.size());
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '+') {
            out.push_back(' ');
        } else if (encoded[i] == '%' && i + 2 < encoded.size() &&
                   isxdigit(static_cast<unsigned char>(encoded[i + 1])) &&
                   isxdigit(static_cast<unsigned char>(encoded[i + 2]))) {
            char hex[3] = {encoded[i + 1], encoded[i + 2], '\0'};
            out.push_back(static_cast<char>(strtol(hex, nullptr, 16)));
            i += 2;
        } else {
            out.push_back(encoded[i]);
        }
    }
    return out;
}

std::map<std::string, std::string> parseQueryOrForm(const std::string& data) {
    std::map<std::string, std::string> params;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t amp = data.find('&', pos);
        if (amp == std::string::npos) {
            amp = data.size();
        }
        std::string pair = data.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            params[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
        } else if (!pair.empty()) {
            params[urlDecode(pair)] = "";
        }
        pos = amp + 1;
    }
    return params;
}

bool writeAll(int fd, const char* data, size_t len) {
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t written = write(fd, data, remaining);
        if (written <= 0) {
            return false;
        }
        data += written;
        remaining -= static_cast<size_t>(written);
    }
    return true;
}

void sendHttpResponse(int clientFd, const char* status, const char* contentType,
                      const std::string& body, const char* extraHeader = nullptr) {
    std::ostringstream header;
    header << "HTTP/1.1 " << status << "\r\n"
           << "Content-Type: " << contentType << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Cache-Control: no-store, no-cache, must-revalidate\r\n"
           << "Connection: close\r\n";
    if (extraHeader != nullptr && extraHeader[0] != '\0') {
        header << extraHeader << "\r\n";
    }
    header << "\r\n";
    std::string headerStr = header.str();
    if (writeAll(clientFd, headerStr.data(), headerStr.size()) && !body.empty()) {
        writeAll(clientFd, body.data(), body.size());
    }
}

void sendJsonError(int clientFd, const char* status, const std::string& message) {
    std::string body = "{\"ok\":false,\"error\":\"" + jsonEscape(message) + "\"}";
    sendHttpResponse(clientFd, status, "application/json; charset=utf-8", body);
}

bool isTcpPortEstablished(uint16_t port) {
    std::ifstream tcpFile("/proc/net/tcp");
    if (!tcpFile.is_open()) {
        return false;
    }
    char hexPort[16];
    snprintf(hexPort, sizeof(hexPort), ":%04X", port);

    std::string line;
    std::getline(tcpFile, line); // Skip header
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

bool isProcessRunning(const std::string& commName) {
    DIR* procDir = opendir("/proc");
    if (!procDir) {
        return false;
    }
    struct dirent* entry = nullptr;
    bool found = false;
    while ((entry = readdir(procDir)) != nullptr) {
        if (!isdigit(static_cast<unsigned char>(entry->d_name[0]))) {
            continue;
        }
        std::string commPath = std::string("/proc/") + entry->d_name + "/comm";
        if (readFirstLine(commPath) == commName) {
            found = true;
            break;
        }
    }
    closedir(procDir);
    return found;
}

int countUndervoltageEvents() {
    std::ifstream logFile("/var/log/messages");
    if (!logFile.is_open()) {
        return 0;
    }
    int count = 0;
    std::string line;
    while (std::getline(logFile, line)) {
        if (line.find("Under-voltage") != std::string::npos ||
            line.find("under-voltage") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

std::string buildStatusJson() {
    // Uptime
    double uptimeSec = 0.0;
    {
        std::istringstream iss(readFirstLine("/proc/uptime"));
        iss >> uptimeSec;
    }

    // Load average
    std::string loadAvg = readFirstLine("/proc/loadavg");

    // Memory
    long memTotalKb = 0;
    long memAvailKb = 0;
    {
        std::ifstream memFile("/proc/meminfo");
        std::string key;
        long val = 0;
        std::string unit;
        while (memFile >> key >> val >> unit) {
            if (key == "MemTotal:") {
                memTotalKb = val;
            } else if (key == "MemAvailable:") {
                memAvailKb = val;
            }
        }
    }

    // SoC Temperature & CPU Frequency
    double socTempC = 0.0;
    {
        std::string tempStr = readFirstLine("/sys/class/thermal/thermal_zone0/temp");
        if (!tempStr.empty()) {
            socTempC = atof(tempStr.c_str()) / 1000.0;
        }
    }
    long cpuFreqMhz = 600;
    {
        std::string freqStr = readFirstLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
        if (!freqStr.empty()) {
            cpuFreqMhz = atol(freqStr.c_str()) / 1000;
        }
    }

    // USB UDC & Gadget state
    std::string udcName;
    std::string udcState = "unknown";
    std::string udcSpeed = "unknown";
    if (DIR* udcDir = opendir("/sys/class/udc")) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(udcDir)) != nullptr) {
            if (entry->d_name[0] != '.') {
                udcName = entry->d_name;
                break;
            }
        }
        closedir(udcDir);
    }
    if (!udcName.empty()) {
        udcState = readFirstLine("/sys/class/udc/" + udcName + "/state");
        udcSpeed = readFirstLine("/sys/class/udc/" + udcName + "/current_speed");
    }

    std::string activeGadget = "none";
    if (!readFirstLine("/sys/kernel/config/usb_gadget/accessory/UDC").empty()) {
        activeGadget = "accessory (AOAv2)";
    } else if (!readFirstLine("/sys/kernel/config/usb_gadget/default/UDC").empty()) {
        activeGadget = "default (MTP)";
    }

    bool tcpProxyActive = isTcpPortEstablished(5288);
    bool aawgdRunning = isProcessRunning("aawgd");
    int undervoltageCount = countUndervoltageEvents();

    // DHCP Leases
    std::ostringstream leasesJson;
    leasesJson << "[";
    {
        std::ifstream leaseFile("/var/lib/misc/dnsmasq.leases");
        std::string line;
        bool first = true;
        while (std::getline(leaseFile, line)) {
            std::istringstream iss(line);
            std::string expiry, mac, ip, hostname;
            if (iss >> expiry >> mac >> ip >> hostname) {
                if (!first) {
                    leasesJson << ",";
                }
                first = false;
                leasesJson << "{\"ip\":\"" << jsonEscape(ip) << "\","
                           << "\"mac\":\"" << jsonEscape(mac) << "\","
                           << "\"hostname\":\"" << jsonEscape(hostname) << "\"}";
            }
        }
    }
    leasesJson << "]";

    // Optional Feature 01 GTE Telemetry JSON
    std::string gteTelemetryRaw = trim(readFileText("/run/gte_telemetry.json", 32 * 1024));
    if (gteTelemetryRaw.empty() || gteTelemetryRaw.front() != '{') {
        gteTelemetryRaw = "null";
    }

    char numBuf[64];
    std::ostringstream out;
    out << "{\"ok\":true,";
    snprintf(numBuf, sizeof(numBuf), "%.1f", uptimeSec);
    out << "\"uptime_sec\":" << numBuf << ",";
    out << "\"load_avg\":\"" << jsonEscape(loadAvg) << "\",";
    out << "\"mem_total_kb\":" << memTotalKb << ",";
    out << "\"mem_available_kb\":" << memAvailKb << ",";
    snprintf(numBuf, sizeof(numBuf), "%.1f", socTempC);
    out << "\"soc_temp_c\":" << numBuf << ",";
    out << "\"cpu_freq_mhz\":" << cpuFreqMhz << ",";
    out << "\"undervoltage_events\":" << undervoltageCount << ",";
    out << "\"udc_name\":\"" << jsonEscape(udcName) << "\",";
    out << "\"udc_state\":\"" << jsonEscape(udcState) << "\",";
    out << "\"udc_speed\":\"" << jsonEscape(udcSpeed) << "\",";
    out << "\"active_gadget\":\"" << jsonEscape(activeGadget) << "\",";
    out << "\"aawgd_running\":" << (aawgdRunning ? "true" : "false") << ",";
    out << "\"tcp_proxy_active\":" << (tcpProxyActive ? "true" : "false") << ",";
    out << "\"dhcp_leases\":" << leasesJson.str() << ",";
    out << "\"gte_telemetry\":" << gteTelemetryRaw;
    out << "}";
    return out.str();
}

std::string tailLines(const std::string& content, size_t maxLines) {
    if (maxLines == 0) {
        return content;
    }
    size_t count = 0;
    size_t pos = content.size();
    while (pos > 0) {
        --pos;
        if (content[pos] == '\n') {
            if (++count > maxLines) {
                return content.substr(pos + 1);
            }
        }
    }
    return content;
}

std::string readCommandOutput(const char* cmd, size_t maxBytes = 128 * 1024) {
    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        return "";
    }
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        out.append(buf);
        if (out.size() >= maxBytes) {
            break;
        }
    }
    pclose(pipe);
    return out;
}

void handleLogsEndpoint(int clientFd, const std::map<std::string, std::string>& query) {
    std::string source = "messages";
    if (auto it = query.find("source"); it != query.end() && !it->second.empty()) {
        source = it->second;
    }
    size_t lines = 200;
    if (auto it = query.find("lines"); it != query.end() && !it->second.empty()) {
        long parsed = atol(it->second.c_str());
        if (parsed > 0 && parsed <= 2000) {
            lines = static_cast<size_t>(parsed);
        }
    }
    bool download = false;
    if (auto it = query.find("download"); it != query.end() && it->second == "1") {
        download = true;
    }

    std::string rawContent;
    std::string filename = source + ".log";
    if (source == "messages") {
        rawContent = readFileText("/var/log/messages", 512 * 1024);
    } else if (source == "hostapd") {
        rawContent = readFileText("/var/log/hostapd", 256 * 1024);
    } else if (source == "car_debug") {
        rawContent = readFileText("/boot/car_debug.log", 512 * 1024);
        filename = "car_debug.log";
    } else if (source == "dmesg") {
        rawContent = readCommandOutput("/bin/dmesg 2>/dev/null", 256 * 1024);
    } else {
        sendJsonError(clientFd, "400 Bad Request", "Unsupported log source");
        return;
    }

    if (download) {
        std::string disp = "Content-Disposition: attachment; filename=\"" + filename + "\"";
        sendHttpResponse(clientFd, "200 OK", "text/plain; charset=utf-8", rawContent, disp.c_str());
        return;
    }

    std::string tailed = tailLines(rawContent, lines);
    std::ostringstream json;
    json << "{\"ok\":true,\"source\":\"" << jsonEscape(source)
         << "\",\"content\":\"" << jsonEscape(tailed) << "\"}";
    sendHttpResponse(clientFd, "200 OK", "application/json; charset=utf-8", json.str());
}

std::string getConfFilePath() {
    struct stat st;
    if (stat(BOOT_CONF_PATH, &st) == 0) {
        return BOOT_CONF_PATH;
    }
    return ETC_CONF_PATH;
}

std::string unquoteShellValue(const std::string& rawVal) {
    std::string out;
    out.reserve(rawVal.size());
    bool inSingle = false;
    bool inDouble = false;

    for (size_t i = 0; i < rawVal.size(); ++i) {
        char c = rawVal[i];
        if (inSingle) {
            if (c == '\'') {
                inSingle = false;
            } else {
                out.push_back(c);
            }
        } else if (inDouble) {
            if (c == '"') {
                inDouble = false;
            } else if (c == '\\' && i + 1 < rawVal.size() &&
                       (rawVal[i + 1] == '"' || rawVal[i + 1] == '\\' ||
                        rawVal[i + 1] == '$' || rawVal[i + 1] == '`')) {
                out.push_back(rawVal[++i]);
            } else {
                out.push_back(c);
            }
        } else {
            if (c == '\'') {
                inSingle = true;
            } else if (c == '"') {
                inDouble = true;
            } else if (c == '\\' && i + 1 < rawVal.size()) {
                out.push_back(rawVal[++i]);
            } else if (c == ' ' || c == '\t') {
                break;
            } else {
                out.push_back(c);
            }
        }
    }
    return out;
}

std::string shellQuote(const std::string& val) {
    if (!val.empty()) {
        bool safeBare = true;
        for (unsigned char c : val) {
            if (!isalnum(c) && c != '_' && c != '-' && c != '.' && c != ':' && c != '/') {
                safeBare = false;
                break;
            }
        }
        if (safeBare) {
            return val;
        }
    }
    std::string out = "'";
    for (char c : val) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::map<std::string, std::string> readConfigMap() {
    std::map<std::string, std::string> cfg = {
        {"AAWG_COUNTRY_CODE", "ES"},
        {"AAWG_CONNECTION_STRATEGY", "1"},
        {"AAWG_EARLY_USB_VERSION_EXCHANGE", "0"},
        {"AAWG_BLUETOOTH_NAME", "futedongle"},
        {"AAWG_WIFI_SSID", "futedongle"},
        {"AAWG_WIFI_PASSWORD", "fute?dongle!"},
        {"AAWG_ENABLE_SSH", "0"},
        {"AAWG_ENABLE_DEBUG_LOG", "0"},
        {"AAWG_ENABLE_WEB_PORTAL", "1"},
        {"AAWG_ENABLE_OBD_TELEMETRY", "0"},
        {"AAWG_OBD_BLE_MAC", ""},
        {"AAWG_ENABLE_GARAGE_OPEN", "1"},
        {"AAWG_GARAGE_WIFI_SSID", "COMARCA"},
        {"AAWG_GARAGE_WIFI_HIDDEN", "1"},
        {"AAWG_GARAGE_WIFI_BAND", "2.4"},
        {"AAWG_GARAGE_WIFI_PASSWORD", "miPassFake"},
        {"AAWG_GARAGE_UDP_HOST", "192.168.1.9"},
        {"AAWG_GARAGE_UDP_PORT", "9876"},
        {"AAWG_GARAGE_UDP_PAYLOAD", "OPEN_BOTH_GARAGE_FIRST"},
        {"AAWG_GARAGE_TIMEOUT_SEC", "5"}
    };

    std::ifstream file(getConfFilePath());
    if (!file.is_open()) {
        return cfg;
    }

    std::string line;
    while (std::getline(file, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = trim(t.substr(0, eq));
        std::string val = unquoteShellValue(trim(t.substr(eq + 1)));
        if (cfg.find(key) != cfg.end()) {
            cfg[key] = val;
        }
    }
    return cfg;
}

bool isValidIdentifier(const std::string& s, size_t maxLen) {
    if (s.empty() || s.size() > maxLen) {
        return false;
    }
    for (unsigned char c : s) {
        if (!isalnum(c) && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

bool isPrintableAscii(const std::string& s, size_t minLen, size_t maxLen) {
    if (s.size() < minLen || s.size() > maxLen) {
        return false;
    }
    for (unsigned char c : s) {
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

bool isValidHost(const std::string& s) {
    if (s.empty() || s.size() > 64 || s.front() == '-' || s.back() == '-') {
        return false;
    }
    for (unsigned char c : s) {
        if (!isalnum(c) && c != '.' && c != '-') {
            return false;
        }
    }
    return true;
}

bool isValidIntInRange(const std::string& s, int minVal, int maxVal) {
    if (s.empty() || s.size() > 6) {
        return false;
    }
    for (unsigned char c : s) {
        if (!isdigit(c)) {
            return false;
        }
    }
    long v = strtol(s.c_str(), nullptr, 10);
    return v >= minVal && v <= maxVal;
}

bool isValidMacOrEmpty(const std::string& s) {
    if (s.empty()) {
        return true;
    }
    if (s.size() != 17) {
        return false;
    }
    for (size_t i = 0; i < 17; ++i) {
        if ((i % 3) == 2) {
            if (s[i] != ':') return false;
        } else {
            if (!isxdigit(static_cast<unsigned char>(s[i]))) return false;
        }
    }
    return true;
}

void handleGetConfig(int clientFd) {
    auto cfg = readConfigMap();
    std::ostringstream json;
    json << "{\"ok\":true,\"config\":{";
    bool first = true;
    for (const auto& [k, v] : cfg) {
        if (!first) json << ",";
        first = false;
        json << "\"" << jsonEscape(k) << "\":\"" << jsonEscape(v) << "\"";
    }
    json << "}}";
    sendHttpResponse(clientFd, "200 OK", "application/json; charset=utf-8", json.str());
}

void handlePostConfig(int clientFd, const std::string& body) {
    auto updates = parseQueryOrForm(body);
    auto cfg = readConfigMap();

    for (const auto& [k, v] : updates) {
        if (cfg.find(k) == cfg.end()) {
            continue;
        }
        bool preserveRawSpaces = (k == "AAWG_WIFI_PASSWORD" ||
                                  k == "AAWG_GARAGE_WIFI_SSID" ||
                                  k == "AAWG_GARAGE_WIFI_PASSWORD" ||
                                  k == "AAWG_GARAGE_UDP_PAYLOAD");
        std::string val = preserveRawSpaces ? v : trim(v);

        if (k == "AAWG_COUNTRY_CODE") {
            if (val.size() != 2 || !isupper(static_cast<unsigned char>(val[0])) || !isupper(static_cast<unsigned char>(val[1]))) {
                sendJsonError(clientFd, "400 Bad Request", "AAWG_COUNTRY_CODE must be a 2-letter uppercase ISO code (e.g. ES)");
                return;
            }
        } else if (k == "AAWG_CONNECTION_STRATEGY") {
            if (val != "0" && val != "1" && val != "2") {
                sendJsonError(clientFd, "400 Bad Request", "AAWG_CONNECTION_STRATEGY must be 0, 1, or 2");
                return;
            }
        } else if (k == "AAWG_EARLY_USB_VERSION_EXCHANGE" ||
                   k == "AAWG_ENABLE_SSH" ||
                   k == "AAWG_ENABLE_DEBUG_LOG" ||
                   k == "AAWG_ENABLE_WEB_PORTAL" ||
                   k == "AAWG_ENABLE_OBD_TELEMETRY" ||
                   k == "AAWG_ENABLE_GARAGE_OPEN" ||
                   k == "AAWG_GARAGE_WIFI_HIDDEN") {
            if (val != "0" && val != "1") {
                sendJsonError(clientFd, "400 Bad Request", k + " must be 0 or 1");
                return;
            }
        } else if (k == "AAWG_BLUETOOTH_NAME" || k == "AAWG_WIFI_SSID") {
            if (!isValidIdentifier(val, 32)) {
                sendJsonError(clientFd, "400 Bad Request", k + " must be 1-32 alphanumeric, '-' or '_' characters");
                return;
            }
        } else if (k == "AAWG_WIFI_PASSWORD") {
            if (!isPrintableAscii(val, 8, 63)) {
                sendJsonError(clientFd, "400 Bad Request", "AAWG_WIFI_PASSWORD must be 8-63 printable ASCII characters");
                return;
            }
        } else if (k == "AAWG_OBD_BLE_MAC") {
            if (!isValidMacOrEmpty(val)) {
                sendJsonError(clientFd, "400 Bad Request", "AAWG_OBD_BLE_MAC must be XX:XX:XX:XX:XX:XX or empty");
                return;
            }
        } else if (k == "AAWG_GARAGE_WIFI_SSID") {
            if (!isPrintableAscii(val, 0, 32)) {
                sendJsonError(clientFd, "400 Bad Request", "AAWG_GARAGE_WIFI_SSID must be up to 32 printable ASCII characters");
                return;
            }
        } else if (k == "AAWG_GARAGE_WIFI_BAND") {
            if (val != "2.4" && val != "all") {
                sendJsonError(clientFd, "400 Bad Request", "AAWG_GARAGE_WIFI_BAND must be 2.4 or all");
                return;
            }
        } else if (k == "AAWG_GARAGE_WIFI_PASSWORD") {
            if (!val.empty() && !isPrintableAscii(val, 8, 63)) {
                sendJsonError(clientFd, "400 Bad Request", "AAWG_GARAGE_WIFI_PASSWORD must be empty (open network) or 8-63 printable ASCII characters");
                return;
            }
        } else if (k == "AAWG_GARAGE_UDP_HOST") {
            if (!val.empty() && !isValidHost(val)) {
                sendJsonError(clientFd, "400 Bad Request", "AAWG_GARAGE_UDP_HOST must be a valid IPv4 address or hostname");
                return;
            }
        } else if (k == "AAWG_GARAGE_UDP_PORT") {
            if (!isValidIntInRange(val, 1, 65535)) {
                sendJsonError(clientFd, "400 Bad Request", "AAWG_GARAGE_UDP_PORT must be between 1 and 65535");
                return;
            }
        } else if (k == "AAWG_GARAGE_UDP_PAYLOAD") {
            if (!isPrintableAscii(val, 1, 128)) {
                sendJsonError(clientFd, "400 Bad Request", "AAWG_GARAGE_UDP_PAYLOAD must be 1-128 printable ASCII characters");
                return;
            }
        } else if (k == "AAWG_GARAGE_TIMEOUT_SEC") {
            if (!isValidIntInRange(val, 2, 15)) {
                sendJsonError(clientFd, "400 Bad Request", "AAWG_GARAGE_TIMEOUT_SEC must be between 2 and 15 seconds");
                return;
            }
        }
        cfg[k] = val;
    }

    if (cfg["AAWG_ENABLE_GARAGE_OPEN"] == "1") {
        if (cfg["AAWG_GARAGE_WIFI_SSID"].empty()) {
            sendJsonError(clientFd, "400 Bad Request", "AAWG_GARAGE_WIFI_SSID is required when garage opener is enabled");
            return;
        }
        if (cfg["AAWG_GARAGE_UDP_HOST"].empty()) {
            sendJsonError(clientFd, "400 Bad Request", "AAWG_GARAGE_UDP_HOST is required when garage opener is enabled");
            return;
        }
    }

    int lockFd = open(BOOT_LOCK_PATH, O_CREAT | O_RDWR, 0600);
    if (lockFd >= 0) {
        flock(lockFd, LOCK_EX);
    }

    bool bootMountedRw = (system("/bin/mount -o remount,rw /boot 2>/dev/null") == 0);
    std::string targetPath = bootMountedRw ? BOOT_CONF_PATH : getConfFilePath();
    std::string tmpPath = targetPath + ".tmp";

    std::ostringstream confContent;
    confContent << "#!/bin/sh\n\n"
                << "######## Configuration options for Android Auto Wireless Dongle ########\n\n"
                << "## Country code (ISO/IEC 3166-1)\n"
                << "AAWG_COUNTRY_CODE=" << cfg["AAWG_COUNTRY_CODE"] << "\n\n"
                << "## Connection strategy (0 = Dongle, 1 = Phone first, 2 = USB first)\n"
                << "AAWG_CONNECTION_STRATEGY=" << cfg["AAWG_CONNECTION_STRATEGY"] << "\n\n"
                << "## Early USB version exchange (0 = Disabled, 1 = Enabled)\n"
                << "AAWG_EARLY_USB_VERSION_EXCHANGE=" << cfg["AAWG_EARLY_USB_VERSION_EXCHANGE"] << "\n\n"
                << "## Custom Bluetooth device alias\n"
                << "AAWG_BLUETOOTH_NAME=" << shellQuote(cfg["AAWG_BLUETOOTH_NAME"]) << "\n\n"
                << "## Custom 5 GHz Wi-Fi SSID\n"
                << "AAWG_WIFI_SSID=" << shellQuote(cfg["AAWG_WIFI_SSID"]) << "\n\n"
                << "## Fixed 5 GHz Wi-Fi WPA2 passphrase\n"
                << "AAWG_WIFI_PASSWORD=" << shellQuote(cfg["AAWG_WIFI_PASSWORD"]) << "\n\n"
                << "######## Diagnostics & Next-Gen Features ########\n\n"
                << "## Enable SSH over eth0 (0 = Disabled, 1 = Enabled)\n"
                << "AAWG_ENABLE_SSH=" << cfg["AAWG_ENABLE_SSH"] << "\n\n"
                << "## Enable bounded 90s boot telemetry log at /boot/car_debug.log\n"
                << "AAWG_ENABLE_DEBUG_LOG=" << cfg["AAWG_ENABLE_DEBUG_LOG"] << "\n\n"
                << "## Enable Local Web Control Portal at http://10.0.0.1\n"
                << "AAWG_ENABLE_WEB_PORTAL=" << cfg["AAWG_ENABLE_WEB_PORTAL"] << "\n\n"
                << "## Enable Passat GTE OBD-II BLE Telemetry\n"
                << "AAWG_ENABLE_OBD_TELEMETRY=" << cfg["AAWG_ENABLE_OBD_TELEMETRY"] << "\n";
    if (!cfg["AAWG_OBD_BLE_MAC"].empty()) {
        confContent << "AAWG_OBD_BLE_MAC=" << cfg["AAWG_OBD_BLE_MAC"] << "\n";
    } else {
        confContent << "#AAWG_OBD_BLE_MAC=\n";
    }
    confContent << "\n######## Boot-Time Garage Door Opener (S38garage_opener) ########\n\n"
                << "## Connect briefly on power-up to a garage Wi-Fi AP and send a UDP trigger\n"
                << "## before starting the 5 GHz Android Auto Access Point.\n"
                << "AAWG_ENABLE_GARAGE_OPEN=" << cfg["AAWG_ENABLE_GARAGE_OPEN"] << "\n"
                << "AAWG_GARAGE_WIFI_SSID=" << shellQuote(cfg["AAWG_GARAGE_WIFI_SSID"]) << "\n"
                << "AAWG_GARAGE_WIFI_HIDDEN=" << cfg["AAWG_GARAGE_WIFI_HIDDEN"] << "\n"
                << "AAWG_GARAGE_WIFI_BAND=" << cfg["AAWG_GARAGE_WIFI_BAND"] << "\n"
                << "AAWG_GARAGE_WIFI_PASSWORD=" << shellQuote(cfg["AAWG_GARAGE_WIFI_PASSWORD"]) << "\n"
                << "AAWG_GARAGE_UDP_HOST=" << shellQuote(cfg["AAWG_GARAGE_UDP_HOST"]) << "\n"
                << "AAWG_GARAGE_UDP_PORT=" << cfg["AAWG_GARAGE_UDP_PORT"] << "\n"
                << "AAWG_GARAGE_UDP_PAYLOAD=" << shellQuote(cfg["AAWG_GARAGE_UDP_PAYLOAD"]) << "\n"
                << "AAWG_GARAGE_TIMEOUT_SEC=" << cfg["AAWG_GARAGE_TIMEOUT_SEC"] << "\n";

    std::string serialized = confContent.str();
    int outFd = open(tmpPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    bool writeOk = false;
    if (outFd >= 0) {
        writeOk = writeAll(outFd, serialized.data(), serialized.size());
        fsync(outFd);
        close(outFd);
        if (writeOk) {
            writeOk = (rename(tmpPath.c_str(), targetPath.c_str()) == 0);
        }
    }

    sync();
    if (bootMountedRw) {
        (void)system("/bin/mount -o remount,ro /boot 2>/dev/null");
    }
    if (lockFd >= 0) {
        flock(lockFd, LOCK_UN);
        close(lockFd);
    }

    if (!writeOk) {
        sendJsonError(clientFd, "500 Internal Server Error", "Failed to write /boot/aawgd.conf");
        return;
    }

    sendHttpResponse(clientFd, "200 OK", "application/json; charset=utf-8",
                     "{\"ok\":true,\"message\":\"Configuration saved to /boot/aawgd.conf\"}");
}

void handlePostActions(int clientFd, const std::string& body) {
    auto params = parseQueryOrForm(body);
    std::string action = params["action"];

    if (action == "restart_aawgd") {
        (void)system("(/etc/init.d/S93aawgd restart) </dev/null >/dev/null 2>&1 &");
        sendHttpResponse(clientFd, "200 OK", "application/json; charset=utf-8",
                         "{\"ok\":true,\"message\":\"Restarting aawgd service...\"}");
    } else if (action == "clear_bt_pairings") {
        (void)system("(rm -rf /persist/bluetooth/* && /etc/init.d/S93aawgd restart) </dev/null >/dev/null 2>&1 &");
        sendHttpResponse(clientFd, "200 OK", "application/json; charset=utf-8",
                         "{\"ok\":true,\"message\":\"Cleared Bluetooth pairings and restarted aawgd.\"}");
    } else if (action == "capture_snapshot_now") {
        (void)system("(flock -x /var/run/boot_mount.lock -c '/bin/mount -o remount,rw /boot 2>/dev/null && { echo \"=== MANUAL WEB SNAPSHOT $(date) ===\"; /usr/bin/bluetoothctl show 2>/dev/null; /sbin/ip addr 2>/dev/null; cat /var/log/messages 2>/dev/null; /bin/dmesg 2>/dev/null; } > /boot/car_debug.log 2>&1 && sync && /bin/mount -o remount,ro /boot 2>/dev/null') </dev/null >/dev/null 2>&1 &");
        sendHttpResponse(clientFd, "200 OK", "application/json; charset=utf-8",
                         "{\"ok\":true,\"message\":\"Captured diagnostic snapshot to /boot/car_debug.log.\"}");
    } else if (action == "reboot") {
        sendHttpResponse(clientFd, "200 OK", "application/json; charset=utf-8",
                         "{\"ok\":true,\"message\":\"Rebooting dongle in 1 second...\"}");
        (void)system("(sleep 1 && sync && /sbin/reboot) </dev/null >/dev/null 2>&1 &");
    } else {
        sendJsonError(clientFd, "400 Bad Request", "Unknown action");
    }
}

void handleClientConnection(int clientFd) {
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string request;
    request.reserve(4096);
    char buf[2048];
    size_t headerEnd = std::string::npos;

    while (headerEnd == std::string::npos && request.size() < 16384) {
        ssize_t n = read(clientFd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        request.append(buf, static_cast<size_t>(n));
        headerEnd = request.find("\r\n\r\n");
    }

    if (headerEnd == std::string::npos) {
        return;
    }

    std::string headerPart = request.substr(0, headerEnd);
    std::string bodyPart = request.substr(headerEnd + 4);

    // Parse Content-Length if POST
    size_t contentLength = 0;
    std::istringstream hStream(headerPart);
    std::string requestLine;
    std::getline(hStream, requestLine);
    requestLine = trim(requestLine);

    std::string hLine;
    while (std::getline(hStream, hLine)) {
        std::string lower = hLine;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(tolower(c)); });
        if (lower.rfind("content-length:", 0) == 0) {
            contentLength = static_cast<size_t>(atol(trim(hLine.substr(15)).c_str()));
        }
    }

    if (contentLength > 32768) {
        sendJsonError(clientFd, "413 Payload Too Large", "Request body too large");
        return;
    }

    while (bodyPart.size() < contentLength) {
        ssize_t n = read(clientFd, buf, std::min(sizeof(buf), contentLength - bodyPart.size()));
        if (n <= 0) {
            break;
        }
        bodyPart.append(buf, static_cast<size_t>(n));
    }

    std::istringstream reqStream(requestLine);
    std::string method, rawTarget;
    reqStream >> method >> rawTarget;

    std::string path = rawTarget;
    std::string queryStr;
    size_t qPos = rawTarget.find('?');
    if (qPos != std::string::npos) {
        path = rawTarget.substr(0, qPos);
        queryStr = rawTarget.substr(qPos + 1);
    }

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        std::string html = readFileText(WEB_ROOT_INDEX, 256 * 1024);
        if (html.empty()) {
            sendHttpResponse(clientFd, "404 Not Found", "text/plain; charset=utf-8", "index.html not found");
        } else {
            sendHttpResponse(clientFd, "200 OK", "text/html; charset=utf-8", html);
        }
    } else if (method == "GET" && path == "/api/v1/status") {
        sendHttpResponse(clientFd, "200 OK", "application/json; charset=utf-8", buildStatusJson());
    } else if (method == "GET" && path == "/api/v1/logs") {
        handleLogsEndpoint(clientFd, parseQueryOrForm(queryStr));
    } else if (method == "GET" && path == "/api/v1/config") {
        handleGetConfig(clientFd);
    } else if (method == "POST" && path == "/api/v1/config") {
        handlePostConfig(clientFd, bodyPart);
    } else if (method == "POST" && path == "/api/v1/actions") {
        handlePostActions(clientFd, bodyPart);
    } else {
        sendJsonError(clientFd, "404 Not Found", "Endpoint not found");
    }
}

} // namespace

int main() {
    // Lower CPU scheduling priority so Android Auto proxy always has priority
    if (nice(10) < 0) {
        // Ignore error if non-privileged
    }
    signal(SIGPIPE, SIG_IGN);

    const char* bindIpEnv = getenv("AAWG_WEB_BIND_IP");
    const char* bindIp = (bindIpEnv && bindIpEnv[0] != '\0') ? bindIpEnv : DEFAULT_BIND_IP;
    const char* portEnv = getenv("AAWG_WEB_PORT");
    int port = (portEnv && atoi(portEnv) > 0) ? atoi(portEnv) : DEFAULT_PORT;

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        perror("futedongle_web: socket failed");
        return 1;
    }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
#ifdef IP_FREEBIND
    setsockopt(serverFd, IPPROTO_IP, IP_FREEBIND, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, bindIp, &addr.sin_addr) <= 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(serverFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        // Fallback to INADDR_ANY if wlan0 does not have 10.0.0.1 assigned yet and IP_FREEBIND is unavailable
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(serverFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            perror("futedongle_web: bind failed");
            close(serverFd);
            return 1;
        }
    }

    if (listen(serverFd, 8) < 0) {
        perror("futedongle_web: listen failed");
        close(serverFd);
        return 1;
    }

    while (true) {
        struct sockaddr_in clientAddr {};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(serverFd, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
        if (clientFd < 0) {
            if (errno == EINTR) {
                continue;
            }
            sleep(1);
            continue;
        }
        handleClientConnection(clientFd);
        close(clientFd);
    }

    close(serverFd);
    return 0;
}
