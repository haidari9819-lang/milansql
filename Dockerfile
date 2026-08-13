# MilanSQL Cloud — Docker Image
# Build: docker build -t milansql:12.0.0 .
# Run:   docker run -d -p 8080:8080 -v milansql-data:/var/lib/milansql milansql:12.0.0

# ── Stage 1: Build ────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential cmake libssl-dev git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF \
    && cmake --build build --target milansql -j$(nproc)

# ── Stage 2: Runtime ──────────────────────────────────────────
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    libssl3 curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home --shell /bin/false milansql \
    && mkdir -p /var/lib/milansql /etc/milansql \
    && chown milansql:milansql /var/lib/milansql \
    && chmod 750 /var/lib/milansql

COPY --from=builder /src/build/milansql /usr/local/bin/milansql

# Generate default config
RUN echo "DB_ROOT_PW=changeme_on_first_login" > /etc/milansql/db.conf \
    && chmod 640 /etc/milansql/db.conf

EXPOSE 8080

VOLUME ["/var/lib/milansql"]

USER milansql

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

ENTRYPOINT ["/usr/local/bin/milansql"]
CMD ["--port", "8080", "--data-dir", "/var/lib/milansql"]
