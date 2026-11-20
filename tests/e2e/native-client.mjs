import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';

export class ControlError extends Error {
    constructor(message, detail) {
        super(message);
        this.name = 'ControlError';
        this.detail = detail;
    }
}

export class NativeClient {
    constructor(fixture, name, { child, expectedError = () => false } = {}) {
        this.name = name;
        this.fixture = fixture;
        fixture?.nativeClients?.set(name, this);
        this.child = child ?? fixture.run(name, 'app/cli/mrtc_cli', ['--control', 'stdio']);
        this.events = [];
        this.pending = new Map();
        this.changed = new EventEmitter();
        this.nextId = 1;
        this.cursor = 0;
        this.buffer = '';
        this.failure = null;
        this.expectedError = expectedError;
        this.child.stdout.setEncoding('utf8');
        this.child.stdout.on('data', data => this.read(data));
        this.child.stdout.on('end', () => {
            if (this.buffer) this.fail(new ControlError(`${name}: truncated JSONL output`));
            else if (!this.quitting || this.pending.size)
                this.fail(new ControlError(`${name}: control output ended`));
        });
        this.child.stdin.on('error', error => this.fail(error));
        this.child.on('error', error => this.fail(error));
        this.child.done.then(result => {
            this.exited = result;
            if (!this.quitting || result.code !== 0 || this.pending.size)
                this.fail(new ControlError(`${name}: exited ${JSON.stringify(result)}`, result));
            this.changed.emit('change');
        });
    }

    fail(error) {
        this.failure ??= error;
        for (const { reject, timer } of this.pending.values()) {
            clearTimeout(timer);
            reject(this.failure);
        }
        this.pending.clear();
        this.changed.emit('change');
    }

    read(data) {
        if (this.failure) return;
        this.buffer += data;
        let newline;
        while ((newline = this.buffer.indexOf('\n')) >= 0) {
            if (Buffer.byteLength(this.buffer.slice(0, newline), 'utf8') > 1024 * 1024) {
                this.fail(new ControlError(`${this.name}: control frame exceeds 1 MiB`));
                return;
            }
            const line = this.buffer.slice(0, newline);
            this.buffer = this.buffer.slice(newline + 1);
            try {
                const event = JSON.parse(line);
                assert.equal(event.version, 1, 'Native control protocol version');
                assert.equal(typeof event.event, 'string', 'Native control event type');
                if (event.event === 'result') {
                    assert.equal(typeof event.ok, 'boolean');
                    const request = this.pending.get(event.id);
                    assert(request, `Unexpected command response ${event.id}`);
                    this.pending.delete(event.id);
                    clearTimeout(request.timer);
                    if (event.ok) {
                        if (request.command === 'stats') this.lastSnapshot = event.stats;
                        request.resolve(event);
                    }
                    else request.reject(new ControlError(`${this.name}: ${request.command} failed`, event));
                } else {
                    assert(['joined', 'peer_joined', 'peer_left', 'message', 'topology', 'error'].includes(event.event),
                        `Unknown event ${event.event}`);
                    assert(Number.isSafeInteger(event.generation), 'Missing event generation');
                    assert(Number.isSafeInteger(event.epoch), 'Missing event epoch');
                    if (['joined', 'peer_joined', 'peer_left', 'message'].includes(event.event))
                        assert.equal(typeof event.peer_id, 'string', 'Missing event peer_id');
                    if (event.event === 'message') assert.equal(typeof event.text, 'string', 'Missing message text');
                    if (event.event === 'topology') {
                        assert.equal(typeof event.mode, 'string', 'Missing topology mode');
                        assert.equal(typeof event.status, 'string', 'Missing topology status');
                    }
                    this.events.push({ ...event, sequence: ++this.cursor });
                    if (this.events.length > 2048) this.events.shift();
                    if (event.event === 'error' && !this.expectedError(event))
                        throw new ControlError(`${this.name}: asynchronous error`, event);
                }
                this.changed.emit('change');
            } catch (error) {
                this.fail(error);
                return;
            }
        }
        if (Buffer.byteLength(this.buffer, 'utf8') > 1024 * 1024)
            this.fail(new ControlError(`${this.name}: control frame exceeds 1 MiB`));
    }

    command(command, fields = {}, timeout = 10000) {
        if (this.failure) return Promise.reject(this.failure);
        if (this.exited) return Promise.reject(new ControlError(`${this.name}: already exited`));
        if (this.pending.size >= 128)
            return Promise.reject(new ControlError(`${this.name}: too many outstanding commands`));
        const id = this.nextId++;
        if (command === 'quit') this.quitting = true;
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.fail(new ControlError(`${this.name}: command deadline: ${command}`, { id, command }));
            }, timeout);
            this.pending.set(id, { resolve, reject, timer, command });
            const line = `${JSON.stringify({ ...fields, version: 1, id, command })}\n`;
            this.fixture?.record?.(`${this.name}:commands`, line);
            this.child.stdin.write(line);
        });
    }

    wait(predicate, label, timeout = 45000) {
        return new Promise((resolve, reject) => {
            const finish = (error, value) => {
                clearTimeout(timer);
                this.changed.off('change', check);
                if (error) reject(error); else resolve(value);
            };
            const check = () => {
                try {
                    if (this.failure) throw this.failure;
                    if (this.exited) throw new ControlError(`${this.name}: exited waiting for ${label}`);
                    const value = predicate();
                    if (value) finish(null, value);
                } catch (error) { finish(error); }
            };
            const timer = setTimeout(() => finish(new ControlError(`${this.name}: event deadline: ${label}`)), timeout);
            this.changed.on('change', check);
            check();
        });
    }

    event(type, predicate = () => true, after = 0, timeout = 45000) {
        return this.wait(() => this.events.find(event => event.sequence > after &&
            event.event === type && predicate(event)), type, timeout);
    }

    async join(server, room, name, options = {}) {
        const cursor = this.cursor;
        await this.command('join', { server, room, name, video: 'synthetic', audio: 'synthetic', ...options });
        const event = await this.event('joined', () => true, cursor);
        assert(event.generation > (this.generation ?? -1), 'Join must advance generation');
        this.peerId = event.peer_id;
        this.generation = event.generation;
        return this;
    }

    async snapshot() { return (await this.command('stats')).stats; }
    expectMessage(peerId, text, after = 0) {
        return this.event('message', event => event.peer_id === peerId && event.text === text &&
            event.generation === this.generation, after);
    }
    async leave() { await this.command('leave'); }
    async sendMessage(text, peerId) {
        await this.command('send_message', { text, ...(peerId ? { peer_id: peerId } : {}) });
    }
    async mute(source, muted) { await this.command('mute', { source, muted }); }
    async quit() {
        await this.command('quit');
        this.child.stdin.end();
        let timer;
        try {
            const result = await Promise.race([this.child.done, new Promise((_, reject) => {
                timer = setTimeout(() => reject(new ControlError(`${this.name}: quit exit deadline`)), 5000);
            })]);
            assert.equal(result.code, 0);
        } finally { clearTimeout(timer); }
    }
}
