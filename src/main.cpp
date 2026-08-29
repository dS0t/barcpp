// bar.cpp — minimal always-on top-of-screen load strip for wlroots compositors
// (Hyprland, Sway, ...). No GTK/Qt/Cairo/toolkit at all: raw wayland-client +
// wlr-layer-shell, direct pixel writes into a shared-memory buffer.
//
// Resource footprint: no forked subprocesses, no widget toolkit, no timers
// beyond a single poll() with a 1s timeout. RSS is typically a couple of MB
// (mostly the mmapped pixel buffer + wayland library), CPU is ~0% between
// redraws and a handful of microseconds per redraw (a few hundred pixels).

#include <wayland-client.h>
#include "wlr-layer-shell-client-protocol.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <linux/nl80211.h>
#include <ifaddrs.h>
#include <net/if.h>

namespace {

// ---------------------------------------------------------------------- //
// runtime config (defaults; overridden by barcpp.conf next to the binary)
// ---------------------------------------------------------------------- //
struct Config {
    int height       = 1;
    int gap_px       = 4;
    int push_windows_px = 2;
    int margin_left_px = 4;
    int margin_right_px = 4;
    int opacity_pct  = 30;
    int opacity_pct_end = 100;
    
    int marker_px = 10;
    int marker_gap_px = 1;
    
    char skin[32]    = "zebra";
    int zebra_dash_px = 10;
    int zebra_space_px = 1;
    int blocks_count = 10;

    int poll_ms      = 2000;
    
    int enable_cpu   = 1;
    int enable_temp  = 1;
    int enable_ram   = 1;
    int enable_wifi  = 1;
    int enable_bat   = 1;
    
    int interval_cpu_ms  = 1000;
    int interval_temp_ms = 5000;
    int interval_ram_ms  = 2000;
    int interval_wifi_ms = 5000;
    int interval_bat_ms  = 10000;

    int bat_low_pct  = 20;
    int cpu_mid_pct  = 20;
    int cpu_high_pct = 80;
    int ram_mid_pct  = 60;
    int ram_high_pct = 80;
    int temp_mid_c   = 45;
    int temp_high_c  = 65;
    int temp_max_c   = 100;
    int wifi_mid_dbm = -65;
    int wifi_low_dbm = -75;

    char bat_path[256] = "";
    char temp_path[256] = "";

    // XRGB8888, opaque
    uint32_t color_bg        = 0xFF161b22;
    uint32_t color_cpu       = 0xFF2ea043;
    uint32_t color_cpu_mid   = 0xFFd29922;
    uint32_t color_cpu_high  = 0xFFf85149;
    uint32_t color_temp      = 0xFF39c5cf;
    uint32_t color_temp_mid  = 0xFFd29922;
    uint32_t color_temp_high = 0xFFf85149;
    uint32_t color_ram_free  = 0xFF58a6ff;
    uint32_t color_ram_avail = 0xFF1f6feb;
    uint32_t color_ram_mid   = 0xFFd29922;
    uint32_t color_ram_high  = 0xFFf85149;
    uint32_t color_bat       = 0xFFf778ba;
    uint32_t color_bat_low   = 0xFFf85149;
    uint32_t color_wifi      = 0xFFcba6f7;
    uint32_t color_wifi_mid  = 0xFFd29922;
    uint32_t color_wifi_low  = 0xFFf85149;
};

Config cfg;

bool parse_color(const char *s, uint32_t *out) {
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == '#') ++s;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    unsigned long v = 0;
    char *end = nullptr;
    v = strtoul(s, &end, 16);
    if (end == s) return false;
    if (v <= 0xFFFFFFul)
        *out = 0xFF000000u | static_cast<uint32_t>(v);
    else
        *out = static_cast<uint32_t>(v);
    return true;
}

bool parse_int(const char *s, int *out) {
    char *end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s) return false;
    *out = static_cast<int>(v);
    return true;
}

