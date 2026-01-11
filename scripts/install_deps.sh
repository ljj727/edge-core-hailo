#!/bin/bash
set -e

echo "======================================"
echo "Stream Daemon 의존성 설치 스크립트"
echo "======================================"

# Root 권한 체크
if [ "$EUID" -eq 0 ]; then 
    echo "⚠️  이 스크립트를 root로 실행하지 마세요"
    exit 1
fi

# OS 확인
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
    VER=$VERSION_ID
else
    echo "❌ 지원하지 않는 OS입니다"
    exit 1
fi

echo "감지된 OS: $OS $VER"

# Ubuntu/Debian 전용
if [[ "$OS" != "ubuntu" && "$OS" != "debian" ]]; then
    echo "❌ Ubuntu 또는 Debian만 지원합니다"
    exit 1
fi

echo ""
echo "1. 시스템 업데이트..."
sudo apt update
sudo apt upgrade -y

echo ""
echo "2. 빌드 도구 설치..."
sudo apt install -y \
    build-essential \
    cmake \
    pkg-config \
    git \
    wget \
    curl

echo ""
echo "3. GStreamer 설치..."
sudo apt install -y \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-tools

# GStreamer 버전 확인
GST_VERSION=$(gst-launch-1.0 --version | grep version | awk '{print $3}')
echo "✅ GStreamer $GST_VERSION 설치 완료"

echo ""
echo "4. gRPC 및 Protocol Buffers 설치..."
sudo apt install -y \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc

# protoc 버전 확인
PROTOC_VERSION=$(protoc --version | awk '{print $2}')
echo "✅ Protocol Buffers $PROTOC_VERSION 설치 완료"

echo ""
echo "5. NATS C Client 설치..."
if [ ! -d "/tmp/nats.c" ]; then
    cd /tmp
    git clone https://github.com/nats-io/nats.c.git
    cd nats.c
    mkdir -p build && cd build
    cmake ..
    make -j$(nproc)
    sudo make install
    sudo ldconfig
    echo "✅ NATS C Client 설치 완료"
else
    echo "ℹ️  NATS C Client가 이미 설치되어 있습니다"
fi

echo ""
echo "6. JSON 라이브러리 설치..."
sudo apt install -y nlohmann-json3-dev

echo ""
echo "7. YAML 설정 파일 라이브러리 설치..."
sudo apt install -y libyaml-cpp-dev

echo ""
echo "8. ZIP 라이브러리 설치 (모델 업로드용)..."
sudo apt install -y libzip-dev

echo ""
echo "9. Hailo SDK 확인..."
if command -v hailortcli &> /dev/null; then
    HAILO_VERSION=$(hailortcli fw-control identify 2>&1 | grep "Firmware Version" | awk '{print $4}' || echo "Unknown")
    echo "✅ Hailo SDK 설치됨 (버전: $HAILO_VERSION)"
else
    echo "⚠️  Hailo SDK가 설치되지 않았습니다"
    echo "   다음 링크에서 다운로드하세요: https://hailo.ai/developer-zone/"
    echo "   HailoRT 설치 후 다음 명령어를 실행하세요:"
    echo "   sudo dpkg -i hailort-*.deb"
fi

# Hailo GStreamer 플러그인 확인
if gst-inspect-1.0 hailonet &> /dev/null; then
    echo "✅ Hailo GStreamer 플러그인 설치됨"
else
    echo "⚠️  Hailo GStreamer 플러그인이 설치되지 않았습니다"
    echo "   다음 명령어로 설치하세요:"
    echo "   sudo apt install -y gstreamer1.0-hailort"
fi

echo ""
echo "======================================"
echo "의존성 설치 완료!"
echo "======================================"
echo ""
echo "다음 단계:"
echo "1. Hailo SDK 설치 (아직 안 했다면)"
echo "2. 프로젝트 빌드:"
echo "   cd /Users/ijongjin/snuailab/project/asdf"
echo "   mkdir build && cd build"
echo "   cmake .."
echo "   make -j\$(nproc)"
echo ""
