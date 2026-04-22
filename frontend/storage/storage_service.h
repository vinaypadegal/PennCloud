#ifndef STORAGE_SERVICE_H
#define STORAGE_SERVICE_H

#include <string>
#include <vector>
#include <grpcpp/grpcpp.h>
#include "../backend/kvs_client.h"

class StorageService {
public:
    // Constructor/destructor
//    StorageService();
//    ~StorageService();

    // List files & folders (renders the HTML page for GET /storage or /list)
    std::string list(const std::string &username, const std::string &request);

    // Uploads a file (handles POST /upload)
    std::string upload(const std::string &username,
    				   const int client,
                       const std::string &request);

    // Downloads a file (handles GET /download)
    std::string download(const std::string &username,
    					const int client,
                         const std::string &request);

    // Creates a new folder (handles POST /createSubFolder)
    std::string createFolder(const std::string &username,
    							const int client,
                             const std::string &request);

    // Deletes a file or folder (handles GET /deleteStorage)
    std::string remove(const std::string &username,
    					const int client,
                       const std::string &request);

    // Renames a file    (handles GET/POST /rename)
    std::string renameFile(const std::string &username,
    						const int client,
                           const std::string &request);

    // Renames a folder  (handles GET/POST /renameFolder)
    std::string renameFolder(const std::string &username,
    						const int client,
                             const std::string &request);

//    // Moves file or folder recursively (handles GET/POST /move)
//    std::string move(const std::string &username,
//    					const int client,
//                     const std::string &request);
    std::string moveFile(const std::string &username,
        				 const int client,
                         const std::string &request);
    std::string moveFolder(const std::string &username,
                               int client,
                               const std::string &request);


//private:
//    KeyValueStoreClient* kvClient;
};

#endif
