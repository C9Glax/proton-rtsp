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

### GPU time of transfers and destination texture

`WINE_MFME_PROFILE=1` also reports how long the GPU takes to execute the
commands of each transfer, measured with D3D11 timestamp queries, in the same
line as `gpu(transfer): n=… avg=…us max=…us skipped=…`. `skipped` counts
transfers that were not measured because the previous results were not ready.
This is the GPU cost of Wine's own copy (or draw) into the app's texture, so it
shows whether any of the extra GPU time per frame comes from the frame hand-off.

A second kind of line is printed once, and again whenever the textures change:

```
mfme-prof 0x…: fast copy transfer | src 1920x1080 fmt=87 usage=0 bind=0x28 … | dst 0x… 1920x1080 fmt=87 usage=0 bind=0x8 cpu=0 misc=0x0 mips=1 array=1 samples=1
```

It shows the format, usage, bind, CPU access and misc flags, mip levels and
size of the texture the app gave us (`dst`). Unusual flags such as a shared or
dynamic texture, many mip levels, or a very large size can make a texture more
expensive to draw with later.

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

## Findings so far

Profiling a busy VRChat world (VR, 75Hz) with a 1080p30 RTSP stream in an AVPro
player, where the game dropped to 24-28fps:

- The Wine media engine is not the bottleneck. Frame transfers took a few
  microseconds, frames were never re-uploaded, nothing waited on the engine or
  device locks, and the frame queue stayed full with the video arriving at 30
  frames per second.
- CPU capacity is not the limit (about 75% of the CPU idle, no thread near
  saturation), and hardware decoding is not the cause: the frame rate was the
  same with and without `--enable-hw-video-decoding`, and the video decode engine
  read 0% in the software decoding case.
- The GPU was not saturated either (graphics engine about 70% busy). The game's
  frame time is quantised by the 75Hz compositor: 37.5fps needs every frame
  within about 26.7ms, otherwise it drops to 25fps. A world that is already near
  that limit can be pushed over it by a small extra cost, which would make the
  drop look sudden when a video starts and recover when it stops.
- Two attempted fixes did not change the frame rate and were reverted: locking
  D3D11 video buffers write-only/read-only instead of read-write (this removes a
  GPU readback per frame), and rotating staging textures with
  `D3D11_MAP_FLAG_DO_NOT_WAIT`. Mapping the buffers still took 6-12ms with the
  second change even though no texture was ever busy, so that wait is the game
  being busy, not the cause of the low frame rate.

Open question: how much GPU/CPU time per frame playing video adds in the same
world. It needs a baseline without video (frame rate and GPU load) to compare
against, and tests with a lower video resolution and frame rate.
