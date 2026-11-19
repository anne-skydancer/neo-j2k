// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// nj2k_diff: the differential, cut-point and reference runner.
//
//   compare  --a SPEC --b SPEC --files LIST [--mode M] [--reduce R] [--kinds K] [--reduce-policy P]
//            [--cut-max N] [--cut-from N] [--cut-step N] [--csv FILE] [--allow-diff]
//            Decodes every request with both backends and compares outcome (decoded or refused),
//            geometry and every sample. Kinds: full (whole file), budget (the viewer's byte budget at
//            the reduce level, when it cuts), first (600 bytes at reduce 5), cuts (every prefix of
//            files up to --cut-max bytes).
//   record   --a SPEC --files LIST --root DIR --out FILE [--modes plain,viewer] [--reduce R]
//            [--cut-max N] [--cut-min N] [--cut-steps 1,13] [--cut-modes M] [--no-whole]
//            Writes a reference table: whole files at each reduce level in each mode, and one digest
//            per file, reduce level, mode and step over its cut points (files of cut-min < size <=
//            cut-max bytes; the prefixes from --cut-from to the file size, every step bytes).
//   verify   --a SPEC --ref FILE --root DIR [--only FILE] [--cut-steps S] [--no-whole] [--no-cuts]
//            Decodes the lines of a reference table again and compares.
//   decode   --a SPEC --files LIST [--bytes N] [--reduce R] [--layers L] [--mode M]
//            Describes single requests.
//   layers   --a SPEC --files LIST [--reduce R] [--ref KNOWN]
//            For one-tile LRCP streams with SOP or PLT: a cut where layer k+1 starts must decode
//            exactly as the whole file limited to k layers (cp_layer), an oracle independent of the
//            truncation code (plan audit B5). KNOWN lists the backend's known defects.
//   resources --a SPEC --ref KNOWN --root DIR
//            Known resource defects of a backend that counts its heap calls (the counting oracle
//            module, lib:<nj2k_oracle_counting>): each "<file> <mode> <reduce> <MiB>" line is a
//            whole-file request for which the backend asks the heap for at least that much. A line
//            that asks for less fails: its defect is gone, and the line goes.
//
// SPEC: oracle | neoj2k | lib:<path>[=<name>]. LIST: a codestream, a folder, or a list file.
// Modes: plain (one codec, non-strict), strict, viewer (the viewer's two-pass request).
// Exit status: 0 when everything matched, 1 on a difference, 2 on a usage or input error, and 2
// when a check matched nothing (verify with no reference line left, layers with no locatable file,
// resources with no line).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "common.h"
#include "decoder.h"
#include "j2k_scan.h"
#include "load.h"

using namespace nj2k::test;

