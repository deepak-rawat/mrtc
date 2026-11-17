import assert from 'node:assert/strict';
import { fixture, until, stats, decoded, chat, probe } from './fixture.mjs';

async function exerciseChat(f, prefix) {
    const args = ['--server', f.url, '--meeting', prefix, '--stun', ''];
    const a = f.run(`${prefix}-a`, 'app/chat/chat', args);
    const b = f.run(`${prefix}-b`, 'app/chat/chat', args);
    await until(() => f.logs.get(`${prefix}-a`).includes('[joined as') &&
        f.logs.get(`${prefix}-b`).includes('[joined as'), 'native CLI clients joined');
    await until(() => {
        if (f.logs.get(`${prefix}-b`).includes(`${prefix}-check`)) return true;
        a.stdin.write(`${prefix}-check\n`);
        return false;
    }, 'CLI chat delivery');
    await until(() => {
        if (f.logs.get(`${prefix}-a`).includes(`${prefix}-reply`)) return true;
        b.stdin.write(`${prefix}-reply\n`);
        return false;
    }, 'CLI chat reply');
    a.stdin.write('quit\n'); b.stdin.write('quit\n');
    assert.equal((await a.done).code, 0);
    assert.equal((await b.done).code, 0);
}
for (const mode of ['sfu', 'mesh']) {
    let f;
    try {
        f = await fixture(mode);
        probe(f, 'native', 'interop');
        await until(() => /(?:PROBE_JOINED|JOINED peer=)/.test(f.logs.get('native')), 'native join');
        const first = await f.page('Browser Alice', 'interop');
        const second = await f.page('Browser Bob', 'interop');
        const a = (await stats(first)).id;
        const b = (await stats(second)).id;
        await decoded(first, [b]);
        await decoded(second, [a]);
        await until(async () => Object.keys((await stats(first)).sources).length === 2, 'native source routing');
        const nativeId = Object.keys((await stats(first)).sources).find(id => id !== b);
        await decoded(first, [nativeId, b]);
        await until(() => f.logs.get('native').includes('PROBE_MEDIA_OK'), 'native audio/video decode');
        await chat(first, 'browser-datachannel');
        await until(async () => (await stats(second)).messages.includes('browser-datachannel'), 'browser SCTP delivery');
        await until(async () => (await stats(first)).messages.includes('native-datachannel'), 'native SCTP delivery');
        await until(() => f.logs.get('native').includes('PROBE_DATA_OK'), 'native receives browser SCTP');
        const tile = await second.locator(`[data-peer-id="${a}"]`).elementHandle();
        if (mode === 'sfu') {
            const sourceFrames = async () => (await stats(second)).sources[a].video;
            await first.evaluate(() => window.mrtc.renegotiate('inactive'));
            await new Promise(resolve => setTimeout(resolve, 700));
            const paused = await sourceFrames();
            await new Promise(resolve => setTimeout(resolve, 500));
            assert.equal(await sourceFrames(), paused);
            await first.evaluate(() => window.mrtc.renegotiate('sendrecv'));
            await until(async () => await sourceFrames() > paused + 2, 'resumed forwarding');
            assert(await tile.evaluate(node => node === document.querySelector(`[data-peer-id="${node.dataset.peerId}"]`)));
        }
        await first.locator('#restart').click();
        await until(async () => (await stats(first)).connections.every(session =>
            session.state === 'connected'), 'live ICE restart');
        await chat(first, 'restart-datachannel');
        await until(async () => (await stats(second)).messages.includes('restart-datachannel'), 'SCTP survives ICE restart');
        await second.locator('#leave').click();
        await until(async () => await first.locator('#remotes figure').count() === 1, 'departed tile removed');
        await second.locator('#join').click();
        await until(async () => (await stats(second)).id, 'same UI rejoins');
        await until(async () => await first.locator('#remotes figure').count() === 2, 'rejoined participant tile');
        await decoded(second, [a]);
        await exerciseChat(f, `${mode}-chat`);
        assert([...f.logs.entries()].some(([name, text]) => name.startsWith('candidate:') &&
            text.includes('.local ')), 'Default-privacy synthetic browser must exercise mDNS host candidates');
        assert(!f.logs.get('native').includes('PROBE_ERROR'), 'Native probe must not hide negotiation errors');
        assert.deepEqual((await stats(first)).errors, []);
        assert.deepEqual((await stats(second)).errors, []);
        console.log(`PASS ${mode}: default-privacy mDNS + Join-click autoplay, actual UI, bidirectional native/browser decoded media, SCTP, restart, leave/rejoin and CLI chat`);
    } catch (error) {
        console.error(error);
        if (f) for (const page of f.browser.contexts().flatMap(context => context.pages()))
            console.error(JSON.stringify(await stats(page).catch(String)));
        if (f) for (const [name, text] of f.logs) console.error(`--- ${name} ---\n${text}`);
        process.exitCode = 1;
    } finally { await f?.close(); }
}
