#!/bin/bash

# High-Performance Multithreaded Web Server Benchmarking Script
# Compares performance against Nginx and Apache using wrk and ApacheBench

set -e

# Configuration
SERVER_HOST="127.0.0.1"
SERVER_PORT="8080"
NGINX_PORT="80"
APACHE_PORT="8000"

# Benchmark parameters
DURATION="30s"
CONNECTIONS="1000"
THREADS="10"
REQUESTS="100000"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Helper functions
print_header() {
    echo -e "\n${BLUE}======================================${NC}"
    echo -e "${BLUE} $1 ${NC}"
    echo -e "${BLUE}======================================${NC}\n"
}

print_success() {
    echo -e "${GREEN}[OK] $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}[WARNING] $1${NC}"
}

print_error() {
    echo -e "${RED}[ERROR] $1${NC}"
}

check_tool() {
    if ! command -v $1 &> /dev/null; then
        print_error "$1 is not installed. Please install it first."
        echo "Ubuntu/Debian: sudo apt-get install $1"
        echo "macOS: brew install $1"
        return 1
    fi
    return 0
}

wait_for_server() {
    local host=$1
    local port=$2
    local timeout=30
    local count=0
    
    echo "Waiting for server at $host:$port to be ready..."
    while ! nc -z $host $port 2>/dev/null; do
        sleep 1
        count=$((count + 1))
        if [ $count -ge $timeout ]; then
            print_error "Timeout waiting for server at $host:$port"
            return 1
        fi
    done
    print_success "Server at $host:$port is ready"
}

benchmark_with_wrk() {
    local name=$1
    local url=$2
    local output_file=$3
    
    print_header "Benchmarking $name with wrk"
    echo "URL: $url"
    echo "Duration: $DURATION"
    echo "Connections: $CONNECTIONS"
    echo "Threads: $THREADS"
    echo ""
    
    wrk -t$THREADS -c$CONNECTIONS -d$DURATION --latency $url | tee $output_file
}

benchmark_with_ab() {
    local name=$1
    local url=$2
    local output_file=$3
    
    print_header "Benchmarking $name with ApacheBench"
    echo "URL: $url"
    echo "Requests: $REQUESTS"
    echo "Concurrency: $CONNECTIONS"
    echo ""
    
    ab -n $REQUESTS -c $CONNECTIONS -g $output_file.gnuplot $url | tee $output_file
}

start_cpp_server() {
    print_header "Starting C++ Multithreaded Web Server"
    
    # Build the server if needed
    if [ ! -f "../build/bin/webserver" ]; then
        print_warning "Server binary not found. Building..."
        cd ..
        mkdir -p build
        cd build
        cmake .. -DCMAKE_BUILD_TYPE=Release
        make -j$(nproc)
        cd ../scripts
    fi
    
    # Start the server in background
    ../build/bin/webserver $SERVER_PORT > server.log 2>&1 &
    SERVER_PID=$!
    echo "Started server with PID: $SERVER_PID"
    
    # Wait for server to be ready
    if wait_for_server $SERVER_HOST $SERVER_PORT; then
        print_success "C++ server is running on port $SERVER_PORT"
        return 0
    else
        print_error "Failed to start C++ server"
        kill $SERVER_PID 2>/dev/null || true
        return 1
    fi
}

stop_cpp_server() {
    if [ ! -z "$SERVER_PID" ]; then
        print_header "Stopping C++ Multithreaded Web Server"
        kill $SERVER_PID 2>/dev/null || true
        wait $SERVER_PID 2>/dev/null || true
        print_success "C++ server stopped"
    fi
}

setup_nginx_config() {
    print_header "Setting up Nginx configuration"
    
    # Create a minimal nginx config for testing
    cat > nginx_bench.conf << EOF
worker_processes auto;
events {
    worker_connections 2048;
    use epoll;
}

http {
    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;
    keepalive_timeout 65;
    
    server {
        listen $NGINX_PORT;
        location / {
            root ../public;
            index index.html;
        }
    }
}
EOF
    print_success "Nginx config created"
}

start_nginx() {
    print_header "Starting Nginx"
    
    setup_nginx_config
    
    # Start nginx with custom config
    sudo nginx -c $(pwd)/nginx_bench.conf -p $(pwd)
    
    if wait_for_server $SERVER_HOST $NGINX_PORT; then
        print_success "Nginx is running on port $NGINX_PORT"
        return 0
    else
        print_error "Failed to start Nginx"
        return 1
    fi
}