void apply_kv(const char *key, const char *val) {
    if (!strcmp(key, "height"))          parse_int(val, &cfg.height);
    else if (!strcmp(key, "gap_px"))     parse_int(val, &cfg.gap_px);
    else if (!strcmp(key, "push_windows_px")) parse_int(val, &cfg.push_windows_px);
    else if (!strcmp(key, "margin_left_px"))  parse_int(val, &cfg.margin_left_px);
    else if (!strcmp(key, "margin_right_px")) parse_int(val, &cfg.margin_right_px);
    else if (!strcmp(key, "opacity_pct"))     parse_int(val, &cfg.opacity_pct);
    else if (!strcmp(key, "opacity_pct_end")) parse_int(val, &cfg.opacity_pct_end);
    else if (!strcmp(key, "marker_px"))       parse_int(val, &cfg.marker_px);
    else if (!strcmp(key, "marker_gap_px"))   parse_int(val, &cfg.marker_gap_px);
    else if (!strcmp(key, "skin"))            { strncpy(cfg.skin, val, sizeof(cfg.skin) - 1); cfg.skin[sizeof(cfg.skin) - 1] = '\0'; }
    else if (!strcmp(key, "zebra_dash_px"))   parse_int(val, &cfg.zebra_dash_px);
    else if (!strcmp(key, "zebra_space_px"))  parse_int(val, &cfg.zebra_space_px);
    else if (!strcmp(key, "blocks_count"))    parse_int(val, &cfg.blocks_count);
    else if (!strcmp(key, "poll_ms"))    parse_int(val, &cfg.poll_ms);
    else if (!strcmp(key, "enable_cpu"))  parse_int(val, &cfg.enable_cpu);
    else if (!strcmp(key, "enable_temp")) parse_int(val, &cfg.enable_temp);
    else if (!strcmp(key, "enable_ram"))  parse_int(val, &cfg.enable_ram);
    else if (!strcmp(key, "enable_wifi")) parse_int(val, &cfg.enable_wifi);
    else if (!strcmp(key, "enable_bat"))  parse_int(val, &cfg.enable_bat);
    else if (!strcmp(key, "interval_cpu_ms"))  parse_int(val, &cfg.interval_cpu_ms);
    else if (!strcmp(key, "interval_temp_ms")) parse_int(val, &cfg.interval_temp_ms);
    else if (!strcmp(key, "interval_ram_ms"))  parse_int(val, &cfg.interval_ram_ms);
    else if (!strcmp(key, "interval_wifi_ms")) parse_int(val, &cfg.interval_wifi_ms);
    else if (!strcmp(key, "interval_bat_ms"))  parse_int(val, &cfg.interval_bat_ms);
    else if (!strcmp(key, "bat_low_pct")) parse_int(val, &cfg.bat_low_pct);
    else if (!strcmp(key, "cpu_mid_pct")) parse_int(val, &cfg.cpu_mid_pct);
    else if (!strcmp(key, "cpu_high_pct")) parse_int(val, &cfg.cpu_high_pct);
    else if (!strcmp(key, "ram_mid_pct")) parse_int(val, &cfg.ram_mid_pct);
    else if (!strcmp(key, "ram_high_pct")) parse_int(val, &cfg.ram_high_pct);
    else if (!strcmp(key, "temp_mid_c"))   parse_int(val, &cfg.temp_mid_c);
    else if (!strcmp(key, "temp_high_c"))  parse_int(val, &cfg.temp_high_c);
    else if (!strcmp(key, "temp_max_c"))   parse_int(val, &cfg.temp_max_c);
    else if (!strcmp(key, "wifi_mid_dbm")) parse_int(val, &cfg.wifi_mid_dbm);
    else if (!strcmp(key, "wifi_low_dbm")) parse_int(val, &cfg.wifi_low_dbm);
    else if (!strcmp(key, "bat_path")) {
        strncpy(cfg.bat_path, val, sizeof(cfg.bat_path) - 1);
        cfg.bat_path[sizeof(cfg.bat_path) - 1] = '\0';
    }
    else if (!strcmp(key, "temp_path")) {
        strncpy(cfg.temp_path, val, sizeof(cfg.temp_path) - 1);
        cfg.temp_path[sizeof(cfg.temp_path) - 1] = '\0';
    }
    else if (!strcmp(key, "color_bg"))        parse_color(val, &cfg.color_bg);
    else if (!strcmp(key, "color_cpu"))       parse_color(val, &cfg.color_cpu);
    else if (!strcmp(key, "color_cpu_mid"))   parse_color(val, &cfg.color_cpu_mid);
    else if (!strcmp(key, "color_cpu_high"))  parse_color(val, &cfg.color_cpu_high);
    else if (!strcmp(key, "color_temp"))      parse_color(val, &cfg.color_temp);
    else if (!strcmp(key, "color_temp_mid"))  parse_color(val, &cfg.color_temp_mid);
    else if (!strcmp(key, "color_temp_high")) parse_color(val, &cfg.color_temp_high);
    else if (!strcmp(key, "color_ram_free"))  parse_color(val, &cfg.color_ram_free);
    else if (!strcmp(key, "color_ram_avail")) parse_color(val, &cfg.color_ram_avail);
    else if (!strcmp(key, "color_ram_mid"))   parse_color(val, &cfg.color_ram_mid);
    else if (!strcmp(key, "color_ram_high"))  parse_color(val, &cfg.color_ram_high);
    else if (!strcmp(key, "color_bat"))       parse_color(val, &cfg.color_bat);
    else if (!strcmp(key, "color_bat_low"))   parse_color(val, &cfg.color_bat_low);
    else if (!strcmp(key, "color_wifi"))      parse_color(val, &cfg.color_wifi);
    else if (!strcmp(key, "color_wifi_mid"))  parse_color(val, &cfg.color_wifi_mid);
    else if (!strcmp(key, "color_wifi_low"))  parse_color(val, &cfg.color_wifi_low);
}

