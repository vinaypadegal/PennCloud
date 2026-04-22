#ifndef SERIALIZER_H
#define SERIALIZER_H

#include <string>
#include <unordered_map>

std::string serializeTablet(std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& table);
bool deserializeTablet(std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& table, const std::string& buffer);

#endif // SERIALIZER_H