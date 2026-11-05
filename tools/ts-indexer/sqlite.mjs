// Writes Sourcetrail index rows (storage version 27, the schema of SqliteIndexStorage::setupTables).
import fs from 'node:fs';
import { DatabaseSync } from 'node:sqlite';

export const STORAGE_VERSION = '27';

// NodeKind.h, Edge.h, LocationType.h, DefinitionKind.h
export const NODE = { SYMBOL: 1, MODULE: 8, NAMESPACE: 16, PACKAGE: 32, CLASS: 128, INTERFACE: 256, ANNOTATION: 512,
  GLOBAL_VARIABLE: 1024, FIELD: 2048, FUNCTION: 4096, METHOD: 8192, ENUM: 16384, ENUM_CONSTANT: 32768, TYPEDEF: 65536,
  TYPE_PARAMETER: 131072, FILE: 262144 };
export const EDGE = { MEMBER: 1, TYPE_USAGE: 2, USAGE: 4, CALL: 8, INHERITANCE: 16, OVERRIDE: 32, IMPORT: 512,
  ANNOTATION_USAGE: 4096 };
export const LOCATION = { TOKEN: 0, SCOPE: 1, LOCAL_SYMBOL: 3, ERROR: 6 };
export const DEFINITION = { IMPLICIT: 1, EXPLICIT: 2 };

const SCHEMA = `
CREATE TABLE IF NOT EXISTS meta(id INTEGER, key TEXT, value TEXT, PRIMARY KEY(id));
CREATE TABLE IF NOT EXISTS element(id INTEGER, PRIMARY KEY(id));
CREATE TABLE IF NOT EXISTS element_component(id INTEGER, element_id INTEGER, type INTEGER, data TEXT, PRIMARY KEY(id), FOREIGN KEY(element_id) REFERENCES element(id) ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS edge(id INTEGER NOT NULL, type INTEGER NOT NULL, source_node_id INTEGER NOT NULL, target_node_id INTEGER NOT NULL, PRIMARY KEY(id), FOREIGN KEY(id) REFERENCES element(id) ON DELETE CASCADE, FOREIGN KEY(source_node_id) REFERENCES node(id) ON DELETE CASCADE, FOREIGN KEY(target_node_id) REFERENCES node(id) ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS node(id INTEGER NOT NULL, type INTEGER NOT NULL, serialized_name TEXT, PRIMARY KEY(id), FOREIGN KEY(id) REFERENCES element(id) ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS symbol(id INTEGER NOT NULL, definition_kind INTEGER NOT NULL, PRIMARY KEY(id), FOREIGN KEY(id) REFERENCES node(id) ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS file(id INTEGER NOT NULL, path TEXT, language TEXT, modification_time TEXT, indexed INTEGER, complete INTEGER, line_count INTEGER, PRIMARY KEY(id), FOREIGN KEY(id) REFERENCES node(id) ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS filecontent(id INTEGER, content TEXT, PRIMARY KEY(id), FOREIGN KEY(id) REFERENCES file(id)ON DELETE CASCADE ON UPDATE CASCADE);
CREATE TABLE IF NOT EXISTS local_symbol(id INTEGER NOT NULL, name TEXT, PRIMARY KEY(id), FOREIGN KEY(id) REFERENCES element(id) ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS source_location(id INTEGER NOT NULL, file_node_id INTEGER, start_line INTEGER, start_column INTEGER, end_line INTEGER, end_column INTEGER, type INTEGER, PRIMARY KEY(id), FOREIGN KEY(file_node_id) REFERENCES node(id) ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS occurrence(element_id INTEGER NOT NULL, source_location_id INTEGER NOT NULL, PRIMARY KEY(element_id, source_location_id), FOREIGN KEY(element_id) REFERENCES element(id) ON DELETE CASCADE, FOREIGN KEY(source_location_id) REFERENCES source_location(id) ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS component_access(node_id INTEGER NOT NULL, type INTEGER NOT NULL, PRIMARY KEY(node_id), FOREIGN KEY(node_id) REFERENCES node(id) ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS error(id INTEGER NOT NULL, message TEXT, fatal INTEGER NOT NULL, indexed INTEGER NOT NULL, translation_unit TEXT, PRIMARY KEY(id), FOREIGN KEY(id) REFERENCES element(id) ON DELETE CASCADE);
`;

