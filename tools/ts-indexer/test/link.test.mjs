import { before, describe, test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { DatabaseSync } from 'node:sqlite';

import { EDGE, NODE, fixtureDir, openIndex, runIndexer, scratchDir } from './helpers.mjs';

const linkCli = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'link.mjs');
const SHOP = 'src/main/java/com/acme/shop';
const SERVICE = 'app.orders.order.service.OrderService';
const RESOURCE = 'com.acme.shop.OrderResource';
const ORDER_SERVICE_TS = 'src/main/webapp/app/orders/order.service.ts';

// What the Java indexer would have written for the fixture's handler methods (and the Feign client's), enough for the
// linking: a file node with its content and each method node declared at its name.
const JAVA_METHODS = {
  [`${SHOP}/OrderResource.java`]: [RESOURCE, ['list', 'create', 'get']],
  [`${SHOP}/PaymentClient.java`]: ['com.acme.shop.PaymentClient', ['authorize']],
};

function serializedName(dotted) {
  return `.\tm${dotted.split('.').map(n => `${n}\ts\tp`).join('\tn')}`;
}

function addJava(dbPath, root, javaMethods) {
  const db = new DatabaseSync(dbPath);
  let id = db.prepare('SELECT max(id) AS m FROM element').get().m + 1;
  let locationId = (db.prepare('SELECT max(id) AS m FROM source_location').get().m ?? 0) + 1;
  const element = () => {
    db.prepare('INSERT INTO element(id) VALUES (?)').run(id);
    return id++;
  };
  for (const [relative, [className, methods]] of Object.entries(javaMethods)) {
    const abs = path.join(root, relative);
    const text = fs.readFileSync(abs, 'utf8');
    const fileId = element();
    db.prepare('INSERT INTO node(id, type, serialized_name) VALUES (?, ?, ?)').run(fileId, NODE.FILE, `/\tm${abs}\ts\tp`);
    db.prepare("INSERT INTO file VALUES (?, ?, 'java', '2026-01-01 00:00:00', 1, 1, ?)").run(fileId, abs, text.split('\n').length);
    db.prepare('INSERT INTO filecontent(id, content) VALUES (?, ?)').run(fileId, text);
    const lines = text.split('\n');
    for (const method of methods) {
      const line = lines.findIndex(l => new RegExp(`^\\s*(public\\s+)?[\\w<>, ]+\\s${method}\\s*\\(`).test(l));
      assert.ok(line >= 0, `${method} in ${relative}`);
      const column = lines[line].indexOf(`${method}(`) + 1;
      const nodeId = element();
      db.prepare('INSERT INTO node(id, type, serialized_name) VALUES (?, ?, ?)')
        .run(nodeId, NODE.METHOD, serializedName(`${className}.${method}`));
      db.prepare('INSERT INTO symbol(id, definition_kind) VALUES (?, 2)').run(nodeId);
      db.prepare('INSERT INTO source_location VALUES (?, ?, ?, ?, ?, ?, 0)')
        .run(locationId, fileId, line + 1, column, line + 1, column + method.length - 1);
      db.prepare('INSERT INTO occurrence VALUES (?, ?)').run(nodeId, locationId++);
    }
  }
  db.close();
}

// A copy of the fixture, changed by `edit(root)`, indexed: TypeScript by the ts-indexer, Java as above.
function mixedIndex(prefix, edit = () => {}, javaMethods = JAVA_METHODS) {
  const dir = scratchDir(prefix);
  const root = path.join(dir, 'angular-spring');
  fs.cpSync(fixtureDir, root, { recursive: true });
  edit(root);
  const db = path.join(dir, 'mixed.srctrldb');
  const r = runIndexer(['--project', root, '--tsconfig', path.join(root, 'tsconfig.json'), '--out', db]);
  assert.equal(r.code, 0, r.stderr);
  addJava(db, root, javaMethods);
  return { dir, root, db, calls: path.join(dir, 'ts-http-calls.json'), log: path.join(dir, '.st-links.log') };
}

