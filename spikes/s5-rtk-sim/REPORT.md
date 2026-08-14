# Spike S5 — RTK simulation infrastructure

**Situation:** S5's original exit criteria (§4 Phase 0) call for RTK Fixed
on the bench with a real rover, an NTRIP loop, and captured NMEA+RTCM
logs. Per §7, S5 explicitly **waits for RTK rover hardware** — no rover
was reachable when this spike ran. Rather than block A10 (NTRIP client +
NMEA parsing + georef fusion, §3.4/§2.3) on hardware arrival, this spike
builds the simulation infrastructure needed to develop and test A10
realistically without a rover, so A10 can proceed now and only needs a
short real-hardware pass later to close the original S5 criteria.

**Status:** Simulation infra complete, self-test passing. This does
**not** close the original S5 exit criteria ("RTK Fixed on bench" needs
real hardware) — it de-risks A10's *implementation and test coverage* so
that when the rover arrives, the remaining work is bench validation, not
building test infrastructure under time pressure.

## What was built

```
spikes/s5-rtk-sim/
  nmea_sim.py            GNSS rover simulator: waypoint-loop trajectory,
                          NMEA 0183 GGA/RMC/GST/GSA/VTG at 1-5 Hz, scripted
                          fix-quality timelines, per-fix-state position
                          noise. Output modes: pty / tcp / file.
  ntrip_caster_sim.py     Minimal NTRIP v1/v2 caster: sourcetable, basic
                          auth per mountpoint, RTCM3 frame streaming, GGA
                          upload logging, configurable drop/reconnect
                          injection.
  rtcm_tool.py            RTCM3 framing: CRC-24Q, frame build/validate,
                          canned-stream generator. Standalone CLI
                          (validate / generate) and the library both
                          ntrip_caster_sim.py and test_roundtrip.py import.
  test_roundtrip.py       End-to-end self-test (10 cases, see below).
  PUBLIC_CASTERS.md       Researched list of real public/community NTRIP
                          options for a later real-hardware validation pass.
  requirements.txt        pynmea2 (used to cross-check sentence validity
                          with a strict third-party parser).
  fixtures/                Generated canned RTCM3 stream (gitignored,
                          regenerated on demand by rtcm_tool.py or
                          ntrip_caster_sim.py's first run).
  .venv/                   Python 3.9 virtualenv (gitignored).
```

Run it: `./.venv/bin/python3 -m unittest -v test_roundtrip` (or
`python3 test_roundtrip.py -v`).

## What is simulated faithfully vs. simplified

Being explicit about this matters: A10 should be built to the *real*
NTRIP/NMEA/RTCM standards using this tool as a standards-conformant test
harness, not to this tool's specific shortcuts.

### Faithful

- **NMEA 0183 sentence syntax & checksums.** `nmea_checksum` is the
  standard XOR-over-body algorithm; every sentence is verified against it
  in the self-test, and every sentence (including edge cases like a
  no-fix epoch with empty lat/lon fields) is additionally round-tripped
  through **pynmea2**, an independent, standards-based parser — this is
  the "a strict NMEA parser built against the standard accepts it" bar
  from the spike brief. Field counts, hemisphere letters, ddmm.mmmm /
  dddmm.mmmm formatting, and the NMEA 2.3+ mode-indicator field are all
  present and correctly positioned.