const INDEXES = `
CREATE INDEX IF NOT EXISTS node_serialized_name_index ON node(serialized_name);
CREATE INDEX IF NOT EXISTS source_location_file_node_id_index ON source_location(file_node_id);
CREATE INDEX IF NOT EXISTS occurrence_element_id_index ON occurrence(element_id);
CREATE INDEX IF NOT EXISTS occurrence_source_location_id_index ON occurrence(source_location_id);
`;

// "YYYY-MM-DD HH:MM:SS" in local time, as Sourcetrail stores file modification times
export function timestamp(date) {
  const p = n => String(n).padStart(2, '0');
  return `${date.getFullYear()}-${p(date.getMonth() + 1)}-${p(date.getDate())} `
    + `${p(date.getHours())}:${p(date.getMinutes())}:${p(date.getSeconds())}`;
}

// One writer per run. Without `append` the database is created anew; with it, rows are added to an existing
// database (for example one the Java indexer wrote), reusing nodes that already exist under the same name. `language`
// is what the file rows record.
export class IndexWriter {
  constructor(dbPath, { append = false, language = 'typescript' } = {}) {
    this.append = append && fs.existsSync(dbPath);
    if (!this.append) fs.rmSync(dbPath, { force: true });
    this.db = new DatabaseSync(dbPath);
    if (this.append) {
      this.db.exec('PRAGMA busy_timeout = 10000;');
      const version = this.db.prepare("SELECT value FROM meta WHERE key = 'storage_version'").get()?.value;
      if (version !== undefined && version !== STORAGE_VERSION) {
        this.db.close();
        throw new Error(`${dbPath} has storage version ${version}, not ${STORAGE_VERSION}`);
      }
    } else {
      this.db.exec('PRAGMA journal_mode = OFF; PRAGMA synchronous = OFF;');
    }
    this.db.exec(SCHEMA);
    this.db.exec('BEGIN;');

    const prepare = sql => this.db.prepare(sql);
    this.insert = {
      element: prepare('INSERT INTO element(id) VALUES (?)'),
      node: prepare('INSERT INTO node(id, type, serialized_name) VALUES (?, ?, ?)'),
      nodeType: prepare('UPDATE node SET type = ? WHERE id = ?'),
      symbol: prepare('INSERT OR REPLACE INTO symbol(id, definition_kind) VALUES (?, ?)'),
      edge: prepare('INSERT INTO edge(id, type, source_node_id, target_node_id) VALUES (?, ?, ?, ?)'),
      file: prepare('INSERT OR REPLACE INTO file(id, path, language, modification_time, indexed, complete, line_count) '
        + 'VALUES (?, ?, ?, ?, 1, 1, ?)'),
      content: prepare('INSERT OR REPLACE INTO filecontent(id, content) VALUES (?, ?)'),
      local: prepare('INSERT INTO local_symbol(id, name) VALUES (?, ?)'),
      location: prepare('INSERT INTO source_location(id, file_node_id, start_line, start_column, end_line, end_column, '
        + 'type) VALUES (?, ?, ?, ?, ?, ?, ?)'),
      occurrence: prepare('INSERT OR IGNORE INTO occurrence(element_id, source_location_id) VALUES (?, ?)'),
      error: prepare('INSERT INTO error(id, message, fatal, indexed, translation_unit) VALUES (?, ?, ?, 1, ?)'),
      metaUpdate: prepare('UPDATE meta SET value = ? WHERE key = ?'),
      metaInsert: prepare('INSERT INTO meta(key, value) VALUES (?, ?)'),
    };
    const max = table => this.db.prepare(`SELECT max(id) AS m FROM ${table}`).get().m ?? 0;
    this.nextId = max('element') + 1;
    this.nextLocationId = max('source_location') + 1;
    this.nodes = new Map(); // serialized name -> {id, kind}
    for (const r of this.db.prepare('SELECT id, type, serialized_name FROM node').all()) {
      this.nodes.set(r.serialized_name, { id: r.id, kind: r.type });
    }
    this.edges = new Map(); // "type|source|target" -> id
    for (const r of this.db.prepare('SELECT id, type, source_node_id, target_node_id FROM edge').all()) {
      this.edges.set(`${r.type}|${r.source_node_id}|${r.target_node_id}`, r.id);
    }
    this.locations = new Map(); // "file|sl|sc|el|ec|type" -> id
    this.language = language;
    this.fileCount = 0;
    this.errorCount = 0;
  }

