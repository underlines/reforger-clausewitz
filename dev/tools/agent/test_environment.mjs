// node dev/tools/agent/test_environment.mjs
import assert from 'node:assert/strict';
import { envPrompt } from './environment.mjs';

assert.equal(envPrompt(null), '');
assert.equal(envPrompt({}), '');
assert.equal(envPrompt({ env: null }), '');

const day = envPrompt({ env: { time: '14:30', daylight: 'day', weather: 'Clear', next: '', nextInMin: 0 } });
assert.match(day, /^ENVIRONMENT: time 14:30 \(day\), weather clear\./);
assert.doesNotMatch(day, /Night|turning/);

const night = envPrompt({ env: { time: '23:10', daylight: 'night', weather: 'Rainy', next: 'Overcast', nextInMin: 12 } });
assert.match(night, /time 23:10 \(night\)/);
assert.match(night, /turning overcast in about 12 min/);
assert.match(night, /Night: detection range is short/);
assert.match(night, /lowers visibility/);

const upcoming = envPrompt({ env: { time: '10:00', daylight: 'day', weather: 'Clear', next: 'Foggy', nextInMin: 5 } });
assert.match(upcoming, /Visibility will drop/);

assert.ok(envPrompt({ env: { time: '19:00', daylight: 'dusk', weather: 'Clear' } }).includes('Dusk'));
assert.ok(envPrompt({ env: { time: '08:00', weather: 'Clear' } }).startsWith('ENVIRONMENT:'));
console.log('environment tests ok');
