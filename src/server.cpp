#include <gflags/gflags.h>
#include <butil/logging.h>
#include <butil/time.h>
#include <brpc/server.h>
#include <brpc/closure_guard.h>
#include <bvar/bvar.h>
#include <bthread/bthread.h>
#include "proto/service.pb.h"

// Define command-line flags
DEFINE_int32(port, 8000, "TCP Port of this server");
DEFINE_int32(idle_timeout_s, -1, "Connection idle timeout in seconds, -1 means disabled");
DEFINE_int32(max_concurrency, 0, "Limit of concurrent requests, 0 means unlimited");
DEFINE_string(instance_name, "engine-node-alpha", "Name prefix identifying this server instance");

// Define custom bvar metrics to expose real-time metrics dynamically via `/vars` and Prometheus
namespace metrics {
    // Monotonic Counter for total incoming orders
    bvar::Adder<uint64_t> g_total_orders_received("matching_orders_received_total");
    bvar::Adder<uint64_t> g_total_orders_filled("matching_orders_filled_total");
    bvar::Adder<uint64_t> g_total_batches_received("matching_batches_received_total");

    // Latency recorder for P50, P90, P99, P99.9 matching latency distributions
    bvar::LatencyRecorder g_matching_latency("matching_latency_ns");

    // Dynamic state exposure of active matching queue size
    bvar::Status<int64_t> g_active_engine_threads("matching_active_engine_threads");
}

// Struct to pass context into background async bthreads
struct AsyncMatchingContext {
    const matching::OrderRequest* request;
    matching::OrderResponse* response;
    google::protobuf::Closure* done;
    int64_t start_time_us;
    std::string instance_name;
};

// Target background function run inside M:N user-space fiber (bthread)
static void* ExecuteOrderMatchingBackground(void* arg) {
    // Take ownership of context
    std::unique_ptr<AsyncMatchingContext> ctx(static_cast<AsyncMatchingContext*>(arg));
    brpc::ClosureGuard done_guard(ctx->done);

    metrics::g_active_engine_threads.set_value(1);

    // Simulate real ultra-low-latency order matching logic (e.g. crossing orders)
    metrics::g_total_orders_received << 1;

    // Simulate minimal computation matching latency
    bthread_usleep(150); // 150 microseconds matching simulation

    // Assemble OrderResponse
    ctx->response->set_order_id(ctx->request->order_id());

    // Simple mock execution engine logic
    if (ctx->request->quantity() > 0 && ctx->request->price() > 0) {
        ctx->response->set_status("FULLY_FILLED");
        ctx->response->set_filled_quantity(ctx->request->quantity());
        ctx->response->set_average_price(ctx->request->price());
        metrics::g_total_orders_filled << 1;
    } else {
        ctx->response->set_status("REJECTED");
        ctx->response->set_filled_quantity(0.0);
        ctx->response->set_average_price(0.0);
    }

    // Calculate latency metrics
    int64_t end_time_us = butil::cpuwide_time_us();
    int64_t duration_ns = (end_time_us - ctx->start_time_us) * 1000;
    ctx->response->set_latency_ns(duration_ns);
    ctx->response->set_engine_instance(ctx->instance_name);

    // Record latency in bvar
    metrics::g_matching_latency << duration_ns;
    metrics::g_active_engine_threads.set_value(0);

    return nullptr;
}

// Implementation of our OrderMatchingService interface
class OrderMatchingServiceImpl : public matching::OrderMatchingService {
public:
    OrderMatchingServiceImpl(const std::string& instance_name)
        : _instance_name(instance_name) {}

    virtual ~OrderMatchingServiceImpl() {}

