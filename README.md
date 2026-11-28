# SiftLog

[![CI / CD](https://github.com/gregoryz2013/SiftLog/actions/workflows/ci.yml/badge.svg)](https://github.com/gregoryz2013/SiftLog/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/gregoryz2013/SiftLog?display_name=tag)](https://github.com/gregoryz2013/SiftLog/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

Single-binary C++20 log ingestion and search engine. Stores logs in Zstandard-compressed chunks with sparse metadata indexing and per-chunk Bloom filters, keeping idle RAM usage around 10 MB without inverted full-text indexes or JVM overhead.

Includes an embedded web UI, structured query parser, automatic PII/secret redaction, crash-safe WAL ingestion, and drop-in compatibility with **Grafana** (Loki data source), **OpenTelemetry** (`/v1/logs`), **Elasticsearch Bulk** (`/_bulk`), **Vector**, **Fluent Bit**, and **Promtail**.

![SiftLog Web Dashboard](docs/dashboard.png)

## Quick Start

### Linux / macOS (Pre-built binary)

```sh
curl -fsSL https://raw.githubusercontent.com/gregoryz2013/SiftLog/main/install.sh | sh
siftlog --host 127.0.0.1 --port 8080 --data-dir ./data
```

Or download binaries directly from [GitHub Releases](https://github.com/gregoryz2013/SiftLog/releases) (`siftlog-linux-x86_64`, `siftlog-windows-x86_64.exe`, `siftlog-macos-arm64`).

### Docker

```sh
docker compose up -d
```

Open `http://127.0.0.1:8080` in your browser.

### Send Test Logs

```sh
curl -X POST http://127.0.0.1:8080/api/v1/logs \
  -H "Content-Type: application/json" \
  -d '[
    {"service": "api-gateway", "level": "INFO", "message": "POST /v1/checkout 200 OK", "trace_id": "trc-8812"},
    {"service": "payment-svc", "level": "ERROR", "message": "Upstream gateway timeout after 3000ms", "trace_id": "trc-8812"}
  ]'
```

## Benchmarks

Measured locally using [`tests/benchmark_ingest.py`](tests/benchmark_ingest.py) against a single `siftlog` instance (`1.21M` JSON log events ingested over HTTP keep-alive):

| Concurrency | Batch Size | Throughput |
| :--- | :---: | :--- |
| 1 worker | 1,000 | **25,590 logs/sec** (25.6k/s) |
| 4 workers | 1,000 | **64,796 logs/sec** (64.8k/s) |
| 8 workers | 2,500 | **110,582 logs/sec** (110.6k/s) |

- **Process Memory:** ~10 MB RAM idle, ~37.1 MB peak during 110k logs/sec ingestion.
- **Compression:** Typically **15x–25x** on real-world production JSON logs (e.g., Nginx, Kubernetes, application logs).

Reproduce ingestion throughput on your hardware:
```sh
python3 tests/benchmark_ingest.py
```

## Architecture & Capabilities

- **Multi-Node Cluster & Replication:** Scale horizontally across multiple nodes using `--peers` (parallel federated search, analytics, and distributed trace stitching across the cluster) and `--replicate-to` (async write replication for high availability).
- **Fast Search Without Full Decompression:** Each chunk stores a sparse index (`min_ts`, `max_ts`, `services`, `levels`, per-service/level counters) plus an **8192-bit Bloom filter** (`1 KB`), skipping non-matching chunks. Matching chunks run a **zero-allocation `string_view` line pre-filter** before JSON parsing, and wave-based **Top-N early-exit pruning** stops scanning older archives as soon as `limit` matches are guaranteed.
- **Analytics & Prometheus `/metrics`:** Built-in `GET /api/v1/analytics` (per-service volume, error rate %, top recurring errors) with an Analytics UI panel, plus a native Prometheus `/metrics` endpoint.
- **Hot In-Memory Chunk LRU Cache (`--cache-mb`):** Configurable RAM cache (default `256 MB`, scalable to `1024+ MB` for production servers or `0` for tiny VPS) keeps recent decompressed chunks in memory for sub-10ms searches, `| stats` aggregations, and trace lookups with zero disk I/O or Zstd overhead.
- **Memory-Safe C++20 Design:** Linear `O(N)` zero-regex PII redactor (immune to `std::regex` stack overflow / ReDoS on pathological inputs), bounds-checked standard containers (`-D_GLIBCXX_ASSERTIONS`), ~10 MB RAM idle, and ~37 MB under 110k logs/sec load (plus configurable `--cache-mb` for hot queries).
- **Zero External Dependencies:** Single statically linked binary (~2.2 MB) with embedded web dashboard.
- **Query Syntax & Aggregation Pipes:** Supports exact phrases (`"connection refused"`), wildcards (`service:auth-*`), boolean operators (`AND`, `OR`, `NOT`, `!`), and aggregation pipes (`| stats count() by service`, `| top 10 message`). All JSON tokens (including nested fields) are indexed in per-chunk Bloom filters.
- **Noise Deduplication (Opt-out & Lossless for Errors):** Sliding-window collapsing applies only to untagged `INFO`/`DEBUG` noise (e.g. repeated health checks) with `repeat_count` counters; `WARN`, `ERROR`, `FATAL`, and traced requests (`trace_id`) are never collapsed. Disable completely with `--no-noise-dedup` if exact per-event timestamps for `INFO`/`DEBUG` are required.
- **Distributed Trace Waterfall & RCA:** Log-based trace stitching (`trace_id`) with Gantt waterfall view and automated root-cause cascade detection.
- **PII & Secret Redaction (ReDoS-Safe):** Deterministic linear-time redactor masking passwords, bearer/JWT tokens, API keys, payment cards, and passport/SNILS numbers before writing to disk (disable with `--no-mask-pii` for raw logs).
- **Grafana & Multi-Protocol Ingestion:** Native support for **OpenTelemetry OTLP/HTTP** (`POST /v1/logs`), **Elasticsearch Bulk** (`POST /_bulk`), **Loki Push/Read API** (`/loki/api/v1/push`, `/loki/api/v1/query_range`, `/loki/api/v1/labels`, `/ready` with `|=`, `!=`, and `|~`), JSON arrays, and NDJSON — connect **Grafana** directly as a Loki data source with zero extra plugins.
- **S3 Offloading & Retention:** Non-blocking chunk sync to S3-compatible storage (AWS S3, MinIO, Yandex Cloud) and automated pruning (`--retention-days`).
- **Alerting & Security:** Sliding-window error spike detection with **Telegram** and **Webhook** notifications (Slack, Discord, Alertmanager bridge), HTTP Basic Auth, `X-Forwarded-For` / `X-Real-IP` support, `/health` probe, and SHA-256 hash-chained audit trail.

## Durability & Zero Data Lock-in

- **Write-Ahead Log (WAL):** Incoming log batches are appended and flushed to `<data-dir>/wal.log` before ingestion returns `200 OK`.
- **Atomic Chunk Commits:** Background flushes rotate `wal.log` → `wal.flushing`, write compressed data to `.zst.tmp` and metadata to `.meta.tmp`, atomically `rename` them into place, and only then delete `wal.flushing`.
- **Startup Recovery:** On startup, any leftover `.tmp` files are cleaned up and any uncommitted records in `wal.flushing` or `wal.log` are automatically replayed into compressed chunks.
- **Open Standard Format (Easy Migration):** Each `.zst` chunk is standard Zstandard-compressed NDJSON (`zstdcat`). If you ever outgrow a single node and migrate to ClickHouse or VictoriaLogs, export all logs with one command: `zstd -dc data/*/*.zst > export.ndjson`.

## Grafana Integration

You can use SiftLog's built-in UI or connect it directly to **Grafana** alongside Prometheus metrics:

1. In Grafana, go to **Connections → Data sources → Add data source → Loki**.
2. Set URL to `http://<siftlog-host>:8080`.
3. Click **Save & test** — `/ready`, `/loki/api/v1/labels`, and `/loki/api/v1/query_range` work out of the box.

## Known Limitations & Trade-offs

- **Cluster model:** SiftLog uses a lightweight stateless federated cluster (`--peers` + `--replicate-to`) rather than consensus-based Raft sharding. For petabyte-scale clusters, multi-tenancy RBAC, or full analytical LogsQL pipelines (`rate`, `regex` extraction on the fly), **VictoriaLogs** or **ClickHouse** are a better fit. SiftLog focuses on minimal resource usage (~10 MB RAM), zero-ops deployment, built-in PII redaction, and instant startup.
- **Query language scope:** Supports boolean text search (`AND`, `OR`, `NOT`), wildcards (`*`, `?`), `key:value` filters, aggregation pipes (`| stats count() by <field>`, `| top N <field>`), and service/error analytics (`/api/v1/analytics`, `/metrics`). Does not yet support on-the-fly regex field extraction or arbitrary math pipelines (`rate()`).
- **Wide unselective scans:** While Bloom filters, line pre-filtering, and Top-N early-exit pruning skip most work, queries requesting `limit=1000` for rare terms across 100 GB of matching chunks still scan matching chunks until `limit` results are found. For multi-terabyte global inverted full-text search, **VictoriaLogs** or **Elasticsearch** will outperform chunk-based engines.

## Log Shipping Integrations

### Vector

```toml
# vector.toml
[sources.app_logs]
type = "file"
include = ["/var/log/app/*.log"]

[sinks.siftlog]
type = "loki"
inputs = ["app_logs"]
endpoint = "http://127.0.0.1:8080"
compression = "none"

[sinks.siftlog.labels]
service = "{{ service }}"
level = "{{ level }}"
```

### Fluent Bit

```ini
# fluent-bit.conf
[SERVICE]
    Flush        1
    Log_Level    info

[INPUT]
    Name         tail
    Path         /var/log/containers/*.log
    Parser       docker
    Tag          kube.*

[OUTPUT]
    Name         loki
    Match        *
    Host         127.0.0.1
    Port         8080
    Labels       job=fluentbit
```

## Configuration

| Flag | Environment Variable | Default | Description |
| :--- | :--- | :--- | :--- |
| `--host` | `SIFTLOG_HOST` | `0.0.0.0` | Bind IP (`127.0.0.1` recommended behind reverse proxy) |
| `--port` | `SIFTLOG_PORT` | `8080` | Listen port |
| `--data-dir` | `SIFTLOG_DATA_DIR` | `./data` | Storage directory for compressed chunks |
| `--retention-days` | `SIFTLOG_RETENTION_DAYS` | `0` | Auto-prune chunks older than N days (`0` = disabled) |
| `--cache-mb` | `SIFTLOG_CACHE_MB` | `256` | Hot in-memory chunk LRU cache budget in MB (`0` = disabled) |
| `--auth-user` | `SIFTLOG_AUTH_USER` | `""` | HTTP Basic Auth username (empty = open) |
| `--auth-password` | `SIFTLOG_AUTH_PASSWORD` | `""` | HTTP Basic Auth password |
| `--no-mask-pii` | `SIFTLOG_MASK_PII` | `enabled` | Disable automatic PII and secret redaction |
| `--no-noise-dedup` | `SIFTLOG_NOISE_DEDUP` | `enabled` | Disable sliding-window noise deduplication |
| `--noise-window` | `SIFTLOG_NOISE_WINDOW` | `5` | Deduplication sliding window in seconds |
| `--s3-endpoint` | `SIFTLOG_S3_ENDPOINT` | `""` | S3 endpoint (e.g. `s3.amazonaws.com`, `storage.yandexcloud.net`) |
| `--s3-bucket` | `SIFTLOG_S3_BUCKET` | `""` | S3 bucket name for chunk offloading |
| `--s3-region` | `SIFTLOG_S3_REGION` | `us-east-1` | S3 region |
| `--s3-access-key` | `SIFTLOG_S3_ACCESS_KEY` | `""` | S3 Access Key ID |
| `--s3-secret-key` | `SIFTLOG_S3_SECRET_KEY` | `""` | S3 Secret Access Key |
| `--s3-prefix` | `SIFTLOG_S3_PREFIX` | `siftlog` | S3 object key prefix |
| `--audit-log` | `SIFTLOG_AUDIT_LOG` | `<data-dir>/audit.log` | Audit trail output path |
| `--error-threshold` | `SIFTLOG_ERROR_THRESHOLD` | `10` | Error spike threshold per service per 60s window |
| `--tg-token` | `SIFTLOG_TG_TOKEN` | `""` | Telegram Bot API token for alerts |
| `--tg-chat-id` | `SIFTLOG_TG_CHAT_ID` | `""` | Telegram chat ID |
| `--alert-webhook` | `SIFTLOG_ALERT_WEBHOOK` | `""` | Generic webhook URL for alerts (Slack, Discord, Alertmanager) |
| `--peers` | `SIFTLOG_PEERS` | `""` | Comma-separated peer URLs for distributed cluster search |
| `--replicate-to` | `SIFTLOG_REPLICATE_TO` | `""` | Comma-separated replica URLs for async write replication |

## API Reference

- `POST /api/v1/logs` — Ingest single JSON object, JSON array, or NDJSON (`application/x-ndjson`).
- `POST /v1/logs` — OpenTelemetry OTLP/HTTP JSON logs ingestion.
- `POST /_bulk` / `POST /{index}/_bulk` — Elasticsearch Bulk API compatible ingestion.
- `POST /loki/api/v1/push` — Loki-compatible push endpoint (returns `204 No Content`).
- `GET /loki/api/v1/query_range` / `GET /loki/api/v1/query` — Grafana Loki-compatible log query API.
- `GET /loki/api/v1/labels` / `GET /loki/api/v1/label/{name}/values` — Label discovery for Grafana.
- `GET /api/v1/search` — Query search (`q`, `service`, `level`, `from`, `to`, `limit`).
- `GET /api/v1/analytics` — Service volume breakdown, error rate %, and top recurring errors.
- `GET /metrics` — Prometheus metrics exposition endpoint.
- `GET /api/v1/histogram` — Time-bucketed log volume and error counts (`q`, `service`, `level`, `from`, `to`, `step`).
- `GET /api/v1/trace` — Reconstruct distributed trace waterfall by `trace_id`.
- `GET /api/v1/stats` — Storage size, compression ratio, known services, and recent alerts.
- `GET /health` / `GET /ready` — Public health and readiness probe endpoints.

## Reverse Proxy & Systemd

Configuration templates are included in `deploy/`:
- `deploy/nginx/siftlog.conf`
- `deploy/apache/siftlog.conf`
- `deploy/systemd/siftlog.service`

## Build from Source

Requires a C++20 compiler, CMake 3.25+, and Ninja. Dependencies (`zstd`, `cpp-httplib`, `nlohmann/json`) are fetched automatically via CMake `FetchContent`.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Run the integration test suite:

```sh
python3 tests/verify_mvp.py
```

## License

Apache License 2.0. See [LICENSE](LICENSE).
