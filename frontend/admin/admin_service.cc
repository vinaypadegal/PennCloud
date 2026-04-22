#include "admin_service.h"
#include <sstream>
#include <grpcpp/grpcpp.h>
#include "../backend/kvs_client.h"
#include "../build/master.grpc.pb.h"
#include <sstream>
#include <thread>
#include <chrono>
#include <vector>
#include <../utils/serializer.h>

using grpc::Status;

AdminConsoleService::AdminConsoleService() {
    string masterAddress = "127.0.0.1:10000";
    masterClient_ = std::make_unique<MasterNodeClient>(
        grpc::CreateChannel(masterAddress, grpc::InsecureChannelCredentials())
    );

    for (int i = 1; i <= 6; ++i) {
        string addr = "127.0.0.1:1000" + to_string(i);
        workerClients_.emplace_back(std::make_unique<KeyValueStoreClient>(
            grpc::CreateChannel(addr, grpc::InsecureChannelCredentials())
        ));

    }
}

AdminConsoleService::~AdminConsoleService() {}


bool AdminConsoleService::shutdownServer(int workerId) {
    auto client = workerClients_[workerId - 1].get();
    string res = client->AdminConsole("admin", "SHUTDOWN");
    if (res.substr(0, 3) == "ERR") {
        return false;
    }
    return true;
}


bool AdminConsoleService::restartServer(int workerId) {
    auto client = workerClients_[workerId - 1].get();
    string res = client->AdminConsole("admin", "RESTART");
    if (res.substr(0, 3) == "ERR") {
        return false;
    }
    return true;
}


bool AdminConsoleService::handleToggle(string worker, string command) {
    cout << worker << " " << command << endl;
    int workerId = stoi(worker);
    if (command == "shutdown") {
        return shutdownServer(workerId);
    } else {
        return restartServer(workerId);
    }
}


string AdminConsoleService::fetchWorkerStatuses() {
    std::string json = R"({"workers":[)";
    for (int i = 1; i <= 6; ++i) {
        bool isAlive = (masterClient_->Alive(i) == "Alive");
        json += "{\"id\":" + std::to_string(i) + ",\"alive\":" + (isAlive ? "true" : "false") + "}";
        if (i != 6) json += ",";
    }
    json += "],";
//    json += R"("frontends":[)";
//    for (int i = 1; i <= 3; ++i) {
//        int port = 8080 + i;
//        std::string addr = "127.0.0.1:" + std::to_string(port);
//        // bool alive = checkHttpAlive(addr);
//        bool alive = false;
//
//        json += "{\"id\":" + std::to_string(i) +
//                ",\"port\":" + std::to_string(port) +
//                ",\"alive\":" + (alive ? "true" : "false") + "}";
//
//        if (i != 3) json += ",";
//    }
//    json += "]}";
    string result = checkHttpAlive("127.0.0.1:8080");
    json += R"("frontends":)";
	json += result.substr(result.find("\r\n\r\n")+4);
    json += "}";
    return json;
}

string AdminConsoleService::checkHttpAlive(const string &hostport) {
    try {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(stoi(hostport.substr(hostport.find(":") + 1)));
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        if (connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0) return "false";
//        cerr << "[Admin] Sending health check to " << hostport << endl;
        string request = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
        send(sock, request.c_str(), request.size(), 0);

        char buffer[1024] = {0};
        int bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
        close(sock);
        return buffer;
    } catch (...) {
        return "false";
    }
}

