#!/usr/bin/env node
// Links the frontend's HTTP calls to the Spring handlers they reach, in a Sourcetrail database that holds both the Java
// and the TypeScript index. Each handler gets an endpoint node `HTTP.<VERB> <path>` (ANY for a @RequestMapping without
// a method) declared at its mapping annotation, with a CALL edge to the handler method there. Each call whose verb and
// normalised path match exactly one handler gets a CALL edge from the calling TypeScript method to that endpoint, at
// the call. The previous endpoint layer is removed first, so a run always reflects the current index. Calls that match
// no handler or several, or whose URL is not resolved, are not linked; they are listed in the log.
//
// Usage: link.mjs --db <db> --http-calls <ts-http-calls.json> --project <TypeScript project dir> [--log <file>]
// Prints "link: <n> linked, <a> ambiguous, <u> unresolved". Exit codes: 0 linked; 1 failed; 2 bad usage.
import fs from 'node:fs';
import path from 'node:path';
import { matchingHandlers, normalizePath, springHandlers } from './endpoints.mjs';

// node:sqlite still announces itself as experimental; Sourcetrail reads anything on stderr as a failed command.
process.removeAllListeners('warning');
process.on('warning', warning => {
  if (warning.name !== 'ExperimentalWarning') process.stderr.write(`${warning.name}: ${warning.message}\n`);
});

const USAGE = 'link: usage: link.mjs --db <db> --http-calls <json> --project <dir> [--log <file>]';
// NodeKind.h, Edge.h, LocationType.h, DefinitionKind.h
const NODE = { PACKAGE: 32, FUNCTION: 4096, METHOD: 8192 };
const EDGE = { MEMBER: 1, CALL: 8 };
const TOKEN = 0;
const EXPLICIT = 2;

// The endpoint layer's names. Sourcetrail's refresh leaves nodes under this prefix out of what a file declares
// (SqliteIndexStorage), since this step rebuilds them.
const HTTP_PACKAGE = '.\tmHTTP\ts\tp';
const endpointName = (verb, endpointPath) => `${HTTP_PACKAGE}\tn${verb} ${endpointPath}\ts\tp`;

function parseArguments(argv) {
  const options = {};
  for (let i = 0; i < argv.length; i++) {
    const key = { '--db': 'db', '--http-calls': 'calls', '--project': 'project', '--log': 'log' }[argv[i]];
    if (!key || i + 1 >= argv.length) throw Object.assign(new Error(USAGE), { code: 2 });
    options[key] = path.resolve(argv[++i]);
  }
  if (!options.db || !options.calls || !options.project) throw Object.assign(new Error(USAGE), { code: 2 });
  return options;
}

// "com.acme.shop.OrderResource.list" for a serialized name hierarchy
function displayName(serialized) {
  const start = serialized.indexOf('\tm');
  const names = serialized.slice(start + 2).split('\tn').map(e => e.slice(0, e.indexOf('\ts')));
  return names.join(serialized.slice(0, start));
}

function removeEndpointLayer(db) {
  db.exec('CREATE TEMP TABLE IF NOT EXISTS endpoint_element(id INTEGER PRIMARY KEY)');
  db.exec('DELETE FROM endpoint_element');
  db.prepare(`INSERT INTO endpoint_element SELECT id FROM node WHERE type = ? AND substr(serialized_name, 1, ?) = ?`)
    .run(NODE.FUNCTION, HTTP_PACKAGE.length + 2, `${HTTP_PACKAGE}\tn`);
  db.exec(`INSERT OR IGNORE INTO endpoint_element SELECT id FROM edge
    WHERE source_node_id IN (SELECT id FROM endpoint_element) OR target_node_id IN (SELECT id FROM endpoint_element)`);
  db.exec('CREATE TEMP TABLE IF NOT EXISTS endpoint_location(id INTEGER PRIMARY KEY)');
  db.exec('DELETE FROM endpoint_location');
  db.exec(`INSERT OR IGNORE INTO endpoint_location SELECT source_location_id FROM occurrence
    WHERE element_id IN (SELECT id FROM endpoint_element)`);
  db.exec('DELETE FROM occurrence WHERE element_id IN (SELECT id FROM endpoint_element)');
  db.exec(`DELETE FROM source_location WHERE id IN (SELECT id FROM endpoint_location)
    AND id NOT IN (SELECT source_location_id FROM occurrence)`);
  db.exec('DELETE FROM edge WHERE id IN (SELECT id FROM endpoint_element)');
  db.exec('DELETE FROM symbol WHERE id IN (SELECT id FROM endpoint_element)');
  db.exec('DELETE FROM node WHERE id IN (SELECT id FROM endpoint_element)');
  db.exec('DELETE FROM element WHERE id IN (SELECT id FROM endpoint_element)');
  // The package goes too, unless something else (not an endpoint) is in it.
  const pkg = db.prepare('SELECT id FROM node WHERE serialized_name = ?').get(HTTP_PACKAGE);
  if (pkg && !db.prepare('SELECT 1 FROM edge WHERE source_node_id = ? OR target_node_id = ?').get(pkg.id, pkg.id)
    && !db.prepare('SELECT 1 FROM occurrence WHERE element_id = ?').get(pkg.id)) {
    db.prepare('DELETE FROM node WHERE id = ?').run(pkg.id);
    db.prepare('DELETE FROM element WHERE id = ?').run(pkg.id);
  }
}

