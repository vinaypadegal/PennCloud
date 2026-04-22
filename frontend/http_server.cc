#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <signal.h>
#include <fstream>
#include <sstream>
#include <ctime>

#include <grpcpp/grpcpp.h>
#include "../build/kvstore.grpc.pb.h"
#include "../build/master.grpc.pb.h"
#include "../backend/kvs_client.h"

#include "user_account/user_account_service.h"
#include "admin/admin_service.h"
#include "storage/storage_service.h"
#include "email/email_service.h"

using grpc::Channel;
using grpc::ClientContext;

using namespace std;

//Core server globals: active sockets, buffers, mutexes, server socket, file size limit, sessions, and credentials
vector<int> sockets_active;
bool v = false;
unordered_map<int, string> buffer_clients;
pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;
int socket_server;
const size_t MAX_FILE_SIZE = 2*1024*1024;

unordered_map<string, string> session_cookies;

unordered_map<string, string> user_credentials = {
		{"user1", "pass1"},
		{"user2", "pass2"},
		{"admin", "pass"}
};

//Utility functions and data: verbose logger and MIME type lookup table

void v_logging(const string &message){
	if(v){
		cerr<<message<<endl;
	}
}

unordered_map<string, string> mime_types = {
    {".html", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".txt", "text/plain"},
    {".json", "application/json"},
    {".ico", "image/x-icon"}
};

// Builds an HTTP/1.1 response header from status, content type, content length, optional extra headers, and adds a keep-alive directive
string buildHttpHeader(const string &status, const string &contentType, size_t contentLength, const string &extraHeaders = "") {
    return "HTTP/1.1 " + status + "\r\n" +
           "Content-Type: " + contentType + "\r\n" +
           "Content-Length: " + to_string(contentLength) + "\r\n" +
           extraHeaders +
		   "Connection: keep-alive\r\n\r\n";
}

// Returns a complete HTTP/1.1 response by prepending the headers to the response body
string httpResponse(const string &status, const string &body, const string &contentType = "text/html", const string &extraHeaders = "") {
    return buildHttpHeader(status, contentType, body.size(), extraHeaders) + body;
}

// Parses the request headers to find and return the 'session_id' cookie value (or empty if missing)
string extractSessionId(const string &request) {
    size_t cookiePos = request.find("Cookie:");
    if (cookiePos != string::npos) {
        size_t sessionStart = request.find("session_id=", cookiePos);
        if (sessionStart != string::npos) {
            size_t end = request.find(";", sessionStart);
            string cookie = (end != string::npos) ?
                            request.substr(sessionStart + 11, end - sessionStart - 11) :
                            request.substr(sessionStart + 11);
            if(cookie != ""){
            	cookie = cookie.substr(0,cookie.find("\r"));
            }
            return cookie;
        }
    }
    return "";
}

// Retrieves the username for a session ID by looking up the session store and querying it via gRPC
string getUsernameFromSession(const string &sessionId) {
	MasterNodeClient master(
		grpc::CreateChannel("127.0.0.1:10000", grpc::InsecureChannelCredentials()));
	string worker = master.Lookup("session-ids");
	if(worker.substr(0,3) == "ERR"){
		return "";
	}
	KeyValueStoreClient client(
		grpc::CreateChannel(worker, grpc::InsecureChannelCredentials()));
	string username = client.Get("session-ids", sessionId);
	if(username.substr(0,3) == "ERR"){
		return "";
	}
	return username;
}

// Generates a unique session ID for a user, stores it in the session-ids store, and updates the active-users list
string generateSessionCookie(const string username) {
	// lookup via master
	MasterNodeClient master(
		grpc::CreateChannel("127.0.0.1:10000", grpc::InsecureChannelCredentials()));
	string worker = master.Lookup("session-ids");

	if(worker.substr(0,3) == "ERR"){
		return "error";
	}
	KeyValueStoreClient client(
		grpc::CreateChannel(worker, grpc::InsecureChannelCredentials()));
	string sid = to_string(rand());
	while(client.Get("session-ids",sid) != "" && client.Get("session-ids",sid).substr(0,3) != "ERR"){
		sid = to_string(rand());
	}
	string activeUsers = client.Get("session-ids","active-users");
	if(activeUsers.substr(0,3) != "ERR"){
		istringstream ss(activeUsers);
		string userinfo;
		bool found = false;
		string found_sid;
		string updatedActiveUsers = "";
		updatedActiveUsers = activeUsers +";"+username+":"+sid+";";
		client.Put("session-ids", sid, username);
		client.CPut("session-ids","active-users", activeUsers, updatedActiveUsers);
	} else {
		string updatedActiveUsers = "";
		updatedActiveUsers += username+":"+sid+";";
		client.Put("session-ids", sid, username);
		client.Put("session-ids","active-users",updatedActiveUsers);
	}
	return sid;
}

