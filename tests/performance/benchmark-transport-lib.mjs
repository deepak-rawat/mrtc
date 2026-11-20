import assert from 'node:assert/strict';
import { transportMetricRules } from './benchmark-lib.mjs';

export function splitFlags(text) {
    const values = [];
    let value = '', quote = '', escaped = false, started = false;
    for (const char of text) {
        if (escaped) { value += char; escaped = false; started = true; }
        else if (char === '\\' && quote !== "'") { escaped = true; started = true; }
        else if (quote) {
            if (char === quote) quote = '';
            else value += char;
        } else if (char === '"' || char === "'") { quote = char; started = true; }
        else if (/\s/.test(char)) {
            if (started) values.push(value);
            value = ''; started = false;
        } else {
            assert(!/[;$`|&<>]/.test(char), 'Shell expansion/operators are not supported in compiler flags');
            value += char; started = true;
        }
    }
    assert(!quote && !escaped, 'Unterminated compiler flag quoting');
    if (started) values.push(value);
    return values;
}

export function parseTransportOutput(text, { smoke = false, seconds } = {}) {
    const rows = text.trim().split('\n').map(line => JSON.parse(line));
    assert(rows.length === 1 && rows[0].type === 'transport-harness', 'Expected one transport harness result');
    const value = rows[0];
    assert(value.ok === true, `Transport functional failure: ${value.error}`);
    assert.equal(value.fingerprintVerified, true, 'Missing verified DTLS fingerprint');
    assert.equal(value.smoke, smoke, 'Smoke output cannot qualify a loaded measurement');
    assert(Number.isSafeInteger(value.packets) && value.packets >= 32, 'Missing exact delivered packet evidence');
    assert(Number.isFinite(value.elapsedSeconds) && value.elapsedSeconds > 0, 'Invalid transport duration');
    if (!smoke) assert(value.elapsedSeconds >= seconds, 'Transport measurement ended before requested duration');
    for (const key of Object.keys(transportMetricRules))
        assert(Number.isFinite(value.metrics?.[key]) && value.metrics[key] > 0, `Invalid transport metric ${key}`);
    assert(value.metrics.transportOneWayMaxMs >= value.metrics.transportOneWayP95Ms &&
        value.metrics.transportOneWayP95Ms >= value.metrics.transportOneWayP50Ms, 'Invalid latency percentiles');
    const rate = value.packets / value.elapsedSeconds;
    assert(Math.abs(value.metrics.transportDeliveredPacketsPerSecond - rate) <= rate * 1e-6,
        'Inconsistent delivered packet rate');
    assert(Math.abs(value.metrics.transportPayloadBytesPerSecond - rate * 1000) <= rate * 1000 * 1e-6,
        'Inconsistent payload byte rate');
    return value;
}
