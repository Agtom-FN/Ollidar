# Public / community NTRIP casters for later live validation

`nmea_sim.py` + `ntrip_caster_sim.py` let A10 (NTRIP client + NMEA parsing +
georef fusion) be built and unit/integration-tested with **zero external
dependencies**. But they cannot validate two things a real caster does:
real network latency/jitter/reconnect behavior against someone else's
server, and (for a real base station) physically meaningful RTCM
corrections. Before RTK Fixed on real hardware is claimed as working
end-to-end, A10 (or a later field test) should also be run once against a
real public caster. This is a shortlist for that step, researched
2026-08 — re-verify pricing/coverage/hostnames before use, this kind of
thing drifts.

The owner is Hong Kong-based, so HK/East-Asia coverage is called out
explicitly for each option.

## Recommended order to try

1. **HK Geodetic Survey SatRef** — best accuracy/relevance if it works out (see caveats), HK-local
2. **RTK2go** — zero-friction, use first for "does our client's NTRIP/RTCM plumbing work at all"
3. **Centipede-RTK / Onocoy** — backup community networks, sparse-to-absent HK coverage but worth a mountpoint-table check
4. **IGS real-time (RTS/NTRIP broadcaster)** — sparse in HK, mainly useful for RINEX/testing global-network-grade streams, not for actual RTK Fixed at a bench in Hong Kong

---

## 1. Hong Kong Geodetic Survey (SatRef) — NTRIP

- **What:** The Survey & Mapping Office (Lands Department) SatRef reference-station network broadcasts DGNSS and **Network RTK (Net-RTK)** corrections over NTRIP. Service has run since 2007; the NTRIP host migrated to `ntrip.geodetic.gov.hk` as of 1 June 2023 (old domain retired 1 Sept 2023) — **use the new hostname**.
- **Cost:** Both DGNSS and Net-RTK are described as **"free of charge"** in the application guidance.
- **Signup:** Fill in an application form and return it by fax or email to `geodetic@landsd.gov.hk` (Geodetic Survey Section, phone +852 3168 2652). Not a self-service instant signup like RTK2go — expect a manual approval step.
- **Caveats:**
  - Registered accounts idle for **12 months are auto-terminated without warning** — don't register early and let the account rot before the bench-validation step.
  - The public-facing marketing pages are thin on hard technical detail (exact mountpoint names, message set, caster port) — get that from the application response / their NTRIP FAQ page rather than assuming; the general NTRIP default port (2101) is a reasonable starting guess but confirm.
  - This is the **most relevant** option for real HK validation: it's the actual local Network RTK infrastructure a shipping product's users would realistically rely on, so any A10 quirks specific to this caster's behavior (auth flow, sourcetable format, message set) are worth finding early.
