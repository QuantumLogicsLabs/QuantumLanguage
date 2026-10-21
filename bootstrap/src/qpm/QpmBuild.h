#pragma once
// `qpm build` — builds the project in the current folder.
//
//   1. A "build" entry in the manifest's "scripts" runs, like `npm run build`.
//   2. Otherwise the folder is built as a native C/C++ program:
//        CMakeLists.txt  → cmake configure (MinGW Makefiles / Ninja) + build
//        Makefile        → make
//        neither, or C++ written in .sa files (which CMake/make rules don't
//        compile) → qpm compiles the sources itself: one executable per file
//        that defines main(), in parallel, recompiling only what changed,
//        with include/ and src/ on the include path and the link libraries
//        for common game / graphics headers (raylib, SDL, SFML, GLFW,
//        OpenGL, windows.h, ...) added automatically.
//
// An optional "build" object in the manifest tunes the direct build:
//
//   "build": {
//     "name": "mygame",            main executable's name (default: package/folder name)
//     "sources": ["src", "vendor/glad.c"],   files/folders to compile (default: all)
//     "exclude": ["tools"],
//     "include": ["vendor/include"],
//     "defines": ["USE_AUDIO"],
//     "libs": ["raylib"],          extra -l libraries
//     "libDirs": ["vendor/lib"],
//     "std": "c++20",              C++ standard (C: "cstd")
//     "flags": ["-Wall"],          extra compile flags
//     "linkFlags": ["-mwindows"],
//     "static": true,              link everything statically
//     "autoLibs": false,           don't add libraries for recognised headers
//     "outDir": "build",
//     "backend": "direct"          or "cmake" / "make"
//   }
//
// The toolchain (g++/gcc or clang++/clang) comes from CXX / CC, PATH, or the
// usual MSYS2 / MinGW / LLVM install folders.

#include <string>
#include <vector>

namespace qpm
{

    enum class BuildBackend
    {
        Auto,
        CMake,
        Make,
        Direct
    };

    struct BuildOptions
    {
        std::string projectDir;
        BuildBackend backend = BuildBackend::Auto; // anything but Auto skips scripts.build
        bool release = true;
        bool run = false;               // run the built program afterwards
        bool clean = false;             // remove build outputs instead of building
        bool verbose = false;           // print every compiler command
        int jobs = 0;                   // parallel compiles; 0 = one per CPU
        std::string target;             // executable to build / run; empty = all
        std::vector<std::string> runArgs;
    };

    // Returns 0 on success (or the program's exit code with `run`).
    int runBuild(const BuildOptions &opts);

} // namespace qpm
