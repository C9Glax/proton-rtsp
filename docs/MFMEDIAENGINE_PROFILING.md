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

## D3D11 video buffer locking (`WINE_MFPLAT_PROFILE`, `WINE_MF_LEGACY_2D_LOCK`)

Profiling a busy VRChat world with a 1080p30 RTSP stream showed the media
engine itself doing almost no work on the game thread (frame transfers took a
few microseconds, the frame queue stayed full, nothing waited on a lock), while
the game ran at 24-28fps and the video-related `wine_threadpool` worker used
about a third of a core. The cost is in how decoded frames are written into
D3D11-backed buffers.

When an app supplies a DXGI device manager (AVPro Video does), decoder output
buffers are D3D11 textures. Locking one for CPU access used to copy the texture
to a staging texture and map it for reading, which waits for the GPU to finish
the copy behind whatever the game has queued, once per video frame, and
unlocking copied everything back and flushed the context. Now output buffers are
locked write-only and input buffers read-only, which removes the readback.

| Variable | Effect |
| --- | --- |
| `WINE_MF_LEGACY_2D_LOCK=1` | Always lock read-write like before, to compare behaviour with the same build |
| `WINE_MFPLAT_PROFILE=1` | One `mfplat-prof` line per second: |

```
mfplat-prof: window 1000100us | dxgi map: n=30 readback=0 avg=40us max=300us | unmap: n=30 avg=120us max=900us
```

`readback` counts maps that copied the texture to the CPU. With the old locking
it equals `n`; with the new locking it should be 0 for decoder output. A large
`map max`/`avg` together with a high `readback` count means the thread was waiting for the GPU.

To compare, play the same stream in the same world and note your frame rate
with and without `WINE_MF_LEGACY_2D_LOCK=1`.
