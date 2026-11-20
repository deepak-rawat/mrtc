import assert from 'node:assert/strict';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { fixture, build } from './fixture.mjs';
import { NativeClient } from './native-client.mjs';
import { active, media, exchange, absent } from './native-checks.mjs';
import { openChurnReport } from './churn-report.mjs';

const cycles = Number(process.env.MRTC_CHURN_CYCLES || 120);
let f;
let deadline;
let deadlineError;
let report, closed = false;
try {
    if (Object.hasOwn(process.env, 'MRTC_CHURN_REPORT')) {
        report = await openChurnReport({ cycles, build,
            root: resolve(dirname(fileURLToPath(import.meta.url)), '../..') });
        await report.initialize();
    }
    assert(Number.isSafeInteger(cycles) && cycles > 0, 'MRTC_CHURN_CYCLES must be a positive integer');
    f = await fixture('sfu', [], { withBrowser: false });
    const observer = new NativeClient(f, 'observer');
    const churn = new NativeClient(f, 'churn');
    await observer.join(f.url, 'churn', 'Observer', { stun: '' });
    let generation = 0;
    const timeout = Number(process.env.MRTC_CHURN_TIMEOUT_MS || 360000);
    assert(Number.isSafeInteger(timeout) && timeout > 0, 'MRTC_CHURN_TIMEOUT_MS must be positive milliseconds');
    deadline = setTimeout(() => {
        const error = Error(`native churn exceeded ${timeout} ms`);
        deadlineError = error;
        observer.fail(error);
        churn.fail(error);
    }, timeout);
    const processes = new Map([['server', f.server], ['observer', observer.child], ['churn', churn.child]]);
    report?.start();
    for (let cycle = 0; cycle < cycles; cycle++) {
        const cycleStarted = report ? performance.now() : 0;
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
        if (report) await report.cycle(cycle + 1, performance.now() - cycleStarted, processes, async () => {
            const [snapshot, observed] = await Promise.all([churn.snapshot(), observer.snapshot()]);
            assert.equal(snapshot.joined, false, 'Resource sample requires the churn client to be left');
            assert.equal(snapshot.active, false, 'Resource sample requires the churn session to be stopped');
            assert.equal(observed.joined, true, 'Resource sample requires the observer to remain joined');
            assert.equal(observed.peers.length, 0, 'Resource sample requires the observer to have no departed peers');
            assert.equal(observed.sources.length, 0, 'Resource sample requires the observer to have no departed sources');
            return {
                observer: { joined: observed.joined, generation: observed.generation, epoch: observed.epoch,
                    peerCount: observed.peers.length, sourceCount: observed.sources.length },
                churn: { joined: snapshot.joined, active: snapshot.active, generation: snapshot.generation,
                    epoch: snapshot.epoch, snapshotCachedPeerCount: snapshot.peers.length,
                    snapshotCachedSourceCount: snapshot.sources.length },
            };
        });
        if ((cycle + 1) % 10 === 0) console.log(`native churn: ${cycle + 1}/${cycles} verified cycles`);
    }
    await churn.quit();
    await observer.quit();
    if (report) {
        assert.equal(f.server.exitCode, null, 'Server exited before controlled shutdown');
        await f.close();
        closed = true;
        assert.equal((await f.server.done).code, 0, 'Server did not shut down cleanly');
        clearTimeout(deadline);
        if (deadlineError) throw deadlineError;
        await report.finish(true);
        console.log(`Churn resource report: ${report.path} (sampled growth only; inspect explicit gate status)`);
    }
    console.log(`PASS ${cycles} leave/join cycles on the same CLI processes, fresh bidirectional decode and exact SCTP delivery every cycle`);
} catch (error) {
    console.error(error);
    if (report) {
        try { await report.finish(false, error); }
        catch (reportError) { console.error(`Churn terminal report write failed: ${reportError}`); }
    }
    await f?.artifacts(error);
    if (f) for (const [name, text] of f.logs) console.error(`--- ${name} (tail) ---\n${text.slice(-4000)}`);
    process.exitCode = 1;
} finally {
    clearTimeout(deadline);
    if (!closed) await f?.close();
    await report?.close();
}