void load_config_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        // trim key end
        char *ke = key + strlen(key);
        while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';
        while (*val == ' ' || *val == '\t') ++val;
        char *ve = val + strlen(val);
        while (ve > val && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\n' || ve[-1] == '\r'))
            *--ve = '\0';
        apply_kv(key, val);
    }
    fclose(f);
    if (cfg.height < 1) cfg.height = 1;
    if (cfg.height > 64) cfg.height = 64;
    if (cfg.gap_px < 0) cfg.gap_px = 0;
    if (cfg.poll_ms < 100) cfg.poll_ms = 100;
}

// Resolve config path: $BARCPP_CONFIG, then <exe_dir>/barcpp.conf,
// then ~/.config/barcpp/config
void load_config() {
    if (const char *env = getenv("BARCPP_CONFIG"); env && *env) {
        load_config_file(env);
        return;
    }

    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            strcpy(slash + 1, "barcpp.conf");
            load_config_file(exe);
            // if file existed we still also allow user config overrides? keep single file for now
            FILE *probe = fopen(exe, "r");
            if (probe) {
                fclose(probe);
                return;
            }
        }
    }

    const char *home = getenv("HOME");
    if (home) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/.config/barcpp/config", home);
        load_config_file(path);
    }
}

// ---------------------------------------------------------------------- //
// wayland / shm state
// ---------------------------------------------------------------------- //
wl_display            *display     = nullptr;
wl_compositor          *compositor  = nullptr;
wl_shm                 *shm         = nullptr;
zwlr_layer_shell_v1    *layer_shell = nullptr;

wl_surface             *surface       = nullptr;
zwlr_layer_surface_v1  *layer_surface = nullptr;

int  surf_width = 0;
bool configured = false;
bool running    = true;

struct Buffer {
    wl_buffer *wlbuf  = nullptr;
    uint32_t  *pixels = nullptr;
    bool       busy   = false;
};
std::array<Buffer, 2> buffers;
int      cur_buffer = 0;
uint8_t *shm_data   = nullptr;
size_t   shm_size   = 0;

// ---------------------------------------------------------------------- //
// /proc readers -- plain file reads, no fork/exec anywhere
// ---------------------------------------------------------------------- //
int read_cpu_percent() {
    static long long prev_idle = 0, prev_total = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    long long user, nice_, system_, idle, iowait, irq, softirq, steal;
    int n = fscanf(f, "cpu %lld %lld %lld %lld %lld %lld %lld %lld",
                   &user, &nice_, &system_, &idle, &iowait, &irq, &softirq, &steal);
    fclose(f);
    if (n < 8) return -1;

    long long idle_all = idle + iowait;
    long long total    = user + nice_ + system_ + idle_all + irq + softirq + steal;
    long long dt = total - prev_total, di = idle_all - prev_idle;
    prev_total = total;
    prev_idle  = idle_all;
    if (dt <= 0) return 0;
    return static_cast<int>(100LL * (dt - di) / dt);
}

int read_temp_c() {
    static char temp_path[256] = "";
    static bool warned = false;
    
    // Find the correct hwmon path once
    if (temp_path[0] == '\0') {
        if (cfg.temp_path[0] != '\0') {
            strcpy(temp_path, cfg.temp_path);
        } else {
            for (int i = 0; i < 20; ++i) {
                char name_path[256];
                snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/hwmon%d/name", i);
                FILE *fn = fopen(name_path, "r");
                if (fn) {
                    char name[64];
                    if (fscanf(fn, "%63s", name) == 1) {
                        if (!strcmp(name, "coretemp") || !strcmp(name, "k10temp")) {
                            snprintf(temp_path, sizeof(temp_path), "/sys/class/hwmon/hwmon%d/temp1_input", i);
                            fclose(fn);
                            break;
                        }
                    }
                    fclose(fn);
                }
            }
            // Fallback to thermal_zone0 if not found
            if (temp_path[0] == '\0') {
                strcpy(temp_path, "/sys/class/thermal/thermal_zone0/temp");
            }
        }
    }

    FILE *f = fopen(temp_path, "r");
    if (!f) {
        if (!warned) {
            fprintf(stderr, "barcpp: Temperature sensor not found at path: %s. Please check temp_path in barcpp.conf\n", temp_path);
            warned = true;
        }
        return -1;
    }
    long temp_milli = 0;
    int n = fscanf(f, "%ld", &temp_milli);
    fclose(f);
    if (n == 1) return static_cast<int>(temp_milli / 1000);
    return -1;
}

// Returns free% and available% of MemTotal. available >= free.
// free%  = MemFree / MemTotal
// avail% = MemAvailable / MemTotal  (free + reclaimable cache/buffers)
struct RamParts {
    int free_pct  = 0;
    int avail_pct = 0;
    int used_pct  = 100; // 100 - avail
    bool ok       = false;
};

