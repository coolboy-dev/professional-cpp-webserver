# Web Server Performance Benchmark Report

Generated on: Wed Sep 10 15:55:28 EAT 2025

## Test Configuration
- **Duration**: 30s
- **Concurrent Connections**: 1000
- **Worker Threads**: 10
- **Total Requests (AB)**: 100000

## Servers Tested
1. **C++ Multithreaded Server** - Custom epoll + thread pool implementation
2. **Nginx** - Industry standard web server
3. **Apache HTTP Server** - Popular web server

## Results Summary

### wrk Benchmark Results
#### C++ Multithreaded Server (wrk)
```
Requests/sec:  43882.65
Transfer/sec:    115.12MB
```


### ApacheBench Results
#### C++ Multithreaded Server (ab)
```
Requests per second:    8131.70 [#/sec] (mean)
Time per request:       122.975 [ms] (mean)
Time per request:       0.123 [ms] (mean, across all concurrent requests)
Transfer rate:          17457.50 [Kbytes/sec] received
```