function link({ db, calls, root, log }) {
  const r = spawnSync(process.execPath, [linkCli, '--db', db, '--http-calls', calls, '--project', root, '--log', log],
    { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr + r.stdout);
  assert.equal(r.stderr, '');
  return r.stdout;
}

// [TypeScript caller, endpoint, Java handler] for each pair of CALL edges through an endpoint node
function links(index) {
  const endpoints = index.nodes.filter(n => n.name.startsWith('HTTP.'));
  const out = [];
  for (const endpoint of endpoints) {
    const into = index.edges.filter(e => e.type === EDGE.CALL && e.target.id === endpoint.id);
    const from = index.edges.filter(e => e.type === EDGE.CALL && e.source.id === endpoint.id);
    for (const i of into) for (const f of from) out.push([i.source.name, endpoint.name, f.target.name]);
  }
  return out.sort();
}

function endpointLocation(index, name) {
  return index.db.prepare(`SELECT f.path, l.start_line, l.start_column, l.end_line, l.end_column, l.type
    FROM node n JOIN occurrence o ON o.element_id = n.id JOIN source_location l ON l.id = o.source_location_id
    JOIN file f ON f.id = l.file_node_id WHERE n.id = ?`).all(index.named(name)[0].id)
    .map(r => [path.basename(r.path), r.start_line, r.start_column, r.end_line, r.end_column, r.type]);
}

const FIXTURE_LINKS = [
  [`${SERVICE}.create`, 'HTTP.POST /api/orders', `${RESOURCE}.create`],
  [`${SERVICE}.get`, 'HTTP.GET /api/orders/{}', `${RESOURCE}.get`],
  [`${SERVICE}.list`, 'HTTP.GET /api/orders', `${RESOURCE}.list`],
];

describe('endpoint linking', () => {
  let fixture;
  before(() => {
    fixture = mixedIndex('link');
  });

  test('linksFixtureCalls', () => {
    const out = link(fixture);
    assert.equal(out, 'link: 3 linked, 0 ambiguous, 1 unresolved\n');
    const index = openIndex(fixture.db);
    try {
      assert.deepEqual(links(index), FIXTURE_LINKS);
      assert.deepEqual(index.nodes.filter(n => n.name.startsWith('HTTP')).map(n => [n.name, n.type]).sort(), [
        ['HTTP', NODE.PACKAGE],
        ['HTTP.GET /api/orders', NODE.FUNCTION],
        ['HTTP.GET /api/orders/{}', NODE.FUNCTION],
        ['HTTP.POST /api/orders', NODE.FUNCTION],
      ]);
      // declared at the Spring annotation: `@GetMapping("/{id}")` on line 30
      assert.deepEqual(endpointLocation(index, 'HTTP.GET /api/orders/{}'), [['OrderResource.java', 30, 5, 30, 24, 0]]);
      // the TypeScript side of the link is at the call's verb, `this.http.get<Order[]>('api/orders')` on line 17
      const [call] = index.edgesBetween(`${SERVICE}.list`, 'HTTP.GET /api/orders');
      const at = index.db.prepare(`SELECT f.path, l.start_line, l.start_column, l.end_column FROM occurrence o
        JOIN source_location l ON l.id = o.source_location_id JOIN file f ON f.id = l.file_node_id
        WHERE o.element_id = ?`).all(call.id).map(r => [path.relative(fixture.root, r.path), r.start_line,
        r.start_column, r.end_column]);
      assert.deepEqual(at, [[ORDER_SERVICE_TS, 17, 22, 24]]);
    } finally {
      index.close();
    }
  });

  test('relinkingReplacesTheEndpointLayer', () => {
    link(fixture);
    const count = table => {
      const db = new DatabaseSync(fixture.db, { readOnly: true });
      try {
        return db.prepare(`SELECT count(*) AS c FROM ${table}`).get().c;
      } finally {
        db.close();
      }
    };
    const before = ['node', 'edge', 'source_location', 'occurrence', 'element', 'symbol'].map(count);
    assert.equal(link(fixture), 'link: 3 linked, 0 ambiguous, 1 unresolved\n');
    assert.deepEqual(['node', 'edge', 'source_location', 'occurrence', 'element', 'symbol'].map(count), before);
    const index = openIndex(fixture.db);
    try {
      assert.deepEqual(links(index), FIXTURE_LINKS);
    } finally {
      index.close();
    }
  });

  test('unresolvedUrlIsReportedNotGuessed', () => {
    link(fixture);
    const index = openIndex(fixture.db);
    try {
      // download(location) gets a URL known only at runtime
      assert.deepEqual(index.edges.filter(e => e.source.name === `${SERVICE}.download` && e.target.name.startsWith('HTTP')), []);
    } finally {
      index.close();
    }
    const log = fs.readFileSync(fixture.log, 'utf8');
    assert.match(log, /^unresolved\tGET\tlocation\tsrc\/main\/webapp\/app\/orders\/order\.service\.ts:35\tURL not resolved$/m);
    assert.doesNotMatch(log, /ambiguous/);
  });

  test('ambiguousHandlerLeavesCallUnlinked', () => {
    const ambiguous = mixedIndex('ambiguous', root => {
      fs.writeFileSync(path.join(root, SHOP, 'LegacyOrderResource.java'), [
        'package com.acme.shop;', '',
        'import java.util.List;',
        'import org.springframework.web.bind.annotation.GetMapping;',
        'import org.springframework.web.bind.annotation.RequestMapping;',
        'import org.springframework.web.bind.annotation.RestController;', '',
        '@RestController', '@RequestMapping("/api")',
        'public class LegacyOrderResource {',
        '    @GetMapping("/orders")',
        '    public List<Order> orders() {', '        return List.of();', '    }', '}', ''].join('\n'));
    }, { ...JAVA_METHODS, [`${SHOP}/LegacyOrderResource.java`]: ['com.acme.shop.LegacyOrderResource', ['orders']] });

    assert.equal(link(ambiguous), 'link: 2 linked, 1 ambiguous, 1 unresolved\n');
    const index = openIndex(ambiguous.db);
    try {
      assert.deepEqual(links(index), FIXTURE_LINKS.filter(([, endpoint]) => endpoint !== 'HTTP.GET /api/orders'));
      // the endpoint leads to both handlers, and nothing leads to it
      assert.deepEqual(index.edges.filter(e => e.source.name === 'HTTP.GET /api/orders').map(e => e.target.name).sort(),
        ['com.acme.shop.LegacyOrderResource.orders', `${RESOURCE}.list`]);
      assert.deepEqual(index.edges.filter(e => e.target.name === 'HTTP.GET /api/orders').map(e => e.source.name), ['HTTP']);
      assert.deepEqual(index.edges.filter(e => e.source.name === `${SERVICE}.list` && e.target.name.startsWith('HTTP')), []);
    } finally {
      index.close();
    }
    const log = fs.readFileSync(ambiguous.log, 'utf8');
    assert.match(log, /^ambiguous\tGET\t\/api\/orders\tsrc\/main\/webapp\/app\/orders\/order\.service\.ts:17\t2 handlers: com\.acme\.shop\.LegacyOrderResource\.orders, com\.acme\.shop\.OrderResource\.list$/m);
  });

  test('feignClientIsNoHandler', () => {
    // a frontend call to the path the Feign client posts to
    const feign = mixedIndex('feign', root => {
      const file = path.join(root, ORDER_SERVICE_TS);
      const text = fs.readFileSync(file, 'utf8');
      fs.writeFileSync(file, text.replace(/}\s*$/,
        "\n  authorize(order: Order): Observable<boolean> {\n    return this.http.post<boolean>('authorizations', order);\n  }\n}\n"));
    });
    assert.equal(link(feign), 'link: 3 linked, 0 ambiguous, 2 unresolved\n');
    assert.match(fs.readFileSync(feign.log, 'utf8'), /^unresolved\tPOST\t\/authorizations\t.*\tno handler$/m);
  });

  test('aJavaPackageNamedHttpIsNoEndpoint', () => {
    const db = new DatabaseSync(fixture.db);
    const id = db.prepare('SELECT max(id) AS m FROM element').get().m + 1;
    db.prepare('INSERT INTO element(id) VALUES (?), (?), (?)').run(id, id + 1, id + 2);
    const pkg = db.prepare('SELECT id FROM node WHERE serialized_name = ?').get(serializedName('HTTP'))?.id;
    db.prepare('INSERT INTO node VALUES (?, ?, ?)').run(id, NODE.CLASS, serializedName('HTTP.Client'));
    // a method whose name holds a space cannot be Java's; one in a class of the HTTP package is still not an endpoint
    db.prepare('INSERT INTO node VALUES (?, ?, ?)').run(id + 1, NODE.FUNCTION, serializedName('HTTP.Client.get'));
    db.prepare('INSERT INTO edge VALUES (?, ?, ?, ?)').run(id + 2, EDGE.MEMBER, pkg, id);
    db.close();
    link(fixture);
    const index = openIndex(fixture.db);
    try {
      assert.deepEqual(index.named('HTTP.Client').map(n => n.type), [NODE.CLASS]);
      assert.deepEqual(index.named('HTTP.Client.get').map(n => n.type), [NODE.FUNCTION]);
      assert.deepEqual(index.edgesBetween('HTTP', 'HTTP.Client').map(e => e.type), [EDGE.MEMBER]);
      assert.deepEqual(links(index), FIXTURE_LINKS);
    } finally {
      index.close();
    }
  });

  test('removeOnlyDropsTheLayer', () => {
    const r = spawnSync(process.execPath, [linkCli, '--db', fixture.db, '--remove'], { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr);
    assert.equal(r.stdout, 'link: endpoints removed, no TypeScript group\n');
    const index = openIndex(fixture.db);
    try {
      assert.deepEqual(index.nodes.filter(n => /^HTTP\.[A-Z]+ \//.test(n.name)), []);
      assert.equal(index.named('HTTP.Client').length, 1);
    } finally {
      index.close();
    }
    link(fixture);
  });

  test('withoutCallsTheEndpointsStayUnlinked', () => {
    const copy = { ...fixture, calls: path.join(fixture.dir, 'none.json') };
    assert.equal(link(copy), 'link: 0 linked, 0 ambiguous, 0 unresolved\n');
    const index = openIndex(fixture.db);
    try {
      assert.deepEqual(links(index), []);
      assert.deepEqual(index.edges.filter(e => e.type === EDGE.CALL && e.source.name.startsWith('HTTP.'))
        .map(e => `${e.source.name} -> ${e.target.name}`).sort(), [
        `HTTP.GET /api/orders -> ${RESOURCE}.list`,
        `HTTP.GET /api/orders/{} -> ${RESOURCE}.get`,
        `HTTP.POST /api/orders -> ${RESOURCE}.create`,
      ]);
    } finally {
      index.close();
    }
    link(fixture);
  });
});