RamParts read_ram_parts() {
    RamParts r;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return r;
    long total = 0, avail = 0, free_kb = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        long val = 0;
        if (sscanf(line, "MemTotal: %ld", &val) == 1) total = val;
        else if (sscanf(line, "MemAvailable: %ld", &val) == 1) avail = val;
        else if (sscanf(line, "MemFree: %ld", &val) == 1) free_kb = val;
        if (total && avail && free_kb) break;
    }
    fclose(f);
    if (total <= 0) return r;
    if (free_kb < 0) free_kb = 0;
    if (avail < free_kb) avail = free_kb;
    if (avail > total) avail = total;
    r.free_pct  = static_cast<int>(100L * free_kb / total);
    r.avail_pct = static_cast<int>(100L * avail / total);
    r.used_pct  = 100 - r.avail_pct;
    r.ok        = true;
    return r;
}

int read_battery_percent() {
    static bool warned = false;
    if (cfg.bat_path[0] != '\0') {
        FILE *f = fopen(cfg.bat_path, "r");
        if (f) {
            int v = -1;
            int n = fscanf(f, "%d", &v);
            fclose(f);
            if (n == 1) return v;
        }
        if (!warned) {
            fprintf(stderr, "barcpp: Battery sensor not found at bat_path: %s\n", cfg.bat_path);
            warned = true;
        }
        return -1;
    }

    static const char *paths[] = {
        "/sys/class/power_supply/BAT0/capacity",
        "/sys/class/power_supply/BAT1/capacity",
    };
    for (auto p : paths) {
        FILE *f = fopen(p, "r");
        if (!f) continue;
        int v = -1;
        int n = fscanf(f, "%d", &v);
        fclose(f);
        if (n == 1) return v;
    }
    
    if (!warned) {
        fprintf(stderr, "barcpp: Battery sensor not found in standard paths. Please specify bat_path in barcpp.conf\n");
        warned = true;
    }
    return -1;
}

// Netlink callback to extract RSSI
static int nl_callback(struct nl_msg *msg, void *arg) {
    int *rssi_out = static_cast<int *>(arg);
    struct nlattr *tb[NL80211_ATTR_MAX + 1];
    struct genlmsghdr *gnlh = static_cast<struct genlmsghdr *>(nlmsg_data(nlmsg_hdr(msg)));

    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), nullptr);

    if (tb[NL80211_ATTR_STA_INFO]) {
        struct nlattr *sinfo[NL80211_STA_INFO_MAX + 1];
        if (nla_parse_nested(sinfo, NL80211_STA_INFO_MAX, tb[NL80211_ATTR_STA_INFO], nullptr) == 0) {
            if (sinfo[NL80211_STA_INFO_SIGNAL]) {
                *rssi_out = (int8_t)nla_get_u8(sinfo[NL80211_STA_INFO_SIGNAL]);
            }
        }
    }
    return NL_SKIP;
}

int read_wifi_rssi() {
    static int nl80211_id = 0;
    static struct nl_sock *sk = nullptr;

    if (!sk) {
        sk = nl_socket_alloc();
        if (!sk) return 1; // 1 signals disabled/fail
        if (genl_connect(sk) < 0) {
            nl_socket_free(sk);
            sk = nullptr;
            return 1;
        }
        nl80211_id = genl_ctrl_resolve(sk, "nl80211");
        if (nl80211_id < 0) {
            nl_socket_free(sk);
            sk = nullptr;
            return 1;
        }
    }

    // Try finding a wireless interface by getting the first one starting with "wl" or "wlan"
    unsigned int if_idx = 0;
    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr) == -1) return 1;
    for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_name && !strncmp(ifa->ifa_name, "wl", 2)) {
            if_idx = if_nametoindex(ifa->ifa_name);
            break;
        }
    }
    freeifaddrs(ifaddr);
    
    if (if_idx == 0) return 1; // no interface

    struct nl_msg *msg = nlmsg_alloc();
    if (!msg) return 1;

    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, nl80211_id, 0, NLM_F_DUMP, NL80211_CMD_GET_STATION, 0);
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, if_idx);

    int rssi = 1; // 1 means not connected or no data (valid rssi is negative)
    nl_socket_modify_cb(sk, NL_CB_VALID, NL_CB_CUSTOM, nl_callback, &rssi);
    
    if (nl_send_auto(sk, msg) >= 0) {
        nl_recvmsgs_default(sk);
    }
    nlmsg_free(msg);

    return rssi;
}

int clamp_pct(int pct) {
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return pct;
}

// ---------------------------------------------------------------------- //
// Cached metric readings
// ---------------------------------------------------------------------- //
long long get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct MetricCache {
    int cpu = 0;
    int temp = -1;
    RamParts ram;
    int bat = -1;
    int wifi = 1; // 1 means not connected

    long long last_cpu_ms = 0;
    long long last_temp_ms = 0;
    long long last_ram_ms = 0;
    long long last_bat_ms = 0;
    long long last_wifi_ms = 0;
};
MetricCache cache;

