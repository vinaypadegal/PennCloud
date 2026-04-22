#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fstream>
#include <cstring>
#include <cstdlib>

// gRPC includes
#include <grpcpp/grpcpp.h>
#include "kvstore.grpc.pb.h"
#include "kvs_client.h"

using namespace std;

// Global pointer to our gRPC key–value store client.
KeyValueStoreClient* kvClient = nullptr;

// Maximum allowed file size: 2 MB in this example.
const size_t MAX_FILE_SIZE = 2 * 1024 * 1024; // 2 MB

// Helper: Build a basic HTTP response.
string httpResponse(const string &status, const string &body, const string &contentType = "text/html") {
    return "HTTP/1.1 " + status + "\r\n" +
           "Content-Type: " + contentType + "\r\n" +
           "Content-Length: " + to_string(body.size()) + "\r\n" +
           "\r\n" + body;
}

// Helper: Parse query parameters from a URL query string.
unordered_map<string, string> parseQueryParams(const string &query) {
    unordered_map<string, string> params;
    istringstream ss(query);
    string pair;
    while (getline(ss, pair, '&')) {
        size_t eqPos = pair.find('=');
        if (eqPos != string::npos) {
            string key = pair.substr(0, eqPos);
            string value = pair.substr(eqPos + 1);
            params[key] = value;
        }
    }
    return params;
}

// Helper: Read the request body from the socket up to a maximum file size.
// It uses the Content-Length header if available.
string readRequestBody(int clientSocket, const string &request, size_t headerEnd) {
    // Try to parse the Content-Length header.
    size_t contentLength = 0;
    size_t clPos = request.find("Content-Length:");
    if (clPos != string::npos) {
        size_t start = request.find_first_of("0123456789", clPos);
        size_t end = request.find("\r\n", start);
        try {
            contentLength = stoi(request.substr(start, end - start));
        } catch (...) {
            contentLength = 0;
        }
    }
    // If no Content-Length header, we will read until no more data is available,
    // but never exceeding MAX_FILE_SIZE.
    if (contentLength == 0 || contentLength > MAX_FILE_SIZE)
        contentLength = MAX_FILE_SIZE;

    // Initially, we have already read the portion in the initial request string.
    string body = request.substr(headerEnd + 4);
    size_t totalSize = body.size();

    // Read until we've got all expected bytes or reach MAX_FILE_SIZE.
    while (totalSize < contentLength && totalSize < MAX_FILE_SIZE) {
        char buf[8192];
        int bytes = read(clientSocket, buf, sizeof(buf));
        if (bytes <= 0) break;
        body.append(buf, bytes);
        totalSize += bytes;
    }
    return body;
}

// Minimal integrated HTTP server function.
// Uses a flat namespace (default row "user") and communicates with the backend via gRPC.
void handleClient(int clientSocket) {
    const int bufferSize = 8192;
    char buffer[bufferSize];
    int bytesRead = read(clientSocket, buffer, bufferSize - 1);
    if (bytesRead <= 0) {
        close(clientSocket);
        return;
    }
    buffer[bytesRead] = '\0';
    string request(buffer);

    // Parse the request line.
    istringstream reqStream(request);
    string requestLine;
    getline(reqStream, requestLine);  // e.g., "GET /index.html HTTP/1.1"
    istringstream lineStream(requestLine);
    string method, url, httpVersion;
    lineStream >> method >> url >> httpVersion;

    // For minimal solution, we use a fixed row "user".
    string row = "user";

    if (method == "GET") {
        // Serve external webpage.html when "/" or "/index.html" is requested.
        if (url == "/" || url == "/index.html") {
            ifstream htmlFile("web_pages/webpage.html");
            if (!htmlFile.is_open()) {
                string notFound = "<html><body><h1>404 Not Found</h1><p>webpage.html missing.</p></body></html>";
                string response = httpResponse("404 Not Found", notFound);
                write(clientSocket, response.c_str(), response.size());
            } else {
                stringstream ss;
                ss << htmlFile.rdbuf();
                string htmlContent = ss.str();
                string response = httpResponse("200 OK", htmlContent, "text/html");
                write(clientSocket, response.c_str(), response.size());
            }
        }
        // Handle download requests: GET /download?filename=myfile
        else if (url.find("/download") == 0) {
            size_t qPos = url.find("?filename=");
            string filename = "myfile";  // default filename
            if (qPos != string::npos) {
                filename = url.substr(qPos + 10);
            }
            // Call the gRPC backend to retrieve file content.
            string fileContent = kvClient->Get(row, filename);
            if (fileContent.empty()) {
                string notFound = "<html><body><h1>404 Not Found</h1><p>File not found.</p></body></html>";
                string response = httpResponse("404 Not Found", notFound);
                write(clientSocket, response.c_str(), response.size());
            } else {
            	fileContent = fileContent.substr(fileContent.find("\r\n\r\n")+4);
            	fileContent = fileContent.substr(0, fileContent.find("\r\n"));
                string response = httpResponse("200 OK", fileContent, "application/octet-stream");
                write(clientSocket, response.c_str(), response.size());
            }
        }
        else {
            string response = httpResponse("400 Bad Request", "<html><body>Unsupported GET URL.</body></html>");
            write(clientSocket, response.c_str(), response.size());
        }
    }
    else if (method == "POST") {
        // Handle file upload requests: POST /upload?filename=myfile
        if (url.find("/upload") == 0) {
            size_t qPos = url.find("?filename=");
            string filename = "myfile";
            if (qPos != string::npos) {
                filename = url.substr(qPos + 10);
            }
            size_t headerEnd = request.find("\r\n\r\n");
            if (headerEnd == string::npos) {
                string resp = httpResponse("400 Bad Request", "<html><body>Missing header terminator.</body></html>");
                write(clientSocket, resp.c_str(), resp.size());
                close(clientSocket);
                return;
            }
            // Read the file content from the socket (up to MAX_FILE_SIZE)
            string fileContent = readRequestBody(clientSocket, request, headerEnd);
            // Optionally, you can check here if fileContent.size() == MAX_FILE_SIZE and then reject if contentLength was larger.
            // Call the gRPC backend to store the file.
            string result = kvClient->Put(row, filename, fileContent);
            std::cout<<"Filename:"<<filename<<std::endl;
            string html = "<html><body>Upload Result: " + result + "<br/><a href='/'>Back</a></body></html>";
            string response = httpResponse("200 OK", html);
            write(clientSocket, response.c_str(), response.size());
        }
        else {
            string response = httpResponse("400 Bad Request", "<html><body>Unsupported POST URL.</body></html>");
            write(clientSocket, response.c_str(), response.size());
        }
    }
    else {
        string response = httpResponse("400 Bad Request", "<html><body>Unsupported method.</body></html>");
        write(clientSocket, response.c_str(), response.size());
    }

    close(clientSocket);
}

int main() {
    // Initialize the gRPC client (assume backend runs on port 10001).
    kvClient = new KeyValueStoreClient(grpc::CreateChannel("0.0.0.0:10001", grpc::InsecureChannelCredentials()));

    // Create a TCP socket.
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        cerr << "Error creating socket" << endl;
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(8080);
    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        cerr << "Bind failed" << endl;
        return 1;
    }

    listen(serverSocket, 10);
    cout << "Web storage service listening on port 8080..." << endl;

    while (true) {
        int clientSocket = accept(serverSocket, nullptr, nullptr);
        if (clientSocket >= 0) {
            handleClient(clientSocket);
        }
    }

    close(serverSocket);
    return 0;
}
