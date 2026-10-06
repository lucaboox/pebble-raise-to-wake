'use strict';
// Tests run the actual C core and worker compiled to WebAssembly with SDK mocks.
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const {spawnSync} = require('child_process');
const root = path.resolve(__dirname, '..');
const output = path.join(root, 'build', 'host-tests', 'worker.wasm');
fs.mkdirSync(path.dirname(output), {recursive: true});
const compile = spawnSync(process.env.CLANG || 'clang', [
  '--target=wasm32', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Oz',
  '-ffreestanding', '-fno-builtin', '-nostdlib', '-Itests/sdk_mock',
  'worker_src/c/raise_to_wake_detector.c', 'tests/worker_harness.c',
  '-Wl,--no-entry', '-Wl,--export-all', '-o', output
], {cwd: root, encoding: 'utf8'});
if (compile.error) throw compile.error;
assert.strictEqual(compile.status, 0, compile.stdout + compile.stderr);

(async function () {
  const {instance} = await WebAssembly.instantiate(fs.readFileSync(output));
  const api = instance.exports;
  const WAIT = 0, ARMED = 1, VIEWING = 4, RAISE = 1, LOWER = 2;
  let now, raises, lowers, worker, tests = 0, raisedAt;
  function reset(confirmations = 3) {
    now = 0; raises = 0; lowers = 0; raisedAt = 0; worker = false;
    api.core_init(confirmations);
    api.mock_reset();
  }
  function feed(x, y, z, vibrated = false, interval = 40) {
    now += interval;
    x = Math.round(x); y = Math.round(y); z = Math.round(z);
    if (worker) api.mock_feed(x, y, z, now, +vibrated);
    else {
      const event = api.core_feed(x, y, z, now, +vibrated);
      if (event === RAISE) { ++raises; raisedAt = now; }
      if (event === LOWER) ++lowers;
    }
  }
  function hold(x, y, z, duration) {
    for (let i = 0; i < Math.ceil(duration / 40); ++i) feed(x, y, z);
  }
  function lower(sign = 1, duration = 800) { hold(sign * 1000, 0, 0, duration); }
  function rotate(duration = 480, sign = 1) {
    const count = Math.ceil(duration / 40);
    for (let i = 1; i <= count; ++i) {
      const angle = i * Math.PI / 2 / count;
      feed(sign * 1000 * Math.cos(angle), -700 * Math.sin(angle), -714 * Math.sin(angle));
    }
  }
  function raise(duration = 480, sign = 1) { rotate(duration, sign); hold(0, -700, -714, 240); }
  // Log-derived endpoints, interpolated at 25 Hz: the supplied log records
  // only one XYZ reading per second, so these are not full hardware replays.
  function rotateFromFlatTo(x, y, z, duration = 480) {
    const count = Math.ceil(duration / 40);
    for (let i = 1; i <= count; ++i) {
      const t = i / count;
      const v = [x * t, -90 + (y + 90) * t, -1000 + (z + 1000) * t];
      const scale = 1000 / Math.hypot(...v);
      feed(...v.map(value => value * scale));
    }
    hold(x, y, z, 280);
  }
  function startWorker(settings = {}) {
    for (const [key, value] of Object.entries(settings)) api.mock_setting(+key, value);
    api.mock_start(); worker = true;
  }
  function test(name, run) {
    reset(); run(); ++tests; console.log('PASS ' + name);
  }

  test('startup already viewing cannot trigger', () => {
    hold(0, -700, -714, 4000);
    assert.strictEqual(raises, 0); assert.strictEqual(api.core_state(), WAIT);
  });
  for (const sign of [-1, 1]) for (const duration of [320, 480, 800, 1200]) {
    test('natural raise sign=' + sign + ' duration=' + duration + 'ms', () => {
      lower(sign); assert.strictEqual(api.core_state(), ARMED);
      raise(duration, sign);
      assert.strictEqual(raises, 1); assert.strictEqual(api.core_state(), VIEWING);
      assert(api.core_latency() <= 200, 'view-entry latency exceeds 200 ms');
      console.log('  measured sample-entry latency: ' + api.core_latency() + ' ms');
    });
  }
  test('abrupt but deliberate orientation change confirms within 200 ms', () => {
    lower(); const entry = now + 40;
    hold(0, -700, -714, 240);
    assert.strictEqual(raises, 1); assert(raisedAt - entry <= 200);
  });
  test('keeps useful near-vertical viewing posture from original XY detector', () => {
    lower(); hold(0, -1000, 0, 360);
    assert.strictEqual(raises, 1);
  });
  for (const pose of [[-433, -647, -663], [-290, -718, -690],
                      [-174, -1048, -42], [-131, -1038, 290], [-179, -1059, 449]]) {
    test('flat resting wrist to log-derived viewing pose ' + pose.join(','), () => {
      hold(15, -90, -1000, 800);
      assert.strictEqual(api.core_state(), ARMED);
      const arrival = now + 480;
      rotateFromFlatTo(...pose);
      assert.strictEqual(raises, 1);
      assert(raisedAt - arrival <= 200, 'settled viewing pose should confirm promptly');
      hold(...pose, 3000); assert.strictEqual(raises, 1);
    });
  }
  test('startup in log-derived tilted viewing pose waits for lowering', () => {
    hold(-433, -647, -663, 3000);
    assert.strictEqual(raises, 0); assert.strictEqual(api.core_state(), WAIT);
    hold(15, -90, -1000, 800); rotateFromFlatTo(-433, -647, -663);
    assert.strictEqual(raises, 1);
  });
  test('holding wrist raised never retriggers', () => {
    lower(); raise(); hold(0, -700, -714, 10000);
    assert.strictEqual(raises, 1); assert.strictEqual(api.core_state(), VIEWING);
  });
  test('lowering and cooldown permit another deliberate raise', () => {
    lower(); raise(); lower(); raise();
    assert.strictEqual(raises, 2); assert.strictEqual(lowers, 1);
  });
  test('hysteresis prevents exit and rearm on inner-boundary jitter', () => {
    lower(); raise();
    hold(540, -700, -500, 240);
    for (let i = 0; i < 100; ++i) feed(i % 2 ? 590 : 540, -700, -500);
    assert.strictEqual(lowers, 0); assert.strictEqual(raises, 1);
    assert.strictEqual(api.core_state(), VIEWING);
  });
  test('short excursion beyond exit boundary does not lower', () => {
    lower(); raise(); feed(690, -600, -400); hold(0, -700, -714, 800);
    assert.strictEqual(lowers, 0); assert.strictEqual(raises, 1);
  });
  test('lowering inside cooldown cannot immediately retrigger', () => {
    lower(); hold(0, -700, -714, 200);
    assert.strictEqual(raises, 1);
    hold(1000, 0, 0, 200); hold(0, -700, -714, 1200);
    assert.strictEqual(raises, 1);
  });
  test('small lowered-wrist motion does not trigger', () => {
    lower();
    for (let i = 0; i < 400; ++i) feed(980, 120 * Math.sin(i / 5), -80 * Math.cos(i / 7));
    assert.strictEqual(raises, 0);
  });
  test('walking-like rhythmic swings do not settle into viewing', () => {
    lower();
    for (let i = 0; i < 500; ++i) {
      const angle = 1.25 * Math.sin(i * Math.PI / 8);
      feed(1000 * Math.cos(angle), -700 * Math.sin(angle), -714 * Math.sin(angle));
    }
    assert.strictEqual(raises, 0);
  });
  test('face-down XY match is rejected by Z', () => {
    lower(); hold(0, -700, 714, 2000); assert.strictEqual(raises, 0);
  });
  test('viewing must settle for consecutive valid samples', () => {
    lower(); rotate(320);
    for (let i = 0; i < 12; ++i) feed(i % 2 ? 150 : -150, -700, -714);
    assert.strictEqual(raises, 0);
    hold(0, -700, -714, 280); assert.strictEqual(raises, 1);
  });
  test('vibration cancels candidate and cannot fake a raise', () => {
    lower(); rotate(320); feed(0, -700, -714, true); hold(0, -700, -714, 1600);
    assert.strictEqual(raises, 0); lower(); raise(); assert.strictEqual(raises, 1);
  });
  test('brief acceleration burst during a raise preserves the lowered reference', () => {
    hold(15, -90, -1000, 800);
    feed(-100, -500, -850); feed(0, -300, -1400);
    assert.strictEqual(raises, 0);
    const entry = now + 40;
    hold(-433, -647, -663, 360);
    assert.strictEqual(raises, 1); assert(raisedAt - entry <= 200);
  });
  test('isolated acceleration burst with no orientation change never raises', () => {
    hold(15, -90, -1000, 800); feed(0, -90, -1450);
    hold(15, -90, -1000, 1200); assert.strictEqual(raises, 0);
  });
  for (const burst of [[-324, 1855, -476], [251, -1506, 1208], [-249, 90, -374]]) {
    test('log-derived acceleration burst does not strand the next viewing pose ' + burst, () => {
      hold(-58, 350, -961, 800); hold(...burst, 80);
      assert.strictEqual(raises, 0);
      const entry = now + 40;
      hold(-138, -1051, -51, 400);
      assert.strictEqual(raises, 1); assert(raisedAt - entry <= 200);
      hold(-138, -1051, -51, 4000); assert.strictEqual(raises, 1);
    });
  }
  test('rotation timeout at viewing entry can retry without another lowering', () => {
    lower(); hold(600, -500, -624, 1440);
    assert.strictEqual(raises, 0);
    const entry = now + 40;
    hold(0, -700, -714, 400);
    assert.strictEqual(raises, 1); assert(raisedAt - entry <= 200);
    hold(0, -700, -714, 8000); assert.strictEqual(raises, 1);
  });
  test('opposing valid orientations do not lose the lowered reference in the filter', () => {
    hold(0, 1000, 0, 800);
    const entry = now + 40;
    hold(0, -1000, 0, 400);
    assert.strictEqual(raises, 1); assert(raisedAt - entry <= 200);
  });
  test('expired non-viewing attempts never light the screen and permit a later raise', () => {
    lower(); hold(600, -500, -624, 6000); assert.strictEqual(raises, 0);
    const entry = now + 40;
    hold(0, -700, -714, 400);
    assert.strictEqual(raises, 1); assert(raisedAt - entry <= 200);
  });
  test('startup already raised cannot use burst or timeout recovery to arm', () => {
    hold(-138, -1051, -51, 800); feed(-324, 1855, -476);
    hold(-138, -1051, -51, 8000);
    assert.strictEqual(raises, 0); assert.strictEqual(api.core_state(), WAIT);
  });
  test('a burst clears partial viewing confirmation and requires fresh stable samples', () => {
    hold(15, -90, -1000, 800);
    for (let i = 0; i < 8 && api.core_state() !== 3; ++i) feed(-433, -647, -663);
    assert.strictEqual(api.core_state(), 3); assert.strictEqual(raises, 0);
    feed(-324, 1855, -476);
    for (let i = 0; i < 3; ++i) feed(-433, -647, -663);
    assert.strictEqual(raises, 0);
    feed(-433, -647, -663); assert.strictEqual(raises, 1);
  });
  test('worker recovers a fast raise without timer expiry or motion unlocking VIEWING', () => {
    startWorker({6: 2}); hold(-58, 350, -961, 800); feed(251, -1506, 1208);
    hold(-138, -1051, -51, 400); assert.strictEqual(api.mock_on_calls(), 1);
    api.mock_expire(0); assert.strictEqual(api.mock_off_calls(), 1);
    feed(-324, 1855, -476); hold(-138, -1051, -51, 8000);
    assert.strictEqual(api.mock_on_calls(), 1); assert.strictEqual(api.mock_state(), VIEWING);
    hold(-58, 350, -961, 800); feed(251, -1506, 1208);
    hold(-138, -1051, -51, 400); assert.strictEqual(api.mock_on_calls(), 2);
  });
  for (const [z, duration] of [[-1450, 480], [-4000, 40]]) {
    test('prolonged or severe impact cancels candidate z=' + z, () => {
      hold(15, -90, -1000, 800); hold(0, 0, z, duration);
      hold(-433, -647, -663, 2000); assert.strictEqual(raises, 0);
      hold(15, -90, -1000, 800); rotateFromFlatTo(-433, -647, -663);
      assert.strictEqual(raises, 1);
    });
  }
  test('impacts and data gaps do not trigger or unlock raised wrist', () => {
    lower(); feed(0, -700, -714, false, 1000); hold(0, -700, -714, 600);
    assert.strictEqual(raises, 0);
    lower(); raise(); feed(0, 0, -4000); feed(0, -700, -714, false, 1000);
    hold(0, -700, -714, 1200);
    assert.strictEqual(raises, 1); assert.strictEqual(api.core_state(), VIEWING);
  });
  test('timestamp rollback preserves viewing lockout', () => {
    lower(); raise(); now = 0; hold(0, -700, -714, 1600);
    assert.strictEqual(raises, 1); assert.strictEqual(api.core_state(), VIEWING);
  });
  test('confirmation count is tunable, with invalid settings defaulting to three', () => {
    reset(2); lower(); raise(); const fastLatency = api.core_latency();
    reset(5); lower(); raise(); assert.strictEqual(raises, 1);
    assert(api.core_latency() >= fastLatency + 80);
    reset(99); lower(); raise(); assert.strictEqual(raises, 1);
  });
  test('worker subscribes before setting 25 Hz, ignores old batch preference', () => {
    startWorker({7: 20}); assert.strictEqual(api.mock_rate(), 25);
    assert.strictEqual(api.mock_batch(), 1); assert.strictEqual(api.mock_rate_order(), 1);
    lower(); raise(); assert.strictEqual(api.mock_on_calls(), 1);
    assert.strictEqual(api.mock_timer_duration(0), 5000);
    hold(0, -700, -714, 8000); assert.strictEqual(api.mock_on_calls(), 1);
    assert(api.mock_logs() < 30, 'logging should not happen per sample');
  });
  test('timer expiry releases light without retriggering while viewing', () => {
    startWorker({6: 2}); lower(); raise(); api.mock_expire(0);
    assert.strictEqual(api.mock_off_calls(), 1); assert.strictEqual(api.mock_state(), VIEWING);
    hold(0, -700, -714, 3000); assert.strictEqual(api.mock_on_calls(), 1);
    lower(); raise(); assert.strictEqual(api.mock_on_calls(), 2);
  });
  test('lowering cancels old timer so it cannot end a subsequent raise', () => {
    startWorker(); lower(); raise(); lower(); assert.strictEqual(api.mock_timer_cancelled(), 1);
    raise(); const offCalls = api.mock_off_calls(); api.mock_expire(0);
    assert.strictEqual(api.mock_off_calls(), offCalls);
    api.mock_expire(1); assert.strictEqual(api.mock_off_calls(), offCalls + 1);
  });
  test('ambient mode uses interaction and never forces off on lowering', () => {
    startWorker({10: 1}); lower(); raise(); lower();
    assert.strictEqual(api.mock_interaction_calls(), 1);
    assert.strictEqual(api.mock_on_calls(), 0); assert.strictEqual(api.mock_off_calls(), 0);
    assert.strictEqual(api.mock_timer_count(), 0);
  });
  test('zero duration retains light until confirmed lowering', () => {
    startWorker({6: 0}); lower(); raise(); assert.strictEqual(api.mock_timer_count(), 0);
    lower(); assert.strictEqual(api.mock_off_calls(), 1);
  });
  test('charging override cancels timer and resets tracking on disconnect', () => {
    startWorker({8: 1}); lower(); raise(); api.mock_battery(1, 1);
    const offCalls = api.mock_off_calls(); api.mock_expire(0);
    assert.strictEqual(api.mock_off_calls(), offCalls);
    api.mock_battery(0, 0); assert.strictEqual(api.mock_off_calls(), offCalls + 1);
    assert.strictEqual(api.mock_state(), WAIT);
    hold(0, -700, -714, 1000); assert.strictEqual(api.mock_on_calls(), 2);
  });
  test('powered-light setting applies to already connected charger at startup', () => {
    api.mock_battery(0, 1); startWorker({9: 1}); assert.strictEqual(api.mock_on_calls(), 1);
    lower(); raise(); assert.strictEqual(api.mock_timer_count(), 0);
  });
  test('timer allocation failure falls back to system interaction timeout', () => {
    startWorker(); api.mock_fail_timer(1); lower(); raise();
    assert.strictEqual(api.mock_on_calls(), 1); assert.strictEqual(api.mock_off_calls(), 1);
    assert.strictEqual(api.mock_interaction_calls(), 1);
  });
  test('sampling-rate failure removes subscription', () => {
    api.mock_fail_rate(-1); startWorker(); assert.strictEqual(api.mock_has_subscription(), 0);
  });
  console.log('\n' + tests + ' checks passed. Pebble SDK build and real-wrist validation remain separate.');
})().catch(error => { console.error(error); process.exitCode = 1; });
