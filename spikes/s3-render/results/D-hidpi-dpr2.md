### Run: vsync=off, nativeHandle=Qt QMetalLayer, postProcessing=on

Host: macOS Tahoe (26.5.1) / Qt 6.11.1 / screen 960x540 @ 60 Hz, Qt devicePixelRatio 2

| Phase | Points | Frames | Secs | FPS | CPU frame p50 (ms) | CPU frame p95 (ms) | GPU frame p50 (ms) | GPU frame p95 (ms) | Frame interval p95 (ms) | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 2M static | 2000000 | 2843 | 4.98 | 570.3 | 0.02 | 0.04 | 1.57 | 1.79 | 1.85 | psize=2.000000 |
| 5M static | 5000000 | 1221 | 4.49 | 271.7 | 0.03 | 0.06 | 3.39 | 3.97 | 4.09 | psize=2.000000 |
| 10M static | 10000000 | 735 | 4.95 | 148.6 | 0.04 | 0.09 | 6.68 | 7.20 | 7.24 | psize=2.000000 |
| 10M static, pointSize 1 | 10000000 | 508 | 3.47 | 146.5 | 0.05 | 0.10 | 6.78 | 7.28 | 7.33 | psize=1.0 |
| 10M static, pointSize 4 | 10000000 | 582 | 4.00 | 145.5 | 0.04 | 0.09 | 6.87 | 7.27 | 7.31 | psize=4.0 |
| live ingest 200k pts/s (0 -> N) | 2000000 | 11485 | 10.00 | 1148.1 | 0.02 | 0.05 | 0.56 | 1.44 | 1.52 | ingested 2000000 pts = 200002 pts/s, ring drops 0 |
| 10M + live ingest 200k pts/s | 11900000 | 1304 | 9.50 | 137.2 | 0.04 | 0.10 | 2.64 | 7.64 | 7.97 | ingested 1900000 pts = 200127 pts/s |
| resize stress @ 10M | 11900000 | 948 | 10.00 | 94.8 | 7.22 | 8.96 | 10.11 | 11.51 | 12.10 | 948 resize requests, 947 swapchain recreates, no crash |
| post-resize steady state | 11900000 | 445 | 4.00 | 111.3 | 0.05 | 0.12 | 8.93 | 9.37 | 9.39 | after resize storm |
| after minimize/restore | 11900000 | 422 | 4.00 | 105.5 | 0.05 | 0.12 | 8.86 | 9.24 | 9.28 | window survived minimize/restore |

<details><summary>run log</summary>

```
QWindow::winId() -> NSView, view.layer is QContainerLayer
native handle: Qt's own QMetalLayer (CAMetalLayer subclass) [sublayer]
Metal device: Apple M4
createSwapChain() OK
points.filamat loaded (24852 bytes)
init complete: 1600x918 px, dpr 2.000000, vsync off
preloaded to 500000 pts in 0.010443 s (1 pages, 20 MB GPU buffers)
preloaded to 2000000 pts in 0.014566 s (2 pages, 40 MB GPU buffers)
PHASE 2M static                    pts=2000000    frames=2843   4.98s -> 570.3 fps | cpu p50 0.02 p95 0.04 | gpu p50 1.57 p95 1.79 | interval p95 1.85 | idle ticks 514747 | psize=2.000000
preloaded to 5000000 pts in 0.028237 s (5 pages, 100 MB GPU buffers)
PHASE 5M static                    pts=5000000    frames=1221   4.49s -> 271.7 fps | cpu p50 0.03 p95 0.06 | gpu p50 3.39 p95 3.97 | interval p95 4.09 | idle ticks 548004 | psize=2.000000
preloaded to 10000000 pts in 0.051849 s (10 pages, 200 MB GPU buffers)
PHASE 10M static                   pts=10000000   frames=735    4.95s -> 148.6 fps | cpu p50 0.04 p95 0.09 | gpu p50 6.68 p95 7.20 | interval p95 7.24 | idle ticks 570975 | psize=2.000000
PHASE 10M static, pointSize 1      pts=10000000   frames=508    3.47s -> 146.5 fps | cpu p50 0.05 p95 0.10 | gpu p50 6.78 p95 7.28 | interval p95 7.33 | idle ticks 399623 | psize=1.0
PHASE 10M static, pointSize 4      pts=10000000   frames=582    4.00s -> 145.5 fps | cpu p50 0.04 p95 0.09 | gpu p50 6.87 p95 7.27 | interval p95 7.31 | idle ticks 459463 | psize=4.0
PHASE live ingest 200k pts/s (0 -> N) pts=2000000    frames=11485  10.00s -> 1148.1 fps | cpu p50 0.02 p95 0.05 | gpu p50 0.56 p95 1.44 | interval p95 1.52 | idle ticks 1047308 | ingested 2000000 pts = 200002 pts/s, ring drops 0
preloaded to 10000000 pts in 0.081138 s (10 pages, 200 MB GPU buffers)
PHASE 10M + live ingest 200k pts/s pts=11900000   frames=1304   9.50s -> 137.2 fps | cpu p50 0.04 p95 0.10 | gpu p50 2.64 p95 7.64 | interval p95 7.97 | idle ticks 1048131 | ingested 1900000 pts = 200127 pts/s
=== resize stress: continuous window resize while rendering 10M+ pts ===
PHASE resize stress @ 10M          pts=11900000   frames=948    10.00s -> 94.8 fps | cpu p50 7.22 p95 8.96 | gpu p50 10.11 p95 11.51 | interval p95 12.10 | idle ticks 0 | 948 resize requests, 947 swapchain recreates, no crash
PHASE post-resize steady state     pts=11900000   frames=445    4.00s -> 111.3 fps | cpu p50 0.05 p95 0.12 | gpu p50 8.93 p95 9.37 | interval p95 9.39 | idle ticks 434252 | after resize storm
=== minimize/restore test ===
restored from minimized
PHASE after minimize/restore       pts=11900000   frames=422    4.00s -> 105.5 fps | cpu p50 0.05 p95 0.12 | gpu p50 8.86 p95 9.24 | interval p95 9.28 | idle ticks 416091 | window survived minimize/restore
benchmark complete
```
</details>
