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
ravenna reload              # live config reload (see below)
ravenna debug on|off        # verbose per-tick logging
```

### ravenna reload

Performs a non-destructive live reload without restarting the module or
dropping any currently-running FS sessions:

1. Calls FreeSWITCH's internal `switch_xml_reload()` to refresh the XML
   source — works identically whether config comes from disk,
   `mod_xml_curl`, `mod_xml_ldap`, or any other bound XML provider.
2. Re-parses `ravenna.conf` in a temporary memory pool.
3. Diffs the new config against the running state:

   | Situation | Action |
   |---|---|
   | Stream in current, absent in new | Sockets closed; stream removed |
   | Stream in both, config unchanged | No-op |
   | Stream in both, config changed | Sockets closed and reopened with new params |
   | Stream in new, absent in current | New sockets opened; stream added |

4. Replaces the endpoint table (endpoints are cheap config references,
   not sockets — always replaced wholesale).
5. Wakes the RX reactor to rebuild its `poll()` set.

Active sessions on a removed or changed stream are **not** forcibly hung
up. Their cursors stop receiving new samples, read silence, and the
session eventually times out via the normal media-timeout path.

```
freeswitch> ravenna reload
XML reload: OK
+OK reload complete: 1 added, 0 removed, 1 restarted, 2 unchanged
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

* **IPv4 multicast only.**  AES67-2018 and the RAVENNA transport spec define
  the session layer exclusively over IPv4 multicast (239.x.x.x, RFC 2365
  administratively-scoped space).  No shipping AES67 or RAVENNA device uses
  IPv6 multicast; IPv6 support is a deliberate non-goal.  `iface-primary` and
  `iface-secondary` accept an IPv4 address on all platforms or a Linux
  interface name on Linux — an IPv6 address will be silently ignored and
  `INADDR_ANY` used instead.
* No RTSP — multicast joins only.
* L32 is non-standard for RTP and intended for routing trunks between
  cooperating instances.
* Linux-side interface lookup accepts an interface name (`ens19`, `eth0`) or
  a dotted IPv4 address. Windows accepts either an adapter friendly-name
  (e.g. `"Ethernet 2"`), an adapter description, or a dotted IPv4 address;
  lookup is case-insensitive via `GetAdaptersAddresses()`.
* DTMF, hold-music injection, and per-call audio level reporting are
  not yet ported from `mod_aes67`.

---

## SMPTE ST 2022-7 Seamless Protection Switching

SMPTE ST 2022-7 defines "Seamless Protection Switching of RTP Datagrams":
an identical RTP stream is transmitted over **two independent network
paths**. The receiver joins both; if a packet is lost on one path the
duplicate from the other path carries it transparently.

### How it works in mod_ravenna

**Transmit side**

Both paths share the same RTP session (same SSRC, sequence counter, and
timestamp). `emit_one_packet()` in `ravenna_tx.c` sends the fully encoded
buffer to `tx_dest` (path A) and then immediately to `tx2_dest` (path B)
with a single additional `sendto()`. There is no re-encoding and no extra
ring read — the same byte buffer is reused.

**Receive side**

`rx_sock` (path A) and `rx2_sock` (path B) are both added to the
`poll()` / `WSAPoll()` reactor in `build_pollset()`. Both map to the same
`ravenna_stream_t`. `handle_packet()` deduplicates by sequence number
using a circular window (`dedup_win[RAVENNA_ST2022_WIN]`):

```
slot = seq % 2048
if dedup_win[slot] == seq → duplicate, drop silently → increment rx_dedup_dropped
else                       → dedup_win[slot] = seq, process normally
```

Window size 2048 comfortably exceeds the maximum expected inter-path
latency difference at any practical network depth.

**Failover**

Fully automatic. If path A goes silent, all new `seq` values arrive on
path B and are passed through unconditionally (their dedup slot has never
been set). No configuration change, no reconnect, no state machine.

### Configuration

Add to a `<stream>` block:

```xml
<param name="st2022-7"        value="true"/>
<!-- Path A — primary NIC
     Linux: interface name (ens19, eth0 …)   Windows: dotted IPv4 (192.168.10.5) -->
<param name="iface-primary"   value="ens19"/>
<param name="rx-address"      value="239.69.10.1"/>
<param name="rx-port"         value="5004"/>
<param name="tx-address"      value="239.69.10.1"/>
<param name="tx-port"         value="5004"/>
<!-- Path B — secondary NIC (physically separate switch/plane)
     Linux: interface name (ens20, eth1 …)   Windows: dotted IPv4 (192.168.11.5) -->
<param name="iface-secondary" value="ens20"/>
<param name="rx2-address"     value="239.69.11.1"/>
<param name="rx2-port"        value="5004"/>
<param name="tx2-address"     value="239.69.11.1"/>
<param name="tx2-port"        value="5004"/>
```

A fully worked example is in `conf/ravenna.conf.xml` (stream `st2022`).

If `st2022-7` is `false` (or omitted) the `rx2`/`tx2`/`iface-secondary` params are ignored
and the stream operates as a single-path AES67 flow.

If `iface-secondary` is omitted, path B sockets fall back to `rtp-iface` (same NIC as path A).
This still deduplicates packets correctly but provides **no hardware redundancy** — both paths
share the same NIC failure domain. Always set `iface-secondary` to a different physical adapter
for real 2022-7 protection.

### Diagnostics

```
ravenna streams
```

The `rx2-pkts` column shows packets received on path B.
The `dedup-drop` column shows duplicates discarded (healthy 2022-7
operation produces `dedup-drop ≈ rx2-pkts` when both paths are alive).

### Network topology requirement

For 2022-7 to provide real protection the two paths **must** be on
physically independent infrastructure (separate switches, NICs, and — for
broadcast facilities — separate SDN planes). Routing both multicast groups
over the same switch defeats the redundancy.

The standard recommends placing path A and path B on separate IP subnets
with different multicast group addresses (as in the example above) rather
than the same group on different interfaces.

