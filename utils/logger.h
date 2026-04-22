#ifndef LOGGER_H
#define LOGGER_H

#include <fstream>
#include <string>
#include <cstdint>
#include <iostream>
#include <vector>

struct CommandLog {
    std::string command;
    std::string row;
    std::string column;
    std::string value1;
    std::string value2;
};

class Logger {
public:
    explicit Logger(const std::string& path);
    ~Logger();

    Logger(Logger&&) = default;
    Logger& operator=(Logger&&) = default;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void logCommand(const CommandLog& cl);
    bool readNextCommand(CommandLog& cl);
    std::vector<CommandLog> getLogs();
    void clearLog();
    std::string getFilename();

private:
    std::ofstream out_;
    std::ifstream in_;
    std::string filename_;

    void writeString(const std::string& str);
    bool readString(std::string& str);
};

#endif