// Every handler in the index's Java files: {verb, path, fileId, start, end, nodeId, name}; nodeId is the first method
// declared after the annotation (null if there is none), name its display name. Also the mappings that could not be
// read, as {where, message}. The database may lack its indexes while Sourcetrail builds it: one query per table.
function collectHandlers(db) {
  const handlers = [], problems = [];
  const files = db.prepare(`SELECT file.id, file.path, filecontent.content FROM file
    LEFT JOIN filecontent ON filecontent.id = file.id WHERE file.path LIKE '%.java'`).all();
  const found = new Map(); // file id -> its handlers
  for (const file of files) {
    const text = file.content ?? (fs.existsSync(file.path) ? fs.readFileSync(file.path, 'utf8') : '');
    if (!text.includes('Mapping')) continue;
    const mappings = springHandlers(text);
    for (const p of mappings.problems) problems.push({ where: `${file.path}:${p.line}`, message: p.message });
    if (mappings.handlers.length > 0) found.set(file.id, { path: file.path, handlers: mappings.handlers });
  }
  if (found.size === 0) return { handlers, problems };

  const methods = new Map(); // file id -> [{line, column, id, serialized_name}] in file order
  for (const m of db.prepare(`SELECT source_location.file_node_id AS file, source_location.start_line AS line,
      source_location.start_column AS column, node.id, node.serialized_name FROM source_location
      JOIN occurrence ON occurrence.source_location_id = source_location.id JOIN node ON node.id = occurrence.element_id
      WHERE source_location.type = ? AND node.type = ?
      AND source_location.file_node_id IN (SELECT value FROM json_each(?))`)
    .all(TOKEN, NODE.METHOD, JSON.stringify([...found.keys()]))) {
    if (!methods.has(m.file)) methods.set(m.file, []);
    methods.get(m.file).push(m);
  }
  for (const list of methods.values()) list.sort((a, b) => a.line - b.line || a.column - b.column);

  for (const [fileId, { path: filePath, handlers: fileHandlers }] of found) {
    for (const h of fileHandlers) {
      const after = m => m.line > h.end.line || (m.line === h.end.line && m.column > h.end.column);
      const method = (methods.get(fileId) ?? []).find(after);
      handlers.push({ ...h, fileId, nodeId: method?.id ?? null,
        name: method ? displayName(method.serialized_name) : `${filePath}:${h.start.line}` });
    }
  }
  return { handlers, problems };
}

