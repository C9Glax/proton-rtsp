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
