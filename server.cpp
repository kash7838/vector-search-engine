// Local web server: serves the frontend, accepts CSV/JSON uploads, runs searches.
//   ./server [port]     (default 8080, listens on 127.0.0.1 only)
#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <map>
#include <netinet/in.h>
#include <sys/socket.h>
#include "VectorEngine.hpp"

static const char* DB_FILE = "vectors.bin";
static const size_t MAX_BODY = 512ull * 1024 * 1024;  // 512 MB upload cap
static MappedDatabase g_db;

struct Request {
    std::string method, path, query, body;
    std::map<std::string, std::string> headers;  // lower-case keys
};

static std::string urlDecode(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            r += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') r += ' ';
        else r += s[i];
    }
    return r;
}

static std::string queryParam(const std::string& q, const std::string& key) {
    size_t pos = 0;
    while (pos < q.size()) {
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos) amp = q.size();
        std::string pair = q.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) return urlDecode(pair.substr(eq + 1));
        pos = amp + 1;
    }
    return "";
}

static std::string jsonEscape(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else if (c < 0x20) r += ' ';
        else r += c;
    }
    return r;
}

static bool readRequest(int fd, Request& req, std::string& err, int& status) {
    std::string buf;
    char chunk[65536];
    size_t header_end = std::string::npos;
    while ((header_end = buf.find("\r\n\r\n")) == std::string::npos) {
        if (buf.size() > 32768) { err = "Headers too large"; status = 431; return false; }
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) { err = "Connection closed"; status = 400; return false; }
        buf.append(chunk, n);
    }
    std::istringstream hs(buf.substr(0, header_end));
    std::string line, target, version;
    std::getline(hs, line);
    std::istringstream rl(line);
    rl >> req.method >> target >> version;
    size_t qm = target.find('?');
    req.path = target.substr(0, qm);
    if (qm != std::string::npos) req.query = target.substr(qm + 1);
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c), v = line.substr(c + 1);
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char ch) { return std::tolower(ch); });
        v.erase(0, v.find_first_not_of(" \t"));
        req.headers[k] = v;
    }
    size_t len = 0;
    if (req.headers.count("content-length")) {
        char* end = nullptr;
        unsigned long long l = std::strtoull(req.headers["content-length"].c_str(), &end, 10);
        if (l > MAX_BODY) { err = "File too large (limit 512 MB)"; status = 413; return false; }
        len = static_cast<size_t>(l);
    }
    req.body = buf.substr(header_end + 4);
    req.body.reserve(len);
    while (req.body.size() < len) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) { err = "Upload interrupted"; status = 400; return false; }
        req.body.append(chunk, n);
    }
    return true;
}

static void sendResponse(int fd, int status, const std::string& ctype, const std::string& body) {
    const char* text = status == 200 ? "OK" : status == 404 ? "Not Found" : status == 413 ? "Payload Too Large" : "Bad Request";
    std::ostringstream h;
    h << "HTTP/1.1 " << status << " " << text << "\r\nContent-Type: " << ctype
      << "\r\nContent-Length: " << body.size() << "\r\nConnection: close\r\n\r\n";
    std::string out = h.str() + body;
    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = send(fd, out.data() + sent, out.size() - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}

static void sendError(int fd, int status, const std::string& msg) {
    sendResponse(fd, status, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(msg) + "\"}");
}

static std::string statusJson() {
    if (!g_db.isOpen()) return "{\"ok\":true,\"loaded\":false}";
    return "{\"ok\":true,\"loaded\":true,\"count\":" + std::to_string(g_db.count()) + ",\"dim\":" + std::to_string(g_db.dim()) + "}";
}

static void handleUpload(int fd, const Request& req) {
    std::string name = queryParam(req.query, "name"), err;
    VectorData vd;
    if (req.body.empty()) return sendError(fd, 400, "The file is empty.");
    if (!loadVectorText(name, req.body, vd, err)) return sendError(fd, 400, err);
    g_db.close();  // release the old mapping before overwriting the file
    if (!saveDatabase(DB_FILE, vd, err) || !g_db.open(DB_FILE, err)) return sendError(fd, 400, err);
    std::cout << "Imported " << name << ": " << vd.count() << " vectors, dim " << vd.dim << "\n";
    sendResponse(fd, 200, "application/json",
                 "{\"ok\":true,\"file\":\"" + jsonEscape(name) + "\",\"count\":" + std::to_string(vd.count()) +
                 ",\"dim\":" + std::to_string(vd.dim) + "}");
}

static void handleSearch(int fd, const Request& req) {
    if (!g_db.isOpen()) return sendError(fd, 400, "Upload a vector file first.");
    JsonValue body;
    std::string err;
    if (!JsonParser(req.body).parse(body, err)) return sendError(fd, 400, err);

    int k = 5;
    if (const JsonValue* kv = body.get("k")) if (kv->type == JsonValue::Num) k = static_cast<int>(kv->num);
    k = std::max(1, std::min(k, 1000));

    std::vector<float> query;
    std::string used = "vector #0 from the database (no query given)";
    const JsonValue* q = body.get("query");
    if (q && q->type == JsonValue::Arr && !q->nums.empty()) {
        query = q->nums;
        used = "your query";
    } else {
        query.assign(g_db.vectors(), g_db.vectors() + g_db.dim());
    }
    if (query.size() != static_cast<size_t>(g_db.dim()))
        return sendError(fd, 400, "Query has " + std::to_string(query.size()) + " values but the database is " +
                                      std::to_string(g_db.dim()) + "-dimensional.");

    auto t0 = std::chrono::high_resolution_clock::now();
    auto results = searchTopK(g_db.vectors(), g_db.count(), g_db.dim(), query, k);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - t0).count();

    std::ostringstream o;
    o << "{\"ok\":true,\"used\":\"" << used << "\",\"scanned\":" << g_db.count() << ",\"latency_us\":" << us << ",\"results\":[";
    for (size_t i = 0; i < results.size(); ++i) {
        char sc[32];
        snprintf(sc, sizeof(sc), "%.6f", results[i].score);
        o << (i ? "," : "") << "{\"id\":" << results[i].id << ",\"score\":" << sc << "}";
    }
    o << "]}";
    sendResponse(fd, 200, "application/json", o.str());
}

static void handle(int fd) {
    Request req;
    std::string err;
    int status = 400;
    if (!readRequest(fd, req, err, status)) return sendError(fd, status, err);

    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
        std::string html;
        if (!readWholeFile("frontend/index.html", html)) return sendResponse(fd, 404, "text/plain", "frontend/index.html not found - run the server from the project folder.");
        return sendResponse(fd, 200, "text/html; charset=utf-8", html);
    }
    if (req.method == "GET" && req.path == "/status") return sendResponse(fd, 200, "application/json", statusJson());
    if (req.method == "POST" && req.path == "/upload") return handleUpload(fd, req);
    if (req.method == "POST" && req.path == "/search") return handleSearch(fd, req);
    sendResponse(fd, 404, "text/plain", "Not found");
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    int port = argc > 1 ? std::atoi(argv[1]) : 8080;

    std::string err;
    if (g_db.open(DB_FILE, err)) std::cout << "Loaded existing " << DB_FILE << " (" << g_db.count() << " vectors)\n";

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // local access only
    addr.sin_port = htons(port);
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(srv, 16) < 0) {
        std::cerr << "Cannot listen on port " << port << " (is it already in use?)\n";
        return 1;
    }
    std::cout << "Vector search running at http://localhost:" << port << "\n";

    while (true) {
        int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) continue;
        timeval tv{30, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        handle(fd);
        close(fd);
    }
}