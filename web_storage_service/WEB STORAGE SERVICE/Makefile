# Compiler and flags
CXX = g++
CXXFLAGS = -Wall -g -std=c++11
LDFLAGS = -lpthread

# Target executable and source files
TARGET = web_storage
SOURCES = web_storage.cpp

# Default target
all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCES) $(LDFLAGS)

# Clean up generated files
clean:
	rm -f $(TARGET)
