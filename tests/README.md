# Regression checks

Run from the project root on Windows:

```powershell
python tests/test_cache_native.py
python tests/test_crypto_native.py
python tests/test_radio_native.py
python tests/test_web_origin_native.py
node --test tests/test_web_dashboard.cjs
```

Prerequisites: Python with the telemetry project's dependencies, host GCC,
Node.js and ESP-IDF 5.5.1 sources. These tests use fixtures and do not access a
radio, flight controller or private key. Temporary native binaries are removed
after testing.

| Suite | Coverage |
| --- | --- |
| Cache | Typed records, Python codec compatibility, ages, compact snapshots, allocation failures and concurrency |
| Crypto | AES-GCM vectors, tamper rejection and nonce counters using the production codec and mbedTLS |
| Radio | Production SX1278 driver, 64-byte SPI limit, full packets, FIFO wrap and transfer failures |
| Origin | Allowed hostname/IP entry points and cross-origin rejection |
| Dashboard | Instruments, plots, missing/stale data, bounded history, DOM reuse, fast polling and response ordering |

Set these environment variables when automatic tool/source discovery does not
match your installation:

| Variable | Value |
| --- | --- |
| `VFPR_PYTHON_DIR` | Python telemetry project containing `lora_link/telemetry.py` |
| `VFPR_HOST_GCC` | Host GCC executable |
| `IDF_PATH` | ESP-IDF source root |
