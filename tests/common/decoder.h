// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The decoders under test, behind one interface: the oracle (OpenJPEG at the pinned source, linked
// statically), any library with OpenJPEG's ABI loaded at run time (the viewer's shipped
// openjp2.dll, a fix build), and neo-j2k through its OpenJPEG compatibility header. The request
// code is one source compiled once per API (replay.inl), so every backend is driven the same way.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nj2k::test {

enum class Mode {
    Plain,   // one codec, non-strict (partial codestreams decode), the reduce factor and layer limit
    Strict,  // the same in strict mode (OpenJPEG's default)
    Viewer,  // the viewer's request (llimagej2coj.cpp): header pass, then a second codec with the
             // reduce factor set before the header, non-strict, the viewer's size guard, its
             // geometry checks and its 8-bit bottom-up copy of up to four components
};
const char* mode_name(Mode m) noexcept;
bool parse_mode(const std::string& s, Mode& m) noexcept;

struct Request {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;  // the bytes the decoder sees: a whole file, or a prefix of it
    int reduce = 0;        // discard level
    int layers = 0;        // layers to decode, 0 for all (cp_layer); Plain and Strict only
    Mode mode = Mode::Plain;
    bool capture = false;  // keep the sample planes in the outcome
    // Compute the identity hashes and keep the decoder's messages. The bench's timed rounds clear
    // it, so they time the viewer's work only: the decode, its checks and its 8-bit copy, with
    // message handlers that ignore the messages, as the viewer's do.
    bool inspect = true;
    bool jp2 = false;  // a JP2 or JPH file, read by the JP2 codec (is_jp2_file)
    // Plain and Strict: the reduce factor set after the header (opj_set_decoded_resolution_factor),
    // not in the parameters before it. OpenJPEG then sizes the image differently at odd origins.
    bool reduce_after_header = false;
};

// Whether bytes start with the JP2 signature box.
bool is_jp2_file(const std::uint8_t* data, std::size_t size) noexcept;

// What a header read gives (opj_read_header's image and opj_get_cstr_info's information), as far as
// both libraries define them; the request's mode picks how the reduce factor reaches the codec:
// Plain and Strict through the parameters, Viewer by opj_set_decoded_resolution_factor before the
// header, non-strict, as the viewer's decode pass does.
struct HeaderFacts {
    struct Comp {
        std::uint32_t dx = 0, dy = 0, w = 0, h = 0, x0 = 0, y0 = 0, prec = 0, sgnd = 0, factor = 0, resno_decoded = 0,
                      alpha = 0;
    };
    struct Tccp {
        std::uint32_t csty = 0, numresolutions = 0, cblkw = 0, cblkh = 0, cblksty = 0, qmfbid = 0, qntsty = 0,
                      numgbits = 0;
        std::int32_t roishift = 0;
        std::vector<std::uint32_t> mant, expn;  // the bands OpenJPEG reports (none when 97 or more)
        std::vector<std::uint8_t> prc;          // the first numresolutions bytes of prcw, then of prch
    };
    bool ok = false;
    std::string stage;  // codec, header or info when !ok
    std::string last_error;
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, numcomps = 0;
    int color_space = 0;
    std::vector<Comp> comps;
    bool icc_buffer = false;  // icc_profile_buf set (OpenJPEG sets it with length 0 for CIELab)
    std::vector<std::uint8_t> icc;
    std::uint32_t tx0 = 0, ty0 = 0, tdx = 0, tdy = 0, tw = 0, th = 0, nbcomps = 0;
    std::uint32_t csty = 0, numlayers = 0, mct = 0;
    int prg = 0;
    std::vector<Tccp> tccp;

    std::uint64_t hash() const;
};
// The first difference between two header reads, "" when they agree (a refusal agrees with a
// refusal, wherever it happened).
std::string header_difference(const HeaderFacts& a, const HeaderFacts& b);

struct Component {
    std::uint32_t w = 0, h = 0, dx = 0, dy = 0, x0 = 0, y0 = 0, prec = 0, sgnd = 0, factor = 0, resno_decoded = 0;
};

struct Outcome {
    bool ok = false;
    std::string stage;  // where a failure happened: codec, header, guard, decode, geometry
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0, numcomps = 0;
    int color_space = 0;
    std::vector<Component> comps;
    std::vector<std::vector<std::int32_t>> planes;  // when captured
    // hash: geometry and every sample of every component; 0 when !ok.
    std::uint64_t hash = 0;
    // viewer mode: FNV-1a of the viewer's 8-bit raw image (the checksum of the earlier benches)
    std::uint64_t u8_hash = 0;
    std::uint32_t warnings = 0, errors = 0;
    std::string last_error;     // every error message, oldest first
    std::string warning_kinds;  // each kind of warning once
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual const std::string& name() const noexcept = 0;
    virtual Outcome decode(const Request& request) = 0;
    // The header read alone (opj_read_header and opj_get_cstr_info).
    virtual HeaderFacts header(const Request& request) = 0;
    // The viewer's upload encode of an 8-bit interleaved, bottom-up raw image (bench and encoder
    // tests); false when the backend cannot encode yet.
    virtual bool encode_upload(const std::uint8_t* raw, int width, int height, int comps, bool reversible,
                               std::vector<std::uint8_t>& out) = 0;
    // Heap calls since the last reset (allocations, reallocations, frees, bytes), when the backend
    // can count them (the counting oracle module, neo-j2k's allocator hook later); false otherwise.
    virtual bool alloc_counts(std::uint64_t out[4]) { (void)out; return false; }
    virtual void alloc_reset() {}
};

// "oracle", "neoj2k", or "lib:<path>[=<name>]" (an OpenJPEG-ABI shared library). Every backend
// decodes on the calling thread: making one clears OPJ_NUM_THREADS (single_thread_openjpeg).
std::unique_ptr<Backend> make_backend(const std::string& spec, std::string* error = nullptr);
std::unique_ptr<Backend> make_oracle_backend();
std::unique_ptr<Backend> make_neoj2k_backend();
std::unique_ptr<Backend> make_library_backend(const std::string& path, const std::string& name, std::string* error);

// Viewer behaviour shared by every backend (indra/llimage, llimagej2coj, release line).
namespace viewer {
constexpr std::uint64_t kMaxComponents = 8;      // MAX_IMAGE_COMPONENTS (llimage.h)
constexpr std::size_t kFirstRequestBytes = 600;  // lltexturefetch: the first request of a texture
// LLImageJ2C::calcDataSizeJ2C: the bytes the fetcher asks for at a discard level, from the image
// size LLImageJ2COJ::getMetadata stores: the tile grid's (tiles x tile size, see grid_side).
std::int64_t data_size_j2c(int w, int h, int discard);
// The side LLImageJ2COJ::getMetadata stores: tiles x tile size, in 32 bits, as a signed int.
int grid_side(std::uint64_t tiles, std::uint32_t tile_size) noexcept;
// LLJ2CDecodeGuard::componentsSupported: the component counts getMetadata takes (1 to 8).
bool components_supported(std::uint64_t components) noexcept;
// LLJ2CReduce::reduceFactor: never ask for a coarser level than the codestream has.
int reduce_factor(int requested_discard, int resolutions, int width, int height, int max_image_size = 4096);
// LLJ2CDecodeGuard::decodeAllowed with MAX_IMAGE_COMPONENTS (8).
bool decode_allowed(std::uint64_t width, std::uint64_t height, std::uint64_t components, unsigned reduce);
}  // namespace viewer

}  // namespace nj2k::test
