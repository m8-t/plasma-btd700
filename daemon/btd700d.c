#include "headset.h"

#include <btd700/btd700_c.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;
static int g_sink_switched = 0;
static int g_switch_pending = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

#define MAX_SINKS      32
#define SINK_NAME_MAX  256

typedef struct {
    unsigned long index;
    char name[SINK_NAME_MAX];
} sink_t;

static char g_fallback[SINK_NAME_MAX];
static char g_state_dir[PATH_MAX];
static char g_state_path[PATH_MAX];

static int is_btd_name(const char* name) {
    return strcasestr(name, "sennheiser") || strcasestr(name, "btd");
}

/* runs argv without a shell; stdout goes into out (NUL-terminated) or is discarded */
static int run_argv(char* const argv[], char* out, size_t outsz) {
    int fds[2] = { -1, -1 };
    if (out && pipe(fds) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        if (out) { close(fds[0]); close(fds[1]); }
        return -1;
    }
    if (pid == 0) {
        if (out) {
            dup2(fds[1], STDOUT_FILENO);
            close(fds[0]);
            close(fds[1]);
        } else {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, STDOUT_FILENO); close(dn); }
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    if (out) {
        close(fds[1]);
        size_t len = 0;
        while (len < outsz - 1) {
            ssize_t r = read(fds[0], out + len, outsz - 1 - len);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            len += (size_t)r;
        }
        out[len] = '\0';
        close(fds[0]);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int list_sinks(sink_t* sinks, int max) {
    char buf[8192];
    char* const argv[] = { "pactl", "list", "short", "sinks", NULL };
    if (run_argv(argv, buf, sizeof(buf)) < 0) return -1;

    int n = 0;
    for (char* line = strtok(buf, "\n"); line && n < max; line = strtok(NULL, "\n")) {
        char* end = NULL;
        unsigned long id = strtoul(line, &end, 10);
        if (end == line || *end != '\t') continue;

        char* name = end + 1;
        char* tab = strchr(name, '\t');
        if (tab) *tab = '\0';
        if (name[0] == '\0' || strlen(name) >= SINK_NAME_MAX) continue;

        sinks[n].index = id;
        strcpy(sinks[n].name, name);
        n++;
    }
    return n;
}

static int get_default_sink(char* out, size_t outsz) {
    char* const argv[] = { "pactl", "get-default-sink", NULL };
    if (run_argv(argv, out, outsz) < 0) return -1;
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] ? 0 : -1;
}

static int set_default_sink(const char* name) {
    char* const argv[] = { "pactl", "set-default-sink", (char*)name, NULL };
    return run_argv(argv, NULL, 0);
}

static int has_sink(const sink_t* sinks, int n, const char* name) {
    for (int i = 0; i < n; i++)
        if (strcmp(sinks[i].name, name) == 0) return 1;
    return 0;
}

static void init_state_path(void) {
    const char* xdg = getenv("XDG_STATE_HOME");
    const char* home = getenv("HOME");

    g_state_dir[0] = '\0';
    g_state_path[0] = '\0';
    if (xdg && xdg[0] == '/')
        snprintf(g_state_dir, sizeof(g_state_dir), "%s/btd700d", xdg);
    else if (home && home[0] == '/')
        snprintf(g_state_dir, sizeof(g_state_dir), "%s/.local/state/btd700d", home);
    else
        fprintf(stderr, "btd700d: no state directory, fallback sink will not persist\n");

    if (g_state_dir[0] &&
        snprintf(g_state_path, sizeof(g_state_path), "%s/fallback-sink", g_state_dir) >= (int)sizeof(g_state_path))
        g_state_path[0] = '\0';
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

static void save_fallback(void) {
    if (!g_state_path[0]) return;

    mkdir_parents(g_state_path);
    FILE* fp = fopen(g_state_path, "w");
    if (!fp) {
        fprintf(stderr, "btd700d: cannot write %s: %s\n", g_state_path, strerror(errno));
        return;
    }
    fprintf(fp, "%s\n", g_fallback);
    if (fclose(fp) != 0)
        fprintf(stderr, "btd700d: cannot write %s: %s\n", g_state_path, strerror(errno));
}

static void load_fallback(void) {
    if (!g_state_path[0]) return;

    FILE* fp = fopen(g_state_path, "r");
    if (!fp) return;

    char line[SINK_NAME_MAX];
    if (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        int ok = line[0] != '\0';
        for (const char* c = line; *c; c++)
            if ((unsigned char)*c < 0x20) ok = 0;
        if (ok) {
            snprintf(g_fallback, sizeof(g_fallback), "%s", line);
            fprintf(stderr, "btd700d: remembered fallback sink %s\n", g_fallback);
        }
    }
    fclose(fp);
}

/* only non-BTD sinks are ever remembered, by name since ids change across restarts */
static void remember_fallback(const char* name) {
    if (!name[0] || is_btd_name(name) || strcmp(g_fallback, name) == 0) return;
    snprintf(g_fallback, sizeof(g_fallback), "%s", name);
    fprintf(stderr, "btd700d: fallback sink is now %s\n", g_fallback);
    save_fallback();
}

static void observe_default(void) {
    char def[SINK_NAME_MAX];
    if (get_default_sink(def, sizeof(def)) == 0) remember_fallback(def);
}

/* order: BTD700_FALLBACK_SINK, remembered sink, then the non-BTD sink with the lowest index */
static int pick_fallback(const sink_t* sinks, int n, char* out) {
    const char* env = getenv("BTD700_FALLBACK_SINK");
    if (env && env[0]) {
        if (has_sink(sinks, n, env) && !is_btd_name(env)) {
            snprintf(out, SINK_NAME_MAX, "%s", env);
            fprintf(stderr, "btd700d: fallback %s (BTD700_FALLBACK_SINK)\n", out);
            return 0;
        }
        fprintf(stderr, "btd700d: BTD700_FALLBACK_SINK=%s is not an available non-BTD sink, ignoring\n", env);
    }

    if (g_fallback[0] && has_sink(sinks, n, g_fallback)) {
        snprintf(out, SINK_NAME_MAX, "%s", g_fallback);
        fprintf(stderr, "btd700d: fallback %s (remembered)\n", out);
        return 0;
    }

    const sink_t* best = NULL;
    for (int i = 0; i < n; i++) {
        if (is_btd_name(sinks[i].name)) continue;
        if (!best || sinks[i].index < best->index) best = &sinks[i];
    }
    if (!best) return -1;

    snprintf(out, SINK_NAME_MAX, "%s", best->name);
    fprintf(stderr, "btd700d: fallback %s (%s, using lowest-index non-BTD sink)\n", out,
            g_fallback[0] ? "remembered sink is gone" : "nothing remembered");
    return 0;
}

static void switch_to_btd700(void) {
    if (g_sink_switched) return;

    sink_t sinks[MAX_SINKS];
    int n = list_sinks(sinks, MAX_SINKS);
    const char* btd = NULL;
    for (int i = 0; i < n && !btd; i++)
        if (is_btd_name(sinks[i].name)) btd = sinks[i].name;

    if (!btd) {
        fprintf(stderr, "btd700d: BTD700 sink not found in PipeWire\n");
        return;
    }

    char def[SINK_NAME_MAX];
    if (get_default_sink(def, sizeof(def)) < 0) {
        fprintf(stderr, "btd700d: cannot read default sink\n");
        return;
    }

    if (is_btd_name(def)) {
        g_sink_switched = 1;
        return;
    }

    remember_fallback(def);
    if (set_default_sink(btd) != 0) {
        fprintf(stderr, "btd700d: cannot set default sink to %s\n", btd);
        return;
    }
    g_sink_switched = 1;
    fprintf(stderr, "btd700d: switched default sink to %s, previous %s\n", btd, def);
}

/* force skips the "did we switch it" check, used for any disconnect */
static void restore_sink(int force) {
    if (!force && !g_sink_switched) return;
    g_sink_switched = 0;

    sink_t sinks[MAX_SINKS];
    char def[SINK_NAME_MAX];
    int n = list_sinks(sinks, MAX_SINKS);
    if (n < 0 || get_default_sink(def, sizeof(def)) < 0) {
        fprintf(stderr, "btd700d: cannot read sinks, leaving default alone\n");
        return;
    }

    if (!is_btd_name(def)) {
        remember_fallback(def);
        fprintf(stderr, "btd700d: default is %s, leaving it\n", def);
        return;
    }

    char fb[SINK_NAME_MAX];
    if (pick_fallback(sinks, n, fb) < 0) {
        fprintf(stderr, "btd700d: no fallback sink available\n");
        return;
    }
    if (set_default_sink(fb) == 0)
        fprintf(stderr, "btd700d: restored default sink to %s\n", fb);
    else
        fprintf(stderr, "btd700d: cannot set default sink to %s\n", fb);
}

/* At link-up the battery read goes first: the headphones stop advertising over
 * LE once audio plays, and switching the sink can start audio right away.
 * headset_sink_hold() keeps this short, the main loop does the switch. */
static void request_switch(btd700_dongle_state_t state) {
    if (state == BTD700_STATE_CONNECTED && headset_enabled()) {
        g_switch_pending = 1;
        return;
    }
    g_switch_pending = 0;
    switch_to_btd700();
}

#define DBUS_NAME  "org.btd700ctl.Dongle"
#define DBUS_PATH  "/org/btd700ctl/Dongle"
#define DBUS_IFACE "org.btd700ctl.Dongle1"
#define ERR_NOT_PRESENT "org.btd700ctl.Error.NotPresent"
#define ERR_FAILED      "org.btd700ctl.Error.Failed"

#define POLL_MS         100
#define RECONNECT_MS    5000
#define REFRESH_RETRY_MS 2000

enum {
    D_FW      = 1 << 0,
    D_STATE   = 1 << 1,
    D_MODE    = 1 << 2,
    D_CODECS  = 1 << 3,
    D_QUALITY = 1 << 4,
    D_GAMING  = 1 << 5,
    D_ALL     = 0x3F,
};

typedef struct {
    int present;
    int state;
    int mode;
    int transport;
    uint16_t supported;
    uint16_t active;
    uint32_t rate;
    uint32_t depth;
    int gaming;
    char fw[32];
    int battery;
    uint64_t battery_time;
    int battery_reading;
} dongle_props_t;

static const struct {
    const char* token;
    btd700_codec_t codec;
    uint16_t mask;
} k_codecs[] = {
    { "sbc",           BTD700_CODEC_SBC,           BTD700_CODEC_MASK_SBC },
    { "aptx",          BTD700_CODEC_APTX,          BTD700_CODEC_MASK_APTX },
    { "aptx-adaptive", BTD700_CODEC_APTX_ADAPTIVE, BTD700_CODEC_MASK_APTX_ADAPTIVE },
    { "aptx-lossless", BTD700_CODEC_APTX_LOSSLESS, BTD700_CODEC_MASK_APTX_LOSSLESS },
    { "aptx-lite",     BTD700_CODEC_APTX_LITE,     BTD700_CODEC_MASK_APTX_LITE },
    { "lc3",           BTD700_CODEC_LC3,           BTD700_CODEC_MASK_LC3 },
};
#define N_CODECS (sizeof(k_codecs) / sizeof(k_codecs[0]))

static const char* const k_modes[] = { "high-quality", "gaming", "broadcast" };
static const char* const k_transports[] = { "disconnected", "classic", "le-audio", "multipoint" };
static const char* const k_states[] = { "none", "disconnected", "connected",
                                        "streaming-audio", "streaming-voice" };

static btd700_driver_t* g_drv;
static sd_bus* g_bus;
static dongle_props_t g_cur;
static dongle_props_t g_pub;
static unsigned g_dirty;
static int g_gaming_broken;
static int g_check_sink_initial;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char* token_of(const char* const* table, size_t n, int v, const char* fallback) {
    return (v >= 0 && (size_t)v < n) ? table[v] : fallback;
}

static int token_index(const char* const* table, size_t n, const char* s) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(table[i], s) == 0) return (int)i;
    return -1;
}

