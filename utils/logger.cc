#include <fstream>
#include <string>
#include <cstdint>
#include <iostream>
#include <vector>
#include "logger.h"

Logger::Logger(const std::string& path)
    : filename_(path),
      out_(path, std::ios::binary | std::ios::app),
      in_(path, std::ios::binary) {
    if (!out_) throw std::runtime_error("Failed to open log file for writing");
    if (!in_) throw std::runtime_error("Failed to open log file for reading");
}

Logger::~Logger() {
    out_.close();
    in_.close();
}

void Logger::writeString(const std::string& str) {
    uint32_t len = str.size();
    out_.write(reinterpret_cast<const char*>(&len), sizeof(len));
    out_.write(str.data(), len);
}

bool Logger::readString(std::string& str) {
    uint32_t len;
    if (!in_.read(reinterpret_cast<char*>(&len), sizeof(len))) return false;

    str.resize(len);
    return static_cast<bool>(in_.read(&str[0], len));
}

void Logger::logCommand(const CommandLog& cl) {
    writeString(cl.command);
    writeString(cl.row);
    writeString(cl.column);
    writeString(cl.value1);
    writeString(cl.value2);
    out_.flush();
}

bool Logger::readNextCommand(CommandLog& cl) {
    return readString(cl.command) &&
           readString(cl.row) &&
           readString(cl.column) &&
           readString(cl.value1) && 
           readString(cl.value2);
}

std::vector<CommandLog> Logger::getLogs() {
    in_.clear();               // Clear EOF and error flags
    in_.seekg(0, std::ios::beg);  // Move read pointer to the beginning

    std::vector<CommandLog> logs;
    CommandLog cl;
    while (readNextCommand(cl)) {
        // std::cout << "[Replaying] " << cl.command << " " << cl.row << " "
        //             << cl.column << " " << cl.value1 << " " << cl.value2 << "\n";
        logs.push_back(cl);
    }
    return logs;
}

void Logger::clearLog() {
    out_.close();
    in_.close();

    std::ofstream clearOut(filename_, std::ios::trunc | std::ios::binary);
    clearOut.close();

    // Reopen streams after clearing
    out_.open(filename_, std::ios::trunc | std::ios::binary);
    in_.open(filename_, std::ios::binary);
}

std::string Logger::getFilename() {
    return filename_;
}