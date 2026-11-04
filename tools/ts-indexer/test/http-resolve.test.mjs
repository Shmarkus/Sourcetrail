// URL resolution on small snippets: what may be resolved, and what must stay unresolved rather than be guessed.
import { describe, test } from 'node:test';
import assert from 'node:assert/strict';
import ts from 'typescript';

import { httpCallCandidates, resolveUrl } from '../http.mjs';

const urlsOf = code => {
  const sourceFile = ts.createSourceFile('snippet.ts', code, ts.ScriptTarget.Latest, true);
  return httpCallCandidates(ts, sourceFile).map(call => resolveUrl(ts, sourceFile, call.url));
};
const resolved = url => ({ url, resolved: true });
const unresolved = url => ({ url, resolved: false });

describe('URL resolution', () => {
  test('constInScopeResolves', () => {
    assert.deepEqual(urlsOf(`
      const base = 'api/orders';
      class S { http: any; list() { return this.http.get(\`\${base}/items\`); } }`), [resolved('api/orders/items')]);
  });

  test('getEndpointForWithOneArgumentResolves', () => {
    // with a second argument (a microservice) the helper returns services/<ms>/<api>, so it is not resolved
    assert.deepEqual(urlsOf(`
      class S {
        http: any;
        config: any;
        local() { return this.http.get(this.config.getEndpointFor('api/orders')); }
        remote() { return this.http.get(this.config.getEndpointFor('api/orders', 'billing')); }
      }`), [resolved('api/orders'), unresolved("this.config.getEndpointFor('api/orders', 'billing')")]);
  });

  test('parameterShadowsConst', () => {
    assert.deepEqual(urlsOf(`
      const base = 'api/orders';
      class S { http: any; list(base: string) { return this.http.get(\`\${base}/items\`); } }`),
    [unresolved('`${base}/items`')]);
  });

  test('localShadowsConst', () => {
    assert.deepEqual(urlsOf(`
      const base = 'api/orders';
      class S {
        http: any;
        list(other: string) {
          let base = other;
          return this.http.get(base + '/items');
        }
        each(bases: string[]) { return bases.map(base => this.http.get(base)); }
      }`), [unresolved("base + '/items'"), unresolved('base')]);
  });

  test('fieldWrittenOutsideTheConstructorIsUnresolved', () => {
    assert.deepEqual(urlsOf(`
      class S {
        http: any;
        resourceUrl = 'api/orders';
        use(url: string) { this.resourceUrl = url; }
        list() { return this.http.get(this.resourceUrl); }
      }`), [unresolved('this.resourceUrl')]);
  });

  test('fieldWithInitialiserAndConstructorAssignmentIsUnresolved', () => {
    assert.deepEqual(urlsOf(`
      class S {
        http: any;
        resourceUrl = 'api/orders';
        constructor() { this.resourceUrl = 'api/other'; }
        list() { return this.http.get(this.resourceUrl); }
      }`), [unresolved('this.resourceUrl')]);
  });

  test('readonlyFieldChangedInTheConstructorIsUnresolved', () => {
    assert.deepEqual(urlsOf(`
      class S {
        http: any;
        private readonly resourceUrl: string;
        constructor(v2: boolean) {
          this.resourceUrl = 'api/orders';
          if (v2) this.resourceUrl += '/v2';
        }
        list() { return this.http.get(this.resourceUrl); }
      }`), [unresolved('this.resourceUrl')]);
  });

  test('writesToOtherObjectsDoNotUnsettleAReadonlyField', () => {
    assert.deepEqual(urlsOf(`
      class S {
        http: any;
        private readonly resourceUrl = 'api/orders';
        list() { return this.http.get(this.resourceUrl); }
      }
      function reset(o: { resourceUrl: string }) { o.resourceUrl = ''; }`), [resolved('api/orders')]);
  });

  test('fieldSetOnceResolves', () => {
    assert.deepEqual(urlsOf(`
      class S {
        http: any;
        resourceUrl = 'api/orders';
        private other: string;
        constructor() { this.other = 'api/other'; }
        list() { return this.http.get(this.resourceUrl); }
        more() { return this.http.get(this.other); }
      }`), [resolved('api/orders'), resolved('api/other')]);
  });

  test('unresolvedMiddleSegmentIsUnresolved', () => {
    assert.deepEqual(urlsOf(`
      import { API_BASE } from './config';
      class S {
        http: any;
        prefix: string;
        list() { return this.http.get(\`api/\${this.prefix}/orders\`); }
        all() { return this.http.get(\`api/\${API_BASE}/orders\`); }
      }`), [unresolved('`api/${this.prefix}/orders`'), unresolved('`api/${API_BASE}/orders`')]);
  });

  test('pathVariablesComeFromLocalData', () => {
    // a parameter or local is request data, so a whole segment made of it is a path variable
    assert.deepEqual(urlsOf(`
      class S {
        http: any;
        items(id: number) { return this.http.get(\`api/orders/\${id}/items\`); }
        notes(customer: { id: number }) { return this.http.post(\`api/customers/\${customer.id}/notes\`, {}); }
        page(page: number) { return this.http.get(\`api/orders?page=\${page}\`); }
        partial(id: number) { return this.http.get(\`api/orders-\${id}\`); }
      }`), [resolved('api/orders/{}/items'), resolved('api/customers/{}/notes'), resolved('api/orders?page={}'),
      unresolved('`api/orders-${id}`')]);
  });
});
