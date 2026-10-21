#include "Serializer.h"
#include <stdexcept>
#include <cstring>

enum class ValueType : uint8_t {
    VAL_NIL = 0,
    VAL_BOOL = 1,
    VAL_NUMBER = 2,
    VAL_STRING = 3,
    VAL_ARRAY = 4,
    VAL_CLOSURE = 5,
    VAL_LONG = 6
};

template <typename T>
void Serializer::writeRaw(std::vector<uint8_t>& out, const T& t) {
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&t);
    out.insert(out.end(), ptr, ptr + sizeof(T));
}

template <typename T>
T Serializer::readRaw(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset + sizeof(T) > data.size()) {
        throw std::runtime_error("Unexpected end of file while deserializing");
    }
    T t;
    std::memcpy(&t, data.data() + offset, sizeof(T));
    offset += sizeof(T);
    return t;
}

void Serializer::writeString(std::vector<uint8_t>& out, const std::string& s) {
    writeRaw<uint32_t>(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

std::string Serializer::readString(const std::vector<uint8_t>& data, size_t& offset) {
    uint32_t len = readRaw<uint32_t>(data, offset);
    if (offset + len > data.size()) {
        throw std::runtime_error("Unexpected string length while deserializing");
    }
    std::string s(reinterpret_cast<const char*>(data.data() + offset), len);
    offset += len;
    return s;
}

void Serializer::writeValue(std::vector<uint8_t>& out, const QuantumValue& val) {
    if (val.isNil()) {
        writeRaw(out, ValueType::VAL_NIL);
    } else if (val.isBool()) {
        writeRaw(out, ValueType::VAL_BOOL);
        writeRaw(out, static_cast<uint8_t>(val.asBool() ? 1 : 0));
    } else if (val.isNumber()) {
        writeRaw(out, ValueType::VAL_NUMBER);
        writeRaw(out, val.asNumber());
    } else if (val.isLong()) {
        writeRaw(out, ValueType::VAL_LONG);
        writeRaw(out, val.asLong());
    } else if (val.isString()) {
        writeRaw(out, ValueType::VAL_STRING);
        writeString(out, val.asString());
    } else if (val.isArray()) {
        writeRaw(out, ValueType::VAL_ARRAY);
        auto arr = val.asArray();
        writeRaw<uint32_t>(out, static_cast<uint32_t>(arr->size()));
        for (const auto& item : *arr) {
            writeValue(out, item);
        }
    } else if (val.isFunction()) {
        writeRaw(out, ValueType::VAL_CLOSURE);
        writeChunk(out, val.asFunction()->chunk);
    } else {
        throw std::runtime_error("Cannot serialize value of type " + val.typeName());
    }
}

QuantumValue Serializer::readValue(const std::vector<uint8_t>& data, size_t& offset) {
    ValueType type = readRaw<ValueType>(data, offset);
    switch (type) {
        case ValueType::VAL_NIL:
            return QuantumValue();
        case ValueType::VAL_BOOL:
            return QuantumValue(readRaw<uint8_t>(data, offset) != 0);
        case ValueType::VAL_NUMBER:
            return QuantumValue(readRaw<double>(data, offset));
        case ValueType::VAL_LONG:
            return QuantumValue::ofLong(readRaw<int64_t>(data, offset));
        case ValueType::VAL_STRING:
            return QuantumValue(readString(data, offset));
        case ValueType::VAL_ARRAY: {
            uint32_t size = readRaw<uint32_t>(data, offset);
            auto arr = std::make_shared<Array>(size);
            for (uint32_t i = 0; i < size; ++i) {
                (*arr)[i] = readValue(data, offset);
            }
            return QuantumValue(arr);
        }
        case ValueType::VAL_CLOSURE: {
            auto chunk = readChunk(data, offset);
            return QuantumValue(std::make_shared<Closure>(chunk));
        }
        default:
            throw std::runtime_error("Unknown value type while deserializing");
    }
}

void Serializer::writeChunk(std::vector<uint8_t>& out, std::shared_ptr<Chunk> chunk) {
    writeString(out, chunk->name);
    
    // code
    writeRaw<uint32_t>(out, static_cast<uint32_t>(chunk->code.size()));
    for (const auto& instr : chunk->code) {
        writeRaw(out, instr.op);
        writeRaw(out, instr.operand);
        writeRaw(out, instr.line);
    }
    
    // params
    writeRaw<uint32_t>(out, static_cast<uint32_t>(chunk->params.size()));
    for (const auto& param : chunk->params) {
        writeString(out, param);
    }
    
    // paramIsRef
    writeRaw<uint32_t>(out, static_cast<uint32_t>(chunk->paramIsRef.size()));
    for (bool isRef : chunk->paramIsRef) {
        writeRaw<uint8_t>(out, isRef ? 1 : 0);
    }
    
    // upvalueCount
    writeRaw(out, chunk->upvalueCount);
    
    // constants
    writeRaw<uint32_t>(out, static_cast<uint32_t>(chunk->constants.size()));
    for (const auto& c : chunk->constants) {
        writeValue(out, c);
    }
}

std::shared_ptr<Chunk> Serializer::readChunk(const std::vector<uint8_t>& data, size_t& offset) {
    auto chunk = std::make_shared<Chunk>();
    chunk->name = readString(data, offset);
    
    // code
    uint32_t codeSize = readRaw<uint32_t>(data, offset);
    chunk->code.reserve(codeSize);
    for (uint32_t i = 0; i < codeSize; ++i) {
        Instruction instr;
        instr.op = readRaw<Op>(data, offset);
        instr.operand = readRaw<int32_t>(data, offset);
        instr.line = readRaw<int>(data, offset);
        chunk->code.push_back(instr);
    }
    
    // params
    uint32_t paramsSize = readRaw<uint32_t>(data, offset);
    chunk->params.reserve(paramsSize);
    for (uint32_t i = 0; i < paramsSize; ++i) {
        chunk->params.push_back(readString(data, offset));
    }
    
    // paramIsRef
    uint32_t refSize = readRaw<uint32_t>(data, offset);
    chunk->paramIsRef.reserve(refSize);
    for (uint32_t i = 0; i < refSize; ++i) {
        chunk->paramIsRef.push_back(readRaw<uint8_t>(data, offset) != 0);
    }
    
    // upvalueCount
    chunk->upvalueCount = readRaw<int>(data, offset);
    
    // constants
    uint32_t constSize = readRaw<uint32_t>(data, offset);
    chunk->constants.reserve(constSize);
    for (uint32_t i = 0; i < constSize; ++i) {
        chunk->constants.push_back(readValue(data, offset));
    }
    
    return chunk;
}

std::vector<uint8_t> Serializer::serialize(std::shared_ptr<Chunk> chunk) {
    std::vector<uint8_t> out;
    writeChunk(out, chunk);
    return out;
}

std::shared_ptr<Chunk> Serializer::deserialize(const std::vector<uint8_t>& data) {
    size_t offset = 0;
    return readChunk(data, offset);
}