/* battery_reading is a setting, not dongle state, so it survives a reset */
static void reset_props(void) {
    int reading = g_cur.battery_reading;
    memset(&g_cur, 0, sizeof(g_cur));
    g_cur.mode = -1;
    g_cur.battery = -1;
    g_cur.battery_reading = reading;
}

/* a failed gaming query is expected (see README), so it is not retried automatically */
static void mark_all_dirty(void) {
    g_dirty |= D_ALL & ~(g_gaming_broken ? D_GAMING : 0u);
}

static int query_failed(const char* what, btd700_error_t err) {
    fprintf(stderr, "btd700d: %s query failed: %s\n", what, btd700_error_string(err));
    return -1;
}

/* re-queries everything flagged dirty, a flag is cleared before its query so an
 * event arriving during the round trip re-arms it. returns -1 on first failure. */
static int refresh_dirty(void) {
    btd700_error_t err;

    if (!btd700_driver_is_connected(g_drv)) return -1;

    if (g_dirty & D_FW) {
        btd700_firmware_version_t fw;
        g_dirty &= ~(unsigned)D_FW;
        err = btd700_driver_firmware_version(g_drv, &fw);
        if (err != BTD700_OK) { g_dirty |= D_FW; return query_failed("firmware", err); }
        snprintf(g_cur.fw, sizeof(g_cur.fw), "%u.%u.%u", fw.major, fw.minor, fw.build);
    }

    if (g_dirty & D_STATE) {
        btd700_dongle_state_t st;
        g_dirty &= ~(unsigned)D_STATE;
        err = btd700_driver_state(g_drv, &st);
        if (err != BTD700_OK) { g_dirty |= D_STATE; return query_failed("state", err); }
        g_cur.state = (int)st;
        if (g_check_sink_initial) {
            g_check_sink_initial = 0;
            if (st == BTD700_STATE_CONNECTED || st == BTD700_STATE_STREAMING_AUDIO)
                request_switch(st);
        }
    }

    if (g_dirty & D_MODE) {
        btd700_audio_config_t cfg;
        g_dirty &= ~(unsigned)D_MODE;
        err = btd700_driver_audio_config(g_drv, &cfg);
        if (err != BTD700_OK) { g_dirty |= D_MODE; return query_failed("audio mode", err); }
        g_cur.mode = (int)cfg.mode;
        g_cur.transport = (int)cfg.transport;
    }

    if (g_dirty & D_CODECS) {
        uint16_t sup = 0, act = 0;
        g_dirty &= ~(unsigned)D_CODECS;
        err = btd700_driver_supported_codecs(g_drv, &sup);
        if (err == BTD700_OK) err = btd700_driver_active_codec(g_drv, &act);
        if (err != BTD700_OK) { g_dirty |= D_CODECS; return query_failed("codecs", err); }
        g_cur.supported = sup & BTD700_CODEC_MASK_ALL;
        g_cur.active = act & BTD700_CODEC_MASK_ALL;
    }

    if (g_dirty & D_QUALITY) {
        btd700_audio_quality_t q;
        g_dirty &= ~(unsigned)D_QUALITY;
        err = btd700_driver_audio_quality(g_drv, &q);
        if (err != BTD700_OK) { g_dirty |= D_QUALITY; return query_failed("audio quality", err); }
        g_cur.rate = q.frequency == BTD700_FREQ_44100 ? 44100 :
                     q.frequency == BTD700_FREQ_48000 ? 48000 :
                     q.frequency == BTD700_FREQ_96000 ? 96000 : 0;
        g_cur.depth = q.resolution == BTD700_RES_16BIT ? 16 :
                      q.resolution == BTD700_RES_24BIT ? 24 : 0;
    }

    if (g_dirty & D_GAMING) {
        int avail = 0;
        g_dirty &= ~(unsigned)D_GAMING;
        err = btd700_driver_is_gaming_available(g_drv, &avail);
        g_gaming_broken = (err != BTD700_OK);
        g_cur.gaming = (err == BTD700_OK && avail) ? 1 : 0;
    }

    return 0;
}

