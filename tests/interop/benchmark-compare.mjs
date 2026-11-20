import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { compare, parseReport } from './benchmark-lib.mjs';

const args = process.argv.slice(2);
const split = args.indexOf('--candidate');
assert(args[0] === '--baseline' && split > 1 && split < args.length - 1 &&
    args.lastIndexOf('--candidate') === split,
    'Usage: node benchmark-compare.mjs --baseline before.jsonl [...] --candidate after.jsonl [...]');
const load = async paths => (await Promise.all(paths.map(async path =>
    parseReport(await readFile(path, 'utf8'), path)))).flat();
const result = compare(await load(args.slice(1, split)), await load(args.slice(split + 1)));
console.log(JSON.stringify(result, null, 2));
process.exitCode = result.regression ? 1 : result.insufficientEvidence ? 2 : 0;
