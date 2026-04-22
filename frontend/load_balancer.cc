// HTTP redirector with both a GET /health JSON endpoint and
// HEAD‐based health checks to backends.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <mutex>
#include <thread>

using namespace std;

// List of frontend servers (must be of the form "http://IP:PORT")
static const vector<string> FRONTENDS = {
    "http://127.0.0.1:8081",
    "http://127.0.0.1:8082",
    "http://127.0.0.1:8083"
};

static size_t next_frontend = 0;
static mutex rr_mutex;

// Performs a HEAD /health request against a frontend, returns true on 200 OK
bool is_alive(const string &frontend) {
    string url = frontend;
    if (url.rfind("http://", 0) == 0) url.erase(0, 7);
    auto colon = url.rfind(':');
    if (colon == string::npos) return false;
    string host = url.substr(0, colon);
    int port = stoi(url.substr(colon + 1));

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        close(sock);
        return false;
    }

    // 1s connect + I/O timeout
    timeval tv{1, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }

    // send HEAD /health
    ostringstream req;
    req << "HEAD /health HTTP/1.1\r\n"
        << "Host: " << host << "\r\n"
        << "Connection: keep-alive\r\n\r\n";
    string rq = req.str();
    cout<<"Request: "<<rq<<endl;
    if (send(sock, rq.c_str(), rq.size(), 0) != (ssize_t)rq.size()) {
        close(sock);
        return false;
    }

    char buf[128];
    ssize_t n = recv(sock, buf, sizeof(buf)-1, 0);
    close(sock);
    if (n <= 0) return false;
    buf[n] = '\0';
    return string(buf).find("200") != string::npos;
}

// Round‐robin pick of a live frontend
string select_frontend() {
    size_t start;
    {
        lock_guard<mutex> lock(rr_mutex);
        start = next_frontend;
    }
    size_t N = FRONTENDS.size();
    for (size_t i = 0; i < N; i++) {
        size_t idx = (start + i) % N;
        if (is_alive(FRONTENDS[idx])) {
            lock_guard<mutex> lock(rr_mutex);
            next_frontend = (idx + 1) % N;
            return FRONTENDS[idx];
        }
    }
    return "";  // none alive
}

// Read only the request line (e.g., "GET /path HTTP/1.1")
bool read_request_line(int sock, string &method, string &path) {
    const int BUF_SIZE = 4096;
    char buf[BUF_SIZE];
    ssize_t n = recv(sock, buf, BUF_SIZE - 1, 0);
    if (n <= 0) return false;
    buf[n] = '\0';
    string req(buf, n);
    auto pos = req.find("\r\n");
    if (pos == string::npos) return false;
    istringstream iss(req.substr(0, pos));
    iss >> method >> path;
    return !method.empty() && !path.empty();
}

void handle_client(int client_sock) {
    string method, path;
    if (!read_request_line(client_sock, method, path)) {
        close(client_sock);
        return;
    }

    // Special GET /health endpoint returning JSON for all frontends
    if (method == "GET" && path == "/health") {
        ostringstream body;
        body << "[";
        for (size_t i = 0; i < FRONTENDS.size(); ++i) {
            bool up = is_alive(FRONTENDS[i]);
            // extract port
            string url = FRONTENDS[i];
            if (url.rfind("http://", 0) == 0) url.erase(0, 7);
            string port = url.substr(url.rfind(':') + 1);
            body << "{\"id\":" << (i+1)
                 << ",\"port\":\"" << port << "\""
                 << ",\"alive\":" << (up ? "true" : "false") << "}";
            if (i + 1 < FRONTENDS.size()) body << ",";
        }
        body << "]";

        string b = body.str();
        ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << b.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << b;
        string out = resp.str();
        send(client_sock, out.c_str(), out.size(), 0);
        close(client_sock);
        return;
    }

    // Otherwise, redirect to a live frontend
    string target = select_frontend();
    if (target.empty()) {
        const char resp[] =
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send(client_sock, resp, sizeof(resp)-1, 0);
        close(client_sock);
        return;
    }

    string location = target + path;
    ostringstream oss;
    oss << "HTTP/1.1 307 Temporary Redirect\r\n"
        << "Location: " << location << "\r\n"
        << "Content-Length: 0\r\n"
        << "Connection: close\r\n\r\n";
    string response = oss.str();
    send(client_sock, response.c_str(), response.size(), 0);
    close(client_sock);
}

int main(int argc, char *argv[]) {
    int port = 80;
    for (int i = 1; i + 1 < argc; ++i) {
        if (string(argv[i]) == "-p") {
            port = stoi(argv[i+1]);
        }
    }

    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_sock); return 1;
    }
    if (listen(listen_sock, 128) < 0) {
        perror("listen"); close(listen_sock); return 1;
    }

    cout << "Load balancer listening on port " << port << endl;
    while (true) {
        sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_sock = accept(listen_sock, (sockaddr*)&cli_addr, &cli_len);
        if (client_sock < 0) continue;
        thread(handle_client, client_sock).detach();
    }

    close(listen_sock);
    return 0;
}