bool AdminConsoleService::handleFrontendToggle(const string &port, const string &action) {
    string cmd;
    if (action == "shutdown") {
        cmd = "pkill -f 'http_server.*-p " + port + "'";
    } else if (action == "restart") {
        cmd = "./http_server -p " + port + " &";
    } else {
        return false;
    }

    int result = system(cmd.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    cerr << "[Admin] Executed: " << cmd << " → result code: " << result << endl;

    return result == 0 || result == 15 || result == 256;
}

static vector<string> splitStringAdmin(const string &str, char delimiter) {
	vector<string> tokens;
	size_t start = 0, pos = 0;
	while ((pos = str.find(delimiter, start)) != string::npos) {
		tokens.push_back(str.substr(start, pos - start));
		start = pos + 1;
	}
	tokens.push_back(str.substr(start));
	return tokens;
}

string AdminConsoleService::home() {
    vector<bool> workersAlive;
    for (int worker = 1; worker <= 6; ++worker) {
        bool status = (masterClient_->Alive(worker) == "Alive") ? true : false;
        workersAlive.push_back(status);
    }

    vector<bool> frontendStatus;
    string frontends = checkHttpAlive("127.0.0.1:8080");
    frontends = frontends.substr(frontends.find("\r\n\r\n")+4);
    vector<string> frontendUpdate = splitStringAdmin(frontends, ',');
    int i = 1;
	bool alive;
    for (string &fe : frontendUpdate) {
		if(fe.find("alive") != string::npos && fe.find("true") != string::npos){
			alive = true;
			frontendStatus.push_back(alive);
			i++;
		} else if (fe.find("alive") != string::npos && fe.find("false") != string::npos){
			alive = false;
			frontendStatus.push_back(alive);
			i++;
		}
	}


    std::string html = R"(
        <!DOCTYPE html>
        <html>
        <head>
          <title>Admin Dashboard</title>
          <style>
            body { font-family: Arial; padding: 20px; }
            .worker { display: flex; justify-content: space-between; padding: 6px; border: 1px solid #ccc; margin: 4px 0; align-items: center; }
            .status { font-weight: bold; padding: 0 10px; }
            .alive { color: green; }
            .dead { color: red; }
            button { padding: 4px 10px; }
          </style>
        </head>
        <body>
          <h1>Admin Dashboard</h1>
          <h2>Front-End Workers</h2>)";
    	  int id = 1;
          for (const auto alive : frontendStatus) {
            int port = 8080 + id;
            html += "<div class='worker'>";
            html += "<span>Front-End " + to_string(id) + "</span>";
            html += "<span>(127.0.0.1:" + to_string(port) + ")</span>";
            html += "<span id=\"frontend-status-" + std::to_string(id) + "\" class=\"status\">...</span>";
            // html += "<form method='POST' action='/admin/frontend-toggle' style='display:inline;'>";
            // html += "<input type='hidden' name='port' value='" + to_string(port) + "' />";
            // html += "<input type='hidden' name='action' value='" + string(alive ? "shutdown" : "restart") + "' />";
            // html += "<button type='submit'>" + string(alive ? "Shutdown" : "Restart") + "</button></form>";
            html += "<button id=\"frontend-btn-" + std::to_string(id) + "\" onclick=\"toggleFrontend(" + to_string(port) + "," + to_string(id) + ")\">...</button>\n";
            html += "</div>";
            id++;
        }
        
        html += R"(
          <h2>Back-End Workers</h2>
        )" + [&]() {
            std::string backendHtml;
            for (int i = 1; i <= 6; ++i) {
                backendHtml +=
                    "<div class=\"worker\">"
                    "<a href=\"/admin/backend?worker=" + std::to_string(i) + "\">Back-End " + std::to_string(i) + "</a>"
                    "<span>(127.0.0.1:1000" + std::to_string(i) + ")</span>\n"
                    "<span id=\"worker-status-" + std::to_string(i) + "\" class=\"status\">...</span>"
                    "<input type=\"hidden\" name=\"workerId\" value=\"" + std::to_string(i) + "\" />"
                    "<input type=\"hidden\" id=\"action-input-" + std::to_string(i) + "\" name=\"action\" value=\"shutdown\" />"
                    "<button id=\"worker-btn-" + std::to_string(i) + "\" onclick=\"toggleWorker(" + std::to_string(i) + ")\">...</button>\n"
                    "</div>\n";
            }
            return backendHtml;
        }() + R"(
          <script>
            function toggleWorker(workerId) {
                const statusEl = document.getElementById(`worker-status-${workerId}`);
                const buttonEl = document.getElementById(`worker-btn-${workerId}`);
                const isAlive = statusEl.classList.contains('alive');
                const action = isAlive ? 'shutdown' : 'restart';
        
                fetch('/admin/toggle', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: `workerId=${workerId}&action=${action}`
                })
                .then(response => response.text())
                .then(_ => {
                    // Toggle the UI on success
                    console.log('toggled');
                })
                .catch(err => {
                    alert('Toggle failed');
                    console.error(err);
                });
            }

            function toggleFrontend(port, id) {
                const statusEl = document.getElementById(`frontend-status-${id}`);
				console.log("Fe "+id+" status: "+statusEl);
                const btnEl = document.getElementById(`frontend-btn-${id}`);
                const isAlive = statusEl.classList.contains('alive');
                const action = isAlive ? 'shutdown' : 'restart';

                fetch('/admin/frontend-toggle', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: `port=${port}&action=${action}`
                })
                .then(res => res.text())
                .then(text => {
                    console.log(`[Frontend ${port}] Toggle ${action}: ${text}`);
                    setTimeout(fetchStatus, 500);  // Refresh status
                })
                .catch(err => {
                    console.error(`[Frontend ${port}] Toggle failed`, err);
                    alert(`Toggle failed for frontend ${port}`);
                });
            }



            function fetchStatus() {
              fetch('/admin/worker-status')
                .then(res => res.json())
                .then(data => {
                  data.workers.forEach(worker => {
                    const id = worker.id;
                    const alive = worker.alive;
        
                    const statusElem = document.getElementById('worker-status-' + id);
                    const btnElem = document.getElementById('worker-btn-' + id);
                    const inputElem = document.getElementById('action-input-' + id);
        
                    if (statusElem) {
                      statusElem.textContent = alive ? 'Alive' : 'Dead';
                      statusElem.className = 'status ' + (alive ? 'alive' : 'dead');
                    }
                    if (btnElem) {
                      btnElem.textContent = alive ? 'Shutdown' : 'Restart';
                    }
                    if (inputElem) {
                      inputElem.value = alive ? 'shutdown' : 'restart';
                    }
                  });
                  data.frontends.forEach(fe => {
                        const id = fe.id;
                        const alive = fe.alive;
                        const row = document.querySelector(`#frontend-status-${id}`);
                        if (row) {
                            row.textContent = alive ? "Alive" : "Dead";
                            row.className = "status " + (alive ? "alive" : "dead");
                        }

                        const btn = document.querySelector(`#frontend-btn-${id}`);
                        if (btn) btn.textContent = alive ? "Shutdown" : "Restart";
                    });
                });
            }
        
            setInterval(fetchStatus, 2000);
            window.onload = fetchStatus;
            console.log("Script loaded and fetchStatus registered.");
          </script>


        </body>
        </html>
        )";
        
        
        return html;
}

