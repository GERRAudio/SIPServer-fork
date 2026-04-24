# mod_ivcore — Debugging & Logging Reference

This document covers every runtime knob mod_ivcore exposes for
diagnosing call setup, HDLC/DPI signalling, and TX pacing problems.

---

## 1. Log channels at a glance

mod_ivcore writes to the standard FreeSWITCH log
(`SWITCH_CHANNEL_LOG`) at three levels:

| Level    | Always on?           | Used for                                                                 |
|----------|----------------------|--------------------------------------------------------------------------|
| `INFO`   | Yes                  | Lifecycle (load, codec init, ACCEPT, hangup) and once-per-second summaries |
| `WARNING`| Yes                  | Recoverable failures (NEW retransmit, recvfrom errors, REJECT)           |
| `ERROR`  | Yes                  | Unrecoverable per-call failures (codec init, session request)            |
| `DEBUG`  | Only if **both** `ivc debug on` and FS log level ≥ DEBUG | Per-frame HDLC/DPI/transport traces and per-frame TX pacer numbers |

The `DEBUG` traces are gated through the `IVC_LOG_DEBUG()` macro, which
checks `ivcore_globals.debug` *before* the log call — so when the toggle
is off there is no formatting/printf cost at all.

---

## 2. Toggling debug at runtime — `ivc debug`

The `ivc` API command exposes a single boolean that controls **every**
per-frame trace in the module.

```text
ivc debug on     # enable per-frame HDLC / DPI / transport / TX pacer traces
ivc debug off    # disable (default)
ivc debug        # show current state
```

You can also set it from the config file:

```xml
<configuration name="ivcore.conf" description="...">
  <settings>
    <param name="debug" value="true"/>   <!-- starts enabled at module load -->
  </settings>
  <cards>
    ...
  </cards>
</configuration>
```

Toggling from the FS CLI / fs_cli takes effect immediately for the next
frame — no module reload needed.

When debug is enabled you will see:

- **Receive loop** — every protocol frame and HDLC payload
  (`recv proto frame type=… sub=… srcCallNo=… …`)
- **HDLC layer** — every SABME / U-UA / I-frame / S-frame RR
- **DPI layer** — every CPUApp message (0xF0 PanelTypeReply,
  0xF1 ConnectReply / DialOut, 0x8B KeyStatusReply,
  0x93 KeyStatusUpdate, 0xF4 DisconnectReply, etc.)
- **TX pacer** — per-frame inter-arrival + sleep numbers (see §4)

---

## 3. Listing channels — `ivc list`

```text
ivc list           # plain-text channel + card table
ivc list xml       # same data as an XML fragment
```

Useful for confirming the channel state machine and the negotiated codec
without enabling the per-frame traces.

---

## 4. TX pacer diagnostics

The TX pacer (configured per card via `<param name="timer-name">`)
gates `channel_write_frame()` to the negotiated ptime so sources like
`tone_stream` and `playback` do not blast frames back-to-back.

### 4.1 Configuration

```xml
<card name="default">
  ...
  <param name="timer-name" value="soft"/>   <!-- soft | none | <module> -->
</card>
```

| Value     | Behaviour                                                                |
|-----------|--------------------------------------------------------------------------|
| `soft`    | Software wall-clock timer. **Default.** Recommended for IVC matrix calls |
| `none`    | No pacing — `channel_write_frame` sends as fast as FreeSWITCH delivers   |
| anything else | Passed straight to `switch_core_timer_init()` — any installed FS timer module (`system`, `clock`, …) |

### 4.2 What you'll see when `ivc debug on`

Two log lines are emitted from `channel_write_frame`:

#### Per-frame DEBUG line

Visible only when `ivc debug on` **and** the FS log level is at DEBUG:

```text
mod_ivcore: TX pacer frame#42 arr=437 us sleep=19563 us datalen=160 flags=0x0
```

| Field      | Meaning                                                                                  |
|------------|------------------------------------------------------------------------------------------|
| `frame#`   | Frames since the last 1-second summary (resets each second)                              |
| `arr`      | µs since the previous `channel_write_frame()` call **before** the pacer ran              |
| `sleep`    | µs that `switch_core_timer_next()` actually blocked on this call                         |
| `datalen`  | Payload bytes (160 for G.722 @ 20 ms, 160 for G.711 @ 20 ms)                             |
| `flags`    | FreeSWITCH frame flags (`0x0` = normal media, `0x40` = SFF_CNG → already short-circuited and not seen here) |

