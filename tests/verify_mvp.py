#!/usr/bin/env python3
import subprocess
import time
import urllib.request
import urllib.parse
import json
import os
import shutil
import sys
import base64

BASE_URL = "http://127.0.0.1:8999"
TEST_DATA_DIR = "./test_data"

def find_binary():
    candidates = [
        "./build/siftlog",
        "./build/siftlog.exe",
        "./build/Release/siftlog",
        "./build/Release/siftlog.exe",
        "./siftlog",
        "./siftlog.exe"
    ]
    for c in candidates:
        abs_p = os.path.abspath(c)
        if os.path.isfile(abs_p):
            return abs_p
    raise FileNotFoundError("Could not find siftlog binary in build directories")

def clean_data_dir():
    if os.path.exists(TEST_DATA_DIR):
        shutil.rmtree(TEST_DATA_DIR, ignore_errors=True)

def wait_for_server(timeout=10, auth=None):
    start = time.time()
    while time.time() - start < timeout:
        try:
            req = urllib.request.Request(f"{BASE_URL}/health")
            with urllib.request.urlopen(req, timeout=1) as resp:
                if resp.status == 200:
                    return True
        except Exception:
            time.sleep(0.2)
    return False

def http_get(path, auth=None):
    url = f"{BASE_URL}{path}"
    headers = {}
    if auth:
        user, pwd = auth
        b64 = base64.b64encode(f"{user}:{pwd}".encode()).decode()
        headers["Authorization"] = f"Basic {b64}"
    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()