// Parses the "Connection" header from the request and returns its value (defaults to "close" if absent)
string extractConnectionHeader(const string &request) {
    size_t connPos = request.find("Connection:");
    if (connPos != string::npos) {
        size_t start = request.find_first_not_of(" ", connPos + 11);
        size_t end = request.find("\r\n", start);
        if (start != string::npos && end != string::npos) {
            return request.substr(start, end - start);
        }
    }
    return "close";
}


// Read up to MAX_FILE_SIZE from the socket after headers
string readRequestBody(int clientSock, const string &req, size_t headerEnd){
    size_t contentLen=0;
    auto cl = req.find("Content-Length:");
    if(cl!=string::npos){
        auto start = req.find_first_of("0123456789",cl);
        auto end   = req.find("\r\n",start);
        contentLen = stoi(req.substr(start,end-start));
    }
    if(contentLen>MAX_FILE_SIZE) contentLen=MAX_FILE_SIZE;

    // already in req
    string body = req.substr(headerEnd+4);
    size_t got = body.size();
    while(got<contentLen){
        char buf[8192];
        int n = read(clientSock,buf,sizeof(buf));
        if(n<=0) break;
        body.append(buf,n);
        got += n;
    }
    return body;
}

// simple URL‐param parser; also decodes "%2F"→"/"
unordered_map<string,string> parseQueryParams(string q){
    unordered_map<string,string> m;
    for(size_t p=0;p<q.size();){
        auto f = q.find("%2F",p);
        if(f==string::npos) break;
        q.replace(f,3,"/");
        p=f+1;
    }
    istringstream ss(q);
    string kv;
    while(getline(ss,kv,'&')){
        auto eq=kv.find('=');
        if(eq!=string::npos){
            m[kv.substr(0,eq)] = kv.substr(eq+1);
        }
    }
    return m;
}

string get_content_type(const string &path){
    auto dot = path.find_last_of('.');
    if(dot!=string::npos){
        auto ext = path.substr(dot);
        auto it = mime_types.find(ext);
        if(it!=mime_types.end()) return it->second;
    }
    return "application/octet-stream";
}

