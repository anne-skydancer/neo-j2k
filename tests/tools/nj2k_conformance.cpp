// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// nj2k_conformance: ISO/IEC 15444-4 (ITU-T T.803) class 1 decoding, profiles 0 and 1, against the
// reference images, with the peak-error and mean-squared-error limits of T.803 (02/2024), Table C.6
// and its profile 1 counterpart. The test files come from OpenJPEG's test data (openjpeg-data:
// input/conformance and baseline/conformance), fetched by tools/fetch_testdata.py; their notice
// allows use for conformance testing only, so they are never committed here.
//
//   nj2k_conformance --a SPEC --data DIR [--mode plain|viewer]
// Exit status: 0 all pass, 1 a failure, 77 the data folder is missing (CTest reports a skip).
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common.h"
#include "decoder.h"
#include "pgx.h"

using namespace nj2k::test;

namespace {

struct Case {
    const char* input;
    int reduce;
    const char* reference;  // c1p?_??: components <reference>_<c>.pgx
    std::vector<double> peak, mse;
};

// T.803 (02/2024) limits. Where OpenJPEG's own list differs, T.803 wins: c1p1_05 component 1 has
// an MSE limit of 9.716 (OpenJPEG lists 9.816), and the 2024 edition sets p0_05 and p1_03 as below.
const std::vector<Case>& cases()
{
    static const std::vector<Case> c = {
        {"p0_01.j2k", 0, "c1p0_01", {0}, {0}},
        {"p0_02.j2k", 0, "c1p0_02", {0}, {0}},
        {"p0_03.j2k", 0, "c1p0_03", {0}, {0}},
        {"p0_04.j2k", 0, "c1p0_04", {5, 4, 6}, {0.776, 0.626, 1.070}},
        {"p0_05.j2k", 0, "c1p0_05", {2, 2, 2, 0}, {0.319, 0.323, 0.317, 0}},
        {"p0_06.j2k", 0, "c1p0_06", {635, 403, 378, 0}, {11287, 6124, 3968, 0}},
        {"p0_07.j2k", 0, "c1p0_07", {0, 0, 0}, {0, 0, 0}},
        {"p0_08.j2k", 1, "c1p0_08", {0, 0, 0}, {0, 0, 0}},
        {"p0_09.j2k", 0, "c1p0_09", {0}, {0}},
        {"p0_10.j2k", 0, "c1p0_10", {0, 0, 0}, {0, 0, 0}},
        {"p0_11.j2k", 0, "c1p0_11", {0}, {0}},
        {"p0_12.j2k", 0, "c1p0_12", {0}, {0}},
        {"p0_13.j2k", 0, "c1p0_13", {0, 0, 0, 0}, {0, 0, 0, 0}},
        {"p0_14.j2k", 0, "c1p0_14", {0, 0, 0}, {0, 0, 0}},
        {"p0_15.j2k", 0, "c1p0_15", {0}, {0}},
        {"p0_16.j2k", 0, "c1p0_16", {0}, {0}},
        {"p1_01.j2k", 0, "c1p1_01", {0}, {0}},
        {"p1_02.j2k", 0, "c1p1_02", {5, 4, 6}, {0.765, 0.616, 1.051}},
        {"p1_03.j2k", 0, "c1p1_03", {2, 2, 1, 0}, {0.311, 0.280, 0.267, 0}},
        {"p1_04.j2k", 0, "c1p1_04", {624}, {3080}},
        {"p1_05.j2k", 0, "c1p1_05", {40, 40, 40}, {8.458, 9.716, 10.154}},
        {"p1_06.j2k", 0, "c1p1_06", {2, 2, 2}, {0.6, 0.6, 0.6}},
        {"p1_07.j2k", 0, "c1p1_07", {0, 0}, {0, 0}},
    };
    return c;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string spec = "oracle", data;
    Mode mode = Mode::Plain;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "--a" && i + 1 < argc) {
            spec = argv[++i];
        } else if (k == "--data" && i + 1 < argc) {
            data = argv[++i];
        } else if (k == "--mode" && i + 1 < argc) {
            if (!parse_mode(argv[++i], mode)) {
                std::fprintf(stderr, "nj2k_conformance: unknown mode\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "usage: nj2k_conformance --a SPEC --data DIR [--mode plain|viewer]\n");
            return 2;
        }
    }
    if (data.empty() || !read_file(join_path(data, "input/conformance/p0_01.j2k"))) {
        std::printf("nj2k_conformance: no conformance data under '%s' (run tools/fetch_testdata.py): skipped\n",
                    data.c_str());
        return 77;
    }
    std::string err;
    auto backend = make_backend(spec, &err);
    if (!backend) {
        std::fprintf(stderr, "nj2k_conformance: %s\n", err.c_str());
        return 2;
    }
    int pass = 0, fail = 0;
    for (const Case& t : cases()) {
        const auto bytes = read_file(join_path(data, std::string("input/conformance/") + t.input));
        if (!bytes) {
            std::printf("FAIL %s: input missing\n", t.input);
            ++fail;
            continue;
        }
        Request rq;
        rq.data = bytes->data();
        rq.size = bytes->size();
        rq.reduce = t.reduce;
        rq.mode = mode;
        rq.capture = true;
        const Outcome o = backend->decode(rq);
        if (!o.ok) {
            std::printf("FAIL %s r%d: not decoded (%s: %s)\n", t.input, t.reduce, o.stage.c_str(), o.last_error.c_str());
            ++fail;
            continue;
        }
        bool ok = true;
        for (std::size_t c = 0; c < t.peak.size(); ++c) {
            const std::string ref = join_path(data, std::string("baseline/conformance/") + t.reference + "_" +
                                                        std::to_string(c) + ".pgx");
            std::string why;
            const auto pgx = read_pgx(ref, &why);
            if (!pgx) {
                std::printf("FAIL %s c%zu: reference %s: %s\n", t.input, c, ref.c_str(), why.c_str());
                ok = false;
                continue;
            }
            if (c >= o.comps.size() || int(o.comps[c].w) != pgx->width || int(o.comps[c].h) != pgx->height) {
                std::printf("FAIL %s c%zu: decoded size differs from the reference %dx%d\n", t.input, c, pgx->width,
                            pgx->height);
                ok = false;
                continue;
            }
            long long peak = 0;
            double se = 0;
            const auto& plane = o.planes[c];
            for (std::size_t i = 0; i < plane.size(); ++i) {
                long long d = (long long)plane[i] - pgx->samples[i];
                d = d < 0 ? -d : d;
                peak = d > peak ? d : peak;
                se += double(d) * double(d);
            }
            const double mse = se / double(plane.size());
            const bool pass_c = double(peak) <= t.peak[c] && mse <= t.mse[c] + 1e-9;
            std::printf("%s %s c%zu: %ux%u peak %lld (limit %g) mse %.4f (limit %g)\n", pass_c ? "pass" : "FAIL",
                        t.input, c, o.comps[c].w, o.comps[c].h, peak, t.peak[c], mse, t.mse[c]);
            ok = ok && pass_c;
        }
        ok ? ++pass : ++fail;
    }
    std::printf("class 1 conformance, %s, %s mode: %d of %zu pass\n", backend->name().c_str(), mode_name(mode), pass,
                cases().size());
    return fail ? 1 : 0;
}
