### Run: vsync=off, nativeHandle=Qt QMetalLayer, postProcessing=on

Host: macOS Tahoe (26.5.1) / Qt 6.11.1 / screen 1920x1080 @ 60 Hz, Qt devicePixelRatio 1

| Phase | Points | Frames | Secs | FPS | CPU frame p50 (ms) | CPU frame p95 (ms) | GPU frame p50 (ms) | GPU frame p95 (ms) | Frame interval p95 (ms) | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 2M static | 2000000 | 1698 | 5.98 | 283.9 | 0.02 | 0.05 | 1.80 | 3.45 | 11.78 | psize=2.000000 |
| 5M static | 5000000 | 1176 | 5.61 | 209.7 | 0.02 | 0.06 | 3.31 | 5.54 | 10.31 | psize=2.000000 |
| 10M static | 10000000 | 826 | 5.95 | 138.8 | 0.05 | 0.11 | 6.92 | 8.53 | 8.73 | psize=2.000000 |
| 10M static, pointSize 1 | 10000000 | 499 | 3.62 | 137.8 | 0.04 | 0.09 | 7.01 | 8.43 | 8.51 | psize=1.0 |
| 10M static, pointSize 4 | 10000000 | 566 | 4.00 | 141.5 | 0.04 | 0.09 | 6.84 | 8.29 | 8.40 | psize=4.0 |
| live ingest 200k pts/s (0 -> N) | 4000000 | 5544 | 20.00 | 277.2 | 0.02 | 0.06 | 1.21 | 4.17 | 11.99 | ingested 4000000 pts = 200001 pts/s, ring drops 0 |
| 10M + live ingest 200k pts/s | 11912000 | 1234 | 9.56 | 129.1 | 0.05 | 0.11 | 3.35 | 8.30 | 9.33 | ingested 1912000 pts = 200041 pts/s |
| resize stress @ 10M | 11912000 | 1107 | 10.00 | 110.6 | 7.49 | 9.87 | 8.24 | 10.02 | 10.70 | 2328 resize requests, 1105 swapchain recreates, no crash |
| post-resize steady state | 11912000 | 478 | 3.99 | 119.7 | 0.05 | 0.10 | 8.34 | 9.60 | 9.64 | after resize storm |
| after minimize/restore | 11912000 | 443 | 4.00 | 110.8 | 0.06 | 0.12 | 8.41 | 9.59 | 9.64 | window survived minimize/restore |

<details><summary>run log</summary>

```
QWindow::winId() -> NSView, view.layer is QContainerLayer
native handle: Qt's own QMetalLayer (CAMetalLayer subclass) [sublayer]
Metal device: Apple M4
createSwapChain() OK
points.filamat loaded (24852 bytes)
init complete: 1280x773 px, dpr 1.000000, vsync off
preloaded to 500000 pts in 0.010429 s (1 pages, 20 MB GPU buffers)
preloaded to 2000000 pts in 0.017694 s (2 pages, 40 MB GPU buffers)
PHASE 2M static                    pts=2000000    frames=1698   5.98s -> 283.9 fps | cpu p50 0.02 p95 0.05 | gpu p50 1.80 p95 3.45 | interval p95 11.78 | idle ticks 641803 | psize=2.000000
preloaded to 5000000 pts in 0.027441 s (5 pages, 100 MB GPU buffers)
PHASE 5M static                    pts=5000000    frames=1176   5.61s -> 209.7 fps | cpu p50 0.02 p95 0.06 | gpu p50 3.31 p95 5.54 | interval p95 10.31 | idle ticks 602961 | psize=2.000000
preloaded to 10000000 pts in 0.047812 s (10 pages, 200 MB GPU buffers)
PHASE 10M static                   pts=10000000   frames=826    5.95s -> 138.8 fps | cpu p50 0.05 p95 0.11 | gpu p50 6.92 p95 8.53 | interval p95 8.73 | idle ticks 705741 | psize=2.000000
PHASE 10M static, pointSize 1      pts=10000000   frames=499    3.62s -> 137.8 fps | cpu p50 0.04 p95 0.09 | gpu p50 7.01 p95 8.43 | interval p95 8.51 | idle ticks 450822 | psize=1.0
PHASE 10M static, pointSize 4      pts=10000000   frames=566    4.00s -> 141.5 fps | cpu p50 0.04 p95 0.09 | gpu p50 6.84 p95 8.29 | interval p95 8.40 | idle ticks 499524 | psize=4.0
PHASE live ingest 200k pts/s (0 -> N) pts=4000000    frames=5544   20.00s -> 277.2 fps | cpu p50 0.02 p95 0.06 | gpu p50 1.21 p95 4.17 | interval p95 11.99 | idle ticks 2627182 | ingested 4000000 pts = 200001 pts/s, ring drops 0
preloaded to 10000000 pts in 0.062475 s (10 pages, 200 MB GPU buffers)
PHASE 10M + live ingest 200k pts/s pts=11912000   frames=1234   9.56s -> 129.1 fps | cpu p50 0.05 p95 0.11 | gpu p50 3.35 p95 8.30 | interval p95 9.33 | idle ticks 1142245 | ingested 1912000 pts = 200041 pts/s
=== resize stress: continuous window resize while rendering 10M+ pts ===
PHASE resize stress @ 10M          pts=11912000   frames=1107   10.00s -> 110.6 fps | cpu p50 7.49 p95 9.87 | gpu p50 8.24 p95 10.02 | interval p95 10.70 | idle ticks 1221 | 2328 resize requests, 1105 swapchain recreates, no crash
PHASE post-resize steady state     pts=11912000   frames=478    3.99s -> 119.7 fps | cpu p50 0.05 p95 0.10 | gpu p50 8.34 p95 9.60 | interval p95 9.64 | idle ticks 481709 | after resize storm
=== minimize/restore test ===
restored from minimized
PHASE after minimize/restore       pts=11912000   frames=443    4.00s -> 110.8 fps | cpu p50 0.06 p95 0.12 | gpu p50 8.41 p95 9.59 | interval p95 9.64 | idle ticks 433775 | window survived minimize/restore
benchmark complete
```
</details>
