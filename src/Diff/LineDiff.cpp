#include "LineDiff.h"
#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_map>

namespace npp {

std::vector<std::string> SplitLines(const std::string& s)
{
    std::vector<std::string> out;
    size_t i = 0, start = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '\r' || c == '\n') {
            out.emplace_back(s.data() + start, i - start);
            if (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ++i;
            ++i;
            start = i;
        } else {
            ++i;
        }
    }
    out.emplace_back(s.data() + start, s.size() - start);
    return out;
}

namespace {

std::string Normalize(const std::string& line, const LineDiffOptions& opt)
{
    std::string out;
    out.reserve(line.size());
    for (char c : line) {
        if (opt.ignoreWhitespace && (c == ' ' || c == '\t')) continue;
        if (opt.ignoreCase && c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        out.push_back(c);
    }
    return out;
}

}  // namespace

std::vector<DiffEntry> ComputeLineDiff(const std::vector<std::string>& a,
                                       const std::vector<std::string>& b,
                                       const LineDiffOptions& opt)
{
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());

    std::vector<std::string> na(n), nb(m);
    for (int i = 0; i < n; ++i) na[i] = Normalize(a[i], opt);
    for (int j = 0; j < m; ++j) nb[j] = Normalize(b[j], opt);

    // Trim the common prefix/suffix first — the typical compare is "two big
    // files with a small changed middle", which shrinks the quadratic LCS
    // from whole-file size down to the changed region.
    int pre = 0;
    while (pre < n && pre < m && na[pre] == nb[pre]) ++pre;
    int endA = n, endB = m;
    while (endA > pre && endB > pre && na[endA - 1] == nb[endB - 1]) {
        --endA; --endB;
    }
    const int nn = endA - pre;   // middle (changed-region) sizes
    const int mm = endB - pre;

    std::vector<DiffEntry> raw;
    raw.reserve(static_cast<size_t>(n + m));
    for (int i = 0; i < pre; ++i) raw.push_back({DiffOp::Equal, i, i});

    // Intern the middle lines so the DP compares ints, not strings. The
    // string_views key into na/nb, which outlive this scope unchanged.
    std::unordered_map<std::string_view, int> internMap;
    internMap.reserve(static_cast<size_t>(nn + mm));
    auto intern = [&](const std::string& s) {
        auto [it, inserted] = internMap.try_emplace(
            std::string_view(s), static_cast<int>(internMap.size()));
        (void)inserted;
        return it->second;
    };
    std::vector<int> ia(nn), ib(mm);
    for (int i = 0; i < nn; ++i) ia[i] = intern(na[pre + i]);
    for (int j = 0; j < mm; ++j) ib[j] = intern(nb[pre + j]);

    // LCS DP on the trimmed middle — a flat (nn+1)×(mm+1) int table, capped
    // so pathological inputs can't ask for gigabytes (pre-trim 20k×20k lines
    // was a ~1.6 GB allocation). Past the cap, fall back to pairing the
    // middle off line-by-line: the collapse below turns it into aligned
    // Change rows, so the panes stay usable — every middle line just shows
    // as changed.
    constexpr size_t kMaxDpCells = 16u * 1024 * 1024;   // 64 MB of int
    const size_t stride = static_cast<size_t>(mm) + 1;
    const size_t cells  = (static_cast<size_t>(nn) + 1) * stride;
    if (nn > 0 && mm > 0 && cells <= kMaxDpCells) {
        std::vector<int> dp(cells, 0);
        for (int i = nn - 1; i >= 0; --i) {
            for (int j = mm - 1; j >= 0; --j) {
                dp[i * stride + j] = (ia[i] == ib[j])
                    ? dp[(i + 1) * stride + (j + 1)] + 1
                    : std::max(dp[(i + 1) * stride + j],
                               dp[i * stride + (j + 1)]);
            }
        }
        int i = 0, j = 0;
        while (i < nn && j < mm) {
            if (ia[i] == ib[j]) {
                raw.push_back({DiffOp::Equal, pre + i, pre + j});
                ++i; ++j;
            } else if (dp[(i + 1) * stride + j] >= dp[i * stride + (j + 1)]) {
                raw.push_back({DiffOp::Del, pre + i, -1});
                ++i;
            } else {
                raw.push_back({DiffOp::Add, -1, pre + j});
                ++j;
            }
        }
        while (i < nn) raw.push_back({DiffOp::Del, pre + i++, -1});
        while (j < mm) raw.push_back({DiffOp::Add, -1, pre + j++});
    } else {
        for (int i = 0; i < nn; ++i) raw.push_back({DiffOp::Del, pre + i, -1});
        for (int j = 0; j < mm; ++j) raw.push_back({DiffOp::Add, -1, pre + j});
    }

    for (int i = endA, j = endB; i < n; ++i, ++j)
        raw.push_back({DiffOp::Equal, i, j});

    // Collapse runs: pair Del+Add into Change rows so left/right stay aligned.
    std::vector<DiffEntry> out;
    out.reserve(raw.size());
    size_t k = 0;
    while (k < raw.size()) {
        if (raw[k].op == DiffOp::Del) {
            size_t delStart = k, delEnd = k;
            while (delEnd < raw.size() && raw[delEnd].op == DiffOp::Del) ++delEnd;
            size_t addStart = delEnd, addEnd = delEnd;
            while (addEnd < raw.size() && raw[addEnd].op == DiffOp::Add) ++addEnd;
            size_t paired = std::min(delEnd - delStart, addEnd - addStart);
            for (size_t p = 0; p < paired; ++p) {
                out.push_back({DiffOp::Change,
                               raw[delStart + p].leftLine,
                               raw[addStart + p].rightLine});
            }
            for (size_t p = paired; p < delEnd - delStart; ++p)
                out.push_back(raw[delStart + p]);
            for (size_t p = paired; p < addEnd - addStart; ++p)
                out.push_back(raw[addStart + p]);
            k = addEnd;
        } else if (raw[k].op == DiffOp::Add) {
            // Standalone add (no preceding del run).
            out.push_back(raw[k++]);
        } else {
            out.push_back(raw[k++]);
        }
    }
    return out;
}

} // namespace npp
