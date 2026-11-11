# High-Throughput, Low-Latency bRPC Microservices Ecosystem

This directory houses the complete architecture, configuration, and production-ready C++ code structure for an industrial-grade, ultra-low-latency distributed RPC system leveraging **Apache bRPC**. It demonstrates high-concurrency RPC processing, user-space cooperative fiber scheduling (`bthread`), dynamic service discovery, built-in observability (`bvar`), custom protocol multiplexing, and robust fault-tolerant load balancing.

---

## 🏛️ 1. SYSTEM ARCHITECTURE & CONCURRENCY MODEL

### 1.1 The Thread/Fiber Model (`bthread`)
The Apache bRPC framework is built on top of `bthread` (Baidu Thread), a highly efficient M:N user-space fiber scheduling library. Unlike standard 1:1 threading models (`pthread` or `std::thread`), where each user-space thread maps directly to an OS kernel thread, `bthread` multiplexes $M$ fibers onto $N$ worker threads (where $N$ typically matches the number of physical CPU cores).

Below is the structural flow from network event triggers to executing downstream requests:

```text
       +--------------------------------------------------------+
       |                  Network Clients                       |
       +--------------------------------------------------------+
                               |
                               | (TCP Connections)
                               v
+----------------------------------------------------------------------+
|                     I/O EVENT LOOPS (epoll)                          |
|  - Managed by a small pool of dedicated background I/O threads.     |
|  - Edge-triggered epoll listens for incoming network packets.         |
|  - Locks are minimized via lock-free ring buffers (Butex).           |
+----------------------------------------------------------------------+
                               |
                               | (Offloads socket read/write events)
                               v
+----------------------------------------------------------------------+
|                  WORKER BTHREADS (M:N Fibers)                        |
|  - Worker Threads (N physical OS threads) run a cooperative scheduler.|
|  - Spawns lightweight bthreads (M fibers) to handle individual tasks.|
|  - Work-stealing algorithm balances fiber queues across cores.        |
+----------------------------------------------------------------------+
                               |
                               | (Runs asynchronous RPC closures)
                               v
+----------------------------------------------------------------------+
|                    TASK QUEUES & BACKGROUND POOLS                     |
|  - Non-blocking execution of RPC handler logic.                      |
|  - Slow/blocking downstream operations are offloaded using           |
|    `bthread_start_background` to prevent worker thread starvation.   |
+----------------------------------------------------------------------+
                               |
                               | (Calls external microservices / DBs)
                               v
       +--------------------------------------------------------+
       |             Backend Downstream Services                |
       +--------------------------------------------------------+
```

### 1.2 `bthread` vs Standard `std::thread` / `pthread`
Standard OS-level context switching is computationally expensive (~1 to 2 microseconds) because it requires shifting into kernel space, saving CPU registers, swapping page tables, and invalidating the translation lookaside buffer (TLB).

In contrast, `bthread` cooperative context switches take only **~10-20 nanoseconds**. Key features include:
* **User-Space Context Switching:** Fiber register states are saved and restored entirely in user-space, avoiding the transition into kernel space.
* **Cooperative Scheduling (Butex):** Instead of standard mutexes which block OS threads, bthreads use **Butex** (Baidu Futex), a lightweight user-space futex. When a bthread waits on a lock, it yields execution to another runnable bthread on the same worker thread instead of causing an OS-level context switch.
* **Work-Stealing Scheduler:** Each OS worker thread maintains its own lock-free double-ended queue (deque) of runnable bthreads. If a thread runs out of tasks, it attempts to steal bthreads from other workers' queues, maximizing CPU utilization and core locality.

### 1.3 Protocol Multiplexing on a Single Port
A key capability of Apache bRPC is serving different protocols (Baidu STD, HTTP/JSON, gRPC, Redis, Memcached) over the exact same port simultaneously. It achieves this via a three-phase connection handler:

1. **Header Inspection:** When data arrives on a socket, the dispatcher reads the first few bytes (the "magic header").
2. **Protocol Parsing Selection:**
   * If it starts with `PRPC` or `BaDu`, it is treated as **Baidu STD Protocol**.
   * If it starts with `POST` or `GET`, it is treated as **HTTP/REST**.
   * If it starts with HTTP/2 frame signatures (e.g. `PRI * HTTP/2.0`), it is treated as **gRPC/H2**.
