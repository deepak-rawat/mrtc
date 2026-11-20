import assert from 'node:assert/strict';
import { fixture } from '../support/fixture.mjs';
import { NativeClient } from '../support/native-client.mjs';
import { active, media, exchange, absent, delay, peer } from '../support/native-checks.mjs';

async function mutedSource(sender, receiver, source) {
    await sender.mute(source, true);
    await delay(1000);
    const before = peer(await receiver.snapshot(), sender.peerId);
    await delay(700);
    const after = peer(await receiver.snapshot(), sender.peerId);
    const counter = source === 'video' ? 'video_frames' : 'audio_samples';
    const other = source === 'video' ? 'audio_samples' : 'video_frames';
    assert.equal(after[counter], before[counter], `${source} stops after drain`);
    assert(after[other] > before[other], `muting ${source} preserves other source`);
    await sender.mute(source, false);
    await media([receiver], [sender]);
}

async function receiveOnly(sender, receiver, mode) {
    await active([sender, receiver], mode);
    await media([receiver], [sender]);
    await delay(1000);
    const before = peer(await sender.snapshot(), receiver.peerId);
    await delay(700);
    const after = peer(await sender.snapshot(), receiver.peerId);
    assert.equal(after.video_frames, before.video_frames, 'receive-only sends no video');
    assert.equal(after.audio_samples, before.audio_samples, 'receive-only sends no audio');
    await exchange(receiver, [sender], `receive-only-chat-${receiver.generation}`);
    await exchange(sender, [receiver], `receive-only-reply-${receiver.generation}`);
}

async function scenario(mode, threshold) {
    let f;
    try {
        f = await fixture(mode, threshold ? ['--p2p-max', String(threshold), '--downgrade-delay-ms', '250'] : []);
        const clients = [];
        async function add(name, options) {
            const client = new NativeClient(f, name);
            await client.join(f.url, 'native', name, { stun: '', ...options });
            clients.push(client);
            return client;
        }
        const a = await add('Alice');
        const b = await add('Bob');
        const initialMode = mode === 'auto' ? 'mesh' : mode;
        const initialEpoch = await active(clients, initialMode);
        await media(clients);
        await exchange(a, [b], `${mode}-${threshold}-initial`);
        await exchange(b, [a], `${mode}-${threshold}-reply`);
        if (mode === 'auto') {
            while (clients.length < threshold) await add(`Peer ${clients.length + 1}`);
            await active(clients, 'mesh');
            await media(clients);
            const extra = await add('Threshold crossing');
            const sfuEpoch = await active(clients, 'sfu');
            assert(sfuEpoch > initialEpoch);
            await media(clients);
            await exchange(a, clients.slice(1), `sfu-${threshold}`);
            const departed = extra.peerId;
            await extra.leave();
            clients.pop();
            await absent(clients, departed);
            const meshEpoch = await active(clients, 'mesh');
            assert(meshEpoch > sfuEpoch);
            await media(clients);
            await exchange(b, clients.filter(client => client !== b), `mesh-${threshold}`);
            await extra.quit();
        } else {
            await mutedSource(a, b, 'video');
            await mutedSource(a, b, 'audio');
            const oldId = b.peerId;
            const oldGeneration = b.generation;
            const leftCursor = a.cursor;
            await b.leave();
            await a.event('peer_left', event => event.peer_id === oldId, leftCursor);
            await absent([a], oldId);
            await b.join(f.url, 'native', 'Bob rejoined', { video: 'none', audio: 'none', stun: '' });
            assert(b.generation > oldGeneration, 'rejoin increments generation');
            await receiveOnly(a, b, mode);
            if (mode === 'mesh') {
                const directions = new Set([b.peerId < a.peerId]);
                // Peer IDs are assigned by the real server: observe both offerer
                // orders rather than assuming joining first means offering first.
                for (let attempt = 0; directions.size < 2 && attempt < 24; attempt++) {
                    await a.leave();
                    await b.leave();
                    const joinSender = () => a.join(f.url, 'native', 'Sender', { stun: '' });
                    const joinReceiver = () => b.join(f.url, 'native', 'Receiver',
                        { video: 'none', audio: 'none', stun: '' });
                    if (attempt % 2 === 0) { await joinReceiver(); await joinSender(); }
                    else { await joinSender(); await joinReceiver(); }
                    const direction = b.peerId < a.peerId;
                    await receiveOnly(a, b, mode);
                    directions.add(direction);
                }
                assert.equal(directions.size, 2, 'receive-only mesh decodes as both offerer and answerer');
            }
            await a.leave();
            await b.leave();
            await a.join(f.url, 'data-only', 'Data Alice', { video: 'none', audio: 'none', data_only: true, stun: '' });
            await b.join(f.url, 'data-only', 'Data Bob', { video: 'none', audio: 'none', data_only: true, stun: '' });
            await active(clients, mode);
            await exchange(a, [b], 'data-only-ready');
            await exchange(b, [a], 'data-only-reply');
            for (const client of clients) for (const remote of (await client.snapshot()).peers) {
                assert.equal(remote.video_frames, 0);
                assert.equal(remote.audio_samples, 0);
            }
        }
        for (const client of clients) await client.quit();
        console.log(`PASS native ${mode}${threshold ? ` ${threshold}→${threshold + 1}→${threshold}` : ''}: decoded media deltas, SCTP, roster and lifecycle`);
    } catch (error) {
        console.error(error);
        await f?.artifacts(error);
        if (f) for (const [name, text] of f.logs) console.error(`--- ${name} (tail) ---\n${text.slice(-4000)}`);
        process.exitCode = 1;
    } finally { await f?.close(); }
}

for (const [mode, threshold] of [['mesh'], ['sfu'], ['auto', 2], ['auto', 4]])
    await scenario(mode, threshold);