static void flush_props(void) {
    const char* names[14];
    size_t n = 0;

    if (g_cur.present != g_pub.present)     names[n++] = "Present";
    if (g_cur.state != g_pub.state)         names[n++] = "State";
    if (g_cur.mode != g_pub.mode)           names[n++] = "AudioMode";
    if (g_cur.transport != g_pub.transport) names[n++] = "Transport";
    if (g_cur.supported != g_pub.supported) names[n++] = "SupportedCodecs";
    if (g_cur.active != g_pub.active)       names[n++] = "ActiveCodecs";
    if (g_cur.rate != g_pub.rate)           names[n++] = "SampleRate";
    if (g_cur.depth != g_pub.depth)         names[n++] = "BitDepth";
    if (g_cur.gaming != g_pub.gaming)       names[n++] = "GamingAvailable";
    if (strcmp(g_cur.fw, g_pub.fw) != 0)    names[n++] = "FirmwareVersion";
    if (g_cur.battery != g_pub.battery)     names[n++] = "HeadsetBattery";
    if (g_cur.battery_time != g_pub.battery_time) names[n++] = "HeadsetBatteryUpdated";
    if (g_cur.battery_reading != g_pub.battery_reading) names[n++] = "BatteryReading";
    g_pub = g_cur;

    if (n == 0 || !g_bus) return;

    names[n] = NULL;
    int r = sd_bus_emit_properties_changed_strv(g_bus, DBUS_PATH, DBUS_IFACE, (char**)names);
    if (r < 0)
        fprintf(stderr, "btd700d: emit PropertiesChanged: %s\n", strerror(-r));
}

