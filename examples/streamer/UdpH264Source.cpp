// UdpH264Source.cpp
#include "UdpH264Source.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <cstring>
#include <iostream>

UdpH264Source::UdpH264Source(uint16_t port) : port(port), sockfd(-1) {}

UdpH264Source::~UdpH264Source() {
    stop();
}

void UdpH264Source::start() {
    running = true;
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        sockfd = -1;
        return;
    }
    recvThread = std::thread(&UdpH264Source::receiveLoop, this);
}

void UdpH264Source::stop() {
    running = false;
    if (sockfd >= 0) close(sockfd);
    if (recvThread.joinable()) recvThread.join();
}

void UdpH264Source::receiveLoop() {
    static const uint8_t annexb4[4] = {0x00, 0x00, 0x00, 0x01};
    static const uint8_t annexb3[3] = {0x00, 0x00, 0x01};
    const size_t MIN_NALU_SIZE = 16; // Minimum size for a valid NALU
    std::vector<uint8_t> naluBuffer;
    bool seenSPS = false, seenPPS = false, seenIDR = false;
    while (running) {
        uint8_t buf[65536];
        ssize_t len = recv(sockfd, buf, sizeof(buf), 0);
        if (len > 0) {
            naluBuffer.insert(naluBuffer.end(), buf, buf + len);
            auto begin = naluBuffer.begin();
            auto end = naluBuffer.end();
            while (true) {
                // Find first start code (3 or 4 bytes)
                auto naluStart4 = std::search(begin, end, annexb4, annexb4 + 4);
                auto naluStart3 = std::search(begin, end, annexb3, annexb3 + 3);
                auto naluStart = (naluStart4 != end && (naluStart3 == end || naluStart4 < naluStart3)) ? naluStart4 : naluStart3;
                int startCodeLen = (naluStart == naluStart4) ? 4 : 3;
                if (naluStart == end) break;
                auto naluDataStart = naluStart + startCodeLen;
                if (naluDataStart >= end) break;
                // Find next start code after this NALU
                auto nextNalu4 = std::search(naluDataStart, end, annexb4, annexb4 + 4);
                auto nextNalu3 = std::search(naluDataStart, end, annexb3, annexb3 + 3);
                auto nextNalu = (nextNalu4 != end && (nextNalu3 == end || nextNalu4 < nextNalu3)) ? nextNalu4 : nextNalu3;
                if (nextNalu == end) {
                    // Incomplete NALU, wait for more data
                    break;
                }
                auto naluEnd = nextNalu;
                size_t naluDataSize = std::distance(naluDataStart, naluEnd);
                if (naluDataSize > 0) {
                    uint8_t naluType = (*(naluDataStart)) & 0x1F;
                    // Only allow very small NALUs for SPS/PPS/SEI
                    if (naluDataSize < 16 && (naluType != 7 && naluType != 8 && naluType != 6)) {
                        naluBuffer.erase(naluBuffer.begin(), naluEnd);
                        begin = naluBuffer.begin();
                        end = naluBuffer.end();
                        continue;
                    }
                    // Buffer latest SPS/PPS/IDR for initialNALUs
                    if (naluType == 7) {
                        std::lock_guard<std::mutex> nlock(nalus_mtx);
                        latestSPS.assign(reinterpret_cast<std::byte*>(&(*(naluDataStart - startCodeLen))), reinterpret_cast<std::byte*>(&(*naluEnd)));
                    }
                    if (naluType == 8) {
                        std::lock_guard<std::mutex> nlock(nalus_mtx);
                        latestPPS.assign(reinterpret_cast<std::byte*>(&(*(naluDataStart - startCodeLen))), reinterpret_cast<std::byte*>(&(*naluEnd)));
                    }
                    if (naluType == 5) {
                        std::lock_guard<std::mutex> nlock(nalus_mtx);
                        latestIDR.assign(reinterpret_cast<std::byte*>(&(*(naluDataStart - startCodeLen))), reinterpret_cast<std::byte*>(&(*naluEnd)));
                    }
                    // Convert Annex B to length-prefixed NALU
                    size_t naluSize = std::distance(naluDataStart, naluEnd);
                    std::vector<std::byte> lengthPrefixed;
                    uint32_t len = htonl(static_cast<uint32_t>(naluSize));
                    std::byte* lenBytes = reinterpret_cast<std::byte*>(&len);
                    lengthPrefixed.insert(lengthPrefixed.end(), lenBytes, lenBytes + 4);
                    lengthPrefixed.insert(lengthPrefixed.end(), reinterpret_cast<std::byte*>(&(*naluDataStart)), reinterpret_cast<std::byte*>(&(*naluEnd)));
                    // For IDR, prepend SPS/PPS to the sample for browser robustness
                    if (naluType == 5) {
                        std::vector<std::byte> idrSample;
                        // Prepend latest SPS/PPS (as length-prefixed)
                        auto appendLengthPrefixed = [&](const rtc::binary& nalu) {
                            if (nalu.empty()) return;
                            size_t offset = 0;
                            if (nalu.size() >= 4 && nalu[0] == std::byte{0x00} && nalu[1] == std::byte{0x00} && nalu[2] == std::byte{0x00} && nalu[3] == std::byte{0x01}) offset = 4;
                            else if (nalu.size() >= 3 && nalu[0] == std::byte{0x00} && nalu[1] == std::byte{0x00} && nalu[2] == std::byte{0x01}) offset = 3;
                            size_t naluSize = nalu.size() - offset;
                            uint32_t len = htonl(static_cast<uint32_t>(naluSize));
                            std::byte* lenBytes = reinterpret_cast<std::byte*>(&len);
                            idrSample.insert(idrSample.end(), lenBytes, lenBytes + 4);
                            idrSample.insert(idrSample.end(), nalu.begin() + offset, nalu.end());
                        };
                        {
                            std::lock_guard<std::mutex> nlock(nalus_mtx);
                            appendLengthPrefixed(latestSPS);
                            appendLengthPrefixed(latestPPS);
                        }
                        // Add the IDR itself
                        idrSample.insert(idrSample.end(), lengthPrefixed.begin(), lengthPrefixed.end());
                        {
                            std::lock_guard<std::mutex> lock(mtx);
                            samples.emplace(rtc::binary(idrSample.begin(), idrSample.end()));
                            cv.notify_one();
                        }
                    } else {
                        // Non-IDR: push as usual
                        std::lock_guard<std::mutex> lock(mtx);
                        samples.emplace(rtc::binary(lengthPrefixed.begin(), lengthPrefixed.end()));
                        cv.notify_one();
                    }
                }
                naluBuffer.erase(naluBuffer.begin(), naluEnd);
                begin = naluBuffer.begin();
                end = naluBuffer.end();
            }
            if (naluBuffer.size() > 1024*1024) naluBuffer.clear();
        }
    }
    // On exit, push any remaining complete NALU (only if fully delimited)
    auto begin = naluBuffer.begin();
    auto end = naluBuffer.end();
    auto naluStart4 = std::search(begin, end, annexb4, annexb4 + 4);
    auto naluStart3 = std::search(begin, end, annexb3, annexb3 + 3);
    auto naluStart = (naluStart4 != end && (naluStart3 == end || naluStart4 < naluStart3)) ? naluStart4 : naluStart3;
    int startCodeLen = (naluStart == naluStart4) ? 4 : 3;
    if (naluStart != end) {
        auto naluDataStart = naluStart + startCodeLen;
        if (naluDataStart < end) {
            // Look for next start code after this NALU
            auto nextNalu4 = std::search(naluDataStart, end, annexb4, annexb4 + 4);
            auto nextNalu3 = std::search(naluDataStart, end, annexb3, annexb3 + 3);
            auto nextNalu = (nextNalu4 != end && (nextNalu3 == end || nextNalu4 < nextNalu3)) ? nextNalu4 : nextNalu3;
            if (nextNalu != end) {
                auto naluEnd = nextNalu;
                size_t naluSize = std::distance(naluDataStart, naluEnd);
                uint8_t naluType = (*(naluDataStart)) & 0x1F;
                if (naluSize >= 16 || naluType == 7 || naluType == 8 || naluType == 6) {
                    std::lock_guard<std::mutex> lock(mtx);
                    samples.emplace(
                        rtc::binary(
                            reinterpret_cast<std::byte*>(&(*naluDataStart)),
                            reinterpret_cast<std::byte*>(&(*naluDataStart)) + naluSize
                        )
                    );
                    cv.notify_one();
                }
            }
            // else: do not push incomplete NALU
        }
    }
}

