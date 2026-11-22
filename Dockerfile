# Multi-stage build for SiftLog
FROM alpine:3.20 AS builder

RUN apk add --no-cache \
    build-base \
    cmake \
    ninja \
    git \
    linux-headers

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=20
RUN cmake --build build --config Release --target siftlog

# Runtime image
FROM alpine:3.20

RUN apk add --no-cache libstdc++ libgcc tzdata ca-certificates \
    && adduser -D -u 10001 -s /sbin/nologin siftlog \
    && mkdir -p /data && chown -R siftlog:siftlog /data

COPY --from=builder /src/build/siftlog /usr/local/bin/siftlog

USER siftlog
WORKDIR /data
VOLUME ["/data"]

EXPOSE 8080

HEALTHCHECK --interval=15s --timeout=3s --retries=3 \
    CMD wget -qO- http://127.0.0.1:8080/health || exit 1

ENTRYPOINT ["/usr/local/bin/siftlog"]
CMD ["--host", "0.0.0.0", "--port", "8080", "--data-dir", "/data"]