namespace {

struct Args {
    std::string cmd, a, b, files, root, out, ref, csv;
    std::vector<int> reduce{0, 1, 2, 3, 4, 5};
    std::vector<std::string> kinds{"full"};
    std::vector<Mode> modes{Mode::Plain};
    std::string reduce_policy = "all";
    std::size_t cut_max = 4096, cut_min = 0, cut_from = 1, cut_step = 1, bytes = 0;
    std::vector<int> cut_steps{1};
    std::vector<Mode> cut_modes;
    std::string only;
    int layers = 0;
    bool allow_diff = false, whole = true, cuts = true, cut_steps_given = false;
};

[[noreturn]] void usage(const char* msg)
{
    std::fprintf(stderr, "nj2k_diff: %s\nsee the comment at the top of tests/tools/nj2k_diff.cpp\n", msg);
    std::exit(2);
}

Args parse(int argc, char** argv)
{
    if (argc < 2) {
        usage("no command");
    }
    Args a;
    a.cmd = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage(("missing value after " + k).c_str());
            }
            return argv[++i];
        };
        if (k == "--a") a.a = next();
        else if (k == "--b") a.b = next();
        else if (k == "--files") a.files = next();
        else if (k == "--root") a.root = next();
        else if (k == "--out") a.out = next();
        else if (k == "--ref") a.ref = next();
        else if (k == "--csv") a.csv = next();
        else if (k == "--reduce") a.reduce = parse_int_list(next());
        else if (k == "--kinds") a.kinds = split(next(), ',');
        else if (k == "--reduce-policy") a.reduce_policy = next();
        else if (k == "--cut-max") a.cut_max = std::stoul(next());
        else if (k == "--cut-from") a.cut_from = std::stoul(next());
        else if (k == "--cut-step") a.cut_step = std::max<std::size_t>(1, std::stoul(next()));
        else if (k == "--allow-diff") a.allow_diff = true;
        else if (k == "--cut-min") a.cut_min = std::stoul(next());
        else if (k == "--cut-steps") {
            a.cut_steps = parse_int_list(next());
            a.cut_steps_given = true;
        }
        else if (k == "--only") a.only = next();
        else if (k == "--bytes") a.bytes = std::stoul(next());
        else if (k == "--layers") a.layers = std::stoi(next());
        else if (k == "--no-whole") a.whole = false;
        else if (k == "--no-cuts") a.cuts = false;
        else if (k == "--cut-modes") {
            for (const auto& m : split(next(), ',')) {
                Mode mode;
                if (!parse_mode(m, mode)) {
                    usage(("unknown mode " + m).c_str());
                }
                a.cut_modes.push_back(mode);
            }
        }
        else if (k == "--mode" || k == "--modes") {
            a.modes.clear();
            for (const auto& m : split(next(), ',')) {
                Mode mode;
                if (!parse_mode(m, mode)) {
                    usage(("unknown mode " + m).c_str());
                }
                a.modes.push_back(mode);
            }
        } else {
            usage(("unknown option " + k).c_str());
        }
    }
    return a;
}

std::unique_ptr<Backend> backend(const std::string& spec)
{
    std::string err;
    auto b = make_backend(spec, &err);
    if (!b) {
        usage(err.c_str());
    }
    return b;
}

struct Input {
    std::string path, name;
    Bytes data;
    int resolutions = 0;
    int grid_w = 0, grid_h = 0;  // the size the viewer stores and budgets with: the tile grid's
};

std::vector<Input> load_inputs(const std::string& files, const std::string& root)
{
    std::vector<Input> out;
    for (const auto& path : expand_inputs(files)) {
        auto data = read_file(path);
        if (!data) {
            std::fprintf(stderr, "nj2k_diff: cannot read %s\n", path.c_str());
            std::exit(2);
        }
        Input in;
        in.path = path;
        in.name = root.empty() ? base_name(path) : relative_to(path, root);
        in.data = std::move(*data);
        if (const auto mh = parse_main_header(in.data)) {
            in.resolutions = mh->min_resolutions;
            in.grid_w = viewer::grid_side(mh->tiles_x(), mh->xtsiz);
            in.grid_h = viewer::grid_side(mh->tiles_y(), mh->ytsiz);
        }
        out.push_back(std::move(in));
    }
    if (out.empty()) {
        usage(("no codestreams in " + files).c_str());
    }
    return out;
}

// The input lengths one kind asks for at one reduce level.
std::vector<std::size_t> lengths(const Input& in, const std::string& kind, int r, const Args& a)
{
    const std::size_t size = in.data.size();
    if (kind == "full") {
        return {size};
    }
    if (kind == "budget") {
        if (r == 0) {
            return {};
        }
        const auto b = std::size_t(viewer::data_size_j2c(in.grid_w, in.grid_h, r));
        return b < size ? std::vector<std::size_t>{b} : std::vector<std::size_t>{};
    }
    if (kind == "first") {
        return r == 5 && size > viewer::kFirstRequestBytes ? std::vector<std::size_t>{viewer::kFirstRequestBytes}
                                                           : std::vector<std::size_t>{};
    }
    if (kind == "cuts") {
        std::vector<std::size_t> v;
        if (size <= a.cut_max) {
            for (std::size_t n = a.cut_from; n < size; n += a.cut_step) {
                v.push_back(n);
            }
        }
        return v;
    }
    usage(("unknown kind " + kind).c_str());
}