3. **Dispatcher Routing:** Once identified, the corresponding parser decodes the request body (e.g., Protobuf message or JSON string), routes it to the registered service stub, and returns the response serialized in the matching protocol.

---

## 🛠️ 2. DIRECTORY STRUCTURE

The ecosystem is structured as follows:
```text
brpc-microservices-ecosystem/
├── CMakeLists.txt              # Standard CMake configuration for bRPC
├── Dockerfile                  # Multi-stage production Docker build
├── docker-compose.yml          # Local container environment orchestration
├── proto/
│   └── service.proto           # High-Frequency Order Matching service contract
├── src/
│   ├── server.cpp              # High-performance asynchronous bRPC server
│   └── client.cpp              # Fault-tolerant resilient bRPC client
└── README.md                   # This comprehensive documentation and guide
```

---

## 🚀 3. OPERATIONAL GUIDE, TUNING & TROUBLESHOOTING

### 3.1 Built-in Web Administration & Profiling Portal
One of bRPC's greatest operational strengths is its built-in admin web portal, accessible directly over the main RPC port via a web browser.
* **`/vars`:** Displays all custom and built-in metric systems (`bvar`). You can feed these directly into Prometheus.
* **`/rpcz`:** Inspects real-time RPC traces, displaying active connections, processing latencies, and request payload samples.
* **`/cpu_profiler`:** Enables/disables CPU profiling dynamically. You can acquire a `.prof` file and inspect it with `gperftools`/`pprof` on the fly without stopping the service.
* **`/heap_profiler`:** Tracks memory allocations and memory leaks in real-time.
* **`/flags`:** Dynamically view and edit command-line flags (e.g., modifying timeouts, log levels, or concurrency limits) without restarting the server.

### 3.2 Performance Tuning Matrix

To achieve sub-millisecond latencies under high concurrency, apply the following optimizations:

| Category | Option / Flag | Recommended Value | Detailed Rationale |
|---|---|---|---|
| **CPU Affinity** | `taskset -c ...` | Bound to physical cores | Avoids thread migration across NUMA nodes, maintaining L1/L2 cache locality. |
| **Network Socket** | `SO_REUSEPORT` | Enabled (`--reuse_port=true`) | Allows multiple server sockets to bind to the same port, letting the OS load-balance connections across multiple acceptor threads. |
| **TCP Options** | `TCP_NODELAY` | Enabled (default in bRPC) | Disables Nagle's algorithm, transmitting small packets immediately to eliminate 40ms delayed ACKs. |
| **Buffer Alloc** | `butil::IOBuf` | Used for all payloads | Non-contiguous memory chain that avoids deep-copying byte buffers during network parsing and serialization. |
| **Fiber Stack** | Stack size settings | `1M` (Normal) or `64KB` (Small) | Adjusts memory allocation per bthread. Small stack sizes maximize the number of concurrent active fibers. |
| **Thread Pool** | `--bthread_concurrency` | Matching CPU cores | Sets the number of OS-level worker threads. Avoid over-provisioning to minimize OS context-switching overhead. |

### 3.3 Benchmarking & Load-Testing Strategy
To benchmark the system, we can use the `wrk` tool for HTTP endpoints, or build the bRPC client to fire high-throughput parallel streams of requests.

Example with `wrk` benchmarking the HTTP/JSON gateway of the same bRPC port:
```bash
# Benchmark HTTP/JSON throughput with 12 threads and 400 connections for 30 seconds
wrk -t12 -c400 -d30s -s benchmark.lua http://localhost:8000/OrderMatchingService/SubmitOrder
```

Example custom benchmark execution:
* Run the client in "benchmark" mode where it spawns $M$ parallel bthreads invoking the asynchronous RPC method iteratively, measuring total QPS (Queries Per Second) and latency distributions (P50, P90, P99, P99.9).

---

## 🏗️ 4. HOW TO RUN

To run the entire ecosystem locally using Docker Compose:
```bash
# Navigate to the ecosystem directory
cd brpc-microservices-ecosystem

# Build and start all services (ZooKeeper, multiple Server nodes, Client)
docker-compose up --build
```
This launches:
1. `zookeeper` at port `2181` for name service registration.
2. `brpc-server-1` and `brpc-server-2` binding to ports `8001` and `8002`, registering themselves in ZooKeeper.
3. `brpc-client` which continuously requests order matches, load-balancing across servers using Consistent Hashing or Round Robin, with built-in speculative execution and failure retry protocols.
