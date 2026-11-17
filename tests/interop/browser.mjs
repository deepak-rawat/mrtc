import assert from 'node:assert/strict';
import { fixture, until, stats, decoded, chat } from './fixture.mjs';

for (const mode of ['mesh', 'sfu', 'auto']) {
    let f;
    try {
        f = await fixture(mode, ['--p2p-max', '2', '--downgrade-delay-ms', '250']);
        const first = await f.page('<Alice>', 'browser');
        const second = await f.page('Bob', 'browser');
        const a = (await stats(first)).id;
        const b = (await stats(second)).id;
        await decoded(first, [b]);
        await decoded(second, [a]);
        assert.equal(await second.locator(`[data-peer-id="${a}"] figcaption`).textContent(), '<Alice>');
        assert.equal(await second.locator('#remotes script').count(), 0);
        await chat(first, 'real-ui-message');
        await until(async () => (await stats(second)).messages.includes('real-ui-message'), 'real UI chat');
        const oldEpoch = (await stats(first)).epoch;
        const third = await f.page('Carol', 'browser');
        const c = (await stats(third)).id;
        if (mode === 'auto') {
            await until(async () => (await stats(first)).mode === 'sfu', 'automatic SFU commit');
            const sfu = (await stats(first)).epoch;
            assert(sfu > oldEpoch);
            await decoded(first, [b, c], sfu);
            await decoded(second, [a, c], sfu);
            await decoded(third, [a, b], sfu);
        } else {
            await decoded(first, [b, c], oldEpoch);
            await decoded(second, [a, c], oldEpoch);
            await decoded(third, [a, b], oldEpoch);
            assert.equal((await stats(first)).mode, mode);
        }
        await third.locator('#leave').click();
        await until(async () => await first.locator('#remotes figure').count() === 1, 'third participant leaves');
        if (mode === 'auto') await until(async () => (await stats(first)).mode === 'mesh', 'automatic mesh commit');
        else assert.equal((await stats(first)).epoch, oldEpoch);
        await decoded(first, [b], (await stats(first)).epoch);
        await decoded(second, [a], (await stats(second)).epoch);
        await first.locator('#leave').click();
        await until(async () => await second.locator('#remotes figure').count() === 0, 'peer tile removal');
        assert.deepEqual((await stats(second)).errors, []);
        console.log(`PASS browser ${mode}: real Join/chat/Leave UI, per-source decoded VP8/Opus and safe names`);
    } catch (error) {
        console.error(error);
        if (f) {
            for (const page of f.browser.contexts().flatMap(context => context.pages()))
                console.error(JSON.stringify(await stats(page).catch(String)));
            for (const [name, text] of f.logs) console.error(`--- ${name} ---\n${text}`);
        }
        process.exitCode = 1;
    } finally { await f?.close(); }
}

let thresholdFixture;
try {
    const maximum = Number(process.env.MRTC_P2P_MAX || '4');
    assert(Number.isInteger(maximum) && maximum >= 2 && maximum < 8,
        'MRTC_P2P_MAX must be 2..7 so one overflow participant fits the room');
    thresholdFixture = await fixture('auto',
        ['--p2p-max', String(maximum), '--downgrade-delay-ms', '250']);
    const pages = [];
    for (let i = 0; i < maximum; i++)
        pages.push(await thresholdFixture.page(`Threshold ${i + 1}`, 'threshold'));
    const ids = await Promise.all(pages.map(async page => (await stats(page)).id));
    for (let i = 0; i < pages.length; i++) {
        await decoded(pages[i], ids.filter(id => id !== ids[i]));
        assert.equal((await stats(pages[i])).mode, 'mesh');
    }
    const meshEpoch = (await stats(pages[0])).epoch;
    const overflow = await thresholdFixture.page('Threshold overflow', 'threshold');
    const overflowId = (await stats(overflow)).id;
    await until(async () => (await stats(pages[0])).mode === 'sfu', 'custom threshold SFU commit');
    const sfuEpoch = (await stats(pages[0])).epoch;
    assert(sfuEpoch > meshEpoch);
    const allPages = [...pages, overflow];
    const allIds = [...ids, overflowId];
    for (let i = 0; i < allPages.length; i++) {
        await decoded(allPages[i], allIds.filter(id => id !== allIds[i]), sfuEpoch);
        assert.deepEqual((await stats(allPages[i])).errors, []);
    }
    await overflow.locator('#leave').click();
    await until(async () => (await stats(pages[0])).mode === 'mesh', 'custom threshold mesh commit');
    const restoredEpoch = (await stats(pages[0])).epoch;
    assert(restoredEpoch > sfuEpoch);
    for (let i = 0; i < pages.length; i++) {
        await decoded(pages[i], ids.filter(id => id !== ids[i]), restoredEpoch);
        assert.deepEqual((await stats(pages[i])).errors, []);
    }
    console.log(`PASS configurable threshold ${maximum}: decoded ${maximum}→${maximum + 1}→${maximum} handoff`);
} catch (error) {
    console.error(error);
    if (thresholdFixture) {
        for (const page of thresholdFixture.browser.contexts().flatMap(context => context.pages()))
            console.error(JSON.stringify(await stats(page).catch(String)));
        for (const [name, text] of thresholdFixture.logs)
            console.error(`--- ${name} ---\n${text}`);
    }
    process.exitCode = 1;
} finally {
    await thresholdFixture?.close();
}