/* runs inside send_and_receive, so it must not issue HID commands */

static void on_event(const btd700_event_t* event, void* user_data) {
    (void)user_data;

    switch (event->type) {
    case BTD700_EVENT_STATE_CHANGED: {
        g_dirty |= D_MODE | D_CODECS | D_QUALITY;
        if (event->data_len < 1) {
            g_dirty |= D_STATE;
            return;
        }

        btd700_dongle_state_t state = (btd700_dongle_state_t)event->data[0];
        g_cur.state = (int)state;
        fprintf(stderr, "btd700d: state -> %s\n", btd700_dongle_state_string(state));

        switch (state) {
        case BTD700_STATE_CONNECTED:
        case BTD700_STATE_STREAMING_AUDIO:
            request_switch(state);
            break;
        case BTD700_STATE_DISCONNECTED:
            g_switch_pending = 0;
            restore_sink(1);
            break;
        default:
            break;
        }
        break;
    }
    case BTD700_EVENT_AUDIO_MODE_CHANGED:
    case BTD700_EVENT_LE_AUDIO_STATE_CHANGED:
    case BTD700_EVENT_SINK_TRANSPORT_CHANGED:
        g_dirty |= D_MODE;
        break;
    case BTD700_EVENT_CODEC_CHANGED:
        g_dirty |= D_CODECS | D_QUALITY;
        break;
    case BTD700_EVENT_AUDIO_QUALITY_CHANGED:
        g_dirty |= D_QUALITY;
        break;
    case BTD700_EVENT_GAMING_AVAILABILITY_CHANGED:
        g_dirty |= D_GAMING;
        break;
    }
}

