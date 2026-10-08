#include "headset.h"

#include <endian.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

/* kernel Bluetooth socket ABI, declared here to avoid a libbluetooth dependency */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_L2CAP    0
#define BDADDR_LE_PUBLIC 0x01
#define BDADDR_LE_RANDOM 0x02
#define L2CAP_CID_ATT    0x0004

typedef struct {
    uint8_t b[6];
} __attribute__((packed)) bt_addr_t;

struct sockaddr_l2_le {
    sa_family_t    l2_family;
    unsigned short l2_psm;
    bt_addr_t      l2_bdaddr;
    unsigned short l2_cid;
    uint8_t        l2_bdaddr_type;
};

#define ATT_ERROR_RSP             0x01
#define ATT_MTU_REQ               0x02
#define ATT_MTU_RSP               0x03
#define ATT_FIND_INFO_REQ         0x04
#define ATT_FIND_BY_TYPE_REQ      0x06
#define ATT_READ_BY_TYPE_REQ      0x08
#define ATT_READ_BY_TYPE_RSP      0x09
#define ATT_READ_BY_GROUP_REQ     0x10
#define ATT_HANDLE_IND            0x1D
#define ATT_HANDLE_CONF           0x1E
#define ATT_ECODE_REQ_NOT_SUPP    0x06
#define ATT_ECODE_ATTR_NOT_FOUND  0x0A
#define ATT_DEFAULT_MTU           23
#define UUID_BATTERY_LEVEL        0x2A19

/* advertised by Sennheiser (Sonova) headphones, see the Bluetooth SIG member UUIDs */
#define SONOVA_SERVICE_UUID "0000fcfe-0000-1000-8000-00805f9b34fb"

#define DEFAULT_INTERVAL_S   300
#define MIN_INTERVAL_S       60
#define MAX_INTERVAL_S       (7L * 24 * 3600)
#define SETTLE_MS            15000   /* after the headphones link up */
#define IDLE_SETTLE_MS       5000    /* after the dongle stops streaming */
#define RETRY_MS             60000
#define CONNECT_TIMEOUT_MS   15000
#define RESPONSE_TIMEOUT_MS  5000
#define DISCOVERY_MS         30000
#define DISCOVERY_POLL_MS    2000
#define DISCOVERY_RETRY_MS   (10 * 60 * 1000)
#define BUS_TIMEOUT_US       (2 * 1000 * 1000)
#define FORGET_AFTER_TIMEOUTS 12

enum { R_IDLE, R_CONNECTING, R_WAITING };

static int g_enabled = 1;
static long g_interval_ms = DEFAULT_INTERVAL_S * 1000L;
static char g_cache_path[PATH_MAX];
static char g_setting_path[PATH_MAX];

static int g_have_addr;
static int g_addr_from_env;
static char g_addr_str[18];
static char g_rejected[18];   /* given up on, skipped by discovery until Refresh */
static bt_addr_t g_addr;
static uint8_t g_addr_type = BDADDR_LE_PUBLIC;

static int g_battery = -1;
static uint64_t g_battery_time;

static int g_was_up;
static int g_was_idle;
static long g_next_read;
static int g_fails;
static int g_timeouts;

static int g_rstate = R_IDLE;
static int g_fd = -1;
static long g_deadline;

static sd_bus* g_sys;
static char g_adapter[64];
static char g_disc_adapter[64];
static int g_discovering;
static long g_disc_deadline;
static long g_disc_next_poll;
static long g_disc_next_allowed;

static long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* "AA:BB:CC:DD:EE:FF" to the little-endian byte order the kernel expects */
static int parse_addr(const char* s, bt_addr_t* out) {
    unsigned v[6];
    char tail;
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6)
        return -1;
    for (int i = 0; i < 6; i++) out->b[5 - i] = (uint8_t)v[i];
    return 0;
}