string AdminConsoleService::workerPage(int workerId) {
    const int NUM_TABLETS = 6;
    auto isTabletStoredHere = [&](int tabletId) {
        int prev1 = ((workerId + NUM_TABLETS - 2) % NUM_TABLETS) + 1;
        int prev2 = ((workerId + NUM_TABLETS - 3) % NUM_TABLETS) + 1;
        return (tabletId == workerId || tabletId == prev1 || tabletId == prev2);
    };

    std::string html = R"(
    <!DOCTYPE html>
    <html>
    <head>
    <title>Worker Tablets</title>
    <style>
        body { font-family: Arial; padding: 20px; }
        .tablet-list { list-style: none; padding: 0; }
        .tablet-item { margin: 10px 0; }
        a { text-decoration: none; color: #007BFF; font-size: 18px; }
        a:hover { text-decoration: underline; }
    </style>
    </head>
    <body>
    <h1>Tablets Stored in Worker )" + std::to_string(workerId) + R"(</h1>
    <ul class="tablet-list">
    )";

    // Dynamically add tablets this worker holds
    for (int tablet = 1; tablet <= NUM_TABLETS; ++tablet) {
        if (isTabletStoredHere(tablet)) {
            html += "<li class='tablet-item'><a href='/admin/backend?worker=" +
                    std::to_string(workerId) + "&tablet=" + std::to_string(tablet) +
                    "'>Tablet " + std::to_string(tablet) + "</a></li>\n";
        }
    }

    html += R"(
    </ul>
    </body>
    </html>
    )";

    return html;
}

string AdminConsoleService::tabletPage(int workerId, int tabletId) {
    const int NUM_TABLETS = 6;
    string binary = workerClients_[workerId - 1]->CopyTablet(tabletId);
    if (binary.substr(0, 3) == "ERR") {
        return "<html><body><h1>Could not load tablet data</h1><br><a href='/admin/dashboard'>Back</a></body></html>";
    }

    unordered_map<string, unordered_map<string, string>> table;
    bool ok = deserializeTablet(table, binary);
    if (!ok) {
        return "<html><body><h1>Deserialization failed</h1><br><a href='/admin/dashboard'>Back</a></body></html>";
    }

    std::ostringstream html;
    html << R"(
    <html>
    <head>
      <title>Tablet View</title>
      <style>
        td { vertical-align: top; white-space: pre-wrap; max-width: 600px; }
        .value-preview { display: block; }
        .value-full { display: none; }
        .toggle-btn { margin-top: 5px; }
      </style>
      <script>
        function toggle(id) {
          const preview = document.getElementById(id + "_preview");
          const full = document.getElementById(id + "_full");
          const btn = document.getElementById(id + "_btn");
          const isCollapsed = full.style.display === "none";
          full.style.display = isCollapsed ? "block" : "none";
          preview.style.display = isCollapsed ? "none" : "block";
          btn.textContent = isCollapsed ? "Show less" : "Show more";
        }
      </script>
    </head>
    <body>
    )";

    html << "<h1>Tablet " << tabletId << " on Worker " << workerId << "</h1>";
    html << "<table border='1'><tr><th>Row</th><th>Column</th><th>Value</th></tr>";

    for (const auto& rowEntry : table) {
        const auto& row = rowEntry.first;
        const auto& cols = rowEntry.second;
        bool first = true;
        for (const auto& colEntry : cols) {
            const auto& col = colEntry.first;
            const auto& val = colEntry.second;

            html << "<tr>";
            if (first) {
                html << "<td rowspan='" << cols.size() << "'>" << row << "</td>";
                first = false;
            }

            html << "<td>" << col << "</td><td>";

            if (val.size() <= 200) {
                html << val;
            } else {
                string id = "cell_" + to_string(rand());
                string preview = val.substr(0, 200);
                html << "<div id='" << id << "_preview' class='value-preview'>" << preview << "...</div>";
                html << "<div id='" << id << "_full' class='value-full'>" << val << "</div>";
                html << "<button id='" << id << "_btn' class='toggle-btn' onclick=\"toggle('" << id << "')\">Show more</button>";
            }

            html << "</td></tr>";
        }
    }

    html << "</table><br><a href='/admin/dashboard'>Back</a></body></html>";
    return html.str();
}
