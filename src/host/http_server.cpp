#include "host/http_server.hpp"
#include "common/embedded_assets.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sstream>
#include <iostream>
#include <cstring>

static void writeAll(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) {
            return;
        }
        sent += static_cast<size_t>(n);
    }
}

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::stop() {
    running = false;
    if (serverFd != -1) {
        close(serverFd);
        serverFd = -1;
    }
}

void HttpServer::start(uint16_t port, const std::function<std::string(std::string)> &getOffer,
                       const std::function<void(std::string, std::string)> &onAnswerReceived) {
    serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd == -1) {
        return;
    }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(serverFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(serverFd);
        serverFd = -1;
        return;
    }

    if (listen(serverFd, 10) < 0) {
        close(serverFd);
        serverFd = -1;
        return;
    }

    running = true;
    while (running) {
        int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (!running)
                break;
            continue;
        }

        char buffer[4096] = {0};
        ssize_t bytesRead = read(clientFd, buffer, sizeof(buffer) - 1);
        if (bytesRead <= 0) {
            close(clientFd);
            continue;
        }

        std::string request(buffer);
        std::string response;

        if (request.find("GET / ") != std::string::npos) {
            std::string content(INDEX_HTML.begin(), INDEX_HTML.end());
            std::ostringstream oss;
            oss << "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " << content.size() << "\r\nConnection: close\r\n\r\n"
                << content;
            response = oss.str();
        } else if (request.find("GET /offer") != std::string::npos) {
            size_t idPos = request.find("id=");
            std::string clientId = "client-unknown";
            if (idPos != std::string::npos) {
                size_t spacePos = request.find(' ', idPos);
                size_t ampPos = request.find('&', idPos);
                size_t endPos = std::min(spacePos, ampPos);
                clientId = request.substr(idPos + 3, endPos - (idPos + 3));
            }
            std::string offer = getOffer(clientId);
            std::ostringstream oss;
            if (offer.empty()) {
                static const std::string full = "session full";
                oss << "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nContent-Length: " << full.size()
                    << "\r\nConnection: close\r\n\r\n"
                    << full;
            } else {
                oss << "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " << offer.size() << "\r\nConnection: close\r\n\r\n"
                    << offer;
            }
            response = oss.str();
        } else if (request.find("GET /xterm.js") != std::string::npos) {
            std::string content(XTERM_JS.begin(), XTERM_JS.end());
            std::ostringstream oss;
            oss << "HTTP/1.1 200 OK\r\nContent-Type: application/javascript\r\nContent-Length: " << content.size()
                << "\r\nConnection: close\r\n\r\n"
                << content;
            response = oss.str();
        } else if (request.find("GET /xterm.css") != std::string::npos) {
            std::string content(XTERM_CSS.begin(), XTERM_CSS.end());
            std::ostringstream oss;
            oss << "HTTP/1.1 200 OK\r\nContent-Type: text/css\r\nContent-Length: " << content.size() << "\r\nConnection: close\r\n\r\n"
                << content;
            response = oss.str();
        } else if (request.find("GET /xterm-addon-fit.js") != std::string::npos) {
            std::string content(XTERM_ADDON_FIT_JS.begin(), XTERM_ADDON_FIT_JS.end());
            std::ostringstream oss;
            oss << "HTTP/1.1 200 OK\r\nContent-Type: application/javascript\r\nContent-Length: " << content.size()
                << "\r\nConnection: close\r\n\r\n"
                << content;
            response = oss.str();
        } else if (request.find("POST /answer") != std::string::npos) {
            size_t idPos = request.find("id=");
            std::string clientId = "client-unknown";
            if (idPos != std::string::npos) {
                size_t spacePos = request.find(' ', idPos);
                size_t ampPos = request.find('&', idPos);
                size_t endPos = std::min(spacePos, ampPos);
                clientId = request.substr(idPos + 3, endPos - (idPos + 3));
            }
            size_t clPos = request.find("Content-Length:");
            size_t bodyPos = request.find("\r\n\r\n");
            size_t contentLength = 0;
            if (clPos != std::string::npos) {
                size_t endLine = request.find("\r\n", clPos);
                std::string clVal = request.substr(clPos + 15, endLine - (clPos + 15));
                size_t first = clVal.find_first_not_of(" \t");
                try {
                    contentLength = (first != std::string::npos) ? std::stoul(clVal.substr(first)) : 0;
                } catch (const std::exception &) {
                    const char *bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    writeAll(clientFd, bad, std::strlen(bad));
                    close(clientFd);
                    continue;
                }
            }
            std::string body;
            if (bodyPos != std::string::npos) {
                body = request.substr(bodyPos + 4);
            }
            while (body.size() < contentLength) {
                char readBuf[1024] = {0};
                ssize_t n = read(clientFd, readBuf, sizeof(readBuf) - 1);
                if (n <= 0)
                    break;
                body.append(readBuf, n);
            }
            std::string sdpAnswer = body;
            response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            writeAll(clientFd, response.data(), response.size());
            close(clientFd);
            onAnswerReceived(clientId, sdpAnswer);
            continue;
        } else {
            response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        }

        if (!response.empty()) {
            writeAll(clientFd, response.data(), response.size());
        }
        close(clientFd);
    }
    stop();
}