static int append_codecs(sd_bus_message* reply, uint16_t mask) {
    int r = sd_bus_message_open_container(reply, 'a', "s");
    if (r < 0) return r;
    for (size_t i = 0; i < N_CODECS; i++) {
        if (!(mask & k_codecs[i].mask)) continue;
        r = sd_bus_message_append(reply, "s", k_codecs[i].token);
        if (r < 0) return r;
    }
    return sd_bus_message_close_container(reply);
}

static int prop_get(sd_bus* bus, const char* path, const char* interface,
                    const char* property, sd_bus_message* reply,
                    void* userdata, sd_bus_error* error) {
    (void)bus; (void)path; (void)interface; (void)userdata; (void)error;

    if (strcmp(property, "Present") == 0)
        return sd_bus_message_append(reply, "b", g_cur.present);
    if (strcmp(property, "State") == 0)
        return sd_bus_message_append(reply, "s",
            token_of(k_states, 5, g_cur.state, "none"));
    if (strcmp(property, "AudioMode") == 0)
        return sd_bus_message_append(reply, "s",
            token_of(k_modes, 3, g_cur.mode, "unknown"));
    if (strcmp(property, "Transport") == 0)
        return sd_bus_message_append(reply, "s",
            token_of(k_transports, 4, g_cur.transport, "unknown"));
    if (strcmp(property, "SupportedCodecs") == 0)
        return append_codecs(reply, g_cur.supported);
    if (strcmp(property, "ActiveCodecs") == 0)
        return append_codecs(reply, g_cur.active);
    if (strcmp(property, "SampleRate") == 0)
        return sd_bus_message_append(reply, "u", g_cur.rate);
    if (strcmp(property, "BitDepth") == 0)
        return sd_bus_message_append(reply, "u", g_cur.depth);
    if (strcmp(property, "GamingAvailable") == 0)
        return sd_bus_message_append(reply, "b", g_cur.gaming);
    if (strcmp(property, "FirmwareVersion") == 0)
        return sd_bus_message_append(reply, "s", g_cur.fw);
    if (strcmp(property, "HeadsetBattery") == 0)
        return sd_bus_message_append(reply, "i", (int32_t)g_cur.battery);
    if (strcmp(property, "HeadsetBatteryUpdated") == 0)
        return sd_bus_message_append(reply, "t", g_cur.battery_time);
    if (strcmp(property, "BatteryReading") == 0)
        return sd_bus_message_append(reply, "b", g_cur.battery_reading);
    return -ENOENT;
}

