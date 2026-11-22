import http.client
import json
import time
import threading
import sys

HOST = "127.0.0.1"
PORT = 8080

def generate_batch(count=1000):
    now = int(time.time() * 1000)
    batch = []
    for i in range(count):
        batch.append({
            "timestamp": now,
            "level": "INFO" if i % 10 != 0 else ("WARN" if i % 20 != 0 else "ERROR"),
            "service": f"service-{(i % 5) + 1}",
            "message": f"User payment transaction processed for id={100000 + i} status=completed latency=12ms",
            "trace_id": f"trace-{1000 + (i // 10)}"
        })
    return batch

def worker(worker_id, batch_json_bytes, num_requests, results, barrier):
    conn = http.client.HTTPConnection(HOST, PORT, timeout=10)
    headers = {"Content-Type": "application/json", "Connection": "keep-alive"}
    
    # Warmup
    conn.request("POST", "/api/v1/logs", batch_json_bytes, headers)
    resp = conn.getresponse()
    resp.read()
    
    barrier.wait()
    t0 = time.perf_counter()
    
    success = 0
    for _ in range(num_requests):
        conn.request("POST", "/api/v1/logs", batch_json_bytes, headers)
        resp = conn.getresponse()
        resp.read()
        if resp.status == 200:
            success += 1
            
    t1 = time.perf_counter()
    conn.close()
    results.append((t1 - t0, success))

def run_test(num_workers, batch_size, total_logs):
    batch = generate_batch(batch_size)
    batch_json = json.dumps(batch).encode('utf-8')
    num_requests_per_worker = max(1, (total_logs // (batch_size * num_workers)))
    actual_total_logs = num_requests_per_worker * batch_size * num_workers
    
    barrier = threading.Barrier(num_workers)
    threads = []
    results = []
    
    t_start = time.perf_counter()
    for w in range(num_workers):
        t = threading.Thread(target=worker, args=(w, batch_json, num_requests_per_worker, results, barrier))
        threads.append(t)
        t.start()
        
    for t in threads:
        t.join()
    t_end = time.perf_counter()
    
    total_time = t_end - t_start
    total_successful_reqs = sum(r[1] for r in results)
    total_ingested = total_successful_reqs * batch_size
    rate = total_ingested / total_time if total_time > 0 else 0
    
    print(f"Workers: {num_workers:2d} | Batch Size: {batch_size:5d} | Logs: {total_ingested:7d} | Time: {total_time:6.2f}s | Rate: {rate:10.1f} logs/sec ({rate/1000:6.2f}k/s)")
    return rate

if __name__ == "__main__":
    print("=== SiftLog Ingestion Throughput Benchmark ===")
    
    # Check if server is running
    try:
        conn = http.client.HTTPConnection(HOST, PORT, timeout=2)
        conn.request("GET", "/api/v1/stats")
        r = conn.getresponse()
        print("Connected to SiftLog server. Current stats:", r.read().decode())
        conn.close()
    except Exception as e:
        print(f"Could not connect to SiftLog at {HOST}:{PORT}: {e}")
        sys.exit(1)

    print("\n--- 1 Worker (Sequential Keep-Alive) ---")
    run_test(num_workers=1, batch_size=100, total_logs=10000)
    run_test(num_workers=1, batch_size=1000, total_logs=50000)
    run_test(num_workers=1, batch_size=2500, total_logs=100000)

    print("\n--- 4 Workers (Concurrent Ingestion) ---")
    run_test(num_workers=4, batch_size=500, total_logs=50000)
    run_test(num_workers=4, batch_size=1000, total_logs=100000)
    run_test(num_workers=4, batch_size=2500, total_logs=200000)

    print("\n--- 8 Workers (High Concurrency Ingestion) ---")
    run_test(num_workers=8, batch_size=1000, total_logs=100000)
    run_test(num_workers=8, batch_size=2500, total_logs=250000)
    run_test(num_workers=8, batch_size=5000, total_logs=300000)
