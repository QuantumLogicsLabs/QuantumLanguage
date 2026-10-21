#pragma once

#include "Vm.h"
#include <vector>
#include <string>
#include <cstdint>

class Serializer {
public:
    static std::vector<uint8_t> serialize(std::shared_ptr<Chunk> chunk);
    static std::shared_ptr<Chunk> deserialize(const std::vector<uint8_t>& data);

private:
    static void writeChunk(std::vector<uint8_t>& out, std::shared_ptr<Chunk> chunk);
    static std::shared_ptr<Chunk> readChunk(const std::vector<uint8_t>& data, size_t& offset);

    static void writeValue(std::vector<uint8_t>& out, const QuantumValue& val);
    static QuantumValue readValue(const std::vector<uint8_t>& data, size_t& offset);

    static void writeString(std::vector<uint8_t>& out, const std::string& s);
    static std::string readString(const std::vector<uint8_t>& data, size_t& offset);

    template <typename T>
    static void writeRaw(std::vector<uint8_t>& out, const T& t);
    template <typename T>
    static T readRaw(const std::vector<uint8_t>& data, size_t& offset);
};
