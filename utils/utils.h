#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <map>

// Hashes a string to a number in the range [1, numBuckets]
int hashToBucket(std::string input, int numBuckets);

std::string encryptMetadata(const std::string &metadata);
std::string decryptMetadata(const std::string &encrypted);
std::string urlDecode(const std::string &src);
std::map<std::string, std::string> parseQueryString(const std::string &query);
std::map<std::string, std::string> parseMetadata(const std::string &meta);
std::string createMetadata(const std::string &sender, const std::string &recipient, const std::string &subject, const std::string &timestamp);

#endif
