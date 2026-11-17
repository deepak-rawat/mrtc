import { test } from 'node:test';
import assert from 'node:assert/strict';
import { sessionId, envelope, MessageHistory, parseSignal } from '../src/protocol.ts';

test('mesh session IDs are deterministic while SFU subscriptions are directional', () => {
    assert.equal(sessionId('mesh', 42, 'p9', 'p2'), 'm:42:p2:p9');
    assert.equal(sessionId('mesh', 42, 'p2', 'p9'), 'm:42:p2:p9');
    assert.equal(sessionId('mesh', 42, 'a', 'Z'), 'm:42:Z:a');
    assert.equal(sessionId('mesh', 42, '_peer', 'Zpeer'), 'm:42:Zpeer:_peer');
    assert.equal(sessionId('mesh', 42, 'Apeer', '-peer'), 'm:42:-peer:Apeer');
    assert.equal(sessionId('sfu', 43, 'p9'), 'p:43:p9');
    assert.equal(sessionId('sfu', 43, 'p9', 'p2'), 's:43:p9:p2');
});
test('MRC2 uses network uint64 and deduplicates overlapping graphs per sender', () => {
    const id = 0x0102030405060708n;
    const wire = envelope('p9', id, 'hello 🎵');
    assert.deepEqual([...new Uint8Array(wire).slice(20, 28)], [1, 2, 3, 4, 5, 6, 7, 8]);
    const history = new MessageHistory();
    assert.deepEqual(history.read(wire), { peer: 'p9', text: 'hello 🎵' });
    assert.equal(history.read(wire), undefined);
    assert.deepEqual(history.read(envelope('p2', id, 'other sender')), { peer: 'p2', text: 'other sender' });
    for (let n = 1n; n <= 256n; n++) history.read(envelope('p9', n, 'bounded'));
    assert.deepEqual(history.read(wire), { peer: 'p9', text: 'hello 🎵' });
});
test('malformed envelopes fail explicitly and legacy envelopes remain readable', () => {
    const history = new MessageHistory();
    assert.throws(() => history.read(new ArrayBuffer(3)), /Truncated/);
    const zero = envelope('p2', 1n, 'x');
    new DataView(zero).setBigUint64(20, 0n);
    assert.throws(() => history.read(zero), /Zero/);
    assert.throws(() => envelope('oversized-peer-id-123', 1n, 'x'), /identity/);
    const legacy = new Uint8Array(22);
    legacy.set(new TextEncoder().encode('MRTCp2'));
    legacy.set(new TextEncoder().encode('ok'), 20);
    assert.deepEqual(history.read(legacy.buffer), { peer: 'p2', text: 'ok' });
});
test('signaling validates versions, roster bounds, epochs, and topology', () => {
    assert.equal(parseSignal('{"type":"topology_commit","version":2,"epoch":2,"mode":"sfu"}').epoch, 2);
    for (const value of [
        { type: 'joined', version: 1 }, { type: 'candidate', epoch: -1 },
        { type: 'joined', mode: 'auto' }, { type: 'joined', peers: ['bad id'] },
        { type: 'joined', peers: Array(9).fill('p2') }, { type: 'offer', sdp: 3 },
        { type: 'topology_prepare', epoch: 2, mode: 'sfu' },
        { type: 'topology_abort', epoch: 2, mode: 'sfu' },
        { type: 'candidate', epoch: 2, session_id: 'p:2:p2', from: 'sfu' },
    ]) assert.throws(() => parseSignal(JSON.stringify(value)));
});