void update_metrics() {
    long long now = get_time_ms();
    
    if (cfg.enable_cpu && (now - cache.last_cpu_ms >= cfg.interval_cpu_ms || cache.last_cpu_ms == 0)) {
        cache.cpu = read_cpu_percent();
        cache.last_cpu_ms = now;
    }

    if (cfg.enable_temp && (now - cache.last_temp_ms >= cfg.interval_temp_ms || cache.last_temp_ms == 0)) {
        cache.temp = read_temp_c();
        cache.last_temp_ms = now;
    }
    
    if (cfg.enable_ram && (now - cache.last_ram_ms >= cfg.interval_ram_ms || cache.last_ram_ms == 0)) {
        cache.ram = read_ram_parts();
        cache.last_ram_ms = now;
    }
    
    if (cfg.enable_bat && (now - cache.last_bat_ms >= cfg.interval_bat_ms || cache.last_bat_ms == 0)) {
        cache.bat = read_battery_percent();
        cache.last_bat_ms = now;
    }
    
    if (cfg.enable_wifi && (now - cache.last_wifi_ms >= cfg.interval_wifi_ms || cache.last_wifi_ms == 0)) {
        cache.wifi = read_wifi_rssi();
        cache.last_wifi_ms = now;
    }
}

uint32_t apply_opacity(uint32_t color, int opacity) {
    if (opacity >= 100) return color | 0xFF000000;
    if (opacity <= 0) return 0;

    uint32_t a = (opacity * 255) / 100;
    uint32_t r = (color >> 16) & 0xFF;
    uint32_t g = (color >> 8) & 0xFF;
    uint32_t b = color & 0xFF;

    r = (r * a) / 255;
    g = (g * a) / 255;
    b = (b * a) / 255;

    return (a << 24) | (r << 16) | (g << 8) | b;
}

uint32_t mix_colors(uint32_t c1, uint32_t c2, float ratio) {
    if (ratio <= 0.0f) return c1;
    if (ratio >= 1.0f) return c2;
    int r1 = (c1 >> 16) & 0xFF;
    int g1 = (c1 >> 8) & 0xFF;
    int b1 = c1 & 0xFF;
    int r2 = (c2 >> 16) & 0xFF;
    int g2 = (c2 >> 8) & 0xFF;
    int b2 = c2 & 0xFF;
    uint32_t r = r1 + ratio * (r2 - r1);
    uint32_t g = g1 + ratio * (g2 - g1);
    uint32_t b = b1 + ratio * (b2 - b1);
    return (r << 16) | (g << 8) | b;
}

// Determines the marker color. It is fully opaque if high/mid, else follows base opacity.
uint32_t get_marker_color(int pct, uint32_t base_color, uint32_t mid_color, uint32_t high_color, int mid_pct, int high_pct, bool inverse) {
    bool is_mid = false;
    bool is_high = false;

    if (inverse) {
        // lower is worse
        if (pct < cfg.bat_low_pct) is_high = true; 
    } else {
        // higher is worse
        if (pct >= high_pct) is_high = true;
        else if (pct >= mid_pct) is_mid = true;
    }

    if (is_high) {
        return apply_opacity(high_color, 100); // opaque red/critical
    } else if (is_mid) {
        return apply_opacity(mid_color, 100);  // opaque amber/warning
    } else {
        return apply_opacity(base_color, cfg.opacity_pct);
    }
}

// Determines the color of the fill bar
uint32_t get_fill_color(int local_pct, uint32_t base_color, uint32_t mid_color, uint32_t high_color, int mid_pct, int high_pct, bool inverse, int alpha) {
    if (strcmp(cfg.skin, "gradient") != 0) {
        return apply_opacity(base_color, alpha); // Solid base color for all other skins
    }

    // Gradient skin logic
    uint32_t c;
    if (inverse) {
        if (local_pct < cfg.bat_low_pct) c = high_color; // usually bat_low
        else if (local_pct < mid_pct) {
            float ratio = (float)(local_pct - cfg.bat_low_pct) / std::max(mid_pct - cfg.bat_low_pct, 1);
            c = mix_colors(high_color, base_color, ratio);
        } else {
            c = base_color;
        }
    } else {
        if (local_pct <= mid_pct) {
            float ratio = (float)local_pct / std::max(mid_pct, 1);
            c = mix_colors(base_color, mid_color, ratio);
        } else if (local_pct <= high_pct) {
            float ratio = (float)(local_pct - mid_pct) / std::max(high_pct - mid_pct, 1);
            c = mix_colors(mid_color, high_color, ratio);
        } else {
            c = high_color;
        }
    }
    return apply_opacity(c, alpha);
}

// ---------------------------------------------------------------------- //
// shared-memory buffer handling
// ---------------------------------------------------------------------- //
const wl_buffer_listener buffer_listener = {
    .release = [](void *data, wl_buffer *) {
        static_cast<Buffer *>(data)->busy = false;
    },
};

