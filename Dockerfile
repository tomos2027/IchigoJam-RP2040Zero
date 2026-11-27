# IchigoJam RP2040-Zero版 ファームウェアのDockerビルド環境
# ツールチェーン (arm-none-eabi-gcc / cmake) と依存ライブラリ
# (pico-sdk 2.3.1 + TinyUSBパッチ / pico-extras) をすべて含みます。
# 使い方: docs/BUILD_DOCKER_JA.md を参照してください。

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        git \
        cmake \
        make \
        python3 \
        gcc-arm-none-eabi \
        libnewlib-arm-none-eabi \
        libstdc++-arm-none-eabi-newlib \
        g++ \
        pkg-config \
        libusb-1.0-0-dev \
    && rm -rf /var/lib/apt/lists/*

# ===== pico-sdk (2.3.1 / コミット 079c6f3) + サブモジュール + TinyUSBパッチ =====
RUN git clone https://github.com/raspberrypi/pico-sdk.git /opt/pico-sdk \
    && cd /opt/pico-sdk \
    && git checkout 079c6f3 \
    && git submodule update --init --recursive

COPY patches/tinyusb_osal_task_delay.patch /tmp/tinyusb_osal_task_delay.patch
RUN cd /opt/pico-sdk/lib/tinyusb \
    && git apply /tmp/tinyusb_osal_task_delay.patch \
    && rm /tmp/tinyusb_osal_task_delay.patch

# ===== pico-extras (コミット 52fd7a7) — pico-sdkの隣に配置 (自動認識される) =====
RUN git clone https://github.com/raspberrypi/pico-extras.git /opt/pico-extras \
    && cd /opt/pico-extras \
    && git checkout 52fd7a7

ENV PICO_SDK_PATH=/opt/pico-sdk

# ソースコードは実行時にホストからマウントする (-v)
WORKDIR /src

# NTSCV_SIG_PRESET は docker run -e NTSCV_SIG_PRESET=1 で上書き可能 (既定0=S_D)
CMD ["sh", "-c", "cmake -B build-docker -DNTSCV_SIG_PRESET=${NTSCV_SIG_PRESET:-0} && cmake --build build-docker -j$(nproc)"]
