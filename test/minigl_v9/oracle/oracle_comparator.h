/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Differential oracle image comparator for ZZ9000 MiniGL wire v9.
 */

#ifndef ORACLE_COMPARATOR_H
#define ORACLE_COMPARATOR_H

#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>

struct CompareMetrics {
    uint32_t total_pixels = 0;
    uint32_t mismatch_pixels = 0;
    uint32_t coverage_mismatch = 0;
    uint32_t max_channel_error = 0;
    double mean_channel_error_all = 0.0;
    double mean_channel_error_mismatch = 0.0;
    bool pass = false;
    std::string verdict; // "PASS", "FAIL", "DEFERRED"
    std::string details;
};

inline CompareMetrics compare_frames(uint32_t width, uint32_t height,
                                    const uint8_t* test_rgba,
                                    const uint8_t* ref_rgba,
                                    uint32_t submit_status = 0,
                                    uint32_t max_tol = 4,
                                    const std::string& case_name = "")
{
    CompareMetrics m;
    m.total_pixels = width * height;

    if (submit_status != 0) {
        // Submit failed / unsupported
        m.pass = false;
        if (submit_status == 0xfffffffb) {
            m.verdict = "DEFERRED";
            m.details = "Opcode deferred in current unit (returned FAILED)";
        } else if (submit_status == 0xfffffffe) {
            m.verdict = "FAIL";
            m.details = "Packet rejected (INVALID)";
        } else {
            m.verdict = "FAIL";
            m.details = "Submit returned status 0x" + std::to_string(submit_status);
        }
        return m;
    }

    uint64_t sum_err_all = 0;
    uint64_t sum_err_mismatch = 0;

    for (uint32_t i = 0; i < m.total_pixels; ++i) {
        const uint8_t* t = test_rgba + i * 4;
        const uint8_t* r = ref_rgba + i * 4;

        int dr = std::abs((int)t[0] - (int)r[0]);
        int dg = std::abs((int)t[1] - (int)r[1]);
        int db = std::abs((int)t[2] - (int)r[2]);
        int da = std::abs((int)t[3] - (int)r[3]);

        int p_max = std::max({dr, dg, db, da});
        if (p_max > 0) {
            m.mismatch_pixels++;
            sum_err_mismatch += (dr + dg + db + da);
        }
        sum_err_all += (dr + dg + db + da);

        if ((uint32_t)p_max > m.max_channel_error)
            m.max_channel_error = p_max;

        // Coverage check: one has drawn content (alpha > 0 and non-black), other is clear/empty
        bool t_drawn = (t[3] > 0 && (t[0] > 0 || t[1] > 0 || t[2] > 0));
        bool r_drawn = (r[3] > 0 && (r[0] > 0 || r[1] > 0 || r[2] > 0));
        if (t_drawn != r_drawn)
            m.coverage_mismatch++;
    }

    m.mean_channel_error_all = (double)sum_err_all / (4.0 * m.total_pixels);
    m.mean_channel_error_mismatch = (m.mismatch_pixels > 0)
        ? ((double)sum_err_mismatch / (4.0 * m.mismatch_pixels))
        : 0.0;

    bool is_line_or_point = (case_name.find("lines") != std::string::npos ||
                             case_name.find("points") != std::string::npos);

    if (m.coverage_mismatch > 0) {
        m.pass = false;
        m.verdict = "FAIL";
        if (is_line_or_point)
            m.details = "Line/point coverage mismatch (" + std::to_string(m.coverage_mismatch) + " px) violates GL raster rule";
        else
            m.details = "Filled polygon coverage mismatch (" + std::to_string(m.coverage_mismatch) + " px) violates watertight rule";
    } else if (m.max_channel_error == 0) {
        m.pass = true;
        m.verdict = "PASS";
        m.details = "Exact pixel match";
    } else if (m.max_channel_error <= max_tol) {
        m.pass = true;
        m.verdict = "PASS";
        m.details = "Within tolerance (raster/fixed-point rounding)";
    } else {
        m.pass = false;
        m.verdict = "FAIL";
        m.details = "Max channel error (" + std::to_string(m.max_channel_error) + ") exceeds tolerance";
    }

    return m;
}

inline void generate_diff_rgba(uint32_t width, uint32_t height,
                               const uint8_t* test_rgba,
                               const uint8_t* ref_rgba,
                               uint8_t* diff_rgba)
{
    uint32_t total = width * height;
    for (uint32_t i = 0; i < total; ++i) {
        const uint8_t* t = test_rgba + i * 4;
        const uint8_t* r = ref_rgba + i * 4;
        uint8_t* d = diff_rgba + i * 4;

        int dr = std::abs((int)t[0] - (int)r[0]);
        int dg = std::abs((int)t[1] - (int)r[1]);
        int db = std::abs((int)t[2] - (int)r[2]);
        int da = std::abs((int)t[3] - (int)r[3]);
        int max_err = std::max({dr, dg, db, da});

        bool t_drawn = (t[3] > 0 && (t[0] > 0 || t[1] > 0 || t[2] > 0));
        bool r_drawn = (r[3] > 0 && (r[0] > 0 || r[1] > 0 || r[2] > 0));

        if (max_err == 0) {
            // Match: Dimmed reference pixel
            d[0] = r[0] / 3;
            d[1] = r[1] / 3;
            d[2] = r[2] / 3;
            d[3] = 255;
        } else if (t_drawn != r_drawn) {
            // Coverage mismatch: Bright magenta
            d[0] = 255;
            d[1] = 0;
            d[2] = 255;
            d[3] = 255;
        } else if (max_err <= 8) {
            // Small raster/rounding diff: Cyan highlight
            d[0] = 0;
            d[1] = (uint8_t)std::min(255, 120 + max_err * 15);
            d[2] = (uint8_t)std::min(255, 120 + max_err * 15);
            d[3] = 255;
        } else {
            // Large channel diff: Bright red highlight
            d[0] = 255;
            d[1] = 0;
            d[2] = (uint8_t)std::min(255, max_err * 2);
            d[3] = 255;
        }
    }
}

#endif // ORACLE_COMPARATOR_H
