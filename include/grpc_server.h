#ifndef STREAM_DAEMON_GRPC_SERVER_H_
#define STREAM_DAEMON_GRPC_SERVER_H_

#include "common.h"
#include "model_registry.h"
#include "stream_manager.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

// Forward declaration for generated protobuf
namespace stream {
class StreamService;
}

namespace stream_daemon {

/**
 * @brief gRPC server for stream management API
 *
 * Provides external API for controlling streams via gRPC.
 */
class GrpcServer {
public:
    /**
     * @brief Factory method
     */
    [[nodiscard]] static Result<std::unique_ptr<GrpcServer>> Create(
        std::shared_ptr<StreamManager> stream_manager,
        std::shared_ptr<ModelRegistry> model_registry,
        int port = kDefaultGrpcPort);

    // Non-copyable, non-movable
    GrpcServer(const GrpcServer&) = delete;
    GrpcServer& operator=(const GrpcServer&) = delete;
    GrpcServer(GrpcServer&&) = delete;
    GrpcServer& operator=(GrpcServer&&) = delete;

    ~GrpcServer();

    /**
     * @brief Start the gRPC server
     */
    [[nodiscard]] VoidResult Start();

    /**
     * @brief Stop the gRPC server
     */
    void Stop();

    /**
     * @brief Check if server is running
     */
    [[nodiscard]] bool IsRunning() const noexcept { return running_.load(); }

    /**
     * @brief Get server port
     */
    [[nodiscard]] int GetPort() const noexcept { return port_; }

    /**
     * @brief Wait for server to shutdown (blocking)
     */
    void Wait();

private:
    GrpcServer(std::shared_ptr<StreamManager> stream_manager,
               std::shared_ptr<ModelRegistry> model_registry,
               int port);

    std::shared_ptr<StreamManager> stream_manager_;
    std::shared_ptr<ModelRegistry> model_registry_;
    int port_;

    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<grpc::Service> service_impl_;
    std::atomic<bool> running_{false};
};

}  // namespace stream_daemon

#endif  // STREAM_DAEMON_GRPC_SERVER_H_
