#!/bin/bash

# Quick benchmark script for development testing
# Lighter version of the full benchmark suite

set -e

SERVER_HOST="127.0.0.1"
SERVER_PORT="8080"
DURATION="10s"
CONNECTIONS="100"
THREADS="4"

GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

print_header() {
    echo -e "\n${BLUE}$1${NC}\n"
}

print_success() {
    echo -e "${GREEN}[OK] $1${NC}"
}

wait_for_server() {
    local count=0
    local timeout=15
    
    echo "Waiting for server to be ready..."
    while ! nc -z $SERVER_HOST $SERVER_PORT 2>/dev/null; do
        sleep 1
        count=$((count + 1))
        if [ $count -ge $timeout ]; then
            echo "[ERROR] Timeout waiting for server"
            return 1
        fi
    done
    print_success "Server is ready"
}

quick_benchmark() {
    print_header "Quick Performance Test"
    
    echo "Configuration:"
    echo "- Duration: $DURATION"
    echo "- Connections: $CONNECTIONS"
    echo "- Threads: $THREADS"
    echo ""
    
    # Test basic endpoint
    echo "Testing root endpoint..."
    wrk -t$THREADS -c$CONNECTIONS -d$DURATION http://$SERVER_HOST:$SERVER_PORT/
    
    echo ""
    echo "Testing static file..."
    wrk -t$THREADS -c$CONNECTIONS -d$DURATION http://$SERVER_HOST:$SERVER_PORT/test.html
    
    echo ""
    echo "Testing API endpoint..."
    wrk -t$THREADS -c$CONNECTIONS -d$DURATION http://$SERVER_HOST:$SERVER_PORT/api/info
}

main() {
    print_header "Quick Benchmark - C++ Multithreaded Web Server"
    
    # Check if server is running
    if ! nc -z $SERVER_HOST $SERVER_PORT 2>/dev/null; then
        echo "Server is not running on $SERVER_HOST:$SERVER_PORT"
        echo "Please start the server first:"
        echo "./build/bin/webserver $SERVER_PORT"
        exit 1
    fi
    
    # Check if wrk is available
    if ! command -v wrk &> /dev/null; then
        echo "[ERROR] wrk is not installed. Please install it first:"
        echo "Ubuntu/Debian: sudo apt-get install wrk"
        echo "macOS: brew install wrk"
        exit 1
    fi
    
    quick_benchmark
    
    print_success "Quick benchmark completed!"
    echo ""
    echo "For comprehensive benchmarking against Nginx/Apache, run:"
    echo "./scripts/benchmark.sh"
}

main "$@"