#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <functional>
#include "utils.h"
#include <bits/stdc++.h>

using namespace std;

int hashToBucket(string input, int numBuckets) {
    size_t hash = std::hash<std::string>{}(input);
    return static_cast<int>((hash % numBuckets) + 1);
}

// Simple query string parser
map<string, string> parseQueryString(const string &query) {
    map<string, string> params;
    size_t start = 0;
    while (start < query.size()) {
        size_t eqPos = query.find('=', start);
        if (eqPos == string::npos)
            break;
        string key = query.substr(start, eqPos - start);
        size_t ampPos = query.find('&', eqPos + 1);
        if (ampPos == string::npos) {
            string value = query.substr(eqPos + 1);
            params[key] = value;
            break;
        } else {
            string value = query.substr(eqPos + 1, ampPos - eqPos - 1);
            params[key] = value;
            start = ampPos + 1;
        }
    }
    return params;
}

map<string, string> parseMetadata(const string &meta) {
    map<string, string> headers;
    istringstream iss(meta);
    string token;
    while (getline(iss, token, ';')) {
        size_t colon = token.find(':');
        if (colon != string::npos) {
            string key = token.substr(0, colon);
            string value = token.substr(colon + 1);
            headers[key] = value;
        }
    }
    return headers;
}

//Utility function to decode url
string urlDecode(const string &src) {
    ostringstream decoded;
    for (size_t i = 0; i < src.length(); ++i) {
        if (src[i] == '%' && i + 2 < src.length()) {
            istringstream iss(src.substr(i + 1, 2));
            int hexVal;
            if (iss >> hex >> hexVal)
                decoded << static_cast<char>(hexVal);
            i += 2;
        } else if (src[i] == '+') {
            decoded << ' ';
        } else {
            decoded << src[i];
        }
    }
    return decoded.str();
}

// A simple concatenation function to create metadata string:
string createMetadata(const string &sender,
    const string &recipient,
    const string &subject,
    const string &timestamp) {
// For example, format: sender:alice;recipient:bob;subject:Hello;timestamp:1632312345;
ostringstream oss;
oss << "sender:" << sender << ";"
<< "recipient:" << recipient << ";"
<< "subject:" << subject << ";"
<< "timestamp:" << timestamp << ";";
return oss.str();
}


// Dummy encryption function (replace with a real encryption in submission)
string encryptMetadata(const string &metadata) {
    return string(metadata.rbegin(), metadata.rend());
}

// Dummy decryption function (simply reverse)
string decryptMetadata(const string &encrypted) {
    return string(encrypted.rbegin(), encrypted.rend());
}

    
// Generate a unique ID (using current time).
string generateUniqueId() {
    time_t now = time(nullptr);
    return to_string(now);
}