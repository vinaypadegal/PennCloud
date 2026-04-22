#ifndef USER_ACCOUNT_SERVICE_H
#define USER_ACCOUNT_SERVICE_H

#include <string>
#include <grpcpp/grpcpp.h>
#include "../../backend/kvs_client.h"
#include "../../build/master.grpc.pb.h"

// Service for user credentials stored in KV‐store.
// Row key: "<username>-userinfo", column "password".
class UserAccountService {
public:
    // master_address e.g. "127.0.0.1:10000"
    explicit UserAccountService(const std::string &master_address = "127.0.0.1:10000");
    ~UserAccountService();

    // Returns true if the user exists *and* their password matches.
    bool authenticate(const std::string &username, const std::string &password);

    // Creates a new user; returns false if the user already exists.
    bool signup(const std::string &username, const std::string &password);

    // Attempts to change a user's password.
    // Returns:
    //   0 = success
    //   1 = user not found
    //   2 = old password mismatch
    int changePassword(const std::string &username,
                       const std::string &oldPassword,
                       const std::string &newPassword);

private:
    std::string masterAddress_;
};

#endif // USER_ACCOUNT_SERVICE_H
