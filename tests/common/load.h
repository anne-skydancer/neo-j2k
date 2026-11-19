// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Machine load while a bench runs. The bench never waits for a quiet machine: it records the load
// it ran under and gates on interleaved ratios and counts, which load moves far less than absolute
// times (plan audit C3).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nj2k::test {

struct LoadSample {
    double total = 0;  // percent of all logical CPUs busy over the interval
    double own = 0;    // this process's share, percent of all logical CPUs
    double other() const noexcept { return total > own ? total - own : 0.0; }
};

class LoadSampler {
public:
    LoadSampler();
    LoadSample sample();  // since the previous sample (or construction)

private:
    std::uint64_t idle_ = 0, total_ = 0, own_ns_ = 0, wall_ns_ = 0;
    bool read(std::uint64_t& idle, std::uint64_t& total, std::uint64_t& own_ns) const;
};

// Names of running processes that load a machine during a bench: compilers, linkers, build tools,
// other benches, viewers. Empty when the platform gives no list.
std::vector<std::string> busy_processes();

int logical_cpus() noexcept;

}  // namespace nj2k::test
