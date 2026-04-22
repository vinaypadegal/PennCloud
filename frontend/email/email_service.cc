// email_service.cc
// This file implements the EmailService class, which provides the core logic
// for handling email-related operations within the application. It interacts
// with a distributed key-value store (KVStore) to persist email data and metadata.
// The service supports sending emails (both internal and external via SMTP - it uses smtp_server.cc),
// displaying user inboxes and sent email folders, viewing individual emails,
// composing new emails, replying to and forwarding emails, and deleting emails.
// It also handles rendering HTML pages for the user interface.

#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <ctime>
#include <unistd.h>

// Include the gRPC-based KVStore client header
#include "../../backend/kvs_client.h"
#include <grpcpp/grpcpp.h>
#include "../../build/kvstore.grpc.pb.h"
#include "email_service.h"
#include "../../utils/utils.h"
#include "./client_pool.h"

using grpc::Channel;
using grpc::ClientContext;
using namespace std;

// // Helper structure to represent an email (not needed now)
// struct Email {
//     string sender;
//     string recipient;
//     string subject;
//     string timestamp;
//     string body;
//     // Optional: Attachments
// };


// Namespace for local utility functions that are not part of the EmailService class interface
namespace {
    // Checks if an email address is external (i.e., not a "@localhost" address).
    // param -  email The email address to check.
    // return - True if the email is external, false otherwise.
    bool isExternalEmail(const string& email) {
        return email.find("@localhost") == string::npos;
    }

    // Sends an email via a local SMTP server (smtp_server.cc)
    // This function establishes a socket connection to the specified SMTP server,
    // performs the basic SMTP handshake, sends the email headers and body,
    // and then closes the connection.
    bool sendViaSMTP(const string& sender, const string& recipient, const string& subject, const string& body) {
        cerr << "[SMTP] Connecting to local SMTP server on port 2500...\n";
    
        // Create a TCP socket
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            cerr << "[SMTP] Socket creation failed\n";
            return false;
        }
    
        // Prepare the server address structure
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(2500);  // SMTP server port (converted to network byte order)
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");// Localhost IP address
    
        // Connect to the SMTP server
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            cerr << "[SMTP] Failed to connect to localhost:2500\n";
            close(sock);
            return false;
        }
    
        //function to send a line of text to the SMTP server
        auto sendLine = [&](const string& msg) {
            string full = msg + "\r\n"; // SMTP requires lines to end with CRLF
            cerr << "[SMTP >>>] " << msg << endl;
            send(sock, full.c_str(), full.size(), 0);
        };

        // Lambda function to receive a line of text from the SMTP server    
        auto recvLine = [&]() -> string {
            char buf[1024] = {0};
            int bytes = recv(sock, buf, sizeof(buf) - 1, 0);
            if (bytes <= 0) {
                cerr << "[SMTP <<<] ERROR: No response from server\n";
                return "";
            }
            string response(buf);
            cerr << "[SMTP <<<] " << response;
            return response;
        };
    
        // SMTP handshake
        recvLine();  // Receive the server's greeting banner
        sendLine("HELO localhost");     recvLine();
        sendLine("MAIL FROM:<" + sender + ">"); recvLine();
        sendLine("RCPT TO:<" + recipient + ">"); recvLine();
        sendLine("DATA"); recvLine();
    
        // Construct email headers
        time_t now = time(nullptr);
        char datebuf[128];
        strftime(datebuf, sizeof(datebuf), "%a, %d %b %Y %H:%M:%S %z", localtime(&now));
    
        stringstream headers;
        headers << "From: " << sender << "\n"
                << "To: " << recipient << "\n"
                << "Subject: " << subject << "\n"
                << "Date: " << datebuf;
    
        // Send headers + body + terminating dot
        sendLine(headers.str() + "\r\n\r\n" + body + "\r\n.");
        recvLine();
    
        // End SMTP session
        sendLine("QUIT"); recvLine();
        close(sock);
    
        cerr << "[SMTP] Email sent via local SMTP server.\n";
        return true;
    }    
    
}

