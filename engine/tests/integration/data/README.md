# E2 golden fixtures — provenance

Both files here are **committed, synthetic, deterministic**, and are read by
`engine/tests/test_e2_replay_golden.cpp` (globbed automatically as
`tests/test_e2_*.cpp` — see that file's header for why it is *not* under
`tests/integration/`). They exist to catch a decode/regression bug that a
purely-generate-at-test-time fixture cannot: byte-for-byte stability across a
refactor. If either file's bytes ever change, the golden test's checksum/count
assertions are the ones that notice.

## `golden_d6.bin` (433 bytes)

One COIN-D6 synthetic revolution: a start packet (angle 0, 1.000 m) followed
by 6 data packets of 20 samples each, spanning 360° at a constant 1.000 m
range and intensity 128. Built by `d6test::build_revolution(6, 20, 1000, 128,
10)` — the exact S1 packet-builder helper (`engine/tests/packet_builder.h`)
every other D6 unit test in this repo already trusts, including the vendor
checksum state-machine replay.

## `golden_mid360.bin` (11,160 bytes)

Eight synthetic Livox Mid-360 Cartesian-high point datagrams (96 points each,
CRC32/ISO-HDLC filled in with the engine's own `mid360::crc32_iso_hdlc`) with
two IMU datagrams interleaved after the 4th and 8th point packet. Concatenated
raw wire bytes — no container framing — because a Mid-360 datagram is
self-delimiting via `DataHeader::length`, exactly how the driver's `kInject`
backend (and a live UDP `recvfrom` loop) consumes it one datagram at a time.

## Regenerating

Both files are produced by the same builder functions the golden test itself
calls to cross-check the committed bytes (`CHECK(committed ==
build_golden_*_bytes())`), so "regenerate" only means something when the
builder's output is *meant* to change:

```sh
cmake --build build --target scanengine_tests
SCANENGINE_REGEN_GOLDEN=1 \
  ./build/scanengine_tests --test-case="e2/regenerate_golden_fixtures"
git diff --stat engine/tests/integration/data/
```

Review the diff before committing — a byte-identical rewrite is a no-op; a
changed rewrite means the builder (or something it depends on, e.g. the CRC32
routine or the D6 checksum replay) changed, which is exactly the event this
fixture is meant to catch.
