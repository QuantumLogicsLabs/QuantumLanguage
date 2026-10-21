#pragma once
#include "Opcode.h"
#include <string>
#include <ostream>

// Pretty-print a single instruction; returns bytes consumed (always 1 for us).
void disassembleInstruction(const Chunk &chunk, size_t idx, std::ostream &out);

// Dump an entire chunk.
void disassembleChunk(const Chunk &chunk, std::ostream &out);