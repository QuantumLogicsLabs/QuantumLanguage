#pragma once
// Project manifests. A project directory can describe itself with any one of
// (looked up in this order):
//
//   package.qon       either style: a JSON object exactly like package.json,
//                     or the requirements style below
//   package.json      npm's JSON manifest
//   requirements.txt  the requirements style below
//
// Requirements style — one dependency per line, '#' starts a comment:
//
//   lodash==4.17.21          pip operators: == === ~= >= <= > < (comma = AND)
//   chalk>=4,<6
//   left-pad ^1.3.0          or an npm range after a space or '@'
//   express@^4
//   mocha                    no range = latest
//
//   [devDependencies]        optional sections ([dev] / [dev-dependencies] work too);
//   jest ^29                 lines before any section are "dependencies"
//
//   [package]                any other section holds `key = value` pairs;
//   name = my-app            a value starting with [ { or " is parsed as JSON
//   version = 1.0.0
//   main = index.sa
//
//   [scripts]
//   start = qrun index.sa
//
// Whatever the format, the manifest is exposed as the same npm-shaped JSON
// object, so callers just read "name", "dependencies", "scripts", ...

#include "QpmJson.h"

#include <filesystem>
#include <string>

namespace qpm
{

    enum class ManifestFormat
    {
        Json,
        Requirements
    };

    struct Manifest
    {
        std::filesystem::path path; // file it was read from (or will be written to)
        ManifestFormat format = ManifestFormat::Json;
        JsonValue data;             // npm-shaped object
        std::string text;           // raw text, kept so requirements-style edits preserve comments
    };

    // The manifest file in `dir`, or an empty path if it has none.
    std::filesystem::path findManifest(const std::filesystem::path &dir);
    inline bool hasManifest(const std::filesystem::path &dir) { return !findManifest(dir).empty(); }

    // Reads and parses a manifest file. Throws std::runtime_error on bad input.
    Manifest loadManifest(const std::filesystem::path &path);

    // Parses manifest text; `fileName` decides the format (package.qon sniffs it).
    Manifest parseManifest(const std::string &text, const std::filesystem::path &fileName);

    // Adds or replaces a "dependencies" entry, in data and (for the
    // requirements style) in the raw text, leaving everything else untouched.
    void setDependency(Manifest &m, const std::string &name, const std::string &range);

    // The text to write back to m.path.
    std::string serializeManifest(const Manifest &m);

    // Turns a pip-style specifier ("==1.2", ">=1,<2", "~=1.4") into an npm
    // range; npm ranges pass through unchanged.
    std::string toNpmRange(const std::string &spec);

} // namespace qpm
