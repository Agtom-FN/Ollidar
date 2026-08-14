### Run: vsync=off, nativeHandle=app CAMetalLayer, postProcessing=on

Host: macOS Tahoe (26.5.1) / Qt 6.11.1 / screen 1920x1080 @ 60 Hz, Qt devicePixelRatio 1

| Phase | Points | Frames | Secs | FPS | CPU frame p50 (ms) | CPU frame p95 (ms) | GPU frame p50 (ms) | GPU frame p95 (ms) | Frame interval p95 (ms) | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 2M static | 2000000 | 1179 | 3.98 | 296.0 | 0.02 | 0.05 | 1.82 | 3.57 | 11.74 | psize=2.000000 |
| 5M static | 5000000 | 835 | 3.96 | 210.6 | 0.02 | 0.05 | 3.27 | 5.56 | 10.23 | psize=2.000000 |
| 10M static | 10000000 | 552 | 3.95 | 139.9 | 0.03 | 0.08 | 6.86 | 8.34 | 8.48 | psize=2.000000 |
| 10M static, pointSize 1 | 10000000 | 548 | 4.00 | 137.0 | 0.03 | 0.08 | 7.04 | 8.48 | 8.57 | psize=1.0 |
| 10M static, pointSize 4 | 10000000 | 563 | 4.00 | 140.8 | 0.03 | 0.07 | 6.79 | 8.34 | 8.51 | psize=4.0 |
| live ingest 200k pts/s (0 -> N) | 1200000 | 2314 | 6.00 | 385.6 | 0.02 | 0.04 | 0.65 | 2.62 | 12.23 | ingested 1200000 pts = 200002 pts/s, ring drops 0 |
| 10M + live ingest 200k pts/s | 11984000 | 1288 | 9.92 | 129.8 | 0.04 | 0.09 | 3.37 | 8.18 | 9.18 | ingested 1984000 pts = 200117 pts/s |
| resize stress @ 10M | 11984000 | 1093 | 9.99 | 109.4 | 7.87 | 10.04 | 8.47 | 10.20 | 10.81 | 1745 resize requests, 1090 swapchain recreates, no crash |
| post-resize steady state | 11984000 | 470 | 4.00 | 117.5 | 0.06 | 0.11 | 8.46 | 9.73 | 9.78 | after resize storm |
| after minimize/restore | 11984000 | 450 | 4.00 | 112.5 | 0.04 | 0.10 | 8.31 | 9.60 | 9.64 | window survived minimize/restore |

<details><summary>run log</summary>

```
QWindow::winId() -> NSView, view.layer is QContainerLayer
native handle: app-created CAMetalLayer added as a sublayer of Qt's NSView
Metal device: Apple M4
createSwapChain() OK
points.filamat loaded (24852 bytes)
init complete: 1280x773 px, dpr 1.000000, vsync off
preloaded to 500000 pts in 0.010172 s (1 pages, 20 MB GPU buffers)
preloaded to 2000000 pts in 0.017034 s (2 pages, 40 MB GPU buffers)
PHASE 2M static                    pts=2000000    frames=1179   3.98s -> 296.0 fps | cpu p50 0.02 p95 0.05 | gpu p50 1.82 p95 3.57 | interval p95 11.74 | idle ticks 502553 | psize=2.000000
preloaded to 5000000 pts in 0.035074 s (5 pages, 100 MB GPU buffers)
PHASE 5M static                    pts=5000000    frames=835    3.96s -> 210.6 fps | cpu p50 0.02 p95 0.05 | gpu p50 3.27 p95 5.56 | interval p95 10.23 | idle ticks 477100 | psize=2.000000
preloaded to 10000000 pts in 0.053713 s (10 pages, 200 MB GPU buffers)
PHASE 10M static                   pts=10000000   frames=552    3.95s -> 139.9 fps | cpu p50 0.03 p95 0.08 | gpu p50 6.86 p95 8.34 | interval p95 8.48 | idle ticks 468980 | psize=2.000000
PHASE 10M static, pointSize 1      pts=10000000   frames=548    4.00s -> 137.0 fps | cpu p50 0.03 p95 0.08 | gpu p50 7.04 p95 8.48 | interval p95 8.57 | idle ticks 495240 | psize=1.0
PHASE 10M static, pointSize 4      pts=10000000   frames=563    4.00s -> 140.8 fps | cpu p50 0.03 p95 0.07 | gpu p50 6.79 p95 8.34 | interval p95 8.51 | idle ticks 489767 | psize=4.0
PHASE live ingest 200k pts/s (0 -> N) pts=1200000    frames=2314   6.00s -> 385.6 fps | cpu p50 0.02 p95 0.04 | gpu p50 0.65 p95 2.62 | interval p95 12.23 | idle ticks 784807 | ingested 1200000 pts = 200002 pts/s, ring drops 0
preloaded to 10000000 pts in 0.083727 s (10 pages, 200 MB GPU buffers)
PHASE 10M + live ingest 200k pts/s pts=11984000   frames=1288   9.92s -> 129.8 fps | cpu p50 0.04 p95 0.09 | gpu p50 3.37 p95 8.18 | interval p95 9.18 | idle ticks 1198933 | ingested 1984000 pts = 200117 pts/s
=== resize stress: continuous window resize while rendering 10M+ pts ===
PHASE resize stress @ 10M          pts=11984000   frames=1093   9.99s -> 109.4 fps | cpu p50 7.87 p95 10.04 | gpu p50 8.47 p95 10.20 | interval p95 10.81 | idle ticks 652 | 1745 resize requests, 1090 swapchain recreates, no crash
PHASE post-resize steady state     pts=11984000   frames=470    4.00s -> 117.5 fps | cpu p50 0.06 p95 0.11 | gpu p50 8.46 p95 9.73 | interval p95 9.78 | idle ticks 486064 | after resize storm
=== minimize/restore test ===
restored from minimized
PHASE after minimize/restore       pts=11984000   frames=450    4.00s -> 112.5 fps | cpu p50 0.04 p95 0.10 | gpu p50 8.31 p95 9.60 | interval p95 9.64 | idle ticks 381696 | window survived minimize/restore
benchmark complete
```
</details>
