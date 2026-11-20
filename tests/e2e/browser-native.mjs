import assert from 'node:assert/strict';
import { fixture, until, stats, decoded, chat } from './fixture.mjs';
import { NativeClient } from './native-client.mjs';
import { media, exchange, active } from './native-checks.mjs';

async function exerciseChat(f, prefix) {
    const a = new NativeClient(f, `${prefix}-a`);
    const b = new NativeClient(f, `${prefix}-b`);
    await a.join(f.url, prefix, 'CLI Alice', { stun: '' });
    await b.join(f.url, prefix, 'CLI Bob', { stun: '' });
    await active([a, b], prefix.startsWith('sfu') ? 'sfu' : 'mesh');
    await media([a, b]);
    await exchange(a, [b], `${prefix}-check`);
    await exchange(b, [a], `${prefix}-reply`);
    await a.quit();
    await b.quit();
}
for (const mode of ['sfu', 'mesh']) {
    let f;
    try {
        f = await fixture(mode);
        const native = new NativeClient(f, 'native');
        await native.join(f.url, 'interop', 'Native', { stun: '' });
        const first = await f.page('Browser Alice', 'interop');
        const second = await f.page('Browser Bob', 'interop');
        const a = (await stats(first)).id;
        const b = (await stats(second)).id;
        await decoded(first, [b]);
        await decoded(second, [a]);
        await until(async () => Object.keys((await stats(first)).sources).length === 2, 'native source routing');
        const nativeId = native.peerId;
        await decoded(first, [nativeId, b]);
        await media([native], [{ peerId: a }, { peerId: b }]);
        const cursor = native.cursor;
        await chat(first, 'browser-datachannel');
        await until(async () => (await stats(second)).messages.includes('browser-datachannel'), 'browser SCTP delivery');
        await native.expectMessage(a, 'browser-datachannel', cursor);
        await native.sendMessage('native-datachannel');
        await until(async () => (await stats(first)).messages.includes('native-datachannel'), 'native SCTP delivery');
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
        await native.snapshot();
        assert.deepEqual((await stats(first)).errors, []);
        assert.deepEqual((await stats(second)).errors, []);
        await native.quit();
        console.log(`PASS ${mode}: default-privacy mDNS + Join-click autoplay, actual UI, bidirectional native/browser decoded media, SCTP, restart, leave/rejoin and CLI chat`);
    } catch (error) {
        console.error(error);
        await f?.artifacts(error);
        if (f) for (const page of f.browser.contexts().flatMap(context => context.pages()))
            console.error(JSON.stringify(await stats(page).catch(String)));
        if (f) for (const [name, text] of f.logs) console.error(`--- ${name} (tail) ---\n${text.slice(-4000)}`);
        process.exitCode = 1;
    } finally { await f?.close(); }
}
