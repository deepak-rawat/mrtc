import assert from 'node:assert/strict';
import { fixture, until, stats, decoded, chat } from './fixture.mjs';
import { NativeClient } from '../support/native-client.mjs';
import { media } from '../support/native-checks.mjs';

async function nativeReply(native, page, peerId, text) {
    const cursor = native.cursor;
    await chat(page, text);
    await native.expectMessage(peerId, text, cursor);
    await native.sendMessage(`reply:${text}`, peerId);
    await until(async () => (await stats(page)).messages.includes(`reply:${text}`), `native reply ${text}`);
}

async function selectedAudioOnce(page, peers) {
    const rendered = await page.locator('#remotes video').evaluateAll(videos => videos.map(video => ({
        peer: video.closest('figure').dataset.peerId,
        audio: video.srcObject?.getAudioTracks().length ?? 0,
        muted: video.muted,
    })));
    assert.deepEqual(rendered.map(tile => tile.peer).sort(), [...peers].sort());
    for (const tile of rendered) assert.equal(tile.audio, 1, `exactly one selected audio track for ${tile.peer}`);
    assert(await page.locator('video[hidden]').evaluateAll(videos => videos.every(video => video.muted)));
}
async function switchTo(page, mode, oldEpoch) {
    await until(async () => {
        const value = await stats(page);
        assert.deepEqual(value.errors, []);
        return value.mode === mode && value.epoch > oldEpoch && !value.pendingEpoch;
    }, `coordinator commits ${mode}`, 45000);
    return (await stats(page)).epoch;
}
let f;
try {
    f = await fixture('auto', ['--p2p-max', '2',
        '--downgrade-delay-ms', process.env.MRTC_DOWNGRADE_DELAY_MS || '250']);
    const native = new NativeClient(f, 'native');
    await native.join(f.url, 'migration', 'Native', { stun: '' });
    const first = await f.page('Browser Alice', 'migration');
    const initial = await stats(first);
    assert.equal(initial.mode, 'mesh');
    await until(async () => Object.keys((await stats(first)).sources).length === 1, 'native mesh source');
    const nativeId = native.peerId;
    await decoded(first, [nativeId]);
    await media([native], [{ peerId: initial.id }]);
    const nativeTile = await first.locator(`[data-peer-id="${nativeId}"]`).elementHandle();
    const second = await f.page('Browser Bob', 'migration');
    const secondId = (await stats(second)).id;
    const firstId = initial.id;
    const sfuEpoch = await switchTo(first, 'sfu', initial.epoch);
    await switchTo(second, 'sfu', initial.epoch);
    await decoded(first, [nativeId, secondId], sfuEpoch);
    await decoded(second, [nativeId, firstId], sfuEpoch);
    await selectedAudioOnce(first, [nativeId, secondId]);
    assert(await nativeTile.evaluate(node => node === document.querySelector(`[data-peer-id="${node.dataset.peerId}"]`)));
    const cursor = native.cursor;
    await chat(first, 'browser-datachannel');
    await until(async () => (await stats(second)).messages.includes('browser-datachannel'), 'new SFU SCTP association');
    await native.expectMessage(firstId, 'browser-datachannel', cursor);
    await native.sendMessage('native-datachannel');
    await until(async () => (await stats(first)).messages.includes('native-datachannel'), 'native SFU SCTP reply');
    await native.event('topology', event => event.mode === 'sfu' && event.status === 'active' && event.epoch === sfuEpoch);
    await media([native], [{ peerId: firstId }, { peerId: secondId }]);
    await second.locator('#leave').click();
    const meshEpoch = await switchTo(first, 'mesh', sfuEpoch);
    await decoded(first, [nativeId], meshEpoch);
    await media([native], [{ peerId: firstId }]);
    await selectedAudioOnce(first, [nativeId]);
    assert(await nativeTile.evaluate(node => node === document.querySelector(`[data-peer-id="${node.dataset.peerId}"]`)));
    await first.locator('#mic').click();
    await first.locator('#camera').click();
    await nativeReply(native, first, firstId, 'mesh-recommit');
    await native.event('topology', event => event.mode === 'mesh' && event.status === 'active' && event.epoch === meshEpoch);
    assert((await stats(first)).capture.every(track => track.state === 'live'));
    assert.deepEqual((await stats(first)).errors, []);
    await native.snapshot();
    await native.quit();
    console.log('PASS actual UI + native automatic 2→3→2, per-source decode, stable tiles, single selected audio, SCTP migration and capture lifetime');
} catch (error) {
    console.error(error);
    await f?.artifacts(error);
    if (f) for (const [name, text] of f.logs) console.error(`--- ${name} (tail) ---\n${text.slice(-4000)}`);
    process.exitCode = 1;
} finally { await f?.close(); }

