import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, writeFile, rm, realpath } from 'node:fs/promises';
import net from 'node:net';

const binary = process.argv[2];
const fixture = `server-runtime-fixture-${process.pid}`;
const sockets = [];
let server;
let log = '';
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

async function until(predicate, description, timeout = 5000) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        const result = await predicate();
        if (result) return result;
        if (server?.exitCode !== null) throw new Error(`server exited: ${log}`);
        await delay(10);
    }
    throw new Error(`timed out: ${description}\n${log}`);
}

async function peer(url, room, version = 2) {
    const ws = new WebSocket(url, 'signaling');
    sockets.push(ws);
    const messages = [];
    ws.addEventListener('message', event => messages.push(JSON.parse(event.data)));
    await once(ws, 'open');
    const send = message => ws.send(JSON.stringify(message));
    const take = (type, epoch) => until(() => {
        const index = messages.findIndex(message => message.type === type &&
            (epoch === undefined || message.epoch === epoch));
        return index < 0 ? null : messages.splice(index, 1)[0];
    }, `${type} epoch=${epoch}`);
    send(version === 1 ? { type: 'join', meeting: room } : {
        type: 'join', version, meeting: room, display_name: '<Alice & Bob>',
        capabilities: { mesh: true, sfu: true, topology_switch: true }
    });
    const joined = await take(version === 1 ? 'error' : 'joined');
    return { ws, messages, send, take, joined, id: joined.peer_id };
}

function topology(peer, type, message, reason) {
    peer.send({ type, version: 2, epoch: message.epoch, mode: message.mode,
        ...(reason ? { reason } : {}) });
}

async function close(peer) {
    if (peer.ws.readyState === WebSocket.CLOSED) return;
    const closed = once(peer.ws, 'close');
    peer.ws.close();
    await closed;
}

try {
    await mkdir(fixture);
    await writeFile(`${fixture}/index.html`, '<!doctype html><title>mrtc-static</title>');
    const reservation = net.createServer();
    reservation.listen(0, '127.0.0.1');
    await once(reservation, 'listening');
    const port = reservation.address().port;
    await new Promise(resolve => reservation.close(resolve));
    server = spawn(binary, ['--port', String(port), '--bind', '127.0.0.1', '--mode', 'auto',
        '--p2p-max', '2', '--downgrade-delay-ms', '100', '--web-root', await realpath(fixture)]);
    server.stderr.on('data', data => { log += data; });
    await until(async () => {
        try {
            const response = await fetch(`http://127.0.0.1:${port}/`,
                { signal: AbortSignal.timeout(200) });
            return response.status === 200 && (await response.text()).includes('mrtc-static');
        } catch { return false; }
    }, 'HTTP mount readiness');
    const url = `ws://127.0.0.1:${port}/ws`;
    const a = await peer(url, 'one');
    const b = await peer(url, 'one');
    const isolated = await peer(url, 'two');
    const legacy = await peer(url, 'one', 1);
    assert.match(legacy.joined.message, /version 2/);
    await close(legacy);
    assert.equal(a.joined.mode, 'mesh');
    assert.equal(b.joined.epoch, 1);
    assert.deepEqual(b.joined.peers, [a.id]);

    const first = a.id < b.id ? a : b;
    const second = first === a ? b : a;
    const session = `m:1:${first.id}:${second.id}`;
    second.send({ type: 'offer', version: 2, epoch: 1, session_id: session,
        to: first.id, sdp: 'v=0\r\n' });
    assert.match((await second.take('error')).message, /invalid session/);
    first.send({ type: 'offer', version: 2, epoch: 1, session_id: session,
        to: second.id, sdp: 'v=0\r\n' });
    const relayed = await second.take('offer', 1);
    assert.equal(relayed.from, first.id);
    assert.equal(relayed.session_id, session);
    second.send({ type: 'offer', version: 2, epoch: 1, session_id: session,
        to: first.id, sdp: 'v=0\r\n' });
    assert.equal((await first.take('offer', 1)).session_id, session);
    first.send({ type: 'candidate', version: 2, epoch: 1,
        session_id: 'm:1:forged:owner', to: second.id, candidate: '' });
    assert.match((await first.take('error')).message, /invalid session/);

    const c = await peer(url, 'one');
    const prepare2 = await a.take('topology_prepare', 2);
    assert.equal(prepare2.mode, 'sfu');
    assert.equal(prepare2.peers.length, 2);
    assert.equal(isolated.messages.some(message => message.type === 'topology_prepare'), false);
    const e = await peer(url, 'one');
    assert.match((await a.take('topology_abort', 2)).reason, /membership/);
    const prepare3 = await a.take('topology_prepare', 3);
    assert.equal(prepare3.peers.length, 3);
    await close(c);
    assert.match((await a.take('topology_abort', 3)).reason, /membership/);
    const prepare4 = await a.take('topology_prepare', 4);
    for (const participant of [a, b, e]) topology(participant, 'topology_ready', prepare4);
    const commit4 = await a.take('topology_commit', 4);
    const duringCommit = await peer(url, 'one');
    assert.equal(duringCommit.joined.mode, 'sfu');
    assert.equal(duringCommit.joined.epoch, 4);
    for (const participant of [a, b, e]) topology(participant, 'topology_ack', commit4);
    await close(duringCommit);
    await close(e);
    const prepare5 = await a.take('topology_prepare', 5);
    assert.equal(prepare5.mode, 'mesh');
    for (const participant of [a, b]) topology(participant, 'topology_ready', prepare5);
    const commit5 = await a.take('topology_commit', 5);
    for (const participant of [a, b]) topology(participant, 'topology_ack', commit5);
    topology(a, 'topology_request', commit5, 'ICE connectivity failed');
    // Requests name the desired fallback topology, not the current topology.
    assert.match((await a.take('error')).message, /invalid message/);
    a.send({ type: 'topology_request', version: 2, epoch: 5, mode: 'sfu', reason: 'ICE failed' });
    const prepare6 = await a.take('topology_prepare', 6);
    topology(b, 'topology_abort', prepare6, 'injected pending failure');
    assert.equal((await a.take('topology_abort', 6)).reason, 'injected pending failure');
    first.send({ type: 'offer', version: 2, epoch: 5,
        session_id: `m:5:${first.id}:${second.id}`, to: second.id, sdp: 'v=0\r\n' });
    assert.equal((await second.take('offer', 5)).from, first.id);
    a.send({ type: 'candidate', version: 2, epoch: 6, session_id: `p:6:${a.id}`,
        to: 'sfu', candidate: '' });
    assert.match((await a.take('error')).message, /invalid session/);
    a.send({ type: 'topology_request', version: 2, epoch: 5, mode: 'sfu', reason: 'ICE failed' });
    assert.match((await a.take('error')).message, /invalid topology/);
    assert.equal(isolated.messages.some(message => message.type === 'topology_prepare'), false);
    await Promise.all([close(a), close(b), close(isolated)]);
    console.log('HTTP mounts and real WebSocket room/epoch/membership/failure routing passed');
} finally {
    for (const socket of sockets) {
        if (socket.readyState !== WebSocket.CLOSED) socket.close();
    }
    if (server && server.exitCode === null) {
        const exited = once(server, 'exit');
        server.kill('SIGTERM');
        const stopped = await Promise.race([exited.then(() => true), delay(3000).then(() => false)]);
        if (!stopped) { server.kill('SIGKILL'); await exited; }
    }
    await rm(fixture, { recursive: true, force: true });
}