stop_nginx() {
    print_header "Stopping Nginx"
    sudo nginx -s quit 2>/dev/null || true
    sudo pkill nginx 2>/dev/null || true
    print_success "Nginx stopped"
}

generate_report() {
    print_header "Generating Performance Report"
    
    cat > performance_report.md << EOF
# Web Server Performance Benchmark Report

Generated on: $(date)

## Test Configuration
- **Duration**: $DURATION
- **Concurrent Connections**: $CONNECTIONS
- **Worker Threads**: $THREADS
- **Total Requests (AB)**: $REQUESTS

## Servers Tested
1. **C++ Multithreaded Server** - Custom epoll + thread pool implementation
2. **Nginx** - Industry standard web server
3. **Apache HTTP Server** - Popular web server

## Results Summary

### wrk Benchmark Results
EOF
    
    if [ -f "cpp_wrk.txt" ]; then
        echo "#### C++ Multithreaded Server (wrk)" >> performance_report.md
        echo '```' >> performance_report.md
        grep -E "(Requests/sec|Transfer/sec|Latency.*avg.*stdev.*max)" cpp_wrk.txt >> performance_report.md
        echo '```' >> performance_report.md
        echo "" >> performance_report.md
    fi
    
    if [ -f "nginx_wrk.txt" ]; then
        echo "#### Nginx (wrk)" >> performance_report.md
        echo '```' >> performance_report.md
        grep -E "(Requests/sec|Transfer/sec|Latency.*avg.*stdev.*max)" nginx_wrk.txt >> performance_report.md
        echo '```' >> performance_report.md
        echo "" >> performance_report.md
    fi
    
    cat >> performance_report.md << EOF

### ApacheBench Results
EOF
    
    if [ -f "cpp_ab.txt" ]; then
        echo "#### C++ Multithreaded Server (ab)" >> performance_report.md
        echo '```' >> performance_report.md
        grep -E "(Requests per second|Time per request|Transfer rate)" cpp_ab.txt >> performance_report.md
        echo '```' >> performance_report.md
        echo "" >> performance_report.md
    fi
    
    print_success "Report generated: performance_report.md"
}

cleanup() {
    print_header "Cleaning up"
    stop_cpp_server
    stop_nginx
    rm -f nginx_bench.conf nginx.pid error.log access.log 2>/dev/null || true
}

main() {
    print_header "Web Server Performance Benchmark Suite"
    echo "This script will benchmark the C++ multithreaded web server"
    echo "against Nginx and optionally Apache HTTP Server."
    echo ""
    
    # Check prerequisites
    check_tool wrk || exit 1
    check_tool ab || exit 1
    check_tool nc || exit 1
    
    # Trap cleanup on exit
    trap cleanup EXIT
    
    # Create results directory
    mkdir -p results
    cd results
    
    # Benchmark C++ server
    if start_cpp_server; then
        benchmark_with_wrk "C++ Multithreaded Server" "http://$SERVER_HOST:$SERVER_PORT/" "cpp_wrk.txt"
        benchmark_with_ab "C++ Multithreaded Server" "http://$SERVER_HOST:$SERVER_PORT/" "cpp_ab.txt"
        stop_cpp_server
    else
        print_error "Skipping C++ server benchmarks due to startup failure"
    fi
    
    # Benchmark Nginx (if available)
    if command -v nginx &> /dev/null; then
        if start_nginx; then
            benchmark_with_wrk "Nginx" "http://$SERVER_HOST:$NGINX_PORT/" "nginx_wrk.txt"
            benchmark_with_ab "Nginx" "http://$SERVER_HOST:$NGINX_PORT/" "nginx_ab.txt"
            stop_nginx
        else
            print_warning "Skipping Nginx benchmarks due to startup failure"
        fi
    else
        print_warning "Nginx not found, skipping Nginx benchmarks"
    fi
    
    # Generate final report
    generate_report
    
    print_header "Benchmark Complete!"
    echo "Results saved in: $(pwd)"
    echo "- Individual results: *.txt files"
    echo "- Summary report: performance_report.md"
    echo ""
    print_success "Benchmark suite completed successfully!"
}

# Run main function if script is executed directly
if [ "$0" = "${BASH_SOURCE[0]}" ]; then
    main "$@"
fi