static int set_addr(const char* addr, int type) {
    bt_addr_t a;
    if (strlen(addr) != 17 || parse_addr(addr, &a) < 0) return -1;
    g_addr = a;
    g_addr_type = (uint8_t)type;
    snprintf(g_addr_str, sizeof(g_addr_str), "%s", addr);
    g_have_addr = 1;
    return 0;
}

static void mkdir_parents(const char* path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0700);
        *p = '/';
    }
}

static void save_addr(void) {
    if (!g_cache_path[0]) return;

    mkdir_parents(g_cache_path);
    FILE* fp = fopen(g_cache_path, "w");
    if (!fp) {
        fprintf(stderr, "btd700d: cannot write %s: %s\n", g_cache_path, strerror(errno));
        return;
    }
    fprintf(fp, "%s %s\n", g_addr_str, g_addr_type == BDADDR_LE_RANDOM ? "random" : "public");
    if (fclose(fp) != 0)
        fprintf(stderr, "btd700d: cannot write %s: %s\n", g_cache_path, strerror(errno));
}

static void load_addr(void) {
    if (!g_cache_path[0]) return;

    FILE* fp = fopen(g_cache_path, "r");
    if (!fp) return;

    char addr[32] = "", type[16] = "";
    if (fscanf(fp, "%31s %15s", addr, type) >= 1 &&
        set_addr(addr, strcmp(type, "random") == 0 ? BDADDR_LE_RANDOM : BDADDR_LE_PUBLIC) == 0)
        fprintf(stderr, "btd700d: remembered headphones at %s\n", g_addr_str);
    fclose(fp);
}

static void save_setting(void) {
    if (!g_setting_path[0]) return;

    mkdir_parents(g_setting_path);
    FILE* fp = fopen(g_setting_path, "w");
    if (!fp) {
        fprintf(stderr, "btd700d: cannot write %s: %s\n", g_setting_path, strerror(errno));
        return;
    }
    fprintf(fp, "%s\n", g_enabled ? "on" : "off");
    if (fclose(fp) != 0)
        fprintf(stderr, "btd700d: cannot write %s: %s\n", g_setting_path, strerror(errno));
}

static void load_setting(void) {
    if (!g_setting_path[0]) return;

    FILE* fp = fopen(g_setting_path, "r");
    if (!fp) return;

    char word[8] = "";
    if (fscanf(fp, "%7s", word) == 1 && strcmp(word, "off") == 0) g_enabled = 0;
    fclose(fp);
}

static void forget_addr(void) {
    snprintf(g_rejected, sizeof(g_rejected), "%s", g_addr_str);
    g_have_addr = 0;
    g_timeouts = 0;
    if (g_cache_path[0]) unlink(g_cache_path);
}

/* ------------------------------------------------------------------------- */
/* discovery through bluetoothd: a UUID filter also reports the headphones,
 * which do not set the LE discoverable flag while linked */

/* what may be NULL to stay quiet. never auto-starts a bluetoothd the user stopped */
static int bluez_call(sd_bus_message* m, sd_bus_message** reply, const char* what) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message_set_auto_start(m, 0);
    int r = sd_bus_call(g_sys, m, BUS_TIMEOUT_US, &err, reply);
    if (r < 0 && what)
        fprintf(stderr, "btd700d: %s: %s\n", what, err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return r;
}

static int adapter_call(const char* adapter, const char* member, int quiet) {
    sd_bus_message* m = NULL;
    int r = sd_bus_message_new_method_call(g_sys, &m, "org.bluez", adapter,
                                           "org.bluez.Adapter1", member);
    if (r >= 0) r = bluez_call(m, NULL, quiet ? NULL : member);
    sd_bus_message_unref(m);
    return r;
}

typedef struct {
    char addr[18];
    char name[64];
    int type;
    int rssi;
} candidate_t;

