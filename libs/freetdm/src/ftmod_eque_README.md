# ftmod_eque — Bare-Bearer Always-Up Signaling Stub

**ftmod_eque** is a FreeTDM signaling module for T1/E1 spans that carry only
raw PCM bearer channels — no D-channel (ISDN), no CAS robbed-bit signaling,
no call-setup handshake of any kind.  Every B-channel on the span is treated
as a permanently connected circuit that is immediately offered to FreeSWITCH
as an inbound call, and is automatically re-offered after each call ends.

Typical use-cases:

- Dedicated leased-line T1/E1 audio circuits (e.g. broadcast feeds, studio
  links, intercom trunks).
- Any TDM span where the far end provides continuous audio without a
  signaling layer.
- Lab/test spans where you want audio looped through FreeSWITCH without
  configuring PRI or CAS.

---

## Name

"Eque" derives from the Latin *aequus* ("level / constant / equal") —
reflecting a circuit that is always up and never needs negotiation.

---

## How It Works

```
FreeSWITCH                 ftmod_eque              T1/E1 hardware
───────────                ─────────────           ──────────────
                           span start
                           │  send SIGSTATUS UP ──▶ (logged)
                           │
                           │  SIGEVENT_START ──────▶ channel_on_routing()
channel_on_routing()  ◀────┘                         (FreeSWITCH dials app)
  answer()
  │  INDICATE_ANSWER ─────▶ eque_indicate()
  │                         │  send SIGEVENT_UP
  │                    ◀────┘
  [audio flows]

  hangup()
  │  channel falls to DOWN state
  │
  │  (reconnect-delay-ms elapses)
  │
  │  SIGEVENT_START ──────▶ channel_on_routing() (re-offered)
```

The module runs one worker thread per span.  That thread:

1. Sends `FTDM_SIGEVENT_SIGSTATUS_CHANGED → UP` for all channels on start.
2. Sends `FTDM_SIGEVENT_START` for every B-channel immediately.
3. Polls every 100 ms and re-sends `FTDM_SIGEVENT_START` for any channel
   that has returned to the `DOWN` state (post-hangup), subject to the
   `reconnect-delay-ms` guard time.

---

## Files

| File | Purpose |
|---|---|
| `libs/freetdm/src/ftmod_eque.c` | Module source |
| `libs/freetdm/src/ftmod_eque.conf` | Reserved config file (currently unused at module-load time) |
| `libs/freetdm/msvc/freetdm.2008.vcxproj` | MSVC build — `ftmod_eque.c` added to ClCompile group |

---

## Build

The module is compiled as part of the `freetdm` library project.  No
separate build step is required — `ftmod_eque.c` is included in
`freetdm.2008.vcxproj` alongside the other FreeTDM sources.

For Linux/Autotools builds, add the file to the appropriate `Makefile.am`
source list in `libs/freetdm/src/`.

---

## Configuration

### 1. freetdm.conf — define the span channels

Create or edit `conf/<profile>/conf/freetdm.conf`.  List only B-channels;
no D-channel or CAS entry is required.

```ini
[span wanpipe t1-bearer-1]
trunk_type = T1
b-channel  = 1-23
```

For a full 24-channel T1 (no D-channel):

```ini
[span wanpipe t1-bearer-1]
trunk_type = T1
b-channel  = 1-24
```

For E1:

```ini
[span wanpipe e1-bearer-1]
trunk_type = E1
b-channel  = 1-15,17-31
```

### 2. freetdm.conf.xml — attach the eque signaling module

Edit `conf/<profile>/conf/autoload_configs/freetdm.conf.xml`.  Add an
`<eque_spans>` block (or use a generic span block understood by your version
of mod_freetdm).  The minimum required parameter is `signaling = eque`.

```xml
<configuration name="freetdm.conf" description="Freetdm Configuration">
  <settings>
    <param name="debug" value="0"/>
  </settings>

  <eque_spans>

    <!--
      t1-bearer-1: 23 B-channels, no D-channel, no CAS.
      Each channel is presented to FreeSWITCH as a permanent inbound circuit.
      After a call ends the channel is re-offered after 2000 ms.
    -->
    <span name="t1-bearer-1">
      <param name="signaling"          value="eque"/>
      <param name="reconnect-delay-ms" value="2000"/>
      <param name="dialplan"           value="XML"/>
      <param name="context"            value="default"/>
    </span>

  </eque_spans>

</configuration>
```