- **Fix-quality scripting and its effect on noise/GST.** The scenario
  state machine (`Scenario`/`ScenarioSegment`) drives GGA's quality
  field (0/1/2/4/5), GSA's fix-type, and RMC/VTG's mode indicator
  together, consistently, per epoch. Position noise is injected in local
  ENU meters *before* projecting back to lat/lon (not applied
  independently to lat and lon in degrees, which would be
  latitude-dependent and wrong), at 1-sigma magnitudes matching the
  brief: Fixed ~2 cm, Float ~30 cm, Single ~2 m (DGPS ~0.5 m added as a
  fifth state for completeness). GST's reported error fields are derived
  from the *same* per-state std devs actually used to perturb the
  position (with a small ±10% jitter so GST isn't suspiciously exact) —
  so a consumer that cross-checks "does GST's claimed uncertainty match
  the actual scatter of GGA fixes" will find them consistent, which is
  what A10's fusion-layer weighting (§3.4: "GNSS position factors
  weighted by fix quality") needs to be tested against.
- **RTCM3 transport framing.** `rtcm_tool.crc24q` implements the actual
  CRC-24Q polynomial (`x^24+x^23+x^18+x^17+x^14+x^11+x^10+x^7+x^6+x^5+x^4+x^3+x+1`,
  independently re-derived from the standard's generator polynomial and
  cross-checked against the `0x1864CFB` constant used in the
  implementation — see the module docstring) with the correct MSB-first,
  zero-initial-value, no-reflection convention RTCM specifies. Preamble
  (0xD3), 10-bit length field, and 3-byte CRC are all real; `iter_frames`
  resyncs on stray bytes the way a real streaming parser must, and the
  self-test includes a corrupted-CRC case to confirm failures are
  actually detected, not just happy-path framing.
- **NTRIP protocol shape.** Sourcetable on `GET /` (both the NTRIP v1
  bare `SOURCETABLE 200 OK` form and the v2 HTTP-headered form, selected
  by the presence of `Ntrip-Version: Ntrip/2.0`), HTTP Basic auth gating
  per mountpoint with a proper 401 + `WWW-Authenticate` challenge, a
  `STR;` sourcetable record with realistic fields, and — importantly for
  A10's fusion loop — logging of the rover's periodic **GGA uploads**
  sent back over the same connection (the standard way a client tells a
  VRS/Network-RTK caster where it is).
- **Reconnect exercising.** `drop_after_frames` / `max_drops` actually
  force-closes the stream mid-frame-count, and the self-test
  (`test_reconnect_after_drop_injection`) proves a client can reconnect
  and keep receiving valid frames afterward — this is meant to be
  pointed at A10's real reconnect logic later, not just exercised by the
  toy client in this repo.

### Simplified / not physically meaningful

- **RTCM payload contents are transport-valid, not semantically valid.**
  `rtcm_tool.make_payload` sets the correct 12-bit message number (DF002)
  at the start of the payload and fills the rest with deterministic
  pseudo-random bytes sized roughly like the real message (per the spike
  brief: "payload can be canned/replayed bytes"). **A10's RTCM3 message
  decoder (if/when it parses MSM/ephemeris content rather than just
  forwarding bytes to the rover) cannot be validated against this
  stream** — only the outer framing, CRC, and message-type dispatch can.
  If A10 needs semantic RTCM decoding tested, that needs real captured
  RTCM (e.g. from a public caster, see `PUBLIC_CASTERS.md`) or a proper
  RTCM encoder, neither of which this spike builds.
- **Position/trajectory model is flat-earth ENU**, not a full geodesic —
  fine at the scale of a walking loop (tens of meters) but would
  accumulate error at larger scales.
- **GEOID_SEP_M is a fixed placeholder** (-2.0 m), not looked up from the
  bundled EGM96 model per §3.4 — the GGA geoid-separation field is
  present and correctly formatted, just not numerically representative
  of Hong Kong's actual undulation.
- **Timestamps are a fixed simulated epoch** (2026-01-01T00:00:00 UTC +
  elapsed sim seconds), not wall-clock UTC — this was a deliberate choice
  for test determinism (the self-test recovers elapsed sim time directly
  from each sentence's own `hhmmss.ss` field rather than relying on
  packet arrival order, which is also more representative of how a real
  consumer should parse a stream). No date-rollover handling is
  implemented since no scenario here runs past 24h of sim time.
  `--time-scale` compresses *wall-clock pacing* only; sentence content
  and cadence in simulated time are unaffected — a client can't tell the
  difference except that epochs arrive faster.
- **PTY mode blocking behavior is a rough approximation of UART
  backpressure**, not a real serial line: writes block if the kernel pty
  buffer fills with no reader, which is *similar* to how a slow consumer
  would stall a real UART, but doesn't model baud-rate-accurate byte
  timing.
- **NTRIP caster simplifications:** no chunked transfer-encoding, no TLS
  (`ntrips://`), no NTRIP v1 "SOURCE" role (this only simulates the
  caster + rover/client sides, not a base station pushing in), no
  sourcetable caching semantics, single-process/no persistence, GSA is
  emitted as one combined `$GNGSA` rather than per-constellation
  `$GPGSA`/`$GLGSA`/... the way some real multi-GNSS receivers do (noted
  as a simplification — pynmea2 accepts our form fine, but a stricter
  consumer expecting per-constellation GSA should be tested against real
  receiver logs eventually).
- **Cannot simulate real-rover-only behaviors at all:** cold-start /
  time-to-first-fix, actual Bluetooth SPP pairing/latency/dropout
  quirks, real multipath-driven fix-state flapping, or genuine
  correction-age effects on solution quality. These remain
  hardware-only and are exactly why the original S5 bench-validation
  criteria still need to be closed once a rover is available — this
  spike narrows what's left to close, it doesn't eliminate it.

## How A10 should consume these tools

- **Unit/integration tests:** import `nmea_sim.GnssRoverSim` +
  `nmea_sim.TcpNmeaServer` and `ntrip_caster_sim.NtripCasterSim` +
  `ntrip_caster_sim.MountConfig` directly (as `test_roundtrip.py` does)
  rather than shelling out to the CLIs — it's faster and avoids
  subprocess stdout-buffering pitfalls (a long-running CLI's `print()`
  output is not flushed promptly when its stdout is piped rather than a
  tty; both CLIs now `sys.stdout.flush()` after their startup line for
  exactly this reason, but importing the classes sidesteps the whole
  issue).
- **Fix-quality gating / capture-UX tests:** drive a `Scenario` string
  that matches the specific transition you want to test (e.g. force
  Single immediately to test "capture UX warns/blocks below fix-quality
  threshold" from §3.4) rather than always using the default 90-second
  timeline; use `--time-scale` (or the `time_scale` constructor arg) to
  keep test runtime short regardless of the simulated scenario length.
- **NTRIP client reconnect tests:** use `MountConfig(drop_after_frames=N,
  max_drops=K)` to force exactly-once or exactly-K-times disconnects at
  a known point in the stream and assert the client's real reconnect
  logic recovers — this is the intended use of the feature, exercised
  end-to-end in `test_reconnect_after_drop_injection`.
- **Manual/dev-loop testing against the real engine binary:** use the
  CLIs as standalone processes — `nmea_sim.py --mode pty` for anything
  that expects a real serial device path (e.g. the engine's
  `BtNmeaSource`/serial transport in a test harness), `nmea_sim.py --mode
  tcp` for anything socket-based, `ntrip_caster_sim.py` for the NTRIP
  side. `rtcm_tool.py validate <file>` is also useful standalone for
  sanity-checking anything A10 writes to a `.lscan` GNSS stream or logs
  from a real caster.
- **Before M3 sign-off:** run A10 against at least one real public
  caster (see `PUBLIC_CASTERS.md` — RTK2go first for protocol-plumbing
  validation, then HK SatRef once the (free, but manually approved)
  application clears, since that's the production-relevant network) to
  catch anything this simulator's simplifications above would hide.

## Self-test result

`./.venv/bin/python3 -m unittest -v test_roundtrip`, run from
`spikes/s5-rtk-sim/` (Python 3.9.6, deps from `requirements.txt`
installed in `.venv/`):

```
test_checksums_and_fix_timeline (test_roundtrip.TestNmeaSimTcp) ... ok
test_auth_required (test_roundtrip.TestNtripCasterRoundtrip) ... ok
test_reconnect_after_drop_injection (test_roundtrip.TestNtripCasterRoundtrip) ... ok
test_sourcetable_v1 (test_roundtrip.TestNtripCasterRoundtrip) ... ok
test_sourcetable_v2 (test_roundtrip.TestNtripCasterRoundtrip) ... ok
test_stream_and_gga_upload (test_roundtrip.TestNtripCasterRoundtrip) ... ok
test_unknown_mountpoint (test_roundtrip.TestNtripCasterRoundtrip) ... ok
test_build_and_validate_roundtrip (test_roundtrip.TestRtcmFraming) ... ok
test_corrupted_frame_fails_crc (test_roundtrip.TestRtcmFraming) ... ok
test_generate_canned_stream (test_roundtrip.TestRtcmFraming) ... ok

----------------------------------------------------------------------
Ran 10 tests in 5.433s

OK
```

Verified stable across 5 consecutive runs (no flakiness observed) despite
the threaded caster/server implementation. `test_checksums_and_fix_timeline`
covers ~95 simulated seconds (475 epochs × 5 sentences) at 5 Hz,
compressed to ~2s of wall time via `time_scale=50`, checksum-verifying
every sentence and confirming the "RTK Fixed 60s → Float 20s → Single
10s → Fixed" timeline is reproduced (>98% per-epoch match against
expected quality, exact FIXED→FLOAT→SINGLE→FIXED transition sequence)
plus a `pynmea2` parse pass over the first 200 lines.

The CLI entry points (`nmea_sim.py --mode pty/tcp/file`,
`ntrip_caster_sim.py`, `rtcm_tool.py validate/generate`) were also each
manually smoke-tested as standalone subprocesses during development
(not part of the automated suite, since spawning real subprocesses and a
real pty is slower and less deterministic than driving the classes
in-process) — all five modes worked as expected.

## Best public-caster candidates for later real validation

Full detail and citations in `PUBLIC_CASTERS.md`. Short version:

1. **HK Geodetic Survey SatRef** (`ntrip.geodetic.gov.hk`) — both DGNSS
   and Network RTK are free of charge, but require a manual
   fax/email application to `geodetic@landsd.gov.hk`. Most
   production-relevant option since it's the actual local
   infrastructure; start the application early since approval isn't
   instant and idle accounts auto-terminate after 12 months.
2. **RTK2go** (`rtk2go.com:2101`) — zero-signup, free, use first to
   validate A10's NTRIP client plumbing against *some* real
   uncontrolled server. HK/East-Asia mountpoint coverage is thin and
   not guaranteed — check the live sourcetable before assuming a
   specific base is usably close.
3. **Centipede-RTK** / **Onocoy** — community networks worth a coverage
   check (`map.centipede-rtk.org`, onocoy's map) but not established as
   having a usably-close HK station as of this research pass.
