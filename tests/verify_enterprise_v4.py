import urllib.request
import urllib.parse
import json
import time
import sys

BASE_URL = "http://127.0.0.1:8080"

def get(path):
    url = f"{BASE_URL}{path}"
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req) as resp:
        return json.loads(resp.read().decode('utf-8'))

def post_json(path, data):
    url = f"{BASE_URL}{path}"
    body = json.dumps(data).encode('utf-8')
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req) as resp:
        content = resp.read().decode('utf-8')
        return json.loads(content) if content else {}

def main():
    print("=== Testing SiftLog v0.4.0 Enterprise Features ===")
    
    # 0. Health check
    try:
        h = get("/health")
        print(f"[OK] Health check: {h}")
    except Exception as e:
        print(f"[FAIL] Server not reachable: {e}")
        sys.exit(1)

    # 1. Test FinOps Noise Dropper (Deduplication)
    print("\n--- 1. Testing FinOps Noise Dropper ---")
    now = int(time.time() * 1000)
    svc = f"k8s-ingress-{int(time.time())}"
    noise_logs = [
        {"timestamp": now + i, "service": svc, "level": "INFO", "message": "GET /healthz 200 OK"}
        for i in range(25)
    ]
    # Add one ERROR that must NOT be deduplicated
    noise_logs.append({"timestamp": now + 30, "service": svc, "level": "ERROR", "message": "Upstream timeout 504"})
    
    post_json("/api/v1/logs", noise_logs)
    
    search_res = get(f"/api/v1/search?service={svc}&q=healthz")
    logs = search_res.get("logs", [])
    print(f"Ingested 25 identical logs. Matched records: {len(logs)}")
    assert len(logs) == 1, f"Expected 1 deduplicated entry, got {len(logs)}"
    dedup_log = logs[0]
    print(f"[OK] FinOps Deduplication verified! repeat_count = {dedup_log.get('repeat_count')}")
    assert dedup_log.get("repeat_count") == 25, f"Expected repeat_count=25, got {dedup_log.get('repeat_count')}"

    # 2. Test Distributed Trace Stitching & Waterfall
    print("\n--- 2. Testing Distributed Trace Stitching ---")
    trace_id = f"trace-order-{int(time.time())}"
    t0 = now + 100
    trace_spans = [
        {"timestamp": t0, "service": "api-gateway", "level": "INFO", "message": "POST /order incoming", "trace_id": trace_id},
        {"timestamp": t0 + 45, "service": "auth-service", "level": "INFO", "message": "JWT validated user=991", "trace_id": trace_id},
        {"timestamp": t0 + 110, "service": "inventory-svc", "level": "INFO", "message": "SKU item reserved", "trace_id": trace_id},
        {"timestamp": t0 + 185, "service": "payment-svc", "level": "INFO", "message": "Card charged $120.00", "trace_id": trace_id},
        {"timestamp": t0 + 240, "service": "api-gateway", "level": "INFO", "message": "HTTP 201 Created sent to client", "trace_id": trace_id}
    ]
    post_json("/api/v1/logs", trace_spans)

    trace_res = get(f"/api/v1/trace?trace_id={trace_id}")
    spans = trace_res.get("spans", [])
    print(f"Trace ID: {trace_res.get('trace_id')}, Duration: {trace_res.get('total_duration_ms')}ms, Spans: {len(spans)}")
    assert len(spans) == 5, f"Expected 5 spans, got {len(spans)}"
    assert spans[0]["service"] == "api-gateway"
    assert spans[0]["offset_ms"] == 0
    assert spans[-1]["service"] == "api-gateway"
    assert spans[-1]["offset_ms"] == 240
    print("[OK] Distributed Trace Stitching & Waterfall verified!")

    # 3. Test Root Cause Analysis (RCA) Engine
    print("\n--- 3. Testing Root Cause Analysis (RCA) Engine ---")
    t_err = now - 50000  # Earliest timestamp in the 60s window
    # First failure is auth-service (Root Cause)
    incident_logs = [
        {"timestamp": t_err, "service": "auth-service", "level": "FATAL", "message": "Redis cluster connection pool exhausted"},
        {"timestamp": t_err + 15, "service": "api-gateway", "level": "ERROR", "message": "Upstream auth failed HTTP 500"},
        {"timestamp": t_err + 25, "service": "order-service", "level": "ERROR", "message": "Cannot authorize cart session"},
        {"timestamp": t_err + 40, "service": "notification-service", "level": "ERROR", "message": "Failed to verify recipient token"}
    ]
    post_json("/api/v1/logs", incident_logs)

    rca_res = get("/api/v1/enterprise/incident/rca")
    print(f"RCA Incident: has_incident={rca_res.get('has_incident')}, status={rca_res.get('status')}")
    assert rca_res.get("has_incident") is True
    root = rca_res.get("root_cause", {})
    print(f"Root Cause: Service={root.get('service')}, Error=\"{root.get('error')}\"")
    assert root.get("service") == "auth-service", f"Expected root service auth-service, got {root.get('service')}"
    assert "Redis" in root.get("error")

    cascade = rca_res.get("cascade", {})
    impacted = cascade.get("impacted_services", [])
    print(f"Cascade: Total Errors={cascade.get('total_errors')}, Impacted Services={impacted}")
    assert "api-gateway" in impacted
    assert "order-service" in impacted
    print("[OK] Root Cause Analysis (RCA) Engine verified!")

    print("\n=== ALL ENTERPRISE v0.4.0 VERIFICATIONS PASSED SUCCESSFULLY! ===")

if __name__ == "__main__":
    main()
