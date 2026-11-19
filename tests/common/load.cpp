// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include "load.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <thread>

#include "common.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>

#include <fstream>
#endif

namespace nj2k::test {

int logical_cpus() noexcept
{
    const unsigned n = std::thread::hardware_concurrency();
    return n ? int(n) : 1;
}

#if defined(_WIN32)
static std::uint64_t ft64(const FILETIME& f)
{
    return (std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}
#endif

bool LoadSampler::read(std::uint64_t& idle, std::uint64_t& total, std::uint64_t& own_ns) const
{
#if defined(_WIN32)
    FILETIME fi, fk, fu, c, e, k, u;
    if (!GetSystemTimes(&fi, &fk, &fu) || !GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        return false;
    }
    idle = ft64(fi);
    total = ft64(fk) + ft64(fu);  // kernel time includes idle time
    own_ns = (ft64(k) + ft64(u)) * 100;
    return true;
#else
    std::ifstream in("/proc/stat");
    std::string cpu;
    std::uint64_t v[10] = {};
    if (!(in >> cpu) || cpu != "cpu") {
        return false;
    }
    for (auto& x : v) {
        in >> x;
    }
    idle = v[3] + v[4];  // idle + iowait
    total = 0;
    for (int i = 0; i < 8; ++i) {
        total += v[i];  // user nice system idle iowait irq softirq steal (guest is inside user)
    }
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    own_ns = (std::uint64_t(ru.ru_utime.tv_sec) + std::uint64_t(ru.ru_stime.tv_sec)) * 1000000000ull +
             (std::uint64_t(ru.ru_utime.tv_usec) + std::uint64_t(ru.ru_stime.tv_usec)) * 1000ull;
    return true;
#endif
}

LoadSampler::LoadSampler()
{
    read(idle_, total_, own_ns_);
    wall_ns_ = now_ns();
}

LoadSample LoadSampler::sample()
{
    LoadSample s;
    std::uint64_t idle = 0, total = 0, own = 0;
    const std::uint64_t wall = now_ns();
    if (read(idle, total, own) && total > total_ && wall > wall_ns_) {
        const double dt = double(total - total_), di = double(idle - idle_);
        s.total = std::clamp(100.0 * (1.0 - di / dt), 0.0, 100.0);
        s.own = std::clamp(100.0 * double(own - own_ns_) / (double(wall - wall_ns_) * logical_cpus()), 0.0, 100.0);
    }
    idle_ = idle;
    total_ = total;
    own_ns_ = own;
    wall_ns_ = wall;
    return s;
}

static bool is_busy_name(std::string n)
{
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".exe") == 0) {
        n.resize(n.size() - 4);
    }
    static const char* const names[] = {"cl", "link", "lld", "lld-link", "ld", "ld.lld", "ld.gold", "cc1", "cc1plus",
                                        "clang", "clang++", "clang-cl", "gcc", "g++", "cmake", "ninja", "msbuild",
                                        "make", "rustc", "cargo", "htbench", "stageprof", "ojbench", "kernels",
                                        "nj2k_bench"};
    for (const char* x : names) {
        if (n == x) {
            return true;
        }
    }
    return n.rfind("vulkanstorm", 0) == 0 || n.rfind("firestorm", 0) == 0 || n.rfind("secondlife", 0) == 0;
}

std::vector<std::string> busy_processes()
{
    std::set<std::string> found;
#if defined(_WIN32)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return {};
    }
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    const DWORD self = GetCurrentProcessId();
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        if (e.th32ProcessID == self) {
            continue;
        }
        char name[MAX_PATH * 3];
        const int n = WideCharToMultiByte(CP_UTF8, 0, e.szExeFile, -1, name, int(sizeof(name)), nullptr, nullptr);
        if (n > 0 && is_busy_name(name)) {
            found.insert(name);
        }
    }
    CloseHandle(snap);
#else
    DIR* d = opendir("/proc");
    if (!d) {
        return {};
    }
    const std::string self = std::to_string(getpid());
    while (dirent* ent = readdir(d)) {
        const std::string pid = ent->d_name;
        if (pid.empty() || !std::isdigit(static_cast<unsigned char>(pid[0])) || pid == self) {
            continue;
        }
        std::ifstream comm("/proc/" + pid + "/comm");
        std::string name;
        if (std::getline(comm, name) && is_busy_name(name)) {
            found.insert(name);
        }
    }
    closedir(d);
#endif
    return {found.begin(), found.end()};
}

}  // namespace nj2k::test
