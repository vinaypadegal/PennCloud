#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <sys/socket.h>
#include <sys/file.h> 
#include <netinet/in.h>
#include <cstring>
#include <csignal>
#include <dirent.h>
#include <ctime>
#include <fstream>
#include <fcntl.h>
#include <bits/stdc++.h>

using namespace std;
#define MAX_THREADS 100
#define BUFFER_SIZE 1024

bool debugMode = false;
int sockfd;
unordered_map<string, unordered_map<string, string>> kvstore;

unordered_set<int> activeClients;
pthread_mutex_t clientMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t kvMutex = PTHREAD_MUTEX_INITIALIZER;


string readFromSocket(int client_fd, string end="\r\n") {
  int bytesRead = 0;
  char buffer[BUFFER_SIZE];
  string recvdMsg = "";
  while (true) {
    bytesRead = recv(client_fd, &buffer, sizeof(buffer), 0);
    recvdMsg.append(buffer, bytesRead);
    if (recvdMsg.find(end) != string::npos) {
      break;
    }
  }
  // cout << "recvd: " << recvdMsg << endl;
  return recvdMsg;
}


bool writeToSocket(int client_fd, string message) {
  const char* data = message.c_str();
  int message_len = message.size();
  int bytesSent = 0;
  while (bytesSent < message_len) {
    int n = send(client_fd, &data[bytesSent], message_len - bytesSent, 0);
    if (n < 0) {
      return false;
    }
    bytesSent = bytesSent + n;
  }
  return true;
}


void printKV() {
	for (const auto& pair : kvstore) {
        std::cout << "Row: " << pair.first << endl;
		unordered_map<string , string> entry = pair.second;
		for (const auto& p : entry) {
			std::cout << "Col: " << p.first << ", Value: " << p.second << endl;
		}
		cout << endl;
    }
}


void expectHELLO(int fd) {
	bool greeted = false;
	while (!greeted) {
		string message = readFromSocket(fd);
		if (message.compare("HELLO\r\n")) {
			writeToSocket(fd, "-ERR Invalid command, start with HELLO\r\n");
		} else {
			writeToSocket(fd, "+OK Server Ready\r\n");
			greeted = true;
		}
	}
}


void handleGET(int fd, string row, string col) {
	if (kvstore.find(row) == kvstore.end()) {
		writeToSocket(fd, "-ERR Row does not exist\r\n");
		return;
	}
	unordered_map<string, string> kvstore_row = kvstore.at(row);
	if (kvstore.at(row).find(col) == kvstore.at(row).end()) {
		writeToSocket(fd, "-ERR Column does not exist\r\n");
		return;
	}
	string value = kvstore.at(row).at(col);
	string res = "+OK\n" + value + "\r\n";
	writeToSocket(fd, res);
}


void handlePUT(int fd, string row, string col, string value) {
	if (kvstore.find(row) == kvstore.end()) {
		unordered_map<string, string> entry;
		kvstore[row] = entry;
	}
	pthread_mutex_lock(&kvMutex);
	kvstore.at(row)[col] = value;
	pthread_mutex_unlock(&kvMutex);

	string res = "+OK\r\n";
	writeToSocket(fd, res);
}


void handleCPUT(int fd, string row, string col, string oldvalue, string newvalue) {
	if (kvstore.find(row) == kvstore.end()) {
		writeToSocket(fd, "-ERR Row does not exist\r\n");
		return;
	}
	if (kvstore.at(row).find(col) == kvstore.at(row).end()) {
		writeToSocket(fd, "-ERR Column does not exist\r\n");
		return;
	} else if (kvstore.at(row).at(col).compare(oldvalue)) {
		writeToSocket(fd, "-ERR Current value is different from provided value\r\n");
		return;
	}
	pthread_mutex_lock(&kvMutex);
	kvstore.at(row)[col] = newvalue;
	pthread_mutex_unlock(&kvMutex);

	string res = "+OK\r\n";
	writeToSocket(fd, res);
}


