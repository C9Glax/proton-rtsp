# Profiling video playback FPS (mfmediaengine)

Diagnostics for the case where a game's frame rate drops to the video's frame
rate (or a multiple of it) while a video or stream is playing, e.g. VRChat with
Unity VideoPlayer / AVPro Video.

## Usage

Set `WINE_MFME_PROFILE=1` in the game's launch options, start playback, and
read the lines from the Wine log. In Steam:

```
WINE_MFME_PROFILE=1 PROTON_LOG=1 %command%
```

The log is written to `~/steam-<appid>.log` (VRChat: `~/steam-438100.log`);
filter it with `grep mfme-prof`. The lines are printed regardless of the
`WINEDEBUG` channel settings. Nothing is logged and nothing changes when the
variable is unset.

Each media engine prints one line per second:

```
mfme-prof 0x... : window 1000123us | tick: n=75 with_frame=30 gap_max=14000us cs_wait_max=0us
 | transfer: n=75 new=30 repeat=45 fast=0 avg=9100us max=15000us gap_max=14000us cs_wait_max=0us dev_wait_max=20us
 | upload: n=75 avg=8800us max=14800us | sink: frames_in=30 queued=5
```

| Field | Meaning |
| --- | --- |
| `tick n / with_frame` | `OnVideoStreamTick` calls, and how many reported a new frame |
| `tick gap_max` | Longest time between two `OnVideoStreamTick` calls |
| `transfer n` | `TransferVideoFrame` calls |
| `new / repeat` | Calls that transferred a new frame vs. the same frame as the previous call |
| `fast` | Calls served by the GPU-to-GPU copy (D3D11-backed samples) |
| `transfer avg / max` | Time spent inside `TransferVideoFrame` (this runs on the game's thread) |
| `cs_wait_max` | Longest wait for the media engine lock |
| `dev_wait_max` | Longest wait for the D3D device lock |
| `upload n / avg / max` | The software upload path (`UpdateSubresource`) |
| `frames_in` | Frames the video pipeline delivered to the sink in the window |
| `queued` | Frames waiting in the sink queue (see `WINE_MFME_VIDEO_QUEUE_SIZE`) |

## Reading the result

- `transfer avg` is a large part of the frame budget (about 13ms at 75Hz) and
  `repeat` is large: frames are being re-uploaded every game frame. The upload
  should be skipped when the frame hasn't changed.
- `transfer avg` is large but `repeat` is near zero: the cost is the upload
  itself (frame size / color conversion), not repetition.
- `cs_wait_max` or `dev_wait_max` are large: the game's threads are blocking on
  each other or on the device lock.
- `frames_in` is below the video's frame rate or `queued` stays at 0: the
  pipeline is not keeping up, which is independent of the render thread.
- Everything is small while FPS is still capped: the stall is outside
  mfmediaengine.

## Source Reader players (`WINE_MFRW_PROFILE`)

Players that decode through `IMFSourceReader` (for example Unity's built-in
video player, which loads `mfreadwrite.dll` on the main game thread) never use
the media engine, so `WINE_MFME_PROFILE` stays silent for them. To profile
these, set `WINE_MFRW_PROFILE=1` as well. Both variables can be set together:

```
WINE_MFME_PROFILE=1 WINE_MFRW_PROFILE=1 PROTON_LOG=1 %command%
```

To see which path a player uses, check which DLLs loaded:

```
grep -ioE 'loaded [^ ]*(mfmediaengine|mfreadwrite|quartz)\.dll' ~/steam-438100.log | sort | uniq -c
```

One line per second is printed (`grep mfrw-prof ~/steam-438100.log`):

```
mfrw-prof: window 1000100us | ReadSample: n=75 async=0 sync=75 sync_avg=31000us sync_max=40000us
 cs_wait_max=0us gap_max=33000us thread_changes=0 last_tid=01c0 | OnReadSample: n=0 gap_max=0us
```

| Field | Meaning |
| --- | --- |
| `n / async / sync` | `ReadSample` calls, split by whether the app uses a callback |
| `sync_avg / sync_max` | How long synchronous calls block the calling thread |
| `cs_wait_max` | Longest wait for the reader's lock |
| `gap_max` | Longest time between two calls |
| `thread_changes / last_tid` | Whether more than one thread calls, and the last caller's thread id |
| `OnReadSample` | Samples delivered through the async callback and the longest gap |

If `sync_avg` is close to the video's frame interval (about 33ms for 30fps) and
`last_tid` is the game's main thread (the thread id that also loaded `d3d11.dll`
in the log), the game thread is waiting for the next video frame, which would
lock its frame rate to the video's.

## D3D11 video buffer locking (`WINE_MFPLAT_PROFILE` and related switches)

Profiling a busy VRChat world with a 1080p30 RTSP stream showed the media
engine itself doing almost no work on the game thread (frame transfers took a
few microseconds, the frame queue stayed full, nothing waited on a lock), while
the game ran at 24-28fps with the GPU only about two thirds busy. Hardware
decoding was not the cause (the same with and without
`--enable-hw-video-decoding`).

The video work that does cost time is writing decoded frames into D3D11-backed
buffers, which an app gets when it supplies a DXGI device manager (AVPro Video
does). Locking one for CPU access maps a staging texture, and each unlock queues
a GPU copy from it. Mapping the same staging texture again before the GPU has
executed that copy waits for it, behind everything the game has queued.

Two changes were made, and each has a switch to go back to the old behaviour so
the same build can be compared:

1. Output buffers are locked write-only and input buffers read-only, which
   removes a GPU-to-CPU readback per frame. This alone did not change the frame
   rate in testing. `WINE_MF_LEGACY_2D_LOCK=1` restores the old read-write locks.
2. Write-only locks rotate between up to three staging textures and map with
   `D3D11_MAP_FLAG_DO_NOT_WAIT`, creating another one instead of waiting when all
   are busy. `WINE_MF_DXGI_BLOCKING_MAP=1` goes back to mapping a single staging
   texture and waiting for it.

| Variable | Effect |
| --- | --- |
| `WINE_MF_LEGACY_2D_LOCK=1` | Always lock read-write (the original behaviour; implies the blocking map) |
| `WINE_MF_DXGI_BLOCKING_MAP=1` | Keep write-only locks, but map one staging texture and wait for the GPU |
| `WINE_MFPLAT_PROFILE=1` | One `mfplat-prof` line per second |

```
mfplat-prof: window 1000100us | dxgi map: n=30 readback=0 avg=40us max=300us | write staging: busy=2 created=1 blocked=0 | unmap: n=30 avg=5us max=20us
```

| Field | Meaning |
| --- | --- |
| `map n / avg / max` | Locks of D3D11 buffers and how long mapping took. Long maps mean the thread waited for the GPU |
| `readback` | Maps that first copied the texture to the CPU (should be 0 for decoder output) |
| `busy` | Write locks that found every existing staging texture still in use |
| `created` | Extra staging textures created because of that (at most two per buffer) |
| `blocked` | Write locks that had to wait for the GPU anyway (should stay near 0) |

To compare, play the same stream in the same world and note your frame rate
with the default, with `WINE_MF_DXGI_BLOCKING_MAP=1` and with
`WINE_MF_LEGACY_2D_LOCK=1`.
