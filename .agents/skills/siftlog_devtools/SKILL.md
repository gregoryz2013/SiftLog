---
name: siftlog-devtools
description: Dedicated engineering workflows for building, testing, benchmarking, and maintaining SiftLog C++20 engine and its Web Dashboard.
---

# SiftLog DevTools Skill

This skill defines the canonical procedures for developing and verifying SiftLog.

## 1. Building the Binary
Always use the bundled CLion MinGW toolchain and Ninja build:
```powershell
$env:PATH = "C:\Program Files\JetBrains\CLion 2025.2.3\bin\mingw\bin;" + $env:PATH
& 'C:\Program Files\JetBrains\CLion 2025.2.3\bin\cmake\win\x64\bin\cmake.exe' --build cmake-build-debug
```

## 2. Running Verification Test Suites
Run the cross-platform integration suites using Python 3:
```powershell
py -3 tests/verify_mvp.py
py -3 tests/verify_enterprise_v4.py
```

## 3. Web UI Architecture
The UI source lives in `web/index.html`. Whenever `web/index.html` is updated, the C++ header `src/web_assets.hpp` must also be kept synchronized using the raw string literal `R"SIFTLOG_HTML(...)SIFTLOG_HTML"`.
