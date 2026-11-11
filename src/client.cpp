#include <gflags/gflags.h>
#include <butil/logging.h>
#include <butil/time.h>
#include <brpc/channel.h>
#include <bthread/bthread.h>
#include "proto/service.pb.h"

// Define command-line flags
DEFINE_string(naming_service, "file://servers.txt", "Naming service address or local servers list");
DEFINE_string(load_balancer, "rr", "Load balancing strategy: 'rr' (Round Robin), 'random', 'c_murmurhash' (Consistent Hashing)");
DEFINE_int32(timeout_ms, 500, "RPC timeout in milliseconds");
DEFINE_int32(max_retry, 3, "Max retry times on failure");
DEFINE_int32(backup_request_ms, 15, "Delay threshold for speculative backup request triggers in milliseconds");
DEFINE_int32(benchmark_threads, 4, "Number of concurrent threads for load-testing");
DEFINE_int32(benchmark_seconds, 10, "Duration of benchmark in seconds");

// Custom callback class to handle non-blocking asynchronous calls
class AsyncMatchingCallback : public google::protobuf::Closure {
public:
    AsyncMatchingCallback(matching::OrderResponse* response, brpc::Controller* cntl, int64_t start_time_us)
        : _response(response), _cntl(cntl), _start_time_us(start_time_us) {}

    void Run() override {
        // Guard allocation deletion
        std::unique_ptr<matching::OrderResponse> response_guard(_response);
        std::unique_ptr<brpc::Controller> cntl_guard(_cntl);

        if (_cntl->Failed()) {
            LOG(WARNING) << "Asynchronous RPC Failed: " << _cntl->ErrorText();
        } else {
            int64_t current_time_us = butil::cpuwide_time_us();
            int64_t client_latency_us = current_time_us - _start_time_us;
            LOG(INFO) << "Async Order Callback Succeeded. OrderId: " << _response->order_id()
                      << " | Status: " << _response->status()
                      << " | Client RTT: " << client_latency_us << " us"
                      << " | Engine Latency: " << _response->latency_ns() / 1000 << " us"
                      << " | Server Instance: " << _response->engine_instance()
                      << " | LB Server: " << _cntl->remote_side();
        }
    }

private:
    matching::OrderResponse* _response;
    brpc::Controller* _cntl;
    int64_t _start_time_us;
};

// Target function for our multithreaded benchmarking bthreads
struct BenchmarkThreadArgs {
    brpc::Channel* channel;
    std::atomic<uint64_t>* total_qps;
    std::atomic<uint64_t>* total_failures;
    bool* stop_flag;
};

static void* RunBenchmarkFiber(void* arg) {
    auto* args = static_cast<BenchmarkThreadArgs*>(arg);
    matching::OrderMatchingService_Stub stub(args->channel);

    uint64_t thread_qps = 0;
    uint64_t thread_fail = 0;
    uint64_t counter = 0;

    while (!*(args->stop_flag)) {
        brpc::Controller cntl;
        matching::OrderRequest request;
        matching::OrderResponse response;

        // Populate request with dummy transactional metrics
        std::string order_id = "bench-" + std::to_string(bthread_self()) + "-" + std::to_string(counter++);
        request.set_order_id(order_id);
        request.set_client_id("client-perf-test");
        request.set_symbol("ETHUSDT");
        request.set_side(matching::OrderSide::BUY);
        request.set_type(matching::OrderType::LIMIT);
        request.set_price(1850.50);
        request.set_quantity(10.0);
        request.set_timestamp_ns(butil::cpuwide_time_us() * 1000);

        // Blocking synchronous RPC call
        stub.SubmitOrder(&cntl, &request, &response, nullptr);

        if (!cntl.Failed()) {
            thread_qps++;
        } else {
            thread_fail++;
        }
    }

    *(args->total_qps) += thread_qps;
    *(args->total_failures) += thread_fail;
    return nullptr;
}