bool same(const Outcome& x, const Outcome& y, Mode m)
{
    if (x.ok != y.ok) {
        return false;
    }
    if (!x.ok) {
        return true;  // both refused
    }
    return x.hash == y.hash && (m != Mode::Viewer || x.u8_hash == y.u8_hash);
}

// first differing sample, for the report
std::string describe(const Outcome& x, const Outcome& y)
{
    std::ostringstream s;
    if (x.ok != y.ok) {
        s << "a " << (x.ok ? "decoded" : "refused at " + x.stage) << ", b " << (y.ok ? "decoded" : "refused at " + y.stage);
        if (!x.ok && !x.last_error.empty()) s << " (a: " << x.last_error << ")";
        if (!y.ok && !y.last_error.empty()) s << " (b: " << y.last_error << ")";
        return s.str();
    }
    if (x.comps.size() != y.comps.size()) {
        s << "components " << x.comps.size() << " vs " << y.comps.size();
        return s.str();
    }
    for (std::size_t c = 0; c < x.comps.size(); ++c) {
        if (x.comps[c].w != y.comps[c].w || x.comps[c].h != y.comps[c].h) {
            s << "component " << c << " size " << x.comps[c].w << "x" << x.comps[c].h << " vs " << y.comps[c].w << "x"
              << y.comps[c].h;
            return s.str();
        }
        if (c < x.planes.size() && c < y.planes.size()) {
            std::size_t diffs = 0, first = 0;
            for (std::size_t i = 0; i < x.planes[c].size() && i < y.planes[c].size(); ++i) {
                if (x.planes[c][i] != y.planes[c][i]) {
                    if (!diffs++) {
                        first = i;
                    }
                }
            }
            if (diffs) {
                s << "component " << c << ": " << diffs << " samples differ, first at " << first << " ("
                  << x.planes[c][first] << " vs " << y.planes[c][first] << ")";
                return s.str();
            }
        }
    }
    s << "geometry or 8-bit copy differs";
    return s.str();
}

bool reduce_wanted(const Input& in, int r, const Args& a)
{
    return a.reduce_policy == "all" || r < std::max(in.resolutions, 1);
}

int cmd_compare(const Args& a)
{
    if (a.a.empty() || a.b.empty() || a.files.empty()) {
        usage("compare needs --a, --b and --files");
    }
    auto A = backend(a.a), B = backend(a.b);
    const auto inputs = load_inputs(a.files, a.root);
    std::FILE* csv = a.csv.empty() ? nullptr : std::fopen(a.csv.c_str(), "w");
    if (csv) {
        std::fprintf(csv, "file,mode,kind,bytes,reduce,a_ok,a_stage,a_hash,b_ok,b_stage,b_hash,same\n");
    }
    struct Tally { std::size_t decodes = 0, same = 0, both_ok = 0, both_refused = 0, stage_differs = 0; };
    std::map<std::string, Tally> tally;
    std::size_t differences = 0;
    for (const auto& in : inputs) {
        for (Mode mode : a.modes) {
            for (int r : a.reduce) {
                if (!reduce_wanted(in, r, a)) {
                    continue;
                }
                for (const auto& kind : a.kinds) {
                    for (std::size_t n : lengths(in, kind, r, a)) {
                        Request rq;
                        rq.data = in.data.data();
                        rq.size = n;
                        rq.reduce = r;
                        rq.mode = mode;
                        rq.capture = true;
                        const Outcome x = A->decode(rq), y = B->decode(rq);
                        const bool eq = same(x, y, mode);
                        auto& t = tally[std::string(mode_name(mode)) + " " + kind];
                        ++t.decodes;
                        t.same += eq;
                        t.both_ok += x.ok && y.ok;
                        t.both_refused += !x.ok && !y.ok;
                        t.stage_differs += !x.ok && !y.ok && x.stage != y.stage;
                        if (!eq) {
                            ++differences;
                            if (differences <= 50) {
                                std::printf("DIFF %s %s %s bytes=%zu r=%d: %s\n", in.name.c_str(), mode_name(mode),
                                            kind.c_str(), n, r, describe(x, y).c_str());
                            }
                        }
                        if (csv) {
                            std::fprintf(csv, "%s,%s,%s,%zu,%d,%d,%s,%s,%d,%s,%s,%d\n", in.name.c_str(), mode_name(mode),
                                         kind.c_str(), n, r, int(x.ok), x.stage.c_str(), hex64(x.hash).c_str(), int(y.ok),
                                         y.stage.c_str(), hex64(y.hash).c_str(), int(eq));
                        }
                    }
                }
            }
        }
    }
    if (csv) {
        std::fclose(csv);
    }
    std::printf("compare %s vs %s, %zu files\n", A->name().c_str(), B->name().c_str(), inputs.size());
    std::printf("%-16s %9s %9s %9s %12s %14s\n", "mode kind", "decodes", "same", "both ok", "both refused",
                "stage differs");
    std::size_t total = 0, total_same = 0;
    for (const auto& [k, t] : tally) {
        std::printf("%-16s %9zu %9zu %9zu %12zu %14zu\n", k.c_str(), t.decodes, t.same, t.both_ok, t.both_refused,
                    t.stage_differs);
        total += t.decodes;
        total_same += t.same;
    }
    std::printf("total: %zu decodes, %zu identical, %zu different\n", total, total_same, differences);
    return differences && !a.allow_diff ? 1 : 0;
}

