#include "user_account_service.h"
#include <sstream>
#include <grpcpp/grpcpp.h>
#include "../backend/kvs_client.h"
#include "../build/master.grpc.pb.h"
#include <sstream>
#include <vector>
#include <algorithm>
using namespace std;

UserAccountService::UserAccountService(const std::string &master_address)
  : masterAddress_(master_address) {}

UserAccountService::~UserAccountService() {}

bool UserAccountService::authenticate(const std::string &username,
                                      const std::string &password) {
    // Lookup the userinfo worker
    MasterNodeClient master(
        grpc::CreateChannel(masterAddress_, grpc::InsecureChannelCredentials()));
    std::string worker = master.Lookup(username + "-userinfo");

    if(worker.substr(0,3) == "ERR"){
		return false;
	}

    // Query the password column
    KeyValueStoreClient client(
        grpc::CreateChannel(worker, grpc::InsecureChannelCredentials()));
    std::string stored = client.Get(username + "-userinfo", "password");

    if (stored.rfind("ERR", 0) == 0 || stored.empty()) return false;
    return stored == password;
}

bool UserAccountService::signup(const std::string &username,
                                const std::string &password) {
    MasterNodeClient master(
        grpc::CreateChannel(masterAddress_, grpc::InsecureChannelCredentials()));
    std::string worker = master.Lookup(username + "-userinfo");
    if(worker.substr(0,3) == "ERR"){
		return false;
	}

    KeyValueStoreClient client(
        grpc::CreateChannel(worker, grpc::InsecureChannelCredentials()));
    std::string existing = client.Get(username + "-userinfo", "password");
    // If there's no error, user already exists
    if (existing.rfind("ERR", 0) != 0) return false;

    // Otherwise create the user
    client.Put(username + "-userinfo", "password", password);
    return true;
}

int UserAccountService::changePassword(const std::string &username,
                                       const std::string &oldPassword,
                                       const std::string &newPassword) {
    MasterNodeClient master(
        grpc::CreateChannel(masterAddress_, grpc::InsecureChannelCredentials()));
    std::string worker = master.Lookup(username + "-userinfo");

    if(worker.substr(0,3) == "ERR"){
		return 3;
	}
    KeyValueStoreClient client(
        grpc::CreateChannel(worker, grpc::InsecureChannelCredentials()));
    std::string stored = client.Get(username + "-userinfo", "password");
    if (stored.rfind("ERR", 0) == 0) {
        // no such user
        return 1;
    }
    if (stored != oldPassword) {
        // wrong old password
        return 2;
    }
    // OK, update
    client.CPut(username + "-userinfo", "password", oldPassword, newPassword);
    return 0;
}

