#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <thread>
#include <optional>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "common.h"
#include "usb.h"
#include "bluetoothHandler.h"
#include "proxyHandler.h"

namespace {
std::atomic<bool> s_garage_opener_fired_this_boot{false};

int runGarageOpenerFire() {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        // Close all inherited descriptors >= 3 before exec so child daemons
        // (hostapd, dnsmasq) never hold open D-Bus, Netlink, or USB descriptors,
        // without touching /bin/sh's own script descriptor after exec.
        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0 || max_fd > 1024) {
            max_fd = 1024;
        }
        for (int fd = 3; fd < static_cast<int>(max_fd); ++fd) {
            close(fd);
        }
        execl("/etc/init.d/S38garage_opener", "S38garage_opener", "fire", static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
} // namespace

void empty_signal_handler(int signal) {
    // Empty. We don't want to do anything but interrupt the thread.
}

ssize_t AAWProxy::readFully(int fd, unsigned char *buffer, size_t nbyte) {
    size_t remaining_bytes = nbyte;
    while (remaining_bytes > 0) {
        ssize_t len = read(fd, buffer, remaining_bytes);

        if (len <= 0) {
            // Error, cannot read more.
            return len;
        }

        buffer += len;
        remaining_bytes -= len;
    }

    return nbyte;
}

ssize_t AAWProxy::writeFully(int fd, const unsigned char *buffer, size_t nbyte) {
    size_t remaining_bytes = nbyte;
    while (remaining_bytes > 0) {
        ssize_t len = write(fd, buffer, remaining_bytes);

        if (len <= 0) {
            return len;
        }

        buffer += len;
        remaining_bytes -= len;
    }

    return nbyte;
}

ssize_t AAWProxy::readMessage(int fd, unsigned char *buffer, size_t buffer_len) {
    size_t header_length = 4;
    if (ssize_t len = readFully(fd, buffer, header_length); len <= 0) {
        return len;
    }

    size_t message_length = (buffer[2] << 8) + buffer[3];

    constexpr char FRAME_TYPE_FIRST = 1 << 0;
    constexpr char FRAME_TYPE_LAST = 1 << 1;
    constexpr char FRAME_TYPE_MASK = FRAME_TYPE_FIRST | FRAME_TYPE_LAST;
    if ((buffer[1] & FRAME_TYPE_MASK) == FRAME_TYPE_FIRST) { // This means the header is 8 bytes long, we need to read four more bytes.
        message_length += 4;
    }

    if ((header_length + message_length) > buffer_len) {
        // Not enough space in the buffer. This is unexpected.
        errno = EMSGSIZE;
        return -1;
    }

    if (ssize_t len = readFully(fd, buffer + header_length, message_length); len <= 0) {
        return len;
    }

    return header_length + message_length;
}

void AAWProxy::forward(ProxyDirection direction, std::atomic<bool>& should_exit) {
    constexpr size_t MAX_TCP_FRAME_BUFFER_LEN = 65544; // 65535 max 16-bit payload + up to 8-byte header
    constexpr size_t USB_BULK_BUFFER_LEN = 16384;
    std::vector<unsigned char> buffer(MAX_TCP_FRAME_BUFFER_LEN);

    bool read_message;
    int read_fd, write_fd;
    std::string read_name, write_name;
    switch (direction) {
        case ProxyDirection::TCP_to_USB:
            read_message = true;

            read_fd = m_tcp_fd;
            read_name = "TCP";

            write_fd = m_usb_fd;
            write_name = "USB";
            break;
        case ProxyDirection::USB_to_TCP:
            read_message = false;

            read_fd = m_usb_fd;
            read_name = "USB";

            write_fd = m_tcp_fd;
            write_name = "TCP";
            break;
    }

    const bool monitor_garage_video =
        (direction == ProxyDirection::TCP_to_USB) &&
        !s_garage_opener_fired_this_boot.load() &&
        (access("/var/run/garage_in_range", F_OK) == 0) &&
        (access("/var/run/garage_opened_once", F_OK) != 0);
    int video_channel_id = -1;
    int foreground_video_frames = 0;

    while (!should_exit) {
        // Read
        ssize_t len = read_message
            ? readMessage(read_fd, buffer.data(), buffer.size())
            : read(read_fd, buffer.data(), USB_BULK_BUFFER_LEN);

        if (len <= 0) {
            // Start logging read/write details if there is an error.
            m_log_communication = true;
        }
        if (m_log_communication) {
            Logger::instance()->info("%d bytes read from %s\n", len, read_name.c_str());
        }

        if (len < 0) {
            Logger::instance()->info("Read from %s failed: %s\n", read_name.c_str(), strerror(errno));
            break;
        }
        else if (len == 0) {
            break;
        }
        else if (should_exit) {
            break;
        }

        // Write
        ssize_t wlen = writeFully(write_fd, buffer.data(), len);

        if (wlen <= 0) {
            // Start logging read/write details if there is an error.
            m_log_communication = true;
        }
        if (m_log_communication) {
            Logger::instance()->info("%d bytes written to %s\n", wlen, write_name.c_str());
        }

        if (wlen < 0) {
            Logger::instance()->info("Write to %s failed: %s\n", write_name.c_str(), strerror(errno));
            break;
        }
        else if (should_exit) {
            break;
        }

        // Detect active H.264 video projection on the MIB3 display:
        // - buffer[0] != 0 excludes Control/SSL/ServiceDiscovery (Channel 0).
        // - (buffer[1] & 0x0C) == 0x08 requires ENCRYPTED (0x08) and non-CONTROL (0x04 == 0).
        // - Locking onto video_channel_id requires an H.264 IDR keyframe/large slice (len > 9000),
        //   which uncompressed PCM/AAC background audio frames (<= 8 KB) never reach.
        // - Once video_channel_id is locked, count all encrypted media frames (len >= 64) on that
        //   channel so stationary P-frames in a parked car still trigger in ~0.4s (12 frames).
        if (monitor_garage_video && buffer[0] != 0 && (buffer[1] & 0x0C) == 0x08) {
            if (video_channel_id < 0 && len > 9000) {
                video_channel_id = static_cast<int>(buffer[0]);
            }
            if (video_channel_id == static_cast<int>(buffer[0]) && len >= 64) {
                ++foreground_video_frames;
                if (foreground_video_frames >= 12) {
                    bool expected = false;
                    if (s_garage_opener_fired_this_boot.compare_exchange_strong(expected, true)) {
                        Logger::instance()->info(
                            "Foreground Android Auto H.264 video detected in garage (ch=%d, %d frames); triggering garage door opener\n",
                            video_channel_id, foreground_video_frames);
                        m_triggered_garage_opener = true;
                        break;
                    }
                }
            }
        }
    }

    stopForwarding(should_exit);

    {
        std::lock_guard<std::mutex> lock(m_exit_mutex);
        if (direction == ProxyDirection::USB_to_TCP) {
            m_usb_tcp_done = true;
        } else {
            m_tcp_usb_done = true;
        }
    }
    m_exit_cv.notify_all();
}

void AAWProxy::stopForwarding(std::atomic<bool>& should_exit) {
    Logger::instance()->info("Interrupting threads to stop forwarding\n");
    should_exit = true;

    if (m_tcp_fd >= 0) {
        shutdown(m_tcp_fd, SHUT_RDWR);
    }

    m_exit_cv.notify_all();
}

constexpr uint16_t default_version_major = 1;
constexpr uint16_t default_version_minor = 7;

bool AAWProxy::openUsbAccessory() {
    Logger::instance()->info("Opening usb accessory\n");

    if ((m_usb_fd = open("/dev/usb_accessory", O_RDWR)) < 0) {
        Logger::instance()->info("error opening /dev/usb_accessory: %s\n", strerror(errno));
        return false;
    }

    return true;
}

bool AAWProxy::shouldDoEarlyVersionExchange() {
    return Config::instance()->getEarlyUsbVersionExchange();
}

bool AAWProxy::performUsbVersionExchange() {
    Logger::instance()->info("Attempting early version exchange with USB\n");

    // Receive version request
    size_t buffer_len = 16;
    unsigned char buffer[buffer_len];

    ssize_t len = read(m_usb_fd, buffer, buffer_len);
    Logger::instance()->info("%zd bytes read from USB\n", len);
    if (len < 0) {
        Logger::instance()->info("Read from USB failed: %s\n", strerror(errno));
        return false;
    }
    else if (len != 10) {
        Logger::instance()->info("Version exchange with USB failed: expected 10 bytes, got %zd\n", len);
        return false;
    }

    if (!(buffer[0] == 0 && buffer[1] == 3) // First two bytes of header are 0 and 3 (FRAME_TYPE_FIRST | FRAME_TYPE_LAST)
        || !(buffer[2] == 0 && buffer[3] == 6) // Length is 6
        || !(buffer[4] == 0 && buffer[5] == 1) // Control message id is 1 (VERSION_REQUEST)
    ) {
        Logger::instance()->info("Version exchange with USB failed: malformed message\n");
        return false;
    }

    usb_version_major = (buffer[6] << 8) + buffer[7];
    usb_version_minor = (buffer[8] << 8) + buffer[9];
    Logger::instance()->info("USB reported version: %d.%d\n", usb_version_major, usb_version_minor);

    // Send version response
    unsigned char version_response_buffer[12] = {
        0x00, 0x03, // Header - FRAME_TYPE_FIRST | FRAME_TYPE_LAST
        0x00, 0x08, // Payload length
        0x00, 0x02, // Control message id 2 (VERSION_RESPONSE)
        (unsigned char)((default_version_major & 0xff00) >> 8), (unsigned char)(default_version_major & 0xff),    // Major version
        (unsigned char)((default_version_minor & 0xff00) >> 8), (unsigned char)(default_version_minor & 0xff),    // Minor version
        0x00, 0x00  // Version response status (0 = Match, 0xFFFF = Mismatch)
    };
    ssize_t wlen = writeFully(m_usb_fd, version_response_buffer, sizeof(version_response_buffer));
    Logger::instance()->info("%zd bytes written to USB\n", wlen);
    if (wlen < 0) {
        Logger::instance()->info("Write to USB failed: %s\n", strerror(errno));
        return false;
    }

    Logger::instance()->info("Version exchange completed with USB\n");
    return true;
}

bool AAWProxy::performTcpVersionExchange() {
    Logger::instance()->info("Performing version exchange with TCP\n");

    // Send version request
    unsigned char version_request_buffer[10] = {
        0x00, 0x03, // Header - FRAME_TYPE_FIRST | FRAME_TYPE_LAST
        0x00, 0x06, // Payload length
        0x00, 0x01, // Control message id 1 (VERSION_REQUEST)
        (unsigned char)((usb_version_major & 0xff00) >> 8), (unsigned char)(usb_version_major & 0xff),    // Major version
        (unsigned char)((usb_version_minor & 0xff00) >> 8), (unsigned char)(usb_version_minor & 0xff),    // Minor version
    };
    ssize_t wlen = writeFully(m_tcp_fd, version_request_buffer, sizeof(version_request_buffer));
    Logger::instance()->info("%zd bytes written to TCP\n", wlen);
    if (wlen < 0) {
        Logger::instance()->info("Write to TCP failed: %s\n", strerror(errno));
        return false;
    }

    // Receive version response
    constexpr size_t expected_response_len = 12;
    unsigned char buffer[16];

    ssize_t len = readFully(m_tcp_fd, buffer, expected_response_len);
    Logger::instance()->info("%zd bytes read from TCP\n", len);
    if (len < 0) {
        Logger::instance()->info("Read from TCP failed: %s\n", strerror(errno));
        return false;
    }
    else if (len != (ssize_t)expected_response_len) {
        Logger::instance()->info("Version exchange with TCP failed: expected 12 bytes, got %zd\n", len);
        return false;
    }

    if (!(buffer[0] == 0 && buffer[1] == 3) // First two bytes of header are 0 and 3 (FRAME_TYPE_FIRST | FRAME_TYPE_LAST)
        || !(buffer[2] == 0 && buffer[3] == 8) // Length is 8
        || !(buffer[4] == 0 && buffer[5] == 2) // Control message id is 2 (VERSION_RESPONSE)
    ) {
        Logger::instance()->info("Version exchange with TCP failed: malformed message\n");
        return false;
    }

    uint16_t tcp_version_major = (buffer[6] << 8) + buffer[7];
    uint16_t tcp_version_minor = (buffer[8] << 8) + buffer[9];
    Logger::instance()->info("TCP reported version: %d.%d\n", tcp_version_major, tcp_version_minor);

    if (tcp_version_major != default_version_major || tcp_version_minor != default_version_minor) {
        Logger::instance()->info("Version from TCP does not match version earlier reported (%d.%d) to USB!\n", default_version_major, default_version_minor);
    }

    uint16_t tcp_version_response_status = (buffer[10] << 8) + buffer[11];
    switch (tcp_version_response_status) {
        case 0: // Match
            break;
        case 0xFF:  // Mismatch
            Logger::instance()->info("TCP reported version mismatch\n");
            break;
        default:
            Logger::instance()->info("TCP reported unknown version response status\n");
            break;
    }

    Logger::instance()->info("Version exchange completed with TCP\n");
    return true;
}

void AAWProxy::handleClient(int server_sock) {
    bool earlyVersionExchange = shouldDoEarlyVersionExchange();
    if (earlyVersionExchange) {
        if (Config::instance()->getConnectionStrategy() != ConnectionStrategy::USB_FIRST) {
            if (!UsbManager::instance().enableDefaultAndWaitForAccessory(std::chrono::seconds(30))) {
                close(server_sock);
                return;
            }
        }

        if (!openUsbAccessory()) {
            close(server_sock);
            return;
        }

        if (!performUsbVersionExchange()) {
            close(m_usb_fd);
            m_usb_fd = -1;
            close(server_sock);
            return;
        }
    }

    struct sockaddr client_address;
    socklen_t client_addresslen = sizeof(client_address);
    if ((m_tcp_fd = accept(server_sock, &client_address, &client_addresslen)) < 0) {
        close(server_sock);
        if (m_usb_fd >= 0) {
            close(m_usb_fd);
            m_usb_fd = -1;
        }
        Logger::instance()->info("accept failed: %s\n", strerror(errno));
        return;
    }

    close(server_sock);

    Logger::instance()->info("Tcp server accepted connection\n");

    // Disable Nagle's algorithm to eliminate touch and audio latency
    int flag = 1;
    if (setsockopt(m_tcp_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag)) < 0) {
        Logger::instance()->info("setsockopt TCP_NODELAY failed: %s\n", strerror(errno));
    } else {
        Logger::instance()->info("TCP_NODELAY enabled on client socket\n");
    }

