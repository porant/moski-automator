# Face points (YuNet): 3 anchors per face

This document describes the face-point model: detecting faces and drawing the animated distortion
at three anchors on **every** detected face. It is opt-in and costs nothing when disabled.

## What it does

When **Face tracking** is on and a point magnitude or a meme morph is non-zero, the filter:

1. Grabs a **small, downscaled** frame of its own input (default 180 px high) a few times per
   second (default 10 FPS) - not once per rendered frame.
2. Hands that frame to a **background worker thread** that runs OpenCV's YuNet
   (`cv::FaceDetectorYN`) on the CPU.
3. For every detected face, places **three anchor points directly on the YuNet landmarks**, so they
   sit on the actual facial features and follow the head's size, tilt (roll) and turn
   (yaw/pitch) with the features themselves:
   * **Point 1 - eyes**: the midpoint of the two eyes,
   * **Point 2 - nose**: the nose tip,
   * **Point 3 - mouth**: the midpoint of the two mouth corners.
   Because the anchors are the landmarks themselves, a tilted, turned or nodding head moves the
   points exactly with it. If landmarks are missing, fractions of the detection box are used as a
   fallback.
4. Draws one circular distortion zone per (face x enabled point). Where a point lands is
   `anchor + offset`; the point's own `radius` and `magnitude` are shared by all faces.

Anchors are normalised to **0..100** (percent of frame width/height) - exactly the units the
shader uses - and faces are sorted **left to right**.

The **animation is configured per point**, in advance, before any face is seen. `Add` / `Set` /
`Start` animate a point's magnitude (or offset/radius); the accumulation of `Add` is shared by
**all faces** at that point. So one `Add` to `point2_magnitude` pulses the nose of every
detected face.

## Shader

The original single-pass distortion is preserved (same `distortZone` math and aspect correction)
but the zones are enumerated dynamically: the CPU fills a `zone_data[MAX_ZONES]` uniform array with
up to `3 * maxFaces` entries `(center_x, center_y, radius, magnitude)` and a `zone_count`, and the
pixel shader loops over them. See `data/face-points.effect`. The classic fixed 6-zone file
`data/6-zone.effect` is kept verbatim (and verified by `tests/verify_shader.py`).

## Independent effects: face blur and debug

Blur and debug are **separate options that do not depend on the morph** (the point-magnitude
distortion) and can be combined with it. They are plain booleans, exposed in the properties, the
dock and `FaceTrack`.

| Setting | Default | Behaviour |
|---|---|---|
| `effect_blur` | off | Blur the **whole detected face box** (feathered at the edge), independent of the distortion |
| `face_blur_px` | 24 | Blur radius in pixels (2..128); larger = stronger/softer cover |
| `effect_debug` | off | Overlay coloured markers: point anchors (1 red, 2 green, 3 blue) and the raw YuNet landmarks (right eye yellow, left eye cyan, nose tip magenta, right mouth orange, left mouth violet) |
| `face_scale` | on | Scale each point's `radius` and `offset` by the detected **face height**, so a distant face gets proportionally smaller points (adapts to size) |

* **Face blur** now uses the face **box** (cx, cy, w, h), not the small point circles, so the face is
  actually covered. It is a cheap fixed 25-tap blur restricted to the box.
* **Debug** shows the points even when every magnitude is 0.
* Both keep the filter active even with the morph at rest (they never take the zero-work fast path
  when on and a face is present).
* Shader order: morph (distortion, driven by magnitudes) -> face blur -> debug overlay. All three
  read the same smoothed per-face tracks.

## Why this is cheap

* Detection never runs on the OBS graphics/render thread; it runs on its own worker thread.
* The render thread only copies a small RGBA buffer into a pending slot and reads the last result
  back. If the worker has not caught up, the newer frame simply replaces the pending one (counted
  as `dropped`), so there is no queue growth.
* Capture is rate-limited (`face_fps`) and only happens while the effect is actually visible:
  while every magnitude sits at rest (0) the filter still takes the zero-work fast path and does
  **no** capture, **no** detection and **no** extra GPU pass.
* The downscaled GPU readback is the only extra GPU cost.

## Requirements / build

Face tracking is compiled in only when OpenCV is present:

* Build with `-DOPA_FACE_TRACKING=ON` (default) and a CMake-visible OpenCV
  (`-DOpenCV_DIR=.../opencv/build`). `find_package(OpenCV COMPONENTS core imgproc objdetect dnn)`
  enables it; if OpenCV is not found the plugin still builds and the tracker stays inert.
* The YuNet model `data/face_detection_yunet_2023mar.onnx` ships with the plugin and is copied to
  `data/obs-plugins/obs-parameter-animator/`.
* On Windows the matching `opencv_world<ver>.dll` is installed next to the plugin DLL. The debug
  OpenCV import lib uses a different ABI (`cv::debug_build_guard`), so the release import lib is
  always linked.

