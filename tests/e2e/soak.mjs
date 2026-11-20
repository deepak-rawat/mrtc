import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { open, mkdir, readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { fixture, build } from '../support/fixture.mjs';
import { NativeClient } from '../support/native-client.mjs';
import { active, media, exchange, delay } from '../support/native-checks.mjs';

const execute = promisify(execFile);
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
function number(name, fallback, minimum, maximum) {
    const text = process.env[name] ?? String(fallback);
    const value = Number(text);
    if (!/^[0-9]+$/.test(text) || !Number.isSafeInteger(value) || value < minimum || value > maximum)
        throw Error(`${name} must be ${minimum}..${maximum}`);
    return value;
}
const seconds = number('MRTC_SOAK_SECONDS', 300, 10, 259200);
const peers = number('MRTC_SOAK_PEERS', 8, 2, 8);
const rooms = number('MRTC_SOAK_ROOMS', 1, 1, 32);
const sampleMs = number('MRTC_SOAK_SAMPLE_MS', 5000, 1000, 60000);
const serverGrowth = number('MRTC_SOAK_SERVER_GROWTH_MIB', 64, 1, 4096) * 1024 ** 2;
const clientGrowth = number('MRTC_SOAK_CLIENT_GROWTH_MIB', 16, 1, 4096) * 1024 ** 2;
assert(seconds * 1000 >= sampleMs * 3, 'Duration must allow at least three samples');
assert(['linux', 'darwin'].includes(process.platform), 'RSS soak qualification requires Linux or macOS');

async function rss(child) {
    assert(Number.isSafeInteger(child.pid) && child.pid > 0, 'Missing owned child PID');
    let text;
    if (process.platform === 'linux') {
        const status = await readFile(`/proc/${child.pid}/status`, 'utf8');
        text = /^VmRSS:\s+([0-9]+)\s+kB$/m.exec(status)?.[1];
    } else {
        text = (await execute('ps', ['-o', 'rss=', '-p', String(child.pid)],
            { timeout: 3000, maxBuffer: 4096 })).stdout.trim();
    }
    assert(text && /^[0-9]+$/.test(text), `Unavailable RSS for owned child ${child.pid}`);
    return Number(text) * 1024;
}

const path = resolve(root, process.env.MRTC_SOAK_REPORT || `build/soak-${Date.now()}.jsonl`);
await mkdir(dirname(path), { recursive: true });
const report = await open(path, 'wx', 0o600);
const record = value => report.write(JSON.stringify(value) + '\n');
const clients = [];
let f;
let closed = false;
try {
    const commit = (await execute('git', ['rev-parse', 'HEAD'], { cwd: root })).stdout.trim();
    const dirty = Boolean((await execute('git', ['status', '--porcelain'], { cwd: root })).stdout);
    await record({ type: 'configuration', commit, dirty, build, platform: process.platform,
        seconds, peers, rooms, sampleMs, serverGrowth, clientGrowthPerProcess: clientGrowth });
    f = await fixture('sfu');
    const groups = [];
    for (let room = 0; room < rooms; room++) {
        const group = [];
        for (let participant = 0; participant < peers; participant++) {
            const client = new NativeClient(f, `soak-${room}-${participant}`);
            clients.push(client);
            group.push(client);
            await client.join(f.url, `soak-${room}`, `Participant ${participant}`, { stun: '' });
        }
        await active(group, 'sfu');
        await media(group);
        groups.push(group);
    }
    const started = performance.now();
    const warmupMs = Math.min(60000, seconds * 1000 / 3);
    const baseline = new Map();
    const maxima = new Map();
    let samples = 0;
    do {
        await Promise.all(groups.map(async (group, room) => {
            await media(group);
            await exchange(group[0], group.slice(1), `soak-${room}-${samples}`);
        }));
        const elapsedMs = performance.now() - started;
        const memory = new Map();
        memory.set('server', await rss(f.server));
        for (const client of clients) memory.set(client.name, await rss(client.child));
        const growth = {};
        for (const [name, bytes] of memory) {
            if (elapsedMs >= warmupMs) {
                if (!baseline.has(name)) baseline.set(name, bytes);
                maxima.set(name, Math.max(maxima.get(name) || 0, bytes - baseline.get(name)));
                growth[name] = maxima.get(name);
            }
        }
        await record({ type: 'sample', elapsedMs, rss: Object.fromEntries(memory), growth });
        for (const [name, bytes] of maxima)
            assert(bytes <= (name === 'server' ? serverGrowth : clientGrowth),
                `${name} exceeded post-warmup RSS growth allowance: ${bytes} bytes`);
        samples++;
        const remaining = seconds * 1000 - (performance.now() - started);
        if (remaining <= 0) break;
        await delay(Math.min(sampleMs, remaining));
    } while (true);
    assert(samples >= 3 && baseline.size === clients.length + 1, 'Insufficient steady-state evidence');
    await Promise.all(clients.map(client => client.quit()));
    assert.equal(f.server.exitCode, null, 'Server exited before controlled shutdown');
    await f.close();
    closed = true;
    assert.equal((await f.server.done).code, 0, 'Server did not shut down cleanly');
    await record({ type: 'result', ok: true, elapsedMs: performance.now() - started, samples,
        maximumPostWarmupGrowth: Object.fromEntries(maxima) });
    console.log(`PASS ${rooms} room(s), ${peers} peers each, ${seconds}s: fresh decoded media/SCTP and bounded sampled RSS`);
    console.log(`Report: ${path}`);
} catch (error) {
    await record({ type: 'result', ok: false, error: error.stack || String(error) });
    if (f) await f.artifacts(error);
    throw error;
} finally {
    if (f && !closed) await f.close();
    await report.close();
}
