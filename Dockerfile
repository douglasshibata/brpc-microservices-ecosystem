# ==========================================
# STAGE 1: Build Environment containing bRPC
# ==========================================
FROM ubuntu:22.04 AS builder

# Set env to prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install core build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    g++ \
    libgoogle-glog-dev \
    libgflags-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libleveldb-dev \
    libsnappy-dev \
    libssl-dev \
    libcrypto++-dev \
    zlib1g-dev \
    libbz2-dev \
    liblz4-dev \
    libzstd-dev \
    curl \
    unzip \
    && rm -rf /var/lib/apt/lists/*

# Clone and build Apache bRPC (pinned to stable release 1.11.0)
WORKDIR /opt
RUN git clone --depth 1 --branch 1.11.0 https://github.com/apache/brpc.git \
    && cd brpc \
    && mkdir build && cd build \
    && cmake -DCMAKE_INSTALL_PREFIX=/usr/local -DWITH_GLOG=ON .. \
    && make -j$(nproc) \
    && make install

# Setup project source directory
WORKDIR /app
COPY . .

# Run build compilation for server and client targets
RUN mkdir build && cd build \
    && cmake .. \
    && make -j$(nproc)

# ==========================================
# STAGE 2: Lightweight Runtime Deployment Image
# ==========================================
FROM ubuntu:22.04

# Install runtime dynamic dependencies
RUN apt-get update && apt-get install -y \
    libgoogle-glog0v5 \
    libgflags2.2 \
    libprotobuf23 \
    libleveldb1d \
    libssl3 \
    zlib1g \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy compiled dynamic library from build stage
COPY --from=builder /usr/local/lib/libbrpc.so* /usr/local/lib/
RUN ldconfig

# Copy compiled binaries from build stage
COPY --from=builder /app/build/brpc-server /usr/local/bin/brpc-server
COPY --from=builder /app/build/brpc-client /usr/local/bin/brpc-client
COPY --from=builder /app/proto/service.proto /app/proto/service.proto

# Default command
CMD ["brpc-server"]
