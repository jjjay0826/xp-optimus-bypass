# Measurements

All numbers are from one laptop. "Per frame" values are averages over 300-frame windows unless noted.
Tools: PresentMon 2, Intel VTune 2024.3, NVIDIA Nsight Systems 2026.5.1, Vulkan SDK API dump, and a series of
test layers that timed every relevant Vulkan call.

## System

| | |
|---|---|
| Laptop | ASUS TUF Gaming F15 FX506LHB (no MUX; built-in 1920×1080 screen wired to the Intel iGPU) |
| CPU | Intel Core i5-10300H (Comet Lake-H, 4C/8T) — no Windows CASO support |
| GPUs | NVIDIA GeForce GTX 1650 4 GB (driver 595.97) + Intel UHD Graphics (driver 101.1999, Vulkan 1.3.212) |
| X-Plane | 12.4.3-r2 (Vulkan), Steam |
| X-Plane swapchain (API dump) | 2 images, MAILBOX, B8G8R8A8_SRGB, 1920×1080, fullscreen-exclusive DISALLOWED |

## 1. Graphics settings barely mattered

| Change | Result |
|---|---|
| FSR native → performance (25% of the pixels) | 23.3 → 25.7 FPS; PresentMon GPU busy 25.2 → 23.1 ms |
| Shadows / object density one step lower | ~no change |
| Clouds off | +1–2 FPS |

## 2. Baseline frame (no layer)

PresentMon, 264 frames:

| | ms per frame |
|---|---|
| Between presents | 37.6 |
| CPU busy (includes waits inside driver calls) | 37.1 |
| GPU busy | 22.7 |
| GPU utilization | 55.6 % |
| Present mode | Hardware: Independent Flip, SyncInterval 0 |

VTune Threading, X-Plane main thread, 30 s:

| | seconds | share |
|---|---|---|
| Running (effective) | 9.2 | 31 % |
| Spinning | 5.2 | 18 % |
| Blocked | 14.9 | 51 % |

Most of the blocking was one function in `nvoglv64.dll` (the NVIDIA Vulkan driver): 13.6 s blocked + 3.4 s spinning,
765 waits in 30 s — once per frame, ~22 ms each.

Nsight Systems, 20 s while flying (~18.5 FPS under the profiler):

| Vulkan call | calls | avg | median | max |
|---|---|---|---|---|
| `vkQueuePresentKHR` | 372 | **18.67 ms** | 19.57 ms | 27.29 ms |
| `vkQueueSubmit` | 6,775 | 0.07 ms | | |
| `vkAcquireNextImageKHR` | 372 | 0.03 ms | | |

From the Nsight GPU workload trace:

- GPU executing X-Plane work: 43.2 % of the time, 23.4 ms per frame
- During each `vkQueuePresentKHR` call the GPU was running X-Plane work for 52 % of the call
- The call returned on average **8.5 ms after** the GPU had finished X-Plane's last work for that frame

## 3. Test layers: moving the wait around did not help — until NVIDIA's present path was bypassed

| # | What the test layer did | Aircraft | FPS | Where the time went (per frame) |
|---|---|---|---|---|
| 1 | Nothing (timing only) | Baron 58 | 19.2–20.5 | X-Plane main thread blocked in `vkQueuePresentKHR` 19.3–23.9 ms; in `vkAcquireNextImageKHR` 1.2–2.0 ms |
| 2 | Swapchain 2 → 3 images | Baron 58 | ~20 | present 20.4–21.8 ms |
| 3 | Present mode MAILBOX → IMMEDIATE | Baron 58 | ~19.4 | present 21.4–25.6 ms |
| 4 | Present on a worker thread (same queue) | Baron 58 | 19.4–20.9 | worker blocked in present 20.3–21.5 ms; main thread now blocked **9.8–14.2 ms in its next `vkQueueSubmit`** (queue busy) |
| 5 | Present on a worker thread on a separate queue | Baron 58 | 19.9–21.2 | worker present 22.2–24.3 ms; main thread now blocked **21.2–24.0 ms in `vkAcquireNextImageKHR`** (inside the driver) |
| 6 | Virtual swapchain: X-Plane never touches the real swapchain; worker does NVIDIA acquire + copy + present | Baron 58 | 20.4–21.4 | worker: NVIDIA acquire **23.1–24.1 ms** + NVIDIA present **23.2–24.1 ms** ≈ one frame; X-Plane waited 18.5–22.4 ms for a free virtual image |
| 7 | **This layer:** GeForce → pinned host memory → Intel Vulkan device → Intel swapchain | Baron 58 | **40.1–45.3** | GeForce frame + copy wait 16–23.5 ms (mostly the GPU finishing the frame); **Intel acquire 0.00–0.01 ms; Intel present 0.01–0.05 ms**; X-Plane waited 0 ms |
| ref | Variant 4 in the main menu (light GPU load) | — | 111–154 | NVIDIA present 0.5–1.1 ms |

