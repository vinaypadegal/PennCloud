#include "storage_service.h"
#include <sstream>
#include <grpcpp/grpcpp.h>
#include "../backend/kvs_client.h"
#include "../build/master.grpc.pb.h"
#include <sstream>
#include <vector>
#include <algorithm>
#include <regex>
using namespace std;


const string master_address = "127.0.0.1:10000";
const size_t MAX_FILE_SIZE = 10 * 1024 * 1024;


// Builds a full HTTP/1.1 response header by combining the status line, content type,
// content length, any extra headers passed, and a keep-alive connection.
string buildHttpHeaderStorage(const string &status, const string &contentType, size_t contentLength, const string &extraHeaders = "") {
    return "HTTP/1.1 " + status + "\r\n" +
           "Content-Type: " + contentType + "\r\n" +
           "Content-Length: " + to_string(contentLength) + "\r\n" +
           extraHeaders +
		   "Connection: keep-alive\r\n\r\n";
}

// Constructs a complete HTTP/1.1 response by generating the headers (status line, content type, content length, any extra headers)
// and then appending the response body.
string httpResponseStorage(const string &status, const string &body, const string &contentType = "text/html", const string &extraHeaders = "") {
    return buildHttpHeaderStorage(status, contentType, body.size(), extraHeaders) + body;
}


// Helper: parse a specific form field value from multipart/form-data body.
// It searches for the field name pattern and then returns the text content of the field.
string parseMultipartFieldStorage(const string &body, const string &fieldName) {
    string pattern = "name=\"" + fieldName + "\"";
    size_t pos = body.find(pattern);
    if (pos != string::npos) {
        size_t valueStart = body.find("\r\n\r\n", pos);
        if (valueStart != string::npos) {
            valueStart += 4;
            // Look for the next CRLF before the multipart boundary.
            size_t valueEnd = body.find("\r\n", valueStart);
            if (valueEnd != string::npos) {
                return body.substr(valueStart, valueEnd - valueStart);
            }
        }
    }
    return "";
}

// Finds the index of the first "\r\n\r\n" in `body` (or body.size() if not found)
static size_t findHeaderEnd(const std::vector<char> &body) {
    for (size_t i = 0; i + 3 < body.size(); ++i) {
        if (body[i] == '\r' && body[i+1] == '\n' &&
            body[i+2] == '\r' && body[i+3] == '\n') {
            return i + 4;
        }
    }
    return body.size();
}

// Parses out filename="…" from the multipart headers.
// If no filename found, returns "uploaded_file".
std::string parseMultipartFilenameStorage(const std::vector<char> &body) {
    static const std::string marker = "filename=\"";

    size_t hdrEnd = findHeaderEnd(body);
    std::string header(body.data(), hdrEnd);


    size_t pos = header.find(marker);
    if (pos == std::string::npos) return "uploaded_file";
    pos += marker.size();
    size_t endPos = header.find('"', pos);
    if (endPos == std::string::npos) return "uploaded_file";
    return header.substr(pos, endPos - pos);
}


// Looks up and returns the correct MIME type for a file based on its extension (using a predefined map),
// falling back to "application/octet-stream" if the extension isn’t recognized.
std::string get_content_type_storage(const std::string &filename) {
    static const std::unordered_map<std::string, std::string> types = {
        // Text
        {".txt",  "text/plain"},
        {".html", "text/html"},
        {".htm",  "text/html"},
        {".css",  "text/css"},
        {".js",   "application/javascript"},
        {".json", "application/json"},
        {".xml",  "application/xml"},
        // PDF
        {".pdf",  "application/pdf"},
        // Images
        {".png",  "image/png"},
        {".jpg",  "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif",  "image/gif"},
        {".bmp",  "image/bmp"},
        {".webp", "image/webp"},
        {".svg",  "image/svg+xml"},
        {".ico",  "image/x-icon"},
        // Audio
        {".mp3",  "audio/mpeg"},
        {".wav",  "audio/wav"},
        {".ogg",  "audio/ogg"},
        {".flac", "audio/flac"},
        // Video
        {".mp4",  "video/mp4"},
        {".mov",  "video/quicktime"},
        {".avi",  "video/x-msvideo"},
        {".mpeg", "video/mpeg"},
        {".webm", "video/webm"},
        {".mkv",  "video/x-matroska"}
    };

    // find last dot after last slash
    auto slash = filename.find_last_of("/\\");
    auto pos   = filename.find_last_of('.');
    if (pos != std::string::npos && (slash == std::string::npos || pos > slash)) {
        std::string ext = filename.substr(pos);
        auto it = types.find(ext);
        if (it != types.end()) {
            return it->second;
        }
    }
    // fallback
    return "application/octet-stream";
}

// Returns true if 's' is a valid name composed of one or two segments (separated by a single dot),
// where each segment contains only letters, digits, parentheses, underscores, or spaces.
bool isValidName(const std::string& s) {
    // only letters, digits, parentheses, comma, underscore and space
    static const std::regex re(R"(^[A-Za-z0-9()_ ]+(\.[A-Za-z0-9()_ ]+)?$)");
    return std::regex_match(s, re);
}

// Decodes a URL-encoded string, converting '%xx' hex sequences back to characters and '+' into spaces.
string urlDecode2(const std::string& s) {
    std::ostringstream out;
    out.fill('0');
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            // grab next two hex digits
            std::string hex = s.substr(i + 1, 2);
            // convert to integer value
            char decoded = static_cast<char>(std::stoi(hex, nullptr, 16));
            out << decoded;
            i += 2;  // skip past the two hex chars
        }
        else if (s[i] == '+') {
            // treat '+' as space
            out << ' ';
        }
        else {
            out << s[i];
        }
    }
    return out.str();
}

