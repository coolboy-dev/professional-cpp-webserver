# Multi-stage Docker build for C++ Multithreaded Web Server
FROM ubuntu:22.04 AS builder

# Avoid interactive prompts during build
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libgtest-dev \
    libgmock-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /build

# Copy source code
COPY . .

# Create build directory and compile
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O3 -march=native" && \
    make -j$(nproc)

# Build tests (optional)
RUN cd build && \
    cmake .. -DBUILD_TESTS=ON && \
    make -j$(nproc)

# Production stage - minimal runtime image
FROM ubuntu:22.04 AS production

# Install only runtime dependencies
RUN apt-get update && apt-get install -y \
    libstdc++6 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user for security
RUN groupadd -r webserver && useradd -r -g webserver webserver

# Create directories
RUN mkdir -p /app/webroot /app/benchmarks && \
    chown -R webserver:webserver /app

# Copy binary and public files from builder stage
COPY --from=builder /build/build/bin/webserver /app/
COPY --from=builder /build/webroot /app/webroot/
COPY --from=builder /build/config.json /app/

# Set proper permissions
RUN chown -R webserver:webserver /app && \
    chmod +x /app/webserver

# Switch to non-root user
USER webserver
WORKDIR /app

# Expose port
EXPOSE 8080

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8080/ || exit 1

# Default command
CMD ["./webserver", "8080"]

# Development stage - includes development tools
FROM builder AS development

# Install additional development tools
RUN apt-get update && apt-get install -y \
    gdb \
    valgrind \
    strace \
    curl \
    wrk \
    apache2-utils \
    netcat \
    && rm -rf /var/lib/apt/lists/*

# Copy scripts
COPY bin/ /build/bin/
RUN chmod +x /build/bin/*.sh

# Set working directory
WORKDIR /build

# Default command for development
CMD ["bash"]