// Hold one real client's readiness to force the coordinator's deadline/abort path.
let failure;
try {
    failure = await fixture('auto', ['--p2p-max', '2', '--downgrade-delay-ms', '250']);
    const native = new NativeClient(failure, 'abort-native');
    await native.join(failure.url, 'abort', 'Abort survivor', { stun: '' });
    const first = await failure.page('Alice', 'abort');
    const firstId = (await stats(first)).id;
    await until(async () => Object.keys((await stats(first)).sources).length === 1, 'native abort source');
    const nativeId = native.peerId;
    await decoded(first, [nativeId]);
    const before = await stats(first);
    const third = await failure.browser.contexts()[0].newPage();
    let replay;
    let replayPrepare;
    let replayCandidate;
    let preparedEpoch;
    let client;
    await third.routeWebSocket('**/ws', socket => {
        client = socket;
        const remote = socket.connectToServer();
        remote.onMessage(message => {
            if (typeof message === 'string') {
                const value = JSON.parse(message);
                if (value.type === 'topology_prepare') {
                    replayPrepare = message;
                    preparedEpoch = value.epoch;
                }
                if (value.type === 'offer' && value.epoch === preparedEpoch) replay = message;
                if (value.type === 'candidate' && value.epoch === preparedEpoch) replayCandidate = message;
            }
            socket.send(message);
        });
        socket.onMessage(message => {
            if (typeof message === 'string' && JSON.parse(message).type === 'topology_ready') return;
            remote.send(message);
        });
    });
    await third.goto(failure.http);
    await third.locator('#name').fill('Held readiness');
    await third.locator('#room').fill('abort');
    await third.locator('#synthetic').check();
    await third.locator('#join').click();
    await until(async () => (await stats(first)).pendingEpoch, 'real pending graph');
    await nativeReply(native, first, firstId, 'during-prepare');
    await until(async () => (await stats(first)).events.some(event => event.startsWith('topology_abort:')),
        'coordinator timeout abort', 30000);
    const after = await stats(first);
    assert.equal(after.epoch, before.epoch);
    assert.equal(after.mode, 'mesh');
    assert(!after.pendingEpoch);
    assert(after.errors.some(error => error.startsWith('Switch aborted:')));
    assert.equal(after.errors.length, 1, 'only the deliberate timeout abort is reported');
    const abortReason = after.errors[0].slice('Switch aborted: '.length);
    assert.equal(abortReason, 'topology preparation timed out');
    await native.event('topology', event => event.status === 'error' && event.reason === abortReason);
    await nativeReply(native, first, firstId, 'after-abort');
    await until(async () => (await stats(first)).sources[nativeId].video > before.sources[nativeId].video + 2,
        'native source still decodes after abort');
    if (!replay || !replayPrepare || !replayCandidate || !client)
        throw Error('No actual pending SDP/candidate/prepare captured for stale-epoch replay');
    const thirdBefore = await stats(third);
    client.send(replay);
    await until(async () => (await stats(third)).events.includes('discard:stale-epoch'), 'stale aborted-epoch SDP discarded');
    const discards = (await stats(third)).events.filter(event => event === 'discard:stale-epoch').length;
    client.send(replayCandidate);
    await until(async () => (await stats(third)).events.filter(event =>
        event === 'discard:stale-epoch').length > discards, 'stale aborted-epoch candidate discarded');
    client.send(replayPrepare);
    await until(async () => (await stats(third)).events.includes('discard:stale-prepare'), 'aborted epoch cannot resurrect');
    assert.equal((await stats(third)).connections.length, thirdBefore.connections.length);
    assert.equal((await stats(first)).id, firstId);
    for (const page of [first, third])
        assert((await stats(page)).errors.every(error => error === `Switch aborted: ${abortReason}`), 'no unrelated browser errors');
    await native.snapshot();
    await native.quit();
    console.log('PASS real coordinator native/browser timeout/abort, old-path media/data continuity and stale SDP/prepare isolation');
} catch (error) {
    console.error(error);
    await failure?.artifacts(error);
    if (failure) for (const [name, text] of failure.logs) console.error(`--- ${name} (tail) ---\n${text.slice(-4000)}`);
    process.exitCode = 1;
} finally { await failure?.close(); }
