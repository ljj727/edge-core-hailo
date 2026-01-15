# Stream Daemon Docker Image
# Hailo8 NPU + GStreamer 기반 실시간 추론 서버
#
# 빌드: docker build -t stream-daemon .
# 실행: docker run --device=/dev/hailo0 -v $(pwd)/config:/app/config -v $(pwd)/models:/app/models -p 50052:50052 stream-daemon

FROM ubuntu:22.04 AS builder

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
    # NATS & SSL
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

# HailoRT 설치 (Docker용 - systemctl 우회)
ARG HAILORT_VERSION=4.23.0
COPY hailort_${HAILORT_VERSION}_amd64.deb /tmp/
RUN cd /tmp && \
    dpkg-deb -x hailort_${HAILORT_VERSION}_amd64.deb /tmp/hailort && \
    cp -r /tmp/hailort/usr/* /usr/ && \
    ln -sf /usr/lib/libhailort.so.${HAILORT_VERSION} /usr/lib/libhailort.so && \
    ln -sf /usr/lib/libhailort.so.${HAILORT_VERSION} /usr/lib/libhailort.so.4 && \
    ldconfig && \
    rm -rf /tmp/hailort /tmp/hailort_${HAILORT_VERSION}_amd64.deb

# 소스 복사 및 빌드
WORKDIR /app
COPY CMakeLists.txt ./
COPY src/ ./src/
COPY include/ ./include/
COPY proto/ ./proto/

RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc)

# ============================================
# Runtime Stage
# ============================================
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Seoul

# 런타임 의존성만 설치
RUN apt-get update && apt-get install -y \
    # GStreamer 런타임
    libgstreamer1.0-0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    # gRPC 런타임
    libgrpc++1 \
    libprotobuf23 \
    # 기타 런타임
    libyaml-cpp0.7 \
    libzip4 \
    libjpeg8 \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

# HailoRT 런타임 (Docker용 - systemctl 우회)
ARG HAILORT_VERSION=4.23.0
COPY hailort_${HAILORT_VERSION}_amd64.deb /tmp/
RUN cd /tmp && \
    dpkg-deb -x hailort_${HAILORT_VERSION}_amd64.deb /tmp/hailort && \
    cp -r /tmp/hailort/usr/* /usr/ && \
    ln -sf /usr/lib/libhailort.so.${HAILORT_VERSION} /usr/lib/libhailort.so && \
    ln -sf /usr/lib/libhailort.so.${HAILORT_VERSION} /usr/lib/libhailort.so.4 && \
    ldconfig && \
    rm -rf /tmp/hailort /tmp/hailort_${HAILORT_VERSION}_amd64.deb

# NATS 라이브러리 복사
COPY --from=builder /usr/local/lib/libnats.so* /usr/local/lib/
RUN ldconfig

# 실행파일 복사
WORKDIR /app
COPY --from=builder /app/build/stream_daemon ./

# 외부 마운트용 디렉토리
RUN mkdir -p /app/config /app/models
VOLUME ["/app/config", "/app/models"]

# config.yaml 심볼릭 링크
RUN ln -sf /app/config/config.yaml /app/config.yaml

# 포트
EXPOSE 50052

# 실행
CMD ["./stream_daemon"]
