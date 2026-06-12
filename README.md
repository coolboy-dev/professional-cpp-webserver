# 🚀 High-Performance Multithreaded Web Server (C++17)

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)](https://github.com/swe-robertkibet/multithreaded-webserver-cpp)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](https://github.com/swe-robertkibet/multithreaded-webserver-cpp)
[![License](https://img.shields.io/badge/license-MIT-green)](https://github.com/swe-robertkibet/multithreaded-webserver-cpp/blob/main/LICENSE)
[![Performance](https://img.shields.io/badge/performance-78.1K%20req%2Fs-brightgreen)](https://github.com/swe-robertkibet/multithreaded-webserver-cpp)

A production-grade, event-driven HTTP/1.1 server engineered for extreme throughput and minimal memory footprint. By combining Linux `epoll` with a scalable worker thread pool and an intelligent LRU cache, this server achieves industry-leading performance on modern Linux environments.

---

## ⚡ Performance Benchmarks

Our server is designed for scalability. Below are the results from sustained stress tests using `wrk`.

### Throughput & Scalability
| Concurrent Connections | Requests/sec | Performance |
| :--- | :--- | :--- |
| 100 | 39,565.85 | Baseline |
| 500 | 55,934.55 | +41.4% |
| 1,000 | 78,091.55 | +97.3% |
| **2,000** | **73,075.56** | **Peak Capacity** |
| 5,000 | 71,508.89 | Sustained |

### Resource Efficiency
- **Memory Footprint**: ~1.7MB (Stable)
- **Memory Growth**: 0% over 10-minute sustained load
- **Avg Latency**: Sub-millisecond for cached assets

---

## 🏗️ Technical Deep Dive

### The Architecture
The server employs a layered design to decouple network I/O from application logic:

1. **Event-Driven I/O Layer**: Uses `epoll` in edge-triggered mode to monitor thousands of sockets simultaneously. The main thread only accepts connections and dispatches I/O events.
2. **Worker Thread Pool**: A fixed-size pool of worker threads processes the request queue, ensuring that slow file I/O doesn't block the event loop.
3. **LRU Caching System**: A thread-safe Least Recently Used (LRU) cache stores hot files in memory, bypassing the filesystem for repeated requests.
4. **Security Layer**: Integrated path traversal protection and a token-bucket rate limiter protect the server from DDoS and directory escape attacks.

### Request Lifecycle
`Client Request` $\rightarrow$ `Epoll Event` $\rightarrow$ `ThreadPool Queue` $\rightarrow$ `HTTP Parser` $\rightarrow$ `LRU Cache/File System` $\rightarrow$ `HTTP Response` $\rightarrow$ `Async Write`

---

## 🛠️ Installation & Setup

### Prerequisites
- **OS**: Linux (Ubuntu 20.04+ recommended)
- **Compiler**: GCC 7+ or Clang 5+ (C++17 support)
- **Build Tool**: CMake 3.16+

### Quick Start
```bash
# Clone the repository
git clone https://github.com/swe-robertkibet/multithreaded-webserver-cpp.git
cd multithreaded-webserver-cpp

# Build the project
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Run the server
./bin/webserver 8080
```

### Docker Deployment
```bash
# Build and run production image
docker-compose up --build webserver
```

---

## 🔌 API Reference

The server provides several built-in endpoints for testing and monitoring:

| Endpoint | Method | Description | Example Response |
| :--- | :--- | :--- | :--- |
| `/` | `GET` | Serves the landing page | `200 OK` |
| `/api/info` | `GET` | Returns real-time server metrics (JSON) | `{"active_connections": 12, ...}` |
| `/files/` | `GET` | Provides directory listing of the webroot | `200 OK (HTML List)` |
| `/test.html` | `GET` | Serves a sample static HTML file | `200 OK` |

---

## ⚙️ Configuration

Customize server behavior via `config.json`:

```json
{
  "server": {
    "host": "0.0.0.0",
    "port": 8080,
    "max_connections": 2000,
    "socket_timeout": 30
  },
  "threading": {
    "thread_pool_size": 8,
    "max_queue_size": 10000
  },
  "cache": {
    "enabled": true,
    "max_size_mb": 100,
    "ttl_seconds": 300
  },
  "rate_limiting": {
    "enabled": true,
    "requests_per_second": 100,
    "burst_size": 200
  }
}
```

---

## 🤝 Contributing

1. Fork the repository.
2. Create a feature branch: `git checkout -b feature/amazing-feature`.
3. Commit your changes: `git commit -m 'Add amazing feature'`.
4. Push to the branch: `git push origin feature/amazing-feature`.
5. Open a Pull Request.

## 📄 License
This project is licensed under the MIT License.

---
**Built with ❤️ using modern C++17 and systems programming best practices.**
