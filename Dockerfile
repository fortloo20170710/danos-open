# DANOS-Open source image (G4)
#
# stage 1 builds and tests the tree from source; stage 2 is a minimal
# runtime carrying danos-mgrd and the YANG models. Use
# danos-build/Dockerfile.mgrd when a prebuilt binary already exists.

###############################################################################
# Stage 1: build + test
###############################################################################
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    libssl-dev \
    openssl \
    python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build/src
COPY . /build/src/

RUN cmake -B /build/out -DCMAKE_BUILD_TYPE=Release -DDANOS_BUILD_TESTS=ON \
    && cmake --build /build/out -j"$(nproc)" \
    && ctest --test-dir /build/out --output-on-failure

###############################################################################
# Stage 2: runtime
###############################################################################
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3t64 \
    libgcc-s1 \
    iproute2 \
    netcat-openbsd \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/out/danos-mgrd/danos-mgrd /usr/bin/danos-mgrd
COPY --from=builder /build/src/danos-models/yang/ /etc/danos/models/
COPY --from=builder /build/src/danos-mgrd/danos-mgrd.service /lib/systemd/system/danos-mgrd.service

RUN mkdir -p /var/lib/danos

EXPOSE 59200 59201
VOLUME ["/var/lib/danos"]

# The daemon refuses plaintext gNMI unless DANOS_ALLOW_INSECURE=1 is set.
# Production deployments mount a TLS cert/key/client CA under /etc/danos
# and point DANOS_TLS_* at them (see docs/management-mtls.md).
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD nc -z 127.0.0.1 59201 || exit 1

ENTRYPOINT ["/usr/bin/danos-mgrd"]
CMD ["--port", "59200", "--metrics-port", "59201", "--wal", "/var/lib/danos/mgrd.wal"]
