import { spawn } from 'node:child_process';
import { createServer } from 'node:net';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { mkdir, writeFile } from 'node:fs/promises';
import { randomUUID } from 'node:crypto';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
export const build = resolve(root, process.env.MRTC_BUILD_DIR || 'build');
export async function until(predicate, label, timeout = 30000) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        if (await predicate()) return;
        await new Promise(resolve => setTimeout(resolve, 100));
    }
    throw Error(`timeout waiting for ${label}`);
}
export async function fixture(mode, extra = [], { webRoot } = {}) {
    const reservation = createServer();
    await new Promise((resolve, reject) => {
        reservation.once('error', reject);
        reservation.listen(0, '127.0.0.1', resolve);
    });
    const port = reservation.address().port;
    await new Promise(resolve => reservation.close(resolve));
    const children = [];
    const logs = new Map();
    const nativeClients = new Map();
    function record(name, data) {
        logs.set(name, ((logs.get(name) || '') + data).slice(-200000));
    }
    function run(name, binary, args) {
        const executable = process.platform === 'win32' && !binary.endsWith('.exe') ? `${binary}.exe` : binary;
        const child = spawn(resolve(build, executable), args, { stdio: ['pipe', 'pipe', 'pipe'] });
        logs.set(name, '');
        child.done = new Promise(resolve => {
            child.once('error', error => {
                child.spawnFailure = error;
                record(name, String(error));
                resolve({ code: -1 });
            });
            child.once('close', (code, signal) => resolve({ code, signal }));
        });
        child.stdout.on('data', data => record(name, data));
        child.stderr.on('data', data => { record(name, data); record(`${name}:stderr`, data); });
        children.push(child);
        return child;
    }
    const url = `ws://127.0.0.1:${port}/ws`;
    const http = `http://127.0.0.1:${port}`;
    const server = run('server', 'app/server/mrtc_server',
        ['--port', String(port), '--bind', '127.0.0.1', '--mode', mode,
            ...(webRoot ? ['--web-root', resolve(root, webRoot)] : []),
            ...extra]);
    async function artifacts(error, extraSnapshots = {}) {
        const destination = resolve(process.env.MRTC_TEST_ARTIFACTS || resolve(root, 'build/e2e-failures'),
            `${Date.now()}-${mode}-${randomUUID().slice(0, 8)}`);
        const snapshots = { ...extraSnapshots };
        for (const [name, client] of nativeClients) {
            try { snapshots[name] = (await client.command('stats', {}, 1000)).stats; }
            catch (error) {
                snapshots[name] = { unavailable: String(error), detail: error.detail, last_known: client.lastSnapshot };
            }
        }
        await mkdir(destination, { recursive: true });
        await writeFile(resolve(destination, 'failure.json'), JSON.stringify({
            mode, error: error.stack || String(error), detail: error.detail, snapshots,
        }, null, 2));
        for (const [name, text] of logs)
            await writeFile(resolve(destination, `${name.replace(/[^a-zA-Z0-9._-]/g, '_')}.log`), text);
        console.error(`Failure artifacts: ${destination}`);
    }
    async function close() {
        for (const child of children)
            if (child.exitCode === null && child.signalCode === null) child.kill('SIGTERM');
        await Promise.race([Promise.all(children.map(child => child.done)),
            new Promise(resolve => setTimeout(resolve, 2000))]);
        for (const child of children)
            if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
        await Promise.all(children.map(child => child.done));
    }
    try {
        await until(async () => {
            if (server.spawnFailure || server.exitCode !== null)
                throw Error(`server exited: ${logs.get('server')}`);
            try {
                const response = await fetch(http, { signal: AbortSignal.timeout(1000) });
                return webRoot ? response.ok : response.status > 0;
            } catch { return false; }
        }, 'real server HTTP readiness');
        return { url, http, run, logs, close, artifacts, record, nativeClients, server };
    } catch (error) {
        await artifacts(error).catch(error => console.error(`Artifact write failed: ${error}`));
        await close();
        throw Error(`${error}\n${[...logs].map(([name, text]) => `${name}: ${text}`).join('\n')}`);
    }
}