static int parse_device(sd_bus_message* m, candidate_t* best, int* found) {
    candidate_t c = { .type = BDADDR_LE_PUBLIC, .rssi = -128 };
    int sonova = 0, have_rssi = 0;
    const char* s;

    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char* key;
        r = sd_bus_message_read(m, "s", &key);
        if (r < 0) return r;

        if (strcmp(key, "Address") == 0) {
            r = sd_bus_message_read(m, "v", "s", &s);
            if (r >= 0) snprintf(c.addr, sizeof(c.addr), "%s", s);
        } else if (strcmp(key, "AddressType") == 0) {
            r = sd_bus_message_read(m, "v", "s", &s);
            if (r >= 0) c.type = strcmp(s, "random") == 0 ? BDADDR_LE_RANDOM : BDADDR_LE_PUBLIC;
        } else if (strcmp(key, "Name") == 0) {
            r = sd_bus_message_read(m, "v", "s", &s);
            if (r >= 0) snprintf(c.name, sizeof(c.name), "%s", s);
        } else if (strcmp(key, "RSSI") == 0) {
            int16_t rssi;
            r = sd_bus_message_read(m, "v", "n", &rssi);
            if (r >= 0) {
                c.rssi = rssi;
                have_rssi = 1;
            }
        } else if (strcmp(key, "UUIDs") == 0) {
            r = sd_bus_message_enter_container(m, 'v', "as");
            if (r >= 0) r = sd_bus_message_enter_container(m, 'a', "s");
            while (r >= 0 && (r = sd_bus_message_read(m, "s", &s)) > 0)
                if (strcasecmp(s, SONOVA_SERVICE_UUID) == 0) sonova = 1;
            if (r >= 0) r = sd_bus_message_exit_container(m);
            if (r >= 0) r = sd_bus_message_exit_container(m);
        } else {
            r = sd_bus_message_skip(m, "v");
        }
        if (r < 0) return r;

        r = sd_bus_message_exit_container(m);
        if (r < 0) return r;
    }
    if (r < 0) return r;

    r = sd_bus_message_exit_container(m);
    if (r < 0) return r;

    /* Only devices advertising right now count (bluetoothd drops RSSI after a
     * scan), so a stale entry is never picked. Headphones paired over BR/EDR
     * report their SDP UUIDs instead of the advertised one, hence the name.
     * The closest one wins if several Sennheiser devices are around. */
    if (!sonova && strncasecmp(c.name, "HDB", 3) == 0) sonova = 1;
    if (sonova && have_rssi && c.addr[0] && strcasecmp(c.addr, g_rejected) != 0 &&
        (!*found || c.rssi > best->rssi)) {
        *best = c;
        *found = 1;
    }
    return 0;
}

static int parse_interfaces(sd_bus_message* m, const char* path, char* adapter, size_t adapter_size,
                            candidate_t* best, int* found) {
    int r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}");
    if (r < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
        const char* iface;
        r = sd_bus_message_read(m, "s", &iface);
        if (r < 0) return r;

        if (strcmp(iface, "org.bluez.Adapter1") == 0 && !adapter[0])
            snprintf(adapter, adapter_size, "%s", path);

        if (strcmp(iface, "org.bluez.Device1") == 0)
            r = parse_device(m, best, found);
        else
            r = sd_bus_message_skip(m, "a{sv}");
        if (r < 0) return r;

        r = sd_bus_message_exit_container(m);
        if (r < 0) return r;
    }
    if (r < 0) return r;
    return sd_bus_message_exit_container(m);
}

/* looks for a Sennheiser device in bluetoothd's object tree, also records the
 * first adapter (only from a complete reply). returns 1 if found, 0 if not,
 * negative on error */
