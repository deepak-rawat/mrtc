import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { mkdir, open } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { fixture, build } from '../support/fixture.mjs';
import { NativeClient } from '../support/native-client.mjs';
import { active, media, exchange, delay } from '../support/native-checks.mjs';
import { integer, host, resources, provenance, percentile } from '../support/process.mjs';
import { decodedDelta, mediaObservations } from './benchmark-lib.mjs';

const seconds = integer(process.env, 'MRTC_BENCH_SECONDS', 60, 60, 600);
const warmupSeconds = integer(process.env, 'MRTC_BENCH_WARMUP_SECONDS', 15, 5, 120);
const peers = integer(process.env, 'MRTC_BENCH_PEERS', 8, 2, 8);
const sampleMs = integer(process.env, 'MRTC_BENCH_SAMPLE_MS', 2000, 1000, 10000);
assert(['linux', 'darwin'].includes(process.platform), 'Benchmark requires Linux or macOS ps');
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const path = resolve(root, process.env.MRTC_BENCH_REPORT || `build/benchmark-${Date.now()}.jsonl`);
await mkdir(dirname(path), { recursive: true });
const report = await open(path, 'wx', 0o600);
const record = value => report.write(JSON.stringify(value) + '\n');
const clients = [];
const runId = randomUUID();
const startedAt = new Date().toISOString();
let f, identity, machine, workload;
let closed = false;
try {
    machine = host();
    identity = await provenance(build);
    workload = { mode: 'sfu', rooms: 1, peers, seconds, warmupSeconds, sampleMs,
        video: 'synthetic VP8 (native defaults)', audio: 'synthetic Opus (native defaults)',
        sctp: 'sequential targeted controller-mediated round trip, peer 0 to every other peer per sample',
        cpu: 'ps cumulative owned-process CPU-time delta / elapsed wall time; 100% = one logical core',
        rss: 'peak sampled resident bytes, not allocation/live heap or leak measurement',
        throughput: 'sum of per-remote-source decoded video frames and audio samples per second',
        latency: 'application SCTP RTT including Node/stdio/controller scheduling; NOT media one-way/capture-to-render latency' };
    await record({ type: 'configuration', schema: 1, runId, startedAt, provenance: identity, host: machine, workload });
    f = await fixture('sfu');
    for (let i = 0; i < peers; i++) {
        const client = new NativeClient(f, `benchmark-${i}`);
        clients.push(client);
        await client.join(f.url, 'benchmark', `Benchmark ${i}`, { stun: '' });
    }
    await active(clients, 'sfu');
    await media(clients);
    // Association readiness and the helper's duplicate-check sleep are outside the timed probes.
    for (let i = 1; i < clients.length; i++) {
        await exchange(clients[0], [clients[i]], `warmup-out-${i}`);
        await exchange(clients[i], [clients[0]], `warmup-back-${i}`);
    }
    await delay(warmupSeconds * 1000);
    await media(clients);

    const prefix = `benchmark:${randomUUID()}:`;
    const expected = new Map();
    const seen = new Map();
    let deliveryFailure;
    const messageKey = (receiver, sender, text) => JSON.stringify([receiver, sender, text]);
    for (const client of clients) {
        let cursor = client.cursor;
        client.changed.on('change', () => {
            for (const event of client.events.filter(event => event.sequence > cursor)) {
                cursor = event.sequence;
                if (event.event !== 'message' || !event.text.startsWith(prefix)) continue;
                const key = messageKey(client.peerId, event.peer_id, event.text);
                seen.set(key, (seen.get(key) || 0) + 1);
                if (!expected.has(key) || seen.get(key) !== 1 || event.generation !== client.generation)
                    deliveryFailure ??= Error(`Unexpected/duplicate benchmark message on ${client.name}`);
            }
        });
    }
    async function roundTrip(receiver, sequence) {
        const sender = clients[0];
        const text = `${prefix}${sequence}:${receiver.peerId}`;
        expected.set(messageKey(receiver.peerId, sender.peerId, text), true);
        expected.set(messageKey(sender.peerId, receiver.peerId, text), true);
        const outgoingCursor = receiver.cursor, incomingCursor = sender.cursor;
        const started = performance.now();
        await sender.sendMessage(text, receiver.peerId);
        await receiver.expectMessage(sender.peerId, text, outgoingCursor);
        await receiver.sendMessage(text, sender.peerId);
        await sender.expectMessage(receiver.peerId, text, incomingCursor);
        const elapsedMs = performance.now() - started;
        if (deliveryFailure) throw deliveryFailure;
        return { receiver: receiver.name, elapsedMs };
    }
    const processes = new Map([['server', f.server], ...clients.map(client => [client.name, client.child])]);
    const snapshots = () => Promise.all(clients.map(client => client.snapshot()));
    let previousDecoded = await snapshots();
    const baselineDecoded = previousDecoded;
    const baselineResources = await resources(processes);
    const controllerStart = process.cpuUsage();
    const started = performance.now();
    const peak = { ...baselineResources };
    const latencies = [];
    let latestResources, latestDecoded, elapsedMs = 0, samples = 0;
    let serverPeakRssBytes = 0, clientsPeakRssBytes = 0, totalPeakRssBytes = 0;
    do {
        const probes = [];
        for (const receiver of clients.slice(1)) probes.push(await roundTrip(receiver, samples));
        latencies.push(...probes.map(probe => probe.elapsedMs));
        await delay(Math.max(0, Math.min((samples + 1) * sampleMs, seconds * 1000) -
            (performance.now() - started)));
        latestDecoded = await snapshots();
        const decoded = decodedDelta(previousDecoded, latestDecoded, clients.map(client => client.peerId));
        latestResources = await resources(processes);
        elapsedMs = performance.now() - started;
        for (const [name, value] of Object.entries(latestResources))
            peak[name] = { rssBytes: Math.max(peak[name].rssBytes, value.rssBytes) };
        const serverRss = latestResources.server.rssBytes;
        const clientsRss = Object.entries(latestResources).filter(([name]) => name !== 'server')
            .reduce((sum, [, value]) => sum + value.rssBytes, 0);
        serverPeakRssBytes = Math.max(serverPeakRssBytes, baselineResources.server.rssBytes, serverRss);
        clientsPeakRssBytes = Math.max(clientsPeakRssBytes, clientsRss,
            Object.entries(baselineResources).filter(([name]) => name !== 'server')
                .reduce((sum, [, value]) => sum + value.rssBytes, 0));
        totalPeakRssBytes = Math.max(totalPeakRssBytes, serverRss + clientsRss,
            Object.values(baselineResources).reduce((sum, value) => sum + value.rssBytes, 0));
        if (deliveryFailure) throw deliveryFailure;
        await record({ type: 'sample', elapsedMs, resources: latestResources, decoded, probes,
            hostLoadAverage: host().loadAverage });
        previousDecoded = latestDecoded;
        samples++;
    } while (elapsedMs < seconds * 1000);
    const controllerCpu = process.cpuUsage(controllerStart);
    const totals = decodedDelta(baselineDecoded, latestDecoded, clients.map(client => client.peerId));
    const cpu = Object.fromEntries(Object.entries(latestResources).map(([name, value]) => {
        const seconds = value.cpuSeconds - baselineResources[name].cpuSeconds;
        assert(seconds >= 0, `CPU time moved backwards for ${name}`);
        return [name, seconds / (elapsedMs / 1000) * 100];
    }));
    const clientsCpuPercent = Object.entries(cpu).filter(([name]) => name !== 'server')
        .reduce((sum, [, value]) => sum + value, 0);
    const metrics = { serverCpuPercent: cpu.server, clientsCpuPercent,
        totalCpuPercent: cpu.server + clientsCpuPercent,
        controllerCpuPercent: (controllerCpu.user + controllerCpu.system) / (elapsedMs * 1000) * 100,
        serverPeakRssBytes, clientsPeakRssBytes, totalPeakRssBytes,
        decodedVideoFramesPerSecond: totals.videoFrames / (elapsedMs / 1000),
        decodedAudioSamplesPerSecond: totals.audioSamples / (elapsedMs / 1000),
        applicationSctpRttP50Ms: percentile(latencies, 0.5),
        applicationSctpRttP95Ms: percentile(latencies, 0.95),
        applicationSctpRttMaxMs: Math.max(...latencies) };
    for (const [name, value] of Object.entries(cpu)) {
        metrics[`process.${name}.cpuPercent`] = value;
        metrics[`process.${name}.peakRssBytes`] = peak[name].rssBytes;
    }
    await Promise.all(clients.map(client => client.quit()));
    if (deliveryFailure) throw deliveryFailure;
    assert.equal(seen.size, expected.size, 'Missing benchmark messages');
    for (const key of expected.keys()) assert.equal(seen.get(key), 1, 'Expected exactly one message');
    assert.equal(f.server.exitCode, null, 'Server exited before controlled shutdown');
    await f.close();
    closed = true;
    assert.equal((await f.server.done).code, 0, 'Server did not shut down cleanly');
    // Detect a concurrent rebuild/source edit rather than silently mixing identities.
    assert.deepEqual(await provenance(build), identity, 'Build/source changed during benchmark');
    await record({ type: 'benchmark-result', schema: 1, runId, startedAt, ok: true, provenance: identity, host: machine,
        hostAfter: host(), workload, elapsedMs, samples, exactMessages: seen.size, metrics,
        mediaObservations: { before: mediaObservations(baselineDecoded), after: mediaObservations(latestDecoded) },
        perProcessCpuPercent: cpu, perProcessPeakRssBytes: Object.fromEntries(
            Object.entries(peak).map(([name, value]) => [name, value.rssBytes])) });
    console.log(`PASS bounded native benchmark: ${path}`);
} catch (error) {
    await record({ type: 'benchmark-result', schema: 1, runId, startedAt, ok: false, provenance: identity,
        host: machine, workload, error: error.stack || String(error) });
    if (f) await f.artifacts(error);
    throw error;
} finally {
    if (f && !closed) await f.close();
    await report.close();
}
