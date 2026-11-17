import assert from 'node:assert/strict';
import { fixture } from './fixture.mjs';
import { NativeClient } from './native-client.mjs';
import { active, media, exchange, absent } from './native-checks.mjs';

const cycles = Number(process.env.MRTC_CHURN_CYCLES || 120);
assert(Number.isSafeInteger(cycles) && cycles > 0, 'MRTC_CHURN_CYCLES must be a positive integer');
let f;
let deadline;
try {
    f = await fixture('sfu', [], { withBrowser: false });
    const observer = new NativeClient(f, 'observer');
    const churn = new NativeClient(f, 'churn');
    await observer.join(f.url, 'churn', 'Observer', { stun: '' });
    let generation = 0;
    const timeout = Number(process.env.MRTC_CHURN_TIMEOUT_MS || 360000);
    assert(Number.isSafeInteger(timeout) && timeout > 0, 'MRTC_CHURN_TIMEOUT_MS must be positive milliseconds');
    deadline = setTimeout(() => {
        const error = Error(`native churn exceeded ${timeout} ms`);
        observer.fail(error);
        churn.fail(error);
    }, timeout);
    for (let cycle = 0; cycle < cycles; cycle++) {
        await churn.join(f.url, 'churn', 'Churn', { stun: '' });
        assert(churn.generation > generation);
        generation = churn.generation;
        await active([observer, churn], 'sfu');
        await media([observer, churn]);
        await exchange(observer, [churn], `observer-${cycle}`);
        await exchange(churn, [observer], `churn-${cycle}`);
        const departed = churn.peerId;
        const cursor = observer.cursor;
        await churn.leave();
        await observer.event('peer_left', event => event.peer_id === departed, cursor);
        await absent([observer], departed);
        if ((cycle + 1) % 10 === 0) console.log(`native churn: ${cycle + 1}/${cycles} verified cycles`);
    }
    await churn.quit();
    await observer.quit();
    console.log(`PASS ${cycles} leave/join cycles on the same CLI processes, fresh bidirectional decode and exact SCTP delivery every cycle`);
} catch (error) {
    console.error(error);
    await f?.artifacts(error);
    if (f) for (const [name, text] of f.logs) console.error(`--- ${name} (tail) ---\n${text.slice(-4000)}`);
    process.exitCode = 1;
} finally {
    clearTimeout(deadline);
    await f?.close();
}
