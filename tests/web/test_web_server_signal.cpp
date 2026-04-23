#include <iouring_runtime/web/WebServer.h>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST(WebServerSignal, RequestStopUnblocksWaiters) {
    iouring_runtime::web::WebServer::ResetStopRequestedForTests();

    bool returned = false;
    std::thread waiter([&] {
        iouring_runtime::web::WebServer::WaitForStopSignal(10ms);
        returned = true;
    });

    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(returned);

    iouring_runtime::web::WebServer::RequestStop();
    waiter.join();

    EXPECT_TRUE(returned);
    EXPECT_TRUE(iouring_runtime::web::WebServer::StopRequested());

    iouring_runtime::web::WebServer::ResetStopRequestedForTests();
}