// Constructor for the EmailService.
// Initializes the KeyValueStoreClient used by this service
EmailService::EmailService(KeyValueStoreClient* client) {
    this->kvClient = client;
}

// Destructor for the EmailService.
// Currently empty, but can be used for cleanup tasks if needed in the future
EmailService::~EmailService() {
    // Cleanup if needed
}
    
// Utility function to append an email ID to the metadata list stored in the "meta" column of a mailbox row
// mailboxRow is the row key (e.g., "inbox_email:user" or "sent_email:user")
// append an email ID (already encrypted)
void EmailService::appendToMetadata(const string &mailboxRow, const string &emailId) {
    cerr << "[EMAIL] Appending to metadata row=" << mailboxRow << ", id=" << emailId << endl;
    // Get a worker client from the pool based on the mailbox row key for data locality
    KeyValueStoreClient* client = ClientPool::getInstance().getWorkerClient(mailboxRow);
    // Retrieve the current metadata string from the "meta" column.
    string currentMeta = client->Get(mailboxRow, "meta");
    string newMeta;
    // If there's an error retrieving the metadata or if it's empty, start a new list with the email ID.
    if (currentMeta.substr(0, 3) == "ERR" || currentMeta.empty()) {
        newMeta = emailId;
    } else {
        // Otherwise, append the new email ID to the existing comma-separated list.
        newMeta = currentMeta + "," + emailId;
    }
    string res = client->Put(mailboxRow, "meta", newMeta);
    cerr << "[EMAIL] Metadata updated → result: " << res << endl;
}

    
// Utility function to remove an email ID from the metadata
// This is used when an email is deleted from the inbox or sent folder.
void EmailService::removeFromMetadata(const string &mailboxRow, const string &emailId) {
    cerr << "[EMAIL] Removing from metadata row=" << mailboxRow << ", id=" << emailId << endl;
    // Get a worker client from the pool.
    KeyValueStoreClient* client = ClientPool::getInstance().getWorkerClient(mailboxRow);
    // Retrieve the current metadata.
    string currentMeta = client->Get(mailboxRow, "meta");
    if (currentMeta.substr(0, 3) == "ERR") return;
    
    // Split the comma-separated metadata string into individual email IDs.
    vector<string> ids;
    istringstream iss(currentMeta);
    string token;
    while (getline(iss, token, ',')) {
        // Add the ID to the vector only if it's not empty and not the ID to be removed.
        if (!token.empty() && token != emailId) {
            ids.push_back(token);
        }
    }

    ostringstream oss;
    for (size_t i = 0; i < ids.size(); i++) {
        if (i > 0) oss << ",";
        oss << ids[i];
    }
    client->Put(mailboxRow, "meta", oss.str());
    cerr << "[EMAIL] New metadata after removal: " << oss.str() << endl;
}

    
// Renders the inbox page for a given user, displaying both received and sent emails.
// It fetches the list of email IDs from the KVStore metadata for both the inbox and sent folders
// and then retrieves the basic email information (sender, subject, date) to display in a table.
string EmailService::renderInbox(const string &username) {
    ostringstream html;
    html << "<html><head><title>Inbox</title>"
         << "<style>"
         << "body { font-family: Arial, sans-serif; background: #f4f4f4; padding: 20px; }"
         << "h1, h2 { color: #333; }"
         << "table { border-collapse: collapse; width: 100%; margin-bottom: 40px; background: #fff; box-shadow: 0 0 10px rgba(0,0,0,0.05); }"
         << "th, td { padding: 12px 15px; border: 1px solid #ddd; }"
         << "th { background-color: #007BFF; color: white; }"
         << "tr:hover { background-color: #f1f1f1; }"
         << "a { text-decoration: none; color: #007BFF; }"
         << "a:hover { text-decoration: underline; }"
         << ".compose-btn { padding: 10px 20px; background: #28a745; color: white; border-radius: 5px; font-weight: bold; text-decoration: none; }"
         << ".compose-btn:hover { background: #218838; }"
         << "</style></head><body>";

    html << "<h1>Welcome, " << username << "</h1>";

    // Inbox Section
    html << "<h2>Inbox</h2>";

    // Construct the row key for the user's inbox metadata.
    string inboxRow = "inbox_email:" + username;
    // Get a worker client for the inbox.
    KeyValueStoreClient* client = ClientPool::getInstance().getWorkerClient(inboxRow);
    string metaList = client->Get(inboxRow, "meta");

    // If there's an error or no emails in the inbox, display a message.
    if (metaList.substr(0, 3) == "ERR" || metaList.empty()) {
        html << "<p>No received emails found.</p>";
    } else {
        // Split the metadata list into individual encrypted email IDs.
        vector<string> ids;
        istringstream iss(metaList);
        string token;
        while (getline(iss, token, ',')) if (!token.empty()) ids.push_back(token);

        html << "<table border='1'><tr><th>Email ID</th><th>From</th><th>Subject</th><th>Date</th><th>Actions</th></tr>";
        for (const string &encId : ids) {
            string decryptedMeta = decryptMetadata(encId);
            auto headers = parseMetadata(decryptedMeta);
            html << "<tr><td>" << encId << "</td><td>" << headers["sender"]
                 << "</td><td>" << headers["subject"] << "</td><td>"
                 << headers["timestamp"] << "</td><td>"
                 << "<a href='/email?id=" << encId << "&folder=inbox&username=" << username << "'>View</a> | "
                 << "<a href='/delete?id=" << encId << "&folder=inbox&username=" << username << "'>Delete</a>"
                 << "</td></tr>";
        }
        html << "</table>";
    }

    // Sent Section
    html << "<h2>Sent Emails</h2>";
    string sentRow = "sent_email:" + username;
    KeyValueStoreClient* client2 = ClientPool::getInstance().getWorkerClient(sentRow);
    string sentMeta = client2->Get(sentRow, "meta");

    if (sentMeta.substr(0, 3)== "ERR" || sentMeta.empty()) {
        html << "<p>No sent emails found.</p>";
    } else {
        vector<string> ids;
        istringstream iss(sentMeta);
        string token;
        while (getline(iss, token, ',')) if (!token.empty()) ids.push_back(token);

        html << "<table border='1'><tr><th>Email ID</th><th>To</th><th>Subject</th><th>Date</th><th>Actions</th></tr>";
        // Iterate through the sent email IDs and display their information.
        for (const string &encId : ids) {
            string decryptedMeta = decryptMetadata(encId);
            auto headers = parseMetadata(decryptedMeta);
            html << "<tr><td>" << encId << "</td><td>" << headers["recipient"]
                 << "</td><td>" << headers["subject"] << "</td><td>"
                 << headers["timestamp"] << "</td><td>"
                 << "<a href='/email?id=" << encId << "&folder=sent&username=" << username << "'>View</a> | "
                 << "<a href='/delete?id=" << encId << "&folder=sent&username=" << username << "'>Delete</a>"
                 << "</td></tr>";
        }
        html << "</table>";
    }

    html << "<br/><a href='/compose?username=" << username << "'>Compose New Email</a></body></html>";
    return html.str();
}

