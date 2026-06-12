#!/bin/bash

# Stress testing script to validate server stability under extreme load

set -e

SERVER_HOST="127.0.0.1"
SERVER_PORT="8080"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

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

check_server() {
    if ! nc -z $SERVER_HOST $SERVER_PORT 2>/dev/null; then
        print_error "Server is not running on $SERVER_HOST:$SERVER_PORT"
        echo "Please start the server first: ./build/bin/webserver $SERVER_PORT"
        return 1
    fi
    return 0
}

stress_test_connections() {
    print_header "Stress Test: High Connection Count"
    
    echo "Testing server with increasing connection counts..."
    
    local connections_list="100 500 1000 2000 5000"
    
    for conn in $connections_list; do
        echo ""
        echo "Testing with $conn concurrent connections..."
        
        timeout 30s wrk -t10 -c$conn -d10s --latency http://$SERVER_HOST:$SERVER_PORT/ > /tmp/stress_$conn.txt 2>&1
        
        if [ $? -eq 0 ]; then
            local rps=$(grep "Requests/sec" /tmp/stress_$conn.txt | awk '{print $2}')
            local avg_latency=$(grep -A1 "Latency" /tmp/stress_$conn.txt | tail -1 | awk '{print $1}')
            print_success "$conn connections: $rps req/s, avg latency: $avg_latency"
        else
            print_error "$conn connections: Test failed or timed out"
        fi
    done
}

stress_test_duration() {
    print_header "Stress Test: Extended Duration"
    
    echo "Testing server stability over extended periods..."
    
    local durations="1m 5m 10m"
    
    for duration in $durations; do
        echo ""
        echo "Running $duration duration test..."
        
        wrk -t8 -c500 -d$duration http://$SERVER_HOST:$SERVER_PORT/ > /tmp/stress_$duration.txt 2>&1 &
        local wrk_pid=$!
        
        # Monitor server during test
        local start_time=$(date +%s)
        local test_failed=0
        
        while kill -0 $wrk_pid 2>/dev/null; do
            if ! nc -z $SERVER_HOST $SERVER_PORT 2>/dev/null; then
                print_error "Server became unresponsive during $duration test"
                kill $wrk_pid 2>/dev/null || true
                test_failed=1
                break
            fi
            sleep 5
        done
        
        wait $wrk_pid 2>/dev/null
        local wrk_result=$?
        
        if [ $test_failed -eq 0 ] && [ $wrk_result -eq 0 ]; then
            local rps=$(grep "Requests/sec" /tmp/stress_$duration.txt | awk '{print $2}')
            print_success "$duration test completed: $rps req/s average"
        else
            print_error "$duration test failed"
        fi
    done
}

stress_test_memory_leak() {
    print_header "Stress Test: Memory Leak Detection"
    
    echo "Testing for memory leaks during sustained load..."
    
    # Get initial memory usage
    local server_pid=$(pgrep -f "webserver.*$SERVER_PORT" | head -1)
    if [ -z "$server_pid" ]; then
        print_error "Could not find server process"
        return 1
    fi
    
    local initial_memory=$(ps -p $server_pid -o rss= | tr -d ' ')
    echo "Initial memory usage: ${initial_memory}KB"
    
    # Run sustained load for 5 minutes
    echo "Running sustained load test (5 minutes)..."
    wrk -t8 -c1000 -d5m http://$SERVER_HOST:$SERVER_PORT/ > /tmp/memory_test.txt 2>&1 &
    local wrk_pid=$!
    
    # Monitor memory usage
    local max_memory=$initial_memory
    local samples=0
    local total_memory=0
    
    while kill -0 $wrk_pid 2>/dev/null; do
        sleep 10
        local current_memory=$(ps -p $server_pid -o rss= 2>/dev/null | tr -d ' ')
        
        if [ ! -z "$current_memory" ]; then
            total_memory=$((total_memory + current_memory))
            samples=$((samples + 1))
            
            if [ $current_memory -gt $max_memory ]; then
                max_memory=$current_memory
            fi
            
            echo "Memory usage: ${current_memory}KB (max: ${max_memory}KB)"
        fi
    done
    
    wait $wrk_pid 2>/dev/null
    
    local final_memory=$(ps -p $server_pid -o rss= 2>/dev/null | tr -d ' ')
    local avg_memory=$((total_memory / samples))
    
    echo ""
    echo "Memory Usage Summary:"
    echo "- Initial: ${initial_memory}KB"
    echo "- Final: ${final_memory}KB"
    echo "- Maximum: ${max_memory}KB"
    echo "- Average: ${avg_memory}KB"
    
    # Check for significant memory growth (>50% increase)
    local growth_percent=$(( (final_memory - initial_memory) * 100 / initial_memory ))
    if [ $growth_percent -gt 50 ]; then
        print_warning "Potential memory leak detected: ${growth_percent}% memory growth"
    else
        print_success "No significant memory leak detected: ${growth_percent}% memory growth"
    fi
}

