import assert from 'node:assert/strict';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { fixture as serverFixture, until } from '../support/fixture.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
process.env.PLAYWRIGHT_BROWSERS_PATH ||= resolve(root, 'tests/e2e/node_modules/.cache/ms-playwright');
export { until } from '../support/fixture.mjs';

export async function fixture(mode, extra = []) {
    const f = await serverFixture(mode, extra, { webRoot: process.env.MRTC_WEB_ROOT || 'app/web/dist' });
    let browser;
    async function artifacts(error) {
        const snapshots = {};
        if (browser) for (const [index, page] of browser.contexts().flatMap(context => context.pages()).entries()) {
            let timer;
            try {
                snapshots[`browser-${index}`] = await Promise.race([
                    page.evaluate(() => window.mrtc?.stats()),
                    new Promise((_, reject) => {
                        timer = setTimeout(() => reject(Error('browser snapshot deadline')), 2000);
                    }),
                ]);
            } catch (error) { snapshots[`browser-${index}`] = { unavailable: String(error) }; }
            finally { clearTimeout(timer); }
        }
        await f.artifacts(error, snapshots);
    }
    async function close() {
        try { if (browser) await browser.close(); }
        finally { await f.close(); }
    }
    try {
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
        const context = await browser.newContext();
        async function page(name, meeting) {
            const page = await context.newPage();
            page.on('pageerror', error => f.logs.set(`page:${name}`, String(error)));
            await page.routeWebSocket('**/ws', socket => {
                const remote = socket.connectToServer();
                socket.onMessage(message => {
                    if (typeof message === 'string') {
                        const value = JSON.parse(message);
                        if (value.type === 'offer') f.logs.set(`sdp:${name}`, message);
                        if (value.type === 'candidate' && value.candidate)
                            f.logs.set(`candidate:${name}:${value.session_id}`, message);
                    }
                    remote.send(message);
                });
            });
            await page.goto(f.http);
            await page.locator('#name').fill(name);
            await page.locator('#room').fill(meeting);
            await page.locator('#synthetic').check();
            await page.locator('#join').click();
            await until(async () => (await page.evaluate(() => window.mrtc.stats())).id, `${name} UI join`);
            return page;
        }
        return { ...f, page, browser, close, artifacts };
    } catch (error) {
        await artifacts(error).catch(error => console.error(`Artifact write failed: ${error}`));
        await close();
        throw Error(`${error}\n${[...f.logs].map(([name, text]) => `${name}: ${text}`).join('\n')}`);
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
