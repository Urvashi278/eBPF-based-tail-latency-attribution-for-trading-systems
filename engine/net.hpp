// Minimal ws:// wss:// http:// https:// client on raw sockets + OpenSSL.
// Just enough for exchange market-data feeds: TLS with SNI + hostname verification,
// HTTP/1.1 GET (Content-Length or chunked), RFC 6455 client frames (masking,
// fragmentation, ping/pong, close). No other dependencies.
#pragma once
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/evp.h>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace net {

struct Url {
    bool tls = false;
    std::string host, port, path;
};

inline Url parse_url(const std::string& u) {
    Url r;
    size_t p = u.find("://");
    if (p == std::string::npos) throw std::runtime_error("bad url: " + u);
    std::string scheme = u.substr(0, p);
    r.tls = scheme == "wss" || scheme == "https";
    std::string rest = u.substr(p + 3);
    size_t slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    r.path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) { r.host = hostport.substr(0, colon); r.port = hostport.substr(colon + 1); }
    else { r.host = hostport; r.port = r.tls ? "443" : "80"; }
    return r;
}

class Conn {
public:
    explicit Conn(const Url& u) : url_(u) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res)
            throw std::runtime_error("resolve failed: " + u.host);
        for (addrinfo* a = res; a; a = a->ai_next) {
            fd_ = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd_ < 0) continue;
            if (connect(fd_, a->ai_addr, a->ai_addrlen) == 0) break;
            close(fd_); fd_ = -1;
        }
        freeaddrinfo(res);
        if (fd_ < 0) throw std::runtime_error("connect failed: " + u.host + ":" + u.port);
        int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (u.tls) {
            ctx_ = SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_default_verify_paths(ctx_);
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
            ssl_ = SSL_new(ctx_);
            SSL_set_tlsext_host_name(ssl_, u.host.c_str());
            SSL_set1_host(ssl_, u.host.c_str());
            SSL_set_fd(ssl_, fd_);
            if (SSL_connect(ssl_) != 1) {
                char e[256]; ERR_error_string_n(ERR_get_error(), e, sizeof e);
                throw std::runtime_error(std::string("TLS handshake failed: ") + e);
            }
        }
    }
    ~Conn() {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); }
        if (ctx_) SSL_CTX_free(ctx_);
        if (fd_ >= 0) close(fd_);
    }
    Conn(const Conn&) = delete;

    void write_all(const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        while (n) {
            ssize_t w = ssl_ ? SSL_write(ssl_, c, (int)n) : ::send(fd_, c, n, MSG_NOSIGNAL);
            if (w <= 0) throw std::runtime_error("write failed");
            c += w; n -= (size_t)w;
        }
    }
    // Buffered reads
    bool fill() {
        if (rpos_ > 0 && rpos_ == rlen_) rpos_ = rlen_ = 0;
        if (rlen_ == sizeof(rbuf_)) {                       // compact
            memmove(rbuf_, rbuf_ + rpos_, rlen_ - rpos_); rlen_ -= rpos_; rpos_ = 0;
        }
        ssize_t r = ssl_ ? SSL_read(ssl_, rbuf_ + rlen_, (int)(sizeof(rbuf_) - rlen_))
                         : ::recv(fd_, rbuf_ + rlen_, sizeof(rbuf_) - rlen_, 0);
        if (r <= 0) return false;
        rlen_ += (size_t)r;
        return true;
    }
    void read_exact(void* out, size_t n) {
        char* o = static_cast<char*>(out);
        while (n) {
            if (rpos_ == rlen_ && !fill()) throw std::runtime_error("connection closed");
            size_t k = std::min(n, rlen_ - rpos_);
            memcpy(o, rbuf_ + rpos_, k); rpos_ += k; o += k; n -= k;
        }
    }
    std::string read_line() {                                // up to CRLF
        std::string s;
        for (;;) {
            if (rpos_ == rlen_ && !fill()) throw std::runtime_error("connection closed");
            char c = rbuf_[rpos_++];
            if (c == '\n') { if (!s.empty() && s.back() == '\r') s.pop_back(); return s; }
            s += c;
        }
    }
    std::string read_to_eof() {
        std::string s(rbuf_ + rpos_, rlen_ - rpos_); rpos_ = rlen_ = 0;
        while (fill()) { s.append(rbuf_, rlen_); rpos_ = rlen_ = 0; }
        return s;
    }
    const Url& url() const { return url_; }
    int fd() const { return fd_; }

private:
    Url url_;
    int fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    char rbuf_[1 << 16];
    size_t rpos_ = 0, rlen_ = 0;
};

inline std::string lower(std::string s) { for (auto& c : s) c = (char)tolower(c); return s; }

