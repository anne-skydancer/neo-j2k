// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The end of every variant file: the generated decoder body in the variant's namespace (STUDY_VNAME), on
// the MQ state type STUDY_MQC_T, and its entry point STUDY_RUN(Scratch&, const T1Block&).
namespace nj2k::detail::STUDY_VNAME {
using opj_mqc_state_t = MqState;
using opj_mqc_t = STUDY_MQC_T;
using opj_t1_t = mqs::T1V<STUDY_MQC_T>;
#include "t1_dec_body.inc"
}  // namespace nj2k::detail::STUDY_VNAME

bool STUDY_RUN(mqs::Scratch& s, const nj2k::detail::T1Block& b) noexcept
{
    static nj2k::detail::STUDY_VNAME::opj_t1_t t;
    static const nj2k::detail::Messages quiet;
    t.data = s.data;
    t.flags = s.flags;
    t.cblkdata = s.cblkdata;
    t.cblkdata_size = s.cblkdata_size;
    return nj2k::detail::STUDY_VNAME::decode_cblk(t, b, quiet);
}
