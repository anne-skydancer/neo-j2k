// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// mqng_profile: the one-thread decode of the encoder study's textures (bench/t1_study/driver.cpp: the
// viewer's upload encode of the synthetic texture, three components) at 256, 512 and 1,024 pixels, lossy and
// lossless, through the instrumented library: the stage shares (Tier-2, Tier-1, wavelet, the rest), the
// median of the repeats. It writes each codestream and the dump of its code-blocks (blockio.h) to --out. With
// --files LIST it also dumps the code-blocks of every file in the list (one dump per file, for the replay's
// exactness check and the mode-switch census).
//
//   mqng_profile --out DIR [--reps 15] [--files LIST]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <neoj2k/neoj2k.h>

#include "../t1_study/driver.h"
#include "hooks.h"

#include <windows.h>

namespace {

std::vector<std::uint8_t> read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// header and decode into int32 planes; the cycles of both
bool decode(const std::vector<std::uint8_t>& data, std::uint64_t* cycles, std::uint64_t* hash)
{
    const std::uint64_t t0 = mqs::tsc();
    nj2k_codec_t* codec = nj2k_create_decompress(NJ2K_CODEC_J2K);
    if (!codec) {
        return false;
    }
    nj2k_dparameters_t p;
    nj2k_set_default_decoder_parameters(&p);
    nj2k_setup_decoder(codec, &p);
    nj2k_image_t image{};
    bool ok = nj2k_read_header_memory(codec, data.data(), data.size(), &image) != NJ2K_FALSE;
    static std::vector<std::vector<std::int32_t>> planes;
    std::vector<nj2k_plane_t> pl;
    planes.resize(ok ? image.numcomps : 0);
    for (NJ2K_UINT32 c = 0; ok && c < image.numcomps; ++c) {
        planes[c].assign(std::size_t(image.comps[c].w) * image.comps[c].h, 0);
        pl.push_back(nj2k_plane_t{planes[c].data(), static_cast<NJ2K_INT64>(image.comps[c].w) * 4, 4, c});
    }
    if (ok) {
        const nj2k_output_t out{sizeof(nj2k_output_t), NJ2K_SAMPLE_I32, image.numcomps, pl.data()};
        ok = nj2k_decode_memory(codec, data.data(), data.size(), &out) != NJ2K_FALSE;
    }
    nj2k_destroy_codec(codec);
    const std::uint64_t t1 = mqs::tsc();
    if (cycles) {
        *cycles = t1 - t0;
    }
    if (hash) {
        std::uint64_t h = 1469598103934665603ull;
        for (const auto& v : planes) {
            for (std::int32_t s : v) {
                h ^= static_cast<std::uint32_t>(s);
                h *= 1099511628211ull;
            }
        }
        *hash = h;
    }
    return ok;
}

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv)
{
    std::string out, list;
    int reps = 15;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) {
            out = argv[++i];
        } else if (a == "--reps" && i + 1 < argc) {
            reps = std::atoi(argv[++i]);
        } else if (a == "--files" && i + 1 < argc) {
            list = argv[++i];
        }
    }
    if (out.empty()) {
        std::fprintf(stderr, "usage: mqng_profile --out DIR [--reps 15] [--files LIST]\n");
        return 2;
    }
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << 7);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    std::printf("TSC %.3f GHz\n", study::tsc_ghz());
    std::printf("%-14s %10s %12s %7s %7s %7s %7s\n", "config", "bytes", "Mcycles", "T2 %", "T1 %", "DWT %", "rest %");
    const int sizes[3] = {256, 512, 1024};
    for (int si = 0; si < 3; ++si) {
        for (int rev = 0; rev < 2; ++rev) {
            const int s = sizes[si];
            const auto raw = study::texture(s, s, 3);
            std::vector<std::uint8_t> cs;
            if (!study::upload(raw, s, s, 3, rev != 0, cs, nullptr)) {
                std::printf("encode failed\n");
                return 1;
            }
            const std::string name = std::to_string(s) + (rev ? "_lossless" : "_lossy");
            {
                std::ofstream f(out + "/" + name + ".j2k", std::ios::binary);
                f.write(reinterpret_cast<const char*>(cs.data()), static_cast<std::streamsize>(cs.size()));
            }
            // the dump, untimed
            mqs::g_blocks.clear();
            mqs::g_dump = true;
            std::uint64_t hash = 0;
            decode(cs, nullptr, &hash);
            mqs::g_dump = false;
            mqs::write_blocks(out + "/" + name + ".blk", mqs::g_blocks);
            const std::size_t nblocks = mqs::g_blocks.size();
            mqs::g_blocks.clear();
            std::vector<double> tot, t2, t1, dw;
            const int r = s == 1024 ? (reps + 1) / 2 : reps;
            for (int k = 0; k < r + 1; ++k) {
                mqs::g_clock = {};
                std::uint64_t c = 0;
                decode(cs, &c, nullptr);
                if (k == 0) {
                    continue;  // warm-up
                }
                tot.push_back(double(c));
                t2.push_back(double(mqs::g_clock.cycles[mqs::kT2]) / double(c));
                t1.push_back(double(mqs::g_clock.cycles[mqs::kT1]) / double(c));
                dw.push_back(double(mqs::g_clock.cycles[mqs::kDwt]) / double(c));
            }
            const double mt2 = median(t2), mt1 = median(t1), mdw = median(dw);
            std::printf("%-14s %10zu %12.2f %7.1f %7.1f %7.1f %7.1f  blocks %zu hash %016llx\n", name.c_str(), cs.size(),
                        median(tot) / 1e6, 100 * mt2, 100 * mt1, 100 * mdw, 100 * (1 - mt2 - mt1 - mdw), nblocks,
                        static_cast<unsigned long long>(hash));
        }
    }
    if (!list.empty()) {
        std::ifstream in(list);
        std::string path;
        int n = 0;
        while (std::getline(in, path)) {
            if (!path.empty() && path.back() == '\r') {
                path.pop_back();
            }
            if (path.empty()) {
                continue;
            }
            const auto data = read_file(path);
            mqs::g_blocks.clear();
            mqs::g_dump = true;
            std::uint64_t hash = 0;
            const bool ok = decode(data, nullptr, &hash);
            mqs::g_dump = false;
            const std::string base = path.substr(path.find_last_of("/\\") + 1);
            mqs::write_blocks(out + "/files/" + base + ".blk", mqs::g_blocks);
            std::printf("dumped %-40s %s blocks %zu\n", base.c_str(), ok ? "ok  " : "FAIL", mqs::g_blocks.size());
            ++n;
        }
        std::printf("%d files\n", n);
    }
    return 0;
}