The debug plugin build (if any) must use a release OpenCV; mixing the debug OpenCV import lib into
a release plugin leaves `cv::cvtColor`-style symbols unresolved.

## Settings

Global (filter properties, **Face tracking** group):

| Key | Default | Meaning |
|---|---|---|
| `face_tracking` | off | Master switch |
| `face_fps` | 10 | Detection rate (1..60); capture is skipped between ticks |
| `face_max` | 4 | Maximum number of faces kept (capped at 8) |
| `face_score` | 0.7 | YuNet confidence threshold (0.1..0.95) |
| `face_height` | 180 | Height the frame is downscaled to before detection (96..480) |
| `face_smooth_ms` | 120 | Temporal smoothing of the point positions (0 = raw/jittery, higher = smoother) |

Top-level (not part of the Face tracking group): `effect_blur` + `face_blur_px` and `effect_debug`
are independent options - see **Independent effects** above.

Per point (three groups: **Eyes**, **Nose**, **Mouth**). Names are `point1_*`,
`point2_*`, `point3_*` with the parameter suffix:

| Suffix | Default | Meaning |
|---|---|---|
| `enable` | on | Draw this point on every face |
| `offset_x` | 0 | Nudge the anchor horizontally (percent of frame) |
| `offset_y` | 0 | Nudge the anchor vertically (percent of frame) |
| `radius` | 10 | Zone radius (percent of the frame height) |
| `magnitude` | 0 | Distortion strength (animated) |

Every parameter also carries the usual animation keys: `_target`, `_duration_ms`, `_easing`,
`_auto_return`, `_return_value`, `_hold_ms`, `_return_ms`, `_return_easing`.

Parameter IDs are `(point - 1) * 5 + offset` with offsets `enable=0, offset_x=1, offset_y=2,
radius=3, magnitude=4`.

## Rendering semantics

* Each render frame builds a zone list from the smoothed per-face tracks: for every tracked face
  (up to `face_max`) and every enabled point with a non-zero magnitude and a positive radius, one
  zone at `trackAnchor + offset`.

## Temporal smoothing (interpolation)

Detection runs at `face_fps` (default 10), so using the raw anchors directly makes the points jump
and jitter between detections. Instead, each face keeps a persistent **track** and the anchors are
eased toward the latest detection on **every render frame**:

* `track += (detection - track) * alpha`, `alpha = 1 - exp(-dt / tau)`, `tau = face_smooth_ms/1000`.
* Detections are matched to tracks by nearest centre (within 25% of the frame); a face that is not
  matched opens a new track (snapped at first sight) and a track not seen this tick expires.
* `face_smooth_ms = 0` disables smoothing (raw positions); larger values are smoother but lag more.
* Because easing happens at the render rate, the motion is smooth even though detection is slow.
* The point's `radius` and `magnitude` are the animated values; because they are shared by all faces,
  the animation (including the accumulating `Add`) applies to **every** face at that point.
* If no face is detected (or tracking is off), there are no zones and the GPU pass is skipped, but
  the animation keeps running so a pulse started now is visible as soon as a face appears.
* While every magnitude sits at rest (0) the filter takes its zero-work fast path: no capture, no
  detection, no uniform upload and no GPU pass.

## WebSocket

`CallVendorRequest` with `vendorName: "obs-parameter-animator"`, `requestType: "FaceTrack"`.
All request fields are optional, so an empty request just reports the current faces:

```json
{ "vendorName": "obs-parameter-animator", "requestType": "FaceTrack",
  "requestData": { "enabled": true, "fps": 12, "maxFaces": 4, "score": 0.75 } }
```

`Get` responses also include `faces`, where each face carries `centerX`, `centerY`, `width`,
`height`, `score` and an `anchors` array of `{name, x, y}` (eyes / nose / mouth) in
percent units, plus `faceTracking`, `faceAvailable`, `faceSequence` and `faceDetectMs`.

## Limitations

* Detection is CPU-only and per-source (the filter's own input). There is no GPU path and no
  persistent face identity: faces are ordered left to right, so the zones follow "every face at the
  Nth anchor", not a named person.
* The point `radius` is a fixed percent of the frame, so a distant face gets the same absolute zone
  size as a near one (no per-face scale). Nudge `offset`/`radius` per point to taste.
* Readback uses libobs `gs_texrender` + `gs_stagesurface` to grab the input a few times per second;
  an internal flag keeps the nested pass-through bounded (recursion guard).
* **Display Capture / Game Capture inputs are not supported for tracking.** Capturing means
  re-rendering the source from inside the filter's own render callback; on duplicator-based captures
  that nested re-render hangs the GPU (`DXGI_ERROR_DEVICE_HUNG`) and OBS then loops on
  "Rebuilding all assets" (which also pegs the CPU). Tracking is skipped for those inputs with a
  warning in the OBS log - use a camera (Video Capture Device) source instead.
* Quality depends on the detection resolution and the model; the defaults are tuned for low cost,
  not for crowded scenes.
