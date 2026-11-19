// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Fuzz target: decode, in the configurations the viewer uses and OpenJPEG's own OSS-Fuzz target
// never takes (non-strict partial decodes at every reduce level, the viewer's two-pass request),
// plus strict mode and layer limits; the cut length comes from the input (fuzz_common.h). The
// decoder under test is the oracle until neo-j2k's decoder lands (stage 1c), when this target
// decodes with neo-j2k and a differential target compares the two.
#include <cstddef>
#include <cstdint>

#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    nj2k::fuzz::fuzz_setup();
    static const auto oracle = nj2k::test::make_oracle_backend();
    const nj2k::fuzz::Input in = nj2k::fuzz::split_input(data, size);
    if (in.codestream.empty() || !nj2k::fuzz::oracle_may_decode(in.codestream, in.reduce)) {
        return 0;
    }
    nj2k::test::Request rq;
    rq.data = in.codestream.data();
    rq.size = in.codestream.size();
    rq.reduce = in.reduce;
    rq.layers = in.mode == nj2k::test::Mode::Viewer ? 0 : in.layers;
    rq.mode = in.mode;
    ++nj2k::fuzz::exercised();
    (void)oracle->decode(rq);
    return 0;
}
