#include "base/seek_coordinator.h"
#include <cassert>
#include <iostream>

using namespace decoder_sdk::internal;

void test_initial_state()
{
    SeekCoordinator coordinator;
    auto state = coordinator.getState();
    assert(state.phase == SeekPhase::kIdle);
    assert(state.targetPosSec < 0);
    assert(!coordinator.isSeekActive());
    std::cout << "test_initial_state passed" << std::endl;
}

void test_request_path()
{
    SeekCoordinator coordinator;
    coordinator.beginSeekRequest(10.5);

    auto state = coordinator.getState();
    assert(state.phase == SeekPhase::kRequested);
    assert(state.targetPosSec == 10.5);
    assert(coordinator.isSeekActive());

    coordinator.commitSeek(10.5, 100, 200);
    state = coordinator.getState();
    assert(state.phase == SeekPhase::kCommitted);
    assert(state.targetPosSec == 10.5);
    assert(state.videoSerial == 100);
    assert(state.audioSerial == 200);
    assert(!state.videoReached);
    assert(!state.audioReached);

    // Video reaches target
    coordinator.reportReachedTarget(true, 100);
    state = coordinator.getState();
    assert(state.phase == SeekPhase::kCommitted);
    assert(state.videoReached);
    assert(!state.audioReached);

    // Audio reaches target -> Should auto finish
    coordinator.reportReachedTarget(false, 200);
    state = coordinator.getState();
    assert(state.phase == SeekPhase::kIdle);
    assert(!coordinator.isSeekActive());

    std::cout << "test_request_path passed" << std::endl;
}

void test_single_stream_seek()
{
    SeekCoordinator coordinator;
    coordinator.beginSeekRequest(15.0);

    // Only video stream (audioSerial = 0)
    coordinator.commitSeek(15.0, 300, 0);
    auto state = coordinator.getState();
    assert(state.phase == SeekPhase::kCommitted);
    assert(!state.videoReached);
    assert(state.audioReached); // Should be true since serial was 0

    coordinator.reportReachedTarget(true, 300);
    state = coordinator.getState();
    assert(state.phase == SeekPhase::kIdle);

    std::cout << "test_single_stream_seek passed" << std::endl;
}

void test_fail_path()
{
    SeekCoordinator coordinator;
    coordinator.beginSeekRequest(20.0);

    coordinator.failSeek(20.0);
    auto state = coordinator.getState();
    assert(state.phase == SeekPhase::kIdle);
    assert(!coordinator.isSeekActive());

    std::cout << "test_fail_path passed" << std::endl;
}

void test_concurrent_seek()
{
    SeekCoordinator coordinator;
    coordinator.beginSeekRequest(30.0);

    // New seek request before previous one committed
    coordinator.beginSeekRequest(40.0);
    auto state = coordinator.getState();
    assert(state.phase == SeekPhase::kRequested);
    assert(state.targetPosSec == 40.0);

    // Commit the latest one
    coordinator.commitSeek(40.0, 500, 600);
    state = coordinator.getState();
    assert(state.phase == SeekPhase::kCommitted);
    assert(state.targetPosSec == 40.0);

    std::cout << "test_concurrent_seek passed" << std::endl;
}

int main()
{
    test_initial_state();
    test_request_path();
    test_single_stream_seek();
    test_fail_path();
    test_concurrent_seek();

    std::cout << "All SeekCoordinator tests passed!" << std::endl;
    return 0;
}