static int require_present(sd_bus_error* error) {
    if (!g_cur.present || !btd700_driver_is_connected(g_drv))
        return sd_bus_error_set_const(error, ERR_NOT_PRESENT, "dongle not present");
    return 0;
}

static int hid_failed(sd_bus_error* error, const char* what, btd700_error_t err) {
    fprintf(stderr, "btd700d: %s failed: %s\n", what, btd700_error_string(err));
    return sd_bus_error_setf(error, ERR_FAILED, "%s failed: %s", what, btd700_error_string(err));
}

static int set_done(sd_bus_message* m) {
    mark_all_dirty();
    return sd_bus_reply_method_return(m, NULL);
}

static int m_set_audio_mode(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    (void)userdata;
    const char* s = NULL;
    int r = sd_bus_message_read(m, "s", &s);
    if (r < 0) return r;

    int mode = token_index(k_modes, 3, s);
    if (mode < 0)
        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "unknown audio mode '%s'", s);

    r = require_present(error);
    if (r < 0) return r;

    /* the transport must be passed back unchanged or the dongle leaves LE Audio / multipoint */
    btd700_audio_config_t cfg;
    btd700_error_t err = btd700_driver_audio_config(g_drv, &cfg);
    if (err != BTD700_OK) return hid_failed(error, "audio config query", err);

    err = btd700_driver_set_audio_mode(g_drv, (btd700_audio_mode_t)mode, cfg.transport);
    if (err != BTD700_OK) return hid_failed(error, "set audio mode", err);

    return set_done(m);
}