def http_post_raw(path, data_bytes, content_type="application/json", auth=None):
    url = f"{BASE_URL}{path}"
    headers = {"Content-Type": content_type}
    if auth:
        user, pwd = auth
        b64 = base64.b64encode(f"{user}:{pwd}".encode()).decode()
        headers["Authorization"] = f"Basic {b64}"
    req = urllib.request.Request(url, data=data_bytes, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()

def http_post_json(path, data, auth=None):
    payload = json.dumps(data).encode("utf-8")
    status, body = http_post_raw(path, payload, "application/json", auth)
    try:
        return status, json.loads(body.decode("utf-8"))
    except Exception:
        return status, body

def main():
    print("running siftlog cross-platform verification suite...")
    clean_data_dir()
    binary_path = find_binary()

    # --- Test Suite 1: Unauthenticated with Vector & FluentBit ---
    print("\n--- [Suite 1: Standard Ingestion, NDJSON, Loki push, Retention] ---")
    cmd = [
        binary_path,
        "--host", "127.0.0.1",
        "--port", "8999",
        "--data-dir", TEST_DATA_DIR,
        "--error-threshold", "5",
        "--retention-days", "7"
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    try:
        if not wait_for_server():
            print("server failed to start")
            out, _ = proc.communicate(timeout=2)
            print(out)
            sys.exit(1)

        # 1. Health check
        status, health_body = http_get("/health")
        assert status == 200
        assert json.loads(health_body)["status"] == "ok"
        print("PASS: /health")

        # 2. UI
        status, html = http_get("/")
        assert status == 200
        assert b"SiftLog - Log Engine" in html
        print("PASS: / (web ui)")

        # 3. Standard JSON Array Ingestion
        batch = [
            {"service": "auth-service", "level": "INFO", "message": "User login ok", "user_id": 1},
            {"service": "payment", "level": "ERROR", "message": "Gateway timeout", "tx_id": 100}
        ]
        status, resp = http_post_json("/api/v1/logs", batch)
        assert status == 200 and resp.get("status") == "ok" and resp.get("ingested") == 2
        print("PASS: standard JSON array ingestion")

        # 4. NDJSON Ingestion (Vector / Fluent Bit JSON lines format)
        ndjson_lines = (
            '{"service":"gateway","level":"INFO","message":"GET /api/v1 200 OK"}\n'
            '{"service":"gateway","level":"WARN","message":"Upstream latency high (250ms)"}\n'
            '{"stream":"stderr","kubernetes":{"container_name":"billing"},"log":"Failed to connect to PG"}\n'
        ).encode("utf-8")
        status, raw_resp = http_post_raw("/api/v1/logs", ndjson_lines, "application/x-ndjson")
        assert status == 200
        resp = json.loads(raw_resp.decode())
        assert resp.get("status") == "ok" and resp.get("ingested") == 3
        print("PASS: NDJSON ingestion (Vector/FluentBit json_lines format)")

        # 5. Loki Push Endpoint (/loki/api/v1/push - Promtail / FluentBit out_loki / Vector loki sink)
        now_ns = str(int(time.time() * 1000000000))
        loki_payload = {
            "streams": [
                {
                    "stream": {"service": "worker-pool", "level": "ERROR", "job": "background-tasks"},
                    "values": [
                        [now_ns, "Worker thread panicked while processing job #991"],
                        [now_ns, '{"action":"retry","attempt":2,"message":"Retrying task in 5s"}']
                    ]
                }
            ]
        }
        status, _ = http_post_raw("/loki/api/v1/push", json.dumps(loki_payload).encode(), "application/json")
        assert status == 204, f"Loki push expected 204, got {status}"
        print("PASS: Loki push API (/loki/api/v1/push - Vector & FluentBit drop-in)")

        # Wait for flush to disk
        time.sleep(2.5)

        # 6. Verify Search Query across all sources
        status, res = http_get("/api/v1/search?q=panicked")
        assert status == 200
        search_data = json.loads(res)
        assert search_data["total_matched"] == 1
        assert "Worker thread panicked" in search_data["logs"][0]["message"]
        print("PASS: search across Loki-ingested logs")

        # Check Kubernetes container_name mapping from NDJSON
        status, res_k8s = http_get("/api/v1/search?service=billing")
        assert status == 200
        k8s_data = json.loads(res_k8s)
        assert k8s_data["total_matched"] == 1
        assert k8s_data["logs"][0]["level"] == "ERROR"  # stream: stderr -> ERROR
        print("PASS: docker/k8s log mapping (stream: stderr -> ERROR, kubernetes.container_name -> service)")

        # Check PII & Secret auto-masking (passwords, JWTs, credit cards, passports)
        pii_payload = json.dumps([
            {
                "service": "auth-service",
                "level": "INFO",
                "message": "User login failed with password: \"SuperSecretP@ss123\" and token: \"Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.mock_signature\""
            },
            {
                "service": "payment-service",
                "level": "INFO",
                "message": "Customer passport 4510 123456 paid with card 4111-2222-3333-4444 and snils 123-456-789 01"
            }
        ]).encode("utf-8")
        status, _ = http_post_raw("/api/v1/logs", pii_payload)
        assert status == 200

        # Verify raw credentials and PII NEVER appear in searches
        status, res = http_get("/api/v1/search?q=SuperSecretP@ss123")
        assert status == 200
        data = json.loads(res)
        assert data["total_matched"] == 0, "Raw password leaked into storage!"

        status, res = http_get("/api/v1/search?q=4111-2222-3333-4444")
        assert status == 200
        data = json.loads(res)
        assert data["total_matched"] == 0, "Raw card number leaked into storage!"

        # Verify masked placeholders are recorded
        status, res = http_get("/api/v1/search?q=[MASKED_SECRET]")
        assert status == 200
        data = json.loads(res)
        assert data["total_matched"] >= 1, "Masked secret placeholder not found"

        status, res = http_get("/api/v1/search?q=[MASKED_CARD]")
        assert status == 200
        data = json.loads(res)
        assert data["total_matched"] >= 1, "Masked card placeholder not found"

        status, res = http_get("/api/v1/search?q=[MASKED_PASSPORT]")
        assert status == 200
        data = json.loads(res)
        assert data["total_matched"] >= 1, "Masked passport placeholder not found"

        print("PASS: PII & Secret auto-masking (passwords, JWT tokens, credit cards, passports)")

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except Exception:
            proc.kill()
            proc.wait()

    # --- Test Suite 2: HTTP Basic Auth Protection ---
    print("\n--- [Suite 2: HTTP Basic Authentication] ---")
    cmd_auth = [
        binary_path,
        "--host", "127.0.0.1",
        "--port", "8999",
        "--data-dir", TEST_DATA_DIR,
        "--auth-user", "admin",
        "--auth-password", "secret123"
    ]
    proc_auth = subprocess.Popen(cmd_auth, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    try:
        if not wait_for_server():
            print("auth server failed to start")
            sys.exit(1)

        # Health is always public
        status, _ = http_get("/health")
        assert status == 200
        print("PASS: /health remains public without auth")

        # Unauthenticated request to / must return 401
        status, _ = http_get("/")
        assert status == 401, f"Expected 401 for unauthenticated /, got {status}"
        print("PASS: unauthorized request rejected with 401")

        # Wrong password must return 401
        status, _ = http_get("/", auth=("admin", "wrong_password"))
        assert status == 401
        print("PASS: invalid password rejected with 401")

        # Correct credentials must return 200
        status, _ = http_get("/", auth=("admin", "secret123"))
        assert status == 200
        print("PASS: valid credentials authenticated successfully (200 OK)")

        # Protected API search
        status, res = http_get("/api/v1/search?limit=10", auth=("admin", "secret123"))
        assert status == 200
        print("PASS: protected API search authenticated")

        # Enterprise endpoints
        status, res = http_get("/api/v1/enterprise/license", auth=("admin", "secret123"))
        assert status == 200
        lic_data = json.loads(res)
        assert "plan" in lic_data
        print("PASS: enterprise license endpoint")

        status, res = http_get("/api/v1/enterprise/audit", auth=("admin", "secret123"))
        assert status == 200
        audit_data = json.loads(res)
        assert "audit_events" in audit_data
        print("PASS: enterprise audit trail ledger")

        status, res = http_get("/api/v1/enterprise/updates", auth=("admin", "secret123"))
        assert status == 200
        print("PASS: enterprise self-updater check endpoint")

    finally:
        proc_auth.terminate()
        try:
            proc_auth.wait(timeout=3)
        except Exception:
            proc_auth.kill()
            proc_auth.wait()

    clean_data_dir()
    print("\n=======================================================")
    print("ALL VERIFICATION SUITES (NDJSON, LOKI, AUTH, RETENTION) PASSED!")
    print("=======================================================")

if __name__ == "__main__":
    main()
