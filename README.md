# Pebble Raise to Wake

A responsive background-worker backlight controller for Pebble watches, based on [rajid/pebble_backlight](https://github.com/rajid/pebble_backlight).

This version retains the background worker and manually declared `light_enable_interaction()` / `light_enable(bool)` calls from rajid/pebble_backlight, which were confirmed working on a real Pebble Time. It builds for every Pebble: aplite (Pebble, Steel), basalt (Time), chalk (Time Round), diorite (Pebble 2), emery (Pebble Time 2), flint (Pebble 2 Duo) and gabbro (Round 2). It replaces the original delayed orientation check with a responsive raise detector. The foreground app is a single settings menu for enabling it, sensitivity, light duration, the light sensor, charger lighting, a daily schedule and debug logging.

## Why the original felt slow

The old worker stored `watch_level_start = time(0L)` and waited for the integer number of seconds to advance before lighting the screen. This adds up to about one second even after the viewing posture is recognized. At 10 Hz, collecting N samples per callback can additionally delay delivery by approximately N × 100 ms. The previous rate change also occurred before subscribing; this version subscribes first, then checks the rate-setting result.

The new worker requests **25 Hz, two samples per callback** (revision 6; previously one). Pairing samples halves worker wake-ups for battery at a cost of at most 40 ms delivery delay. All gesture timing uses `AccelData.timestamp` in milliseconds and consecutive sample counts. The `time()` calls remaining in the foreground app are for scheduled start/stop alarms only.

## Detector and latency

`worker_src/c/raise_to_wake_detector.c` is the portable detector; `worker_src/c/backlight_worker.c` owns the Pebble subscriptions and backlight/timer calls. The existing `wscript` already includes every C source under `worker_src`, so it builds both modules into the worker.

The detector progresses through `WAIT_LOWERED → ARMED → ROTATING → CONFIRMING_VIEW → VIEWING`:

1. Confirm a stable posture outside the wider viewing boundary before arming. Starting the worker while already viewing does not trigger.
2. Require a meaningful X/Y/Z gravity-vector change toward the viewing region, rather than arbitrary movement.
3. Require three consecutive viewing samples with plausible gravity magnitude and limited sample-to-sample motion. A short gravity filter rejects jitter without a long settling delay.
4. Activate the light immediately on confirmation. Stay in `VIEWING` even if the light's duration expires.
5. Require three samples clearly outside the wider exit boundary before reporting lowering. A 500 ms cooldown also prevents rapid repeat raises.

The viewing region includes tilted and near-vertical poses observed in real Pebble Time logs, while rejecting shallow typing tilts. Entering requires X −550…550, Y −1150…−530, Z −1100…450 milli-g. Exiting requires leaving the wider X −650…650, Y −1250…−350, Z −1200…550 region. The previous Y entry limit of −300 accepted typing poses around Y −344…−410; revision 4 moved it to −550 (33°). Real looks in later logs ended at −525…−680, so −550 split them and made raises hit or miss; revision 7 uses −530 (32°). A simulation of thousands of varied raises and wrist fidgets put the best trade there: raises from an arm at your side went from 86% to 98% detected, while keyboard fidgets stayed quiet, and moving the line to −525 or beyond made fidgets wake the screen many times an hour. Values between the entry and exit boundaries do not flicker the light. Settling for ~320 ms at least 75 milli-g outside the viewing region (`RTW_REARM_VIEW_MARGIN_MG`), such as back at a keyboard, also ends a look and rearms; previously only leaving the wide exit region did, so a look from a desk could leave the next raise ignored. Z still rejects more fully screen-down poses, such as Y −700 / Z +714.

Before a successful raise, the detector refreshes its resting reference after eight consecutive settled samples anywhere outside the viewing region (including typing poses between the two boundaries) with no more than 40 milli-g total drift from one anchor. This prevents a reference from an earlier arm position exaggerating a tiny later typing tilt. Arming from `WAIT_LOWERED` still requires a genuinely lowered pose. A continuous rotation breaks that stationary window, so the reference remains fixed during a raise. This reference maintenance adds no waiting once a valid raise reaches viewing orientation.

Direction is measured by decreasing squared X/Y/Z distance to the accepted viewing region. The previous fixed ideal-direction dot product incorrectly rejected raises from a flat resting wrist toward a vertical screen: Y becomes more negative while Z becomes less negative, cancelling its direction score. The detector still requires a 420 milli-g vector change at Normal sensitivity (about a 24° wrist turn) and consecutive stable viewing samples. A reference resting just outside the viewing region cannot shrink its distance by the full progress amount, so reaching the region from there also counts as progress (revision 7); before, a wrist paused near the viewing angle blocked every raise until it was fully lowered.

At 25 Hz, three valid samples span 80 ms from first to third; sampling phase, filtering and settling add some delay. Host traces of 320–1200 ms raises measured **120–200 ms from raw viewing-region entry to detection**, before real-device delivery overhead. Log-derived flat-to-tilted/vertical traces also confirm within 200 ms of reaching their final posture. Entering the wider region while still moving can take longer to confirm. These are synthetic results, not hardware measurements: the supplied watch log contains XYZ snapshots only once per second, so the tests interpolate between recorded endpoints.

A bounded acceleration burst outside the normal 700–1300 milli-g range can interrupt confirmation for less than 400 ms without discarding an armed lowered reference. The recovery band is 250–2500 milli-g, covering brief roughly 2 g acceleration and low-magnitude samples observed during real raises. These samples never count as viewing confirmation; the filter is reseeded and stable valid samples are required after recovery. Sustained bursts, severe impacts, vibration, long sample gaps and timestamp discontinuities cancel incomplete gestures without unlocking an already-raised wrist.

If the gravity filter averages opposing valid orientations into an implausibly small vector, it reseeds from the valid raw sample and clears confirmation rather than discarding the lowered reference. A rotation timeout clears the pending attempt and returns to `ARMED`. If the wrist is outside the viewing region at that moment, the reference is re-anchored to the current pose (revision 5). Before this, the old reference was kept and `ARMED` re-entered `ROTATING` on the very next sample, so the timeout never expired anything: a wrist parked in a typing pose stayed one ~10° tilt from waking, and slow drifts into view eventually triggered. A raise must now cover the full rotation within the timeout, measured from where the wrist actually paused. It still does not require another lowering before retrying an attempt that never activated the light. After a successful raise, `VIEWING` remains locked until confirmed lowering; neither motion recovery nor a timeout can rearm it. Starting the worker already raised still requires an initial confirmed lowered pose.

## The app

Opening the app goes straight to one settings menu (no intermediate screen). Toggle rows show a green check or red cross on the right; value rows show their current value underneath and change with each press of Select. Every change takes effect immediately: the app sends it to the running worker (`RTW_MSG_SETTINGS`) instead of killing and relaunching it, which used to freeze the screen for a second.

| Row | What it does | Key |
| --- | --- | --- |
| **Enabled** | Starts/stops the background worker (raise to wake itself) | — |
| **Sensitivity** | High (~19° turn, 3 samples), Normal (~24°, 3), Low (~30°, 4). Replaces the old *Viewing samples* setting | 13 |
| **Light duration** | Cycles 3, 5, 8, 10, 15, 30 s, *Until lowered*. Shows *Watch default* while the light sensor is on, since the watch's own timeout applies then | 6 |
| **Light sensor** | Light only when dark (`light_enable_interaction()`), the biggest battery saver | 10 |
| **Charger light** | Off / While charging / Plugged in (light stays on) | 8, 9 |
| **Schedule** | When on, raise to wake runs only between **Turn on at** and **Turn off at**, which appear underneath. Times use the watch's 12/24-hour setting and a boxed picker like the built-in Alarms app (Up/Down change, Select next, Back previous) | 14, 0–3 |
| **Logging** | Debug logging, off by default. Off, the worker prints only its startup line | 12 |

Keys 7 (old callback batching) and 11 (old viewing samples) are no longer read.

Fixed along the way:

- Before the Schedule switch existed, the app scheduled its start/stop alarms on every launch even when no schedule was set. Both defaulted to 00:00, so a worker you had turned off came back on every midnight.
- Opening the app killed and relaunched the worker, which cut any raise-to-wake light. Now it never restarts it. Only the very first launch starts it; after that **Enabled** is respected.
- Opening the app sends the worker a hand-off message: a forced gesture light becomes a normal system interaction light, so the worker's timer cannot switch it off while you are pressing buttons. Stopping the worker hands off the same way.

## Light behavior

- Without the light sensor, the worker uses `light_enable(true)`. Lowering or the duration timeout releases only its own override with `light_enable(false)`, returning control to the system.
- With the light sensor, a raise calls `light_enable_interaction()`. Lowering within 10 s of that raise (`RTW_INTERACTION_RELEASE_MS`) now ends the light too; previously it stayed on for the watch's full timeout. Later lowerings, or any after an app hand-off, leave the light alone, since it is more likely a button press or notification by then.
- Timers are cancelled on lowering, a later activation, shutdown, or entry into a charging/powered override. An old timeout cannot release a newer gesture or charging light. A timer allocation failure falls back to a normal system interaction timeout.
- Charging/powered overrides apply to an already-connected charger at worker startup and switch on or off as soon as the setting changes. Disconnecting restarts posture detection from `WAIT_LOWERED`.

## Tune on Pebble Time

The thresholds below were tuned from Pebble Time logs. The Pebble Time 2 has a different motion sensor (a 6-axis IMU). The API reports the same milli-g axes, so the detector should behave the same, but check a few raises with **Logging** on. If a watch rejects the 25 Hz request, the worker logs a warning and keeps running at the service default (also 25 Hz) instead of stopping.

All important parameters are in `src/c/raise_to_wake_config.h`:

| Parameter | Initial value / purpose |
| --- | --- |
| `RTW_ACCEL_SAMPLING_RATE`, `RTW_SAMPLES_PER_CALLBACK` | 25 Hz, 2; worker wakes 12.5 times a second |
| `RTW_VIEW_*`, `RTW_RETAIN_*` | Viewing and wider exit orientation bounds |
| `RTW_SENSITIVITY_*` | Rotation and confirmation samples per app Sensitivity level |
| `RTW_ROTATION_START_MG`, `RTW_ROTATION_TOTAL_MG` | 220 / 420; required gravity-vector change (420 is the fallback; Sensitivity sets 330 / 420 / 520) |
| `RTW_VIEW_PROGRESS_MG_SQUARED` | 10000; minimum decrease in squared distance to the viewing region |
| `RTW_STABLE_DELTA_MG`, `RTW_STABLE_RESIDUAL_MG` | 140 / 180; short stability checks |
| `RTW_GRAVITY_FILTER_DIVISOR` | 2; short filter |
| `RTW_TRANSIENT_GRAVITY_*`, `RTW_TRANSIENT_MOTION_MAX_MS` | 250–2500 milli-g / 400 ms; bounded motion interruption without discarding the lowered reference |
| `RTW_LOWER_CONFIRM_SAMPLES`, `RTW_COOLDOWN_MS` | 3 / 500 ms; lowering and repeat suppression |
| `RTW_REST_REFRESH_SAMPLES`, `RTW_REST_REFRESH_DRIFT_MG` | 8 / 40; refresh the lowered reference after a settled window, without following ongoing rotation |
| `RTW_ROTATION_TIMEOUT_MS` | 2000 ms per attempt; expiry clears confirmation and permits a fresh attempt from the confirmed lowered reference |
| `RTW_DEFAULT_LIGHT_DURATION_SECONDS` | 5; the app's Light duration overrides this |
| `RTW_INTERACTION_RELEASE_MS` | 10000; light sensor mode: lowering within this of the raise ends the light |
| `RTW_XYZ_LOG_INTERVAL_MS` | 1000; XYZ logging interval when **Logging** is on in the app |

The worker checks once a minute that motion data is still arriving and reconnects the accelerometer if a whole minute passed without any (firmware can reconfigure the sensor, for example when Motion Backlight is switched), logging `RTW no motion data for a minute; reconnecting`.

The startup log prints `RTW rev=9 ... sensitivity=N ... log=0|1` to identify the build. Revision 9 fixes raise to wake going dead while walking: a jolt over 400 ms mid-raise returns the detector to waiting for a lowered wrist, which it only accepted when held steady for three samples, so a swinging arm never rearmed and every raise was ignored until the arm was held still. Lowering now needs three lowered samples without a steadiness check, and footstep jolts no longer reset that count (raises still need steady viewing samples). Revision 8 added the stalled-stream reconnect. Revision 7 fixed hit-or-miss raises (viewing angle, rearming at a desk, raising from near the viewing angle, 24° Normal turn, 2 s attempts). Revision 6 added the stricter rotation, Sensitivity, paired samples, the logging setting, live settings and the light hand-off; revision 5 added timeout re-anchoring. With **Logging** on, state transitions and raises are logged, including `RTW raise detected: 3 samples, ... ms from view entry`. The millisecond latency measures the sampled viewing-entry point, not a separately measured physical wrist angle. Transition messages include triggering XYZ values. Once-per-second raw XYZ / filtered gravity logs also show `gate=` and confirmation count `n=`. Gates distinguish `outside-view`, `no-view-progress`, `small-rotation`, `unsettled`, `accel-magnitude`, `rotation-timeout`, `sample-gap`, `vibration`, `rest-refreshed`, and waiting for lowering/cooldown. There is no continuous per-sample logging.

For a hardware run, turn **Logging** on and use Normal sensitivity. Lower the wrist briefly, then raise normally; repeat with slower raises and on the wrist you normally wear the watch. Hold viewing for longer than the light timeout: it must not retrigger. Test tiny viewing-boundary movements, lowering and raising again, walking, typing, button interactions, notifications, and connecting/disconnecting the charger. Record missed raises, false activations, viewing XYZ values and logged latencies before changing thresholds.

After the gesture feels good, change `RTW_ACCEL_SAMPLING_RATE` to `ACCEL_SAMPLING_10HZ` and compare. At 10 Hz, three samples span 200 ms before delivery/filter delays; two span 100 ms. Sample-count settings and filter behavior must be evaluated again; equivalent responsiveness is not assumed.

## Appstore listing

Assets for the [Rebble developer portal](https://dev-portal.rebble.io) are in `store/`:

| File | Use |
| --- | --- |
| `store/icon-144x144.png`, `store/icon-48x48.png` | Large and small app icons |
| `store/banner-720x320.png` | Marketing banner at the top of the listing |
| `store/screenshots/` | Unframed watch screenshots, up to 5 per platform (add emery captures from the emulator) |

The launcher icon on the watch is `resources/images/menu_icon.png` (colour) and `menu_icon_bw.png` (black and white): a raised fist wearing a lit watch, drawn pixel by pixel at 25×25. The app has its own UUID, separate from rajid/pebble_backlight, so the two never replace each other on a watch. Credit to the original project belongs in the listing description.

## Build, install and collect logs

### Import into CloudPebble

Create/import a native C project from GitHub using `https://github.com/lucaboox/pebble-raise-to-wake` and branch `main`. The source folders use CloudPebble's standard `src/c` and `worker_src/c` layout, including the shared tuning header. The importer can ignore the host-only `tests` folder. Compile, then install onto your watch: Basalt is the Pebble Time, Emery the Pebble Time 2. Building for Emery, Flint or Gabbro needs a recent SDK that knows the new watches. Project metadata is in `package.json` (converted from the old `appinfo.json`, which predates them). Local `.pbw` compilation remains unverified as described below.

### Local SDK

Use a functioning [Pebble SDK environment](https://developer.rebble.io/sdk/). From this project directory:

```sh
pebble build
pebble install --phone PHONE_IP
pebble logs --phone PHONE_IP
```

Enable the Developer Connection in the phone app and replace `PHONE_IP` with its displayed address. The project targets all seven platforms, including `emery` (Pebble Time 2). See the [Pebble CLI guide](https://developer.rebble.io/guides/tools-and-resources/pebble-tool/) for installation and logs. Worker light declarations are deliberately preserved despite their absence from the worker's public header; real-hardware behavior is the reference here.

No `.pbw` was generated in the current development environment: the native Windows Pebble CLI was unavailable and WSL could not start because nested virtualization is unsupported. Full SDK linking, worker memory limits and hardware gesture accuracy remain to be verified in a functioning SDK environment.

## Host regression checks

With Node.js, Clang and wasm-ld available:

```sh
node tests/raise_to_wake.test.js
```

This compiles the actual C detector and worker into WebAssembly with a small SDK shim, then exercises gesture latency, log-derived viewing endpoints, shallow typing poses and acceleration bursts, settled-reference maintenance, hysteresis, cooldown, lowering, walking-like traces, timeout recovery, opposing gravity vectors, impacts, vibration, sample gaps, settings, timer lifecycle, ambient mode and charging behavior. All 87 checks passed. It is not a Pebble SDK build or a substitute for real-wrist traces.
