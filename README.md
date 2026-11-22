# SiftLog

SiftLog is a lightweight, high-performance on-premise log ingestion and search daemon written in modern C++20. It stores logs in compressed Zstandard chunks with sparse metadata indexing, avoiding the memory footprint of inverted full-text indexes.

It includes an embedded web dashboard, native support for running behind reverse proxies (Nginx, Apache HTTP Server, Caddy, HAProxy), and drop-in compatibility with **Fluent Bit** and **Vector**.

## Highlights

- **Low Memory Footprint:** Consumes ~10 MB RAM under load. Designed for single VPS deployments (1 vCPU, 1 GB RAM).
- **Zstandard Block Compression:** Typical 15x–25x compression ratio on structured JSON logs.
- **Sparse Chunk Indexing:** Skips non-matching chunks by timestamp and metadata without decompression.
- **Standalone Binary:** 2.2 MB statically linked executable with embedded web dashboard.
- **Fluent Bit & Vector Compatible:** Native support for Loki push API (`/loki/api/v1/push`) and NDJSON streaming.
- **PII & Secrets Auto-Masking:** Automatically scrubs passwords, JWTs, API keys, payment cards, and passports before saving to disk.
- **Log Retention Policy:** Automated background pruning of chunks older than N days (`--retention-days`).
- **Access Control:** Built-in HTTP Basic Auth for UI and API (`--auth-user`, `--auth-password`).
- **Reverse Proxy Ready:** Transparent support for `X-Forwarded-For`, `X-Real-IP`, subpath routing, and load balancer health checks (`/health`).
- **Error Tracking & Alerting:** Built-in sliding-window error spike detector with Telegram Bot API webhook support.

## Getting Started

### Build from Source

Dependencies (`zstd`, `cpp-httplib`, `nlohmann/json`) are resolved automatically via CMake `FetchContent`:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The resulting binary will be located at `build/siftlog` (`build/siftlog.exe` on Windows).

### Running

```bash
./build/siftlog --host 127.0.0.1 --port 8080 --data-dir ./data --retention-days 14
```

Command-line flags and matching environment variables:

| Flag | Environment Variable | Default | Description |
| :--- | :--- | :--- | :--- |
| `--host` | `SIFTLOG_HOST` | `0.0.0.0` | Bind IP (`127.0.0.1` recommended behind reverse proxy) |
| `--port` | `SIFTLOG_PORT` | `8080` | Listen port |
| `--data-dir` | `SIFTLOG_DATA_DIR` | `./data` | Path to store compressed log chunks |
| `--retention-days` | `SIFTLOG_RETENTION_DAYS` | `0` | Auto-prune chunks older than N days (`0` = disabled) |
| `--auth-user` | `SIFTLOG_AUTH_USER` | `""` | HTTP Basic Auth username (empty = public) |
| `--auth-password` | `SIFTLOG_AUTH_PASSWORD` | `""` | HTTP Basic Auth password |
| `--no-mask-pii` | `SIFTLOG_MASK_PII` | `enabled` | Disable automatic PII, secret, and token redaction |
| `--license` | `SIFTLOG_LICENSE` | `""` | Enterprise license file or key token |
| `--update-url` | `SIFTLOG_UPDATE_URL` | `""` | Enterprise update server hostname |
| `--audit-log` | `SIFTLOG_AUDIT_LOG` | `<data-dir>/audit.log` | Enterprise compliance audit trail file |
| `--error-threshold` | `SIFTLOG_ERROR_THRESHOLD` | `10` | Error spike threshold per service within 60s window |
| `--tg-token` | `SIFTLOG_TG_TOKEN` | `""` | Telegram Bot API token for alert dispatches |
| `--tg-chat-id` | `SIFTLOG_TG_CHAT_ID` | `""` | Telegram chat ID |

---

## Log Shipping Integrations

### 1. Vector Configuration

Point your Vector `loki` sink or `http` sink directly to SiftLog:

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

### 2. Fluent Bit Configuration

Using the built-in Loki output plugin:

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

Or using standard HTTP output with JSON lines:

```ini
[OUTPUT]
    Name         http
    Match        *
    Host         127.0.0.1
    Port         8080
    URI          /api/v1/logs
    Format       json_lines
```

---

## API Reference

### 1. Ingestion (`POST /api/v1/logs` or `POST /loki/api/v1/push`)

Supports single JSON objects, JSON arrays, and NDJSON (newline-delimited JSON):

```bash
curl -X POST http://127.0.0.1:8080/api/v1/logs \
  -H "Content-Type: application/json" \
  -d '[
    {
      "service": "billing",
      "level": "INFO",
      "message": "Payment charged successfully",
      "amount": 49.99
    },
    {
      "service": "auth",
      "level": "ERROR",
      "message": "LDAP connection timeout"
    }
  ]'
```

### 2. Search (`GET /api/v1/search`)

Parameters:
- `q`: Substring query (matches message or raw JSON)
- `service`: Filter by exact service name
- `level`: Filter by level (`INFO`, `WARN`, `ERROR`, `DEBUG`, `FATAL`)
- `from`: Start epoch timestamp in milliseconds
- `to`: End epoch timestamp in milliseconds
- `limit`: Max records to return (1 to 1000, default 100)

```bash
curl "http://127.0.0.1:8080/api/v1/search?service=billing&level=ERROR&q=timeout&limit=50"
```

### 3. Server Stats (`GET /api/v1/stats`)

Returns storage metrics, compression ratio, active services, and recent alert events.

```bash
curl http://127.0.0.1:8080/api/v1/stats
```

### 4. Health Check (`GET /health`)

Returns `{"status":"ok"}`. Always public, used by reverse proxies and load balancers.

---

## Reverse Proxy Setup

### Nginx

A complete configuration template is available in `deploy/nginx/siftlog.conf`:

```nginx
upstream siftlog_backend {
    server 127.0.0.1:8080;
    keepalive 32;
}

server {
    listen 80;
    server_name logs.example.com;

    client_max_body_size 50M;

    location / {
        proxy_pass http://siftlog_backend;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header Connection "";
        proxy_buffering off;
    }
}
```

### Apache HTTP Server

A complete virtual host configuration is available in `deploy/apache/siftlog.conf`:

```apache
<VirtualHost *:80>
    ServerName logs.example.com
    LimitRequestBody 52428800

    ProxyPreserveHost On
    ProxyRequests Off

    RequestHeader set X-Forwarded-Proto "http"
    RequestHeader set X-Real-IP "%{REMOTE_ADDR}s"

    ProxyPass / http://127.0.0.1:8080/ retry=1 acquire=3000 timeout=60 Keepalive=On
    ProxyPassReverse / http://127.0.0.1:8080/
</VirtualHost>
```

---

## License

Apache License 2.0.