// Views the content of a specific email.
// Retrieves the email body from the KVStore using the provided email ID and folder.
// It also decrypts and parses the metadata to display the email headers.
// The 'folder' parameter tells us whether to use "inbox" or "sent" key format
string EmailService::viewEmail(const string &username, const string &emailId, const string &folder) {
    // Determine the row key based on the folder.
    string rowKey = (folder == "sent") ? "sent_email:" + username : "inbox_email:" + username;
    KeyValueStoreClient* client = ClientPool::getInstance().getWorkerClient(rowKey);
    // Retrieve the email body using the email ID as the column key.
    string body = client->Get(rowKey, emailId);
    if (body.substr(0, 3) == "ERR") {
        return "<html><body><h1>Error:</h1><p>Email not found or failed to load.</p></body></html>";
    }
    // Decrypt and parse the email metadata.
    string decryptedMeta = decryptMetadata(emailId);
    auto headers = parseMetadata(decryptedMeta);

    ostringstream html;
    html << "<html><head><title>Email Details</title>"
         << "<style>"
         << "body { font-family: Arial; background: #f4f4f4; padding: 20px; }"
         << "h1, h2 { color: #333; }"
         << "p { line-height: 1.6; }"
         << "a { color: #007BFF; text-decoration: none; }"
         << "a:hover { text-decoration: underline; }"
         << ".back-btn { margin-top: 20px; display: inline-block; padding: 10px 20px; background: #007BFF; color: white; border-radius: 5px; text-decoration: none; }"
         << ".back-btn:hover { background: #0056b3; }"
         << "</style></head><body>"
         << "<h1>Email Details (" << folder << ")</h1>"
         << "<p><strong>From:</strong> " << headers["sender"] << "</p>"
         << "<p><strong>To:</strong> " << headers["recipient"] << "</p>"
         << "<p><strong>Subject:</strong> " << headers["subject"] << "</p>"
         << "<p><strong>Date:</strong> " << headers["timestamp"] << "</p>"
         << "<p><strong>Body:</strong><br><div style='background:#fff;padding:15px;border:1px solid #ccc;'>" << body << "</div></p>"
         << "<p><a href='/reply?username=" << username << "&id=" << emailId << "&folder=" << folder << "'>Reply</a> | "
         << "<a href='/forward?username=" << username << "&id=" << emailId << "&folder=" << folder << "'>Forward</a></p>"
         << "<a class='back-btn' href='/inbox?username=" << username << "&folder=" << folder << "'>Back to Inbox</a>"
         << "</body></html>";
    return html.str();
}

