// UdpH264Source.hpp
// StreamSource implementation for receiving H264 over UDP
// Improvements: Increased UDP receive buffer, added debug logging for packet arrival and timing.
#pragma once
#include "stream.hpp"
#include <atomic>
#include <netinet/in.h>
#include <thread>
#include "concurrentqueue.h"

class UdpH264Source : public StreamSource {
    int sockfd;
    std::atomic<bool> running{false};
    moodycamel::ConcurrentQueue<rtc::binary> samples;
    uint16_t port;
    uint64_t sampleTime_us = 0;
    uint64_t sampleDuration_us;
    rtc::binary currentSample;

    std::thread recvThread;
    void receiveLoop();
public:
    UdpH264Source(uint16_t port, uint32_t fps = 15);
    ~UdpH264Source();
    void start() override;
    void stop() override;
    void loadNextSample() override;
    rtc::binary getSample() override;
    uint64_t getSampleTime_us() override;
    uint64_t getSampleDuration_us() override;
};