### 3. dialplan — handle the inbound bearer channel

Because there is no called number, the `destination_number` channel variable
will be empty (or `0` depending on your board driver).  Route on the span
name or channel number instead:

```xml
<extension name="eque-bearer-t1-bearer-1">
  <condition field="${ftdm_span_name}" expression="^t1-bearer-1$">
    <action application="answer"/>
    <action application="bridge" data="sofia/internal/someextension@pbx.local"/>
  </condition>
</extension>
```

To bridge the raw audio through to another system without any B-leg setup:

```xml
<action application="answer"/>
<action application="echo"/>
```

---

## Parameters

| Parameter | Default | Description |
|---|---|---|
| `signaling` | *(required)* | Must be `eque` to select this module. |
| `reconnect-delay-ms` | `2000` | Milliseconds to wait after a channel returns to `DOWN` before it is re-offered as a new inbound call.  Increase this if the application consuming the bearer needs a tail-end guard time. |
| `dialplan` | — | FreeSWITCH dialplan type, typically `XML`. |
| `context` | — | FreeSWITCH dialplan context for the inbound calls. |

---

## Signaling Events Emitted

| Event | When |
|---|---|
| `FTDM_SIGEVENT_SIGSTATUS_CHANGED → UP` | Once per channel at span start. |
| `FTDM_SIGEVENT_START` | At span start and after each reconnect-delay-ms interval for idle channels. |
| `FTDM_SIGEVENT_UP` | In response to `INDICATE_ANSWER` from FreeSWITCH, and immediately on outgoing calls. |
| `FTDM_SIGEVENT_SIGSTATUS_CHANGED → DOWN` | When the span worker thread exits (span stop). |

---

## Outgoing Calls

Outgoing calls (FreeSWITCH bridges *to* a bearer channel) are supported.
The module answers immediately with `SIGEVENT_UP` — no network signaling is
sent because there is none.  The channel must not already be in use.

Dial string format (standard mod_freetdm syntax):

```
freetdm/t1-bearer-1/1    — channel 1 on span t1-bearer-1
freetdm/t1-bearer-1/a    — first available channel
```

---

## Thread Safety and State Machine

- One detached worker thread per span, started by `ftdm_span_start()`.
- Channels use the FreeTDM pending-channel queue
  (`FTDM_SPAN_USE_CHAN_QUEUE`) for state-change delivery.
- `FTDM_SPAN_USE_SKIP_STATES` is set, allowing direct transitions to
  `UP` without passing through `PROGRESS` / `PROGRESS_MEDIA`.
- The worker thread polls at a fixed 100 ms granularity; `reconnect-delay-ms`
  is rounded up to the nearest 100 ms boundary.

---

## Limitations

- **No called-number signaling.** `destination_number` will be empty or
  board-driver-dependent.  Route calls using span or channel metadata.
- **No on-hook / off-hook events.** The circuit is considered permanently
  off-hook.  If the far end goes silent or disappears, FreeTDM alarm events
  (`FTDM_SIGEVENT_ALARM_TRAP`) from the board driver will surface normally,
  but no signaling-layer disconnect is synthesized.
- **Single call per channel.** As with all FreeTDM B-channels, only one call
  at a time is supported per physical channel.
- **No transfer or hold.** `FTDM_SPAN_USE_TRANSFER` is not set.

---

## Relationship to Existing Modules

| Module | When to use instead |
|---|---|
| `sangoma_isdn` / `libpri` | ISDN PRI with D-channel (the default for T1/E1 in most deployments). |
| `analog` | FXS/FXO analog lines with loop-start, ground-start, or wink signaling. |
| `analog_em` | E&M tie-trunk signaling. |
| `freetdm_r2` | MFC-R2 signaling (Latin America / parts of Asia). |
| **`eque`** | Bearer-only span — no signaling, raw audio, always-up. |
