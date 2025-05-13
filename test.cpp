// udp_nalu_type_printer.cpp
#include <iostream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <algorithm>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <port>\n";
        return 1;
    }
    int port = std::stoi(argv[1]);
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }

    static const uint8_t annexb4[4] = {0x00, 0x00, 0x00, 0x01};
    static const uint8_t annexb3[3] = {0x00, 0x00, 0x01};
    std::vector<uint8_t> buffer;
    while (true) {
        uint8_t buf[65536];
        ssize_t len = recv(sockfd, buf, sizeof(buf), 0);
        if (len <= 0) continue;
        buffer.insert(buffer.end(), buf, buf + len);

        auto begin = buffer.begin();
        auto end = buffer.end();
        while (true) {
            // Find first start code (3 or 4 bytes)
            auto naluStart4 = std::search(begin, end, annexb4, annexb4 + 4);
            auto naluStart3 = std::search(begin, end, annexb3, annexb3 + 3);
            auto naluStart = (naluStart4 != end && (naluStart3 == end || naluStart4 < naluStart3)) ? naluStart4 : naluStart3;
            int startCodeLen = (naluStart == naluStart4) ? 4 : 3;
            if (naluStart == end) break;
            auto naluDataStart = naluStart + startCodeLen;
            if (naluDataStart >= end) break;
            auto nextNalu4 = std::search(naluDataStart, end, annexb4, annexb4 + 4);
            auto nextNalu3 = std::search(naluDataStart, end, annexb3, annexb3 + 3);
            auto nextNalu = (nextNalu4 != end && (nextNalu3 == end || nextNalu4 < nextNalu3)) ? nextNalu4 : nextNalu3;
            size_t naluSize = std::distance(naluStart, nextNalu != end ? nextNalu : end);
            if (naluSize > startCodeLen) {
                uint8_t naluType = (*(naluDataStart)) & 0x1F;
                std::cout << "NALU type: " << int(naluType) << std::endl;
            }
            if (nextNalu != end) {
                begin = nextNalu;
            } else {
                if (naluStart != buffer.begin())
                    buffer.erase(buffer.begin(), naluStart);
                break;
            }
        }
        if (buffer.size() > 1024*1024) buffer.clear();
    }
    close(sockfd);
    return 0;
}