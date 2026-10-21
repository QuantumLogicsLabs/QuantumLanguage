#include "QpmColor.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <unistd.h>
#endif

#include <cstdlib>

namespace qpm
{
    namespace
    {
        int g_color = -1; // -1 = not decided yet

        bool detect()
        {
            if (std::getenv("NO_COLOR"))
                return false;
            const char *force = std::getenv("FORCE_COLOR");
            if (force && *force && std::string(force) != "0")
                return true;
#ifdef _WIN32
            HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD mode = 0;
            if (!GetConsoleMode(out, &mode))
                return false;
            return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) ||
                   SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
            return isatty(1);
#endif
        }
    } // namespace

    bool colorEnabled()
    {
        if (g_color < 0)
            g_color = detect() ? 1 : 0;
        return g_color == 1;
    }

    void disableColor()
    {
        g_color = 0;
    }

    bool stdoutIsConsole()
    {
#ifdef _WIN32
        DWORD mode = 0;
        return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
#else
        return isatty(1);
#endif
    }

    std::string paint(const std::string &text, const char *sgr)
    {
        return colorEnabled() ? std::string("\x1b[") + sgr + "m" + text + "\x1b[0m" : text;
    }

} // namespace qpm
