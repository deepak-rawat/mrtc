export type Mode = 'mesh' | 'sfu';
export interface Signal {
  type: string;
  version?: number;
  epoch?: number;
  session_id?: string;
  peer_id?: string;
  display_name?: string;
  peers?: string[];
  peer_names?: Record<string, string>;
  mode?: Mode;
  from?: string;
  to?: string;
  sdp?: string;
  candidate?: string;
  reason?: string;
  message?: string;
}
export const validPeer = (id: string): boolean => /^[A-Za-z0-9_-]{1,15}$/.test(id);
export function sessionId(mode: Mode, epoch: number, self: string, remote = 'sfu'): string {
  if (mode === 'mesh') return `m:${epoch}:${[self, remote].sort().join(':')}`;
  return remote === 'sfu' ? `p:${epoch}:${self}` : `s:${epoch}:${self}:${remote}`;
}
export function parseSignal(raw: string): Signal {
  if (raw.length > 65536) throw Error('Signaling message too large');
  const value: unknown = JSON.parse(raw);
  if (typeof value !== 'object' || value === null) throw Error('Invalid signaling object');
  const object: Record<string, unknown> = Object.fromEntries(Object.entries(value));
  if (typeof object.type !== 'string') throw Error('Missing signaling type');
  for (const key of ['session_id', 'peer_id', 'display_name', 'from', 'to', 'sdp', 'candidate', 'reason', 'message'])
    if (object[key] !== undefined && typeof object[key] !== 'string') throw Error(`Invalid ${key}`);
  if (object.version !== undefined && object.version !== 2) throw Error('Server does not support protocol v2');
  if (object.epoch !== undefined && (!Number.isInteger(object.epoch) || typeof object.epoch !== 'number' ||
      object.epoch < 1 || object.epoch > 0xffffffff)) throw Error('Invalid epoch');
  if (object.mode !== undefined && object.mode !== 'mesh' && object.mode !== 'sfu') throw Error('Invalid topology');
  if (object.peers !== undefined && (!Array.isArray(object.peers) || object.peers.length > 8 ||
      !object.peers.every(id => typeof id === 'string' && validPeer(id)))) throw Error('Invalid roster');
  if (object.peer_names !== undefined && (typeof object.peer_names !== 'object' || object.peer_names === null ||
      !Object.values(object.peer_names).every(name => typeof name === 'string'))) throw Error('Invalid peer names');
  if (['joined', 'topology_prepare', 'topology_ready', 'topology_commit', 'topology_ack',
    'topology_abort', 'topology_request'].includes(object.type)) {
    if (!object.epoch || !object.mode) throw Error('Missing topology addressing');
    if ((object.type === 'joined' || object.type === 'topology_prepare') && !Array.isArray(object.peers))
      throw Error('Missing topology roster');
    if ((object.type === 'topology_abort' || object.type === 'topology_request') && !object.reason)
      throw Error('Missing topology reason');
  }
  if (['offer', 'answer', 'candidate'].includes(object.type)) {
    if (!object.epoch || !object.session_id || !object.from) throw Error('Missing session addressing');
    if (object.type === 'candidate' ? typeof object.candidate !== 'string' : !object.sdp)
      throw Error('Missing session payload');
  }
  return object as unknown as Signal;
}
const encoder = new TextEncoder();
const decoder = new TextDecoder();
export function envelope(peer: string, id: bigint, text: string): ArrayBuffer {
  if ((peer !== '*' && !validPeer(peer)) || id <= 0n || id > 0xffffffffffffffffn) throw Error('Invalid message identity');
  const payload = encoder.encode(text);
  const bytes = new Uint8Array(28 + payload.length);
  bytes.set(encoder.encode('MRC2'));
  bytes.set(encoder.encode(peer), 4);
  new DataView(bytes.buffer).setBigUint64(20, id, false);
  bytes.set(payload, 28);
  return bytes.buffer;
}
export class MessageHistory {
  private readonly senders = new Map<string, Set<bigint>>();
  read(data: ArrayBuffer): { peer: string; text: string } | undefined {
    const bytes = new Uint8Array(data);
    if (bytes.length > 65536) throw Error('Chat envelope too large');
    if (bytes.length < 20) throw Error('Truncated chat envelope');
    const magic = decoder.decode(bytes.subarray(0, 4));
    if (magic !== 'MRC2' && magic !== 'MRTC') throw Error('Invalid chat envelope');
    const field = bytes.subarray(4, 20);
    const end = field.indexOf(0);
    if (end < 1) throw Error('Invalid sender field');
    const peer = decoder.decode(field.subarray(0, end));
    if (!validPeer(peer)) throw Error('Invalid sender identity');
    if (magic === 'MRC2') {
      if (bytes.length < 28) throw Error('Truncated v2 envelope');
      const id = new DataView(data).getBigUint64(20, false);
      if (!id) throw Error('Zero message ID');
      let seen = this.senders.get(peer);
      if (!seen) {
        if (this.senders.size >= 16) this.senders.delete(this.senders.keys().next().value!);
        seen = new Set();
        this.senders.set(peer, seen);
      }
      if (seen.has(id)) return;
      seen.add(id);
      if (seen.size > 256) seen.delete(seen.values().next().value!);
    }
    return { peer, text: decoder.decode(bytes.subarray(magic === 'MRC2' ? 28 : 20)) };
  }
}
