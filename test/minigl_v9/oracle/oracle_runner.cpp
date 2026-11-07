/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Differential oracle test runner for ZZ9000 MiniGL wire v9.
 * Loads the wire v9 packet corpus, drives mw_host_* API, compares against
 * the Amiberry plugin reference frames, and emits diff PNGs.
 */

#include "mw_host.h"
#include "minipng.h"
#include "oracle_comparator.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct LoadedCase {
    std::string name;
    fs::path dir_path;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<std::vector<uint8_t>> packets;
    struct Req {
        std::vector<uint8_t> data;
        uint32_t reply_capacity = 0;
    };
    std::vector<Req> requests;
    std::vector<uint8_t> ref_rgba;
    std::vector<std::vector<uint8_t>> ref_replies;
};

static uint32_t read_u32_be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool load_case(const fs::path& case_dir, LoadedCase& out)
{
    out.name = case_dir.filename().string();
    out.dir_path = case_dir;

    fs::path bin_path = case_dir / "case.bin";
    std::ifstream bf(bin_path, std::ios::binary | std::ios::ate);
    if (!bf.is_open())
        return false;

    size_t file_size = bf.tellg();
    bf.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(file_size);
    bf.read(reinterpret_cast<char*>(buf.data()), file_size);

    if (file_size < 20)
        return false;

    uint32_t magic = read_u32_be(buf.data());
    uint32_t ver   = read_u32_be(buf.data() + 4);
    if (magic != 0x4D574331 || ver != 1)
        return false;

    out.width  = read_u32_be(buf.data() + 8);
    out.height = read_u32_be(buf.data() + 12);
    uint32_t num_packets = read_u32_be(buf.data() + 16);

    size_t offset = 20;
    for (uint32_t i = 0; i < num_packets; ++i) {
        if (offset + 4 > file_size) return false;
        uint32_t pkt_len = read_u32_be(buf.data() + offset);
        offset += 4;
        if (offset + pkt_len > file_size) return false;
        std::vector<uint8_t> pkt(buf.data() + offset, buf.data() + offset + pkt_len);
        out.packets.push_back(std::move(pkt));
        offset += pkt_len;
    }

    if (offset + 4 <= file_size) {
        uint32_t num_reqs = read_u32_be(buf.data() + offset);
        offset += 4;
        for (uint32_t i = 0; i < num_reqs; ++i) {
            if (offset + 8 > file_size) return false;
            uint32_t req_len = read_u32_be(buf.data() + offset);
            uint32_t rep_cap = read_u32_be(buf.data() + offset + 4);
            offset += 8;
            if (offset + req_len > file_size) return false;
            LoadedCase::Req r;
            r.data.assign(buf.data() + offset, buf.data() + offset + req_len);
            r.reply_capacity = rep_cap;
            out.requests.push_back(std::move(r));
            offset += req_len;
        }
    }

    // Load reference RGBA
    fs::path ref_path = case_dir / "case.ref.rgba";
    std::ifstream rf(ref_path, std::ios::binary | std::ios::ate);
    if (rf.is_open()) {
        size_t ref_size = rf.tellg();
        rf.seekg(0, std::ios::beg);
        out.ref_rgba.resize(ref_size);
        rf.read(reinterpret_cast<char*>(out.ref_rgba.data()), ref_size);
    }

    // Load reference replies
    for (size_t i = 0; i < out.requests.size(); ++i) {
        fs::path reply_path = case_dir / ("reply_" + std::to_string(i) + ".bin");
        std::ifstream rep_f(reply_path, std::ios::binary | std::ios::ate);
        if (rep_f.is_open()) {
            size_t rep_size = rep_f.tellg();
            rep_f.seekg(0, std::ios::beg);
            std::vector<uint8_t> rep_data(rep_size);
            rep_f.read(reinterpret_cast<char*>(rep_data.data()), rep_size);
            out.ref_replies.push_back(std::move(rep_data));
        }
    }

    return true;
}

