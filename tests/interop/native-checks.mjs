import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { until } from './fixture.mjs';

export const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
export const peer = (snapshot, id) => snapshot.peers.find(value => value.peer_id === id);

export async function active(clients, mode, roster = clients) {
    await until(async () => {
        const values = await Promise.all(clients.map(client => client.snapshot()));
        return values.every((value, i) => value.mode === mode && value.status === 'active' &&
            value.peers.length === roster.length - 1 &&
            roster.filter(client => client !== clients[i]).every(client => peer(value, client.peerId)));
    }, `native active ${mode} roster`, 45000);
    const values = await Promise.all(clients.map(client => client.snapshot()));
    assert.equal(new Set(values.map(value => value.epoch)).size, 1, 'same committed epoch');
    return values[0].epoch;
}

export async function media(clients, sources = clients, { video = true, audio = true } = {}) {
    const baselines = await Promise.all(clients.map(client => client.snapshot()));
    await until(async () => {
        const values = await Promise.all(clients.map(client => client.snapshot()));
        return values.every((value, i) => sources.filter(source => source !== clients[i]).every(source => {
            const before = peer(baselines[i], source.peerId);
            const now = peer(value, source.peerId);
            const decoded = (label, counter, minimum) => {
                const oldSource = baselines[i].sources.find(entry => entry.peer_id === source.peerId && entry.label === label);
                const newSource = value.sources.find(entry => entry.peer_id === source.peerId && entry.label === label);
                return newSource && newSource.epoch === value.epoch && newSource.generation === value.generation &&
                    newSource[counter] > (oldSource?.[counter] ?? 0) + minimum;
            };
            return before && now && value.status === 'active' &&
                value.epoch === baselines[i].epoch && now.epoch === value.epoch &&
                (!video || now.video_frames > before.video_frames + 2) &&
                (!audio || now.audio_samples > before.audio_samples) &&
                (!video || decoded('camera', 'video_frames', 2)) &&
                (!audio || decoded('mic', 'audio_samples', 0));
        }));
    }, 'fresh per-source native decoded media deltas in active epoch', 45000);
}

export async function exchange(sender, receivers, text) {
    const snapshot = await sender.snapshot();
    sender.readyAssociations ??= new Set();
    for (const receiver of receivers) {
        const key = `${sender.generation}:${snapshot.epoch}:${receiver.peerId}:${receiver.generation}`;
        if (sender.readyAssociations.has(key)) continue;
        const cursor = receiver.cursor;
        const readiness = `association-ready:${randomUUID()}`;
        // A broadcast can partially succeed. Retry only a targeted send that
        // explicitly failed before accepting data, then broadcast exactly once.
        await until(async () => {
            try { await sender.sendMessage(readiness, receiver.peerId); return true; }
            catch (error) {
                if (error.detail?.ok === false && error.detail.code === -5 &&
                    error.detail.error === 'peer not ready or not open') return false;
                throw error;
            }
        }, 'SCTP targeted send readiness', 10000);
        await receiver.expectMessage(sender.peerId, readiness, cursor);
        sender.readyAssociations.add(key);
        if (sender.readyAssociations.size > 64) sender.readyAssociations.delete(sender.readyAssociations.values().next().value);
    }
    const offsets = receivers.map(client => client.cursor);
    await sender.sendMessage(text);
    await Promise.all(receivers.map((client, i) => client.expectMessage(sender.peerId, text, offsets[i])));
    await delay(250);
    receivers.forEach((client, i) => assert.equal(client.events.filter(event =>
        event.sequence > offsets[i] && event.event === 'message' && event.peer_id === sender.peerId &&
        event.text === text).length, 1, `${client.name}: exactly one chat delivery`));
}

export async function absent(clients, departedId) {
    await until(async () => (await Promise.all(clients.map(client => client.snapshot())))
        .every(value => !peer(value, departedId) &&
            (value.sources ?? []).every(source => source.peer_id !== departedId)), `departed roster/source ${departedId}`);
}