void free_shm() {
    for (int i = 0; i < 2; ++i) {
        if (buffers[i].wlbuf) {
            wl_buffer_destroy(buffers[i].wlbuf);
            buffers[i].wlbuf = nullptr;
        }
        buffers[i].pixels = nullptr;
        buffers[i].busy = false;
    }
    if (shm_data && shm_data != MAP_FAILED) {
        munmap(shm_data, shm_size);
        shm_data = nullptr;
    }
    shm_size = 0;
}

bool alloc_shm(int width, int height) {
    size_t stride = static_cast<size_t>(width) * 4;
    size_t single = stride * static_cast<size_t>(height);
    shm_size       = single * 2;

    int fd = memfd_create("barcpp", MFD_CLOEXEC);
    if (fd < 0) { perror("memfd_create"); return false; }
    if (ftruncate(fd, static_cast<off_t>(shm_size)) < 0) { perror("ftruncate"); close(fd); return false; }

    shm_data = static_cast<uint8_t *>(
        mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (shm_data == MAP_FAILED) { perror("mmap"); close(fd); return false; }

    wl_shm_pool *pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(shm_size));
    for (int i = 0; i < 2; ++i) {
        buffers[i].pixels = reinterpret_cast<uint32_t *>(shm_data + i * single);
        buffers[i].wlbuf  = wl_shm_pool_create_buffer(
            pool, static_cast<int32_t>(i * single), width, height,
            static_cast<int32_t>(stride), WL_SHM_FORMAT_ARGB8888);
        wl_buffer_add_listener(buffers[i].wlbuf, &buffer_listener, &buffers[i]);
    }
    wl_shm_pool_destroy(pool);
    close(fd);
    return true;
}

void fill_span(uint32_t *pixels, int width, int x0, int x1, uint32_t col) {
    x0 = std::max(x0, 0);
    x1 = std::min(x1, width);
    const int h = cfg.height;
    for (int x = x0; x < x1; ++x)
        for (int y = 0; y < h; ++y)
            pixels[y * width + x] = col;
}