// Decodes the URL-encoded query string and splits it into key=value pairs (using '&' as delimiter),
// returning an unordered_map of parameter names to their corresponding values.
unordered_map<string, string> parseQueryParamsStorage(string query) {
    unordered_map<string, string> params;
    int pos = 0;
    query = urlDecode2(query);
    cout<<"Query "<<query<<endl;

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

// Reads the HTTP request body up to MAX_FILE_SIZE: parses the Content-Length header (with safe fallback), takes any bytes already in `request` past `headerEnd`, and reads the remainder from `clientSocket` until the full body is retrieved.
string readRequestBodyStorage(int clientSocket, const string &request, size_t headerEnd) {
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
    if (contentLength == 0 || contentLength > MAX_FILE_SIZE)
        contentLength = MAX_FILE_SIZE;


    string body = request.substr(headerEnd + 4);
    size_t totalSize = body.size();
    while (totalSize < contentLength && totalSize < MAX_FILE_SIZE) {
        char buf[8192];
        int bytes = read(clientSocket, buf, sizeof(buf));
        if (bytes <= 0) break;
        body.append(buf, bytes);
        totalSize += bytes;
    }
    return body;
}

// Read a single line (up to \r\n) from the socket
static string recvLine(int sock) {
    string line;
    char c;
    while (recv(sock, &c, 1, 0) == 1) {
        line.push_back(c);
        if (line.size() >= 2 && line.substr(line.size()-2) == "\r\n")
            break;
    }
    return line;
}

// Read a Transfer-Encoding: chunked body verbatim into a vector<char>
std::vector<char> readChunkedBodyRaw(int clientSock) {
    std::vector<char> body;
    while (true) {
        //read the chunk-size line
        std::string sizeLine = recvLine(clientSock);
        if (sizeLine.size() < 2) throw std::runtime_error("Client closed prematurely");
        sizeLine.resize(sizeLine.size()-2);  // drop "\r\n"
        size_t chunkSize = std::stoul(sizeLine, nullptr, 16);
        if (chunkSize == 0) {
            // consume the final "\r\n" after the zero-chunk
            recvLine(clientSock);
            break;
        }
        //read exactly chunkSize bytes
        size_t got = 0;
        body.resize(body.size() + chunkSize);
        while (got < chunkSize) {
            ssize_t n = recv(clientSock,
                             body.data() + (body.size() - chunkSize) + got,
                             chunkSize - got, 0);
            if (n <= 0) throw std::runtime_error("Client closed mid-chunk");
            got += n;
        }
        //discard the trailing "\r\n"
        recvLine(clientSock);
    }
    return body;
}

// Stream a single string back in chunked‐encoding
void sendChunkedResponse(int clientSock,
                         const string& statusLine,
                         const string& contentType,
                         const string& data,
						 const string & extraheaders,
                         size_t chunkSize = 65536)
{
    // status + headers
	ostringstream hdr;
	    hdr << statusLine                  << "\r\n"
	        << "Content-Type: " << contentType   << "\r\n"
	        // extraheaders must be e.g. "Content-Disposition: attachment; filename=\"foo.jpg\""
	        << extraheaders                 << "\r\n"
	        << "Transfer-Encoding: chunked\r\n"
	        << "Connection: keep-alive\r\n"
	        << "\r\n";
	    auto headerStr = hdr.str();
	    send(clientSock, headerStr.data(), headerStr.size(), 0);

    // the chunks
    size_t offset = 0;
    while (offset < data.size()) {
        size_t sz = min(chunkSize, data.size() - offset);
        // hex length + CRLF
        char lenBuf[32];
        int len = sprintf(lenBuf, "%zx\r\n", sz);
        send(clientSock, lenBuf, len, 0);

        // the data + CRLF
        send(clientSock, data.data() + offset, sz, 0);
        send(clientSock, "\r\n", 2, 0);

        offset += sz;
    }
    // zero-length chunk to end
    send(clientSock, "0\r\n\r\n", 5, 0);
}


// Helper to split comma‑separated lists
static vector<string> splitStringStorage(const string &str, char delimiter) {
	vector<string> tokens;
	size_t start = 0, pos = 0;
	while ((pos = str.find(delimiter, start)) != string::npos) {
		tokens.push_back(str.substr(start, pos - start));
		start = pos + 1;
	}
	tokens.push_back(str.substr(start));
	return tokens;
}

// Recursively delete a folder and all of its contents, or delete a file (with chunked support)
//   baseKey:           the single row key (e.g. "user-storage")
//   absPath:           the absolute path of the folder to delete (e.g. "abc/def")
//   client:            KeyValueStoreClient
void deleteRec(KeyValueStoreClient &client,
               const std::string &baseKey,
               const std::string &absPath) {
    // Determine if absPath is a folder or a file
    std::string marker = client.Get(baseKey, absPath);
    if (marker != "KV-Folder") {
        //File case: handle chunked or single-part
        //Delete each chunk if chunked
        std::string fileMetaCol = absPath + "-metadata";
        std::string fileMeta    = client.Get(baseKey, fileMetaCol);
        if (!fileMeta.empty() && fileMeta.rfind("ERR", 0) != 0) {
            auto chunks = splitStringStorage(fileMeta, ',');
            for (auto &chunk : chunks) {
                client.Delete(baseKey, chunk + "-content");
            }
            client.Delete(baseKey, fileMetaCol);
        }
        //Delete legacy single-content column
        client.Delete(baseKey, absPath + "-content");
        //Delete file marker
        client.Delete(baseKey, absPath);
        return;
    }

    //Folder case: recurse
    //Mark folder as deleted and delete its metadata column
    std::string metaCol = absPath + "-metadata";
    client.Put(baseKey, absPath, "DEL KV-Folder");
    std::string children = client.Get(baseKey, metaCol);
    client.Delete(baseKey, metaCol);

    //Recursively delete all children
    if (!children.empty() && children.rfind("ERR", 0) != 0) {
        auto items = splitStringStorage(children, ',');
        for (auto &childAbs : items) {
            deleteRec(client, baseKey, childAbs);
        }
    }
}



// Recursively rename a folder and all of its contents, handling chunked files.
void renameFolderRec(KeyValueStoreClient &client,
                     const std::string &baseKey,
                     const std::string &oldAbs,
                     const std::string &newAbs) {
    //Copy folder marker & metadata
    std::string oldMetaCol = oldAbs + "-metadata";
    std::string folderMeta = client.Get(baseKey, oldMetaCol);
    client.Put(baseKey, newAbs, "KV-Folder");
    client.Put(baseKey, newAbs + "-metadata", folderMeta);

    //Mark old folder as deleted and remove its metadata
    client.Put(baseKey, oldAbs, "DEL KV-Folder");
    client.Delete(baseKey, oldMetaCol);

    //Process each child in the old folder
    auto children = splitStringStorage(folderMeta, ',');
    for (auto &childAbs : children) {
        //Determine local name and new child path
        auto pos = childAbs.find_last_of('/');
        std::string local = (pos == std::string::npos ? childAbs
                                                        : childAbs.substr(pos + 1));
        std::string newChildAbs = newAbs + "/" + local;

        //Check if child is a folder
        std::string val = client.Get(baseKey, childAbs);
        if (val == "KV-Folder") {
            // Recurse for subfolders
            renameFolderRec(client, baseKey, childAbs, newChildAbs);
        } else {
            //Handle file rename with chunking
            std::string fileMetaCol = childAbs + "-metadata";
            std::string fileMeta    = client.Get(baseKey, fileMetaCol);
            std::vector<std::string> oldChunks;
            std::vector<std::string> newChunks;

            //If chunk metadata exists, rename each chunk
            if (!fileMeta.empty() && fileMeta.rfind("ERR", 0) != 0) {
                oldChunks = splitStringStorage(fileMeta, ',');
                for (auto &chunk : oldChunks) {
                    // suffix includes the dash and index, e.g. "-1"
                    std::string suffix   = chunk.substr(childAbs.size());
                    std::string target   = newChildAbs + suffix;
                    // Copy content column
                    std::string content  = client.Get(baseKey, chunk + "-content");
                    client.Put(baseKey, target + "-content", content);
                    // Delete old content column
                    client.Delete(baseKey, chunk + "-content");
                    newChunks.push_back(target);
                }
                // Write new metadata list and remove old
                std::string newMetaCol = newChildAbs + "-metadata";
                std::ostringstream mb;
                for (size_t i = 0; i < newChunks.size(); ++i) {
                    if (i) mb << ',';
                    mb << newChunks[i];
                }
                client.Put(baseKey, newMetaCol, mb.str());
                client.Delete(baseKey, fileMetaCol);
            } else {
                //Single-part file: copy & delete old
                std::string content = client.Get(baseKey, childAbs + "-content");
                client.Put(baseKey, newChildAbs + "-content", content);
                client.Delete(baseKey, childAbs + "-content");
            }

            //Rename file marker
            client.Put(baseKey, newChildAbs, "KV-File");
            client.Delete(baseKey, childAbs);
        }
    }
}

//Recursively move (rename) a folder and all of its contents, handling chunked files.
void renameFolderMoveRec(KeyValueStoreClient &client,
                         const std::string &baseKey,
                         const std::string &oldAbs,
                         const std::string &newAbs)
{
    // Read old folder metadata
    std::string oldMetaCol = oldAbs + "-metadata";
    std::string folderMeta = client.Get(baseKey, oldMetaCol);
    if (folderMeta.rfind("ERR", 0) == 0) folderMeta.clear();

    // Split into child paths
    auto oldChildren = splitStringStorage(folderMeta, ',');
    oldChildren.erase(
          std::remove_if(oldChildren.begin(), oldChildren.end(),
            [&](auto &s){ return s.empty() || s == oldAbs; }),
          oldChildren.end()
        );

    // Build new metadata list for newAbs
    std::ostringstream metaBuilder;
    for (size_t i = 0; i < oldChildren.size(); ++i) {
        if (i) metaBuilder << ",";
        auto &childOld = oldChildren[i];
        size_t slash = childOld.find_last_of('/');
        std::string local = (slash == std::string::npos
                             ? childOld
                             : childOld.substr(slash + 1));
        if(local.empty()){
        	metaBuilder << newAbs;
        }
        else{
        	metaBuilder << newAbs << "/" << local;
        }
    }
    std::string newMeta = metaBuilder.str();

    // Create new folder marker & metadata
    client.Put(baseKey, newAbs,               "KV-Folder");
    client.Put(baseKey, newAbs + "-metadata", newMeta);

    // Delete old folder marker & metadata
    client.Put(baseKey, oldAbs,              "DEL KV-Folder");
    client.Delete(baseKey, oldMetaCol);

    // Recurse into each child
    for (auto &childOld : oldChildren) {
        size_t slash = childOld.find_last_of('/');
        std::string local = (slash == std::string::npos
                             ? childOld
                             : childOld.substr(slash + 1));
        std::string childNew = newAbs;
        if(!local.empty()){
        	childNew+= "/" + local;
        }
        std::string marker = client.Get(baseKey, childOld);
        cout<<"marker: "<<marker<<endl;
        cout<<"child old: "<<childOld<<endl;
        cout<<"Child new: "<<childNew;
        if (marker == "KV-Folder") {
            // Subfolder → move recursively
            renameFolderMoveRec(client, baseKey, childOld, childNew);
        } else if(marker == "KV-File"){
            // File : handle chunked move
            std::string fileMetaCol = childOld + "-metadata";
            std::string fileMeta    = client.Get(baseKey, fileMetaCol);
            // If chunk metadata exists, rename each chunk
            if (!fileMeta.empty() && fileMeta.rfind("ERR", 0) != 0) {
                auto chunks = splitStringStorage(fileMeta, ',');
                std::vector<std::string> newChunks;
                for (auto &chunk : chunks) {
                    // Derive suffix ("-1", "-2", etc.)
                    std::string suffix = chunk.substr(childOld.size());
                    std::string newChunk = childNew + suffix;
                    // Copy content column
                    std::string content = client.Get(baseKey, chunk + "-content");
                    client.Put(baseKey, newChunk + "-content", content);
                    // Delete old content column
                    cout<<"old file path: "<<chunk<<endl;
					cout<<"new file path: "<<newChunk<<endl;
                    client.Delete(baseKey, chunk + "-content");
                    newChunks.push_back(newChunk);
                }
                // Write new metadata list
                std::string newMetaCol = childNew + "-metadata";
                std::ostringstream mb;
                for (size_t j = 0; j < newChunks.size(); ++j) {
                    if (j) mb << ',';
                    mb << newChunks[j];
                }
                client.Put(baseKey, newMetaCol, mb.str());
                client.Delete(baseKey, fileMetaCol);
            } else {
                // Single-part file: copy & delete old
                std::string content = client.Get(baseKey, childOld + "-content");
                client.Put(baseKey, childNew + "-content", content);
                client.Delete(baseKey, childOld + "-content");
            }
            // Move file marker
            client.Put(baseKey, childNew, "KV-File");
            client.Delete(baseKey, childOld);
        }
    }
}




// Move a single file into another folder, handling chunked storage
void moveFile(KeyValueStoreClient& client,
              const std::string& baseKey,
              const std::string& oldAbsFilePath,
              const std::string& newFolderAbsPath) {
    //Extract filename & source folder
    size_t pos = oldAbsFilePath.find_last_of('/');
    std::string fileName = (pos == std::string::npos
                            ? oldAbsFilePath
                            : oldAbsFilePath.substr(pos + 1));
    std::string srcFolderPath = (pos == std::string::npos
                                 ? ""
                                 : oldAbsFilePath.substr(0, pos));
    //Compute new absolute file path
    std::string newAbsFilePath = newFolderAbsPath.empty()
                                 ? fileName
                                 : newFolderAbsPath + "/" + fileName;
    {
            // split leaf into base + extension
            auto dotPos   = fileName.find_last_of('.');
            std::string leafBase = dotPos == std::string::npos
                                   ? fileName
                                   : fileName.substr(0, dotPos);
            std::string leafExt  = dotPos == std::string::npos
                                   ? ""
                                   : fileName.substr(dotPos);

            // check for existing file or folder at the target path
            std::string existing = client.Get(baseKey, newAbsFilePath);
            int suffix = 1;
            while (existing == "KV-File" || existing == "KV-Folder") {
                // build "leafBase(suffix) + leafExt"
                std::string newLeaf = leafBase
                                      + "(" + std::to_string(suffix) + ")"
                                      + leafExt;
                newAbsFilePath = newFolderAbsPath.empty()
                                 ? newLeaf
                                 : newFolderAbsPath + "/" + newLeaf;
                existing = client.Get(baseKey, newAbsFilePath);
                ++suffix;
            }
            // update fileName to match the new leaf
            fileName = newAbsFilePath.substr(
                         newAbsFilePath.find_last_of('/') + 1);
        }

    //Handle chunked content if present
    std::string fileMetaCol = oldAbsFilePath + "-metadata";
    std::string fileMeta    = client.Get(baseKey, fileMetaCol);
    if (!fileMeta.empty() && fileMeta.rfind("ERR", 0) != 0) {
        // File was stored in chunks
        auto chunks = splitStringStorage(fileMeta, ',');
        std::vector<std::string> newChunks;
        for (auto &chunk : chunks) {
            // Suffix includes dash + index
            std::string suffix   = chunk.substr(oldAbsFilePath.size());
            std::string newChunk = newAbsFilePath + suffix;
            // Copy chunk content
            std::string content  = client.Get(baseKey, chunk + "-content");
            client.Put(baseKey, newChunk + "-content", content);
            // Delete old chunk content
            client.Delete(baseKey, chunk + "-content");
            newChunks.push_back(newChunk);
        }
        // Write new metadata list
        std::string newChunkMetaCol = newAbsFilePath + "-metadata";
        std::ostringstream cmb;
        for (size_t i = 0; i < newChunks.size(); ++i) {
            if (i) cmb << ',';
            cmb << newChunks[i];
        }
        client.Put(baseKey, newChunkMetaCol, cmb.str());
        // Remove old metadata column
        client.Delete(baseKey, fileMetaCol);
    } else {
        // Single-part file
        std::string content = client.Get(baseKey, oldAbsFilePath + "-content");
        client.Put(baseKey, newAbsFilePath + "-content", content);
        client.Delete(baseKey, oldAbsFilePath + "-content");
    }

    //Move file marker
    client.Put(baseKey, newAbsFilePath, "KV-File");
    client.Delete(baseKey, oldAbsFilePath);

    //Update destination folder metadata
    std::string destMetaCol = newFolderAbsPath.empty()
                              ? "metadata"
                              : newFolderAbsPath + "-metadata";
    std::string destMeta    = client.Get(baseKey, destMetaCol);
    std::vector<std::string> destItems = splitStringStorage(destMeta, ',');
    destItems.push_back(newAbsFilePath);
    std::ostringstream dmb;
    for (size_t i = 0; i < destItems.size(); ++i) {
        if (i) dmb << ',';
        dmb << destItems[i];
    }
    client.Put(baseKey, destMetaCol, dmb.str());

    //Update source folder metadata
    std::string srcMetaCol = srcFolderPath.empty()
                              ? "metadata"
                              : srcFolderPath + "-metadata";
    std::string srcMeta    = client.Get(baseKey, srcMetaCol);
    auto srcItems = splitStringStorage(srcMeta, ',');
    std::vector<std::string> kept;
    for (auto &item : srcItems) {
        if (item != oldAbsFilePath) kept.push_back(item);
    }
    if (kept.empty()) {
        client.Delete(baseKey, srcMetaCol);
    } else {
        std::ostringstream smb;
        for (size_t i = 0; i < kept.size(); ++i) {
            if (i) smb << ',';
            smb << kept[i];
        }
        client.Put(baseKey, srcMetaCol, smb.str());
    }
}


//A moveFolder that invokes it and fixes the two parent lists
void moveFolder(KeyValueStoreClient &client,
                const string &baseKey,
                const string &oldAbs,
                const string &newParentFolder)
{
    //extract the folder name and its old parent
    size_t slash   = oldAbs.find_last_of('/');
    string name    = (slash==string::npos ? oldAbs : oldAbs.substr(slash+1));
    string srcParent = (slash==string::npos ? "" : oldAbs.substr(0,slash));

    //compute newAbs properly for root
    string newAbs = newParentFolder.empty()
                    ? name
                    : newParentFolder + "/" + name;

    // deep-copy & delete
    renameFolderMoveRec(client, baseKey, oldAbs, newAbs);

    //add newAbs to destination parent’s metadata
    string destMetaCol = newParentFolder.empty()
                         ? "metadata"
                         : newParentFolder + "-metadata";
    string destMeta = client.Get(baseKey, destMetaCol);
    if (destMeta.rfind("ERR",0)==0 || destMeta.empty()) {
        destMeta = newAbs;
    } else {
        destMeta += "," + newAbs;
    }
    client.Put(baseKey, destMetaCol, destMeta);

    //remove oldAbs from source parent’s metadata
    string srcMetaCol = srcParent.empty()
                        ? "metadata"
                        : srcParent + "-metadata";
    string srcMeta = client.Get(baseKey, srcMetaCol);
    auto items = splitStringStorage(srcMeta, ',');
    ostringstream rebuilt;
    bool first = true;
    for (auto &item : items) {
        if (item == oldAbs) continue;
        if (!first) rebuilt << ",";
        first = false;
        rebuilt << item;
    }
    if (first) {
        client.Delete(baseKey, srcMetaCol);
    } else {
        client.Put(baseKey, srcMetaCol, rebuilt.str());
    }
}




// Ensures `newName` ends with the same extension as `oldName`.
// If `newName` already has an extension, it’s replaced.
// If `newName` has no extension, the old extension is appended.
// If `oldName` has no extension, `newName` is returned unchanged.
static std::string ensureSameExtension(const std::string &oldName,
                                       const std::string &newName) {
    // Extract old extension (including the dot), or empty if none
    auto posOld = oldName.find_last_of('.');
    std::string extOld;
    if (posOld != std::string::npos && posOld + 1 < oldName.size()) {
        extOld = oldName.substr(posOld);  // e.g. ".pdf"
    }

    // If oldName had no “.ext”, strip any extension from newName (if present)
    if (extOld.empty()) {
        auto posNew = newName.find_last_of('.');
        if (posNew != std::string::npos) {
            return newName.substr(0, posNew);
        }
        return newName;
    }

    //Find any extension in newName
    auto posNew = newName.find_last_of('.');
    if (posNew != std::string::npos && posNew + 1 < newName.size()) {
        // Has an extension—strip it off
        return newName.substr(0, posNew) + extOld;
    } else {
        // No extension—append old one
        return newName + extOld;
    }
}




string webStorageList(const string &username,
                            const string &request) {
	MasterNodeClient masterclient(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(master_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
	string worker = masterclient.Lookup(username+"-storage");  // worker 4

	if(worker.substr(0,3) == "ERR"){
		return httpResponseStorage("503 Service Unavailable","<html><body>Servers down, sorry for the inconvenience!</body></html>");
	}
	cout << "Worker address "<<worker << endl;
	string worker_address(worker);
	KeyValueStoreClient client(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(worker_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
    // auto client = makeClient(username);
    //If all servers for this username-storage are down

    // Base key (single row).
	string baseKey = username + "-storage";

	// Parse current folder from the query parameter "folder".
	string url = request.substr(0, request.find("\r\n"));
	size_t qmPos = url.find("?");
	string queryStr = "";
	string currentFolder = "";
	if (qmPos != string::npos) {
		queryStr = url.substr(qmPos + 1, url.find(" ", qmPos + 1) - qmPos - 1);
		auto params = parseQueryParamsStorage(queryStr);
		if (params.find("folder") != params.end())
			currentFolder = params["folder"];
	}

	// Determine which metadata column to read.
	// For root folder (currentFolder empty): column "metadata"
	// For subfolder: column name equals currentFolder.
	string metaCol = currentFolder.empty() ? "metadata" : currentFolder+"-metadata";
	string metadata = client.Get(baseKey, metaCol);
	if(metadata.substr(0,3) == "ERR"){
		client.Put(baseKey, metaCol, "");
	}
	if(!currentFolder.empty()){
		string checkFolder = client.Get(baseKey, currentFolder);
		if(checkFolder == "DEL KV-Folder"){
			return httpResponseStorage("400 Bad Request",
					"<html><body>This folder was deleted. <p><a href='/storage'>Back to Storage Home</a></p></body></html>");
		}
	}
	ostringstream listing;
	listing << "<!DOCTYPE html>\n<html>\n<head>\n"
			<< "  <meta charset='utf-8'>\n"
			<< "  <title>Web Storage</title>\n"
			<< "  <style>\n"
			<< "    ul { list-style: none; padding-left: 0; }\n"
			<< "    li { padding: 4px 0; }\n"
			<< "    img { vertical-align: middle; margin-right: 5px; }\n"
			<< "  </style>\n"
			<< "</head>\n<body>\n";

	if (currentFolder.empty())
		listing << "  <h1>" << username << "'s Files in Storage Home</h1>\n";
	else
		listing << "  <h1>" << username << "'s Files in Folder " << currentFolder << "</h1>\n";
	cout << "Metadata (" << metaCol << "): " << metadata << endl;
	if (metadata.substr(0,3) == "ERR" || metadata.empty()) {
		listing << "  <p>No files or folders uploaded yet.</p>\n";
	} else {
		// Assume metadata is comma-separated list of local item names.
		vector<string> entries = splitStringStorage(metadata, ',');
		listing << "  <ul>\n";
		for (auto &item : entries) {
			cout<<"item:"<<item<<endl;
			if (item.empty())
				continue;
			item = item.substr(item.find_last_of("/")+1);
			// Compute absolute column name for this item.
			string absolutePath = currentFolder.empty() ? item : (currentFolder + "/" + item);
			string colValue = client.Get(baseKey, absolutePath);
			cout << "Column value for " << absolutePath << ": " << colValue << endl;
			if (colValue == "KV-Folder") {
				// It's a folder.
				listing << "    <li> "
						<< "<a href='/list?folder=" << absolutePath << "'>" << item << "</a>"
						<< " | <a href='/renameFolder?folder=" << absolutePath << "'>Rename</a>"
						<< " | <a href='/moveFolder?folder="<<absolutePath<<"'>Move</a>"
						<< " | <a href='/deleteStorage?path=" << absolutePath << "'>Delete</a>"
						<< "</li>\n";
			} else if(colValue == "KV-File") {
				// Otherwise, treating it as a file.
				listing << "    <li> "
						<< item
						<< " | <a href='/download?filename=" << item;
				if (!currentFolder.empty())
					listing << "&folder=" << currentFolder;
				listing << "'>Download</a>";
				listing << " | <a href='rename?filename="<<item;
				if (!currentFolder.empty())
					listing << "&folder=" << currentFolder;
				listing << "'>Rename</a>"
						<< " | <a href='deleteStorage?path=" <<absolutePath << "'>Delete</a>";
				listing << " | <a href='moveFile?filename="<<item;
				if (!currentFolder.empty())
					listing << "&folder=" << currentFolder;
				listing << "'>Move</a>";
				listing << "</li>\n";
			}
		}
		listing << "  </ul>\n";
	}

	// Append Upload File form; include current folder as hidden input.
	listing << "<h2>Upload File</h2>\n"
			<< "  <form method='POST' action='/upload' enctype='multipart/form-data'>\n";
	if (!currentFolder.empty()) {
		listing << "    <input type='hidden' name='folder' value='" << currentFolder << "'>\n";
	}
	listing << "    <p><label for='file'>Choose file to upload in this folder:</label>\n"
			<< "    <input type='file' id='file' name='file'></p>\n"
			<< "  <p>Valid characters in File Name: A-Z a-z 0-9 ( ) _ and space (and the . extension)</p>\n"
			<< "    <p><input type='submit' value='Upload'></p>\n"
			<< "  </form>\n";

	// Append Create Sub-folder form; hidden field "parentFolder" carries current folder.
	listing << "<h2>Create a Sub-folder</h2>\n"
			<< "  <form method='POST' action='/createSubFolder'>\n"
			<< "    <input type='hidden' name='parentFolder' value='" << currentFolder << "'>\n"
			 << "  <p>Valid characters in File Name: A-Z a-z 0-9 ( ) _ and space</p>\n"
			<< "    <p><label for='newFolder'>New Folder Name:</label>\n"
			<< "    <input type='text' id='newFolder' name='newfolder'></p>\n"
			<< "    <p><input type='submit' value='Create Folder'></p>\n"
			<< "  </form>\n";
	if (currentFolder.find("/") != string::npos) {
		listing << "<p><a href='/list?folder=" << currentFolder.substr(0,currentFolder.find_last_of('/')) << "'>Back to Parent folder</a></p>";
	} else if (url.find("folder") != string::npos){
		listing << "<p><a href='/storage'>Back to Storage Home</a></p>";
	}
	listing << "<p><a href='/logout'>Logout</a></p>";
	listing << "  <br/>\n</body>\n</html>\n";
	return httpResponseStorage("200 OK", listing.str(), "text/html");
}

// extractBoundary(request) should return "--<boundary>"
static std::string extractBoundary(const std::string &request) {
    auto ct = request.find("Content-Type: multipart/form-data;");
    if (ct == std::string::npos) return "";
    auto b  = request.find("boundary=", ct);
    if (b == std::string::npos) return "";
    b += 9; // skip "boundary="
    auto e = request.find("\r\n", b);
    return "--" + request.substr(b, e - b);
}

/// Read all of a multipart body, stopping once we see the final boundary marker.
//  This will block until the client actually sends the trailing `--<boundary>--`.
static std::vector<char> readMultipartBody(int clientSock,
                                           const std::string &request,
                                           size_t headerEnd)
{
    //figure out boundary markers
    std::string boundary = extractBoundary(request);
    if (boundary.empty()) throw std::runtime_error("No multipart boundary");
    // final boundary has `--` appended
    std::string finalMarker = "\r\n" + boundary + "--";

    //start with whatever the client already sent after the headers
    std::vector<char> buf;
    buf.insert(buf.end(),
               request.begin() + headerEnd + 4,
               request.end());

    // keep receiving until we see the final boundary inside buf
    while (true) {
        char tmp[65536];
        ssize_t n = ::recv(clientSock, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.insert(buf.end(), tmp, tmp + n);

        // only start searching once we're at least as big as the final marker
        if (buf.size() >= finalMarker.size()) {
            if (std::search(buf.begin(),
                            buf.end(),
                            finalMarker.begin(),
                            finalMarker.end())
                != buf.end())
            {
                break;
            }
        }
    }

    return buf;
}

// Parses out filename="…" from the multipart headers in headerPart.
// If no filename found, returns an empty string.
std::string parseMultipartFilenameStorageHeader(const std::string &headerPart) {
    const std::string marker = "filename=\"";
    //find the start of filename="
    size_t pos = headerPart.find(marker);
    if (pos == std::string::npos) {
        return "";
    }
    pos += marker.size();
    //find the closing quote
    size_t endPos = headerPart.find('"', pos);
    if (endPos == std::string::npos) {
        endPos = headerPart.size();
    }
    //extract raw value
    std::string filename = headerPart.substr(pos, endPos - pos);

    //strip any long paths—keep only the base name after last slash or backslash
    size_t slash = filename.find_last_of("/\\");
    if (slash != std::string::npos && slash + 1 < filename.size()) {
        filename = filename.substr(slash + 1);
    }
    return filename;
}



//send a completely binary payload with correct headers + Content-Disposition
void sendBinaryResponse(int clientSocket,
                        const std::string &statusLine,
                        const std::string &contentType,
                        const std::string &filename,
                        const std::string &payload)
{
    std::ostringstream hdr;
    hdr << statusLine << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << payload.size() << "\r\n"
        << "Content-Disposition: attachment; filename=\"" << filename << "\"\r\n"
        << "Connection: keep-alive\r\n"
        << "\r\n";

    auto headerStr = hdr.str();
    // send headers
    ::send(clientSocket, headerStr.data(), headerStr.size(), 0);
    // send raw binary
    ::send(clientSocket, payload.data(), payload.size(), 0);
}

string webStorageUpload(const string &username,
                             int clientSocket,
                             const string &request) {
	MasterNodeClient masterclient(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(master_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
	string worker = masterclient.Lookup(username+"-storage");  // worker 4
	cout<<"Worker returned from master L: "<<worker<<endl;

	if(worker.substr(0,3) == "ERR"){
		return httpResponseStorage("503 Service Unavailable","<html><body>Servers down, sorry for the inconvenience!</body></html>");
	}
	cout << "Worker address "<<worker << endl;
	string worker_address(worker);
	KeyValueStoreClient client(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(worker_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
//    auto client = makeClient(username);
    string baseKey = username + "-storage";
	string folderPath = ""; // current folder (relative path) in which to store the file

	//get folder from the URL.
	size_t firstLineEnd = request.find("\r\n");
	string requestLine = request.substr(0, firstLineEnd);
	size_t qmPos = requestLine.find("?");
	if (qmPos != string::npos) {
		size_t spaceAfterQuery = requestLine.find(" ", qmPos);
		string queryStr = requestLine.substr(qmPos + 1, spaceAfterQuery - qmPos - 1);
		auto params = parseQueryParamsStorage(queryStr);
		if (params.find("folder") != params.end())
			folderPath = params["folder"];
	}

	// find the multipart boundary string
	std::string boundary = extractBoundary(request);

	if (boundary.empty()) {
		return httpResponseStorage("400 Bad Request",
			"<html><body>No multipart boundary.</body></html>");
	}

	bool hasFolder = false;
	// If not found in URL, try to parse from body
	if (folderPath.empty()){
		folderPath = parseMultipartFieldStorage(request, "folder");
		if(!folderPath.empty()){
			hasFolder = true;
		}
	}
	// Process the body.
	size_t headerEnd = request.find("\r\n\r\n");
	if (headerEnd == string::npos)
		return httpResponseStorage("400 Bad Request", "<html><body>Missing header terminator.</body></html>");

	// parse filename out of the header block
	std::string headerPart = request.substr(headerEnd+4);
	if(hasFolder){
		size_t bound1 = request.find(boundary);
		headerPart = request.substr(bound1 + boundary.size());
	}
	std::string filename = parseMultipartFilenameStorageHeader(headerPart);
	// Parse the filename from multipart data.
	// Remove accidental slashes.
	size_t slashPos = filename.find("/");
	if (slashPos != string::npos)
		filename = filename.substr(slashPos + 1);


	if (filename.empty() || !isValidName(filename)) {
		return httpResponseStorage("400 Bad Request",
			"<html><body>Missing file name or incorrect format of filename.</body></html>");
	}

	//pull in entire body as a binary buffer
	// Check if Transfer Encoding
	cout<<"Before"<<endl;
	bool isChunked = request.find("Transfer-Encoding: chunked") != string::npos;
	vector<char> raw;
	if (isChunked) {
		raw = readChunkedBodyRaw(clientSocket);
	} else {
		raw = readMultipartBody(clientSocket, request, headerEnd);
	}
	cout<<"After"<<endl;

	// convert raw→string so we can search for CRLFCRLF and the final boundary
	std::string rawStr(raw.begin(), raw.end());

	//locate the header/data split
	auto hdrEnd = rawStr.find("\r\n\r\n");
	if (hdrEnd == std::string::npos) {
		return httpResponseStorage("400 Bad Request",
			"<html><body>Malformed multipart body.</body></html>");
	}
	size_t dataStart = hdrEnd + 4;
	if(hasFolder){
		string rawIntermediate = rawStr.substr(hdrEnd+4);
		size_t bound1 = rawIntermediate.find("\r\n\r\n");
		dataStart = hdrEnd + 4 + bound1 + 4;
	}
	//locate the final boundary (must start with CRLF + boundary)
	std::string finalMarker = "\r\n" + boundary;
	auto boundaryPos = rawStr.rfind(finalMarker);
	if (boundaryPos == std::string::npos || boundaryPos < dataStart) {
		boundaryPos = rawStr.size();
	}

	//extract just the file’s bytes
	std::string fileData(
	  rawStr.data() + dataStart,
	  boundaryPos - dataStart
	);

	// Compute the absolute file path.
	// For root, it's just filename; for subfolder, it's parentFolder+"/"+filename.
	string absolutePath = (folderPath.empty() ? filename : folderPath + "/" + filename);

	//Check if file name already exists. If exists, append a number.

	string resp = client.Get(baseKey, absolutePath);
	if(resp == "KV-File"){
		int i = 1;
		string newAbsolutePath;
		while(true){
			newAbsolutePath = absolutePath.substr(0,absolutePath.find(".")) + "("+to_string(i)+")"+absolutePath.substr(absolutePath.find("."));
			resp = client.Get(baseKey, newAbsolutePath);
			if(resp != "KV-File"){
				filename += "("+to_string(i)+")";
				break;
			}
			i++;
		}
		absolutePath = newAbsolutePath;
	}

	client.Put(baseKey, absolutePath, "KV-File");

	cout<<"File data: "<<fileData.substr(0,100)<<endl;
	int i = 1;
	int chunkSize = 1000000;
	int offset = 0;
	int fileSize = fileData.size();
	cout<<"File size: "<<fileSize<<endl;
	while(offset < fileSize){
		int len = min(chunkSize, fileSize - offset);
		string fileChunkName =  absolutePath + "-"+to_string(i);
		string fileChunk = fileData.substr(offset,len);
		offset += len;
		i++;
		string fileMetadata = client.Get(baseKey, absolutePath+"-metadata");
		cout<<"File metadata: "<<fileMetadata<<endl;
		if(fileMetadata.find("not found") != string::npos){
			fileMetadata = fileChunkName;
		} else {
			fileMetadata += ","+fileChunkName;
		}
		client.Put(baseKey, absolutePath+"-metadata",fileMetadata);

		// Store file content in column identified by absolutePath.
		auto result = client.Put(baseKey, fileChunkName+"-content", fileChunk);
		cerr << "[UPLOAD] Storing file '" << filename << "' at column '" << fileChunkName
			 << "' in row '" << baseKey << "', size=" << fileChunk.size() << endl;
	}

	// Update parent folder metadata.
	// Determine which metadata column to update:
	// For root: column "metadata"; for subfolder: column = folderPath.
	string metaCol = folderPath.empty() ? "metadata" : folderPath+"-metadata";
	string parentMeta = client.Get(baseKey, metaCol);
	string oldParentMeta = parentMeta;
	// If the parent's metadata does not already contain the local filename, add it.
	if (parentMeta.empty()) {
		parentMeta = absolutePath;
	} else {
		// Split into entries and check.
		vector<string> items = splitStringStorage(parentMeta, ',');
		bool found = false;
		for(auto &s : items) {
			if (s == filename) { found = true; break; }
		}
		if (!found)
			parentMeta += "," + absolutePath;
	}
	client.CPut(baseKey, metaCol, oldParentMeta, parentMeta);

	// Build response HTML.
	ostringstream html;
	html << "<html><body>Upload Result: UPLOADED"
		 << "<br/>File stored as <strong>" << filename << "</strong> in folder <strong>"
		 << (folderPath.empty() ? "root" : folderPath) << "</strong>."
		 << "<br/><a href='/list?folder=" << folderPath << "'>Back to folder</a>"
		 << "</body></html>";
	return httpResponseStorage("200 OK", html.str());
}

string webStorageDownload(const string &username,
							const int clientSocket,
                               const string &request) {
	MasterNodeClient masterclient(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(master_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
	string worker = masterclient.Lookup(username+"-storage");  // worker 4
	cout<<"Worker returned from master M: "<<worker<<endl;

	if(worker.substr(0,3) == "ERR"){
		return httpResponseStorage("503 Service Unavailable","<html><body>Servers down, sorry for the inconvenience!</body></html>");
	}
	cout << "Worker address "<<worker << endl;
	string worker_address(worker);
	KeyValueStoreClient client(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(worker_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
    //auto client = makeClient(username);
    // Parse query parameters.
	string url = request.substr(0, request.find("\r\n"));
	size_t qmPos = url.find("?");
	string queryStr = "";
	auto params = unordered_map<string, string>();
	if (qmPos != string::npos) {
		queryStr = url.substr(qmPos + 1, url.find(" ", qmPos + 1) - qmPos - 1);
		params = parseQueryParamsStorage(queryStr);
	}

	// Get filename and folder (if provided).
	string filename = (params.count("filename") ? params["filename"] : "myfile");
	string folderPath = (params.count("folder") ? params["folder"] : "");

	// Compute absolute file path.
	string absolutePath = (folderPath.empty() ? filename : folderPath + "/" + filename);
	cout<<"Absolute path: "<<absolutePath<<endl;

	string fileMetadata = client.Get(username + "-storage", absolutePath+"-metadata");
	vector<string> files = splitStringStorage(fileMetadata, ',');
	string fileContent = "";
	for(auto &fileChunk : files) {
		fileContent += client.Get(username + "-storage", fileChunk+"-content");
	}

	// Retrieve file content from column absolutePath.
    //string fileContent = client.Get(username + "-storage", absolutePath+"-content");
	cout<<"Bytes fileContent: "<<fileContent.size()<<endl;
	if (fileContent.empty()) {
		string notFound = "<html><body><h1>404 Not Found</h1><p>File not found.</p></body></html>";
		return httpResponseStorage("404 Not Found", notFound);
	}
	// build MIME + Content-Disposition header
string mime = get_content_type_storage(filename);
string cd   = "Content-Disposition: attachment; filename=\"" + filename + "\"";

// stream it in chunked mode
sendChunkedResponse(
    clientSocket,
    "HTTP/1.1 200 OK",
    mime,
    fileContent,
    cd
	);
	return "";

	}

string webStorageCreateFolder(const string &username,
                                   int clientSocket,
                                   const string &request) {
	MasterNodeClient masterclient(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(master_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
	string worker = masterclient.Lookup(username+"-storage");  // worker 4
	cout<<"Worker returned from master N: "<<worker<<endl;

	if(worker.substr(0,3) == "ERR"){
		return httpResponseStorage("503 Service Unavailable","<html><body>Servers down, sorry for the inconvenience!</body></html>");
	}
	cout << "Worker address "<<worker << endl;
	string worker_address(worker);
	KeyValueStoreClient client(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(worker_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
    //auto client = makeClient(username);
    // Base key for the single row.
	string baseKey = username + "-storage";

	// Determine the parent's folder (absolute path) from either the URL query parameter or the hidden form field.
	// Default is root (which we denote by an empty string).
	string parentFolder = "";  // parent's absolute path; empty means root.
	size_t firstLineEnd = request.find("\r\n");
	string requestLine = request.substr(0, firstLineEnd);
	size_t qmPos = requestLine.find("?");
	if (qmPos != string::npos) {
		size_t spaceAfterQuery = requestLine.find(" ", qmPos);
		string queryStr = requestLine.substr(qmPos + 1, spaceAfterQuery - qmPos - 1);
		auto params = parseQueryParamsStorage(queryStr);
		// We expect a query parameter named "folder" to indicate the current folder.
		if (params.find("folder") != params.end()) {
			parentFolder = params["folder"];  // e.g. "abc" or "abc/def"
		}
	}
	//check the POST body for a hidden field "parentFolder".
	size_t headerEnd = request.find("\r\n\r\n");
	if (headerEnd == string::npos) {
		return httpResponseStorage("400 Bad Request",
			"<html><body>Missing header terminator.</body></html>");
	}
	string body = readRequestBodyStorage(clientSocket, request, headerEnd);
	unordered_map<string, string> formFields = parseQueryParamsStorage(body);
	if (formFields.find("parentFolder") != formFields.end() && !formFields["parentFolder"].empty()) {
		parentFolder = formFields["parentFolder"];
	}

	// Parse new folder name from the form field "newfolder".
	string newFolder = "";
	if (formFields.find("newfolder") != formFields.end()) {
		newFolder = formFields["newfolder"];
	}

	if (newFolder.empty() || !isValidName(newFolder)) {
		return httpResponseStorage("400 Bad Request",
			"<html><body>Missing new folder name or incorrect folder name.</body></html>");
	}

	// Determine the parent's metadata column to update.
	// For root, use column "metadata"; otherwise, the parent's absolute path.
	string parentMetaCol = (parentFolder.empty() ? "metadata" : parentFolder+"-metadata");

	// Compute new folder's absolute path.
	// If parentFolder is empty, new folder's absolute path equals newFolder;
	// else, it is parentFolder + "/" + newFolder.
	string newFolderAbs = (parentFolder.empty() ? newFolder : parentFolder + "/" + newFolder);

	// Check if the new folder already exists (i.e. if column newFolderAbs exists).
	string newFolderCol = client.Get(baseKey, newFolderAbs);
	if (!newFolderCol.empty() && newFolderCol.substr(0,3) != "ERR" && newFolderCol != "DEL KV-Folder") {
		return httpResponseStorage("400 Bad Request",
			"<html><body>Folder already exists.</body></html>");
	}

	// Update parent's metadata: retrieve parent's metadata column.
	string parentMeta = client.Get(baseKey, parentMetaCol);
	// The parent's metadata is a comma-separated list of local names.
	vector<string> items = splitStringStorage(parentMeta, ',');
	bool exists = false;
	for (auto &s : items) {
		if (s == newFolder) {
			exists = true;
			break;
		}
	}
	if (!exists) {
		if (parentMeta.empty())
			parentMeta = newFolderAbs;
		else
			parentMeta += "," + newFolderAbs;
		client.Put(baseKey, parentMetaCol, parentMeta);
	}

	// Create the new folder column.
	// Here we initialize the new folder's column with marker "FOLDER" to denote that it is a folder.
	client.Put(baseKey, newFolderAbs, "KV-Folder");
	client.Put(baseKey, newFolderAbs+"-metadata", "");

	// Build and return an HTML response confirming the creation.
	ostringstream response;
	response << "<html><body>"
			 << "<h1>Folder Created</h1>"
			 << "<p>Folder '" << newFolder << "' has been created in '"
			 << (parentFolder.empty() ? "root" : parentFolder) << "'.</p>"
			 << "<p><a href='/list?folder=" << newFolderAbs << "'>View Folder</a></p>"
			 << "<p><a href='/storage'>Back to Storage</a></p>"
			 << "</body></html>";

	return httpResponseStorage("200 OK", response.str(), "text/html");
}

string webStorageDelete(const string &username,
                              int clientSocket,
                              const string &request) {
	MasterNodeClient masterclient(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(master_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
	string worker = masterclient.Lookup(username+"-storage");  // worker 4
	cout<<"Worker returned from master O: "<<worker<<endl;

	if(worker.substr(0,3) == "ERR"){
		return httpResponseStorage("503 Service Unavailable","<html><body>Servers down, sorry for the inconvenience!</body></html>");
	}
	cout << "Worker address "<<worker << endl;
	string worker_address(worker);
	KeyValueStoreClient client(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(worker_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
//    auto client = makeClient(username);
    string baseKey = username + "-storage";

	string url = request.substr(0, request.find("\r\n"));
	size_t qm = url.find("?");
	if (qm == string::npos) {
		return httpResponseStorage("400 Bad Request",
			"<html><body>Missing path parameter.</body></html>");
	}
	auto params = parseQueryParamsStorage(url.substr(qm+1, url.find(" ", qm)-qm-1));
	if (!params.count("path")) {
		return httpResponseStorage("400 Bad Request",
			"<html><body>Missing path parameter.</body></html>");
	}
	string absPath = params["path"];
	// determine parent folder + local name
	size_t slash = absPath.find_last_of('/');
	string parent = (slash == string::npos ? "" : absPath.substr(0, slash));
	string local  = (slash == string::npos ? absPath : absPath.substr(slash+1));

	// Update parent metadata: remove local
	string parentMetaCol = parent.empty() ? "metadata" : parent + "-metadata";
	string pmeta = client.Get(baseKey, parentMetaCol);
	auto items = splitStringStorage(pmeta, ',');
	vector<string> kept;
	for (auto &it : items) {
		if (it.find(local) == string::npos) kept.push_back(it);
	}
	// re‐join
	ostringstream joined;
	for (size_t i = 0; i < kept.size(); i++) {
		if (i) joined << ",";
		joined << kept[i];
	}
	client.Put(baseKey, parentMetaCol, joined.str());

	deleteRec(client, baseKey, absPath);

	// Check marker to see if file or folder
	string marker = client.Get(baseKey, absPath);

	// Response
	ostringstream resp;
	resp << "<html><body>";
	if (marker == "KV-Folder") {
		resp << "<p>Folder <strong>" << local << "</strong> deleted.</p>";
	} else {
		resp << "<p>File <strong>" << local << "</strong> deleted.</p>";
	}
	resp << "<p><a href='/list?folder=" << parent << "'>Back to ";
	resp << (parent.empty() ? "root" : parent) << "</a></p>";
	resp << "</body></html>";

	return httpResponseStorage("200 OK", resp.str(), "text/html");
}

// Rename file handler with chunked-support
string webStorageRenameFile(const string &username,
                             int clientSocket,
                             const string &request) {
	MasterNodeClient masterclient(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(master_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
	string worker = masterclient.Lookup(username+"-storage");  // worker 4
	cout<<"Worker returned from master P: "<<worker<<endl;

	if(worker.substr(0,3) == "ERR"){
		return httpResponseStorage("503 Service Unavailable","<html><body>Servers down, sorry for the inconvenience!</body></html>");
	}
	cout << "Worker address "<<worker << endl;
	string worker_address(worker);
	KeyValueStoreClient client(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(worker_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
//    auto client = makeClient(username);
    string baseKey = username + "-storage";

    bool isPost = request.rfind("POST /rename", 0) == 0;
    string currentFolder, oldName;

    if (!isPost) {
        // Serve rename form (GET)
        string url = request.substr(0, request.find("\r\n"));
        size_t qm = url.find('?');
        if (qm != string::npos) {
            auto params = parseQueryParamsStorage(
                              url.substr(qm+1, url.find(' ', qm) - qm - 1));
            if (params.count("folder"))    currentFolder = params["folder"];
            if (params.count("filename"))  oldName       = params["filename"];
        }
        ostringstream html;
        html << "<html><body>"
             << "<h1>Rename File</h1>"
             << "<form method='POST' action='/rename'>"
             << "  <input type='hidden' name='folder' value='" << currentFolder << "'>"
             << "  <input type='hidden' name='oldfilename' value='" << oldName << "'>"
             << "  <p>Current: <strong>" << oldName << "</strong></p>"
             << "  <p><label>New name: <input type='text' name='newfilename'></label></p>"
             << "  <p><input type='submit' value='Rename'></p>"
             << "</form>"
             << "<p><a href='/list?folder=" << currentFolder << "'>Back</a></p>"
             << "</body></html>";
        return httpResponseStorage("200 OK", html.str(), "text/html");
    }

    // Handle POST
    size_t hdrEnd = request.find("\r\n\r\n");
    string body = readRequestBodyStorage(clientSocket, request, hdrEnd);
    auto fields = parseQueryParamsStorage(body);
    currentFolder = fields["folder"];
    oldName       = fields["oldfilename"];
    string newName= fields["newfilename"];

    if (newName.empty() || !isValidName(newName)) {
        return httpResponseStorage("400 Bad Request",
            "<html><body>Invalid new filename.</body></html>");
    }
    // Preserve extension
    newName = ensureSameExtension(oldName, newName);

    // Absolute paths
    string oldAbs = currentFolder.empty() ? oldName : currentFolder + "/" + oldName;
    string newAbs = currentFolder.empty() ? newName : currentFolder + "/" + newName;

    // Check for name collision
    if (client.Get(baseKey, newAbs) == "KV-File") {
        return httpResponseStorage("400 Bad Request",
            "<html><body>File name already exists.</body></html>");
    }

    // Chunked rename: try reading chunk metadata
    string oldMetaCol = oldAbs + "-metadata";
    string fileMeta   = client.Get(baseKey, oldMetaCol);
    vector<string> oldChunks;
    vector<string> newChunks;
    if (!fileMeta.empty() && fileMeta.rfind("ERR",0) != 0) {
        // Parse and rename each chunk
        oldChunks = splitStringStorage(fileMeta, ',');
        for (auto &chunk : oldChunks) {
            string suffix = chunk.substr(oldAbs.size());
            string newChunk = newAbs + suffix;
            // Copy content
            string content = client.Get(baseKey, chunk + "-content");
            client.Put(baseKey, newChunk + "-content", content);
            client.Delete(baseKey, chunk + "-content");
            newChunks.push_back(newChunk);
        }
        // Write new metadata list
        string newMetaCol = newAbs + "-metadata";
        ostringstream mb;
        for (size_t i = 0; i < newChunks.size(); ++i) {
            if (i) mb << ',';
            mb << newChunks[i];
        }
        client.Put(baseKey, newMetaCol, mb.str());
        client.Delete(baseKey, oldMetaCol);
    } else {
        // Single-column file
        string content = client.Get(baseKey, oldAbs + "-content");
        client.Put(baseKey, newAbs + "-content", content);
        client.Delete(baseKey, oldAbs + "-content");
    }

    // Rename markers
    client.Put(baseKey, newAbs, "KV-File");
    client.Delete(baseKey, oldAbs);

    // Update folder listing
    string metaCol = currentFolder.empty() ? "metadata" : currentFolder + "-metadata";
    string meta = client.Get(baseKey, metaCol);
    auto entries = splitStringStorage(meta, ',');
    for (auto &e : entries) if (e == oldAbs) e = newAbs;
    ostringstream pm;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) pm << ',';
        pm << entries[i];
    }
    client.Put(baseKey, metaCol, pm.str());

    // Confirmation page
    ostringstream resp;
    resp << "<html><body>"
         << "<p>Renamed <strong>" << oldName << "</strong> to <strong>" << newName << "</strong>.</p>"
         << "<p><a href='/list?folder=" << currentFolder << "'>Back</a></p>"
         << "</body></html>";
    return httpResponseStorage("200 OK", resp.str(), "text/html");
}


string webStorageRenameFolder(const string &username,
                                   int clientSocket,
                                   const string &request) {
	MasterNodeClient masterclient(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(master_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
	string worker = masterclient.Lookup(username+"-storage");  // worker 4
	cout<<"Worker returned from master Q: "<<worker<<endl;

	if(worker.substr(0,3) == "ERR"){
		return httpResponseStorage("503 Service Unavailable","<html><body>Servers down, sorry for the inconvenience!</body></html>");
	}
	cout << "Worker address "<<worker << endl;
	string worker_address(worker);
	KeyValueStoreClient client(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(worker_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
//    auto client = makeClient(username);
    string baseKey = username + "-storage";

	// Detect GET vs POST
	bool isPost = request.rfind("POST /renameFolder", 0) == 0;
	string folderPath, oldName, newName;

	if (!isPost) {
		// GET: parse folder=absolutePath
		string url = request.substr(0, request.find("\r\n"));
		size_t qm = url.find("?");
		if (qm != string::npos) {
			auto params = parseQueryParamsStorage(url.substr(qm+1, url.find(" ", qm)-qm-1));
			if (params.count("folder")) {
				folderPath = params["folder"];
				// oldName = last segment
				auto pos = folderPath.find_last_of('/');
				oldName = (pos==string::npos? folderPath : folderPath.substr(pos+1));
			}
		}
		// render form
		ostringstream html;
		html << "<html><body>"
			 << "<h1>Rename Folder</h1>"
			 << "<form method='POST' action='/renameFolder'>\n"
			 << "  <input type='hidden' name='folder' value='" << folderPath << "'>\n"
			 << "  <p>Current name: <strong>" << oldName << "</strong></p>\n"
			 << "  <p>Valid characters in File Name: A-Z a-z 0-9 ( ) _ and space</p>\n"
			 << "  <p><label>New name: <input type='text' name='newname'></label></p>\n"
			 << "  <p><input type='submit' value='Rename'></p>\n"
			 << "</form>"
			 << "<p><a href='/list?folder=" << folderPath << "'>Back</a></p>"
			 << "</body></html>";
		return httpResponseStorage("200 OK", html.str(), "text/html");
	}

	// POST: perform rename
	size_t hdrEnd = request.find("\r\n\r\n");
	string body = readRequestBodyStorage(clientSocket, request, hdrEnd);
	auto fields = parseQueryParamsStorage(body);
	folderPath = fields["folder"];
	newName    = fields["newname"];
	if (newName.empty() || !isValidName(newName)) {
		return httpResponseStorage("400 Bad Request",
			"<html><body>Missing new folder name or incorrect format of folder name.</body></html>");
	}

	// Determine parent metadata column
	// parent path = everything before last '/'
	size_t slash = folderPath.find_last_of('/');
	string parent = (slash == string::npos ? "" : folderPath.substr(0, slash));
	string parentMetaCol = parent.empty() ? "metadata" : parent + "-metadata";

	// oldAbsolute and newAbsolute
	string oldAbs = folderPath;
	string oldLocal = folderPath.substr(slash==string::npos?0:slash+1);
	string newAbs = (parent.empty() ? newName : parent + "/" + newName);

	string existFolder = client.Get(baseKey, newAbs);
	if (existFolder == "KV-Folder") {
		return httpResponseStorage("400 Bad Request",
			"<html><body>Folder with same name already exists.</body></html>");
	}
	// update parent metadata: replace oldAbs with newAbs
	string pmeta = client.Get(baseKey, parentMetaCol);
	auto parts = splitStringStorage(pmeta, ',');
	for (auto &p : parts) {
		if (p == oldAbs) p = newAbs;
	}
	// join back
	ostringstream joined;
	for (int i = 0; i < (int)parts.size(); i++) {
		if (i) joined << ",";
		joined << parts[i];
	}
//	joined << "," << newAbs;
	client.Put(baseKey, parentMetaCol, joined.str());

	// recursively rename the folder and its children
	renameFolderRec(client, baseKey, oldAbs, newAbs);

	// response
	ostringstream resp;
	resp << "<html><body>"
		 << "<p>Folder <strong>" << oldLocal << "</strong> renamed to <strong>" << newName << "</strong>.</p>"
		 << "<p><a href='/list?folder=" << newAbs << "'>View \"" << newName << "\"</a></p>"
		 << "</body></html>";
	return httpResponseStorage("200 OK", resp.str(), "text/html");
}



// Helper to build a JSON string representing the folder hierarchy
static string buildFolderTreeJSON(
    KeyValueStoreClient& client,
    const string& baseKey,
    const string& folderPath
) {
    // Determine metadata column
    string metaCol = folderPath.empty()
        ? "metadata"
        : folderPath + "-metadata";
    string metadata = client.Get(baseKey, metaCol);
    if (metadata.rfind("ERR", 0) == 0 || metadata.empty()) {
        return "[]";
    }
    auto entries = splitStringStorage(metadata, ',');
    ostringstream ss;
    ss << "[";
    bool first = true;
    for (auto& fullPath : entries) {
        // Only include folders in the move dialog
        string marker = client.Get(baseKey, fullPath);
        if (marker != "KV-Folder") continue;
        if (!first) ss << ",";
        first = false;
        // local name is the segment after last '/'
        size_t p = fullPath.find_last_of('/');
        string name = (p == string::npos ? fullPath : fullPath.substr(p + 1));
        ss << "{";
        ss << "\"name\":\"" << name << "\",";
        ss << "\"path\":\"" << fullPath << "\",";
        ss << "\"children\":"
           << buildFolderTreeJSON(client, baseKey, fullPath)
           << "}";
    }
    ss << "]";
    return ss.str();
}


// Handler for GET  and POST (perform move)
string webStorageMoveFile(const string &username,
                          int clientSocket,
                          const string &request) {
	MasterNodeClient masterclient(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(master_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
	string worker = masterclient.Lookup(username+"-storage");  // worker 4
	cout<<"Worker returned from master R: "<<worker<<endl;

	if(worker.substr(0,3) == "ERR"){
		return httpResponseStorage("503 Service Unavailable","<html><body>Servers down, sorry for the inconvenience!</body></html>");
	}
	cout << "Worker address "<<worker << endl;
	string worker_address(worker);
	KeyValueStoreClient client(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(worker_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
//    auto client = makeClient(username);
    string baseKey = username + "-storage";

    bool isPost = request.rfind("POST /moveFile", 0) == 0;
    if (!isPost) {
        // GET: render multi-step, form-based UI
        string url = request.substr(0, request.find("\r\n"));
        size_t qm = url.find('?');
        string queryStr;
        if (qm != string::npos)
            queryStr = url.substr(qm + 1,
                                  url.find(' ', qm) - qm - 1);

        auto params = parseQueryParamsStorage(queryStr);
        string sourceFolder, filename, currentFolder;
        if (params.count("folder"))        sourceFolder   = params["folder"];
        if (params.count("filename"))      filename       = params["filename"];
        if (params.count("currentFolder")) currentFolder  = params["currentFolder"];

        // Fetch this level's folder list
        string metaCol = currentFolder.empty()
                         ? "metadata"
                         : currentFolder + "-metadata";
        string metadata = client.Get(baseKey, metaCol);
        if (metadata.rfind("ERR", 0) == 0) metadata.clear();

        ostringstream html;
        html << "<!DOCTYPE html>\n<html>\n<head>\n"
             << "  <meta charset='utf-8'>\n"
             << "  <title>Move File</title>\n"
             << "  <style>\n"
             << "    ul { list-style:none; padding-left:0; }\n"
             << "    li { margin:4px 0; }\n"
             << "    form { display:inline; }\n"
             << "  </style>\n"
             << "</head>\n<body>\n"
             << "  <h1>Move file <strong>" << filename << "</strong> into:</h1>\n"
             << (currentFolder.empty()
                   ? "  <p>Current target: <strong>Root</strong></p>\n"
                   : "  <p>Current target: <strong>" + currentFolder + "</strong></p>\n")
             << "  <ul>\n";

        // Home (root) option
                html << "    <li>\n"
                     << "      <form method='POST' action='/moveFile'>\n"
                     << "        <input type='hidden' name='sourceFolder' value='" << sourceFolder << "'>\n"
                     << "        <input type='hidden' name='filename'     value='" << filename     << "'>\n"
                     << "        <input type='hidden' name='targetFolder' value=''>\n"
                     << "        <button type='submit'>Home</button>\n"
                     << "      </form>\n"
                     << "    </li>\n";



        if (!metadata.empty()) {
            auto entries = splitStringStorage(metadata, ',');
            for (auto &fullPath : entries) {
                // Only show subfolders
                if (client.Get(baseKey, fullPath) != "KV-Folder") continue;
                // Local name = after last '/'
                size_t slash = fullPath.find_last_of('/');
                string local = (slash == string::npos)
                               ? fullPath
                               : fullPath.substr(slash + 1);


                html << "    <li>\n"
                     << "      <a href='/moveFile?"
                        "folder="   << sourceFolder
                     << "&filename="  << filename
                     << "&currentFolder=" << fullPath
                     << "'>" << local << "/</a>\n"
                     // Move-here form
                     << "      <form method='POST' action='/moveFile'>\n"
                     << "        <input type='hidden' name='sourceFolder' value='" << sourceFolder << "'>\n"
                     << "        <input type='hidden' name='filename'     value='" << filename     << "'>\n"
                     << "        <input type='hidden' name='targetFolder' value='" << fullPath     << "'>\n"
                     << "        <button type='submit'>Move Here</button>\n"
                     << "      </form>\n"
                     << "    </li>\n";
            }
        }

        html << "  </ul>\n";

        // “Up” or “Cancel” link
        if (!currentFolder.empty()) {
            size_t slash = currentFolder.find_last_of('/');
            string parent = (slash == string::npos)
                            ? ""
                            : currentFolder.substr(0, slash);
            html << "  <p><a href='/moveFile?"
                 << "folder="    << sourceFolder
                 << "&filename=" << filename;
            if (!parent.empty())
                html << "&currentFolder=" << parent;
            html << "'>Up</a></p>\n";
        } else {
            html << "  <p><a href='/list?folder=" << sourceFolder
                 << "'>Cancel</a></p>\n";
        }

        html << "</body>\n</html>\n";
        return httpResponseStorage("200 OK", html.str(), "text/html");
    }

    // POST
    size_t hdrEnd = request.find("\r\n\r\n");
    string body   = readRequestBodyStorage(clientSocket, request, hdrEnd);
    auto fields   = parseQueryParamsStorage(body);

    string sourceFolder = fields["sourceFolder"];
    string filename     = fields["filename"];
    string targetFolder = fields["targetFolder"];
    if (filename.empty()) {
        return httpResponseStorage("400 Bad Request",
            "<html><body>Missing filename or target folder.</body></html>");
    }

    string oldAbs = sourceFolder.empty()
                    ? filename
                    : sourceFolder + "/" + filename;
    moveFile(client, baseKey, oldAbs, targetFolder);

    ostringstream resp;
    resp << "<!DOCTYPE html>\n<html>\n<body>\n"
         << "<p>Moved file <strong>" << filename
         << "</strong> to <strong>"
         << (targetFolder.empty() ? "Root" : targetFolder)
         << "</strong>.</p>\n"
         << "<p><a href='/list?folder=" << targetFolder
         << "'>View Folder</a></p>\n"
         << "</body>\n</html>\n";
    return httpResponseStorage("200 OK", resp.str(), "text/html");
}



// GET-only, multi-step,HTML UI for moving folders
string webStorageMoveFolder(const string &username,
                            int clientSocket,
                            const string &request) {
	MasterNodeClient masterclient(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(master_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
	string worker = masterclient.Lookup(username+"-storage");  // worker 4
	cout<<"Worker returned from master S: "<<worker<<endl;

	if(worker.substr(0,3) == "ERR"){
		return httpResponseStorage("503 Service Unavailable","<html><body>Servers down, sorry for the inconvenience!</body></html>");
	}
	cout << "Worker address "<<worker << endl;
	string worker_address(worker);
	KeyValueStoreClient client(
	  // Channel from which RPCs are made - endpoint is the target_address
	  grpc::CreateChannel(worker_address,
						  // Indicate when channel is not authenticated
						  grpc::InsecureChannelCredentials()));
//    auto client = makeClient(username);
    string baseKey = username + "-storage";

    bool isPost = request.rfind("POST /moveFolder", 0) == 0;
    if (!isPost) {
        // GET branch:
        // Parse query string
        string url = request.substr(0, request.find("\r\n"));
        size_t qm  = url.find('?');
        string query;
        if (qm != string::npos)
            query = url.substr(qm + 1, url.find(' ', qm) - qm - 1);

        auto params = parseQueryParamsStorage(query);
        // folder being moved
        string sourceFolder = params.count("folder")
                              ? params["folder"] : "";
        // folder we are currently browsing as target
        string currentFolder = params.count("currentFolder")
                               ? params["currentFolder"] : "";

        // List only subfolders of currentFolder
        string metaCol = currentFolder.empty()
                         ? "metadata"
                         : currentFolder + "-metadata";
        string metadata = client.Get(baseKey, metaCol);
        if (metadata.rfind("ERR", 0) == 0) metadata.clear();
        vector<string> children = metadata.empty()
                                  ? vector<string>()
                                  : splitStringStorage(metadata, ',');

        // Build HTML
        ostringstream html;
        html << "<!DOCTYPE html>\n<html>\n<head>\n"
             << "  <meta charset='utf-8'>\n"
             << "  <title>Move Folder</title>\n"
             << "  <style>\n"
             << "    ul { list-style:none; padding-left:0; }\n"
             << "    li { margin:4px 0; }\n"
             << "    form { display:inline; }\n"
             << "  </style>\n"
             << "</head>\n<body>\n"
             << "  <h1>Move folder <strong>" << sourceFolder
             << "</strong> into:</h1>\n"
             << "  <p>Current target: <strong>"
             << (currentFolder.empty() ? "Root" : currentFolder)
             << "</strong></p>\n"
             << "  <ul>\n";

        //Home (root) option
                html << "    <li>\n"
                     << "      <form method='POST' action='/moveFolder'>\n"
                     << "        <input type='hidden' name='folder' value='" << sourceFolder << "'>\n"
                     << "        <input type='hidden' name='targetFolder' value=''>\n"
                     << "        <button type='submit'>Home</button>\n"
                     << "      </form>\n"
                     << "    </li>\n";


        for (const auto &fullPath : children) {
            if (client.Get(baseKey, fullPath) != "KV-Folder")
                continue;
            size_t slash = fullPath.find_last_of('/');
            string local = (slash == string::npos
                            ? fullPath
                            : fullPath.substr(slash + 1));

            html << "    <li>\n"
                 << "      <a href='/moveFolder?"
                 << "folder=" << sourceFolder
                 << "&currentFolder=" << fullPath
                 << "'>" << local << "/</a>\n"
                 // Move-here form
                 << "      <form method='POST' action='/moveFolder'>\n"
                 << "        <input type='hidden' name='folder' value='"
                 << sourceFolder << "'>\n"
                 << "        <input type='hidden' name='targetFolder' value='"
                 << fullPath << "'>\n"
                 << "        <button type='submit'>Move Here</button>\n"
                 << "      </form>\n"
                 << "    </li>\n";
        }
        html << "  </ul>\n";

        // “Up” or “Cancel” link
        if (!currentFolder.empty()) {
            size_t slash = currentFolder.find_last_of('/');
            string parent = (slash == string::npos
                             ? ""
                             : currentFolder.substr(0, slash));
            html << "  <p><a href='/moveFolder?"
                 << "folder=" << sourceFolder;
            if (!parent.empty())
                html << "&currentFolder=" << parent;
            html << "'>Up</a></p>\n";
        } else {
            // Cancel back to listing of sourceFolder's parent
            size_t slashS = sourceFolder.find_last_of('/');
            string srcParent = (slashS == string::npos
                                ? ""
                                : sourceFolder.substr(0, slashS));
            html << "  <p><a href='/list?folder=" << srcParent
                 << "'>Cancel</a></p>\n";
        }

        html << "</body>\n</html>\n";
        return httpResponseStorage("200 OK", html.str(), "text/html");
    }

    // POST branch
    size_t hdrEnd = request.find("\r\n\r\n");
    string body  = readRequestBodyStorage(clientSocket, request, hdrEnd);
    auto fields  = parseQueryParamsStorage(body);

    string sourceFolder = fields["folder"];
    string targetFolder = fields["targetFolder"];
    if (targetFolder == sourceFolder ||
            targetFolder.rfind(sourceFolder + "/", 0) == 0)
        {
            return httpResponseStorage(
                "400 Bad Request",
                "<html><body>Cannot move a folder into itself.</body></html>"
            );
        }
    //name-collision guard
    {
        // extract just the last segment of sourceFolder
        size_t slash = sourceFolder.find_last_of('/');
        string childName = (slash == string::npos
                            ? sourceFolder
                            : sourceFolder.substr(slash + 1));

        // read target’s metadata column
        string metaCol = targetFolder.empty()
                         ? "metadata"
                         : targetFolder + "-metadata";
        string targetMeta = client.Get(baseKey, metaCol);
        if (targetMeta.rfind("ERR", 0) == 0) targetMeta.clear();

        // split into full-paths, then pull out each local name
        auto entries = splitStringStorage(targetMeta, ',');
        for (auto &entry : entries) {
            size_t s = entry.find_last_of('/');
            string local = (s == string::npos
                            ? entry
                            : entry.substr(s + 1));
            if (local == childName) {
                return httpResponseStorage("400 Bad Request",
                    "<html><body>"
                    "Cannot move: a folder or file named <strong>"
                    + childName +
                    "</strong> already exists in the destination.</body></html>"
                );
            }
        }
    }

    if (sourceFolder.empty()) {
        return httpResponseStorage("400 Bad Request",
            "<html><body>Missing source or target folder.</body></html>");
    }


    moveFolder(client, baseKey, sourceFolder, targetFolder);



    ostringstream resp;
    resp << "<!DOCTYPE html>\n<html>\n<body>\n"
         << "<p>Moved folder <strong>" << sourceFolder
         << "</strong> into <strong>"
         << (targetFolder.empty() ? "Root" : targetFolder)
         << "</strong>.</p>\n"
         << "<p><a href='/list?folder=" << targetFolder
         << "'>View Destination</a></p>\n"
         << "</body>\n</html>\n";
    return httpResponseStorage("200 OK", resp.str(), "text/html");
}


// LIST
// username: already authenticated by http_server
string StorageService::list(const string &username,
                                 const string &request) {
    return webStorageList(username, request);
}

//UPLOAD
string StorageService::upload(const string &username,
								const int client,
                                   const string &request) {
    return webStorageUpload(username, client, request);
}

//DOWNLOAD
string StorageService::download(const string &username,
									const int client,
                                     const string &request) {
    return webStorageDownload(username, client, request);
}

// CREATE FOLDER
string StorageService::createFolder(const string &username,
									  const int client,
                                         const string &request) {
    return webStorageCreateFolder(username, client, request);
}

//DELETE
string StorageService::remove(const string &username,
								const int client,
                                   const string &request) {
    return webStorageDelete(username, client, request);
}

//RENAME FILE
string StorageService::renameFile(const string &username,
									const int client,
                                       const string &request) {
    return webStorageRenameFile(username, client, request);
}

// RENAME FOLDER
string StorageService::renameFolder(const string &username,
									  const int client,
                                         const string &request) {
    return webStorageRenameFolder(username, client, request);
}



//  MOVE FILE
string StorageService::moveFile(const string &username,
								 const int client,
                                 const string &request) {
    return webStorageMoveFile(username, client, request);
}

// MOVE FOLDER
string StorageService::moveFolder(const string &username,
                                  const int client,
                                  const string &request) {
    return webStorageMoveFolder(username, client, request);
}
