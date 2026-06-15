#include <iouring/http/WebServer.h>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST(WebServerSignal, RequestStopUnblocksWaiters) {
    iouring::http::WebServer::ResetStopRequestedForTests();

    bool returned = false;
    std::thread waiter([&] {
        iouring::http::WebServer::WaitForStopSignal(10ms);
        returned = true;
    });

    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(returned);

    iouring::http::WebServer::RequestStop();
    waiter.join();

    EXPECT_TRUE(returned);
    EXPECT_TRUE(iouring::http::WebServer::StopRequested());

    iouring::http::WebServer::ResetStopRequestedForTests();
}