  newElement() {
    const id = this.nextId++;
    this.insert.element.run(id);
    return id;
  }

  // A node for a serialized name; its parent (`parentId`) gets a MEMBER edge. An existing node keeps its id; a
  // placeholder kind (package for a module path, or symbol) is upgraded to the real one.
  node(serialized, kind, parentId) {
    const known = this.nodes.get(serialized);
    if (known) {
      if ((known.kind === NODE.PACKAGE && kind === NODE.MODULE) || known.kind === NODE.SYMBOL) {
        this.insert.nodeType.run(kind, known.id);
        known.kind = kind;
      }
      return known.id;
    }
    const id = this.newElement();
    this.insert.node.run(id, kind, serialized);
    this.nodes.set(serialized, { id, kind });
    if (parentId !== undefined) this.edge(EDGE.MEMBER, parentId, id);
    return id;
  }

  symbol(nodeId, definitionKind) {
    this.insert.symbol.run(nodeId, definitionKind);
  }

  edge(type, sourceId, targetId) {
    const key = `${type}|${sourceId}|${targetId}`;
    let id = this.edges.get(key);
    if (id === undefined) {
      id = this.newElement();
      this.insert.edge.run(id, type, sourceId, targetId);
      this.edges.set(key, id);
    }
    return id;
  }

  file(absolutePath, text, modified) {
    const id = this.node(`/\tm${absolutePath}\ts\tp`, NODE.FILE);
    this.insert.file.run(id, absolutePath, this.language, timestamp(modified), text.split('\n').length);
    this.insert.content.run(id, text);
    this.fileCount++;
    return id;
  }

  local(name) {
    const id = this.newElement();
    this.insert.local.run(id, name);
    return id;
  }

  // range: 0-based [line, column, endLine, endColumn) as SCIP has it (or [line, column, endColumn]);
  // stored 1-based with an inclusive end column.
  location(fileId, range, type, elementId) {
    const [sl, sc, el, ec] = range.length === 3 ? [range[0], range[1], range[0], range[2]] : range;
    const key = `${fileId}|${sl}|${sc}|${el}|${ec}|${type}`;
    let id = this.locations.get(key);
    if (id === undefined) {
      id = this.nextLocationId++;
      this.insert.location.run(id, fileId, sl + 1, sc + 1, el + 1, Math.max(ec, sc + 1), type);
      this.locations.set(key, id);
    }
    this.insert.occurrence.run(elementId, id);
  }

  error(fileId, absolutePath, line, column, message) {
    const id = this.newElement();
    this.insert.error.run(id, message, 0, absolutePath);
    this.location(fileId, [line, column, column + 1], LOCATION.ERROR, id);
    this.errorCount++;
  }

  meta(key, value) {
    if (this.insert.metaUpdate.run(value, key).changes === 0) this.insert.metaInsert.run(key, value);
  }

  finish() {
    this.meta('storage_version', STORAGE_VERSION);
    if (!this.append) this.meta('timestamp', timestamp(new Date()));
    this.db.exec('COMMIT;');
    this.db.exec(INDEXES);
    this.db.close();
  }

  abort() {
    try {
      this.db.exec('ROLLBACK;');
    } finally {
      this.db.close();
    }
  }
}