// Overloaded viewEmail defaults to inbox.
string EmailService::viewEmail(const string &username, const string &emailId) {
    return viewEmail(username, emailId, "inbox");
}

// Renders the "Compose New Email" page, providing a form for the user to enter recipient, subject, and body.
string EmailService::renderCompose(const string &username) {
    std::ostringstream html;
    html << "<html><head><title>Compose Email</title>"
         << "<style>"
         << "body { font-family: Arial; background: #f4f4f4; padding: 20px; }"
         << "form { background: #fff; padding: 20px; border-radius: 5px; box-shadow: 0 0 5px rgba(0,0,0,0.1); }"
         << "input[type=text], textarea { width: 100%; padding: 10px; margin-bottom: 15px; border: 1px solid #ccc; border-radius: 4px; }"
         << "input[type=submit] { background: #28a745; color: white; padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; }"
         << "input[type=submit]:hover { background: #218838; }"
         << ".back-link { margin-top: 20px; display: inline-block; color: #007BFF; }"
         << "</style></head><body>"
         << "<h1>Compose New Email</h1>"
         << "<form method='POST' action='/send'>"
         << "To: <input type='text' name='to'><br>"
         << "Subject: <input type='text' name='subject'><br>"
         << "Body:<br><textarea name='body' rows='10' cols='30'></textarea><br>"
         << "<input type='hidden' name='username' value='" << username << "'>"
         << "<input type='submit' value='Send'>"
         << "</form>"
         << "<a class='back-link' href='/inbox?username=" << username << "'>Back to Inbox</a>"
         << "</body></html>";
    return html.str();
}