void UdpH264Source::loadNextSample() {
    std::unique_lock<std::mutex> lock(mtx);
    if (samples.empty()) {
        cv.wait(lock, [&]{ return !samples.empty() || !running; });
    }
    if (!samples.empty()) {
        currentSample = std::move(samples.front());
        samples.pop();
        sampleTime_us += sampleDuration_us;
    } else {
        currentSample.clear(); // Ensure currentSample is empty if nothing is available
    }
}

rtc::binary UdpH264Source::getSample() {
    return currentSample;
}

uint64_t UdpH264Source::getSampleTime_us() {
    return sampleTime_us;
}

uint64_t UdpH264Source::getSampleDuration_us() {
    return sampleDuration_us;
}

// Returns a buffer containing SPS, PPS, and IDR (if available), all as length-prefixed NALUs
rtc::binary UdpH264Source::getInitialNALUs() {
    std::lock_guard<std::mutex> lock(nalus_mtx);
    rtc::binary out;
    auto appendLengthPrefixed = [&](const rtc::binary& nalu) {
        if (nalu.empty()) return;
        // Find start code length
        size_t offset = 0;
        if (nalu.size() >= 4 && nalu[0] == std::byte{0x00} && nalu[1] == std::byte{0x00} && nalu[2] == std::byte{0x00} && nalu[3] == std::byte{0x01}) {
            offset = 4;
        } else if (nalu.size() >= 3 && nalu[0] == std::byte{0x00} && nalu[1] == std::byte{0x00} && nalu[2] == std::byte{0x01}) {
            offset = 3;
        }
        size_t naluSize = nalu.size() - offset;
        uint32_t len = htonl(static_cast<uint32_t>(naluSize));
        std::byte* lenBytes = reinterpret_cast<std::byte*>(&len);
        out.insert(out.end(), lenBytes, lenBytes + 4);
        out.insert(out.end(), nalu.begin() + offset, nalu.end());
    };
    appendLengthPrefixed(latestSPS);
    appendLengthPrefixed(latestPPS);
    appendLengthPrefixed(latestIDR);
    return out;
}