// ---- reference tables ---------------------------------------------------------------------------

struct CutDigest {
    std::uint64_t digest = 0;
    std::size_t count = 0, decoded = 0;
};

CutDigest cut_digest(Backend& b, const Input& in, Mode mode, int r, std::size_t from, std::size_t step)
{
    Fnv64 h;
    CutDigest d;
    for (std::size_t n = from; n < in.data.size(); n += step) {
        Request rq;
        rq.data = in.data.data();
        rq.size = n;
        rq.reduce = r;
        rq.mode = mode;
        const Outcome o = b.decode(rq);
        h.u64(n);
        h.u32(o.ok ? 1 : 0);
        h.u64(o.ok ? (mode == Mode::Viewer ? o.hash ^ o.u8_hash : o.hash) : 0);
        ++d.count;
        d.decoded += o.ok;
    }
    d.digest = h.value();
    return d;
}

int cmd_record(const Args& a)
{
    if (a.a.empty() || a.files.empty() || a.out.empty() || a.root.empty()) {
        usage("record needs --a, --files, --root and --out");
    }
    auto A = backend(a.a);
    const auto inputs = load_inputs(a.files, a.root);
    std::ofstream out(a.out, std::ios::binary);
    out << "# neo-j2k reference table, written by nj2k_diff record with backend " << A->name() << "\n";
    out << "# whole <file> <mode> <reduce> <bytes> <ok> <hash>\n";
    out << "# cuts <file> <mode> <reduce> <from> <step> <count> <decoded> <digest>\n";
    const std::vector<Mode>& cut_modes = a.cut_modes.empty() ? a.modes : a.cut_modes;
    for (const auto& in : inputs) {
        for (Mode mode : a.modes) {
            for (int r : a.reduce) {
                if (!a.whole) {
                    break;
                }
                Request rq;
                rq.data = in.data.data();
                rq.size = in.data.size();
                rq.reduce = r;
                rq.mode = mode;
                const Outcome o = A->decode(rq);
                out << "whole\t" << in.name << '\t' << mode_name(mode) << '\t' << r << '\t' << in.data.size() << '\t'
                    << int(o.ok) << '\t' << hex64(o.ok ? (mode == Mode::Viewer ? o.hash ^ o.u8_hash : o.hash) : 0)
                    << '\n';
            }
        }
        if (in.data.size() > a.cut_min && in.data.size() <= a.cut_max) {
            for (Mode mode : cut_modes) {
                for (int r : a.reduce) {
                    for (int step : a.cut_steps) {
                        const CutDigest d = cut_digest(*A, in, mode, r, a.cut_from, std::size_t(step));
                        out << "cuts\t" << in.name << '\t' << mode_name(mode) << '\t' << r << '\t' << a.cut_from << '\t'
                            << step << '\t' << d.count << '\t' << d.decoded << '\t' << hex64(d.digest) << '\n';
                    }
                }
            }
        }
        std::fprintf(stderr, ".");
    }
    std::fprintf(stderr, "\nrecorded %zu files to %s\n", inputs.size(), a.out.c_str());
    return 0;
}