function readCalls(file) {
  if (!fs.existsSync(file)) return [];
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

// Writes the endpoint layer, once the previous one is gone: every endpoint, and every edge (they touch endpoints), is
// new.
class Writer {
  constructor(db) {
    const max = table => db.prepare(`SELECT max(id) AS m FROM ${table}`).get().m ?? 0;
    this.nextId = max('element') + 1;
    this.nextLocationId = max('source_location') + 1;
    this.edges = new Map(); // "type|source|target" -> id
    this.insert = {
      element: db.prepare('INSERT INTO element(id) VALUES (?)'),
      node: db.prepare('INSERT INTO node(id, type, serialized_name) VALUES (?, ?, ?)'),
      symbol: db.prepare('INSERT INTO symbol(id, definition_kind) VALUES (?, ?)'),
      edge: db.prepare('INSERT INTO edge(id, type, source_node_id, target_node_id) VALUES (?, ?, ?, ?)'),
      location: db.prepare(`INSERT INTO source_location(id, file_node_id, start_line, start_column, end_line,
        end_column, type) VALUES (?, ?, ?, ?, ?, ?, ?)`),
      occurrence: db.prepare('INSERT INTO occurrence(element_id, source_location_id) VALUES (?, ?)'),
    };
    this.nodes = new Map(); // serialized name -> id
    // The package may hold more than endpoints, and stay.
    const pkg = db.prepare('SELECT id FROM node WHERE serialized_name = ?').get(HTTP_PACKAGE);
    if (pkg) this.nodes.set(HTTP_PACKAGE, pkg.id);
  }

  element() {
    const id = this.nextId++;
    this.insert.element.run(id);
    return id;
  }

  // {id, created}; an explicitly defined symbol if `definition` is given
  node(serialized, type, definition) {
    if (this.nodes.has(serialized)) return { id: this.nodes.get(serialized), created: false };
    const id = this.element();
    this.insert.node.run(id, type, serialized);
    this.nodes.set(serialized, id);
    if (definition !== undefined) this.insert.symbol.run(id, definition);
    return { id, created: true };
  }

  edge(type, source, target) {
    const key = `${type}|${source}|${target}`;
    if (!this.edges.has(key)) {
      const id = this.element();
      this.insert.edge.run(id, type, source, target);
      this.edges.set(key, id);
    }
    return this.edges.get(key);
  }

  // 1-based lines and columns, end column inclusive
  locate(elementId, fileId, start, end) {
    const id = this.nextLocationId++;
    this.insert.location.run(id, fileId, start.line, start.column, end.line, end.column, TOKEN);
    this.insert.occurrence.run(elementId, id);
  }
}

async function main(argv) {
  const options = parseArguments(argv);
  const calls = readCalls(options.calls);
  // Imported once the warning handler is in place.
  const { DatabaseSync } = await import('node:sqlite');
  const db = new DatabaseSync(options.db);
  // The layer is removed table by table; checking each deleted row's references would cost seconds on a large index.
  db.exec('PRAGMA busy_timeout = 10000; PRAGMA foreign_keys = OFF;');
  db.exec('BEGIN;');
  const log = [];
  let linked = 0, ambiguous = 0, unresolved = 0;
  try {
    removeEndpointLayer(db);
    const { handlers, problems } = collectHandlers(db);
    for (const p of problems) log.push(`mapping\t${p.where}\t${p.message}`);

    const writer = new Writer(db);
    const fileIds = new Map(db.prepare('SELECT path, id FROM file').all().map(f => [f.path, f.id]));
    const callerIds = new Map(db.prepare('SELECT serialized_name, id FROM node WHERE serialized_name IN '
      + '(SELECT value FROM json_each(?))').all(JSON.stringify(calls.map(c => c.source_name)))
      .map(n => [n.serialized_name, n.id]));
    // A call made from several declarations (overloads) is one call.
    const sites = new Map();
    for (const call of calls) {
      const key = `${call.file}:${call.line}:${call.col}`;
      if (!sites.has(key)) sites.set(key, { ...call, where: `${call.file}:${call.line}`, sources: [] });
      sites.get(key).sources.push(call.source_name);
    }
    // One endpoint per verb and path, at each of its handlers (several make it ambiguous).
    for (const handler of handlers.filter(h => h.nodeId !== null)) {
      const pkg = writer.node(HTTP_PACKAGE, NODE.PACKAGE).id;
      const endpoint = writer.node(endpointName(handler.verb, handler.path), NODE.FUNCTION, EXPLICIT);
      if (endpoint.created) writer.edge(EDGE.MEMBER, pkg, endpoint.id);
      writer.locate(endpoint.id, handler.fileId, handler.start, handler.end);
      writer.locate(writer.edge(EDGE.CALL, endpoint.id, handler.nodeId), handler.fileId, handler.start, handler.end);
      handler.endpointId = endpoint.id;
    }

    for (const site of sites.values()) {
      const report = (kind, url, reason) => log.push(`${kind}\t${site.verb}\t${url}\t${site.where}\t${reason}`);
      if (!site.url_resolved) {
        unresolved++;
        report('unresolved', site.url, 'URL not resolved');
        continue;
      }
      const callPath = normalizePath(site.url);
      const matches = matchingHandlers(handlers, site.verb, callPath);
      if (matches.length > 1) {
        ambiguous++;
        report('ambiguous', callPath, `${matches.length} handlers: ${matches.map(h => h.name).sort().join(', ')}`);
        continue;
      }
      const [handler] = matches;
      const tsFile = fileIds.get(path.join(options.project, site.file));
      const sources = site.sources.map(s => callerIds.get(s)).filter(id => id !== undefined);
      if (!handler) {
        unresolved++;
        report('unresolved', callPath, 'no handler');
      } else if (handler.nodeId === null) {
        unresolved++;
        report('unresolved', callPath, `no method after the mapping at ${handler.name}`);
      } else if (tsFile === undefined || sources.length === 0) {
        unresolved++;
        report('unresolved', callPath, 'calling method not in the index');
      } else {
        const at = { line: site.line, column: site.col };
        const verbEnd = { line: site.line, column: site.col + site.verb.length - 1 };
        for (const source of sources) {
          writer.locate(writer.edge(EDGE.CALL, source, handler.endpointId), tsFile, at, verbEnd);
        }
        linked++;
      }
    }
    db.exec('COMMIT;');
  } catch (e) {
    db.exec('ROLLBACK;');
    throw e;
  } finally {
    db.close();
  }
  if (options.log) fs.writeFileSync(options.log, log.length ? `${log.join('\n')}\n` : '');
  process.stdout.write(`link: ${linked} linked, ${ambiguous} ambiguous, ${unresolved} unresolved\n`);
}

main(process.argv.slice(2)).catch(e => {
  process.stderr.write(`${e.code === 2 ? e.message : `link: ${e.stack ?? e}`}\n`);
  process.exitCode = e.code === 2 ? 2 : 1;
});