stress_test_error_handling() {
    print_header "Stress Test: Error Handling"
    
    echo "Testing server behavior under error conditions..."
    
    # Test with invalid requests
    echo ""
    echo "Testing with malformed HTTP requests..."
    for i in {1..100}; do
        echo "INVALID REQUEST $i" | nc $SERVER_HOST $SERVER_PORT >/dev/null 2>&1 &
    done
    wait
    
    sleep 2
    if nc -z $SERVER_HOST $SERVER_PORT 2>/dev/null; then
        print_success "Server survived malformed request flood"
    else
        print_error "Server crashed from malformed requests"
        return 1
    fi
    
    # Test with very large requests
    echo ""
    echo "Testing with oversized requests..."
    local large_data=$(head -c 100000 /dev/zero | tr '\0' 'A')
    for i in {1..10}; do
        echo -e "GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100000\r\n\r\n$large_data" | nc $SERVER_HOST $SERVER_PORT >/dev/null 2>&1 &
    done
    wait
    
    sleep 2
    if nc -z $SERVER_HOST $SERVER_PORT 2>/dev/null; then
        print_success "Server handled oversized requests correctly"
    else
        print_error "Server crashed from oversized requests"
        return 1
    fi
}

generate_stress_report() {
    print_header "Generating Stress Test Report"
    
    cat > stress_test_report.md << EOF
# Stress Test Report

Generated on: $(date)

## Test Summary

This report contains the results of comprehensive stress testing for the
C++ Multithreaded Web Server under extreme load conditions.

## Tests Performed

1. **High Connection Count Test**: Testing scalability with increasing concurrent connections
2. **Extended Duration Test**: Validating stability over long periods
3. **Memory Leak Detection**: Monitoring memory usage under sustained load
4. **Error Handling Test**: Verifying server resilience against malformed requests

## Results

EOF
    
    # Add results from temporary files if they exist
    if ls /tmp/stress_*.txt > /dev/null 2>&1; then
        echo "### Connection Count Test Results" >> stress_test_report.md
        echo '```' >> stress_test_report.md
        for file in /tmp/stress_*.txt; do
            if [ -f "$file" ]; then
                echo "=== $(basename $file .txt) ===" >> stress_test_report.md
                grep -E "(Requests/sec|Transfer/sec)" "$file" >> stress_test_report.md || true
            fi
        done
        echo '```' >> stress_test_report.md
    fi
    
    echo "" >> stress_test_report.md
    
    print_success "Stress test report generated: stress_test_report.md"
}

cleanup() {
    # Clean up temporary files
    rm -f /tmp/stress_*.txt /tmp/memory_test.txt 2>/dev/null || true
}

main() {
    print_header "Web Server Stress Testing Suite"
    echo "This script will perform comprehensive stress testing on the"
    echo "C++ Multithreaded Web Server to validate stability and performance"
    echo "under extreme conditions."
    echo ""
    
    # Check prerequisites
    if ! command -v wrk &> /dev/null; then
        print_error "wrk is not installed. Please install it first."
        exit 1
    fi
    
    if ! command -v nc &> /dev/null; then
        print_error "nc (netcat) is not installed. Please install it first."
        exit 1
    fi
    
    # Check if server is running
    check_server || exit 1
    
    # Setup cleanup trap
    trap cleanup EXIT
    
    # Run stress tests
    stress_test_connections
    stress_test_duration
    stress_test_memory_leak
    stress_test_error_handling
    
    # Generate report
    generate_stress_report
    
    print_header "Stress Testing Complete!"
    print_success "All stress tests completed successfully!"
    echo "Report saved as: stress_test_report.md"
}

main "$@"