import { after, before, describe, test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { DatabaseSync } from 'node:sqlite';

import { EDGE, NODE, fixtureDir, openIndex, runIndexer, scratchDir } from './helpers.mjs';

const SERVICE = 'app.orders.order.service.OrderService';
const LIST_COMPONENT = 'app.orders.order-list.component.OrderListComponent';

describe('the fixture index', () => {
  let dir, run, index;

  before(() => {
    dir = scratchDir('fixture');
    run = runIndexer(['--project', fixtureDir, '--tsconfig', path.join(fixtureDir, 'tsconfig.json'),
      '--out', path.join(dir, 'ts.srctrldb')]);
    index = openIndex(path.join(dir, 'ts.srctrldb'));
  });
  after(() => {
    index?.close();
    fs.rmSync(dir, { recursive: true, force: true });
  });

  test('runsCleanly', () => {
    assert.equal(run.code, 0, run.stderr);
    assert.equal(run.stderr, '');
    const files = index.db.prepare('SELECT count(*) AS n FROM file').get().n;
    assert.equal(run.stdout, `ts-indexer: ${files} files, 0 errors\n`);
    assert.ok(files >= 6);
  });

  test('writesStorageVersion27', () => {
    assert.equal(index.db.prepare("SELECT value FROM meta WHERE key = 'storage_version'").get().value, '27');
    for (const f of index.db.prepare('SELECT language, indexed, complete FROM file').all()) {
      assert.deepEqual({ ...f }, { language: 'typescript', indexed: 1, complete: 1 });
    }
  });

  test('namesRootedAtWebappRoot', () => {
    assert.deepEqual(index.named(SERVICE).map(n => n.type), [NODE.CLASS]);
    const rooted = index.nodes.filter(n => n.type !== NODE.FILE && n.name.startsWith('src.main.webapp'));
    assert.deepEqual(rooted.map(n => n.name), []);
  });

  test('typeStubsBehaveLikeTheirNpmPackage', () => {
    // types/@angular/common/http/index.d.ts is reached through tsconfig `paths`: a library, not project code
    assert.equal(index.named('@angular/common.HttpClient.get').length, 1);
    const stubNames = n => /^types\.(@angular|rxjs)\b/.test(n.name);
    assert.deepEqual(index.nodes.filter(stubNames).map(n => n.name), []);
    const stubFiles = index.db.prepare("SELECT path FROM file WHERE path LIKE '%/types/@angular/%'").all();
    assert.deepEqual(stubFiles, []);
  });

  test('callVsUsageFromAst', () => {
    assert.deepEqual(index.edgesBetween(`${LIST_COMPONENT}.load`, `${SERVICE}.list`).map(e => e.type), [EDGE.CALL]);
    // `subscribe(this.show)` passes the method without invoking it
    assert.deepEqual(index.edgesBetween(`${LIST_COMPONENT}.load`, `${LIST_COMPONENT}.show`).map(e => e.type),
      [EDGE.USAGE]);
    // an optional call `this.load?.()` is still a call
    assert.deepEqual(index.edgesBetween(`${LIST_COMPONENT}.refresh`, `${LIST_COMPONENT}.load`).map(e => e.type),
      [EDGE.CALL]);
  });

  test('overloadsAreSeparateNodes', () => {
    const finds = index.named(`${SERVICE}.find`);
    assert.deepEqual(finds.map(n => n.type), [NODE.METHOD, NODE.METHOD]);
    const signatures = finds.map(n => n.serialized_name.slice(n.serialized_name.lastIndexOf('\tp') + 2)).sort();
    assert.deepEqual(signatures, ['(number)', '(string)']);
    // the implementation's body belongs to every overload
    for (const find of finds) {
      const calls = index.edges.filter(e => e.source === find && e.type === EDGE.CALL).map(e => e.target.name).sort();
      assert.deepEqual(calls, [`${SERVICE}.get`, `${SERVICE}.list`]);
    }
  });

  test('constructorParameterPropertiesAreFields', () => {
    assert.deepEqual(index.named(`${LIST_COMPONENT}.orderService`).map(n => n.type), [NODE.FIELD]);
    assert.equal(index.edgesBetween(LIST_COMPONENT, `${LIST_COMPONENT}.orderService`)[0]?.type, EDGE.MEMBER);
  });

  test('decoratorIsAnnotationUsage', () => {
    assert.deepEqual(index.edgesBetween(LIST_COMPONENT, '@angular/core.Component').map(e => e.type),
      [EDGE.ANNOTATION_USAGE]);
    // declared `export declare const Component: ComponentDecorator`, as in @angular/core
    assert.deepEqual(index.named('@angular/core.Component').map(n => n.type), [NODE.GLOBAL_VARIABLE]);
  });

  test('moduleCodeGoesToToplevel', () => {
    assert.deepEqual(index.edgesBetween('app.orders.order.service.spec.toplevel', `${SERVICE}.list`).map(e => e.type),
      [EDGE.CALL]);
    const fromModules = index.edges.filter(e => e.source.type === NODE.MODULE && e.type !== EDGE.MEMBER);
    assert.deepEqual(fromModules.map(e => `${e.source.name} -> ${e.target.name}`), []);
  });

  test('importsComeFromTheFile', () => {
    const imports = index.edges.filter(e => e.type === EDGE.IMPORT && e.target.name === SERVICE);
    assert.ok(imports.length >= 3, 'list, detail and spec import OrderService');
    assert.ok(imports.every(e => e.source.type === NODE.FILE));
  });

  test('implementsIsInheritanceAndOverride', () => {
    const detail = 'app.orders.order-detail.component.OrderDetailComponent';
    assert.deepEqual(index.edgesBetween(detail, '@angular/core.OnInit').map(e => e.type), [EDGE.INHERITANCE]);
    assert.deepEqual(index.edgesBetween(`${detail}.ngOnInit`, '@angular/core.OnInit.ngOnInit').map(e => e.type),
      [EDGE.OVERRIDE]);
  });

  test('definitionsHaveLocations', () => {
    const [list] = index.named(`${SERVICE}.list`);
    const locations = index.db.prepare(`SELECT l.start_line, l.start_column, l.end_line, l.end_column, l.type, f.path
      FROM occurrence o JOIN source_location l ON l.id = o.source_location_id JOIN file f ON f.id = l.file_node_id
      WHERE o.element_id = ? ORDER BY l.type`).all(list.id).map(r => ({ ...r }));
    const file = path.join(fixtureDir, 'src/main/webapp/app/orders/order.service.ts');
    assert.deepEqual(locations, [
      { start_line: 16, start_column: 3, end_line: 16, end_column: 6, type: 0, path: file },
      { start_line: 16, start_column: 3, end_line: 18, end_column: 3, type: 1, path: file },
    ]);
  });
});

describe('the command line', () => {
  test('multiAppLayoutIsReported', () => {
    const dir = scratchDir('multi');
    try {
      fs.writeFileSync(path.join(dir, 'tsconfig.json'), JSON.stringify({ compilerOptions: { strict: true } }));
      for (const app of ['shop', 'admin']) {
        fs.mkdirSync(path.join(dir, 'apps', app, 'src'), { recursive: true });
        fs.writeFileSync(path.join(dir, 'apps', app, 'tsconfig.app.json'),
          JSON.stringify({ extends: '../../tsconfig.json', include: ['src/**/*.ts'] }));
        fs.writeFileSync(path.join(dir, 'apps', app, 'src', 'main.ts'), 'export const app = 1;\n');
      }
      fs.mkdirSync(path.join(dir, 'node_modules'));
      const r = runIndexer(['--project', dir, '--tsconfig', path.join(dir, 'tsconfig.json'),
        '--out', path.join(dir, 'ts.srctrldb')]);
      assert.equal(r.code, 4);
      assert.equal(r.stderr, `ts-indexer: several TypeScript apps in ${dir}; not supported yet\n`);
      assert.equal(fs.existsSync(path.join(dir, 'ts.srctrldb')), false);
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });

  test('missingNodeModulesExits3', () => {
    const dir = scratchDir('nomodules');
    try {
      fs.cpSync(path.join(fixtureDir, 'src/main/webapp/app'), path.join(dir, 'src/main/webapp/app'), { recursive: true });
      const tsconfig = JSON.parse(fs.readFileSync(path.join(fixtureDir, 'tsconfig.json'), 'utf8'));
      delete tsconfig.compilerOptions.paths;
      fs.writeFileSync(path.join(dir, 'tsconfig.json'), JSON.stringify(tsconfig));
      const r = runIndexer(['--project', dir, '--tsconfig', path.join(dir, 'tsconfig.json'),
        '--out', path.join(dir, 'ts.srctrldb')]);
      assert.equal(r.code, 3);
      assert.equal(r.stderr, `ts-indexer: node_modules missing in ${dir}; run npm install\n`);
      assert.equal(r.stdout, '');
      assert.equal(fs.existsSync(path.join(dir, 'ts.srctrldb')), false);
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });

  test('appendKeepsExistingRows', () => {
    const dir = scratchDir('append');
    try {
      const out = path.join(dir, 'all.srctrldb');
      assert.equal(runIndexer(['--project', fixtureDir, '--tsconfig', path.join(fixtureDir, 'tsconfig.json'),
        '--out', out]).code, 0);
      const db = new DatabaseSync(out);
      db.exec('DELETE FROM node; DELETE FROM edge; DELETE FROM file; DELETE FROM element; DELETE FROM source_location;'
        + 'DELETE FROM occurrence; DELETE FROM symbol; DELETE FROM filecontent; DELETE FROM local_symbol;');
      db.exec("INSERT INTO element(id) VALUES (1), (2), (3);"
        + "INSERT INTO node(id, type, serialized_name) VALUES (1, 32, '.\tmcom\ts\tp'), (2, 32, '.\tmapp\ts\tp'),"
        + " (3, 128, '.\tmcom\ts\tp\tnacme\ts\tp');");
      db.close();

      const r = runIndexer(['--project', fixtureDir, '--tsconfig', path.join(fixtureDir, 'tsconfig.json'),
        '--out', out, '--append']);
      assert.equal(r.code, 0, r.stderr);
      const index = openIndex(out);
      try {
        assert.equal(index.named('com.acme').length, 1, 'the existing row is kept');
        assert.deepEqual(index.named('app').map(n => n.id), [2], 'an existing package node is reused');
        assert.equal(index.named(SERVICE).length, 1);
        assert.ok(index.named(SERVICE)[0].id > 3);
      } finally {
        index.close();
      }
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });

  test('usageErrorsExit2', () => {
    const r = runIndexer(['--project', fixtureDir]);
    assert.equal(r.code, 2);
    assert.match(r.stderr, /^ts-indexer: usage: /);
  });
});