// Validates the user session, locates the email backend via gRPC, forwards the request to EmailService, and wraps the result in an HTTP response.
string handleEmailService(string session_id, string request, string type, string queryParams, string data) {

	string username = getUsernameFromSession(session_id);
    if (username.empty()) {
        return httpResponse("401 Unauthorized", "<html><body>Please login to access emails.</body></html>");
    }

    string master_address("127.0.0.1:10000");
	MasterNodeClient masterclient(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(master_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
	string worker = masterclient.Lookup(username+"-email");  // worker 4
	if(worker.substr(0,3) == "ERR"){
		return httpResponse("503 Service Unavailable","<html><body>Servers down, sorry for the inconvenience!</body></html>");
	}
	cout << "Worker address "<<worker << endl;
	string worker_address(worker);
	KeyValueStoreClient client(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(worker_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));

    KeyValueStoreClient* kvClientPtr = new KeyValueStoreClient(
        grpc::CreateChannel(worker_address, grpc::InsecureChannelCredentials()));
    EmailService emailService(kvClientPtr);

    string response = emailService.handleRequest(request, type, queryParams, data);

    if (response.substr(0,3) == "ERR") {
        string content = "<html><body><h1>"+response+"</h1></body></html>";
        return httpResponse("400 Bad Request", content);
    }

    return httpResponse("200 OK", response);
}

string get_file_content(const string filename) {
    ifstream file(filename);
    if (!file) {
        return "";
    }
    stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Gracefully shuts down the server: notify all clients, close sockets, and exit
void server_shutdown(int){
	string res = "HTTP/1.1 503 Server shutting down\r\n";
	pthread_mutex_lock(&client_mutex);

	for(int client:sockets_active){
		send(client, res.c_str(), res.size(), 0);
		v_logging("["+ to_string(client) + "] S: "+res);
		close(client);
	}
	sockets_active.clear();
	buffer_clients.clear();
	pthread_mutex_unlock(&client_mutex);
	close(socket_server);
	exit(0);
}

// Closes a client socket, removes its buffer and descriptor from active lists, and logs the disconnection
void remove_client_connection(int client){
	close(client);
	pthread_mutex_lock(&client_mutex);
	buffer_clients.erase(client);
	vector<int> socket;
	int i;
	for(i=0;i<sockets_active.size();i++){
		if(sockets_active[i] != client){
			socket.push_back(sockets_active[i]);
		}
	}
	sockets_active = socket;
	pthread_mutex_unlock(&client_mutex);
	v_logging("["+ to_string(client) + "] Connection closed");
}

// Logs out a user by deleting their session ID and updating the active-users list
void handleLogout(const string sid, const string username){
	// lookup via master
	MasterNodeClient master(
		grpc::CreateChannel("127.0.0.1:10000", grpc::InsecureChannelCredentials()));
	string worker = master.Lookup("session-ids");
	if(worker.substr(0,3) == "ERR"){
		return;
	}
	KeyValueStoreClient client(
		grpc::CreateChannel(worker, grpc::InsecureChannelCredentials()));

	string activeUsers = client.Get("session-ids","active-users");
	istringstream ss(activeUsers);
	string userinfo;
	bool found = false;
	string updatedActiveUsers = "";
	while (getline(ss, userinfo, ';')) {
		size_t eqPos = userinfo.find(':');
		if (eqPos != string::npos) {
			string key = userinfo.substr(0, eqPos);
			string value = userinfo.substr(eqPos + 1);
			if(sid == value){
				found = true;
			}
		}
		if(!found){
			updatedActiveUsers += userinfo+";";
		}
	}
	client.Delete("session-ids",sid);
	client.CPut("session-ids","active-users",activeUsers,updatedActiveUsers);
}

// Registers default user credentials in the key-value store via the master node
void dummySetUp(string port) {
	string master_address("127.0.0.1:10000");
	MasterNodeClient masterclient(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(master_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
	string worker = masterclient.Lookup("user1-userinfo");  // worker 4

	if(worker.substr(0,3) == "ERR"){
		return ;
	}
	string worker_address(worker);
	KeyValueStoreClient client1(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(worker_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
    client1.Put("user1-userinfo", "password", "pass1");
    string worker2 = masterclient.Lookup("user2-userinfo");  // worker 4

    if(worker2.substr(0,3) == "ERR"){
		return ;
	}
	string worker_address2(worker2);
	KeyValueStoreClient client2(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(worker_address2,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
    client2.Put("user2-userinfo", "password", "pass2");

	string worker3 = masterclient.Lookup("admin-userinfo");  // worker 4

	if(worker3.substr(0,3) == "ERR"){
		return;
	}
	cout << "Worker address " << worker3 << endl;
	string worker_address3(worker3);
	KeyValueStoreClient client3(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(worker_address3,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
    client3.Put("admin-userinfo", "password", "admin");
}

// Handles a single client: reads and parses HTTP requests, routes them to the appropriate services, and returns responses until the connection closes
void* client_connection(void* arg){
	/*Server sends command response to and receives command from client
	with debug (-v) logs on the server end*/

	int client = *(int *) arg;
	delete (int*) arg;
	string init_message = "Server ready\r\n";
	v_logging("["+ to_string(client) + "] S: " + init_message);
	int buffer_size = 2000;
	char buffer[buffer_size];
	bool receiving_data = false;
	string path = "";
	string version;
	string method;
	string header;
	StorageService storageService;
	UserAccountService userService;
	AdminConsoleService adminService;
	// bool keep_alive = true;
	// dummySetUp("10001");

	bool keepAlive = true;

	while(keepAlive){
		ssize_t recv_bytes = recv(client, buffer, buffer_size-1, 0);
		if(recv_bytes <= 0){
			break;
		}
		buffer[recv_bytes] = '\0';
		pthread_mutex_lock(&client_mutex);
		buffer_clients[client] += buffer;
		string request(buffer);
		pthread_mutex_unlock(&client_mutex);
		string res;
		v_logging("["+ to_string(client) + "] C: " + request);

		string keep_Alive = extractConnectionHeader(request);
		if(keep_Alive != "keep-alive"){
			keepAlive = false;
			break;
		}
		bool vlogs = false;
		bool isHead = false;
		if(request.find("HEAD /") != string::npos){
			isHead = true;
		}

		if(request.find("POST /credentials") != string::npos || request.find("HEAD /credentials") != string::npos){
			v_logging("["+ to_string(client) + "] C: " + "POST /credentials request");
			// parse body
			string body = request.substr(request.find("\r\n\r\n")+4);
			auto params = parseQueryParams(body);
			string u = params["username"], p = params["password"];
			if(userService.authenticate(u,p)){
				// issue cookie
				string sid = generateSessionCookie(u);

				// dashboard page
				string html;
				if(sid == "error"){
					res = httpResponse("503 Service Unavailable","<html><body>Servers down, sorry for the inconvenience!</body></html>");
				} else if (u == "admin") {
					string hdr = 
					"Set-Cookie: session_id=" + sid + "; Path=/; HttpOnly\r\n"
					"Location: /admin/dashboard\r\n";
					res = buildHttpHeader("302 Found", "text/html", 0, hdr);
				} else {
					html = R"(
						<!DOCTYPE html><html><head><title>Dashboard</title></head>
						<body><h1>Welcome )"+u+R"(</h1>
						<nav>
						  <a href="/email.html">Email</a> |
						  <a href="/storage">Storage</a>
						</nav>
						</body></html>
						)";
					string hdr = "Set-Cookie: session_id="+sid+"; Path=/; HttpOnly\r\n";
					res = buildHttpHeader("200 OK","text/html",html.size(),hdr) + html;
				}
			} else {
				string html = "<html><body><h1>Invalid credentials</h1>"
							  "<a href='/login.html'>Try again</a></body></html>";
				res = httpResponse("401 Unauthorized", html);
			}
		} else if (request.find("POST /signup") != string::npos || request.find("HEAD /signup") != string::npos){
			v_logging("["+ to_string(client) + "] C: " + "POST /signup request");
			string body = request.substr(request.find("\r\n\r\n")+4);
			auto params = parseQueryParams(body);
			string u=params["username"], p=params["password"];
			if(userService.signup(u,p)){
				string html = "<html><body><h1>Signed up!</h1>"
							  "<a href='/login.html'>Login</a></body></html>";
				if(isHead){
					res = buildHttpHeader("200 OK","text/html",html.size());
				} else {
					res = httpResponse("200 OK",html);
				}
			} else {
				string html = "<html><body><h1>User exists</h1>"
							  "<a href='/login.html'>Login</a></body></html>";
				res = httpResponse("409 Conflict",html);
			}
		} else if (request.find("POST /changepassword") != string::npos || request.find("HEAD /changepassword") != string::npos){
			v_logging("["+ to_string(client) + "] C: " + "POST /changepassword request");
			string body = request.substr(request.find("\r\n\r\n")+4);
			auto p        = parseQueryParams(body);
			string u      = p["username"];
			string oldPwd = p["old_password"];
			string newPwd = p["new_password"];
			int response = userService.changePassword(u, oldPwd, newPwd);
			if(response==0){
				string html = "<html><body><h1>Password changed</h1>"
							  "<a href='/login.html'>Login</a></body></html>";
				res = isHead ? buildHttpHeader("200 OK","text/html",html.size()) : httpResponse("200 OK",html);
			} else if(response==1){
				string error = "<html><body>User not found</body></html>";
				res = isHead ? buildHttpHeader("404 Not Found","text/html",error.size()) : httpResponse("404 Not Found",
							"<html><body>User not found</body></html>");
			} else if(response == 2){
				string error = "<html><body>Wrong old password</body></html>";
				res = isHead ? buildHttpHeader("401 Unauthorized","text/html",error.size()) : httpResponse("401 Unauthorized",
							"<html><body>Wrong old password</body></html>");
			}else {
				string error = "<html><body>Servers down, sorry for the inconvenience!</body></html>";
				res = isHead ? buildHttpHeader("503 Service Unavailable","text/html",error.size()) : httpResponse("503 Service Unavailable",
							"<html><body>Wrong old password</body></html>");
			}
		} else if(request.find("GET /logout") != string::npos || request.find("HEAD /logout") != string::npos){
			v_logging("["+ to_string(client) + "] C: " + "GET /logout request");
			string session_id = extractSessionId(request);
			string username = getUsernameFromSession(session_id);
			handleLogout(session_id, username);
			string html = "<html><body><h1>Logged out successfully!</h1>"
						  "<a href='/login.html'>Login</a></body></html>";
			if(isHead){
				res = buildHttpHeader("200 OK","text/html",html.size());
			} else {
				res = httpResponse("200 OK",html);
			}
		} else if(request.find("GET /admin/dashboard") != string::npos){
			string session_id = extractSessionId(request);
			string username = getUsernameFromSession(session_id);
			if (username.empty() || username != "admin") {
				res = httpResponse("401 Unauthorized",
					"<html><body>Restricted Access. Only Admin can view this page.</body></html>");
			} else {
				string html = adminService.home();
				res = httpResponse("200 OK", html);
			}
			v_logging("["+ to_string(client) + "] C: " + "GET /admin/dashboard request");
		} else if(request.find("GET /admin/worker-status") != string::npos){

			string json = adminService.fetchWorkerStatuses();
			res = buildHttpHeader("200 OK", "application/json", json.size()) + json;

			v_logging("["+ to_string(client) + "] C: " + "GET /admin/worker-status request");
		} else if(request.find("GET /admin/backend?worker=") != string::npos){
			size_t workerPos = request.find("worker=");
			int workerId = request[workerPos + 7] - '0';

			size_t tabletPos = request.find("&tablet=");
			if (tabletPos != std::string::npos) {
				int tabletId = request[tabletPos + 8] - '0';
				std::string html = adminService.tabletPage(workerId, tabletId);
				res = httpResponse("200 OK", html);
				v_logging("[" + std::to_string(client) + "] C: GET /admin/backend?worker=...&tablet=... request");
			} else {
				std::string html = adminService.workerPage(workerId);
				res = httpResponse("200 OK", html);
				v_logging("[" + std::to_string(client) + "] C: GET /admin/backend?worker=... request");
			}
		} else if(request.find("POST /admin/toggle") != string::npos){
			string session_id = extractSessionId(request);
			string username = getUsernameFromSession(session_id);
			if (username.empty() || username != "admin") {
				res = httpResponse("401 Unauthorized",
					"<html><body>Restricted Access. Only Admin can view this page.</body></html>");
			} else {
				size_t bodyStart = request.find("\r\n\r\n");
				string postData = (bodyStart != string::npos) ? request.substr(bodyStart + 4) : "";
				auto params = parseQueryParams(postData);
				string workerId = params["workerId"], command = params["action"];
				bool done = adminService.handleToggle(workerId, command);
				if (done) {
					res = httpResponse("200 OK", "");
				} else {
					res = httpResponse("500 Internal Server Error",
						"<html><body>Something went wrong on our end!</body></html>");
				}
			}
			v_logging("["+ to_string(client) + "] C: " + "GET /admin request");
		} else if(request.find("GET /storage") != string::npos || request.find("GET /list") != string::npos){
			string session_id = extractSessionId(request);
			string username = getUsernameFromSession(session_id);
			if (username.empty()) {
				string error = "<html><body>Unauthorized. Please login.</body></html>";
				res = isHead ? buildHttpHeader("401 Unauthorized","text/html",error.size()) : httpResponse("401 Unauthorized",
					"<html><body>Unauthorized. Please login.</body></html>");
			} else {
				res = storageService.list(username, request);
			}
			v_logging("["+ to_string(client) + "] C: " + "GET /storage or /list request");
		} else if (request.find("GET /deleteStorage") == 0 || request.find("HEAD /deleteStorage") == 0) {
			string session_id = extractSessionId(request);
			string username = getUsernameFromSession(session_id);
			if (username.empty()) {
				string error = "<html><body>Unauthorized. Please login.</body></html>";
				res = isHead ? buildHttpHeader("401 Unauthorized","text/html",error.size()) : httpResponse("401 Unauthorized",
					"<html><body>Unauthorized. Please login.</body></html>");
			} else {
			    res = storageService.remove(username, client, request);
			}
			v_logging("["+ to_string(client) + "] C: " + "GET /deleteStorage request");
		} else if (request.find("GET /renameFolder") == 0 || request.find("POST /renameFolder") == 0 || request.find("HEAD /renameFolder") == 0) {
			string session_id = extractSessionId(request);
			string username = getUsernameFromSession(session_id);
			if (username.empty()) {
				string error = "<html><body>Unauthorized. Please login.</body></html>";
				res = isHead ? buildHttpHeader("401 Unauthorized","text/html",error.size()) : httpResponse("401 Unauthorized",
					"<html><body>Unauthorized. Please login.</body></html>");
			} else {
			    res = storageService.renameFolder(username, client, request);
			}
			v_logging("["+ to_string(client) + "] C: " + "/renameFolder request");
		} else if (request.find("GET /rename") == 0 || request.find("POST /rename") == 0 || request.find("HEAD /rename") == 0) {
			string session_id = extractSessionId(request);
			string username = getUsernameFromSession(session_id);
			if (username.empty()) {
				string error = "<html><body>Unauthorized. Please login.</body></html>";
				res = isHead ? buildHttpHeader("401 Unauthorized","text/html",error.size()) : httpResponse("401 Unauthorized",
					"<html><body>Unauthorized. Please login.</body></html>");
			} else {
				res = storageService.renameFile(username, client, request);
			}
			v_logging("["+ to_string(client) + "] C: " + "/rename file request");
		} else if (request.find("POST /moveFile") == 0 || request.find("GET /moveFile") == 0) {
			string session_id = extractSessionId(request);
			string username = getUsernameFromSession(session_id);
			if (username.empty()) {
				res = httpResponse("401 Unauthorized",
					"<html><body>Unauthorized. Please login.</body></html>");
			} else {
				res = storageService.moveFile(username, client, request);
			}
			v_logging("["+ to_string(client) + "] C: " + "/moveFile file request");
		} else if (request.find("POST /moveFolder") == 0 ||
			         request.find("GET /moveFolder")  == 0) {
			    // Extract session and username
			    string session_id = extractSessionId(request);
			    string username   = getUsernameFromSession(session_id);
			    if (username.empty()) {
			        res = httpResponse(
			            "401 Unauthorized",
			            "<html><body>Unauthorized. Please login.</body></html>"
			        );
			    } else {
			        // Delegate to webStorageMoveFolder handler
			        res = storageService.moveFolder(
			            username,
			            client,     // socket fd
			            request
			        );
			    }
			    v_logging("["+ to_string(client) + "] C: " + "/moveFolder folder request");
		} else if(request.find("POST /createSubFolder") != string::npos || request.find("HEAD /createSubFolder") != string::npos){
			string session_id = extractSessionId(request);
			string username = getUsernameFromSession(session_id);
			if (username.empty()) {
				string error = "<html><body>Unauthorized. Please login.</body></html>";
				res = isHead ? buildHttpHeader("401 Unauthorized","text/html",error.size()) : httpResponse("401 Unauthorized",
					"<html><body>Unauthorized. Please login.</body></html>");
			} else {
				res = storageService.createFolder(username, client, request);
			}
			v_logging("["+ to_string(client) + "] C: " + "POST /createSubFolder request");
		} else if(request.find("POST /upload") != string::npos || request.find("HEAD /upload") != string::npos){
			string session_id = extractSessionId(request);
			string username = getUsernameFromSession(session_id);
			if (username.empty()) {
				string error = "<html><body>Unauthorized. Please login.</body></html>";
				res = isHead ? buildHttpHeader("401 Unauthorized","text/html",error.size()) : httpResponse("401 Unauthorized",
					"<html><body>Unauthorized. Please login.</body></html>");
			} else {
				res = storageService.upload(username, client, request);
			}
			v_logging("["+ to_string(client) + "] C: " + "POST /upload request");
			vlogs = true;
			v_logging("["+ to_string(client) + "] S: "+res.substr(0,res.find("\r\n\r\n")));
		} else if(request.find("GET /download") != string::npos || request.find("HEAD /download") != string::npos){
			string session_id = extractSessionId(request);
			string username = getUsernameFromSession(session_id);
			if (username.empty()) {
				string error = "<html><body>Unauthorized. Please login.</body></html>";
				res = isHead ? buildHttpHeader("401 Unauthorized","text/html",error.size()) : httpResponse("401 Unauthorized",
					"<html><body>Unauthorized. Please login.</body></html>");
			} else {
				string result = storageService.download(username, client, request);
				if(result != "Transfer" and result != "Binary"){
					res = result;
				}
			}
			v_logging("["+ to_string(client) + "] C: " + "GET /download request");
			vlogs = true;
			v_logging("["+ to_string(client) + "] S: "+res.substr(0,res.find("\r\n\r\n")));
		} else if(request.find("GET /email") != string::npos && request.find("GET /email.html") == string::npos){
			v_logging("["+ to_string(client) + "] C: " + "GET /email request");
			size_t first_line_end = request.find("\r\n");
			string req_line = request.substr(0, first_line_end);
			// Finds the URL between the first and second space.
			size_t first_space = req_line.find(" ");
			size_t second_space = req_line.find(" ", first_space + 1);
			string url = req_line.substr(first_space + 1, second_space - first_space - 1);
			// If there's a query string, it is extracted.
			string queryStr;
			size_t qpos = url.find("?");
			if(qpos != string::npos) {
				queryStr = url.substr(qpos + 1);
			}
			else {
				queryStr = "";
			}
			string session_id = extractSessionId(request);
			res = handleEmailService(session_id,"/email", "GET", queryStr, "");
		}
		else if(request.find("/delete") != string::npos) {
			v_logging("["+ to_string(client) + "] C: POST /delete request");
			// Extract query parameters from the request line.
			size_t first_line_end = request.find("\r\n");
			string req_line = request.substr(0, first_line_end);
			size_t first_space = req_line.find(" ");
			size_t second_space = req_line.find(" ", first_space + 1);
			string url = req_line.substr(first_space + 1, second_space - first_space - 1);
			string queryStr;
			size_t qpos = url.find("?");
			if(qpos != string::npos) {
				 queryStr = url.substr(qpos + 1);
			} else {
				 queryStr = "";
			}
			string session_id = extractSessionId(request);
			res = handleEmailService(session_id,"/delete", "POST", queryStr, "");
		}
		else if(request.find("POST /send") != string::npos) {
			v_logging("["+ to_string(client) + "] C: POST /send request");
			// For POST requests, the body starts after the empty line (\r\n\r\n)
			size_t bodyStart = request.find("\r\n\r\n");
			string postData = (bodyStart != string::npos) ? request.substr(bodyStart + 4) : "";
			string session_id = extractSessionId(request);
			res = handleEmailService(session_id,"/send", "POST", "", postData);
		}
		else if(request.find("GET /compose") != string::npos) {
			v_logging("["+ to_string(client) + "] C: GET /compose request");
			string session_id = extractSessionId(request);
			size_t first_line_end = request.find("\r\n");
			string req_line = request.substr(0, first_line_end);
			size_t first_space = req_line.find(" ");
			size_t second_space = req_line.find(" ", first_space + 1);
			string url = req_line.substr(first_space + 1, second_space - first_space - 1);
			
			string queryStr;
			size_t qpos = url.find("?");
			if (qpos != string::npos) {
				queryStr = url.substr(qpos + 1);
			} else {
				queryStr = "";
			}
			
			res = handleEmailService(session_id,"/compose", "GET", queryStr, "");
		} else if(request.find("/inbox") != string::npos || request.find("/email.html") != string::npos) {
			v_logging("["+ to_string(client) + "] C: GET /inbox request");
			size_t first_line_end = request.find("\r\n");
			string req_line = request.substr(0, first_line_end);
			size_t first_space = req_line.find(" ");
			size_t second_space = req_line.find(" ", first_space + 1);
			string url = req_line.substr(first_space + 1, second_space - first_space - 1);
			string session_id = extractSessionId(request);
			string queryStr;
			string username = getUsernameFromSession(session_id);
			size_t qpos = url.find("?");
			if(qpos != string::npos) {
				 queryStr = url.substr(qpos + 1);
				 if(queryStr.find("username=") == string::npos) {
					 queryStr += "&username="+username;
				 }
			} else {
				 queryStr = "username="+username;
			}
			
			res = handleEmailService(session_id, "/inbox", "GET", queryStr, "");
		}
		else if(request.find("GET /reply") != std::string::npos) {
			v_logging("[" + to_string(client) + "] C: GET /reply request");
			size_t first_line_end = request.find("\r\n");
			string req_line = request.substr(0, first_line_end);
			size_t first_space = req_line.find(" ");
			size_t second_space = req_line.find(" ", first_space + 1);
			string url = req_line.substr(first_space + 1, second_space - first_space - 1);
			
			// Extract query string from URL
			string queryStr;
			size_t qpos = url.find("?");
			if(qpos != string::npos) {
				 queryStr = url.substr(qpos + 1);
			} else {
				 queryStr = "";
			}
			string session_id = extractSessionId(request);
			res = handleEmailService(session_id, "/reply", "GET", queryStr, "");
		}
		else if(request.find("GET /forward") != std::string::npos) {
			v_logging("[" + to_string(client) + "] C: GET /forward request");
			size_t first_line_end = request.find("\r\n");
			string req_line = request.substr(0, first_line_end);
			size_t first_space = req_line.find(" ");
			size_t second_space = req_line.find(" ", first_space + 1);
			string url = req_line.substr(first_space + 1, second_space - first_space - 1);
			
			// Extract query string from URL.
			string queryStr;
			size_t qpos = url.find("?");
			if(qpos != std::string::npos) {
				 queryStr = url.substr(qpos + 1);
			} else {
				 queryStr = "";
			}
			string session_id = extractSessionId(request);
			// Dispatch to EmailService's forward rendering function.
			res = handleEmailService(session_id, "/forward", "GET", queryStr, "");
		} else if (request.find("HEAD /health") != string::npos) {
			res = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n";
		} 
		else if (request.find("POST /admin/frontend-toggle") != string::npos) {
			string session_id = extractSessionId(request);
			string username = getUsernameFromSession(session_id);
			if (username.empty() || username != "admin") {
				res = httpResponse("401 Unauthorized",
					"<html><body>Restricted Access. Only Admin can view this page.</body></html>");
			}
			else {
				string postData = readRequestBody(client, request, request.find("\r\n\r\n"));
				cerr << "[DEBUG] Received frontend-toggle POST with body: " << postData << endl;
				auto params = parseQueryParams(postData);
				string port = params["port"];
				string action = params["action"];
			
				bool result = adminService.handleFrontendToggle(port, action);
				if (result) {
					res = httpResponse("200 OK", "Success");
				} else {
					res = httpResponse("500 Internal Server Error", "<html><body>Failed to toggle frontend.</body></html>");
				}
			}
		}					
		else if(request.find("GET") != string::npos){
			v_logging("["+ to_string(client) + "] C: " + "GET request");
			string req = request.substr(0, request.find("\r\n"));
			vector<string> get_data;
			stringstream ss(req);
			string val;
			while(getline(ss, val, ' ')){
				get_data.push_back(val);
			}
			method = get_data[0];
			if(get_data[1].find("/") != string::npos){
				path = get_data[1].substr(get_data[1].find("/"));
			} else {
				path = "/";
			}
			version = get_data[2];
			header = request.substr(request.find("\r\n")+2,request.find("\r\n\r\n"));
			if (path == "/") path = "/index.html";
			path = "../frontend/web_pages" + path;
			string content = get_file_content(path);
			if (content.empty()) {
				res = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
			} else {
				res = "HTTP/1.1 200 OK\r\n";
				res += "content-type: " + get_content_type(path) + "\r\n";
				res += "content-length: "+ to_string(content.length()) +"\r\nConnection: keep-alive\r\n\r\n";
				res += content;
			}
		}else if (request.find("HEAD") != string::npos) {
		    v_logging("["+ to_string(client) + "] C: HEAD request");

		    string req = request.substr(0, request.find("\r\n"));
		    vector<string> head_data;
		    stringstream ss(req);
		    string val;
		    while (getline(ss, val, ' ')) {
		        head_data.push_back(val);
		    }

		    method = head_data[0];
		    path = (head_data[1].find("/") != string::npos) ? head_data[1] : "/";
		    version = head_data[2];

		    if (path == "/") path = "/index.html";
		    string full_path = "../frontend/web_pages" + path;
		    string content = get_file_content(full_path);

		    if (content.empty()) {
		        res = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
		    } else {
		        res = "HTTP/1.1 200 OK\r\n";
		        res += "Content-Type: " + get_content_type(full_path) + "\r\n";
		        res += "Content-Length: " + to_string(content.length()) + "\r\nConnection: keep-alive\r\n\r\n";
		    }
		} else {
			res = "HTTP/1.1 405 Method Not Allowed\r\nConnection: keep-alive\r\n\r\n";
		}
		request.clear();
		send(client, res.c_str(), res.size(), 0);
		if(!vlogs){
			v_logging("["+ to_string(client) + "] S: "+res);
		}
	}
	remove_client_connection(client);
	return nullptr;
}

// Entry point: parse CLI options, initialize the server socket, listen on the port, and spawn a thread for each incoming client
int main(int argc, char *argv[])
{
	int port = 8080;
	int i;
	for(i=1;i<argc; i++){
		string arg = argv[i];
		if(arg == "-p" && i+1 <argc){
			port = stoi(argv[++i]);
		} else if(arg == "-v"){
			v = true;
		}
	}

	signal(SIGINT, server_shutdown); //To disconnect all clients when server is abruptly interrupted by CTRL+C

	socket_server = socket(AF_INET, SOCK_STREAM, 0); //Creating stream socket
	if(socket_server == -1){
		perror("Creation of socket failed.");
		return 1;
	}

	sockaddr_in server_address{};
	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = INADDR_ANY;
	server_address.sin_port = htons(port);

	if(bind(socket_server, (struct sockaddr*)&server_address, sizeof(server_address)) == -1){
		perror("Binding to port failed");
		return 1;
	}

	if(listen(socket_server, 100) == -1){
		perror("Listen failed");
		return 1;
	}

	cout<<"Server running on port "<<port<<endl;

	while(true){
		sockaddr_in client_address{};
		socklen_t client_size = sizeof(client_address);
		int client = accept(socket_server, (struct sockaddr*)&client_address, &client_size); //Accepting incoming connection to client
		if(client == -1){
			perror("Accepting client failed");
			continue;
		}
		v_logging("["+ to_string(client) + "] New connection");
		pthread_mutex_lock(&client_mutex);
		sockets_active.push_back(client);
		pthread_mutex_unlock(&client_mutex);

		pthread_t thread;
		int* client_pointer = new int(client);
		pthread_create(&thread, nullptr, client_connection, client_pointer);
		pthread_detach(thread);
	}
  return 0;
}