// Handles the submission of a new email.
// Parses the POST data to extract recipient, subject, and body.
// Generates a timestamp and metadata for the email, encrypts the metadata,
// stores the email body in the KVStore (in the recipient's inbox and sender's sent folder),
// and updates the metadata lists for both. It also handles sending external emails via SMTP.
string EmailService::sendEmail(const string &postData) {
    std::cerr << "[EmailService] sendEmail received postData: \"" << postData << "\"" << std::endl;
    // Parse the URL-encoded POST data into a map of key-value pairs.
    map<string, string> formData = parseQueryString(postData);
    for (auto &pair : formData) {
        pair.second = urlDecode(pair.second);
    }

    // Extract the recipient, subject, body, and sender username from the form data.
    string recipient = formData["to"];
    string subject = formData["subject"];
    string body = formData["body"];
    string senderUsername = formData["username"];

    // Generate timestamp
    time_t now = time(nullptr);
    char dtBuffer[64];
    strftime(dtBuffer, sizeof(dtBuffer), "%Y-%m-%d %H:%M:%S", localtime(&now));
    string timestamp(dtBuffer);

    // Always generate fresh metadata for new email
    string rawMetadata = createMetadata(senderUsername, recipient, subject, timestamp);
    string encryptedMetadata = encryptMetadata(rawMetadata);

    bool inboxSuccess = false;
    // If the recipient is an external email address, send it via SMTP.
    if (isExternalEmail(recipient)) {
        inboxSuccess = sendViaSMTP(senderUsername, recipient, subject, body);
    } else{
        // If it's an internal email, store it in the recipient's inbox in the KVStore.
        string inboxRow = "inbox_email:" + recipient;
        KeyValueStoreClient* inboxClient = ClientPool::getInstance().getWorkerClient(inboxRow);
        string inboxPutResult = inboxClient->Put(inboxRow, encryptedMetadata, body);
        inboxSuccess = inboxPutResult.substr(0, 3) != "ERR";
        // If storing the email body was successful, append the encrypted metadata (email ID) to the recipient's inbox metadata list.
        if (inboxSuccess) {
            appendToMetadata(inboxRow, encryptedMetadata);
            cerr << "[EMAIL] Stored body for " << recipient << " in inbox. Now appending to metadata..." << endl;
        }
        else{
            cerr << "[EMAIL] Failed to store email body for " << recipient << ": " << inboxPutResult << endl;
        }
    }
    

    // Define mailbox rows
    string sentRow = "sent_email:" + senderUsername;

    KeyValueStoreClient* senderClient = ClientPool::getInstance().getWorkerClient(sentRow);
    // Store email body under the new encryptedMetadata
    string senderResult = senderClient->Put(sentRow, encryptedMetadata, body);
    bool senderSuccess = senderResult.substr(0, 3) != "ERR";
    if (senderSuccess) {
        appendToMetadata(sentRow, encryptedMetadata);
        cerr << "[EMAIL] Stored body for " << senderUsername << " in sent. Now appending to metadata..." << endl;
    }
    else{
        cerr << "[EMAIL] Failed to store in sent_email for " << senderUsername << ": " << senderResult << endl;
    }
    // Update metadata lists with the new email ID
    // Redirect the user back to their inbox with a message indicating success or potential failures.
    ostringstream html;
    html << "<html><head><meta http-equiv='refresh' content='2;url=/inbox?username=" 
     << senderUsername << "'></head><body>";

    if (!inboxSuccess)
        html << "<p style='color:red;'>Warning: Failed to deliver to " << recipient << "</p>";

    if (!senderSuccess)
        html << "<p style='color:red;'>Warning: Email was not stored in your sent folder</p>";

    html << "<p>Email send attempt completed. Redirecting...</p></body></html>";
    return html.str();  
}