- **Sources:** [Geodetic Survey of Hong Kong — NTrip Service](https://www.geodetic.gov.hk/en/satref/ntrip.htm) · [Geodetic Survey of Hong Kong — Network RTK](https://www.geodetic.gov.hk/en/satref/Net_RTK.htm) · [Geodetic Survey of Hong Kong — FAQ NTrip](https://www.geodetic.gov.hk/en/satref/faq_rtk_ntrip.htm) · [SatRef GNSS Raw Data Streams (RTCM) — data.gov.hk](https://data.gov.hk/en-data/dataset/hk-landsd-openmap-satref-raw-data-rtcm)

## 2. RTK2go

- **What:** Free, community-run NTRIP caster (rtk2go.com:2101). 800+ base stations online at any time, ~11,000+ registered mountpoints, no central quality control — it's whatever volunteers are broadcasting.
- **Cost:** Free.
- **Signup (rover/client role, which is all A10 needs):** **No registration.** Configure the NTRIP client with any valid-looking email address as the username and `none` as the password, point at `rtk2go.com:2101`, pick a mountpoint from the sourcetable (`GET /` lists them).
- **Caveats:**
  - **HK/East-Asia coverage is thin and not guaranteed to be near Hong Kong.** RTK2go's own regional-caster experiment currently only splits out **Japan** (a separate regional endpoint at `rtk2go.com:2104` filtering to Japan-tagged mountpoints) and Poland — there is no dedicated HK regional view, and general mountpoint coverage in East Asia is sparse and volunteer-dependent (stations can go offline any time with no SLA). Check the live sourcetable / monitor map (`monitor.use-snip.com`) at test time rather than assuming a specific mountpoint will be up.
  - Best used as the "prove the plumbing works" step (sourcetable parse, auth header format, GGA upload, reconnect) against *some* real caster — not as a guarantee of a usably-close base for actual RTK Fixed in HK.
- **Sources:** [RTK2go — Hassle Free RTK NTRIP Streaming](http://rtk2go.com/) · [RTK2go — free community NTRIP caster overview](https://innovation.world/online-tool/rtk2go/) · [RTK2go Regional Caster Tables](http://rtk2go.com/regionalcastertables/) · [Using the RTK2go NTRIP Caster — GNSS Store](https://gnss.store/blogs/elt-rtk-base/24-using-the-rtk2go-ntrip-caster)

## 3. Centipede-RTK

- **What:** Open-source, open-data community RTK base network (originally France-centric), running the "Millipede" NTRIP caster, with stated ambitions of global/anycast presence.
- **Cost:** Free, community/open-data model.
- **Signup:** Rover/client access follows the same no-signup NTRIP pattern as other open casters (check `map.centipede-rtk.org` sourcetable for current mountpoints and exact credential convention before connecting).
- **Caveats:** Base density is explicitly France/Europe-heavy; the project's own guidance is that a rover should be within ~10 km of a base for cm-level results (20 km base spacing). **Check `map.centipede-rtk.org` for any HK/East-Asia bases before relying on this** — as of this research pass, coverage there is not established and should be treated as unlikely to have a usably-close HK station. Worth a periodic check since the network is actively growing.
- **Sources:** [Centipede-RTK — Home](https://www.centipede-rtk.org/) · [Centipede-RTK map](https://map.centipede-rtk.org/) · [millipede-caster (GitHub)](https://github.com/pbeyssac/millipede-caster)

## 4. Onocoy

- **What:** Newer (crypto-incentivized) crowdsourced global RTK correction network/marketplace — 7,800+ stations across 168 countries per their own figures, growing quickly, with an explicit push for miner (base-station) growth in Southeast/East Asia.
- **Cost:** Has a commercial/marketplace model for rover data access alongside the "mine rewards" base-station side; check current pricing at signup (this is not guaranteed-free the way RTK2go is).
- **Signup:** Self-service account at onocoy.com.
- **Caveats:** Density in HK specifically is unverified as of this research pass (their coverage is expanding, not yet audited here) — check their live coverage map before relying on it. Newer/less battle-tested as production RTK infrastructure than RTK2go or a national geodetic network.
- **Sources:** [onocoy.com](https://onocoy.com/) · [onocoy documentation — connect a station](https://docs.onocoy.com/documentation/quick-start-guides/mine-rewards/3.-connect-your-station-to-onocoy)

## 5. IGS real-time service / other national networks (lower priority for HK)

- The International GNSS Service real-time RTCM/NTRIP broadcasters are free and globally recognized, but station density near Hong Kong is sparse (this is scientific-grade global infrastructure, not a dense local RTK network) — useful mainly as a "does a totally standards-conformant, well-run caster work with our client" cross-check, not for actually achieving RTK Fixed on a bench in HK.
- Neighboring national/regional networks (e.g. Taiwan's e-GNSS, mainland China's CORS networks, Japan's GEONET) may have public or semi-public NTRIP access but typically require in-country registration/agreements not evaluated here — worth a follow-up look only if SatRef + RTK2go prove insufficient for field testing.

## What this means for A10 / S5

- Keep `ntrip_caster_sim.py` as the primary dev/CI-loop tool (deterministic, offline, scriptable drop/reconnect injection) — don't gate day-to-day A10 development on any of the above being reachable/registered/online.
- Before M3 (RTK milestone) sign-off, do at least one real-caster pass: RTK2go first (fastest to reach, validates protocol plumbing against a real, uncontrolled server), then SatRef once the application is approved (validates the HK-relevant, closest-to-production case). Note in the M3 field-test report which mountpoints were used and their observed fix behavior, since that's real-world data this simulator cannot manufacture.
