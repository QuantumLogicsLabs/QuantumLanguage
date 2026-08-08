cmake_minimum_required(VERSION 3.16)
project(Quantum CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if(MSVC)
    add_compile_options(/O2 /W3)
else()
    add_compile_options(-O2 -Wall -Wextra -Wno-unused-parameter)
    # Static-link the MinGW/GCC runtime so the .exe works without any DLLs
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

# ── Collect sources ────────────────────────────────────────────────────────────
file(GLOB_RECURSE QUANTUM_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(FILTER QUANTUM_SOURCES EXCLUDE REGEX ".*main_vm\\.cpp$")
# qpm is a standalone tool (its own main(), no VM/lexer/parser/compiler needed)
# built from src/qpm/ separately — keep it out of the other 3 targets' sources.
list(FILTER QUANTUM_SOURCES EXCLUDE REGEX ".*/src/qpm/.*\\.cpp$")

# ── quantum  — compiler + bundler ─────────────────────────────────────────────
add_executable(quantum ${QUANTUM_SOURCES})
target_compile_definitions(quantum PRIVATE QUANTUM_MODE_COMPILER=1)
if(WIN32)
    target_link_libraries(quantum advapi32)
endif()

# ── qrun  — pure interpreter ──────────────────────────────────────────────────
add_executable(qrun ${QUANTUM_SOURCES})
target_compile_definitions(qrun PRIVATE QRUN_MODE=1)
if(WIN32)
    target_link_libraries(qrun advapi32)
endif()

# ── quantum_stub  — standalone runtime template ───────────────────────────────
add_executable(quantum_stub ${QUANTUM_SOURCES})
if(WIN32)
    target_link_libraries(quantum_stub advapi32)
endif()

# ── qpm  — standalone npm-compatible package manager (Windows only) ───────────
if(WIN32)
    file(GLOB QPM_SOURCES CONFIGURE_DEPENDS "src/qpm/*.cpp")
    add_executable(qpm ${QPM_SOURCES})
    target_link_libraries(qpm z winhttp crypt32 ws2_32)
endif()


message(STATUS "Quantum v2.0.0 — Bytecode VM  (static linking enabled)")
message(STATUS "  Binaries : quantum  qrun  quantum_stub  qpm")