int main(int argc, char* argv[])
{
    std::string corpus_dir = "build/minigl_v9_corpus";
    std::string diff_dir = "build/oracle_diffs";

    if (argc >= 2)
        corpus_dir = argv[1];
    if (argc >= 3)
        diff_dir = argv[2];

    std::printf("================================================================================\n");
    std::printf("ZZ9000 MiniGL Wire v9 Differential Oracle Runner\n");
    std::printf("Corpus dir: %s\n", corpus_dir.c_str());
    std::printf("Diff output dir: %s\n", diff_dir.c_str());
    std::printf("================================================================================\n\n");

    if (!fs::exists(corpus_dir)) {
        std::fprintf(stderr, "Error: Corpus directory %s does not exist.\n", corpus_dir.c_str());
        return 1;
    }

    fs::create_directories(diff_dir);

    std::vector<fs::path> case_dirs;
    for (const auto& entry : fs::directory_iterator(corpus_dir)) {
        if (entry.is_directory())
            case_dirs.push_back(entry.path());
    }
    std::sort(case_dirs.begin(), case_dirs.end());

    if (case_dirs.empty()) {
        std::fprintf(stderr, "Error: No test cases found in %s\n", corpus_dir.c_str());
        return 1;
    }

    std::printf("Discovered %zu test cases.\n\n", case_dirs.size());

    // Print table header
    std::printf("%-28s %-9s %-10s %-10s %-8s %-8s %-10s %s\n",
        "Case", "Size", "Submit", "CovMismatch", "MaxErr", "MeanErr", "Verdict", "Details");
    std::printf("--------------------------------------------------------------------------------------------------------\n");

    int total_pass = 0;
    int total_fail = 0;
    int total_deferred = 0;

    for (const auto& cdir : case_dirs) {
        LoadedCase tc;
        if (!load_case(cdir, tc)) {
            std::printf("%-28s %-9s %-10s %-10s %-8s %-8s %-10s %s\n",
                cdir.filename().string().c_str(), "???", "ERROR", "-", "-", "-", "FAIL", "Failed to load case.bin");
            total_fail++;
            continue;
        }

        std::string dim_str = std::to_string(tc.width) + "x" + std::to_string(tc.height);

        mw_ctx* ctx = mw_host_create(tc.width, tc.height);
        if (!ctx) {
            std::printf("%-28s %-9s %-10s %-10s %-8s %-8s %-10s %s\n",
                tc.name.c_str(), dim_str.c_str(), "NULL_CTX", "-", "-", "-", "FAIL", "mw_host_create returned NULL");
            total_fail++;
            continue;
        }

        uint32_t last_submit_code = 0;
        for (const auto& pkt : tc.packets) {
            last_submit_code = mw_host_submit(ctx, pkt.data(), (uint32_t)pkt.size());
            if (last_submit_code != 0)
                break;
        }

        // Perform requests if any
        bool request_failed = false;
        std::string req_error;
        bool version_only = tc.packets.empty() && tc.requests.size() == 1 &&
            tc.requests[0].data.size() == 4 && read_u32_be(tc.requests[0].data.data()) == 4 /* MW_PLUGIN_VERSION */;
        std::vector<uint8_t> reply;
        for (size_t i = 0; i < tc.requests.size(); ++i) {
            const auto& req = tc.requests[i];
            if (reply.size() < req.reply_capacity)
                reply.resize(req.reply_capacity, 0);
            uint32_t reply_bytes = 0;
            uint32_t rcode = mw_host_request(ctx, req.data.data(), (uint32_t)req.data.size(),
                                             reply.data(), req.reply_capacity, &reply_bytes);
            if (rcode != 0) {
                request_failed = true;
                req_error = "Request returned 0x" + std::to_string(rcode);
            } else if (version_only) {
                // The version string is backend-specific: the firmware does not
                // (and must not) reproduce the plugin's release string. Check it
                // is a non-empty NUL-terminated ASCII string within the reply.
                if (reply_bytes == 0 || reply_bytes > 64 /* MW_PLUGIN_VERSION_BYTES */) {
                    request_failed = true;
                    req_error = "Plugin version reply length invalid";
                } else {
                    size_t nul = 0;
                    while (nul < reply_bytes && reply[nul] != 0) {
                        if (reply[nul] < 0x20 || reply[nul] > 0x7e) break;
                        ++nul;
                    }
                    if (nul == 0 || nul >= reply_bytes || reply[nul] != 0) {
                        request_failed = true;
                        req_error = "Plugin version reply not NUL-terminated ASCII";
                    }
                }
            } else if (i < tc.ref_replies.size()) {
                const auto& ref_rep = tc.ref_replies[i];
                if (reply_bytes != ref_rep.size() ||
                    (reply_bytes > 0 && std::memcmp(reply.data(), ref_rep.data(), reply_bytes) != 0)) {
                    req_error = "Request reply bytes mismatch";
                }
            }
        }

        std::vector<uint8_t> test_rgba(tc.width * tc.height * 4, 0);
        uint32_t readback_code = mw_host_readback_rgba8(ctx, test_rgba.data(), (uint32_t)test_rgba.size());
        mw_host_destroy(ctx);

        if (readback_code != 0 && last_submit_code == 0) {
            std::printf("%-28s %-9s 0x%-8x %-10s %-8s %-8s %-10s %s\n",
                tc.name.c_str(), dim_str.c_str(), last_submit_code, "-", "-", "-", "FAIL", "mw_host_readback_rgba8 failed");
            total_fail++;
            continue;
        }

        if (tc.ref_rgba.empty() || tc.ref_rgba.size() != test_rgba.size()) {
            std::printf("%-28s %-9s 0x%-8x %-10s %-8s %-8s %-10s %s\n",
                tc.name.c_str(), dim_str.c_str(), last_submit_code, "-", "-", "-", "FAIL", "Missing or size-mismatched case.ref.rgba");
            total_fail++;
            continue;
        }

        if (version_only) {
            // No frame was rendered: the frame comparison is meaningless here.
            std::printf("%-28s %-9s 0x%-8x %-10s %-8s %-8s %-10s %s\n",
                tc.name.c_str(), dim_str.c_str(), last_submit_code, "-", "-", "-",
                request_failed ? "FAIL" : "PASS",
                request_failed ? req_error.c_str() : "Version reply OK (frame skipped)");
            if (request_failed) total_fail++; else total_pass++;
            continue;
        }

        CompareMetrics metrics = compare_frames(tc.width, tc.height, test_rgba.data(), tc.ref_rgba.data(), last_submit_code, 4, tc.name);
        if (request_failed && metrics.pass) {
            metrics.pass = false;
            metrics.verdict = "FAIL";
            metrics.details = req_error;
        }

        // Generate diff image
        std::vector<uint8_t> diff_rgba(tc.width * tc.height * 4, 255);
        generate_diff_rgba(tc.width, tc.height, test_rgba.data(), tc.ref_rgba.data(), diff_rgba.data());

        fs::path diff_png_path = fs::path(diff_dir) / (tc.name + "_diff.png");
        minipng::write_rgba_png(diff_png_path.string(), tc.width, tc.height, diff_rgba.data());

        // If failure, also write test and ref PNGs for inspection
        if (!metrics.pass && metrics.verdict == "FAIL") {
            fs::path test_png_path = fs::path(diff_dir) / (tc.name + "_test.png");
            fs::path ref_png_path  = fs::path(diff_dir) / (tc.name + "_ref.png");
            minipng::write_rgba_png(test_png_path.string(), tc.width, tc.height, test_rgba.data());
            minipng::write_rgba_png(ref_png_path.string(), tc.width, tc.height, tc.ref_rgba.data());
        }

        char cov_buf[32];
        std::snprintf(cov_buf, sizeof(cov_buf), "%u (%0.1f%%)",
            metrics.coverage_mismatch, (double)metrics.coverage_mismatch * 100.0 / metrics.total_pixels);

        char max_buf[16], mean_buf[16], sub_buf[16];
        std::snprintf(max_buf, sizeof(max_buf), "%u", metrics.max_channel_error);
        std::snprintf(mean_buf, sizeof(mean_buf), "%.2f", metrics.mean_channel_error_all);
        std::snprintf(sub_buf, sizeof(sub_buf), "0x%08x", last_submit_code);

        std::printf("%-28s %-9s %-10s %-10s %-8s %-8s %-10s %s\n",
            tc.name.c_str(), dim_str.c_str(), sub_buf, cov_buf, max_buf, mean_buf,
            metrics.verdict.c_str(), metrics.details.c_str());

        if (metrics.verdict == "PASS")
            total_pass++;
        else if (metrics.verdict == "DEFERRED")
            total_deferred++;
        else
            total_fail++;
    }

    std::printf("--------------------------------------------------------------------------------------------------------\n");
    std::printf("Summary: %zu cases | %d Passed | %d Deferred | %d Failed\n",
        case_dirs.size(), total_pass, total_deferred, total_fail);
    std::printf("Diff images written to: %s/\n", diff_dir.c_str());

    return (total_fail == 0) ? 0 : 2;
}
