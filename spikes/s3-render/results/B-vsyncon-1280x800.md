### Run: vsync=on, nativeHandle=Qt QMetalLayer, postProcessing=on

Host: macOS Tahoe (26.5.1) / Qt 6.11.1 / screen 1920x1080 @ 60 Hz, Qt devicePixelRatio 1

| Phase | Points | Frames | Secs | FPS | CPU frame p50 (ms) | CPU frame p95 (ms) | GPU frame p50 (ms) | GPU frame p95 (ms) | Frame interval p95 (ms) | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 2M static | 2000000 | 300 | 4.97 | 60.4 | 0.03 | 0.05 | 4.11 | 5.25 | 18.44 | psize=2.000000 |
| 5M static | 5000000 | 300 | 4.95 | 60.6 | 0.03 | 0.05 | 7.12 | 8.80 | 18.04 | psize=2.000000 |
| 10M static | 10000000 | 298 | 4.92 | 60.5 | 0.03 | 0.06 | 10.76 | 11.90 | 17.81 | psize=2.000000 |
| 10M static, pointSize 1 | 10000000 | 240 | 4.00 | 60.0 | 0.03 | 0.04 | 10.86 | 11.84 | 17.86 | psize=1.0 |
| 10M static, pointSize 4 | 10000000 | 240 | 4.00 | 60.0 | 0.03 | 0.05 | 10.81 | 11.85 | 17.74 | psize=4.0 |
| live ingest 200k pts/s (0 -> N) | 2000000 | 601 | 10.00 | 60.1 | 0.03 | 0.05 | 0.33 | 1.20 | 18.76 | ingested 2000000 pts = 200001 pts/s, ring drops 0 |
| 10M + live ingest 200k pts/s | 11980000 | 596 | 9.90 | 60.2 | 0.03 | 0.05 | 2.67 | 3.20 | 17.80 | ingested 1980000 pts = 199937 pts/s |
| resize stress @ 10M | 11980000 | 600 | 10.00 | 60.0 | 15.53 | 16.53 | 9.88 | 10.81 | 17.80 | 601 resize requests, 600 swapchain recreates, no crash |
| post-resize steady state | 11980000 | 241 | 4.00 | 60.3 | 0.03 | 0.05 | 10.60 | 11.61 | 17.72 | after resize storm |
| after minimize/restore | 11980000 | 229 | 4.00 | 57.3 | 0.03 | 0.04 | 10.54 | 11.65 | 17.83 | window survived minimize/restore |

<details><summary>run log</summary>

```
QWindow::winId() -> NSView, view.layer is QContainerLayer
native handle: Qt's own QMetalLayer (CAMetalLayer subclass) [sublayer]
Metal device: Apple M4
createSwapChain() OK
points.filamat loaded (24852 bytes)
init complete: 1280x773 px, dpr 1.000000, vsync on
preloaded to 500000 pts in 0.010337 s (1 pages, 20 MB GPU buffers)
preloaded to 2000000 pts in 0.032659 s (2 pages, 40 MB GPU buffers)
PHASE 2M static                    pts=2000000    frames=300    4.97s -> 60.4 fps | cpu p50 0.03 p95 0.05 | gpu p50 4.11 p95 5.25 | interval p95 18.44 | idle ticks 639762 | psize=2.000000
preloaded to 5000000 pts in 0.049799 s (5 pages, 100 MB GPU buffers)
PHASE 5M static                    pts=5000000    frames=300    4.95s -> 60.6 fps | cpu p50 0.03 p95 0.05 | gpu p50 7.12 p95 8.80 | interval p95 18.04 | idle ticks 627430 | psize=2.000000
preloaded to 10000000 pts in 0.075080 s (10 pages, 200 MB GPU buffers)
PHASE 10M static                   pts=10000000   frames=298    4.92s -> 60.5 fps | cpu p50 0.03 p95 0.06 | gpu p50 10.76 p95 11.90 | interval p95 17.81 | idle ticks 653766 | psize=2.000000
PHASE 10M static, pointSize 1      pts=10000000   frames=240    4.00s -> 60.0 fps | cpu p50 0.03 p95 0.04 | gpu p50 10.86 p95 11.84 | interval p95 17.86 | idle ticks 524753 | psize=1.0
PHASE 10M static, pointSize 4      pts=10000000   frames=240    4.00s -> 60.0 fps | cpu p50 0.03 p95 0.05 | gpu p50 10.81 p95 11.85 | interval p95 17.74 | idle ticks 543918 | psize=4.0
PHASE live ingest 200k pts/s (0 -> N) pts=2000000    frames=601    10.00s -> 60.1 fps | cpu p50 0.03 p95 0.05 | gpu p50 0.33 p95 1.20 | interval p95 18.76 | idle ticks 1320754 | ingested 2000000 pts = 200001 pts/s, ring drops 0
preloaded to 10000000 pts in 0.094739 s (10 pages, 200 MB GPU buffers)
PHASE 10M + live ingest 200k pts/s pts=11980000   frames=596    9.90s -> 60.2 fps | cpu p50 0.03 p95 0.05 | gpu p50 2.67 p95 3.20 | interval p95 17.80 | idle ticks 1366007 | ingested 1980000 pts = 199937 pts/s
=== resize stress: continuous window resize while rendering 10M+ pts ===
PHASE resize stress @ 10M          pts=11980000   frames=600    10.00s -> 60.0 fps | cpu p50 15.53 p95 16.53 | gpu p50 9.88 p95 10.81 | interval p95 17.80 | idle ticks 1 | 601 resize requests, 600 swapchain recreates, no crash
PHASE post-resize steady state     pts=11980000   frames=241    4.00s -> 60.3 fps | cpu p50 0.03 p95 0.05 | gpu p50 10.60 p95 11.61 | interval p95 17.72 | idle ticks 419134 | after resize storm
=== minimize/restore test ===
restored from minimized
PHASE after minimize/restore       pts=11980000   frames=229    4.00s -> 57.3 fps | cpu p50 0.03 p95 0.04 | gpu p50 10.54 p95 11.65 | interval p95 17.83 | idle ticks 399148 | window survived minimize/restore
benchmark complete
```
</details>
