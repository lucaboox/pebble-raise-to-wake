# Pebble Raise to Wake

A responsive background-worker backlight controller for Pebble Time, based on [rajid/pebble_backlight](https://github.com/rajid/pebble_backlight).

This version retains the background worker and manually declared `light_enable_interaction()` / `light_enable(bool)` calls from rajid/pebble_backlight, which were confirmed working on a real Pebble Time. It replaces the original delayed orientation check with a responsive raise detector. The foreground app still controls worker enable/disable, scheduled operation, light duration, ambient mode, and charging/powered lighting.

## Why the original felt slow

The old worker stored `watch_level_start = time(0L)` and waited for the integer number of seconds to advance before lighting the screen. This adds up to about one second even after the viewing posture is recognized. At 10 Hz, collecting N samples per callback can additionally delay delivery by approximately N × 100 ms. The previous rate change also occurred before subscribing; this version subscribes first, then checks the rate-setting result.

The new worker requests **25 Hz, one sample per callback**. All gesture timing uses `AccelData.timestamp` in milliseconds and consecutive sample counts. The `time()` calls remaining in the foreground app are for scheduled start/stop alarms only.

## Detector and latency

`worker_src/c/raise_to_wake_detector.c` is the portable detector; `worker_src/c/backlight_worker.c` owns the Pebble subscriptions and backlight/timer calls. The existing `wscript` already includes every C source under `worker_src`, so it builds both modules into the worker.

The detector progresses through `WAIT_LOWERED → ARMED → ROTATING → CONFIRMING_VIEW → VIEWING`:

1. Confirm a stable posture outside the wider viewing boundary before arming. Starting the worker while already viewing does not trigger.
2. Require a meaningful X/Y/Z gravity-vector change toward the viewing region, rather than arbitrary movement.
3. Require three consecutive viewing samples with plausible gravity magnitude and limited sample-to-sample motion. A short gravity filter rejects jitter without a long settling delay.
4. Activate the light immediately on confirmation. Stay in `VIEWING` even if the light's duration expires.
5. Require three samples clearly outside the wider exit boundary before reporting lowering. A 500 ms cooldown also prevents rapid repeat raises.

The viewing region was widened after real Pebble Time logs showed missed tilted and near-vertical poses. Entering requires X −550…550, Y −1150…−300, Z −1100…450 milli-g. Exiting requires leaving the wider X −650…650, Y −1250…−200, Z −1200…550 region. Values between the entry and exit boundaries do not flicker the light or rearm the detector. Z still rejects more fully screen-down poses, such as Y −700 / Z +714.

Direction is measured by decreasing squared X/Y/Z distance to the accepted viewing region. The previous fixed ideal-direction dot product incorrectly rejected raises from a flat resting wrist toward a vertical screen: Y becomes more negative while Z becomes less negative, cancelling its direction score. The detector still requires a 350 milli-g vector change and consecutive stable viewing samples.

At 25 Hz, three valid samples span 80 ms from first to third; sampling phase, filtering and settling add some delay. Host traces of 320–1200 ms raises measured **120–200 ms from raw viewing-region entry to detection**, before real-device delivery overhead. Log-derived flat-to-tilted/vertical traces also confirm within 200 ms of reaching their final posture. Entering the wider region while still moving can take longer to confirm. These are synthetic results, not hardware measurements: the supplied watch log contains XYZ snapshots only once per second, so the tests interpolate between recorded endpoints.

A bounded acceleration burst outside the normal 700–1300 milli-g range can interrupt confirmation for less than 400 ms without discarding an armed lowered reference. The recovery band is 250–2500 milli-g, covering brief roughly 2 g acceleration and low-magnitude samples observed during real raises. These samples never count as viewing confirmation; the filter is reseeded and stable valid samples are required after recovery. Sustained bursts, severe impacts, vibration, long sample gaps and timestamp discontinuities cancel incomplete gestures without unlocking an already-raised wrist.

If the gravity filter averages opposing valid orientations into an implausibly small vector, it reseeds from the valid raw sample and clears confirmation rather than discarding the lowered reference. A rotation timeout also clears the pending attempt and returns to `ARMED` using that reference. It does not require another lowering before retrying an attempt that never activated the light. After a successful raise, `VIEWING` remains locked until confirmed lowering; neither motion recovery nor a timeout can rearm it. Starting the worker already raised still requires an initial confirmed lowered pose.

## Settings and light behavior

- **Viewing samples** replaces the old Responsiveness menu. Default 3; range 2–10. It adjusts confirmation samples, keeping callback delivery at one sample. New persistent key 11 prevents old callback-batch settings under key 7 from restoring latency. Other existing settings keep their keys.
- **Set Timeout** keeps its seconds-based duration. Default 5 seconds when absent; 0 means a forced-on gesture light lasts until confirmed lowering.
- **Use light sensor** preserves `light_enable_interaction()` and Pebble's normal automatic timeout. Custom duration does not override the system interaction timeout. Lowering does not force an ambient-mode light off, protecting button/notification interactions.
- With ambient mode disabled, the worker preserves `light_enable(true)`. Lowering or timeout releases only its own forced-on override using `light_enable(false)`, returning control to the system. This tracks our override, not every external backlight interaction.
- Timers are cancelled on lowering, a later activation, shutdown, or entry into a charging/powered override. An old timeout cannot release a newer gesture or charging light. A timer allocation failure falls back to a normal system interaction timeout.
- Charging/powered overrides remain active as configured, and apply to an already-connected charger at worker startup. Disconnecting restarts posture detection from `WAIT_LOWERED`.

## Tune on Pebble Time

All important parameters are in `src/c/raise_to_wake_config.h`:

| Parameter | Initial value / purpose |
| --- | --- |
| `RTW_ACCEL_SAMPLING_RATE`, `RTW_SAMPLES_PER_CALLBACK` | 25 Hz, 1; responsive delivery |
| `RTW_VIEW_*`, `RTW_RETAIN_*` | Viewing and wider exit orientation bounds |
| `RTW_DEFAULT_CONFIRM_SAMPLES` | 3; also adjustable in the app |
| `RTW_ROTATION_START_MG`, `RTW_ROTATION_TOTAL_MG` | 220 / 350; required gravity-vector change |
| `RTW_VIEW_PROGRESS_MG_SQUARED` | 10000; minimum decrease in squared distance to the viewing region |
| `RTW_STABLE_DELTA_MG`, `RTW_STABLE_RESIDUAL_MG` | 140 / 180; short stability checks |
| `RTW_GRAVITY_FILTER_DIVISOR` | 2; short filter |
| `RTW_TRANSIENT_GRAVITY_*`, `RTW_TRANSIENT_MOTION_MAX_MS` | 250–2500 milli-g / 400 ms; bounded motion interruption without discarding the lowered reference |
| `RTW_LOWER_CONFIRM_SAMPLES`, `RTW_COOLDOWN_MS` | 3 / 500 ms; lowering and repeat suppression |
| `RTW_ROTATION_TIMEOUT_MS` | 1500 ms per attempt; expiry clears confirmation and permits a fresh attempt from the confirmed lowered reference |
| `RTW_DEFAULT_LIGHT_DURATION_SECONDS` | 5; persisted Set Timeout setting overrides this |
| `RTW_LOG_XYZ`, `RTW_XYZ_LOG_INTERVAL_MS` | XYZ logging once per second; set logging to 0 after tuning |

The startup log prints `RTW rev=3` to identify the build with recovery fixes. State transitions and raises are logged, including `RTW raise detected: 3 samples, ... ms from view entry`. The millisecond latency measures the sampled viewing-entry point, not a separately measured physical wrist angle. Transition messages include triggering XYZ values. Once-per-second raw XYZ / filtered gravity logs also show `gate=` and confirmation count `n=`. Gates distinguish `outside-view`, `no-view-progress`, `small-rotation`, `unsettled`, `accel-magnitude`, `rotation-timeout`, `sample-gap`, `vibration`, and waiting for lowering/cooldown. There is no continuous per-sample logging.

For the first hardware run, use three viewing samples and 25 Hz. Lower the wrist briefly, then raise normally; repeat with slower raises and on the wrist you normally wear the watch. Hold viewing for longer than the light timeout: it must not retrigger. Test tiny viewing-boundary movements, lowering and raising again, walking, typing, button interactions, notifications, and connecting/disconnecting the charger. Record missed raises, false activations, viewing XYZ values and logged latencies before changing thresholds.

After the gesture feels good, change `RTW_ACCEL_SAMPLING_RATE` to `ACCEL_SAMPLING_10HZ` and compare. At 10 Hz, three samples span 200 ms before delivery/filter delays; two span 100 ms. Sample-count settings and filter behavior must be evaluated again; equivalent responsiveness is not assumed.

## Build, install and collect logs

### Import into CloudPebble

Create/import a native C project from GitHub using `https://github.com/lucaboox/pebble-raise-to-wake` and branch `main`. The source folders use CloudPebble's standard `src/c` and `worker_src/c` layout, including the shared tuning header. The importer can ignore the host-only `tests` folder. Compile for Basalt to test on Pebble Time, install onto your watch, and start the background worker from the app. Local `.pbw` compilation remains unverified as described below.

### Local SDK

Use a functioning [Pebble SDK environment](https://developer.rebble.io/sdk/). From this project directory:

```sh
pebble build
pebble install --phone PHONE_IP
pebble logs --phone PHONE_IP
```

Enable the Developer Connection in the phone app and replace `PHONE_IP` with its displayed address. The project still targets `basalt` (Pebble Time), plus its original aplite/chalk/diorite targets. See the [Pebble CLI guide](https://developer.rebble.io/guides/tools-and-resources/pebble-tool/) for installation and logs. Worker light declarations are deliberately preserved despite their absence from the worker's public header; real-hardware behavior is the reference here.

No `.pbw` was generated in the current development environment: the native Windows Pebble CLI was unavailable and WSL could not start because nested virtualization is unsupported. Full SDK linking, worker memory limits and hardware gesture accuracy remain to be verified in a functioning SDK environment.

## Host regression checks

With Node.js, Clang and wasm-ld available:

```sh
node tests/raise_to_wake.test.js
```

This compiles the actual C detector and worker into WebAssembly with a small SDK shim, then exercises gesture latency, log-derived viewing endpoints and acceleration bursts, hysteresis, cooldown, lowering, walking-like traces, timeout recovery, opposing gravity vectors, impacts, vibration, sample gaps, settings, timer lifecycle, ambient mode and charging behavior. All 52 checks passed. It is not a Pebble SDK build or a substitute for real-wrist traces.