void draw(int width) {
    if (width <= 0) return;
    Buffer &b = buffers[cur_buffer];
    if (b.busy) return;

    update_metrics();

    int cpu = cache.cpu;
    int temp = cache.temp;
    RamParts ram = cache.ram;
    int bat = cache.bat;
    int wifi_dbm = cache.wifi;

    // Count dynamic segments
    int dynamic_segments = 0;
    if (cfg.enable_cpu) dynamic_segments++;
    if (cfg.enable_temp && temp >= 0) dynamic_segments++;
    if (cfg.enable_ram) dynamic_segments++;
    if (cfg.enable_wifi && wifi_dbm < 0) dynamic_segments++;
    if (cfg.enable_bat && bat >= 0) dynamic_segments++;

    int total_segments = dynamic_segments;

    if (total_segments == 0) {
        fill_span(b.pixels, width, 0, width, apply_opacity(cfg.color_bg, cfg.opacity_pct));
        wl_surface_attach(surface, b.wlbuf, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, width, cfg.height);
        b.busy = true;
        wl_surface_commit(surface);
        cur_buffer ^= 1;
        return;
    }

    const int gaps_w   = cfg.gap_px * (total_segments - 1);
    const int usable   = std::max(width - gaps_w, dynamic_segments);
    const int seg_w    = (dynamic_segments > 0) ? (usable / dynamic_segments) : 0;
    const int rem      = (dynamic_segments > 0) ? (usable % dynamic_segments) : 0;

    fill_span(b.pixels, width, 0, width, apply_opacity(cfg.color_bg, cfg.opacity_pct));

    int x = 0;
    int current_seg = 0;
    
    // Helper to draw a single dynamic segment with speedometer colors and skins
    auto draw_segment = [&](int pct, uint32_t base_color, uint32_t mid_color, uint32_t high_color, int mid_pct, int high_pct, bool inverse, bool solid_status_color = false) {
        int w = seg_w + (current_seg == dynamic_segments - 1 ? rem : 0);
        int fill_w = w * pct / 100;

        bool is_zebra = !strcmp(cfg.skin, "zebra");
        bool is_blocks = !strcmp(cfg.skin, "blocks");

        // Request color at 100% opacity, so we don't double-multiply it later
        uint32_t overall_col = get_fill_color(pct, base_color, mid_color, high_color, mid_pct, high_pct, inverse, 100);
        
        for (int dx = 0; dx < fill_w; ++dx) {
            uint32_t col;
            bool draw_pixel = true;

            // Marker logic
            if (dx < cfg.marker_px) {
                col = get_marker_color(pct, base_color, mid_color, high_color, mid_pct, high_pct, inverse);
            } else if (dx < cfg.marker_px + cfg.marker_gap_px) {
                draw_pixel = false; // Gap after marker
            } else {
                int local_pct = (dx * 100) / std::max(w, 1);
                
                // Alpha gradient logic
                int alpha = cfg.opacity_pct;
                if (cfg.opacity_pct_end != cfg.opacity_pct && w > 0) {
                    float ratio = (float)dx / w;
                    alpha = cfg.opacity_pct + ratio * (cfg.opacity_pct_end - cfg.opacity_pct);
                }

                if (solid_status_color) {
                    // overall_col is fully opaque, so we just apply alpha once
                    col = apply_opacity(overall_col | 0xFF000000, alpha);
                } else {
                    col = get_fill_color(local_pct, base_color, mid_color, high_color, mid_pct, high_pct, inverse, alpha);
                }

                if (is_zebra) {
                    int shifted_dx = dx - (cfg.marker_px + cfg.marker_gap_px);
                    int cycle = cfg.zebra_dash_px + cfg.zebra_space_px;
                    if (cycle > 0 && (shifted_dx % cycle) >= cfg.zebra_dash_px) {
                        draw_pixel = false;
                    }
                } else if (is_blocks) {
                    int block_w = w / std::max(cfg.blocks_count, 1);
                    if (block_w > 1) {
                        if ((dx % block_w) == block_w - 1) {
                            draw_pixel = false;
                        }
                    }
                }
            }

            if (draw_pixel) {
                for (int y = 0; y < cfg.height; ++y) {
                    b.pixels[y * width + (x + dx)] = col;
                }
            }
        }
        x += w;
        if (current_seg + 1 < total_segments) x += cfg.gap_px;
        current_seg++;
    };

    // 1. CPU
    if (cfg.enable_cpu) {
        int pct = clamp_pct(cpu);
        draw_segment(pct, cfg.color_cpu, cfg.color_cpu_mid, cfg.color_cpu_high, cfg.cpu_mid_pct, cfg.cpu_high_pct, false);
    }

    // 1.5 TEMP
    if (cfg.enable_temp && temp >= 0) {
        int pct = clamp_pct(temp * 100 / std::max(cfg.temp_max_c, 1));
        draw_segment(pct, cfg.color_temp, cfg.color_temp_mid, cfg.color_temp_high, 
                     cfg.temp_mid_c * 100 / std::max(cfg.temp_max_c, 1), 
                     cfg.temp_high_c * 100 / std::max(cfg.temp_max_c, 1), false);
    }

    // 2. RAM
    if (cfg.enable_ram) {
        int w = seg_w + (current_seg == dynamic_segments - 1 ? rem : 0);

        if (ram.ok) {
            int used_pct  = clamp_pct(ram.used_pct);
            int reclaim_pct = clamp_pct(ram.avail_pct - ram.free_pct); // simplified for brevity
            if (reclaim_pct < 0) reclaim_pct = 0;

            int used_w    = w * used_pct / 100;
            int reclaim_w = w * reclaim_pct / 100;

            bool is_zebra = !strcmp(cfg.skin, "zebra");
            bool is_blocks = !strcmp(cfg.skin, "blocks");

            // Draw Used memory (left part)
            for (int dx = 0; dx < used_w; ++dx) {
                uint32_t col;
                bool draw_pixel = true;

                if (dx < cfg.marker_px) {
                    col = get_marker_color(used_pct, cfg.color_ram_free, cfg.color_ram_mid, cfg.color_ram_high, cfg.ram_mid_pct, cfg.ram_high_pct, false);
                } else if (dx < cfg.marker_px + cfg.marker_gap_px) {
                    draw_pixel = false;
                } else {
                    int local_pct = (dx * 100) / std::max(w, 1);
                    int alpha = cfg.opacity_pct;
                    if (cfg.opacity_pct_end != cfg.opacity_pct && w > 0) {
                        float ratio = (float)dx / w;
                        alpha = cfg.opacity_pct + ratio * (cfg.opacity_pct_end - cfg.opacity_pct);
                    }
                    col = get_fill_color(local_pct, cfg.color_ram_free, cfg.color_ram_mid, cfg.color_ram_high, cfg.ram_mid_pct, cfg.ram_high_pct, false, alpha);

                    if (is_zebra) {
                        int shifted_dx = dx - (cfg.marker_px + cfg.marker_gap_px);
                        int cycle = cfg.zebra_dash_px + cfg.zebra_space_px;
                        if (cycle > 0 && (shifted_dx % cycle) >= cfg.zebra_dash_px) draw_pixel = false;
                    } else if (is_blocks) {
                        int block_w = w / std::max(cfg.blocks_count, 1);
                        if (block_w > 1 && (dx % block_w) == block_w - 1) draw_pixel = false;
                    }
                }

                if (draw_pixel) {
                    for (int y = 0; y < cfg.height; ++y) b.pixels[y * width + (x + dx)] = col;
                }
            }

            // Draw Reclaimable/Cache memory right after Used
            for (int dx = used_w; dx < used_w + reclaim_w && dx < w; ++dx) {
                bool draw_pixel = true;
                
                if (dx < cfg.marker_px) {
                    // Overlapping marker area - should be extremely rare if used is 0
                    draw_pixel = false;
                } else if (dx < cfg.marker_px + cfg.marker_gap_px) {
                    draw_pixel = false;
                } else {
                    if (is_zebra) {
                        int shifted_dx = dx - (cfg.marker_px + cfg.marker_gap_px);
                        int cycle = cfg.zebra_dash_px + cfg.zebra_space_px;
                        if (cycle > 0 && (shifted_dx % cycle) >= cfg.zebra_dash_px) draw_pixel = false;
                    } else if (is_blocks) {
                        int block_w = w / std::max(cfg.blocks_count, 1);
                        if (block_w > 1 && (dx % block_w) == block_w - 1) draw_pixel = false;
                    }
                }

                if (draw_pixel) {
                    int alpha = cfg.opacity_pct;
                    if (cfg.opacity_pct_end != cfg.opacity_pct && w > 0) {
                        float ratio = (float)dx / w;
                        alpha = cfg.opacity_pct + ratio * (cfg.opacity_pct_end - cfg.opacity_pct);
                    }
                    for (int y = 0; y < cfg.height; ++y) {
                        b.pixels[y * width + (x + dx)] = apply_opacity(cfg.color_ram_avail, alpha);
                    }
                }
            }
        }
        x += w;
        if (current_seg + 1 < total_segments) x += cfg.gap_px;
        current_seg++;
    }

    // 3. WIFI (if present)
    if (cfg.enable_wifi && wifi_dbm < 0) {
        // -50 dBm = 100%, -90 dBm = 0%
        int pct = clamp_pct((wifi_dbm + 90) * 100 / 40);
        // We pass solid_status_color=true so the whole bar uses the final color
        draw_segment(pct, cfg.color_wifi, cfg.color_wifi_mid, cfg.color_wifi_low, 33, 16, true, true);
    }

    // 4. BAT (if present)
    if (cfg.enable_bat && bat >= 0) {
        int pct = clamp_pct(bat);
        // Pass solid_status_color=true
        draw_segment(pct, cfg.color_bat, cfg.color_bat_low, cfg.color_bat_low, 50, cfg.bat_low_pct, true, true);
    }

    wl_surface_attach(surface, b.wlbuf, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, cfg.height);
    b.busy = true;
    wl_surface_commit(surface);
    cur_buffer ^= 1;
}

