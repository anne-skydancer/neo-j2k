// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The decode of a codestream after its main header: the walk over the tile-parts (OpenJPEG 2.5.4's
// opj_j2k_read_tile_header, opj_j2k_read_sot, opj_j2k_read_sod, opj_j2k_decode_tile and
// opj_j2k_decode_tiles at the oracle's pin, as one state machine with the same states, checks and
// outcomes for whole and cut codestreams), each tile's decode (tile.h) and the copy of its samples
// to the output (opj_j2k_update_image_data): the planes of an nj2k_image_t, or the caller's own
// buffers, converted on the way.
#pragma once

#include <cstddef>
#include <cstdint>

#include <neoj2k/neoj2k.h>

namespace nj2k::detail {

struct Decoder;

// A component of the decoded image, as opj_image_comp_t carries it through the decode.
struct OutComp {
    std::uint32_t dx, dy, w, h, x0, y0, prec, sgnd, factor, resno_decoded;
    std::int32_t* data;  // the image plane (nj2k_image_t output), or null
    bool written;        // a tile wrote it (opj_j2k_are_all_used_components_decoded)
};

// Where the samples go.
struct OutSink {
    // nj2k_decode: planes of int32 from the C heap, handed to the image
    bool image_planes;
    // nj2k_decode_memory: the caller's planes
    const nj2k_output_t* output;
};

// The bytes of the codestream from the stream offset 'origin' on, and what the host said.
struct CodestreamBytes {
    const std::uint8_t* base;     // the byte at stream offset origin
    std::uint64_t origin;
    std::uint64_t actual_end;     // stream offset after the last byte available
    std::uint64_t claimed_end;    // the length the host gave for the stream (0: none)
    std::uint64_t start;          // stream offset just after the first SOT marker
    bool has_seek;                // the host gave a seek function (OpenJPEG's opj_stream_has_seek)
};

// Decodes every tile into comps (mh->numcomps entries, geometry set by the caller), through sink.
// truncated is set when the codestream ended before its end (the walk reached its end without EOC,
// or a tile-part's data was cut). Messages go to the decoder's handlers.
nj2k_status_t decode_codestream(Decoder& d, const CodestreamBytes& bytes, OutComp* comps, const OutSink& sink,
                                bool& truncated) noexcept;

}  // namespace nj2k::detail
