import { describe, test } from 'node:test';
import assert from 'node:assert/strict';

import { ANY, matchingHandlers, normalizePath, springHandlers } from '../endpoints.mjs';

const controller = (classAnnotations, body) =>
  `package x;\n\n${classAnnotations}\npublic class C {\n${body}\n}\n`;
const routes = source => springHandlers(source).handlers.map(h => `${h.verb} ${h.path}`);

describe('Spring mappings', () => {
  test('normalizesBothSides', () => {
    assert.equal(normalizePath('api/orders'), '/api/orders');
    assert.equal(normalizePath('/api//orders/'), '/api/orders');
    assert.equal(normalizePath('/api/orders/{id}'), '/api/orders/{}');
    assert.equal(normalizePath('/api/orders/{id:\\d+}/items?page=1'), '/api/orders/{}/items');
    assert.equal(normalizePath(''), '/');
  });

  test('classPrefixAndMethodPaths', () => {
    const source = controller('@RestController\n@RequestMapping("/api/orders")', [
      '  @GetMapping\n  public List<Order> list() { return null; }',
      '  @PostMapping("")\n  public Order create(Order o) { return o; }',
      '  @GetMapping("/{id}")\n  public Order get(Long id) { return null; }',
      '  @DeleteMapping(value = "/{id}", produces = "application/json")\n  public void delete(Long id) {}',
    ].join('\n'));
    assert.deepEqual(routes(source),
      ['GET /api/orders', 'POST /api/orders', 'GET /api/orders/{}', 'DELETE /api/orders/{}']);
  });

  test('requestMappingVerbsAndPathLists', () => {
    const source = controller('@Controller', [
      '  @RequestMapping(path = {"/a", "/b"}, method = {RequestMethod.GET, RequestMethod.PUT})\n  public void ab() {}',
      '  @RequestMapping(value = "/c", method = POST)\n  public void c() {}',
      '  @RequestMapping("/any")\n  public void any() {}',
      '  @org.springframework.web.bind.annotation.PatchMapping(path = "/p" + "/q")\n  public void pq() {}',
    ].join('\n'));
    assert.deepEqual(routes(source), ['GET /a', 'PUT /a', 'GET /b', 'PUT /b', 'POST /c', `${ANY} /any`, 'PATCH /p/q']);
  });

  test('annotationPositions', () => {
    const source = controller('@RestController', '  @GetMapping(\n      "/x")\n  public void x() {}');
    const [h] = springHandlers(source).handlers;
    assert.deepEqual([h.start, h.end], [{ line: 5, column: 3 }, { line: 6, column: 11 }]);
  });

  test('nonLiteralPathsAreReportedNotGuessed', () => {
    const source = controller('@RestController\n@RequestMapping("/api")',
      '  @GetMapping(Paths.ORDERS)\n  public void list() {}\n  @GetMapping("/ok")\n  public void ok() {}');
    const { handlers, problems } = springHandlers(source);
    assert.deepEqual(handlers.map(h => h.path), ['/api/ok']);
    assert.equal(problems.length, 1);
    assert.match(problems[0].message, /Paths\.ORDERS/);
  });

  test('stringConstantsOfTheFile', () => {
    const source = controller('@RestController\n@RequestMapping(Paths.BASE)', '');
    assert.match(springHandlers(source).problems[0].message, /Paths\.BASE/);
    const constants = controller('@RestController\n@RequestMapping(BASE)', [
      '  public static final String BASE = "/api";',
      '  private static final String DONE = "/done" + "/page";',
      '  static String MUTABLE = "/mutable";',
      '  @GetMapping(DONE)\n  public void done() {}',
      '  @GetMapping({BASE_TOO, "/x"})\n  public void x() {}',
      '  @GetMapping("/item/" + "{id}")\n  public void item() {}',
      '  @GetMapping(MUTABLE)\n  public void mutable() {}',
    ].join('\n'));
    const { handlers, problems } = springHandlers(constants);
    assert.deepEqual(handlers.map(h => `${h.verb} ${h.path}`), ['GET /api/done/page', 'GET /api/item/{}']);
    assert.deepEqual(problems.map(p => p.message), ['path is not a string literal: {BASE_TOO, "/x"}',
      'path is not a string literal: MUTABLE']);
  });

  test('commentsStringsAndNonControllers', () => {
    assert.deepEqual(routes(controller('@RestController',
      '  // @GetMapping("/old")\n  /* @PostMapping("/older") */\n  @GetMapping("/new")\n  public String n() { return "http://x/*y"; }')),
    ['GET /new']);
    assert.deepEqual(routes(controller('@Service', '  @GetMapping("/x")\n  public void x() {}')), []);
    assert.deepEqual(routes(controller('@Schema(description = "a class of things")\n@RestController',
      '  @GetMapping("/x")\n  public void x() {}')), ['GET /x']);
    const feign = 'package x;\n\n@FeignClient(name = "payments")\npublic interface P {\n'
      + '  @PostMapping("/authorizations")\n  boolean authorize(Order o);\n}\n';
    assert.deepEqual(routes(feign), []);
  });

  test('nestedTypesAreReportedNotParsed', () => {
    const source = controller('@RestController\n@RequestMapping("/a")', [
      '  @GetMapping("/x")\n  public void x() {}',
      '  @RequestMapping("/inner")\n  static class B {\n    @GetMapping("/y")\n    public void y() {}\n  }',
      '  @PostMapping({"/after"})\n  public void after() {}',
      '  @PutMapping("/record")\n  public void record(String raw) {}',
    ].join('\n'));
    const { handlers, problems } = springHandlers(source);
    assert.deepEqual(handlers.map(h => `${h.verb} ${h.path}`), ['GET /a/x', 'POST /a/after', 'PUT /a/record']);
    assert.deepEqual(problems.map(p => p.line), [8, 10]);
    for (const p of problems) assert.match(p.message, /nested type/);
  });

  test('otherTopLevelTypesAreReportedNotParsed', () => {
    const source = `${controller('@RestController', '  @GetMapping("/x")\n  public void x() {}')}\n`
      + 'class Helper {\n  @GetMapping("/z")\n  public void z() {}\n}\n';
    const { handlers, problems } = springHandlers(source);
    assert.deepEqual(handlers.map(h => `${h.verb} ${h.path}`), ['GET /x']);
    assert.equal(problems.length, 1);
    assert.match(problems[0].message, /outside the first type/);
  });

  test('classVerbsApplyToItsMethods', () => {
    const source = controller('@RestController\n@RequestMapping(value = "/a", method = RequestMethod.POST)', [
      '  @RequestMapping("/x")\n  public void x() {}',
      '  @RequestMapping(value = "/p", method = {RequestMethod.POST, RequestMethod.PUT})\n  public void p() {}',
      '  @PostMapping("/q")\n  public void q() {}',
      '  @GetMapping("/g")\n  public void g() {}',
    ].join('\n'));
    const { handlers, problems } = springHandlers(source);
    assert.deepEqual(handlers.map(h => `${h.verb} ${h.path}`), ['POST /a/x', 'POST /a/p', 'POST /a/q']);
    assert.equal(problems.length, 1);
    assert.match(problems[0].message, /GET.*POST/);
  });

  test('charLiteralsAndRegexPathVariables', () => {
    const source = controller('@RestController', [
      "  private static final char QUOTE = '\"';",
      '  @GetMapping("/items/{id:\\d{3}}/parts")\n  public void item() {}',
    ].join('\n'));
    assert.deepEqual(routes(source), ['GET /items/{}/parts']);
    assert.equal(normalizePath('/a/{id:[0-9]{2,3}}/b/{x}'), '/a/{}/b/{}');
  });

  test('aMappingNamingTheVerbComesFirst', () => {
    const handlers = [{ verb: ANY, path: '/a', id: 1 }, { verb: 'GET', path: '/a', id: 2 }, { verb: ANY, path: '/b', id: 3 }];
    assert.deepEqual(matchingHandlers(handlers, 'GET', '/a').map(h => h.id), [2]);
    assert.deepEqual(matchingHandlers(handlers, 'POST', '/a').map(h => h.id), [1]);
    assert.deepEqual(matchingHandlers(handlers, 'GET', '/b').map(h => h.id), [3]);
    assert.deepEqual(matchingHandlers(handlers, 'GET', '/c'), []);
  });
});
