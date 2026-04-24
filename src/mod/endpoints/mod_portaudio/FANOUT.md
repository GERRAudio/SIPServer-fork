# mod_portaudio — Fan-out Capture (Design Notes)

This document captures the design for adding SPMC fan-out to
`mod_portaudio` so that a single soundcard capture device can be
read by **multiple simultaneous FS sessions** without stealing
samples from each other.

Work is deferred until `mod_ravenna` (which uses an identical ring
primitive) has been validated in production.

---

## Background — current architecture

```
PortAudio callback
      │
      ▼
PABLIO read ring  ──────►  PA_MASTER session  channel_read_frame()
                                │
                           PA_SLAVE sessions  (write/playback only;
                                               they do NOT read from PA)
```

The `PA_MASTER` / `PA_SLAVE` split exists for **full-duplex bridging**:
the master owns the soundcard and relays audio to/from the slave.
It is not a fan-out — only one session drains the PABLIO read ring,
and two sessions trying to drain it simultaneously would steal samples.

---

## Target architecture

```
PortAudio callback
      │
      ▼
PABLIO read ring
      │
  demux pump  (one drainer, runs on the master session or a dedicated thread)
      │
      ▼
  capture_ring  (ravenna_ring_t / identical SPMC ring)
      ├──► cursor A  ──►  FS session 1  channel_read_frame()
      ├──► cursor B  ──►  FS session 2  channel_read_frame()
      └──► cursor C  ──►  FS session 3  channel_read_frame()
```

The playback (write) path is unchanged — multiple sessions can already
mix audio into the PA output via the existing master/slave mechanism.

---

## Changes required

### 1. Shared ring primitive

Copy (or include) `ravenna_ring.[ch]` from `mod_ravenna`.  The ring is
self-contained, has no Ravenna-specific dependencies, and is already
proven at 48 kHz / 1 ms / 125 µs ptimes.

Alternatively, factor it into a shared FreeSWITCH utility header
(`switch_spmc_ring.h`) and use it from both modules — but that is a
larger repo change.

### 2. Per-stream `capture_ring`

Add to `audio_stream_t`:

```c
ravenna_ring_t   *capture_ring;   /* non-NULL when capture is active */
switch_mutex_t   *capture_mtx;    /* protects attach / detach        */
```

Create the ring when the first session calls `validate_main_audio_stream()`.

### 3. Demux pump

One path drains PABLIO and writes into `capture_ring`.  Two options:

**Option A — inside the existing master's `channel_on_routing` loop**  
The master session already has a tight loop.  Add:

```c
if (stream->capture_ring) {
    int16_t tmp[SPF];
    ReadAudioStream(stream->stream, tmp, SPF, &stream->timeInfo);
    ravenna_ring_write(stream->capture_ring, tmp, SPF);
}
```

This costs zero extra threads.

**Option B — dedicated capture thread per `audio_stream_t`**  
Cleaner separation; necessary if the master session can be absent (i.e.
capture-only use).  Start the thread in `create_audio_stream()`, stop
it in `destroy_actual_stream()`.

*Recommendation*: start with Option A; promote to Option B if a
capture-only mode is needed later.

### 4. `channel_on_init` / `channel_on_hangup`

```c
/* on_init — attach cursor */
if (stream->capture_ring) {
    tech_pvt->capture_cursor = ravenna_ring_attach(stream->capture_ring);
}

/* on_hangup — detach cursor */
if (tech_pvt->capture_cursor) {
    ravenna_ring_detach(&tech_pvt->capture_cursor);
}
```

### 5. `channel_read_frame`

Replace the direct PABLIO read with a cursor read:

```c
if (tech_pvt->capture_cursor) {
    int got = ravenna_cursor_read(tech_pvt->capture_cursor,
                                  (int16_t *)tech_pvt->read_frame.data,
                                  samples_per_frame);
    if (got < 0) {
        /* overrun — slow consumer, hang up this leg only */
        switch_channel_hangup(channel, SWITCH_CAUSE_MEDIA_TIMEOUT);
        return SWITCH_STATUS_FALSE;
    }
    /* zero-pad if ring is briefly empty (startup transient) */
    if (got < samples_per_frame)
        memset((int16_t *)tech_pvt->read_frame.data + got, 0,
               (samples_per_frame - got) * sizeof(int16_t));
} else {
    /* legacy single-session path, unchanged */
    ReadAudioStream(...);
}
```

### 6. `PA_MASTER` / `PA_SLAVE` flag

Once the demux pump is in place the distinction is only needed for the
**playback/mix** path.  Capture sessions no longer need to be slaved
to a master — any number of them can attach and read independently.

---

## Ring sizing

| Sample rate | ptime  | Samples/pkt | Ring depth (×1024 pkts) | Latency at depth |
|-------------|--------|-------------|-------------------------|-----------------|
| 48 000 Hz   | 20 ms  | 960         | 983 040 samples ≈ 3.8 MB| ~20 s           |
| 48 000 Hz   | 10 ms  | 480         | 491 520 samples ≈ 1.9 MB| ~10 s           |
| 8 000 Hz    | 20 ms  | 160         | 163 840 samples ≈ 320 KB| ~20 s           |

Start with 256 packets for a comfortable local-machine margin without
wasting RAM.  Tune down once validated.

---

## Overrun policy

Identical to `mod_ravenna`: a cursor that falls more than `capacity`
samples behind is marked `overrun`; the next `channel_read_frame` call
on that session detects it, logs a warning, and hangs up **only that
session**.  All other cursors are unaffected.

---

## Effort estimate

| Task | Lines |
|------|-------|
| Copy / include ring primitive | ~0 (copy ravenna_ring.[ch]) |
| Add `capture_ring` to `audio_stream_t` | ~20 |
| Demux pump (Option A) | ~30 |
| `channel_on_init` / `channel_on_hangup` cursor attach/detach | ~20 |
| `channel_read_frame` cursor path | ~30 |
| Config / doc | ~20 |
| **Total** | **~120 lines** |

No changes to the PortAudio library, PABLIO, or the playback path.

---

## Prerequisites

- `mod_ravenna` validated in production (ring correctness, overrun
  handling, PTP pacing).
- Decision on whether to share the ring via a common header or keep
  it module-local.
