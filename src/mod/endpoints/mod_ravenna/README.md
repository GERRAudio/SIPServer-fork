# mod_ravenna

A lean, **AES67-compatible** Ravenna multicast endpoint module for FreeSWITCH.
Designed as a drop-in alternative to `mod_aes67` for sites that need to
listen to and produce many AES67 streams without the per-stream / per-packet
GStreamer thread overhead.

## Why a new module?

`mod_aes67` builds a full GStreamer pipeline (and thread + main-loop) per
stream. With dozens of multicast sources the CPU cost of per-element task
threads, the jitter buffer element, and per-buffer allocation becomes
the dominant load on the box.

`mod_ravenna` replaces the entire per-stream machinery with:

| Concern | mod_aes67 | mod_ravenna |
|---|---|---|
| Threads per stream | 1 GMainLoop + GStreamer task pool | **0** (shared reactor + shared pacer) |
| Total RX threads | N | **1** |
| Total TX threads | N | **1** |
| Jitter buffering | `rtpjitterbuffer` element | inline ring (lock-free, fixed) |
| Per-packet allocation | yes (`GstBuffer`) | **none** in steady state |
| Sample fan-out | re-encoded per consumer | one ring + N read cursors |
| Time source | `GstPtpClock` | `mod_ptp_timer` (hard requirement) |
| Codec support | L16, L24 | L16, L24, **L32** |
| Sub-ms ptime | yes | yes (125 µs, 250 µs, 333 µs, 1 ms, …) |

## Hard requirement: mod_ptp_timer

`mod_ravenna` will fail to start its TX pacer if `mod_ptp_timer` is not
already loaded. Add this to `conf/autoload_configs/modules.conf.xml`,
**in this order**:

```xml
<load module="mod_ptp_timer"/>
<load module="mod_ravenna"/>
```

## Configuration

See `conf/ravenna.conf.xml`. The schema deliberately mirrors `aes67.conf.xml`
so existing configurations only need an element rename:

```xml
<configuration name="ravenna.conf">
  <settings>
    <param name="sample-rate"      value="48000"/>
    <param name="ptime-ms"         value="1.0"/>
    <param name="rx-codec"         value="L24"/>
    <param name="tx-codec"         value="L24"/>
    <param name="rtp-payload-type" value="98"/>
    <param name="rtp-iface"        value="ens19"/>
  </settings>

  <streams>
    <stream name="udp1">
      <param name="rx-address" value="239.69.0.1"/>
      <param name="rx-port"    value="5004"/>
      <param name="tx-address" value="239.69.1.1"/>
      <param name="tx-port"    value="5004"/>
      <param name="channels"   value="4"/>
      <param name="ptime-ms"   value="1.0"/>
    </stream>
  </streams>

  <endpoints>
    <endpoint name="udp1-rx1">
      <param name="instream"        value="udp1:0"/>
      <param name="multiple-listen" value="true"/>
    </endpoint>
    <endpoint name="udp1-tx1">
      <param name="outstream"       value="udp1:0"/>
    </endpoint>
  </endpoints>
</configuration>
```

### Audio fork (no extra packets, no extra threads)

Set `multiple-listen="true"` on an RX endpoint and have several call legs
dial the same `endpoint/<name>`. Each session gets its own **fan-out
cursor** on the per-channel sample ring; the network only sees one
multicast subscription and the box only does one decode. A consumer that
falls behind by more than the ring capacity is flagged `overrun` and the
corresponding FS leg is hung up (per design choice).

### Codec selection

| Wire codec | Format | AES67 interop | Notes |
|---|---|---|---|
| L16 | 16-bit BE, interleaved | yes | RFC 3551 |
| L24 | 24-bit BE, interleaved | yes (recommended) | RFC 3190 |
| L32 | 32-bit BE, interleaved | no | extra precision for routing trunks |

Internally everything is processed as native S16; L24/L32 are converted
on the fly at the codec boundary.

### Sub-ms ptime

`ptime-ms="0.125"` (= 125 µs / 8000 packets per second) is supported.
The TX pacer ticks at 1 ms via `mod_ptp_timer` and emits up to 16
packets per stream per tick to catch up with the configured cadence.

## Dial strings

Outbound:

```
originate ravenna/endpoint/udp1-tx1 &echo
```

The `endpoint/<name>` form is identical to `mod_aes67`'s, so existing
dialplans typically only need the `aes67/` → `ravenna/` rename.

## API

```
ravenna status              # one-line module summary
ravenna streams             # per-stream stats (rx/tx packet counts, drops)
ravenna endpoints           # endpoints and active session counts
ravenna debug on|off        # verbose per-tick logging
```

## Building on Linux

```sh
cd src/mod/endpoints/mod_ravenna
./bootstrap.sh   # if regenerating autotools
make
sudo make install
```

Add `mod_ravenna` to `modules.conf` so the top-level build picks it up.

## Building on Windows (Visual Studio)

Same procedure as `mod_ptp_timer`:

1. Copy a sibling endpoint module's `.2017.vcxproj` (e.g. `mod_aes67`'s)
   into this folder as `mod_ravenna.2017.vcxproj`.
2. Replace the `<ProjectGuid>`, `<RootNamespace>`, `<TargetName>` and the
   `ItemGroup` with the six `.c` files listed in `Makefile.am` and the
   `.h` files alongside.
3. Drop any GStreamer-specific include/lib paths and `<AdditionalLibraryDirectories>`
   entries — `mod_ravenna` only needs `Ws2_32.lib` (already linked by the
   FreeSWITCH `module.props`).
4. Add the project to `Freeswitch.2017.sln`, set Build for `x64`,
   add a project dependency on `FreeSwitchCore`.
5. Build. Output is `<repo>\x64\Release\mod\mod_ravenna.dll` and the
   sample config is deployed to `conf\autoload_configs\ravenna.conf.xml`.

## Tuning

* `RAVENNA_RING_PACKETS_DEFAULT` (header) — ring depth in *packets*.
  Default 1024; with 1 ms / 48 kHz that is ≈ 1 second.
* `RAV_RX_BATCH` (`ravenna_rx.c`) — max packets drained per ready socket
  before re-polling.
* `RAV_TX_TIMER_INTERVAL_MS` (`ravenna_tx.c`) — base TX tick period.
  Lower than 1 ms requires sub-ms support in `mod_ptp_timer`, which is
  not the default; the current pacer compensates by emitting multiple
  packets per tick.
* RX/TX socket buffers default to 4 MiB; raise via OS limits if you see
  drops on bursty multicast networks.

## Limits / TODO

* No SAP discovery — streams are configured statically (matches AES67
  module by design choice).
* No RTSP — multicast joins only.
* L32 is non-standard for RTP and intended for routing trunks between
  cooperating instances.
* Linux-side interface lookup accepts a dotted IP or an `ifname`. On
  Windows, only a dotted IP is accepted today; an adapter-name lookup
  via `GetAdaptersAddresses` is a clean follow-up.
* DTMF, hold-music injection, and per-call audio level reporting are
  not yet ported from `mod_aes67`.
