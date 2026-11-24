#pragma once
// 行単位の並列処理
#include <algorithm>
#include <functional>
#include <thread>
#include <vector>

namespace rd {

inline unsigned threadCount() {
    static const unsigned n = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    return n;
}

// [0, n) を分割して fn(begin, end) を並列に呼ぶ
template <class Fn>
void parallelFor(int n, Fn&& fn, int minChunk = 16) {
    const int nt = static_cast<int>(std::min<unsigned>(threadCount(), std::max(1, n / minChunk)));
    if (nt <= 1) {
        fn(0, n);
        return;
    }
    std::vector<std::thread> ts;
    ts.reserve(nt);
    for (int t = 0; t < nt; ++t) {
        const int a = n * t / nt, b = n * (t + 1) / nt;
        ts.emplace_back([&fn, a, b] { fn(a, b); });
    }
    for (auto& t : ts) t.join();
}

}  // namespace rd