static int find_headset(candidate_t* best) {
    sd_bus_message* m = NULL;
    sd_bus_message* reply = NULL;
    char adapter[sizeof(g_adapter)] = "";
    int found = 0;

    int r = sd_bus_message_new_method_call(g_sys, &m, "org.bluez", "/",
                                           "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (r >= 0) r = bluez_call(m, &reply, "GetManagedObjects");
    sd_bus_message_unref(m);
    if (r < 0) return r;

    r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    while (r >= 0 && (r = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}")) > 0) {
        const char* path;
        r = sd_bus_message_read(reply, "o", &path);
        if (r >= 0) r = parse_interfaces(reply, path, adapter, sizeof(adapter), best, &found);
        if (r >= 0) r = sd_bus_message_exit_container(reply);
    }
    sd_bus_message_unref(reply);

    if (r < 0) {
        fprintf(stderr, "btd700d: cannot parse bluetoothd objects: %s\n", strerror(-r));
        return r;
    }
    memcpy(g_adapter, adapter, sizeof(g_adapter));
    return found;
}

static void stop_discovery(void) {
    if (!g_discovering) return;
    g_discovering = 0;
    if (g_sys) adapter_call(g_disc_adapter, "StopDiscovery", 0);
}

static int adopt_candidate(void) {
    candidate_t c;
    if (find_headset(&c) != 1) return 0;
    if (set_addr(c.addr, c.type) < 0) return 0;

    fprintf(stderr, "btd700d: found headphones %s (%s) over LE\n",
            g_addr_str, c.name[0] ? c.name : "no name");
    save_addr();
    return 1;
}

static void start_discovery(long now) {
    if (now < g_disc_next_allowed) return;
    g_disc_next_allowed = now + DISCOVERY_RETRY_MS;

    if (!g_sys) {
        fprintf(stderr, "btd700d: no system bus, cannot look for the headphones over LE "
                        "(set BTD700_HEADSET_ADDRESS)\n");
        return;
    }

    /* a device bluetoothd already saw with the Sennheiser UUID needs no scan */
    if (adopt_candidate()) return;
    if (!g_adapter[0]) {
        fprintf(stderr, "btd700d: no Bluetooth adapter in bluetoothd, battery level unavailable\n");
        return;
    }

    sd_bus_message* m = NULL;
    int r = sd_bus_message_new_method_call(g_sys, &m, "org.bluez", g_adapter,
                                           "org.bluez.Adapter1", "SetDiscoveryFilter");
    if (r >= 0)
        r = sd_bus_message_append(m, "a{sv}", 2,
                                  "UUIDs", "as", 1, SONOVA_SERVICE_UUID,
                                  "Transport", "s", "le");
    if (r >= 0) r = bluez_call(m, NULL, "SetDiscoveryFilter");
    sd_bus_message_unref(m);

    memcpy(g_disc_adapter, g_adapter, sizeof(g_disc_adapter));
    if (r >= 0) r = adapter_call(g_disc_adapter, "StartDiscovery", 0);
    /* a late reply may still have started the scan, so let the deadline stop it */
    if (r < 0 && r != -ETIMEDOUT) {
        /* clears a scan of ours bluetoothd might still hold (it then says InProgress) */
        adapter_call(g_disc_adapter, "StopDiscovery", 1);
        fprintf(stderr, "btd700d: LE scan failed, is Bluetooth on and LE enabled "
                        "(ControllerMode in /etc/bluetooth/main.conf)?\n");
        return;
    }

    fprintf(stderr, "btd700d: looking for the headphones over LE\n");
    g_discovering = 1;
    g_disc_deadline = now + DISCOVERY_MS;
    g_disc_next_poll = now + DISCOVERY_POLL_MS;
}

static void step_discovery(long now) {
    if (now >= g_disc_next_poll) {
        g_disc_next_poll = now + DISCOVERY_POLL_MS;
        if (adopt_candidate()) {
            stop_discovery();
            g_next_read = now + IDLE_SETTLE_MS;
            return;
        }
    }
    if (now >= g_disc_deadline) {
        stop_discovery();
        fprintf(stderr, "btd700d: headphones not found over LE, trying again in %d min\n",
                DISCOVERY_RETRY_MS / 60000);
    }
}

/* ------------------------------------------------------------------------- */
/* the read: connect the ATT channel, Read By Type for the Battery Level UUID,
 * which returns the value without a service discovery */

static void close_read(void) {
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    g_rstate = R_IDLE;
}

static void read_ok(int level, long now) {
    close_read();
    g_fails = 0;
    g_timeouts = 0;
    g_next_read = now + g_interval_ms;
    if (level != g_battery)
        fprintf(stderr, "btd700d: headphone battery %d%%\n", level);
    g_battery = level;
    g_battery_time = (uint64_t)time(NULL);
}

static void read_failed(long now, const char* what, int err) {
    close_read();
    g_fails++;
    if (err == ETIMEDOUT) g_timeouts++;
    g_next_read = now + (g_fails < 3 ? RETRY_MS : g_interval_ms);

    const char* hint = "";
    if (err == ECONNREFUSED) hint = " (Bluetooth LE disabled on the PC adapter?)";
    else if (err == EAFNOSUPPORT || err == EHOSTUNREACH || err == ENODEV) hint = " (no Bluetooth adapter or it is off?)";
    else if (err == ENOSYS || err == ENOTCONN) hint = " (the headphones did not complete the connection, busy with audio?)";
    if (g_fails <= 3 || g_fails % 12 == 0)
        fprintf(stderr, "btd700d: battery read from %s failed: %s%s%s%s\n", g_addr_str, what,
                err ? ": " : "", err ? strerror(err) : "", hint);

    /* the headphones were never seen at this address for about an hour, maybe a different pair */
    if (!g_addr_from_env && g_timeouts >= FORGET_AFTER_TIMEOUTS) {
        fprintf(stderr, "btd700d: forgetting %s, will look for the headphones again\n", g_addr_str);
        forget_addr();
        g_disc_next_allowed = 0;
    }
}

static void start_read(long now) {
    struct sockaddr_l2_le sa;
    int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_L2CAP);
    if (fd < 0) {
        read_failed(now, "socket", errno);
        return;
    }

    memset(&sa, 0, sizeof(sa));
    sa.l2_family = AF_BLUETOOTH;
    sa.l2_cid = htole16(L2CAP_CID_ATT);
    sa.l2_bdaddr_type = BDADDR_LE_PUBLIC;
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        int e = errno;
        close(fd);
        read_failed(now, "bind", e);
        return;
    }

    /* bounds the kernel's own wait for the headphones to advertise */
    struct timeval tv = { CONNECT_TIMEOUT_MS / 1000, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sa.l2_bdaddr = g_addr;
    sa.l2_bdaddr_type = g_addr_type;
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) {
        int e = errno;
        close(fd);
        read_failed(now, "connect", e);
        return;
    }

    g_fd = fd;
    g_rstate = R_CONNECTING;
    g_deadline = now + CONNECT_TIMEOUT_MS;
}

