// smtp_server.cc

// This file implements a basic SMTP server that listens on a specified port
// (default 2500). It handles standard SMTP commands like HELO, MAIL FROM,
// RCPT TO, DATA, NOOP, RSET, and QUIT. For emails addressed to "@localhost",
// it stores them in a distributed key-value store (KVStore). It also has
// experimental support (-x flag) for relaying emails to external domains
// by performing MX record lookups and forwarding the email to the resolved
// SMTP server. The server uses a thread pool to handle concurrent client
// connections.

#include <iostream>
#include <fstream>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <bits/stdc++.h>
#include <map>
#include <mutex>
#include <atomic>
#include <pthread.h>
#include <signal.h>
#include <resolv.h> // For DNS queries (MX lookup)
#include <netdb.h>  // For gethostbyname
#include "./client_pool.h"

#include "../../backend/kvs_client.h"
#include <grpcpp/grpcpp.h>
#include "../../build/kvstore.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using namespace std;

#define BUFFER_LENGTH 1024 // Buffer length for SMTP commands

// GLOBAL VARIABLES
bool verbose = false;                // Flag for verbose mode (-v)
vector<int> active_client_file_desc; // Active client file descriptors
volatile bool shuttingDown = false;  // Global shutdown flag
bool extraCreditNonLocal = false;    // Accept non-local recipients if true (-x)

// Use a global pointer for the KVStoreClient, to be initialized in main function
// KeyValueStoreClient* kvStoreClientPtr = nullptr;

// Utility: Shutdown handler for SIGINT (Ctrl+C)
void serverShutdown(int signum)
{
    shuttingDown = true;
    for (int client_fd : active_client_file_desc)
    {
        string msg = "421 localhost SMTP Server not available. Shutting down transmission channel\r\n";
        send(client_fd, msg.c_str(), msg.size(), 0);
        close(client_fd);
    }
    cerr << "\nS: Server shutting down...\n";
    sleep(1); // Add a short delay before exit
    exit(EXIT_SUCCESS);
}

// Dummy encryption function (replace with a real encryption in submission)
string encrypt(const string &metadata)
{
    // For now, simply reverse the string (as a placeholder for encryption)
    return std::string(metadata.rbegin(), metadata.rend());
}

// Dummy decryption function (simply reverse)
string decrypt(const string &encryptedMetadata)
{
    return std::string(encryptedMetadata.rbegin(), encryptedMetadata.rend());
}

void appendToMetadata(const string &mailboxRow, const string &rawMeta) {
    // Build the encrypted metadata
    string encryptedMeta = encrypt(rawMeta);
    
    KeyValueStoreClient* client = ClientPool::getInstance().getWorkerClient(mailboxRow);
    // Get current metadata
    string currentMeta = client->Get(mailboxRow, "meta");
    
    // Create new metadata
    string newMeta;
    if (currentMeta.substr(0, 3) == "ERR" || currentMeta.empty()) {
        newMeta = encryptedMeta;
    } else {
        newMeta = currentMeta + "," + encryptedMeta;
    }
    
    // Update metadata
    client->Put(mailboxRow, "meta", newMeta);
}

// Utility function to send a delivery failure notification back to the sender's inbox.
// This is used when an email cannot be delivered (e.g., invalid recipient, MX record not found).
void sendDeliveryFailure(const string& sender, const string& failedRecipient, const string& reason) {
    string timestamp;
    {
        time_t now = time(nullptr);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
        timestamp = buf;
    }

    string subject = "Delivery Failed to " + failedRecipient;
    string body = "Your email could not be delivered to " + failedRecipient + ".\nReason: " + reason;

    string rawMeta = "sender:mailer-daemon@localhost;recipient:" + sender + ";subject:" + subject + ";timestamp:" + timestamp + ";";
    string encryptedMeta = encrypt(rawMeta);

    string inboxRow = "inbox_email:" + sender;
    KeyValueStoreClient* client = ClientPool::getInstance().getWorkerClient(inboxRow);
    client->Put(inboxRow, encryptedMeta, body);

    // Update meta
    string curMeta = client->Get(inboxRow, "meta");
    string newMeta = (curMeta.substr(0, 3) == "ERR" || curMeta.empty()) ? encryptedMeta : (curMeta + "," + encryptedMeta);
    client->Put(inboxRow, "meta", newMeta);

    cerr << "[SMTP] Delivery failure email saved to sender's inbox" << endl;
}


