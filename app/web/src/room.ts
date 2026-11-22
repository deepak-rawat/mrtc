import { capture } from './capture';
import type { Capture } from './capture';
import { envelope, MessageHistory, parseSignal, sessionId, validPeer } from './protocol';
import type { Mode, Signal } from './protocol';

interface Session {
  id: string;
  peer: string;
  pc: RTCPeerConnection;
  stream: MediaStream;
  channels: Set<RTCDataChannel>;
  candidates: RTCIceCandidateInit[];
  receiveKinds: Set<string>;
  answers: number;
  decoder: HTMLVideoElement;
  videoFrames: number;
  audioSamples: number;
  videoAt: number;
  audioAt: number;
}
interface Graph {
  epoch: number;
  mode: Mode;
  peers: Set<string>;
  sessions: Map<string, Session>;
  ready: boolean;
}
export interface RoomView {
  status(text: string): void;
  error(text: string): void;
  local(stream: MediaStream | null): void;
  participant(id: string, name: string): void;
  remove(id: string): void;
  media(id: string, stream: MediaStream): void;
  message(id: string, text: string): void;
}
export interface JoinOptions { server: string; meeting: string; name: string; synthetic: boolean }
export class Room {
  id = '';
  readonly errors: string[] = [];
  readonly events: string[] = [];
  readonly messages: string[] = [];
  private ws: WebSocket | undefined;
  private source: Capture | undefined;
  private active: Graph | undefined;
  private pending: Graph | undefined;
  private retiring: Graph | undefined;
  private retirement: number | undefined;
  private poll: number | undefined;
  private chain = Promise.resolve();
  private sequence = 0n;
  private history = new MessageHistory();
  private readonly names = new Map<string, string>();
  private closed = true;
  private generation = 0;
  private checking = false;
  private fallbackRequested = false;
  private highestEpoch = 0;
  private readonly view: RoomView;
  constructor(view: RoomView) { this.view = view; }
  private fail(error: unknown): void {
    const text = error instanceof Error ? error.message : String(error);
    this.errors.push(text);
    if (this.errors.length > 64) this.errors.shift();
    this.view.error(text);
  }
  private event(text: string): void {
    this.events.push(text);
    if (this.events.length > 256) this.events.shift();
  }
  private send(type: string, fields: Record<string, unknown> = {}): void {
    if (this.ws?.readyState !== WebSocket.OPEN) throw Error('Signaling is not connected');
    this.ws.send(JSON.stringify({ type, version: 2, ...fields }));
  }
  async join(options: JoinOptions): Promise<void> {
    if (!this.closed) throw Error('Already joined');
    const bytes = new TextEncoder();
    if (bytes.encode(options.name).length > 63 || !options.name.trim()) throw Error('Name must be 1–63 UTF-8 bytes');
    if (bytes.encode(options.meeting).length > 63 || !options.meeting.trim()) throw Error('Room must be 1–63 UTF-8 bytes');
    const url = new URL(options.server);
    if (url.protocol !== 'ws:' && url.protocol !== 'wss:') throw Error('Server must use WS or WSS');
    if (location.protocol === 'https:' && url.protocol !== 'wss:') throw Error('HTTPS pages require WSS');
    this.closed = false;
    const generation = ++this.generation;
    this.errors.length = 0;
    this.events.length = 0;
    this.messages.length = 0;
    this.history = new MessageHistory();
    this.sequence = 0n;
    this.fallbackRequested = false;
    this.highestEpoch = 0;
    try {
      const source = await capture(options.synthetic);
      if (this.closed || generation !== this.generation) {
        await source.close();
        return;
      }
      this.source = source;
      this.view.local(this.source.stream);
      const ws = this.ws = new WebSocket(url, 'signaling');
      ws.onopen = () => {
        if (this.closed || generation !== this.generation) return;
        this.send('join', { meeting: options.meeting, display_name: options.name,
          capabilities: { mesh: true, sfu: true, topology_switch: true } });
      };
      ws.onerror = () => {
        if (!this.closed && generation === this.generation) this.fail('WebSocket connection failed');
      };
      ws.onclose = () => {
        if (!this.closed && generation === this.generation) this.fail('Signaling disconnected; leave and rejoin to reconnect');
      };
      ws.onmessage = event => {
        if (typeof event.data !== 'string') { this.fail('Non-text signaling message'); return; }
        const raw: string = event.data;
        this.chain = this.chain.then(async () => {
          if (!this.closed && generation === this.generation) await this.signal(parseSignal(raw));
        }).catch(error => { if (!this.closed && generation === this.generation) this.fail(error); });
      };
      this.poll = window.setInterval(() => {
        if (this.checking) return;
        this.checking = true;
        void this.observeActive().then(() => this.checkReady())
          .catch(error => { if (!this.closed && generation === this.generation) this.fail(error); })
          .finally(() => { this.checking = false; });
      }, 150);
      this.view.status('Joining…');
    } catch (error) {
      if (generation === this.generation) await this.leave();
      throw error;
    }
  }
  private roster(message: Signal): string[] {
    const peers = message.peers ?? [];
    for (const peer of peers) {
      this.names.set(peer, message.peer_names?.[peer] ?? this.names.get(peer) ?? peer);
      this.view.participant(peer, this.names.get(peer)!);
    }
    return peers;
  }
  private session(graph: Graph, peer: string): Session {
    const id = sessionId(graph.mode, graph.epoch, this.id, peer);
    const existing = graph.sessions.get(id);
    if (existing) return existing;
    const pc = new RTCPeerConnection({ iceServers: [], bundlePolicy: 'max-bundle' });
    const decoder = document.createElement('video');
    decoder.muted = true;
    decoder.autoplay = true;
    decoder.playsInline = true;
    decoder.hidden = true;
    const session: Session = { id, peer, pc, stream: new MediaStream(), channels: new Set(),
      candidates: [], receiveKinds: new Set(), answers: 0, decoder,
      videoFrames: 0, audioSamples: 0, videoAt: 0, audioAt: 0 };
    decoder.srcObject = session.stream;
    document.body.append(decoder);
    graph.sessions.set(id, session);
    const isPublisher = graph.mode === 'mesh' || peer === 'sfu';
    if (isPublisher) {
      for (const track of this.source!.stream.getTracks()) pc.addTrack(track, this.source!.stream);
      this.codecs(pc);
      if (graph.mode === 'sfu' || this.id < peer) this.channel(graph, session, pc.createDataChannel('chat'));
    }
    pc.ondatachannel = event => {
      if (this.live(graph)) this.channel(graph, session, event.channel);
      else event.channel.close();
    };
    pc.onicecandidate = event => {
      if (!this.live(graph)) return;
      this.send('candidate', { to: peer, epoch: graph.epoch, session_id: id,
        candidate: event.candidate?.candidate ?? '' });
    };
    pc.ontrack = event => {
      if (!this.live(graph)) return;
      session.stream.addTrack(event.track);
      void decoder.play().catch(error => { if (this.live(graph)) this.fail(error); });
      if (this.active === graph && peer !== 'sfu') this.view.media(peer, session.stream);
    };
    pc.onconnectionstatechange = () => {
      if (!this.live(graph) || pc.connectionState !== 'failed') return;
      this.fail(`Connection failed: ${peer} (epoch ${graph.epoch})`);
      if (graph === this.active && graph.mode === 'mesh' && !this.fallbackRequested) {
        this.fallbackRequested = true;
        this.send('topology_request', { mode: 'sfu', epoch: graph.epoch, reason: 'mesh connectivity failed' });
      }
    };
    return session;
  }
  private codecs(pc: RTCPeerConnection): void {
    for (const transceiver of pc.getTransceivers()) {
      const kind = transceiver.sender.track?.kind ?? transceiver.receiver.track.kind;
      const codecs = RTCRtpSender.getCapabilities(kind)?.codecs.filter(codec =>
        codec.mimeType.toLowerCase() === (kind === 'video' ? 'video/vp8' : 'audio/opus'));
      if (!codecs?.length) throw Error(`Required ${kind === 'video' ? 'VP8' : 'Opus'} codec unavailable`);
      transceiver.setCodecPreferences(codecs);
    }
  }
  private channel(graph: Graph, session: Session, channel: RTCDataChannel): void {
    const generation = this.generation;
    if (session.channels.size >= 2) { channel.close(); this.fail('Too many data channels'); return; }
    session.channels.add(channel);
    channel.binaryType = 'arraybuffer';
    channel.onclose = () => session.channels.delete(channel);
    channel.onmessage = event => {
      if (this.closed || generation !== this.generation || !this.live(graph) ||
          graph.sessions.get(session.id) !== session) return;
      if (!(event.data instanceof ArrayBuffer)) { this.fail('Expected binary conference envelope'); return; }
      try {
        const message = this.history.read(event.data);
        if (!message) return;
        this.messages.push(message.text);
        if (this.messages.length > 256) this.messages.shift();
        this.view.message(message.peer, message.text);
      } catch (error) { this.fail(error); }
    };
  }
  private live(graph: Graph): boolean {
    return !this.closed && (this.active === graph || this.pending === graph || this.retiring === graph);
  }
  private async offer(graph: Graph, session: Session, restart = false): Promise<void> {
    const offer = await session.pc.createOffer({ iceRestart: restart });
    await session.pc.setLocalDescription(offer);
    this.send('offer', { to: session.peer, epoch: graph.epoch, session_id: session.id, sdp: offer.sdp });
  }
  private receiveDirections(session: Session, sdp: string): void {
    session.receiveKinds.clear();
    for (const section of sdp.split(/\r?\nm=/).slice(1)) {
      const kind = section.split(' ', 1)[0];
      if ((kind === 'audio' || kind === 'video') && !/^(?:audio|video) 0 /.test(section) &&
          !/\r?\na=(?:inactive|recvonly)(?:\r?\n|$)/.test(section)) session.receiveKinds.add(kind);
    }
  }
  private async signal(message: Signal): Promise<void> {
    this.event(`${message.type}:${message.epoch ?? ''}:${message.peer_id ?? message.from ?? ''}`);
    if (message.type === 'error') throw Error(message.message ?? message.reason ?? 'Server rejected request');
    if (message.version !== 2) throw Error('Expected protocol v2 signaling');
    if (message.type === 'joined') {
      if (this.active) throw Error('Duplicate joined response');
      if (!message.peer_id || !validPeer(message.peer_id) || message.version !== 2) throw Error('Invalid v2 joined response');
      this.id = message.peer_id;
      this.highestEpoch = message.epoch ?? 0;
      // Assign the graph before asynchronous negotiation can emit candidates.
      this.active = { epoch: message.epoch!, mode: message.mode!, peers: new Set(), sessions: new Map(), ready: false };
      this.active = await this.buildInto(this.active, message);
      this.view.status(`${this.active.mode === 'mesh' ? 'P2P' : 'SFU'} · epoch ${this.active.epoch}`);
      return;
    }
    if (message.type === 'peer_joined') {
      const peer = message.peer_id;
      if (!peer || !validPeer(peer) || peer === this.id) throw Error('Invalid participant');
      if (!this.names.has(peer) && this.names.size >= 8) throw Error('Participant limit exceeded');
      this.names.set(peer, message.display_name ?? peer);
      this.view.participant(peer, this.names.get(peer)!);
      if (this.active) {
        this.active.peers.add(peer);
        if (this.active.mode === 'mesh') {
          const session = this.session(this.active, peer);
          if (this.id < peer) await this.offer(this.active, session);
        }
      }
      return;
    }
    if (message.type === 'peer_left') {
      const peer = message.peer_id;
      if (!peer) throw Error('Missing departing participant');
      for (const graph of [this.active, this.pending, this.retiring]) {
        graph?.peers.delete(peer);
        for (const [id, session] of graph?.sessions ?? []) {
          if (session.peer === peer) {
            this.closeSession(session);
            graph!.sessions.delete(id);
          }
        }
      }
      this.names.delete(peer);
      this.view.remove(peer);
      return;
    }
    if (message.type === 'topology_prepare') {
      if (!this.active || !message.epoch || message.epoch <= this.active.epoch) return;
      if (this.pending?.epoch === message.epoch) return;
      if (message.epoch <= this.highestEpoch) { this.event('discard:stale-prepare'); return; }
      this.highestEpoch = message.epoch;
      this.closeGraph(this.pending);
      this.closeGraph(this.retiring);
      this.retiring = undefined;
      this.pending = { epoch: message.epoch, mode: message.mode!, peers: new Set(), sessions: new Map(), ready: false };
      await this.buildInto(this.pending, message);
      this.view.status(`Switching ${this.active.mode} → ${message.mode} · epoch ${message.epoch}`);
      return;
    }
    if (message.type === 'topology_abort') {
      if (!this.pending || this.pending.epoch !== message.epoch) return;
      if (this.pending.mode !== message.mode) throw Error('Abort topology mismatch');
      this.closeGraph(this.pending);
      this.pending = undefined;
      this.fail(`Switch aborted: ${message.reason ?? 'coordinator cancelled'}`);
      this.view.status(`${this.active?.mode === 'mesh' ? 'P2P' : 'SFU'} · active (switch aborted)`);
      return;
    }
    if (message.type === 'topology_commit') {
      if (this.active && this.active.epoch === message.epoch && this.active.mode === message.mode) {
        this.send('topology_ack', { epoch: this.active.epoch, mode: this.active.mode });
        return;
      }
      const next = this.pending;
      if (!next || next.epoch !== message.epoch) return;
      if (next.mode !== message.mode) throw Error('Commit topology mismatch');
      if (!next.ready) throw Error('Coordinator committed before local media readiness');
      this.closeGraph(this.retiring);
      this.retiring = this.active;
      this.active = next;
      this.pending = undefined;
      for (const session of next.sessions.values())
        if (session.peer !== 'sfu') this.view.media(session.peer, session.stream);
      this.view.status(`${next.mode === 'mesh' ? 'P2P' : 'SFU'} · epoch ${next.epoch}`);
      this.send('topology_ack', { epoch: next.epoch, mode: next.mode });
      window.clearTimeout(this.retirement);
      this.retirement = window.setTimeout(() => {
        this.closeGraph(this.retiring); this.retiring = undefined;
      }, 1000);
      return;
    }
    if (!['offer', 'answer', 'candidate'].includes(message.type)) return;
    const graph = [this.active, this.pending].find(value => value?.epoch === message.epoch);
    if (!graph) { this.event('discard:stale-epoch'); return; }
    const peer = message.from;
    if (!peer || (peer !== 'sfu' && !graph.peers.has(peer))) throw Error('Session peer is not in roster');
    const expected = sessionId(graph.mode, graph.epoch, this.id, peer);
    if (message.session_id !== expected) throw Error('Session ownership mismatch');
    if (graph.mode === 'mesh' && message.type === 'offer' && peer > this.id &&
        !graph.sessions.get(expected)?.pc.remoteDescription) throw Error('Non-designated initial mesh offerer');
    if (graph.mode === 'sfu' && peer === 'sfu' && message.type === 'offer') throw Error('Unexpected SFU publisher offer');
    const session = this.session(graph, peer);
    if (message.type === 'candidate') {
      const candidate = message.candidate ? { candidate: message.candidate, sdpMLineIndex: 0 } : undefined;
      if (candidate) {
        if (session.pc.remoteDescription) await this.addCandidate(session, candidate);
        else {
          if (session.candidates.length >= 64) throw Error('Too many queued ICE candidates');
          session.candidates.push(candidate);
        }
      }
      return;
    }
    if (!message.sdp) throw Error('Missing SDP');
    this.receiveDirections(session, message.sdp);
    await session.pc.setRemoteDescription({ type: message.type === 'offer' ? 'offer' : 'answer', sdp: message.sdp });
    for (const candidate of session.candidates.splice(0)) await this.addCandidate(session, candidate);
    if (message.type === 'offer') {
      this.codecs(session.pc);
      const answer = await session.pc.createAnswer();
      await session.pc.setLocalDescription(answer);
      this.send('answer', { to: peer, epoch: graph.epoch, session_id: session.id, sdp: answer.sdp });
    } else session.answers++;
  }
  private async addCandidate(session: Session, candidate: RTCIceCandidateInit): Promise<void> {
    const ufrag = candidate.candidate?.match(/(?:^| )ufrag ([^ ]+)/)?.[1];
    if (ufrag && !session.pc.remoteDescription?.sdp.includes(`a=ice-ufrag:${ufrag}\r\n`)) {
      this.event('discard:stale-ice-generation');
      return;
    }
    await session.pc.addIceCandidate(candidate);
  }
  private async buildInto(graph: Graph, message: Signal): Promise<Graph> {
    if (!message.epoch || !message.mode) throw Error('Missing topology fields');
    graph.mode = message.mode;
    for (const peer of this.roster(message)) graph.peers.add(peer);
    if (graph.mode === 'sfu') await this.offer(graph, this.session(graph, 'sfu'));
    else for (const peer of graph.peers) {
      const session = this.session(graph, peer);
      if (this.id < peer) await this.offer(graph, session);
    }
    return graph;
  }
  private async checkReady(): Promise<void> {
    const graph = this.pending;
    if (!graph || graph.ready) return;
    if (graph.mode === 'sfu' && !graph.sessions.has(sessionId('sfu', graph.epoch, this.id))) return;
    for (const session of this.active?.sessions.values() ?? []) {
      if (session.peer === 'sfu' || !graph.peers.has(session.peer)) continue;
      const recent = Date.now() - 1500;
      if ((session.videoAt > recent || session.audioAt > recent) &&
          !graph.sessions.has(sessionId(graph.mode, graph.epoch, this.id, session.peer))) return;
    }
    for (const session of graph.sessions.values()) {
      if (session.pc.connectionState !== 'connected') return;
      if ((graph.mode === 'mesh' || session.peer === 'sfu') &&
          ![...session.channels].some(channel => channel.readyState === 'open')) return;
      if (graph.mode === 'sfu' && session.peer === 'sfu') continue;
      const decoded = new Set<string>();
      for (const report of (await session.pc.getStats()).values()) {
        if (report.type !== 'inbound-rtp') continue;
        if (report.kind === 'video' && report.framesDecoded > 0) decoded.add('video');
        if (report.kind === 'audio' && report.totalSamplesReceived > 0) decoded.add('audio');
      }
      const previous = [...this.active?.sessions.values() ?? []].find(value => value.peer === session.peer);
      for (const kind of session.receiveKinds) {
        const last = kind === 'video' ? previous?.videoAt : previous?.audioAt;
        if ((!previous || (last ?? 0) > Date.now() - 1500) && !decoded.has(kind)) return;
      }
    }
    if (this.pending !== graph || this.closed) return;
    graph.ready = true;
    this.event(`ready:${graph.epoch}`);
    this.send('topology_ready', { epoch: graph.epoch, mode: graph.mode });
  }
  private async observeActive(): Promise<void> {
    for (const session of this.active?.sessions.values() ?? []) {
      if (session.peer === 'sfu') continue;
      let video = 0, audio = 0;
      for (const report of (await session.pc.getStats()).values()) {
        if (report.type !== 'inbound-rtp') continue;
        if (report.kind === 'video') video += report.framesDecoded ?? 0;
        if (report.kind === 'audio') audio += report.totalSamplesReceived ?? 0;
      }
      if (video > session.videoFrames) session.videoAt = Date.now();
      if (audio > session.audioSamples) session.audioAt = Date.now();
      session.videoFrames = video;
      session.audioSamples = audio;
    }
  }
  sendChat(text: string): void {
    if (!this.active || !text.trim()) throw Error('Join before sending a message');
    const sessions = [...this.active.sessions.values()].filter(session =>
      this.active!.mode === 'mesh' || session.peer === 'sfu');
    const channels = sessions.map(session => [...session.channels].find(channel => channel.readyState === 'open'));
    if (!channels.length || channels.some(channel => !channel)) throw Error('Chat transport is not ready');
    const payload = envelope(this.active.mode === 'sfu' ? '*' : this.id, ++this.sequence, text);
    if (payload.byteLength > 65535) throw Error('Chat message is too large');
    if (channels.some(channel => channel!.bufferedAmount + payload.byteLength > 1024 * 1024))
      throw Error('Chat transport is backpressured; retry after queued messages drain');
    for (const channel of channels) channel!.send(payload);
    this.view.message(this.id, text);
  }
  mute(kind: 'audio' | 'video', muted: boolean): void {
    for (const track of this.source?.stream.getTracks() ?? []) if (track.kind === kind) track.enabled = !muted;
  }
  async renegotiate(direction: RTCRtpTransceiverDirection = 'sendrecv', restart = false): Promise<void> {
    if (!this.active) throw Error('Not joined');
    for (const session of this.active.sessions.values()) {
      if (this.active.mode === 'sfu' && session.peer !== 'sfu') continue;
      for (const transceiver of session.pc.getTransceivers())
        if (transceiver.sender.track?.kind === 'video') transceiver.direction = direction;
      await this.offer(this.active, session, restart);
    }
  }
  async stats(): Promise<unknown> {
    const sources: Record<string, { video: number; audio: number; epoch: number }> = {};
    const connections = [];
    for (const graph of [this.active, this.pending, this.retiring]) {
      if (!graph) continue;
      for (const session of graph.sessions.values()) {
        const value = { video: 0, audio: 0, epoch: graph.epoch };
        for (const report of (await session.pc.getStats()).values()) {
          if (report.type !== 'inbound-rtp') continue;
          if (report.kind === 'video') value.video += report.framesDecoded ?? 0;
          if (report.kind === 'audio') value.audio += report.totalSamplesReceived ?? 0;
        }
        if (graph === this.active && session.peer !== 'sfu') sources[session.peer] = value;
        connections.push({ id: session.id, peer: session.peer, epoch: graph.epoch,
          state: session.pc.connectionState, answers: session.answers,
          channels: [...session.channels].map(channel => channel.readyState) });
      }
    }
    return { id: this.id, mode: this.active?.mode, epoch: this.active?.epoch, pendingEpoch: this.pending?.epoch,
      sources, connections, errors: [...this.errors], events: [...this.events], messages: [...this.messages],
      capture: this.source?.stream.getTracks().map(track => ({ kind: track.kind, state: track.readyState })) };
  }
  private closeSession(session: Session): void {
    session.pc.onicecandidate = null;
    session.pc.ontrack = null;
    session.pc.onconnectionstatechange = null;
    session.pc.ondatachannel = null;
    for (const channel of session.channels) {
      channel.onmessage = null;
      channel.onclose = null;
      channel.close();
    }
    session.channels.clear();
    session.pc.close();
    session.decoder.srcObject = null;
    session.decoder.remove();
  }
  private closeGraph(graph: Graph | undefined): void {
    for (const session of graph?.sessions.values() ?? []) this.closeSession(session);
    graph?.sessions.clear();
  }
  async leave(): Promise<void> {
    this.closed = true;
    ++this.generation;
    window.clearInterval(this.poll);
    window.clearTimeout(this.retirement);
    if (this.ws?.readyState === WebSocket.OPEN) this.send('leave');
    this.ws?.close();
    this.ws = undefined;
    for (const graph of [this.active, this.pending, this.retiring]) this.closeGraph(graph);
    this.active = this.pending = this.retiring = undefined;
    const source = this.source;
    this.source = undefined;
    await source?.close();
    for (const id of this.names.keys()) this.view.remove(id);
    this.names.clear();
    this.view.local(null);
    this.view.status('Not connected');
    this.id = '';
  }
}