int cmd_verify(const Args& a)
{
    if (a.a.empty() || a.ref.empty() || a.root.empty()) {
        usage("verify needs --a, --ref and --root");
    }
    auto A = backend(a.a);
    std::ifstream in(a.ref);
    if (!in) {
        usage(("cannot read " + a.ref).c_str());
    }
    std::map<std::string, Input> cache;
    std::string line;
    std::size_t lines = 0, whole = 0, cuts = 0, bad = 0, cut_decodes = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto f = split(line, '\t');
        if (f.size() < 7) {
            usage(("bad reference line: " + line).c_str());
        }
        if (!a.only.empty() && f[1] != a.only) {
            continue;
        }
        if ((f[0] == "whole" && !a.whole) || (f[0] == "cuts" && !a.cuts)) {
            continue;
        }
        if (f[0] == "cuts" && f.size() >= 9 && a.cut_steps_given &&
                std::find(a.cut_steps.begin(), a.cut_steps.end(), std::stoi(f[5])) == a.cut_steps.end()) {
            continue;
        }
        ++lines;
        auto it = cache.find(f[1]);
        if (it == cache.end()) {
            Input inp;
            inp.path = join_path(a.root, f[1]);
            inp.name = f[1];
            auto data = read_file(inp.path);
            if (!data) {
                std::printf("MISSING %s\n", inp.path.c_str());
                ++bad;
                continue;
            }
            inp.data = std::move(*data);
            it = cache.emplace(f[1], std::move(inp)).first;
        }
        const Input& inp = it->second;
        Mode mode;
        if (!parse_mode(f[2], mode)) {
            usage(("bad mode in " + line).c_str());
        }
        const int r = std::stoi(f[3]);
        if (f[0] == "whole") {
            ++whole;
            Request rq;
            rq.data = inp.data.data();
            rq.size = inp.data.size();
            rq.reduce = r;
            rq.mode = mode;
            const Outcome o = A->decode(rq);
            const std::uint64_t got = o.ok ? (mode == Mode::Viewer ? o.hash ^ o.u8_hash : o.hash) : 0;
            const bool ok_ref = f[5] == "1";
            const auto want = parse_hex64(f[6]);
            if (std::stoul(f[4]) != inp.data.size() || o.ok != ok_ref || !want || got != *want) {
                ++bad;
                std::printf("MISMATCH whole %s %s r%d: reference ok=%s hash=%s, now ok=%d hash=%s%s%s\n", f[1].c_str(),
                            f[2].c_str(), r, f[5].c_str(), f[6].c_str(), int(o.ok), hex64(got).c_str(),
                            o.ok ? "" : " stage=", o.ok ? "" : o.stage.c_str());
            }
        } else if (f[0] == "cuts" && f.size() >= 9) {
            ++cuts;
            const CutDigest d = cut_digest(*A, inp, mode, r, std::stoul(f[4]), std::stoul(f[5]));
            cut_decodes += d.count;
            const auto want = parse_hex64(f[8]);
            if (!want || d.digest != *want || d.count != std::stoul(f[6]) || d.decoded != std::stoul(f[7])) {
                ++bad;
                std::printf("MISMATCH cuts %s %s r%d: reference %s decoded %s/%s, now %s decoded %zu/%zu\n", f[1].c_str(),
                            f[2].c_str(), r, f[8].c_str(), f[7].c_str(), f[6].c_str(), hex64(d.digest).c_str(), d.decoded,
                            d.count);
            }
        } else {
            usage(("bad reference line: " + line).c_str());
        }
    }
    std::printf("verify %s against %s: %zu lines (%zu whole-file decodes, %zu cut digests over %zu decodes), %zu mismatches\n",
                A->name().c_str(), a.ref.c_str(), lines, whole, cuts, cut_decodes, bad);
    if (lines == 0) {
        // a check that checks nothing must not pass: a renamed --only file, a --cut-steps value the
        // table does not hold, or an empty table
        std::fprintf(stderr, "nj2k_diff: no reference line of %s matched the options\n", a.ref.c_str());
        return 2;
    }
    return bad ? 1 : 0;
}