static void send_pdu(const uint8_t* pdu, size_t len) {
    if (send(g_fd, pdu, len, MSG_NOSIGNAL) < 0)
        fprintf(stderr, "btd700d: ATT send: %s\n", strerror(errno));
}

static int is_att_request(uint8_t op) {
    switch (op) {
    case 0x04: case 0x06: case 0x08: case 0x0A: case 0x0C: case 0x0E:
    case 0x10: case 0x12: case 0x16: case 0x18: case 0x20:
        return 1;
    }
    return 0;
}

/* the headphones may talk to the PC's (empty) GATT server meanwhile, answer
 * so their side is not left waiting. returns 1 once the read is finished */
static int handle_pdu(const uint8_t* pdu, ssize_t n, long now) {
    if (n < 1) return 0;

    switch (pdu[0]) {
    case ATT_READ_BY_TYPE_RSP:
        /* pdu[1] is the size of each handle/value pair, the first pair is enough */
        if (n >= 5 && pdu[1] >= 3 && n >= 2 + pdu[1] && pdu[4] <= 100)
            read_ok(pdu[4], now);
        else
            read_failed(now, "malformed Battery Level response", 0);
        return 1;

    case ATT_ERROR_RSP:
        if (n >= 5 && pdu[1] == ATT_READ_BY_TYPE_REQ) {
            char what[64];
            snprintf(what, sizeof(what), "Battery Level not readable, ATT error 0x%02x", pdu[4]);
            read_failed(now, what, 0);
            return 1;
        }
        return 0;

    case ATT_MTU_REQ: {
        const uint8_t rsp[3] = { ATT_MTU_RSP, ATT_DEFAULT_MTU, 0 };
        send_pdu(rsp, sizeof(rsp));
        return 0;
    }

    case ATT_HANDLE_IND: {
        const uint8_t conf = ATT_HANDLE_CONF;
        send_pdu(&conf, 1);
        return 0;
    }
    }

    if (is_att_request(pdu[0])) {
        uint16_t handle = n >= 3 ? (uint16_t)(pdu[1] | pdu[2] << 8) : 0;
        int lookup = pdu[0] == ATT_FIND_INFO_REQ || pdu[0] == ATT_FIND_BY_TYPE_REQ ||
                     pdu[0] == ATT_READ_BY_TYPE_REQ || pdu[0] == ATT_READ_BY_GROUP_REQ;
        const uint8_t rsp[5] = { ATT_ERROR_RSP, pdu[0], (uint8_t)(handle & 0xFF), (uint8_t)(handle >> 8),
                                 lookup ? ATT_ECODE_ATTR_NOT_FOUND : ATT_ECODE_REQ_NOT_SUPP };
        send_pdu(rsp, sizeof(rsp));
    }
    return 0;
}