// SMTPServer class encapsulates socket setup, the main accept loop, and client handling.
class SMTPServer
{
public:
    SMTPServer(unsigned short port) : portNumber(port) {}

    void run()
    {
        int server_fd;
        struct sockaddr_in server_addr, client_addr;
        socklen_t client_len = sizeof(client_addr);

        // Create TCP socket.
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == -1)
        {
            perror("Failure - server socket creation");
            exit(EXIT_FAILURE);
        }
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // Bind socket to IP and port.
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(portNumber);
        if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        {
            perror("Failure - Socket Bind");
            exit(EXIT_FAILURE);
        }

        // Listen for connections (maximum pending: 100).
        if (listen(server_fd, 100) < 0)
        {
            perror("FAILURE - Listen for connections");
            exit(EXIT_FAILURE);
        }
        cerr << "SMTP Server Listening on port " << portNumber << endl;

        // Main accept loop.
        while (!shuttingDown)
        {
            int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0)
            {
                if (shuttingDown)
                    break;
                perror("Accept failed");
                continue;
            }
            if (verbose)
                cerr << "[" << client_fd << "] New Connection Established" << endl;
            active_client_file_desc.push_back(client_fd);

            pthread_t client_thread;
            int *client_socket = new int(client_fd);
            if (pthread_create(&client_thread, NULL, SMTPServer::handle_client, client_socket) != 0)
            {
                perror("Failure - Creating Threads");
                delete client_socket;
            }
            pthread_detach(client_thread);
        }
        close(server_fd);
    }

