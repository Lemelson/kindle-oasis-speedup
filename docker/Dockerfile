# Native armv7 hard-float build container (runs under QEMU on x86_64/arm64 hosts via Docker's binfmt support).
# The libraries are linked against the glibc 2.20 libraries copied from YOUR OWN Kindle (KLIBS), so they
# reference no symbol version newer than the device has.
FROM --platform=linux/arm/v7 debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev binutils file && rm -rf /var/lib/apt/lists/*