// Returns (status, headers-lowercased-joined, body). Handles chunked + Content-Length.
inline std::string read_http_response(Conn& c, int& status, std::string& headers) {
    std::string line = c.read_line();
    if (line.size() < 12) throw std::runtime_error("bad http status line: " + line);
    status = atoi(line.c_str() + 9);
    long clen = -1; bool chunked = false;
    headers.clear();
    while (!(line = c.read_line()).empty()) {
        std::string l = lower(line);
        headers += l + "\n";
        if (l.rfind("content-length:", 0) == 0) clen = atol(l.c_str() + 15);
        if (l.rfind("transfer-encoding:", 0) == 0 && l.find("chunked") != std::string::npos) chunked = true;
    }
    return std::to_string(clen) + (chunked ? "c" : "n");   // caller decides how to read body
}

inline std::string http_get(const std::string& url, int* status_out = nullptr) {
    Url u = parse_url(url);
    Conn c(u);
    std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host +
                      "\r\nUser-Agent: kslat/1.0\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
    c.write_all(req.data(), req.size());
    int status; std::string headers;
    std::string how = read_http_response(c, status, headers);
    if (status_out) *status_out = status;
    std::string body;
    if (how.back() == 'c') {
        for (;;) {
            long n = strtol(c.read_line().c_str(), nullptr, 16);
            if (n <= 0) break;
            std::string chunk(n, '\0'); c.read_exact(chunk.data(), n);
            body += chunk; c.read_line();
        }
    } else {
        long clen = atol(how.c_str());
        if (clen >= 0) { body.resize(clen); c.read_exact(body.data(), clen); }
        else body = c.read_to_eof();
    }
    return body;
}

inline std::string b64(const unsigned char* d, size_t n) {
    std::string out(4 * ((n + 2) / 3), '\0');
    int k = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), d, (int)n);
    out.resize(k);
    return out;
}

class WebSocket {
public:
    explicit WebSocket(const std::string& url) : c_(parse_url(url)) {
        unsigned char key[16]; RAND_bytes(key, sizeof key);
        std::string k = b64(key, sizeof key);
        const Url& u = c_.url();
        std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + k +
                          "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: kslat/1.0\r\n\r\n";
        c_.write_all(req.data(), req.size());
        int status; std::string headers;
        read_http_response(c_, status, headers);
        if (status != 101) throw std::runtime_error("websocket upgrade failed, http " + std::to_string(status));
        std::string expect = k + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        unsigned char sha[SHA_DIGEST_LENGTH];
        SHA1(reinterpret_cast<const unsigned char*>(expect.data()), expect.size(), sha);
        if (headers.find("sec-websocket-accept: " + lower(b64(sha, sizeof sha))) == std::string::npos)
            throw std::runtime_error("bad Sec-WebSocket-Accept");
    }

    void send_text(const std::string& s) { send_frame(0x1, s.data(), s.size()); }

    // Blocks until a complete text/binary message arrives. Handles ping/pong/close.
    // Returns false when the server closes.
    bool recv(std::string& msg) {
        msg.clear();
        for (;;) {
            unsigned char h[2]; c_.read_exact(h, 2);
            bool fin = h[0] & 0x80; int op = h[0] & 0x0f;
            bool masked = h[1] & 0x80; uint64_t len = h[1] & 0x7f;
            if (len == 126) { unsigned char e[2]; c_.read_exact(e, 2); len = (e[0] << 8) | e[1]; }
            else if (len == 127) { unsigned char e[8]; c_.read_exact(e, 8); len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | e[i]; }
            unsigned char mask[4] = {0, 0, 0, 0};
            if (masked) c_.read_exact(mask, 4);
            std::string payload(len, '\0');
            if (len) c_.read_exact(payload.data(), len);
            if (masked) for (uint64_t i = 0; i < len; i++) payload[i] ^= mask[i & 3];
            if (op == 0x9) { send_frame(0xA, payload.data(), payload.size()); continue; }   // ping
            if (op == 0xA) continue;                                                        // pong
            if (op == 0x8) { send_frame(0x8, payload.data(), std::min<size_t>(payload.size(), 2)); return false; }
            msg += payload;
            if (fin) return true;
        }
    }

private:
    void send_frame(int op, const char* d, size_t n) {
        std::string f;
        f += char(0x80 | op);
        if (n < 126) f += char(0x80 | n);
        else if (n < 65536) { f += char(0x80 | 126); f += char(n >> 8); f += char(n & 0xff); }
        else { f += char(0x80 | 127); for (int i = 7; i >= 0; i--) f += char((n >> (8 * i)) & 0xff); }
        unsigned char m[4]; RAND_bytes(m, 4);
        f.append(reinterpret_cast<char*>(m), 4);
        for (size_t i = 0; i < n; i++) f += char(d[i] ^ m[i & 3]);
        c_.write_all(f.data(), f.size());
    }
    Conn c_;
};

} // namespace net