Notes:

- All rows were flown in the default Baron 58 under similar (not identical) conditions. Results for the heavier A330
  are in section 4.
- In row 7 X-Plane's main thread polls `vkGetFenceStatus` thousands of times per frame, i.e. it is ahead and waiting
  for the GPU — the GTX 1650 is now the limit.
- A variant that deferred *all* queue submissions to a worker crashed X-Plane (`VK_ERROR_DEVICE_LOST`), most likely
  because X-Plane's OpenGL plugin bridge synchronises with Vulkan through shared semaphores. This layer only moves
  presentation.

## 4. Results with the release build (0.9.0-beta)

| Aircraft | Without (NVIDIA Optimus path) | With the layer |
|---|---|---|
| Baron 58, similar conditions to rows 1–6 (not the identical spot) | ~20 FPS | 40.1–45.3 FPS (mostly 42–43; v7 test build) |
| A330 | under 19 FPS (previous day) | 31.8–45.8 FPS, mostly 36–41 (release build) |

Control test (A330): with the layer set to `mode=off` and the Steam overlay disabled as well, FPS did **not** improve
(Overwolf, OBS, RTSS and MSI Afterburner were already not running in all tests). Third-party overlays are therefore not
the cause; the gain comes from bypassing NVIDIA's present path. All layer-on tests above ran with the Steam overlay
enabled, so the two coexist.

Hitch check (A330, release build, ~2 min of flight after loading, 300-frame windows): 4 of 16 windows had frames over
50 ms (1–3 frames each, worst 74.6 ms). In every one of them the GeForce frame + copy maximum stayed below 30 ms and the
Intel acquire/present maximum below 0.3 ms, so those hitches came from X-Plane's own CPU-side frame time, not from the
GPU or the presentation path. (At ~20 FPS every frame already took ~50 ms, so such hitches were not noticeable.)

## 4b. Fixed benchmark (0.9.1-beta)

Procedure: [BENCHMARK.md](../BENCHMARK.md) — default Baron 58 at KSEA 16L, clear weather, 12:00, cockpit view, no input.
Values from the layer's own log (300-frame windows).

| | `mode=off` | `mode=auto` (bypass active) |
|---|---|---|
| FPS per window | 21.1, 22.2, 21.3 | 33.7, 36.3, 35.4, 34.4 |
| Median | **21.3** | **34.9** |
| Frames over 50 ms per window | 63, 37, 54 | 4, 0, 1, 0 |
| NVIDIA present blocked X-Plane (avg) | 20.4–21.4 ms | — (X-Plane waited 0.00 ms; Intel present 0.01–0.02 ms) |
| GeForce frame + copy after present (avg) | — | 11.3–11.6 ms |

GTX 1650 utilization in Task Manager was ~52%. With the bypass on, the same spot with FSR "performance" instead of
"ultra" gave a median of 36.8 FPS (12 windows), and object density at minimum 39.4 FPS (10 windows); in both runs FPS
drifted up to 41–47 in the last windows. So at KSEA neither the GPU nor object density is the main limit; it is most
likely X-Plane's main-thread CPU work and its own waiting for the GPU.

## 5. Interpretation

- Row 6 shows the throughput of NVIDIA's Optimus present path under load: acquire + present ≈ 47 ms per frame on a
  worker thread that does nothing else, i.e. ~21 frames per second at most, regardless of what X-Plane does.
- The same path takes ~0.5 ms in the main menu, so it slows down as GPU load rises.
- Doing the cross-GPU transfer ourselves (two copies, like Optimus) costs ~1–2 ms on the GeForce and ~0.02 ms to
  present on the Intel side, so the slowness is in how NVIDIA's driver implements the path on this system, not in
  PCIe, the iGPU or Windows composition.
- What exactly NVIDIA's driver waits on inside that path was not observed directly (no driver symbols).

The full investigation log (Traditional Chinese) is in `investigation_zh-TW.md`.
