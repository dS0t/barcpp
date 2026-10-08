#include "logic.h"

#include <cstdio>

static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

int main() {
    // clamp_pct
    CHECK(clamp_pct(-5) == 0);
    CHECK(clamp_pct(50) == 50);
    CHECK(clamp_pct(150) == 100);

    // wifi_dbm_to_pct: linear in dBm, -50 = 100%, -90 = 0%
    CHECK(wifi_dbm_to_pct(-30) == 100);
    CHECK(wifi_dbm_to_pct(-50) == 100);
    CHECK(wifi_dbm_to_pct(-70) == 50);
    CHECK(wifi_dbm_to_pct(-80) == 25);
    CHECK(wifi_dbm_to_pct(-90) == 0);
    CHECK(wifi_dbm_to_pct(-100) == 0);

    // normal direction (high value is bad, e.g. CPU, RAM, DISK, TEMP)
    CHECK(get_marker_state(59, 60, 80, false) == 0);
    CHECK(get_marker_state(60, 60, 80, false) == 1);
    CHECK(get_marker_state(79, 60, 80, false) == 1);
    CHECK(get_marker_state(80, 60, 80, false) == 2);

    // inverse direction (low value is bad), BAT: warning below 30, critical below 10
    CHECK(get_marker_state(31, 30, 10, true) == 0);
    CHECK(get_marker_state(30, 30, 10, true) == 0);
    CHECK(get_marker_state(29, 30, 10, true) == 1);
    CHECK(get_marker_state(10, 30, 10, true) == 1);
    CHECK(get_marker_state(9, 30, 10, true) == 2);
    CHECK(get_marker_state(0, 30, 10, true) == 2);

    // WIFI with dBm thresholds -70 (warning) and -80 (critical), mapped to bar percent
    const int wifi_mid = wifi_dbm_to_pct(-70);
    const int wifi_low = wifi_dbm_to_pct(-80);
    CHECK(get_marker_state(wifi_dbm_to_pct(-60), wifi_mid, wifi_low, true) == 0);
    CHECK(get_marker_state(wifi_dbm_to_pct(-75), wifi_mid, wifi_low, true) == 1);
    CHECK(get_marker_state(wifi_dbm_to_pct(-85), wifi_mid, wifi_low, true) == 2);

    // SWAP occupancy: warning from 25%, critical from 60% (normal direction)
    CHECK(get_marker_state(24, 25, 60, false) == 0);
    CHECK(get_marker_state(25, 25, 60, false) == 1);
    CHECK(get_marker_state(60, 25, 60, false) == 2);

    // PSI "some avg10": warning from 5%, critical from 20% (normal direction)
    CHECK(get_marker_state(4, 5, 20, false) == 0);
    CHECK(get_marker_state(5, 5, 20, false) == 1);
    CHECK(get_marker_state(20, 5, 20, false) == 2);

    // PSI overlay length: 0..psi_high_pct (20) maps to 0..100%, clamped above
    CHECK(psi_overlay_pct(0, 20) == 0);
    CHECK(psi_overlay_pct(5, 20) == 25);
    CHECK(psi_overlay_pct(20, 20) == 100);
    CHECK(psi_overlay_pct(100, 20) == 100);
    CHECK(psi_overlay_pct(5, 0) == 100);

    if (failures == 0) {
        std::puts("all tests passed");
        return 0;
    }
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
}
