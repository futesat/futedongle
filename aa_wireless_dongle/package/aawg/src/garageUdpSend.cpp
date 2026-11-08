#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fstream>
#include <netdb.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>

namespace {

// Returns true once wlan0 has at least one completed ARP entry (Flags != 0x0)
// and zero incomplete (0x0) ARP entries pending resolution.
bool isArpResolvedOnWlan0(const std::string &targetIp) {
  std::ifstream arpFile("/proc/net/arp");
  if (!arpFile.is_open()) {
    return true;
  }

  std::string line;
  if (!std::getline(arpFile, line)) {
    return true;
  }

  bool anyCompleteOnWlan0 = false;
  bool anyPendingOnWlan0 = false;
  bool targetComplete = false;

  while (std::getline(arpFile, line)) {
    std::istringstream iss(line);
    std::string ip, hwType, flags, hwAddr, mask, device;
    if (!(iss >> ip >> hwType >> flags >> hwAddr >> mask >> device)) {
      continue;
    }
    if (device != "wlan0") {
      continue;
    }
    if (flags == "0x0" || hwAddr == "00:00:00:00:00:00") {
      anyPendingOnWlan0 = true;
    } else {
      anyCompleteOnWlan0 = true;
      if (ip == targetIp) {
        targetComplete = true;
      }
    }
  }

  if (targetComplete) {
    return true;
  }
  if (anyPendingOnWlan0) {
    return false;
  }
  return anyCompleteOnWlan0;
}

} // namespace

int main(int argc, char *argv[]) {
  openlog("garage_udp_send", LOG_PID, LOG_DAEMON);

  if (argc != 4) {
    fprintf(stderr, "Usage: %s <host> <port> <payload>\n", argv[0]);
    return 1;
  }

  const char *host = argv[1];
  const char *portStr = argv[2];
  const char *payload = argv[3];
  const size_t payloadLen = strlen(payload);

  char *endPtr = nullptr;
  long portNum = strtol(portStr, &endPtr, 10);
  if (portStr[0] == '\0' || *endPtr != '\0' || portNum < 1 || portNum > 65535) {
    syslog(LOG_WARNING, "Invalid UDP port: %s", portStr);
    return 2;
  }

  struct sockaddr_in destAddr {};
  destAddr.sin_family = AF_INET;
  destAddr.sin_port = htons(static_cast<uint16_t>(portNum));

  // Fast-path numeric IPv4 addresses without touching NSS/DNS resolver
  if (inet_pton(AF_INET, host, &destAddr.sin_addr) != 1) {
    setenv("RES_OPTIONS", "timeout:1 attempts:1", 1);
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    struct addrinfo *res = nullptr;
    int gaiErr = getaddrinfo(host, nullptr, &hints, &res);
    if (gaiErr != 0 || res == nullptr) {
      syslog(LOG_WARNING, "Failed to resolve %s: %s", host,
             gai_strerror(gaiErr));
      return 2;
    }
    destAddr.sin_addr =
        reinterpret_cast<const struct sockaddr_in *>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
  }

  char resolvedIp[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &destAddr.sin_addr, resolvedIp, sizeof(resolvedIp));

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    syslog(LOG_WARNING, "socket() failed: %s", strerror(errno));
    return 3;
  }

  int broadcastEnable = 1;
  (void)setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable,
                   sizeof(broadcastEnable));
#ifdef SO_BINDTODEVICE
  (void)setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, "wlan0", 5);
#endif
  int tosVoice = 0xB8; // DSCP EF -> 802.11e WMM AC_VO (Voice priority)
  (void)setsockopt(sock, IPPROTO_IP, IP_TOS, &tosVoice, sizeof(tosVoice));
#ifdef SO_PRIORITY
  int skbPrio = 6;
  (void)setsockopt(sock, SOL_SOCKET, SO_PRIORITY, &skbPrio, sizeof(skbPrio));
#endif

  struct timeval tv {};
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (connect(sock, reinterpret_cast<const struct sockaddr *>(&destAddr),
              sizeof(destAddr)) < 0) {
    syslog(LOG_WARNING, "connect(%s:%ld) failed: %s", resolvedIp, portNum,
           strerror(errno));
    close(sock);
    return 4;
  }

  ssize_t sent = send(sock, payload, payloadLen, 0);
  if (sent < 0 || static_cast<size_t>(sent) != payloadLen) {
    syslog(LOG_WARNING, "send() failed (%zd/%zu bytes): %s", sent, payloadLen,
           strerror(errno));
    close(sock);
    return 5;
  }

  // Wait up to 800ms for Layer-2 ARP resolution on wlan0 so the kernel
  // neighbor queue flushes the datagram before wlan0 is torn down.
  for (int i = 0; i < 40; ++i) {
    if (isArpResolvedOnWlan0(resolvedIp)) {
      break;
    }
    usleep(20000);
  }

  // Send three additional confirmatory UDP datagrams now that Layer-2 ARP and
  // WPA2 PTK keys are ready to guard against transient 2.4 GHz RF collisions.
  for (int retry = 0; retry < 3; ++retry) {
    usleep(45000);
    if (send(sock, payload, payloadLen, 0) < 0 && errno == ECONNREFUSED) {
      (void)send(sock, payload, payloadLen, 0);
    }
  }

  // Allow 120ms for brcmfmac SDIO TX queue and 802.11 ACK completion.
  usleep(120000);

  syslog(LOG_INFO, "Sent %zu UDP bytes (4x burst) to %s:%ld", payloadLen,
         resolvedIp, portNum);
  close(sock);
  closelog();
  return 0;
}