// One request, described: for looking at a single case.
int cmd_decode(const Args& a)
{
    if (a.a.empty() || a.files.empty()) {
        usage("decode needs --a and --files (and --bytes, --reduce, --layers, --mode)");
    }
    auto A = backend(a.a);
    const auto inputs = load_inputs(a.files, a.root);
    for (const auto& in : inputs) {
        for (Mode mode : a.modes) {
            for (int r : a.reduce) {
                Request rq;
                rq.data = in.data.data();
                rq.size = a.bytes ? std::min(a.bytes, in.data.size()) : in.data.size();
                rq.reduce = r;
                rq.layers = a.layers;
                rq.mode = mode;
                const Outcome o = A->decode(rq);
                std::printf("%s %s bytes=%zu/%zu r=%d layers=%d: %s", in.name.c_str(), mode_name(mode), rq.size,
                            in.data.size(), r, a.layers, o.ok ? "decoded" : ("refused at " + o.stage).c_str());
                if (o.ok) {
                    std::printf(" %ux%u x%u hash %s", o.comps[0].w, o.comps[0].h, o.numcomps, hex64(o.hash).c_str());
                }
                std::printf(", %u warnings, %u errors%s%s; process peak commit %.0f MB\n", o.warnings, o.errors,
                            o.last_error.empty() ? "" : ": ", o.last_error.c_str(),
                            double(peak_commit_bytes()) / (1024.0 * 1024.0));
                if (!o.warning_kinds.empty()) {
                    std::printf("    warnings: %s\n", o.warning_kinds.c_str());
                }
            }
        }
    }
    return 0;
}

int cmd_layers(const Args& a)
{
    if (a.a.empty() || a.files.empty()) {
        usage("layers needs --a and --files");
    }
    auto A = backend(a.a);
    const auto inputs = load_inputs(a.files, a.root);
    // Known defects of the backend: "<file> <reduce|*> <layers kept|*>" lines. A known case that
    // matches again is reported too, so the list cannot go stale.
    std::vector<std::vector<std::string>> known;
    if (!a.ref.empty()) {
        std::ifstream kf(a.ref);
        if (!kf) {
            usage(("cannot read " + a.ref).c_str());
        }
        std::string line;
        while (std::getline(kf, line)) {
            const std::string t = trim(line);
            if (t.empty() || t[0] == '#') {
                continue;
            }
            std::vector<std::string> f;
            for (const auto& part : split(t, ' ')) {
                if (!part.empty()) {
                    f.push_back(part);
                }
            }
            if (f.size() != 3) {
                usage(("bad known-defect line: " + t).c_str());
            }
            known.push_back(f);
        }
    }
    auto is_known = [&](const std::string& file, int r, std::size_t kept) {
        for (const auto& f : known) {
            if (f[0] == file && (f[1] == "*" || f[1] == std::to_string(r)) &&
                    (f[2] == "*" || f[2] == std::to_string(kept))) {
                return true;
            }
        }
        return false;
    };
    std::size_t files = 0, checks = 0, bad = 0, known_seen = 0, stale = 0;
    for (const auto& in : inputs) {
        const auto bounds = layer_boundaries(in.data);
        if (!bounds) {
            continue;
        }
        ++files;
        for (int r : a.reduce) {
            if (r >= std::max(in.resolutions, 1)) {
                continue;
            }
            for (std::size_t k = 0; k < bounds->size(); ++k) {
                Request cut;
                cut.data = in.data.data();
                cut.size = (*bounds)[k];
                cut.reduce = r;
                cut.capture = true;
                Request limited = cut;
                limited.size = in.data.size();
                limited.layers = int(k + 1);
                const Outcome x = A->decode(cut), y = A->decode(limited);
                ++checks;
                const bool match = x.ok && y.ok && x.hash == y.hash;
                const bool expected_defect = is_known(in.name, r, k + 1);
                if (!match && expected_defect) {
                    ++known_seen;
                } else if (!match) {
                    ++bad;
                    std::printf("LAYER MISMATCH %s r%d: the cut at %zu keeps the first %zu layers, unlike the whole file "
                                "limited to them: %s\n",
                                in.name.c_str(), r, (*bounds)[k], k + 1, describe(x, y).c_str());
                } else if (expected_defect) {
                    ++stale;
                    std::printf("KNOWN DEFECT GONE %s r%d keeping %zu layers: matches now; remove it from %s\n",
                                in.name.c_str(), r, k + 1, a.ref.c_str());
                }
            }
        }
    }
    std::printf("layers %s: %zu files with locatable layers, %zu checks, %zu mismatches, %zu known defects seen, "
                "%zu known defects gone\n",
                A->name().c_str(), files, checks, bad, known_seen, stale);
    return bad || stale ? 1 : (files ? 0 : 2);
}