void handleDELETE(int fd, string row, string col) {
	if (kvstore.find(row) != kvstore.end()) {
		unordered_map<string, string> kvstore_row = kvstore.at(row);
		if (kvstore_row.find(col) != kvstore_row.end()) {
			pthread_mutex_lock(&kvMutex);
			kvstore.at(row).erase(col);
			if (kvstore.at(row).empty()) {
				kvstore.erase(row);
			}
			pthread_mutex_unlock(&kvMutex);
		}
	}
	string res = "+OK\r\n";
	writeToSocket(fd, res);
}


void processMessage(int client_fd, string message, bool *quit) {
	size_t pos = message.find(' '); 
	string command = message.substr(0, pos);
	string parameterString = message.substr(pos + 1);

	string s;
	stringstream ss(parameterString.substr(0, parameterString.size() - 2));
	vector<string> params;
	while (getline(ss, s, '|')) {
		params.push_back(s);
	}

	if (command == "GET") {
		// GET request received
		handleGET(client_fd, params.at(0), params.at(1));
	} else if (command == "PUT") {
		// PUT request received
		handlePUT(client_fd, params.at(0), params.at(1), params.at(2));
	} else if (command == "CPUT") {
		// CPUT request received
		handleCPUT(client_fd, params.at(0), params.at(1), params.at(2), params.at(3));
	} else if (command == "DELETE") {
		// CPUT request received
		handleDELETE(client_fd, params.at(0), params.at(1));
	} else if (message == "QUIT\r\n") {
		// QUIT received
		writeToSocket(client_fd, "+OK Goodbye\r\n");
    	*quit = true;
	} else {
		writeToSocket(client_fd, "-ERR Command unrecognized\r\n");
	}
	printKV();
}


void* communicationThreadFunc(void *args) {
	int client_fd = *(int *)args;
	
	expectHELLO(client_fd);

	bool quit = false;
	while (!quit) {
		string message = readFromSocket(client_fd);
		// cout << "message: " << message << endl;
		processMessage(client_fd, message, &quit);
	}
	close(client_fd);
	pthread_mutex_lock(&clientMutex);
	activeClients.erase(client_fd);
	pthread_mutex_unlock(&clientMutex);
	return nullptr;
}