    // Set read/write timeouts on the TCP socket before version exchange and forwarding
    struct timeval tv = {
        .tv_sec = 10,
        .tv_usec = 0,
    };

    if (setsockopt(m_tcp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) ||
        setsockopt(m_tcp_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))) {
        Logger::instance()->info("setsockopt timeout failed: %s\n", strerror(errno));
        if (m_usb_fd >= 0) {
            close(m_usb_fd);
            m_usb_fd = -1;
        }
        close(m_tcp_fd);
        m_tcp_fd = -1;
        return;
    }

    // Phone connected via TCP, we can stop retrying bluetooth connection
    BluetoothHandler::instance().stopConnectWithRetry();

    if (earlyVersionExchange) {
        if (!performTcpVersionExchange()) {
            close(m_usb_fd);
            m_usb_fd = -1;
            close(m_tcp_fd);
            m_tcp_fd = -1;
            return;
        }
    } else {
        if (Config::instance()->getConnectionStrategy() != ConnectionStrategy::USB_FIRST) {
            if (!UsbManager::instance().enableDefaultAndWaitForAccessory(std::chrono::seconds(30))) {
                close(m_tcp_fd);
                m_tcp_fd = -1;
                return;
            }
        }

        if (!openUsbAccessory()) {
            close(m_tcp_fd);
            m_tcp_fd = -1;
            return;
        }
    }

    // Setup signal handler
    struct sigaction sa;
    sa.sa_handler = empty_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL)) {
        Logger::instance()->info("Adding signal handler failed: %s\n", strerror(errno));
    }

    Logger::instance()->info("Forwarding data between TCP and USB\n");
    std::atomic<bool> should_exit = false;
    {
        std::lock_guard<std::mutex> lock(m_exit_mutex);
        m_usb_tcp_done = false;
        m_tcp_usb_done = false;
    }
    m_usb_tcp_thread = std::thread(&AAWProxy::forward, this, ProxyDirection::USB_to_TCP, std::ref(should_exit));
    m_tcp_usb_thread = std::thread(&AAWProxy::forward, this, ProxyDirection::TCP_to_USB, std::ref(should_exit));

    {
        std::unique_lock<std::mutex> lock(m_exit_mutex);
        // Sleep with 0% CPU until either forwarding thread requests exit
        m_exit_cv.wait(lock, [&]() {
            return should_exit.load() || (m_usb_tcp_done && m_tcp_usb_done);
        });

        // Once exit is requested, pulse SIGUSR1 every 25ms under m_exit_mutex
        // to wake any thread still blocked in read(/dev/usb_accessory).
        while (!m_usb_tcp_done || !m_tcp_usb_done) {
            if (m_usb_tcp_thread && !m_usb_tcp_done) {
                pthread_kill(m_usb_tcp_thread->native_handle(), SIGUSR1);
            }
            if (m_tcp_usb_thread && !m_tcp_usb_done) {
                pthread_kill(m_tcp_usb_thread->native_handle(), SIGUSR1);
            }
            m_exit_cv.wait_for(lock, std::chrono::milliseconds(25), [&]() {
                return m_usb_tcp_done && m_tcp_usb_done;
            });
        }
    }

    m_usb_tcp_thread->join();
    m_usb_tcp_thread = std::nullopt;

    m_tcp_usb_thread->join();
    m_tcp_usb_thread = std::nullopt;

    signal(SIGUSR1, SIG_DFL);

    close(m_usb_fd);
    m_usb_fd = -1;

    if (m_triggered_garage_opener.load()) {
        shutdown(m_tcp_fd, SHUT_RDWR);
    }
    close(m_tcp_fd);
    m_tcp_fd = -1;

    usb_version_major = 0;
    usb_version_minor = 0;

    Logger::instance()->info("Forwarding stopped\n");

    if (m_triggered_garage_opener.load()) {
        // Detach USB gadget immediately so the head unit cleanly exits the first session,
        // and allow 100ms for the kernel to transmit TCP FIN over 5 GHz before switching wlan0.
        UsbManager::instance().disableGadget();
        usleep(100000);
        Logger::instance()->info("Executing runtime garage door opener sequence (/etc/init.d/S38garage_opener fire)\n");
        int rc = runGarageOpenerFire();
        Logger::instance()->info("Garage door opener sequence completed (rc=%d); restarting Android Auto session\n", rc);
    }
}

std::optional<std::thread> AAWProxy::startServer(int32_t port) {
    Logger::instance()->info("Starting tcp server\n");
    int server_sock;
    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        Logger::instance()->info("creating socket failed: %s\n", strerror(errno));
        return std::nullopt;
    }

    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        Logger::instance()->info("setsockopt failed: %s\n", strerror(errno));
        close(server_sock);
        return std::nullopt;
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_sock, (struct sockaddr*)&address, sizeof(address)) < 0) {
        Logger::instance()->info("bind failed: %s\n", strerror(errno));
        close(server_sock);
        return std::nullopt;
    }

    if (listen(server_sock, 3) < 0) {
        Logger::instance()->info("listen failed: %s\n", strerror(errno));
        close(server_sock);
        return std::nullopt;
    }

    Logger::instance()->info("Tcp server listening on %d\n", port);

    return std::thread(&AAWProxy::handleClient, this, server_sock);
}