static void step_read(long now) {
    if (now >= g_deadline) {
        if (g_rstate == R_CONNECTING) read_failed(now, "headphones not reachable", ETIMEDOUT);
        else read_failed(now, "no answer to the battery read", 0);
        return;
    }

    struct pollfd p = { g_fd, (short)(POLLIN | (g_rstate == R_CONNECTING ? POLLOUT : 0)), 0 };
    if (poll(&p, 1, 0) <= 0) return;

    if (g_rstate == R_CONNECTING) {
        int e = 0;
        socklen_t len = sizeof(e);
        if (getsockopt(g_fd, SOL_SOCKET, SO_ERROR, &e, &len) < 0) e = errno;
        if (e) {
            read_failed(now, "connect", e);
            return;
        }
        if (!(p.revents & POLLOUT)) {
            if (p.revents & (POLLHUP | POLLERR)) read_failed(now, "link dropped", 0);
            return;
        }
        g_timeouts = 0;

        const uint8_t req[7] = { ATT_READ_BY_TYPE_REQ, 0x01, 0x00, 0xFF, 0xFF,
                                 UUID_BATTERY_LEVEL & 0xFF, UUID_BATTERY_LEVEL >> 8 };
        if (send(g_fd, req, sizeof(req), MSG_NOSIGNAL) != (ssize_t)sizeof(req)) {
            read_failed(now, "send", errno);
            return;
        }
        g_rstate = R_WAITING;
        g_deadline = now + RESPONSE_TIMEOUT_MS;
        return;
    }

    if (p.revents & POLLIN) {
        uint8_t buf[ATT_DEFAULT_MTU + 512];
        for (;;) {
            ssize_t n = recv(g_fd, buf, sizeof(buf), 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                read_failed(now, "receive", errno);
                return;
            }
            if (n == 0) {
                read_failed(now, "link dropped", 0);
                return;
            }
            if (handle_pdu(buf, n, now)) return;
        }
    } else if (p.revents & (POLLHUP | POLLERR)) {
        read_failed(now, "link dropped", 0);
    }
}

/* ------------------------------------------------------------------------- */

