#ifndef KVSTORE_CLIENT_H
#define KVSTORE_CLIENT_H

#include <string>

using namespace std;

class KVStoreClient {
public:
    // Constructor: take host and port where the KVStore server is running.
    KVStoreClient(const string &host, int port);

    // Connect to the server (you can call this in the constructor).
    bool connectToServer();

    // Basic operations.
    void put(const string &row, const string &col, const string &value);
    string get(const string &row, const string &col);
    void remove(const string &row, const string &col);
    string cput(const string &row, const string &col, const string &oldValue, const string &newValue);

private:
    string host_;
    int port_;
    int sockfd_;

    // Helper to send command and receive response.
    string sendCommand(const string &command);
};

#endif // KVSTORE_CLIENT_H
