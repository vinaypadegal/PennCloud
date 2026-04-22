#include <fstream>
#include <string>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include "serializer.h"

// Write a string to a stream
void writeString(std::ostream& out, const std::string& str) {
    uint32_t len = str.size();
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    out.write(str.data(), len);
}

// Read a string from a stream
bool readString(std::istream& in, std::string& str) {
    uint32_t len;
    if (!in.read(reinterpret_cast<char*>(&len), sizeof(len))) return false;
    str.resize(len);
    return static_cast<bool>(in.read(&str[0], len));
}

// Serialize tablet to a binary string
std::string serializeTablet(std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& table) {
    std::ostringstream out(std::ios::binary);

    uint32_t outerSize = table.size();
    out.write(reinterpret_cast<const char*>(&outerSize), sizeof(outerSize));

    for (const auto& [row, innerMap] : table) {
        writeString(out, row);
        uint32_t innerSize = innerMap.size();
        out.write(reinterpret_cast<const char*>(&innerSize), sizeof(innerSize));

        for (const auto& [col, val] : innerMap) {
            writeString(out, col);
            writeString(out, val);
        }
    }

    return out.str();
}

// Deserialize tablet from a binary string
bool deserializeTablet(std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& table, const std::string& buffer) {
    if (buffer.empty()) {
        return true;  // Treat empty input as valid
    }
    
    std::istringstream in(buffer, std::ios::binary);

    uint32_t outerSize;
    if (!in.read(reinterpret_cast<char*>(&outerSize), sizeof(outerSize))) return false;

    for (uint32_t i = 0; i < outerSize; ++i) {
        std::string row;
        if (!readString(in, row)) return false;

        uint32_t innerSize;
        if (!in.read(reinterpret_cast<char*>(&innerSize), sizeof(innerSize))) return false;

        for (uint32_t j = 0; j < innerSize; ++j) {
            std::string col, val;
            if (!readString(in, col)) return false;
            if (!readString(in, val)) return false;

            table[row][col] = val;
        }
    }

    return true;
}
