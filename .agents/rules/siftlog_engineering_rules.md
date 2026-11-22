# SiftLog Engineering & Architecture Rules

## Core Principles
1. **Zero Runtime Dependencies**: The `siftlog` executable must remain fully statically linked on Windows (`-static -static-libgcc -static-libstdc++`) with no requirements for external DLLs, VC++ runtimes, or system packages.
2. **Ultra-Low Memory Footprint**: In-memory data structures must be bounded. Target footprint is under 15 MB RAM under standard ingestion load.
3. **No Heavy External JavaScript/CSS**: The embedded Web UI must remain self-contained, lightning-fast, and run smoothly directly from the single-binary assets (`src/web_assets.hpp`) without external CDN dependencies or npm build steps.
4. **Data Integrity & Non-destructive Deduplication**: FinOps noise deduplication applies strictly to `INFO` and `DEBUG` logs. Errors (`ERROR`, `FATAL`) and warnings (`WARN`) must never be dropped or deduplicated.
5. **Always Respond in Russian**: Consistent with global rules, communication with the user is in Russian.
