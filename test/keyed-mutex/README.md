# Keyed mutex test

Checks the DXVK keyed-mutex fence synchronisation (`DXVK_KEYED_MUTEX_BLOCKING`)
without needing VRChat. Two D3D11 devices share a texture through a keyed mutex
(like AVPro Video does); the producer queues GPU load and writes a frame number,
the consumer reads it back.

```
./run.sh                          # newest build in ~/.steam/steam/compatibilitytools.d
./run.sh /path/to/proton-dir      # a specific build (the dir containing "proton")
./run.sh /path/to/proton-dir 600 40   # more frames, more GPU load
```

It runs the test twice, with the fix and with `DXVK_KEYED_MUTEX_BLOCKING=1`.

What to look for:

- **`producer ReleaseSync`**: should be much smaller with the fix than with `blocking`
  when the GPU is loaded (raise `load` until the blocking run shows several ms).
- **`stale frames` / `wrong pixel values`** must be 0 in both runs. A non-zero count in the
  fixed run means the consumer used the texture before the producer's GPU work was done.
- Both runs must end with `PASS` and not hang or print `AcquireSync failed`.

`kmtest.exe` is built with `x86_64-w64-mingw32-g++ -O2 -static -o kmtest.exe kmtest.cpp -ld3d11 -ldxgi -luuid`.

## When cross-device sharing does not work

If the DXVK log says `Failed to open shared NT handle` and the sanity check fails, the second device
cannot see the first device's data in your Wine build. The test then falls back to a single device that
uses the keyed mutex on its own texture. That still measures the CPU stalls and checks the mutex
hand-over and the data, but cannot say anything about visibility across devices.
