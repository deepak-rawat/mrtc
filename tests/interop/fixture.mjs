import { spawn } from 'node:child_process';
import { createServer } from 'node:net';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { randomUUID } from 'node:crypto';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
process.env.PLAYWRIGHT_BROWSERS_PATH ||= resolve(root, 'tests/interop/node_modules/.cache/ms-playwright');
export const build = resolve(root, process.env.MRTC_BUILD_DIR || 'build/reorg-web');
export async function until(predicate, label, timeout = 30000) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        if (await predicate()) return;
        await new Promise(resolve => setTimeout(resolve, 100));
    }
    throw Error(`timeout waiting for ${label}`);
}
export async function fixture(mode, extra = [], { withBrowser = true } = {}) {
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
    let browser;
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
            ...(withBrowser ? ['--web-root', resolve(root, process.env.MRTC_WEB_ROOT || 'app/web/dist')] : []),
            ...extra]);
    async function artifacts(error) {
        const destination = resolve(process.env.MRTC_TEST_ARTIFACTS || resolve(root, 'build/interop-failures'),
            `${Date.now()}-${mode}-${randomUUID().slice(0, 8)}`);
        const snapshots = {};
        for (const [name, client] of nativeClients) {
            try { snapshots[name] = (await client.command('stats', {}, 1000)).stats; }
            catch (error) {
                snapshots[name] = { unavailable: String(error), detail: error.detail, last_known: client.lastSnapshot };
            }
        }
        if (browser) for (const [index, page] of browser.contexts().flatMap(context => context.pages()).entries()) {
            let timer;
            try {
                snapshots[`browser-${index}`] = await Promise.race([
                    page.evaluate(() => window.mrtc?.stats()),
                    new Promise((_, reject) => {
                        timer = setTimeout(() => reject(Error('browser snapshot deadline')), 2000);
                    }),
                ]);
            }
            catch (error) { snapshots[`browser-${index}`] = { unavailable: String(error) }; }
            finally { clearTimeout(timer); }
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
        if (browser) await browser.close();
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
                return withBrowser ? response.ok : response.status > 0;
            } catch { return false; }
        }, 'real server HTTP readiness');
        if (withBrowser) {
            const engine = process.env.MRTC_BROWSER || 'chromium';
            if (!['chromium', 'firefox', 'webkit'].includes(engine))
                throw Error(`Unsupported MRTC_BROWSER: ${engine}`);
            if (engine !== 'chromium' && process.env.MRTC_CHROME_PATH)
                throw Error('MRTC_CHROME_PATH only applies to Chromium');
            const engines = await import('playwright');
            browser = await engines[engine].launch({
            headless: true,
            ...(process.env.MRTC_CHROME_PATH ? { executablePath: process.env.MRTC_CHROME_PATH } : {}),
            args: engine === 'chromium' ? ['--no-sandbox', '--mute-audio'] : [],
            });
        }
        const context = await browser?.newContext();
        async function page(name, meeting) {
            if (!context) throw Error('This fixture has no browser');
            const page = await context.newPage();
            page.on('pageerror', error => logs.set(`page:${name}`, String(error)));
            await page.routeWebSocket('**/ws', socket => {
                const remote = socket.connectToServer();
                socket.onMessage(message => {
                    if (typeof message === 'string') {
                        const value = JSON.parse(message);
                        if (value.type === 'offer') logs.set(`sdp:${name}`, message);
                        if (value.type === 'candidate' && value.candidate)
                            logs.set(`candidate:${name}:${value.session_id}`, message);
                    }
                    remote.send(message);
                });
            });
            await page.goto(http);
            await page.locator('#name').fill(name);
            await page.locator('#room').fill(meeting);
            await page.locator('#synthetic').check();
            await page.locator('#join').click();
            await until(async () => (await page.evaluate(() => window.mrtc.stats())).id, `${name} UI join`);
            return page;
        }
        return { url, http, run, page, logs, close, browser, artifacts, record, nativeClients, server };
    } catch (error) {
        await artifacts(error).catch(error => console.error(`Artifact write failed: ${error}`));
        await close();
        throw Error(`${error}\n${[...logs].map(([name, text]) => `${name}: ${text}`).join('\n')}`);
    }
}
export const stats = page => page.evaluate(() => window.mrtc.stats());
export async function decoded(page, peers, epoch) {
    await until(async () => {
        const value = await stats(page);
        assert.deepEqual(value.errors, []);
        return (!epoch || value.epoch === epoch) && peers.every(id =>
            value.sources[id]?.video > 2 && value.sources[id]?.audio > 0);
    }, `per-source decoded audio/video: ${peers.join(',')}`, 45000);
}
export async function chat(page, text) {
    await page.locator('#message').fill(text);
    await page.locator('#chat-form button').click();
}
