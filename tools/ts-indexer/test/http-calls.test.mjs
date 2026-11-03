import { after, before, describe, test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { displayName, fixtureDir, openIndex, runIndexer, scratchDir } from './helpers.mjs';

const SERVICE = 'app.orders.order.service.OrderService';
const SERVICE_FILE = 'src/main/webapp/app/orders/order.service.ts';

describe('the HTTP call sidecar', () => {
  let dir, calls, index;

  before(() => {
    dir = scratchDir('http');
    const r = runIndexer(['--project', fixtureDir, '--tsconfig', path.join(fixtureDir, 'tsconfig.json'),
      '--out', path.join(dir, 'ts.srctrldb')]);
    assert.equal(r.code, 0, r.stderr);
    calls = JSON.parse(fs.readFileSync(path.join(dir, 'ts-http-calls.json'), 'utf8'));
    index = openIndex(path.join(dir, 'ts.srctrldb'));
  });
  after(() => {
    index?.close();
    fs.rmSync(dir, { recursive: true, force: true });
  });

  const callFrom = method => calls.filter(c => displayName(c.source_name) === `${SERVICE}.${method}`);

  test('getToALiteral', () => {
    assert.deepEqual(callFrom('list'), [{ source_name: callFrom('list')[0]?.source_name, verb: 'GET',
      url: 'api/orders', url_resolved: true, file: SERVICE_FILE, line: 17, col: 22 }]);
  });

  test('postThroughATemplateString', () => {
    assert.deepEqual(callFrom('create').map(({ verb, url, url_resolved, line }) => ({ verb, url, url_resolved, line })),
      [{ verb: 'POST', url: 'api/orders', url_resolved: true, line: 21 }]);
  });

  test('getThroughAResourceUrlField', () => {
    // resourceUrl = this.applicationConfigService.getEndpointFor('api/orders'); the id is a path variable
    assert.deepEqual(callFrom('get').map(({ verb, url, url_resolved }) => ({ verb, url, url_resolved })),
      [{ verb: 'GET', url: 'api/orders/{}', url_resolved: true }]);
  });

  test('unresolvableUrlKeepsTheExpression', () => {
    assert.deepEqual(callFrom('download').map(({ verb, url, url_resolved }) => ({ verb, url, url_resolved })),
      [{ verb: 'GET', url: 'location', url_resolved: false }]);
  });

  test('sourceNamesAreNodes', () => {
    const names = new Set(index.nodes.map(n => n.serialized_name));
    assert.equal(calls.length, 4);
    for (const call of calls) assert.ok(names.has(call.source_name), call.source_name);
  });

  test('httpOutOption', () => {
    const elsewhere = path.join(dir, 'calls', 'http.json');
    fs.mkdirSync(path.dirname(elsewhere));
    const r = runIndexer(['--project', fixtureDir, '--tsconfig', path.join(fixtureDir, 'tsconfig.json'),
      '--out', path.join(dir, 'second.srctrldb'), '--http-out', elsewhere]);
    assert.equal(r.code, 0, r.stderr);
    assert.equal(JSON.parse(fs.readFileSync(elsewhere, 'utf8')).length, 4);
  });
});
