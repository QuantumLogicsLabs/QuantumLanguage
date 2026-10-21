#pragma once
// ANSI colors for qpm's console output. They're on when stdout is a console
// that understands them (qpm switches that on where needed) or FORCE_COLOR is
// set, and off through a pipe, with NO_COLOR, or after `--no-color`.

#include <string>

namespace qpm
{

    bool colorEnabled();
    void disableColor();

    // True when stdout is a console window rather than a pipe or a file.
    bool stdoutIsConsole();

    // `text` wrapped in the SGR code `sgr` ("1;32", "90", "38;5;208", ...), or as is without color.
    std::string paint(const std::string &text, const char *sgr);

} // namespace qpm