int cmd_resources(const Args& a)
{
    if (a.a.empty() || a.ref.empty() || a.root.empty()) {
        usage("resources needs --a (a counting backend), --ref and --root");
    }
    auto A = backend(a.a);
    std::uint64_t probe[4];
    if (!A->alloc_counts(probe)) {
        usage((A->name() + " does not count its heap calls (lib:<nj2k_oracle_counting>)").c_str());
    }
    std::ifstream kf(a.ref);
    if (!kf) {
        usage(("cannot read " + a.ref).c_str());
    }
    std::size_t lines = 0, gone = 0;
    std::string line;
    while (std::getline(kf, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        std::vector<std::string> f;
        for (const auto& part : split(t, ' ')) {
            if (!part.empty()) {
                f.push_back(part);
            }
        }
        Mode mode;
        if (f.size() != 4 || !parse_mode(f[1], mode)) {
            usage(("bad known-defect line: " + t).c_str());
        }
        const std::string path = join_path(a.root, f[0]);
        const auto data = read_file(path);
        if (!data) {
            usage(("cannot read " + path).c_str());
        }
        ++lines;
        Request rq;
        rq.data = data->data();
        rq.size = data->size();
        rq.reduce = std::stoi(f[2]);
        rq.mode = mode;
        A->alloc_reset();
        const Outcome o = A->decode(rq);
        std::uint64_t c[4];
        A->alloc_counts(c);
        const double mib = double(c[3]) / (1024.0 * 1024.0);
        std::printf("%s %s r%s: %s; asked the heap for %.0f MiB in %llu calls (the defect: at least %s MiB)\n",
                    f[0].c_str(), f[1].c_str(), f[2].c_str(), o.ok ? "decoded" : ("refused at " + o.stage).c_str(), mib,
                    static_cast<unsigned long long>(c[0] + c[1]), f[3].c_str());
        if (mib < std::stod(f[3])) {
            ++gone;
            std::printf("KNOWN DEFECT GONE %s %s r%s: below %s MiB now; remove it from %s\n", f[0].c_str(), f[1].c_str(),
                        f[2].c_str(), f[3].c_str(), a.ref.c_str());
        }
    }
    std::printf("resources %s: %zu known resource defects, %zu gone\n", A->name().c_str(), lines, gone);
    return gone ? 1 : (lines ? 0 : 2);
}

}  // namespace

int main(int argc, char** argv)
{
    const Args a = parse(argc, argv);
    if (a.cmd == "compare") return cmd_compare(a);
    if (a.cmd == "record") return cmd_record(a);
    if (a.cmd == "verify") return cmd_verify(a);
    if (a.cmd == "layers") return cmd_layers(a);
    if (a.cmd == "decode") return cmd_decode(a);
    if (a.cmd == "resources") return cmd_resources(a);
    usage(("unknown command " + a.cmd).c_str());
}