// ---------------------------------------------------------------------- //
// listeners
// ---------------------------------------------------------------------- //
const zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = [](void *, zwlr_layer_surface_v1 *ls, uint32_t serial,
                    uint32_t width, uint32_t) {
        zwlr_layer_surface_v1_ack_configure(ls, serial);
        if (width > 0) {
            if (configured && surf_width != static_cast<int>(width)) {
                free_shm();
                configured = false;
            }
            surf_width = static_cast<int>(width);
            if (!configured) {
                configured = alloc_shm(surf_width, cfg.height);
            }
        }
        if (configured) draw(surf_width);
    },
    .closed = [](void *, zwlr_layer_surface_v1 *) { running = false; },
};

const wl_registry_listener registry_listener = {
    .global = [](void *, wl_registry *reg, uint32_t name,
                 const char *interface, uint32_t) {
        if (!strcmp(interface, wl_compositor_interface.name))
            compositor = static_cast<wl_compositor *>(
                wl_registry_bind(reg, name, &wl_compositor_interface, 4));
        else if (!strcmp(interface, wl_shm_interface.name))
            shm = static_cast<wl_shm *>(
                wl_registry_bind(reg, name, &wl_shm_interface, 1));
        else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name))
            layer_shell = static_cast<zwlr_layer_shell_v1 *>(
                wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1));
    },
    .global_remove = [](void *, wl_registry *, uint32_t) {},
};

} // namespace

int main() {
    load_config();

    display = wl_display_connect(nullptr);
    if (!display) {
        fprintf(stderr, "barcpp: cannot connect to a Wayland display\n");
        return 1;
    }

    wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, nullptr);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !layer_shell) {
        fprintf(stderr,
                "barcpp: compositor is missing wl_compositor/wl_shm/"
                "zwlr_layer_shell_v1 (are you on a wlroots compositor?)\n");
        return 1;
    }

    surface = wl_compositor_create_surface(compositor);
    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, /*output=*/nullptr,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "barcpp");

    zwlr_layer_surface_v1_set_anchor(
        layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_margin(layer_surface, 0, cfg.margin_right_px, 0, cfg.margin_left_px);
    zwlr_layer_surface_v1_set_size(layer_surface, 0, cfg.height);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, cfg.push_windows_px);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 0);
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, nullptr);

    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    (void)read_cpu_percent();

    int wl_fd = wl_display_get_fd(display);
    while (running) {
        wl_display_flush(display);

        pollfd pfd{wl_fd, POLLIN, 0};
        int ret = poll(&pfd, 1, cfg.poll_ms);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_dispatch(display) < 0) break;
        } else if (ret == 0) {
            draw(surf_width);
        }
    }

    wl_display_disconnect(display);
    return 0;
}