int main(int argc, char *argv[]) {
	int c;
	unsigned short portNumber = 10000;

	while ((c = getopt(argc, argv, "vp:")) != -1) {
		switch (c) {
			case 'v':
				debugMode = true;
				break;
			case 'p':
				portNumber = atoi(optarg);
				break;
		}
	}

	// create socket
	sockfd = socket(PF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		fprintf(stderr, "Cannot open socket, exiting.\n");
		exit(EXIT_FAILURE);
	}

	// bind socket to IP and Port
	struct sockaddr_in server_addr;
	bzero(&server_addr, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htons(INADDR_ANY);
	server_addr.sin_port = htons(portNumber);
	bind(sockfd, (struct sockaddr*) &server_addr, sizeof(server_addr));

	// listen on port <portNumber>
	listen(sockfd, 10);

	while (true) {
		int client_fd = accept(sockfd, NULL, NULL);
		if (client_fd < 0) {
			perror("Accepting client failed..");
			continue;
		}

		pthread_t thread;
		pthread_mutex_lock(&clientMutex);
		activeClients.insert(client_fd);
		pthread_mutex_unlock(&clientMutex);

		pthread_create(&thread, NULL, communicationThreadFunc, &client_fd);
		pthread_detach(thread);
  	}

	return 0;
}
// kvstore_server.cc
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <cstring>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef KVSTORE_CLIENT_H
#define KVSTORE_CLIENT_H

using namespace std;

// Global variables for configuration.
bool debugMode = false;
bool aSet = false;
int sockfd;

// In-memory key–value store: maps a row to a mapping of column names to values
unordered_map<string, unordered_map<string, string>> kvStore;

// Function to process a single command line and return a response.
// SUPPORTED COMMANDS:
// PUT <row> <column> <value>
// GET <row> <column>
// DELETE <row> <column>
// CPUT <row> <column> <old_value> <new_value>
// SCAN <prefix>
string processCommand(const string &commandLine) {
    istringstream iss(commandLine);
    string cmd;
    iss >> cmd;
    
    if (cmd == "PUT") {
        string row, col, value;
        iss >> row >> col;
        getline(iss, value);
        if (!value.empty() && value[0] == ' ')
            value.erase(0, 1); // Remove leading space.
        kvStore[row][col] = value;
        return "OK\n";
    } else if (cmd == "GET") {
        string row, col;
        iss >> row >> col;
        if (kvStore.find(row) != kvStore.end() && kvStore[row].find(col) != kvStore[row].end()) {
            return kvStore[row][col] + "\n";
        } else {
            return "NOT_FOUND\n";
        }
    } else if (cmd == "DELETE") {
        string row, col;
        iss >> row >> col;
        if (kvStore.find(row) != kvStore.end() && kvStore[row].find(col) != kvStore[row].end()) {
            kvStore[row].erase(col);
            return "OK\n";
        } else {
            return "NOT_FOUND\n";
        }
    } else if (cmd == "CPUT") {
        string row, col, oldVal, newVal;
        iss >> row >> col >> oldVal;
        getline(iss, newVal);
        if (!newVal.empty() && newVal[0] == ' ')
            newVal.erase(0, 1);
        if (kvStore.find(row) != kvStore.end() && kvStore[row].find(col) != kvStore[row].end()) {
            if (kvStore[row][col] == oldVal) {
                kvStore[row][col] = newVal;
                return "OK\n";
            } else {
                return "FAIL\n";
            }
        } else {
            return "NOT_FOUND\n";
        }
    }
    // New SCAN command to list keys starting with a given prefix.
    else if (cmd == "SCAN") {
        string prefix;
        iss >> prefix;
        string result;
        for (const auto &pair : kvStore) {
            if (pair.first.find(prefix) == 0) { // if row key starts with prefix
                result += pair.first + "\n";
            }
        }
        return "+OK\n" + result;
    } else {
        return "ERROR: Unknown command\n";
    }
}

int main(int argc, char *argv[]) {
    int c;
    unsigned short portNumber = 10000; // Default port number

    // Parse CLI options
    while ((c = getopt(argc, argv, "vp:")) != -1) {
        switch (c) {
            case 'v':
                debugMode = true;
                break;
            case 'p':
                portNumber = atoi(optarg);
                break;
            default:
                aSet = true;
                break;
        }
    }

    if (aSet) {
        fprintf(stderr, "%s", "Project T04\n");
        exit(EXIT_FAILURE);
    }

    // Create the server socket.
    sockfd = socket(PF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "Cannot open socket, exiting.\n");
        exit(EXIT_FAILURE);
    }

    // Bind the socket to an IP address and port.
    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(portNumber);
    if (bind(sockfd, (struct sockaddr*) &server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections.
    if (listen(sockfd, 10) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    cout << "KVStore server listening on port " << portNumber << endl;

    // Main loop: accept and process incoming connections
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int newsockfd = accept(sockfd, (struct sockaddr*)&client_addr, &addr_len);
        if (newsockfd < 0) {
            perror("accept");
            continue;
        }
        
        // Read data from the client.
        char buffer[4096];
        memset(buffer, 0, sizeof(buffer));
        int n = read(newsockfd, buffer, sizeof(buffer) - 1);
        if (n < 0) {
            perror("read");
            close(newsockfd);
            continue;
        }
        string commandLine(buffer);
        if (debugMode) {
            cout << "Received command: " << commandLine;
        }
        
        string response = processCommand(commandLine);
        
        // Write the response back to the client
        write(newsockfd, response.c_str(), response.size());
        close(newsockfd);
    }
    
    close(sockfd);
    return 0;
}
#endif