int main(int argc, char* argv[]) {
    // Parse gflags
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

    // Initialize Channel
    brpc::Channel channel;

    // Configure client-side channel options
    brpc::ChannelOptions options;
    options.protocol = "baidu_std"; // Use high-performance Baidu STD Protocol
    options.timeout_ms = FLAGS_timeout_ms;
    options.max_retry = FLAGS_max_retry;

    // REDUCE LONG-TAIL LATENCY SPIKES: Speculative Backup Request Execution.
    // If an RPC does not return within backup_request_ms, a speculative second request
    // is dispatched to another server. The response that returns first is accepted.
    options.backup_request_ms = FLAGS_backup_request_ms;

    // Connect channel with a naming service and set load balancer strategy.
    // In a dynamic production infrastructure, the channel can be initialized using
    // a dynamic discovery service registry, such as ZooKeeper, Consul, Nacos, or DNS:
    //
    // For ZooKeeper:
    //   channel.Init("zookeeper://127.0.0.1:2181/service/order_matching", "rr", &options);
    //
    // For Consul:
    //   channel.Init("consul://127.0.0.1:8500/order_matching_service", "random", &options);
    //
    // For Domain DNS-based service discovery:
    //   channel.Init("dns://order-matching.production.local", "c_murmurhash", &options);
    //
    // Below we default to our highly configurable gflags-driven naming service:
    if (channel.Init(FLAGS_naming_service.c_str(), FLAGS_load_balancer.c_str(), &options) != 0) {
        LOG(ERROR) << "Failed to initialize Channel at naming service: " << FLAGS_naming_service;
        return -1;
    }

    LOG(INFO) << "================================================================";
    LOG(INFO) << "bRPC Client Initialized Successfully";
    LOG(INFO) << "Naming Service Connection: " << FLAGS_naming_service;
    LOG(INFO) << "Load Balancing Protocol Strategy: " << FLAGS_load_balancer;
    LOG(INFO) << "Backup Request speculative delay: " << FLAGS_backup_request_ms << " ms";
    LOG(INFO) << "================================================================";

    // 1. DEMONSTRATE BLOCKING SYNCHRONOUS RPC
    LOG(INFO) << "[Sync RPC] Dispatching high-priority order match request...";
    {
        matching::OrderMatchingService_Stub stub(&channel);
        brpc::Controller cntl;
        matching::OrderRequest request;
        matching::OrderResponse response;

        request.set_order_id("order-sync-001");
        request.set_client_id("terminal-client-01");
        request.set_symbol("BTCUSDT");
        request.set_side(matching::OrderSide::BUY);
        request.set_type(matching::OrderType::LIMIT);
        request.set_price(64250.0);
        request.set_quantity(0.5);
        request.set_timestamp_ns(butil::cpuwide_time_us() * 1000);

        int64_t start_time_us = butil::cpuwide_time_us();
        stub.SubmitOrder(&cntl, &request, &response, nullptr);

        if (!cntl.Failed()) {
            int64_t end_time_us = butil::cpuwide_time_us();
            LOG(INFO) << "[Sync RPC] Succeeded! Match Status: " << response.status()
                      << " | Filled Quantity: " << response.filled_quantity()
                      << " | Avg Exec Price: " << response.average_price()
                      << " | Round-Trip-Time (RTT): " << (end_time_us - start_time_us) << " us"
                      << " | Engine Internal latency: " << response.latency_ns() / 1000 << " us"
                      << " | Selected LB Server: " << cntl.remote_side()
                      << " | Engine Node Instance: " << response.engine_instance();
        } else {
            LOG(ERROR) << "[Sync RPC] Failed to process order matching: " << cntl.ErrorText();
        }
    }

    bthread_usleep(500 * 1000); // Sleep 500ms

    // 2. DEMONSTRATE NON-BLOCKING ASYNCHRONOUS RPC USING CALLBACKS
    LOG(INFO) << "[Async RPC] Dispatching order match request asynchronously...";
    {
        matching::OrderMatchingService_Stub stub(&channel);

        // Dynamically allocate response, controller and callback to survive scope exit
        auto* response = new matching::OrderResponse();
        auto* cntl = new brpc::Controller();

        matching::OrderRequest request;
        request.set_order_id("order-async-102");
        request.set_client_id("terminal-client-01");
        request.set_symbol("SOLUSDT");
        request.set_side(matching::OrderSide::SELL);
        request.set_type(matching::OrderType::LIMIT);
        request.set_price(142.20);
        request.set_quantity(15.0);
        request.set_timestamp_ns(butil::cpuwide_time_us() * 1000);

        int64_t start_time_us = butil::cpuwide_time_us();
        auto* callback = new AsyncMatchingCallback(response, cntl, start_time_us);

        // Pass callback to stub invocation. The framework calls callback->Run() on completion
        stub.SubmitOrder(cntl, &request, response, callback);
        LOG(INFO) << "[Async RPC] Callback registered. Main thread is free to proceed.";
    }

    bthread_usleep(500 * 1000); // Wait for callback to complete and log

    // 3. DEMONSTRATE SYSTEM BENCHMARK (HIGH-THROUGHPUT TEST)
    LOG(INFO) << "[Benchmarking] Spawning load-testing fibers to compute performance throughput...";
    {
        std::atomic<uint64_t> total_qps{0};
        std::atomic<uint64_t> total_failures{0};
        bool stop_flag = false;

        std::vector<bthread_t> tids(FLAGS_benchmark_threads);
        std::vector<BenchmarkThreadArgs> thread_args(FLAGS_benchmark_threads);

        int64_t bench_start_us = butil::cpuwide_time_us();

        for (int i = 0; i < FLAGS_benchmark_threads; ++i) {
            thread_args[i] = {&channel, &total_qps, &total_failures, &stop_flag};
            bthread_attr_t attr;
            bthread_attr_init(&attr);
            if (bthread_start_background(&tids[i], &attr, RunBenchmarkFiber, &thread_args[i]) != 0) {
                LOG(FATAL) << "Fail to start benchmark fiber " << i;
            }
        }

        LOG(INFO) << "[Benchmarking] Load test running for " << FLAGS_benchmark_seconds << " seconds...";
        bthread_usleep(FLAGS_benchmark_seconds * 1000 * 1000);

        stop_flag = true;

        for (int i = 0; i < FLAGS_benchmark_threads; ++i) {
            bthread_join(tids[i], nullptr);
        }

        int64_t bench_end_us = butil::cpuwide_time_us();
        double elapsed_sec = (bench_end_us - bench_start_us) / 1000000.0;

        uint64_t qps = total_qps.load();
        uint64_t fails = total_failures.load();

        LOG(INFO) << "=================== BENCHMARK REPORT ===================";
        LOG(INFO) << "Duration: " << elapsed_sec << " seconds";
        LOG(INFO) << "Successful Match Operations: " << qps;
        LOG(INFO) << "Failed Match Operations: " << fails;
        LOG(INFO) << "Estimated System Throughput: " << (qps / elapsed_sec) << " QPS (Queries per Second)";
        LOG(INFO) << "Robustness Success Ratio: " << (qps * 100.0 / (qps + fails + 0.1)) << " %";
        LOG(INFO) << "========================================================";
    }

    LOG(INFO) << "bRPC client shutting down cleanly.";
    return 0;
}
