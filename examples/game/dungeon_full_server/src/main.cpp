#include "types.h"
#include "net/io_worker.h"
#include "net/io_worker_pool.h"
#include "net/game_session.h"
#include "game/player_context.h"
#include "game/room_manager.h"
#include "db/memory_db_service.h"
#include "db/sqlite_db_service.h"

#include <spdlog/spdlog.h>
#include <signal.h>
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>

static std::atomic<bool> g_running{true};
void SignalHandler(int) { g_running = false; }

int main() {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    constexpr std::uint16_t kPort = 7777;
    constexpr std::uint16_t kWorkerCount = 2;

    spdlog::info("GameServer starting on port {}...", kPort);

    // 글로벌 서비스
    PlayerManager player_manager;
    IoWorkerPool worker_pool(kWorkerCount);
    RoomManager room_manager(worker_pool.GetGlobalQueue(), &worker_pool,
                             worker_pool.GetTimer());

    std::unique_ptr<DbService> db;

    const char* db_env = std::getenv("GAMESERVER_DB");
    if (db_env && std::string(db_env) == "memory") {
        db = std::make_unique<MemoryDbService>();
        spdlog::info("DB backend: memory");
    } else {
        const char* path = std::getenv("GAMESERVER_SQLITE_PATH");
        db = std::make_unique<SqliteDbService>(path ? path : "gameserver.db");
        spdlog::info("DB backend: SQLite ({})", path ? path : "gameserver.db");
    }

    // 세션 ID 생성기
    static std::atomic<iouring_runtime::core::SessionId> g_next_sid{1};

    // 각 IoWorker 개별 시작
    iouring_runtime::core::Address addr{"0.0.0.0", kPort};

    for (std::uint16_t i = 0; i < kWorkerCount; ++i) {
        auto* worker = worker_pool.GetWorker(i);

        iouring_runtime::core::io::SessionFactory factory =
            [worker, &player_manager, &room_manager, db_ptr = db.get()]
            (int fd, iouring_runtime::core::ring::IoRing& ring,
             iouring_runtime::core::buffer::BufferPool& pool,
             iouring_runtime::core::ContextId shard_id)
            -> iouring_runtime::core::io::SessionRef
        {
            auto session = std::make_shared<GameSession>(fd, ring, pool, worker);
            auto sid = g_next_sid.fetch_add(1);
            session->SetSessionId(sid);
            session->SetServices(&player_manager, &room_manager, db_ptr);

            worker->AddSession(sid, session);
            return session;
        };

        worker->Start(addr, std::move(factory));
    }

    spdlog::info("GameServer ready - {} workers, port {}", kWorkerCount, kPort);

    // 메인 스레드: 시그널 대기 + 주기적 빈 Room 정리
    int cleanup_counter = 0;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (++cleanup_counter >= 10) {
            cleanup_counter = 0;
            room_manager.CleanupEmptyRooms();
        }
    }

    spdlog::info("Shutting down...");
    worker_pool.StopAll();
    spdlog::info("GameServer stopped.");

    return 0;
}