#### Once-per-second INFO summary

Always visible at the default INFO level when `ivc debug on`:

```text
mod_ivcore: TX pacer 1s stats  frames=50  arr min/avg/max=312/19987/20104 us  sleep min/avg/max=0/13/19688 us  timer=soft
```

| Field      | Meaning                                                            |
|------------|--------------------------------------------------------------------|
| `frames`   | Number of writes in this 1-second window. Should be `≈ 1000 / ptime` (50 at 20 ms). Anything significantly higher means FS bursted us. |
| `arr min/avg/max` | Inter-arrival times **before** pacing.                       |
| `sleep min/avg/max` | Time the pacer blocked.                                    |
| `timer`    | Active timer name (`soft`, `none`, or any installed timer module)  |

### 4.3 How to read the numbers

The two numbers always sum (per frame) to roughly the wire ptime
(20 000 µs by default):

```text
arr + sleep ≈ ptime
```

Because the previous frame's `sleep` consumed wall-clock time before
`arr` was measured, a fast FS source will show `arr ≈ ptime` *after the
first second of pacing* — the pacer has effectively absorbed the burst
and the wall-clock spacing now matches the wire cadence.

| Scenario                                | `arr` distribution           | `sleep` distribution         | What it means                                              |
|-----------------------------------------|------------------------------|------------------------------|------------------------------------------------------------|
| **Pacer working, fast source** (tone_stream, playback) | `min` low (a few hundred µs), `avg/max` close to ptime | `min` low after warm-up, `max` close to ptime | FS is bursting; pacer is absorbing it ✅                    |
| **FS already paced upstream** (bridged RTP) | `min/avg/max` all ≈ ptime    | `min/avg/max` all near 0     | Nothing to do; pacer is a no-op ✅                          |
| **Pacer disabled** (`timer-name=none`)  | `min/avg` very low (µs–ms)   | always 0                     | "Hurried" symptom expected ❌ — re-enable pacing            |
| **Frame count anomalies**               | `frames` ≫ 50 (at 20 ms)     | n/a                          | FS is delivering more frames than the negotiated ptime — check codec_ms / read leg |
| **Pacer can't keep up**                 | `max` ≫ ptime                | `max` ≪ expected             | System under load; consider `timer-name=system` or check CPU |

### 4.4 Cost when disabled

The diagnostic block has near-zero cost when `ivc debug off`:

- one global read (`ivcore_globals.debug`)
- one branch
- the per-frame `tx_dbg_*` fields are not touched

So leaving the toggle off in production has no measurable impact on the
TX hot path.

---

## 5. Quick recipes

### Confirm the pacer is doing something

```text
fs_cli> ivc debug on
fs_cli> originate ivcore/main/intercom1 &playback(tone_stream://%(2000,4000,440))
```

Watch the once-per-second `TX pacer 1s stats` line.  With `timer-name=soft`
you should see `arr min` very low and `sleep max` close to 20 000 µs.
With `timer-name=none` you'll see `sleep` always 0 and `arr` distribution
much wider.

### Compare paced vs. unpaced

1. Set `<param name="timer-name" value="none"/>`, `ivc rescan`, originate a
   tone playback. Note the `arr min/avg/max` line.
2. Set `<param name="timer-name" value="soft"/>`, `ivc rescan`, repeat.
3. The second run's `arr avg` should be much closer to 20 000 µs and the
   audio should sound clean on the matrix.

### Investigate hangups during call setup

```text
fs_cli> ivc debug on
fs_cli> sofia loglevel all 9    # if you also want SIP traces
```

You'll see every NEW retransmit, every ACK/ACCEPT/REJECT, and every HDLC
SABME from the IVC card. If `dpi_send_cb` is involved (0xF1 / 0x93 / 0xF4
sequencing) it will be obvious from the DPI traces.

### Forget what's loaded

```text
fs_cli> ivc list
fs_cli> ivc list xml      # for tooling
```
