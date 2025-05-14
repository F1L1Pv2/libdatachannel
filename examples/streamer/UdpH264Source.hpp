// UdpH264Source.hpp
// StreamSource implementation for receiving H264 over UDP
#pragma once
#include "stream.hpp"
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <netinet/in.h>

class UdpH264Source : public StreamSource {
    int sockfd;
    std::thread recvThread;
    std::atomic<bool> running{false};
    std::queue<rtc::binary> samples;
    std::mutex mtx;
    std::condition_variable cv;
    uint16_t port;
    uint64_t sampleTime_us = 0;
    uint64_t sampleDuration_us = 33333; // ~30fps by default
    rtc::binary currentSample;

    // Buffer for latest SPS, PPS, and IDR NALUs
    rtc::binary latestSPS;
    rtc::binary latestPPS;
    rtc::binary latestIDR;
    std::mutex nalus_mtx;

    void receiveLoop();
public:
    UdpH264Source(uint16_t port);
    ~UdpH264Source();
    void start() override;
    void stop() override;
    void loadNextSample() override;
    rtc::binary getSample() override;
    uint64_t getSampleTime_us() override;
    uint64_t getSampleDuration_us() override;
};
