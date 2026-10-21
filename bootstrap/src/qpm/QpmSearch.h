#pragma once
// Read-only registry queries. `qpm search` asks the QPM registry's /search
// endpoint (npm's /-/v1/search on an npm-style registry) and prints a table
// like `npm search`; `qpm view` prints one package's registry document like
// `npm view`.

#include <string>
#include <vector>

namespace qpm
{

    struct SearchOptions
    {
        std::vector<std::string> terms; // every term must match; "=user" matches the author
        bool npm = false;               // search the upstream npm registry instead
        bool json = false;              // print the results as a JSON array
        bool parseable = false;         // tab-separated, one package per line
        bool longFormat = false;        // don't truncate rows to the console width
        bool description = true;        // include the DESCRIPTION column
        bool color = true;              // highlight matches when stdout is a console
        int limit = 20;                 // most results to print
    };

    // Returns a process exit code.
    int runSearch(const SearchOptions &opts);

    struct ViewOptions
    {
        std::string spec;                // "name", "name@1.2.0", "name@^1" or "name@latest"
        std::vector<std::string> fields; // dotted paths to print instead of the summary
        bool json = false;
        bool color = true;
    };

    // Returns a process exit code.
    int runView(const ViewOptions &opts);

} // namespace qpm
