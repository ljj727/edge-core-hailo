# Stream Daemon Build Environment
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# 기본 빌드 도구
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    git \
    wget \
    # GStreamer
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-tools \
    # gRPC & Protobuf
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler \
    protobuf-compiler-grpc \
    # JSON & YAML
    nlohmann-json3-dev \
    libyaml-cpp-dev \
    # ZIP & JPEG
    libzip-dev \
    libjpeg-dev \
    # NATS C client (빌드 필요)
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# NATS C 라이브러리 빌드
RUN cd /tmp && \
    git clone https://github.com/nats-io/nats.c.git && \
    cd nats.c && \
    mkdir build && cd build && \
    cmake .. -DNATS_BUILD_STREAMING=OFF && \
    make -j$(nproc) && \
    make install && \
    ldconfig && \
    rm -rf /tmp/nats.c

WORKDIR /app

# 빌드 스크립트
CMD ["bash", "-c", "mkdir -p build && cd build && cmake .. && make -j$(nproc)"]