    // 1. SubmitOrder: Highly optimized asynchronous implementation using brpc::Closure
    void SubmitOrder(google::protobuf::RpcController* controller,
                     const ::matching::OrderRequest* request,
                     ::matching::OrderResponse* response,
                     ::google::protobuf::Closure* done) override {

        // Setup closure guard to prevent resource leaks. If we pass "done" to another
        // background task execution, we must call guard.release() to transfer ownership.
        brpc::ClosureGuard done_guard(done);

        int64_t start_time_us = butil::cpuwide_time_us();

        // Instantiate async context to transfer to background bthread
        auto* ctx = new AsyncMatchingContext{
            request,
            response,
            done,
            start_time_us,
            _instance_name
        };

        // Create bthread attribute
        bthread_attr_t attr;
        bthread_attr_init(&attr);

        // Spawn asynchronous execution on background cooperative bthread fiber
        // This offloads the physical reactor thread from being blocked by matching/calculations
        bthread_t tid;
        done_guard.release(); // Ownership of done is transferred to ExecuteOrderMatchingBackground
        if (bthread_start_background(&tid, &attr, ExecuteOrderMatchingBackground, ctx) != 0) {
            LOG(ERROR) << "Fail to start background bthread for order: " << request->order_id();
            // Fallback to synchronous execution. ExecuteOrderMatchingBackground(ctx) handles
            // the closure deletion itself via its internal ClosureGuard, so we must NOT
            // retake ownership of done inside this calling frame to prevent double-free crashes.
            ExecuteOrderMatchingBackground(ctx);
        }
    }

    // 2. SubmitBatchOrders: Vectorized execution optimization for batch submissions
    void SubmitBatchOrders(google::protobuf::RpcController* controller,
                           const ::matching::BatchOrderRequest* request,
                           ::matching::BatchOrderResponse* response,
                           ::google::protobuf::Closure* done) override {

        brpc::ClosureGuard done_guard(done);
        int64_t start_time_us = butil::cpuwide_time_us();

        metrics::g_total_batches_received << 1;

        int total_orders = request->orders_size();
        for (int i = 0; i < total_orders; ++i) {
            const auto& order = request->orders(i);
            metrics::g_total_orders_received << 1;

            auto* resp_item = response->add_responses();
            resp_item->set_order_id(order.order_id());

            // Fast inline processing for vectorized batches
            if (order.quantity() > 0) {
                resp_item->set_status("FULLY_FILLED");
                resp_item->set_filled_quantity(order.quantity());
                resp_item->set_average_price(order.price());
                metrics::g_total_orders_filled << 1;
            } else {
                resp_item->set_status("REJECTED");
            }

            int64_t end_time_us = butil::cpuwide_time_us();
            int64_t duration_ns = (end_time_us - start_time_us) * 1000;
            resp_item->set_latency_ns(duration_ns);
            resp_item->set_engine_instance(_instance_name);

            metrics::g_matching_latency << duration_ns;
        }

        response->set_total_processed(total_orders);
    }

private:
    std::string _instance_name;
};

int main(int argc, char* argv[]) {
    // Parse gflags
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

    // Initialize Server instance
    brpc::Server server;

    // Build the service implementation
    OrderMatchingServiceImpl matching_service_impl(FLAGS_instance_name);

    // Register our matching service to bRPC Server.
    if (server.AddService(&matching_service_impl,
                          brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "Failed to register OrderMatchingService to server!";
        return -1;
    }

    // Configure the server settings
    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;
    options.max_concurrency = FLAGS_max_concurrency;

    // Enable HTTP access and built-in administration tools (/status, /vars, /rpcz)
    options.has_builtin_services = true;

    // Start the server
    if (server.Start(FLAGS_port, &options) != 0) {
        LOG(ERROR) << "Failed to start bRPC Server on port: " << FLAGS_port;
        return -1;
    }

    LOG(INFO) << "================================================================";
    LOG(INFO) << "bRPC Ultra-low Latency Order Matching Server Started Successfully";
    LOG(INFO) << "Instance Name: " << FLAGS_instance_name;
    LOG(INFO) << "Port: " << FLAGS_port;
    LOG(INFO) << "Web Admin Console & Metrics: http://localhost:" << FLAGS_port << "/vars";
    LOG(INFO) << "Web Tracing System: http://localhost:" << FLAGS_port << "/rpcz";
    LOG(INFO) << "================================================================";

    // Wait until the server is stopped (e.g. via SIGINT / SIGTERM)
    server.RunUntilAskedToQuit();

    LOG(INFO) << "Server shutting down gracefully. Clear ports and memory.";
    return 0;
}
