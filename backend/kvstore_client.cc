#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <grpcpp/grpcpp.h>
#include "../build/kvstore.grpc.pb.h"
#include "../build/master.grpc.pb.h"
#include "kvs_client.h"

using namespace std;

using grpc::Channel;
using grpc::ClientContext;


void RunClient(string port) {
    string master_address("127.0.0.1:10000");
    MasterNodeClient masterclient(
      // Channel from which RPCs are made - endpoint is the target_address
      grpc::CreateChannel(master_address,
                          // Indicate when channel is not authenticated
                          grpc::InsecureChannelCredentials()));
    string worker = masterclient.Lookup("alice123-userinfo");  // worker 4
    cout << worker << endl;

    string worker_address(worker);
    KeyValueStoreClient client(
      // Channel from which RPCs are made - endpoint is the target_address
      grpc::CreateChannel(worker_address,
                          // Indicate when channel is not authenticated
                          grpc::InsecureChannelCredentials()));

    client.Put("alice123-userinfo", "col1", "hello world");
    client.Put("alice123-userinfo", "col3", "another string");
  
    client.AdminConsole("admin", "SHUTDOWN");
    sleep(10);
    client.AdminConsole("admin", "RESTART");

    string res = client.Get("alice123-userinfo", "col3");
    cout << res << endl;
    client.CPut("alice123-userinfo", "col3", "another string", "i am ironman");
    res = client.Get("alice123-userinfo", "col3");
    cout << res << endl;

    client.Delete("alice123-userinfo", "col1");
    res = client.Get("alice123-userinfo", "col1");
    cout << res << endl;

    // string target_address("127.0.0.1:10000");
    // MasterNodeClient client(
    //   // Channel from which RPCs are made - endpoint is the target_address
    //   grpc::CreateChannel(target_address,
    //                       // Indicate when channel is not authenticated
    //                       grpc::InsecureChannelCredentials()));
    // string worker = client.Lookup("user1"); // worker 5
    // cout << worker << endl;
    // worker = client.Lookup("alice123-userinfo"); // worker 4
    // cout << worker << endl;
    // worker = client.Lookup("alice123-userinfo"); // worker 2
    // cout << worker << endl;
}


int main(int argc, char* argv[]) {
    RunClient("10000");
    return 0;
}
