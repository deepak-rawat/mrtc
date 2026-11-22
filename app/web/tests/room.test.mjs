import { test } from 'node:test';
import assert from 'node:assert/strict';
import { registerHooks } from 'node:module';
import { envelope } from '../src/protocol.ts';

const sourceRoot = new URL('../src/', import.meta.url).href;
registerHooks({
    resolve(specifier, context, nextResolve) {
        if (context.parentURL?.startsWith(sourceRoot) &&
            specifier.startsWith('./') && !specifier.endsWith('.ts'))
            return nextResolve(`${specifier}.ts`, context);
        return nextResolve(specifier, context);
    },
});
const { Room } = await import('../src/room.ts');

function fixture() {
    const messages = [];
    const room = new Room({
        status() {}, error() {}, local() {}, participant() {}, remove() {}, media() {},
        message(peer, text) { messages.push({ peer, text }); },
    });
    room.closed = false;
    room.id = 'alice';
    const graph = { epoch: 1, mode: 'mesh', peers: new Set(['bob']), sessions: new Map(), ready: true };
    const channel = {
        readyState: 'open', bufferedAmount: 0, sent: [],
        close() { this.readyState = 'closed'; },
        send(data) { this.sent.push(data); },
    };
    const session = {
        id: 'm:1:alice:bob', peer: 'bob', channels: new Set(),
        pc: { close() {} }, decoder: { srcObject: {}, remove() {} },
    };
    graph.sessions.set(session.id, session);
    room.active = graph;
    room.channel(graph, session, channel);
    return { room, graph, session, channel, messages };
}

test('retiring data channels accept overlap once, then reject delayed callbacks after cleanup', () => {
    const f = fixture();
    const receive = f.channel.onmessage;
    const wire = envelope('bob', 1n, 'overlap');
    receive({ data: wire });
    f.room.active = { ...f.graph, epoch: 2, sessions: new Map() };
    f.room.retiring = f.graph;
    receive({ data: wire });
    assert.deepEqual(f.messages, [{ peer: 'bob', text: 'overlap' }]);
    f.room.closeGraph(f.graph);
    assert.equal(f.channel.onmessage, null);
    assert.equal(f.session.channels.size, 0);
    receive({ data: envelope('bob', 2n, 'retired') });
    assert.equal(f.messages.length, 1);
});

test('chat selects only the active graph and checks readiness and backpressure before sending', () => {
    const f = fixture();
    const next = fixture();
    next.graph.epoch = 2;
    f.room.pending = next.graph;
    f.room.sendChat('before');
    assert.equal(f.channel.sent.length, 1);
    assert.equal(next.channel.sent.length, 0);
    f.room.retiring = f.graph;
    f.room.active = next.graph;
    f.room.pending = undefined;
    f.room.sendChat('after');
    assert.equal(f.channel.sent.length, 1);
    assert.equal(next.channel.sent.length, 1);
    next.channel.bufferedAmount = 1024 * 1024;
    assert.throws(() => f.room.sendChat('queued'), /backpressured/);
    assert.equal(next.channel.sent.length, 1);
    const edgeSize = envelope('alice', 1n, 'edge').byteLength;
    next.channel.bufferedAmount = 1024 * 1024 - edgeSize;
    f.room.sendChat('edge');
    assert.equal(next.channel.sent.length, 2);
    next.channel.bufferedAmount++;
    assert.throws(() => f.room.sendChat('edge'), /backpressured/);
    assert.equal(next.channel.sent.length, 2);
    next.channel.bufferedAmount = 0;
    f.room.sendChat('x'.repeat(65535 - 28));
    assert.equal(next.channel.sent.at(-1).byteLength, 65535);
    assert.throws(() => f.room.sendChat('x'.repeat(65536 - 28)), /too large/);
    next.channel.readyState = 'closed';
    assert.throws(() => f.room.sendChat('closed'), /not ready/);
    assert.equal(next.channel.sent.length, 3);
});
