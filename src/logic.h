#pragma once

// Pure threshold helpers. No Wayland, sysfs or config state here, so they can be unit-tested.

inline int clamp_pct(int pct) {
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return pct;
}

// Wi-Fi bar length: -50 dBm = 100%, -90 dBm = 0%
inline int wifi_dbm_to_pct(int dbm) {
    return clamp_pct((dbm + 90) * 100 / 40);
}

// PSI overlay length in percent of the segment: 0..high_pct maps to 0..100%, larger values are clamped.
inline int psi_overlay_pct(int psi, int high_pct) {
    int denom = high_pct > 0 ? high_pct : 1;
    return clamp_pct(psi * 100 / denom);
}

// Returns 2 for critical, 1 for warning, 0 for normal.
// inverse: low values are bad (BAT, WIFI), so thresholds are checked from below.
inline int get_marker_state(int pct, int mid_pct, int high_pct, bool inverse) {
    if (inverse) {
        if (pct < high_pct) return 2;
        if (pct < mid_pct) return 1;
        return 0;
    }
    if (pct >= high_pct) return 2;
    if (pct >= mid_pct) return 1;
    return 0;
}