private:
    unsigned short portNumber;

    // Helper functions for trimming whitespace.
    static inline void ltrim(string &s)
    {
        s.erase(s.begin(), find_if(s.begin(), s.end(), [](int ch)
                                   { return !isspace(ch); }));
    }
    static inline void rtrim(string &s)
    {
        s.erase(find_if(s.rbegin(), s.rend(), [](int ch)
                        { return !isspace(ch); })
                    .base(),
                s.end());
    }
    static inline void trim(string &s)
    {
        ltrim(s);
        rtrim(s);
    }

    // Resolves the MX (Mail Exchanger) record for a given domain name.
    // This is used to find the SMTP server responsible for accepting emails for that domain.
    static string resolveMXDomain(const string &domain)
    {
        unsigned char nsbuf[4096];
        ns_msg msg;
        ns_rr rr;

        int l = res_query(domain.c_str(), C_IN, T_MX, nsbuf, sizeof(nsbuf));
        if (l < 0)
        {
            perror("res_query failed");
            return "";
        }

        if (ns_initparse(nsbuf, l, &msg) < 0)
        {
            perror("ns_initparse failed");
            return "";
        }

        int count = ns_msg_count(msg, ns_s_an);
        int lowest_pref = INT_MAX;
        string best_mx_host = "";

        for (int i = 0; i < count; i++)
        {
            if (ns_parserr(&msg, ns_s_an, i, &rr) < 0)
                continue;

            const u_char *rdata = ns_rr_rdata(rr);
            int preference = (rdata[0] << 8) | rdata[1];
            char mxname[1025];
            if (dn_expand(nsbuf, nsbuf + l, rdata + 2, mxname, sizeof(mxname)) < 0)
                continue;

            if (preference < lowest_pref)
            {
                lowest_pref = preference;
                best_mx_host = mxname;
            }
        }

        return best_mx_host;
    }

    // Handles one SMTP client connection; processes SMTP commands in a loop.
    // This function is executed in a separate thread for each connected client.
    static void *handle_client(void *socket_client)
    {
        int file_desc_client = *((int *)socket_client);
        delete (int *)socket_client; // Free allocated memory

        // Send welcome message.
        const char *welcomeMsg = "220 localhost Simple Mail Transfer (SMTP) Server Ready\r\n";
        send(file_desc_client, welcomeMsg, strlen(welcomeMsg), 0);

        char bufferClients[BUFFER_LENGTH];
        string finalBuf;
        string senderEmail = "";
        vector<string> validRecipients;    // Valid local recipients (@localhost)
        vector<string> nonLocalRecipients; // Non-local recipients (if extraCreditNonLocal enabled)
        bool isDataRecptMode = false;      // Flag indicating DATA mode.
        bool inHeaderSection = true;       // Thread-local: reset per session
        string headersBlock;               // Stores headers during DATA mode
        string bodyBlock;                  // Stores body during DATA mode

        while (true)
        {
            memset(bufferClients, 0, sizeof(bufferClients));
            int bytes_received = recv(file_desc_client, bufferClients, sizeof(bufferClients) - 1, 0);
            if (bytes_received < 1)
                break;
            bufferClients[bytes_received] = '\0';
            finalBuf += bufferClients;

            // Process complete lines ending with CRLF.
            size_t position;
            while ((position = finalBuf.find("\r\n")) != string::npos)
            {
                string currentCommand = finalBuf.substr(0, position);
                finalBuf.erase(0, position + 2);

                // Trim whitespace.
                currentCommand.erase(currentCommand.begin(),
                                     find_if(currentCommand.begin(), currentCommand.end(), [](int ch)
                                             { return !isspace(ch); }));
                currentCommand.erase(find_if(currentCommand.rbegin(), currentCommand.rend(), [](int ch)
                                             { return !isspace(ch); })
                                         .base(),
                                     currentCommand.end());
                if (currentCommand.empty())
                    continue;

                if (verbose)
                    cerr << "[" << file_desc_client << "] C: " << currentCommand << endl;
                if (currentCommand.size() > BUFFER_LENGTH)
                {
                    string r = "-ERR Command given too long\r\n";
                    send(file_desc_client, r.c_str(), r.size(), 0);
                    continue;
                }

                // If in DATA mode, accumulate lines until a single dot is received.
                if (isDataRecptMode)
                {
                    if (currentCommand == ".")
                    { // End DATA mode
                        isDataRecptMode = false;
                        string r;
                        if (validRecipients.empty() && nonLocalRecipients.empty())
                        {
                            r = "550 Requested action not taken: no valid recipients\r\n";
                        }
                        else
                        {
                            string subject = "No Subject";
                            istringstream headerStream(headersBlock);
                            string headerLine;
                            while (getline(headerStream, headerLine))
                            {
                                if (headerLine.find("Subject:") == 0)
                                {
                                    subject = headerLine.substr(8); // Get everything after 'Subject:'
                                    trim(subject);
                                    break;
                                }
                            }
                            // === AUTO-FILL MISSING HEADERS ===
                            if (headersBlock.find("From:") == string::npos)
                            headersBlock += "From: " + senderEmail + "\n";

                            if (headersBlock.find("To:") == string::npos) {
                            if (!validRecipients.empty())
                                headersBlock += "To: " + validRecipients[0] + "\n";
                            else if (!nonLocalRecipients.empty())
                                headersBlock += "To: " + nonLocalRecipients[0] + "\n";
                            }

                            // Add RFC-compliant Date header
                            char dateBuf[128];
                            time_t now = time(nullptr);  
                            strftime(dateBuf, sizeof(dateBuf), "%a, %d %b %Y %H:%M:%S %z", localtime(&now));
                            if (headersBlock.find("Date:") == string::npos) {
                                headersBlock += "Date: " + string(dateBuf) + "\n";
                            }
                            
                            // Generate and insert Message-ID
                            ostringstream msgId;
                            msgId << "<" << now << "." << rand() % 10000 << "@" << senderEmail.substr(senderEmail.find("@") + 1) << ">";
                            headersBlock += "Message-ID: " + msgId.str() + "\n";

                            string emailBdy = bodyBlock;
                            // Get current timestamp.
                            // time_t now = time(nullptr);
                            char dtBuffer[64];
                            strftime(dtBuffer, sizeof(dtBuffer), "%Y-%m-%d %H:%M:%S", localtime(&now));
                            string timestamp(dtBuffer);

                            // Build metadata string:
                            // "sender:<senderEmail>;recipient:<recipientEmail>;subject:<subject>;timestamp:<timestamp>;"
                            // Then encrypt it (e.g. reverse the string) to generate our unique email id.
                            for (const string &recipient : validRecipients)
                            {
                                // Use the full email addresses.
                                string recipientEmail = recipient; // e.g., "bob@localhost"
                                // cerr << "Preparing to write to inbox of: " << recipientEmail << endl;
                                // Build raw metadata string.
                                ostringstream metaStream;
                                metaStream << "sender:" << senderEmail << ";"
                                           << "recipient:" << recipientEmail << ";"
                                           << "subject:" << subject << ";"
                                           << "timestamp:" << timestamp << ";";
                                string rawMeta = metaStream.str();
                                // Encrypt the metadata: (dummy encryption: reverse the string)
                                string encryptedMeta = encrypt(rawMeta);

                                // Compute the recipient’s mailbox row key.
                                string inboxRow = "inbox_email:" + recipientEmail;
                                // Store the email body under the encrypted metadata column.
                                KeyValueStoreClient* inboxClient = ClientPool::getInstance().getWorkerClient(inboxRow);
                                inboxClient->Put(inboxRow, encryptedMeta, emailBdy);

                                // Update the metadata column ("meta") for the recipient.
                                appendToMetadata(inboxRow, rawMeta);
                                cout << "Put the inbox entry successfully" << endl;
                                if (senderEmail.find("@localhost") != string::npos)
                                {
                                    string sentRow = "sent_email:" + senderEmail;
                                    KeyValueStoreClient* sentClient = ClientPool::getInstance().getWorkerClient(sentRow);

                                    // Always write the email to sent
                                    sentClient->Put(sentRow, encryptedMeta, emailBdy);
                                    appendToMetadata(sentRow, rawMeta);
                                }
                            }
                            // Also, process nonLocalRecipients if extra credit mode is enabled.
                            if (extraCreditNonLocal && !nonLocalRecipients.empty())
                            {
                                for (const string &recipient : nonLocalRecipients)
                                {
                                    size_t atPos = recipient.find('@');
                                    if (atPos == string::npos)
                                        continue;
                                    string domain = recipient.substr(atPos + 1);
                                    string mxHost = resolveMXDomain(domain);

                                    if (mxHost.empty())
                                    {
                                        cerr << "Failed to resolve MX for domain: " << domain << endl;
                                        sendDeliveryFailure(senderEmail, recipient, "MX record not found");
                                        continue;
                                    }

                                    // Connect to the external SMTP server (default port 25)
                                    struct sockaddr_in relay_addr{};
                                    struct hostent *host = gethostbyname(mxHost.c_str());
                                    if (!host)
                                    {
                                        cerr << "gethostbyname failed for: " << mxHost << endl;
                                        sendDeliveryFailure(senderEmail, recipient, "host not found");
                                        continue;
                                    }

                                    int relay_fd = socket(AF_INET, SOCK_STREAM, 0);
                                    if (relay_fd < 0)
                                    {
                                        cerr << "Socket creation failed for relay" << endl;
                                        sendDeliveryFailure(senderEmail, recipient, "socket creation failed");
                                        continue;
                                    }
                                    int relayPort = (mxHost == "127.0.0.1") ? 2525 : 25;

                                    relay_addr.sin_family = AF_INET;
                                    relay_addr.sin_port = htons(relayPort);
                                    relay_addr.sin_addr = *(struct in_addr *)host->h_addr;

                                    if (connect(relay_fd, (struct sockaddr *)&relay_addr, sizeof(relay_addr)) < 0)
                                    {
                                        cerr << "Failed to connect to external SMTP server: " << mxHost << endl;
                                        sendDeliveryFailure(senderEmail, recipient, "SMTP connect failed");
                                        close(relay_fd);
                                        continue;
                                    }

                                    auto recvSMTP = [&relay_fd]() -> string
                                    {
                                        char buf[1024];
                                        memset(buf, 0, sizeof(buf));
                                        recv(relay_fd, buf, sizeof(buf) - 1, 0);
                                        return string(buf);
                                    };

                                    auto sendSMTP = [&relay_fd](const string &cmd)
                                    {
                                        string msg = cmd + "\r\n";
                                        send(relay_fd, msg.c_str(), msg.size(), 0);
                                    };

                                    recvSMTP(); // Greet

                                    sendSMTP("HELO seas.upenn.edu");
                                    cerr << "[RELAY] S: " << recvSMTP() << endl;
                                    
                                    string rewrittenSender = senderEmail;
                                    if (rewrittenSender.find("@localhost") != string::npos) {
                                        rewrittenSender.replace(rewrittenSender.find("@localhost"), 10, "@seas.upenn.edu");
                                        if (verbose)
                                            cerr << "[RELAY] Rewriting sender: " << senderEmail << " → " << rewrittenSender << endl;
                                    }

                                    sendSMTP("MAIL FROM:<" + rewrittenSender + ">");
                                    cerr << "[RELAY] S: " << recvSMTP() << endl;

                                    sendSMTP("RCPT TO:<" + recipient + ">");
                                    cerr << "[RELAY] S: " << recvSMTP() << endl;

                                    sendSMTP("DATA");
                                    cerr << "[RELAY] S: " << recvSMTP() << endl;

                                    sendSMTP(headersBlock + "\r\n\r\n" + bodyBlock + "\r\n.");
                                    cerr << "[RELAY] S: " << recvSMTP() << endl;

                                    sendSMTP("QUIT");
                                    cerr << "[RELAY] S: " << recvSMTP() << endl;

                                    close(relay_fd);
                                }
                            }

                            validRecipients.clear();
                            headersBlock.clear();
                            bodyBlock.clear();
                            r = "250 Data input complete. Exiting DATA mode.\r\n";
                        }
                        send(file_desc_client, r.c_str(), r.size(), 0);
                        continue;
                    }
                    else
                    {
                        if (inHeaderSection)
                        {
                            string trimmed = currentCommand;
                            trim(trimmed);
                            if (trimmed.empty()) {
                                inHeaderSection = false;
                                if (verbose)
                                cerr << "[SMTP] Blank line detected — switching to body" << endl;
                            }
                            else if(trimmed.find(':') == string::npos) // No colon found =  Probably not a header
                            {
                                inHeaderSection = false;
                                if (verbose)
                                    cerr << "[SMTP] Non-header line — switching to body early" << endl;
                                bodyBlock += currentCommand + "\n";
                            }
                            else{
                                if (verbose)
                                cerr << "[SMTP] Adding to Headers: " << currentCommand << endl;
                                headersBlock += currentCommand + "\n";
                            }
                        }
                        else
                        {
                            cerr << "[SMTP] Adding to body: " << currentCommand << endl;
                            bodyBlock += currentCommand + "\n";
                        }
                        continue;
                    }
                }

                // Process SMTP commands outside DATA mode.
                string cmdPart = currentCommand.substr(0, currentCommand.find(" "));
                string messagePart = (currentCommand.size() > cmdPart.size()) ? currentCommand.substr(cmdPart.size() + 1) : "";
                transform(cmdPart.begin(), cmdPart.end(), cmdPart.begin(), ::toupper);
                string r; // Response string.

                if (cmdPart == "HELO")
                {
                    r = "250 localhost HELO " + messagePart + ". Happy to meet you\r\n";
                }
                else if (cmdPart == "MAIL")
                {
                    if (messagePart.find("FROM:") == 0)
                    {
                        senderEmail = messagePart.substr(5);
                        trim(senderEmail);
                        if (senderEmail.empty() || senderEmail.front() != '<' || senderEmail.back() != '>')
                        {
                            r = "500 Syntax error, command format unrecognized. Use format MAIL FROM:<youremail>\r\n";
                        }
                        else
                        {
                            senderEmail.erase(remove(senderEmail.begin(), senderEmail.end(), '<'), senderEmail.end());
                            senderEmail.erase(remove(senderEmail.begin(), senderEmail.end(), '>'), senderEmail.end());
                            validRecipients.clear();
                            nonLocalRecipients.clear();
                            r = "250 OK\r\n";
                        }
                    }
                    else
                    {
                        cerr << "[" << file_desc_client << "] Invalid MAIL FROM format: " << messagePart << endl;
                        r = "500 Syntax error, command format unrecognized\r\n";
                    }
                }
                else if (cmdPart == "RCPT")
                {
                    if (senderEmail.empty())
                    {
                        r = "503 Bad sequence of commands: MAIL FROM required before RCPT TO\r\n";
                    }
                    else if (messagePart.find("TO:") == 0)
                    {
                        string recipientEmail = messagePart.substr(3);
                        trim(recipientEmail);
                        if (recipientEmail.empty() || recipientEmail.front() != '<' || recipientEmail.back() != '>')
                        {
                            r = "500 Syntax error, command format unrecognized\r\n";
                        }
                        else
                        {
                            recipientEmail.erase(remove(recipientEmail.begin(), recipientEmail.end(), '<'), recipientEmail.end());
                            recipientEmail.erase(remove(recipientEmail.begin(), recipientEmail.end(), '>'), recipientEmail.end());
                            size_t pos = recipientEmail.find("@localhost");
                            if (pos == string::npos || recipientEmail.substr(pos) != "@localhost")
                            {
                                if (extraCreditNonLocal)
                                {
                                    nonLocalRecipients.push_back(recipientEmail);
                                    r = "250 OK\r\n";
                                }
                                else
                                {
                                    r = "550 Requested action not taken: unavailable\r\n";
                                }
                            }
                            else
                            {
                                validRecipients.push_back(recipientEmail);
                                r = "250 OK\r\n";
                            }
                        }
                    }
                    else
                    {
                        cerr << "[" << file_desc_client << "] Invalid RCPT TO format: " << messagePart << endl;
                        r = "500 Syntax error, command format unrecognized\r\n";
                    }
                }
                else if (cmdPart == "DATA")
                {
                    if (senderEmail.empty())
                    {
                        r = "503 Bad sequence of commands: MAIL FROM required first\r\n";
                    }
                    else if (validRecipients.empty() && (!extraCreditNonLocal || nonLocalRecipients.empty()))
                    {
                        r = "503 Bad sequence of commands: RCPT TO required first\r\n";
                    }
                    else
                    {
                        isDataRecptMode = true;
                        inHeaderSection = true;
                        headersBlock.clear();
                        bodyBlock.clear();
                        r = "354 Start mail input; end with <CRLF>.<CRLF>\r\n";
                    }
                }
                else if (cmdPart == "NOOP")
                {
                    r = "250 OK\r\n";
                }
                else if (cmdPart == "RSET")
                {
                    senderEmail.clear();
                    validRecipients.clear();
                    headersBlock.clear();
                    bodyBlock.clear();
                    isDataRecptMode = false;
                    r = "250 OK\r\n";
                }
                else if (cmdPart == "QUIT")
                {
                    r = "221 localhost SMTP Service Closing transmission!\r\n";
                    send(file_desc_client, r.c_str(), r.size(), 0);
                    if (verbose)
                        cerr << "[" << file_desc_client << "] S: Received QUIT command. Closing connection." << endl;
                    shutdown(file_desc_client, SHUT_WR);
                    close(file_desc_client);
                    return NULL;
                }
                else
                {
                    r = "500 Syntax error, Unknown command\r\n";
                }
                send(file_desc_client, r.c_str(), r.size(), 0);
                if (verbose)
                    cerr << "[" << file_desc_client << "] S: " << r << endl;
            }
        }
        close(file_desc_client);
        return NULL;
    }
};

// Main: Parse command-line arguments, register shutdown, and start the server.
int main(int argc, char *argv[])
{
    int port = 2500;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-p") == 0 && i < argc - 1)
        {
            port = atoi(argv[i + 1]);
            i++;
        }
        else if (strcmp(argv[i], "-a") == 0)
        {
            cerr << "Author: Ashay Katre / ashayk" << endl;
            exit(EXIT_SUCCESS);
        }
        else if (strcmp(argv[i], "-v") == 0)
        {
            verbose = true;
        }
        else if (strcmp(argv[i], "-x") == 0)
        {
            extraCreditNonLocal = true;
        }
    }
    signal(SIGINT, serverShutdown);
    res_init(); // Required for DNS resolver initialization
    // Initialize gRPC runtime.
    grpc_init();

    SMTPServer server((unsigned short)port);
    server.run();

    grpc_shutdown();
    return 0;
}
