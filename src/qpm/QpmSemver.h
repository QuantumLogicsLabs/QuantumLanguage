#pragma once
// Semantic-version parsing and npm-style range satisfaction (^, ~, x-ranges,
// comparators, hyphen ranges, ||-alternation). Covers the overwhelming
// majority of real-world package.json range strings; not a byte-for-byte
// clone of npm's `node-semver` for every corner case.

#include <string>
#include <vector>

namespace qpm
{

    struct SemVer
    {
        int major = 0, minor = 0, patch = 0;
        std::vector<std::string> prerelease;

        bool hasPrerelease() const { return !prerelease.empty(); }
        std::string toString() const;

        // Strict parse of a full "X.Y.Z[-pre][+build]" version string.
        static bool tryParse(const std::string &s, SemVer &out);
    };

    // <0 if a<b, 0 if equal, >0 if a>b (release precedence; prerelease < release).
    int compareSemVer(const SemVer &a, const SemVer &b);

    class SemVerRange
    {
    public:
        explicit SemVerRange(const std::string &rangeStr);
        bool satisfies(const SemVer &v) const;
        bool isAny() const { return isAny_; }

    private:
        // Comparator: {op, version} where op is one of ">=", "<=", ">", "<", "=".
        using Comparator = std::pair<std::string, SemVer>;
        std::vector<std::vector<Comparator>> groups_; // OR of AND-groups
        bool isAny_ = false;
    };

    // Highest version in `versions` satisfying `range`. Prerelease versions
    // are excluded unless the range itself pins that exact major.minor.patch
    // with a prerelease tag. Returns "" if nothing matches.
    std::string maxSatisfying(const std::vector<std::string> &versions, const std::string &range);

} // namespace qpm
