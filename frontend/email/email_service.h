#ifndef EMAIL_SERVICE_H
#define EMAIL_SERVICE_H

#include <string>
#include <map>
#include <vector>
#include <grpcpp/grpcpp.h>
#include "../../backend/kvs_client.h"
#include "../../build/kvstore.grpc.pb.h"

class EmailService {
public:
    // Constructor: takes a pointer to a KeyValueStoreClient.
    EmailService(KeyValueStoreClient* client);

    // Destructor.
    ~EmailService();

    // Sends an email by storing it in the KV store and updating both sender and recipient metadata.
    // The email is stored under a column whose key is an encrypted metadata string.
    std::string sendEmail(const std::string &postData);

    // Deletes an email from the specified mailbox (folder "inbox" or "sent") and updates metadata.
    std::string deleteEmail(const std::string &username, const std::string &emailId, const std::string &folder);

    // Overloaded deleteEmail defaults to "inbox" folder.
    std::string deleteEmail(const std::string &username, const std::string &emailId);

    // Renders the email composition page.
    std::string renderCompose(const std::string &username);

    // Renders the inbox page with separate sections for received and sent emails.
    std::string renderInbox(const std::string &username);

    // Displays full details of a single email from the specified mailbox.
    std::string viewEmail(const std::string &username, const std::string &emailId, const std::string &folder);

    // Overloaded viewEmail: defaults to "inbox" folder.
    std::string viewEmail(const std::string &username, const std::string &emailId);

    // Generate a pre-populated compose page for replying to an email
    std::string renderReplyCompose(const std::string &username, const std::string &emailId, const std::string &folder = "inbox");

    // Generate a pre-populated compose page for forwarding an email
    std::string renderForwardCompose(const std::string &username, const std::string &emailId, const std::string &folder = "inbox");


    // Dispatches HTTP requests to the appropriate EmailService function.
    std::string handleRequest(const std::string &path, const std::string &reqType, 
                              const std::string &queryStr, const std::string &postData);

    // Helper: Appends an email's encrypted metadata (i.e., the unique email ID) to the metadata list 
    // in the mailbox row. 'mailboxRow' is the full key for a mailbox (e.g., "inbox_email:user").
    void appendToMetadata(const std::string &mailboxRow, const std::string &emailId);

    // Helper: Removes an email's encrypted metadata from the mailbox's metadata list.
    void removeFromMetadata(const std::string &mailboxRow, const std::string &emailId);

private:
    // Pointer to our KVStoreClient.
    KeyValueStoreClient* kvClient;
};

#endif // EMAIL_SERVICE_H