// Renders the "Reply to Email" page, pre-filling the recipient and subject based on the original email.
// It retrieves the original email's metadata to get the sender and subject.
std::string EmailService::renderReplyCompose(const std::string &username, const std::string &emailId, const std::string &folder) {
    // Get the original email
    string rowKey = (folder == "sent") ? "sent_email:" + username : "inbox_email:" + username;
    KeyValueStoreClient* client = ClientPool::getInstance().getWorkerClient(rowKey);
    string originalBody = client->Get(rowKey, emailId);
    if (originalBody.substr(0, 3) == "ERR") originalBody = "[Original message not available]";

    // Decrypt metadata to get sender and subject
    string decryptedMeta = decryptMetadata(emailId);
    map<string, string> headers;
    istringstream metaStream(decryptedMeta);
    string pair;
    while(getline(metaStream, pair, ';')) {
        if(pair.empty()) continue;
        size_t colonPos = pair.find(':');
        if(colonPos != string::npos) {
            string key = pair.substr(0, colonPos);
            string value = pair.substr(colonPos + 1);
            headers[key] = value;
        }
    }
    
    // Auto-populate fields
    string replyTo = headers["sender"];
    string originalSubject = headers["subject"];
    string replySubject = (originalSubject.find("Re:") == 0) ? 
                        originalSubject : "Re: " + originalSubject;
    
    // Format reply body
    string quotedBody = "\n\n---------- Original Message ----------\nFrom: " + headers["sender"] +
                         "\nSubject: " + originalSubject + "\n\n" + originalBody;
    
    // Render form with pre-filled values
    ostringstream html;
    html << "<html><head><title>Reply Email</title></head><body>";
    html << "<h1>Reply to Message</h1>";
    html << "<form method='POST' action='/send'>"
         << "<input type='hidden' name='is_reply' value='true'>"
         << "<input type='hidden' name='original_id' value='" << emailId << "'>"
         << "To: <input type='text' name='to' value='" << replyTo << "'><br>"
         << "Subject: <input type='text' name='subject' value='" << replySubject << "'><br>"
         << "Body:<textarea name='body' rows='10' cols='30'>" << quotedBody << "</textarea><br>"
         << "<input type='hidden' name='username' value='" << username << "'>"
         << "<input type='submit' value='Send'></form>";
    html << "<a href='/inbox?username=" << username << "'>Back to Inbox</a>";
    html << "</body></html>";
    
    return html.str();
}

// Renders the "Forward Email" page, pre-filling the subject based on the original email and quoting the original message.
// It retrieves the original email's metadata to get the sender, subject, and timestamp.
std::string EmailService::renderForwardCompose(const std::string &username, const std::string &emailId, const std::string &folder) {
    // Get the original email
    string rowKey = (folder == "sent") ? "sent_email:" + username : "inbox_email:" + username;
    KeyValueStoreClient* client = ClientPool::getInstance().getWorkerClient(rowKey);
    string originalBody = client->Get(rowKey, emailId);
    if (originalBody.substr(0, 3) == "ERR") originalBody = "[Original message not available]";
    // Decrypt metadata to get sender and subject
    string decryptedMeta = decryptMetadata(emailId);
    map<string, string> headers;
    istringstream metaStream(decryptedMeta);
    string pair;
    while(getline(metaStream, pair, ';')) {
        if(pair.empty()) continue;
        size_t colonPos = pair.find(':');
        if(colonPos != string::npos) {
            string key = pair.substr(0, colonPos);
            string value = pair.substr(colonPos + 1);
            headers[key] = value;
        }
    }
    
    string originalSubject = headers["subject"];
    string forwardSubject = (originalSubject.find("Fwd:") == 0) ?
                          originalSubject : "Fwd: " + originalSubject;
    
    // Format forwarded body with complete header information
    string quotedBody = "\n\n---------- Forwarded Message ----------\nFrom: " + headers["sender"] +
                         "\nSubject: " + originalSubject + "\nDate: " + headers["timestamp"] +
                         "\n\n" + originalBody;
    
    // Render form with pre-filled values
    ostringstream html;
    html << "<html><head><title>Forward Email</title></head><body>";
    html << "<h1>Forward Message</h1>";
    html << "<form method='POST' action='/send'>"
         << "<input type='hidden' name='is_forward' value='true'>"
         << "<input type='hidden' name='original_id' value='" << emailId << "'>"
         << "To: <input type='text' name='to'><br>"
         << "Subject: <input type='text' name='subject' value='" << forwardSubject << "'><br>"
         << "Body:<textarea name='body' rows='10' cols='30'>" << quotedBody << "</textarea><br>"
         << "<input type='hidden' name='username' value='" << username << "'>"
         << "<input type='submit' value='Send'></form>";
    html << "<a href='/inbox?username=" << username << "'>Back to Inbox</a>";
    html << "</body></html>";
    
    return html.str();
}