static int m_set_codec(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    (void)userdata;
    const char* s = NULL;
    int r = sd_bus_message_read(m, "s", &s);
    if (r < 0) return r;

    size_t idx = N_CODECS;
    for (size_t i = 0; i < N_CODECS; i++)
        if (strcmp(k_codecs[i].token, s) == 0) idx = i;
    if (idx == N_CODECS)
        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "unknown codec '%s'", s);

    r = require_present(error);
    if (r < 0) return r;

    uint16_t supported = 0;
    btd700_error_t err = btd700_driver_supported_codecs(g_drv, &supported);
    if (err != BTD700_OK) return hid_failed(error, "supported codecs query", err);

    if (!(supported & k_codecs[idx].mask))
        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                                 "codec '%s' not supported by the dongle", s);

    err = btd700_driver_set_codec(g_drv, k_codecs[idx].codec);
    if (err != BTD700_OK) return hid_failed(error, "set codec", err);

    return set_done(m);
}

static int m_connect(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    (void)userdata;
    int r = require_present(error);
    if (r < 0) return r;
    btd700_error_t err = btd700_driver_trigger_connect(g_drv);
    if (err != BTD700_OK) return hid_failed(error, "connect", err);
    return set_done(m);
}

static int m_disconnect(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    (void)userdata;
    int r = require_present(error);
    if (r < 0) return r;
    btd700_error_t err = btd700_driver_trigger_disconnect(g_drv);
    if (err != BTD700_OK) return hid_failed(error, "disconnect", err);
    return set_done(m);
}

static void sync_headset_props(void) {
    g_cur.battery = headset_battery();
    g_cur.battery_time = headset_battery_time();
    g_cur.battery_reading = headset_enabled();
}

/* works without a dongle, it only stores the choice */
static int m_set_battery_reading(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    (void)userdata; (void)error;
    int on = 0;
    int r = sd_bus_message_read(m, "b", &on);
    if (r < 0) return r;

    headset_set_enabled(on);
    sync_headset_props();
    return sd_bus_reply_method_return(m, NULL);
}

