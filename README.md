# netify-plugin-stats

A Netify Agent processor plugin for OpenWrt that counts **per-application** and
**per-application-category** traffic (bytes, packets, flows) into an on-router
binary ring-buffer time-series store, plus a `netify-stats-query` CLI and an
rpcd object that serve the data as JSON for a LuCI front-end.

Builds **only** against the public Netify Agent plugin API (`nd-plugin.hpp`,
`nd-flow.hpp`, `nd-apps.hpp`, `nd-category.hpp`) and reads its config from UCI
via **libuci** directly. No dependency on or shared code with any other Netify
plugin.

## Components

| Path | Purpose |
|---|---|
| `/usr/lib/netify-plugin-stats.so` | the processor plugin (loaded by netifyd) |
| `/usr/bin/netify-stats-query` | CLI: read the store → JSON contract |
| `/etc/config/netify-stats` | UCI config (store path, sample interval, top-N, tiers) |
| `/etc/netifyd/plugins.d/90-netify-plugin-stats.conf` | netifyd loader stanza |
| `/usr/libexec/rpcd/luci.netify-stats` | rpcd exec wrapper (ubus object `luci.netify-stats`) |
| `/usr/share/rpcd/acl.d/luci-netify-stats.json` | read-only ACL for the LuCI session |

## Architecture

- The plugin (`nspPlugin : ndPluginProcessor`) folds `DPI_UPDATE`/`DPI_COMPLETE`
  flow events into per-app and per-category accumulators, tracking **deltas**
  against each flow's cumulative `total_lower/upper_*` counters (keyed by the
  flow's `digest_lower` xxh64 id; dropped on `FLOW_EXPIRE`).
- A worker thread samples every `sample_interval` (default 10s): ranks apps by
  bytes, keeps `top_n_apps` (rest → `__other__`), keeps all categories, and
  appends one slot per series to tier 0 of the store. Wider tiers consolidate
  automatically (RRD-style).
- The store is fixed-size memory-mapped ring-buffer files, one per
  `(dimension, tier)`: `apps.t{0,1,2}.rrb`, `cats.t{0,1,2}.rrb` under
  `store_path` (default `/tmp/netify-stats`, tmpfs). Size is known up front and
  independent of uptime.
- `netify-stats-query` shares the exact store struct headers, so there is no
  reader/writer format drift.

## Building

The package is built on a Linux host with the Netify Agent dev libs and an
OpenWrt SDK. Two helpers (run from the repo root):

- `scripts/remote-build.sh` — sync to the build host and run the **native unit
  tests** (`autogen + configure + make check`). The pure logic (store,
  accumulator, tier consolidation, config parser, query builder) is covered by
  doctest unit tests + an integration round-trip; the plugin/CLI are gated
  behind `--enable-package` (they need libuci/agent libs) and are not part of
  the native test build.
- `scripts/sdk-build.sh` — stage the package into the OpenWrt SDK and
  cross-build the `.apk` (`make package/netify-plugin-stats/{clean,compile}`).
  Env: `NSP_BUILD_HOST`, `NSP_SDK_DIR`, `NSP_BUILD_TIMEOUT`.

The OpenWrt package (`deploy/openwrt/Makefile`) configures with
`--enable-package --disable-tests`, depends on `+netifyd +rpcd +libuci
+libstdcpp`, and links `-luci`.

## Installing (apk-based OpenWrt)

```sh
# netifyd 5.2.7 (matching what the plugin was built against) + the plugin:
apk add --allow-untrusted /tmp/netifyd-5.2.7-r1.apk
apk add --allow-untrusted /tmp/netify-plugin-stats-0.1.0-r1.apk
```

Configure a capture interface and (re)start netifyd:

```sh
uci set netifyd.@netifyd[0].autoconfig=0
uci add_list netifyd.@netifyd[0].internal_if=br-lan
uci commit netifyd
/etc/init.d/netifyd restart
```

## Verifying

```sh
# store fills under traffic
ls -la /tmp/netify-stats/        # apps.t0/t1/t2.rrb, cats.t0/t1/t2.rrb

# query CLI
netify-stats-query --dimension=apps --metric=rx_bytes --range=1h
netify-stats-query --dimension=cats --metric=flows   --range=1d

# via rpcd/ubus (what the LuCI app uses)
ubus call luci.netify-stats query '{"dimension":"apps","metric":"rx_bytes","range":"1h"}'
```

Response shape:
```json
{ "step": 10, "start": 1780280390,
  "series": [ { "name": "netflix", "values": [12345, null, ...] }, ... ] }
```
`metric` ∈ `rx_bytes|tx_bytes|pkts|flows`; `range` ∈ `1h|1d|30d` (tier select).

## Configuration (`/etc/config/netify-stats`)

```
config netify_stats 'global'
    option store_path '/tmp/netify-stats'   # tmpfs by default; point elsewhere to persist
    option sample_interval '10'             # seconds
    option top_n_apps '50'                  # rest rolled into __other__
config tier { option step '10';  option count '360'  }   # 1h @ 10s
config tier { option step '60';  option count '1440' }   # 1d @ 60s
config tier { option step '300'; option count '8640' }   # 30d @ 5m
```
The plugin re-reads config on the agent `RELOAD` event.

## API facts confirmed against the live agent

- `ndFlow` cumulative counters `total_lower/upper_bytes` (u64) and
  `total_lower/upper_packets` (u32); per-flow id = `digest_lower`.
- `libnetifyd` does **not** export `ndUci` — config is read via the libuci C
  API directly (package links `-luci`).
- netifyd loads a processor plugin only when its loader stanza lives in
  `/etc/netifyd/plugins.d` **and** the section name starts with `proc-`.
- The rpcd exec wrapper filename = ubus object name (`luci.netify-stats`).

## Known follow-ups

- **tx/rx orientation:** mapped tx=lower (local egress), rx=upper (ingress).
  Confirm against controlled directional traffic; the store is
  direction-agnostic, so a correction is a one-line swap in
  `DispatchProcessorEvent`.
- **Category names:** v0.1 uses numeric `cat-<id>` labels. Real category tags
  require capturing the `ndInstanceStatus` categories table at the
  `UPDATE_INIT` event and calling `ndCategories::GetTag(Type::APP, id)`.
- **netifyd staying resident:** verified the plugin loads and samples whenever
  netifyd runs (manual `netifyd -d -I br-lan` stays up and fills the store).
  Keeping netifyd resident under procd/init is a netifyd service-config matter
  (interface selection), independent of this plugin.
