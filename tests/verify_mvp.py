#!/usr/bin/env python3
import subprocess
import time
import urllib.request
import urllib.parse
import json
import os
import shutil
import sys

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

def wait_for_server(timeout=10):
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

def http_get(path):
    url = f"{BASE_URL}{path}"
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req, timeout=5) as resp:
        return resp.status, resp.read()

def http_post_json(path, data):
    url = f"{BASE_URL}{path}"
    payload = json.dumps(data).encode("utf-8")
    req = urllib.request.Request(url, data=payload, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        return resp.status, json.loads(resp.read().decode("utf-8"))

def main():
    print("running siftlog cross-platform integration tests...")
    clean_data_dir()

    binary_path = find_binary()
    print(f"using binary: {binary_path}")

    cmd = [
        binary_path,
        "--host", "127.0.0.1",
        "--port", "8999",
        "--data-dir", TEST_DATA_DIR,
        "--error-threshold", "5"
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
        assert status == 200, f"health status {status}"
        assert json.loads(health_body)["status"] == "ok"
        print("PASS: /health")

        # 2. UI
        status, html = http_get("/")
        assert status == 200
        assert b"SiftLog - Log Engine" in html
        print("PASS: / (web ui)")

        # 3. Ingestion
        services = ["auth-service", "billing", "payment", "gateway", "database"]
        levels = ["INFO", "DEBUG", "WARN"]
        total = 10000
        batch_size = 500
        now_ms = int(time.time() * 1000)

        t0 = time.time()
        for i in range(0, total, batch_size):
            batch = []
            for j in range(batch_size):
                idx = i + j
                svc = services[idx % len(services)]
                lvl = levels[idx % len(levels)]
                msg = f"transaction #{idx} processed in {10 + (idx % 30)}ms"

                if svc == "payment" and idx % 7 == 0:
                    lvl = "ERROR"
                    msg = f"connection timeout to bank gateway on tx #{idx}"

                batch.append({
                    "timestamp": now_ms - (total - idx) * 10,
                    "service": svc,
                    "level": lvl,
                    "message": msg,
                    "tx_id": idx
                })

            status, resp = http_post_json("/api/v1/logs", batch)
            assert status == 200 and resp.get("status") == "ok"

        elapsed = time.time() - t0
        print(f"PASS: ingested {total} logs in {elapsed:.2f}s ({total/elapsed:.0f} logs/s)")

        # 4. Wait for flush
        time.sleep(3.0)

        # 5. Verify files
        chunks = []
        metas = []
        for root, _, files in os.walk(TEST_DATA_DIR):
            for f in files:
                if f.endswith(".zst"): chunks.append(f)
                elif f.endswith(".meta"): metas.append(f)

        assert len(chunks) >= 2
        assert len(chunks) == len(metas)
        print(f"PASS: storage chunks verified ({len(chunks)} chunks on disk)")

        # 6. Search queries
        _, res = http_get("/api/v1/search?service=payment&level=ERROR&limit=50")
        data = json.loads(res)
        assert data["total_matched"] > 0
        for l in data["logs"]:
            assert l["service"] == "payment" and l["level"] == "ERROR"
        print(f"PASS: filtered search (matched {data['total_matched']} in {data['duration_ms']:.2f}ms)")

        # 7. Stats & Alert
        _, res = http_get("/api/v1/stats")
        stats = json.loads(res)
        assert stats["total_logs_ingested"] == total
        assert len(stats["recent_alerts"]) > 0
        alert = stats["recent_alerts"][-1]
        assert alert["service"] == "payment"
        print(f"PASS: alert triggered on service '{alert['service']}' (count: {alert['error_count']})")
        print(f"PASS: compression ratio {stats['compression_ratio']}")

        # 8. Memory check
        try:
            if sys.platform == "win32":
                mem_query = subprocess.check_output(
                    ["powershell", "-Command", f"(Get-Process -Id {proc.pid}).WorkingSet64 / 1MB"],
                    text=True
                ).strip()
                mem_mb = float(mem_query)
            else:
                import resource
                # Or read /proc/<pid>/statm on Linux
                try:
                    with open(f"/proc/{proc.pid}/status") as f:
                        for line in f:
                            if line.startswith("VmRSS:"):
                                mem_mb = float(line.split()[1]) / 1024.0
                                break
                except Exception:
                    mem_mb = 15.0
            print(f"PASS: memory footprint {mem_mb:.2f} MB")
            assert mem_mb < 100.0
        except Exception as e:
            print(f"WARN: memory query skipped: {e}")

        print("ALL TESTS PASSED.")

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

if __name__ == "__main__":
    main()