static int m_refresh(sd_bus_message* m, void* userdata, sd_bus_error* error) {
    (void)userdata;
    int r = require_present(error);
    if (r < 0) return r;

    g_gaming_broken = 0;
    g_dirty |= D_ALL;
    headset_request_read();
    if (refresh_dirty() < 0)
        return sd_bus_error_set_const(error, ERR_FAILED, "refresh failed");

    flush_props();
    return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable k_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD_WITH_ARGS("SetAudioMode", SD_BUS_ARGS("s", mode), SD_BUS_NO_RESULT,
                            m_set_audio_mode, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("SetCodec", SD_BUS_ARGS("s", codec), SD_BUS_NO_RESULT,
                            m_set_codec, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Connect", "", "", m_connect, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Disconnect", "", "", m_disconnect, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Refresh", "", "", m_refresh, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_ARGS("SetBatteryReading", SD_BUS_ARGS("b", enabled), SD_BUS_NO_RESULT,
                            m_set_battery_reading, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("Present", "b", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("State", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("AudioMode", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Transport", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("SupportedCodecs", "as", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ActiveCodecs", "as", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("SampleRate", "u", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("BitDepth", "u", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("GamingAvailable", "b", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("FirmwareVersion", "s", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("HeadsetBattery", "i", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("HeadsetBatteryUpdated", "t", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("BatteryReading", "b", prop_get, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_VTABLE_END
};

static void close_bus(void) {
    if (!g_bus) return;
    sd_bus_flush_close_unref(g_bus);
    g_bus = NULL;
}

static void init_bus(void) {
    int r = sd_bus_open_user(&g_bus);
    if (r < 0) {
        fprintf(stderr, "btd700d: session bus unavailable (%s), running without D-Bus\n", strerror(-r));
        g_bus = NULL;
        return;
    }

    r = sd_bus_add_object_vtable(g_bus, NULL, DBUS_PATH, DBUS_IFACE, k_vtable, NULL);
    if (r < 0) {
        fprintf(stderr, "btd700d: vtable: %s\n", strerror(-r));
        close_bus();
        return;
    }
    r = sd_bus_request_name(g_bus, DBUS_NAME, 0);
    if (r < 0) {
        fprintf(stderr, "btd700d: cannot claim %s (%s), running without D-Bus\n", DBUS_NAME, strerror(-r));
        close_bus();
        return;
    }

    fprintf(stderr, "btd700d: serving %s on the session bus\n", DBUS_NAME);
}

static void bus_pump(void) {
    headset_pump();
    if (!g_bus) return;

    int r;
    do {
        r = sd_bus_process(g_bus, NULL);
    } while (r > 0);

    if (r < 0) {
        fprintf(stderr, "btd700d: session bus lost (%s), continuing without D-Bus\n", strerror(-r));
        close_bus();
    }
}

static void idle_wait(int ms) {
    if (g_bus) {
        sd_bus_wait(g_bus, (uint64_t)ms * 1000);
    } else {
        usleep((useconds_t)ms * 1000);
    }
}

static int try_connect(void) {
    if (btd700_driver_connect(g_drv) != BTD700_OK) return -1;

    fprintf(stderr, "btd700d: connected to dongle\n");
    reset_props();
    g_cur.present = 1;
    g_gaming_broken = 0;
    g_check_sink_initial = 1;
    g_dirty = 0;
    mark_all_dirty();
    return 0;
}

/* battery reads need the headphones on the dongle, and an LE connection only
 * gets through while the dongle is not streaming */
static void update_headset(void) {
    int up = g_cur.present && (g_cur.state == BTD700_STATE_CONNECTED ||
                               g_cur.state == BTD700_STATE_STREAMING_AUDIO ||
                               g_cur.state == BTD700_STATE_STREAMING_VOICE);
    int idle = g_cur.present && g_cur.state == BTD700_STATE_CONNECTED;

    headset_tick(up, idle);
    sync_headset_props();
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    btd700_error_t err = btd700_driver_create(&g_drv);
    if (err != BTD700_OK) {
        fprintf(stderr, "btd700d: create: %s\n", btd700_error_string(err));
        return 1;
    }
    btd700_driver_set_event_callback(g_drv, on_event, NULL);

    reset_props();
    g_pub = g_cur;
    init_state_path();
    load_fallback();
    observe_default();
    headset_init(g_state_dir);
    sync_headset_props();
    g_pub = g_cur;
    init_bus();

    long next_connect = 0;
    long next_refresh = 0;
    int announced_missing = 0;
    int stable = 0;

    while (g_running) {
        if (!btd700_driver_is_connected(g_drv) && now_ms() >= next_connect) {
            if (try_connect() == 0) {
                announced_missing = 0;
                next_refresh = refresh_dirty() < 0 ? now_ms() + REFRESH_RETRY_MS : 0;
            } else {
                if (!announced_missing)
                    fprintf(stderr, "btd700d: dongle not found, retrying every %ds in background\n",
                            RECONNECT_MS / 1000);
                announced_missing = 1;
                next_connect = now_ms() + RECONNECT_MS;
            }
            flush_props();
        }

        if (btd700_driver_is_connected(g_drv)) {
            err = btd700_driver_poll_events(g_drv, POLL_MS);
            if (!g_running) break;

            if (err == BTD700_ERR_HID || err == BTD700_ERR_DEVICE_NOT_OPEN) {
                fprintf(stderr, "btd700d: HID error, reconnecting...\n");
                btd700_driver_disconnect(g_drv);
                g_switch_pending = 0;
                if (stable) restore_sink(0);
                stable = 0;
                g_dirty = 0;
                reset_props();
                next_connect = 0;
            } else {
                stable = 1;
                if (g_dirty && now_ms() >= next_refresh && refresh_dirty() < 0)
                    next_refresh = now_ms() + REFRESH_RETRY_MS;
            }
        } else {
            idle_wait(POLL_MS);
        }

        update_headset();
        if (g_switch_pending && !headset_sink_hold()) {
            g_switch_pending = 0;
            switch_to_btd700();
        }
        bus_pump();
        flush_props();
    }

    restore_sink(0);
    btd700_driver_disconnect(g_drv);
    headset_shutdown();
    close_bus();
    btd700_driver_destroy(g_drv);
    fprintf(stderr, "btd700d: shutdown\n");
    return 0;
}
