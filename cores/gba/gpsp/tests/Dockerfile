# The image cores/gba/gpsp/tests/run.sh runs the tests in: Debian trixie for arm64 (natively on an arm64 computer, emulated
# elsewhere), with what builds gpSP, the frontend (with its KMS, ALSA and evdev parts) and the link test ROM.
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc g++ make libc6-dev libdrm-dev libasound2-dev pkg-config python3 patch curl ca-certificates git \
        gcc-arm-none-eabi binutils-arm-none-eabi \
    && rm -rf /var/lib/apt/lists/*