// Deletes a specific email from the KVStore.
// It removes the email body and also removes the email ID from the mailbox metadata list
string EmailService::deleteEmail(const string &username, const string &emailId, const string &folder) {
    // Determine the mailbox row key based on folder
    cerr << "[EMAIL] Deleting email: row=" << username << ", id=" << emailId << endl;
    string rowKey = (folder == "sent") ? "sent_email:" + username : "inbox_email:" + username;
    // Get a worker client.
    KeyValueStoreClient* client = ClientPool::getInstance().getWorkerClient(rowKey);
    string res = client->Delete(rowKey, emailId);
    if (res.substr(0, 3) == "ERR") {
        return "<html><body><h1>Error:</h1><p>Delete failed: email not found or server error.</p></body></html>";
    }
    // If the body was deleted successfully, remove the email ID from the metadata list.
    removeFromMetadata(rowKey, emailId);
    return "Email deleted successfully!";
}

// Overloaded deleteEmail defaults to inbox.
string EmailService::deleteEmail(const string &username, const string &emailId) {
    return deleteEmail(username, emailId, "inbox");
}

// Handles incoming HTTP requests and dispatches them to the appropriate EmailService methods.
// It parses the request path, type (GET/POST), query string, and post data to determine the action to take.
string EmailService::handleRequest(const string &path, const string &reqType, const string &queryStr, const string &postData) {
    std::map<std::string, std::string> queryParams = parseQueryString(queryStr);
    
    // Get username from query parameters. If not present, default to "test".
    std::string username = (queryParams.find("username") != queryParams.end()) ? 
                             urlDecode(queryParams["username"]) : "test";
    // If the username does not contain "@" then append "@localhost"
    if (username.find("@") == std::string::npos) {
        username += "@localhost";
    }
    
    // Determine folder; default to "inbox".
    std::string folder = (queryParams.find("folder") != queryParams.end()) ? queryParams["folder"] : "inbox";
    
    if (path == "/inbox" && reqType == "GET") {
        return renderInbox(username);
    } else if (path == "/send" && reqType == "POST") {
        return sendEmail(postData);
    } else if (path == "/email" && reqType == "GET") {
        // Decode 'id' parameter to get the proper encrypted metadata.
        std::string emailId = (queryParams.find("id") != queryParams.end()) ? 
                              urlDecode(queryParams["id"]) : "1";
        return viewEmail(username, emailId, folder);
    } else if (path == "/compose" && reqType == "GET") {
        return renderCompose(username);
    } else if (path == "/delete" && reqType == "POST") {
        std::string emailId = (queryParams.find("id") != queryParams.end()) ?
                              urlDecode(queryParams["id"]) : "1";
        return deleteEmail(username, emailId, folder);
    } 
    else if (path == "/reply" && reqType == "GET") {
        std::string emailId = (queryParams.find("id") != queryParams.end()) ?
                              urlDecode(queryParams["id"]) : "1";
        return renderReplyCompose(username, emailId, folder);
    }
    else if (path == "/forward" && reqType == "GET") {
        std::string emailId = (queryParams.find("id") != queryParams.end()) ?
                              urlDecode(queryParams["id"]) : "1";
        return renderForwardCompose(username, emailId, folder);
    }    
    else {
        return "Invalid path";
    }
}
