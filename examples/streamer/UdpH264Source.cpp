// UdpH264Source.cpp
#include "UdpH264Source.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <cstring>
#include <iostream>

UdpH264Source::UdpH264Source(uint16_t port, uint32_t fps) : port(port), sockfd(-1) {
    sampleDuration_us = 1000 * 1000 / fps;
}

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
    uint8_t buf[1048576]; // 1 MB buffer
    while (running)
    {
        ssize_t len = recv(sockfd, buf, sizeof(buf), 0);
        if (len > 0) {
            rtc::binary sample;
            sample.insert(sample.end(), reinterpret_cast<std::byte*>(buf), reinterpret_cast<std::byte*>(buf + len));
            samples.enqueue(std::move(sample));
        }
    }
    
}

void UdpH264Source::loadNextSample() {
    rtc::binary sample;
    if (samples.try_dequeue(sample)) {
        currentSample = std::move(sample);
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