void headset_init(const char* state_dir) {
    const char* iv = getenv("BTD700_BATTERY_INTERVAL");
    if (iv && iv[0]) {
        char* end = NULL;
        errno = 0;
        long interval = strtol(iv, &end, 10);
        if (*end != '\0' || errno == ERANGE || interval <= 0) {
            fprintf(stderr, "btd700d: BTD700_BATTERY_INTERVAL=%s is not a number of seconds, using %d\n",
                    iv, DEFAULT_INTERVAL_S);
            interval = DEFAULT_INTERVAL_S;
        }
        if (interval < MIN_INTERVAL_S) interval = MIN_INTERVAL_S;
        if (interval > MAX_INTERVAL_S) interval = MAX_INTERVAL_S;
        g_interval_ms = interval * 1000;
    }

    if (state_dir && state_dir[0]) {
        snprintf(g_cache_path, sizeof(g_cache_path), "%s/headset", state_dir);
        snprintf(g_setting_path, sizeof(g_setting_path), "%s/battery-reading", state_dir);
    }
    load_setting();
    if (!g_enabled) fprintf(stderr, "btd700d: headphone battery reading is off\n");

    const char* env = getenv("BTD700_HEADSET_ADDRESS");
    if (env && env[0]) {
        char addr[32];
        snprintf(addr, sizeof(addr), "%s", env);
        char* slash = strchr(addr, '/');
        int type = BDADDR_LE_PUBLIC;
        if (slash) {
            type = strcmp(slash + 1, "random") == 0 ? BDADDR_LE_RANDOM : BDADDR_LE_PUBLIC;
            *slash = '\0';
        }
        if (set_addr(addr, type) == 0) {
            g_addr_from_env = 1;
            fprintf(stderr, "btd700d: headphones at %s (BTD700_HEADSET_ADDRESS)\n", g_addr_str);
        } else {
            fprintf(stderr, "btd700d: BTD700_HEADSET_ADDRESS=%s is not an address, ignoring\n", env);
        }
    }
    if (!g_have_addr) load_addr();

    int r = sd_bus_open_system(&g_sys);
    if (r < 0) {
        fprintf(stderr, "btd700d: system bus unavailable (%s)\n", strerror(-r));
        g_sys = NULL;
    }
}

void headset_shutdown(void) {
    close_read();
    stop_discovery();
    if (g_sys) sd_bus_flush_close_unref(g_sys);
    g_sys = NULL;
}

void headset_pump(void) {
    if (!g_sys) return;

    int r;
    do {
        r = sd_bus_process(g_sys, NULL);
    } while (r > 0);

    if (r < 0) {
        fprintf(stderr, "btd700d: system bus lost (%s)\n", strerror(-r));
        g_discovering = 0;
        sd_bus_flush_close_unref(g_sys);
        g_sys = NULL;
    }
}

static void reset_reading(void) {
    close_read();
    stop_discovery();
    g_was_up = 0;
    g_was_idle = 0;
    g_battery = -1;
    g_battery_time = 0;
}

void headset_set_enabled(int on) {
    on = on ? 1 : 0;
    if (on == g_enabled) return;

    g_enabled = on;
    save_setting();
    fprintf(stderr, "btd700d: headphone battery reading turned %s\n", on ? "on" : "off");
    /* when turned on, the next tick treats the headphones as freshly linked */
    reset_reading();
    g_fails = 0;
    g_disc_next_allowed = 0;
}

int headset_enabled(void) { return g_enabled; }

void headset_request_read(void) {
    g_next_read = 0;
    g_fails = 0;
    g_disc_next_allowed = 0;
    g_rejected[0] = '\0';
}

void headset_tick(int up, int idle) {
    if (!g_enabled) return;
    long now = mono_ms();

    if (!up) {
        reset_reading();
        return;
    }

    if (!g_was_up) {
        g_was_up = 1;
        g_fails = 0;
        g_next_read = now + SETTLE_MS;
    }
    if (idle && !g_was_idle && g_next_read < now + IDLE_SETTLE_MS)
        g_next_read = now + IDLE_SETTLE_MS;
    g_was_idle = idle;

    if (g_discovering) {
        step_discovery(now);
        return;
    }

    if (g_rstate != R_IDLE) {
        /* back off as soon as audio starts, a pending LE connection must not compete with it */
        if (!idle) {
            close_read();
            g_next_read = now + RETRY_MS;
            return;
        }
        step_read(now);
        return;
    }

    if (!idle || now < g_next_read) return;

    if (!g_have_addr) start_discovery(now);
    else start_read(now);
}

int headset_battery(void) { return g_battery; }
uint64_t headset_battery_time(void) { return g_battery_time; }
