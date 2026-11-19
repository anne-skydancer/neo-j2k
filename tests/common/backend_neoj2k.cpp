// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// neo-j2k as a backend, driven by the same request source as OpenJPEG through the compatibility
// header: this file compiling is the check that OpenJPEG code builds against neo-j2k unchanged.
// Since stage 1b the header reads; until the decoder lands (stage 1c) every decode fails at
// "decode".
#include <neoj2k/neoj2k_openjpeg.h>

#include "common.h"
#include "decoder.h"

#define REPLAY_NS nj2k_replay_neoj2k
#include "replay.inl"

namespace nj2k::test {

namespace {

class NeoBackend final : public Backend {
public:
    NeoBackend() : api_(REPLAY_NS::linked_api()) {}
    const std::string& name() const noexcept override { return name_; }
    Outcome decode(const Request& rq) override { return REPLAY_NS::replay(api_, rq); }
    HeaderFacts header(const Request& rq) override { return REPLAY_NS::replay_header(api_, rq); }
    bool encode_upload(const std::uint8_t* raw, int width, int height, int comps, bool reversible,
                       std::vector<std::uint8_t>& out) override
    {
        return REPLAY_NS::replay_upload(api_, raw, width, height, comps, reversible, out);
    }

private:
    std::string name_ = "neoj2k";
    REPLAY_NS::Api api_;
};

}  // namespace

std::unique_ptr<Backend> make_neoj2k_backend()
{
    return std::make_unique<NeoBackend>();
}

}  // namespace nj2k::test
