import { spawn } from 'node:child_process';
import { createServer } from 'node:net';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';

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
    let browser;
    function run(name, binary, args) {
        const child = spawn(resolve(build, binary), args, { stdio: ['pipe', 'pipe', 'pipe'] });
        logs.set(name, '');
        child.done = new Promise(resolve => {
            child.once('error', error => { logs.set(name, logs.get(name) + error); resolve({ code: -1 }); });
            child.once('exit', (code, signal) => resolve({ code, signal }));
        });
        for (const stream of [child.stdout, child.stderr])
            stream.on('data', data => logs.set(name, (logs.get(name) + data).slice(-200000)));
        children.push(child);
        return child;
    }
    const url = `ws://127.0.0.1:${port}/ws`;
    const http = `http://127.0.0.1:${port}`;
    const server = run('server', 'app/server/mrtc_server',
        ['--port', String(port), '--bind', '127.0.0.1', '--mode', mode,
            '--web-root', resolve(root, process.env.MRTC_WEB_ROOT || 'web/dist'), ...extra]);
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
            if (server.exitCode !== null) throw Error(`server exited: ${logs.get('server')}`);
            try { return (await fetch(http)).ok; } catch { return false; }
        }, 'real server HTTP readiness');
        if (withBrowser) {
            const { chromium } = await import('playwright');
            browser = await chromium.launch({
            headless: true,
            ...(process.env.MRTC_CHROME_PATH ? { executablePath: process.env.MRTC_CHROME_PATH } : {}),
            args: ['--no-sandbox', '--mute-audio'],
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
        return { url, http, run, page, logs, close, browser };
    } catch (error) {
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
export function probe(fixture, name, meeting, duration = 180) {
    return fixture.run(name, 'app/sfu_probe', ['--server', fixture.url, '--meeting', meeting,
        '--name', name, '--duration', String(duration), '--synthetic', '--chat']);
}
