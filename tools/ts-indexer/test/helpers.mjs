// Shared helpers for the ts-indexer tests: run the CLI, read the database it wrote.
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { DatabaseSync } from 'node:sqlite';

const here = path.dirname(fileURLToPath(import.meta.url));
export const indexerCli = path.join(here, '..', 'index.mjs');
export const fixtureDir = path.resolve(here, '..', '..', '..', 'testing', 'cli', 'angular-spring');

export const NODE = { MODULE: 8, PACKAGE: 32, CLASS: 128, INTERFACE: 256, GLOBAL_VARIABLE: 1024, FIELD: 2048,
  FUNCTION: 4096, METHOD: 8192, FILE: 262144 };
export const EDGE = { MEMBER: 1, TYPE_USAGE: 2, USAGE: 4, CALL: 8, INHERITANCE: 16, OVERRIDE: 32, IMPORT: 512,
  ANNOTATION_USAGE: 4096 };

export function scratchDir(prefix) {
  return fs.mkdtempSync(path.join(os.tmpdir(), `ts-indexer-${prefix}-`));
}

export function runIndexer(args, nodeOptions = []) {
  const r = spawnSync(process.execPath, [...nodeOptions, indexerCli, ...args], { encoding: 'utf8' });
  return { code: r.status, stdout: r.stdout, stderr: r.stderr };
}

// "app.orders.order.service.OrderService.list" for a serialized name hierarchy (signatures dropped)
export function displayName(serialized) {
  const delimiter = serialized.slice(0, serialized.indexOf('\tm'));
  return serialized.slice(serialized.indexOf('\tm') + 2).split('\tn').map(e => e.slice(0, e.indexOf('\ts'))).join(delimiter);
}

export function openIndex(dbPath) {
  const db = new DatabaseSync(dbPath, { readOnly: true });
  const nodes = db.prepare('SELECT id, type, serialized_name FROM node').all()
    .map(n => ({ ...n, name: displayName(n.serialized_name) }));
  const byId = new Map(nodes.map(n => [n.id, n]));
  const edges = db.prepare('SELECT id, type, source_node_id, target_node_id FROM edge').all()
    .map(e => ({ ...e, source: byId.get(e.source_node_id), target: byId.get(e.target_node_id) }));
  return {
    db,
    nodes,
    edges,
    named: name => nodes.filter(n => n.name === name),
    edgesBetween: (source, target) => edges.filter(e => e.source.name === source && e.target.name === target),
    close: () => db.close(),
  };
}
