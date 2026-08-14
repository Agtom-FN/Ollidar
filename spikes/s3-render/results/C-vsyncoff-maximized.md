### Run: vsync=off, nativeHandle=Qt QMetalLayer, postProcessing=on

Host: macOS Tahoe (26.5.1) / Qt 6.11.1 / screen 1920x1080 @ 60 Hz, Qt devicePixelRatio 1

| Phase | Points | Frames | Secs | FPS | CPU frame p50 (ms) | CPU frame p95 (ms) | GPU frame p50 (ms) | GPU frame p95 (ms) | Frame interval p95 (ms) | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 2M static | 2000000 | 2735 | 4.98 | 548.7 | 0.02 | 0.04 | 1.68 | 1.95 | 2.11 | psize=2.000000 |
| 5M static | 5000000 | 1292 | 4.97 | 259.9 | 0.02 | 0.05 | 3.59 | 4.08 | 4.12 | psize=2.000000 |
| 10M static | 10000000 | 702 | 4.95 | 141.7 | 0.04 | 0.10 | 7.03 | 7.45 | 7.47 | psize=2.000000 |
| 10M static, pointSize 1 | 10000000 | 555 | 4.00 | 138.8 | 0.04 | 0.09 | 7.17 | 7.59 | 7.62 | psize=1.0 |
| 10M static, pointSize 4 | 10000000 | 569 | 4.00 | 142.3 | 0.04 | 0.09 | 7.00 | 7.44 | 7.48 | psize=4.0 |
| live ingest 200k pts/s (0 -> N) | 2000000 | 9991 | 10.00 | 998.9 | 0.02 | 0.04 | 0.58 | 1.61 | 2.26 | ingested 2000000 pts = 200001 pts/s, ring drops 0 |
| 10M + live ingest 200k pts/s | 11986000 | 1278 | 9.93 | 128.7 | 0.04 | 0.10 | 2.97 | 7.99 | 8.50 | ingested 1986000 pts = 200007 pts/s |
| resize stress @ 10M | 11986000 | 1086 | 10.00 | 108.6 | 8.17 | 10.07 | 8.58 | 10.20 | 10.82 | 2488 resize requests, 1081 swapchain recreates, no crash |
| post-resize steady state | 11986000 | 484 | 4.00 | 121.0 | 0.04 | 0.09 | 8.29 | 9.52 | 9.57 | after resize storm |
| after minimize/restore | 11986000 | 454 | 4.00 | 113.5 | 0.04 | 0.09 | 8.34 | 9.61 | 9.66 | window survived minimize/restore |

<details><summary>run log</summary>

```
QWindow::winId() -> NSView, view.layer is QContainerLayer
native handle: Qt's own QMetalLayer (CAMetalLayer subclass) [sublayer]
Metal device: Apple M4
createSwapChain() OK
points.filamat loaded (24852 bytes)
init complete: 1920x946 px, dpr 1.000000, vsync off
preloaded to 500000 pts in 0.008957 s (1 pages, 20 MB GPU buffers)
preloaded to 2000000 pts in 0.015166 s (2 pages, 40 MB GPU buffers)
PHASE 2M static                    pts=2000000    frames=2735   4.98s -> 548.7 fps | cpu p50 0.02 p95 0.04 | gpu p50 1.68 p95 1.95 | interval p95 2.11 | idle ticks 511341 | psize=2.000000
preloaded to 5000000 pts in 0.027698 s (5 pages, 100 MB GPU buffers)
PHASE 5M static                    pts=5000000    frames=1292   4.97s -> 259.9 fps | cpu p50 0.02 p95 0.05 | gpu p50 3.59 p95 4.08 | interval p95 4.12 | idle ticks 511382 | psize=2.000000
preloaded to 10000000 pts in 0.045120 s (10 pages, 200 MB GPU buffers)
PHASE 10M static                   pts=10000000   frames=702    4.95s -> 141.7 fps | cpu p50 0.04 p95 0.10 | gpu p50 7.03 p95 7.45 | interval p95 7.47 | idle ticks 513347 | psize=2.000000
PHASE 10M static, pointSize 1      pts=10000000   frames=555    4.00s -> 138.8 fps | cpu p50 0.04 p95 0.09 | gpu p50 7.17 p95 7.59 | interval p95 7.62 | idle ticks 414459 | psize=1.0
PHASE 10M static, pointSize 4      pts=10000000   frames=569    4.00s -> 142.3 fps | cpu p50 0.04 p95 0.09 | gpu p50 7.00 p95 7.44 | interval p95 7.48 | idle ticks 415269 | psize=4.0
PHASE live ingest 200k pts/s (0 -> N) pts=2000000    frames=9991   10.00s -> 998.9 fps | cpu p50 0.02 p95 0.04 | gpu p50 0.58 p95 1.61 | interval p95 2.26 | idle ticks 1020038 | ingested 2000000 pts = 200001 pts/s, ring drops 0
preloaded to 10000000 pts in 0.067253 s (10 pages, 200 MB GPU buffers)
PHASE 10M + live ingest 200k pts/s pts=11986000   frames=1278   9.93s -> 128.7 fps | cpu p50 0.04 p95 0.10 | gpu p50 2.97 p95 7.99 | interval p95 8.50 | idle ticks 1031367 | ingested 1986000 pts = 200007 pts/s
=== resize stress: continuous window resize while rendering 10M+ pts ===
PHASE resize stress @ 10M          pts=11986000   frames=1086   10.00s -> 108.6 fps | cpu p50 8.17 p95 10.07 | gpu p50 8.58 p95 10.20 | interval p95 10.82 | idle ticks 1402 | 2488 resize requests, 1081 swapchain recreates, no crash
PHASE post-resize steady state     pts=11986000   frames=484    4.00s -> 121.0 fps | cpu p50 0.04 p95 0.09 | gpu p50 8.29 p95 9.52 | interval p95 9.57 | idle ticks 407641 | after resize storm
=== minimize/restore test ===
restored from minimized
PHASE after minimize/restore       pts=11986000   frames=454    4.00s -> 113.5 fps | cpu p50 0.04 p95 0.09 | gpu p50 8.34 p95 9.61 | interval p95 9.66 | idle ticks 384895 | window survived minimize/restore
benchmark complete
```
</details>
