// Pyright's parser, taken from the bundle scip-python ships: a syntax-only parse of one Python file, for the facts
// SCIP does not carry (calls, decorators, imports, base class lists) and for syntax errors.
//
// The bundle is a webpack build: its chunks export module tables, the main file keeps the Node built-ins. A minimal
// webpack runtime below loads the parser module from them; the scip-python version is pinned, and the tests run this.
import fs from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';

import { at } from './convert.mjs';

const CHUNKS = ['pyright-internal.js', 'vendor.js'];

function webpackRequire(dist) {
  const require = createRequire(path.join(dist, 'scip-python.js'));
  const modules = {};
  for (const chunk of CHUNKS) Object.assign(modules, require(path.join(dist, chunk)).modules);
  const main = fs.readFileSync(path.join(dist, 'scip-python.js'), 'utf8');
  for (const [, id, builtin] of main.matchAll(/(\d+):e=>\{e\.exports=require\("([^"]+)"\)\}/g)) {
    modules[id] ??= module => { module.exports = require(builtin); };
  }
  const cache = {};
  const load = id => {
    if (cache[id]) return cache[id].exports;
    if (!modules[id]) throw new Error(`scip-python bundle: module ${id} missing`);
    const module = { exports: {} };
    cache[id] = module;
    modules[id].call(module.exports, module, module.exports, load);
    return module.exports;
  };
  const own = Object.prototype.hasOwnProperty;
  load.o = (object, key) => own.call(object, key);
  load.d = (exports, getters) => {
    for (const key in getters) {
      if (!own.call(exports, key)) Object.defineProperty(exports, key, { enumerable: true, get: getters[key] });
    }
  };
  load.r = exports => {
    Object.defineProperty(exports, Symbol.toStringTag, { value: 'Module' });
    Object.defineProperty(exports, '__esModule', { value: true });
  };
  load.n = module => {
    const getter = module?.__esModule ? () => module.default : () => module;
    load.d(getter, { a: getter });
    return getter;
  };
  const find = (...markers) => {
    const id = Object.keys(modules).find(k => markers.every(m => modules[k].toString().includes(m)));
    if (id === undefined) throw new Error(`scip-python bundle: no module with ${markers.join(', ')}`);
    return load(id);
  };
  return { find };
}

// Loads the parser from scip-python's `dist` folder. Returns {parse(text)}.
export function loadPythonParser(dist) {
  const { find } = webpackRequire(dist);
  const { Parser, ParseOptions } = find('t.ParseOptions=', 't.Parser=');
  const { printParseNodeType } = find('t.printParseNodeType=');
  const T = {}; // node type name -> number
  for (let n = 0; ; n++) {
    let name;
    try {
      name = printParseNodeType(n);
    } catch {
      break; // past the last node type
    }
    T[name] = n;
  }
  for (const name of ['Call', 'Class', 'Decorator', 'Function', 'Import', 'ImportFrom', 'MemberAccess', 'Name']) {
    if (T[name] === undefined) throw new Error(`scip-python bundle: parse node type ${name} unknown`);
  }
  return { parse: text => parse(Parser, ParseOptions, T, text) };
}

function parse(Parser, ParseOptions, T, text) {
  const errors = [];
  const sink = {
    addError: (message, range) => errors.push({ message, range }),
    addWarning() {}, addInformation() {}, addUnusedCode() {}, addUnreachableCode() {}, addDeprecated() {},
  };
  const result = new Parser().parseSourceFile(text, new ParseOptions(), sink);

  // Pyright's offsets count UTF-16 code units, like SCIP's columns here
  const lineStarts = [0];
  for (const m of text.matchAll(/\r\n|\r|\n/g)) lineStarts.push(m.index + m[0].length);
  const position = offset => {
    let lo = 0, hi = lineStarts.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (lineStarts[mid] <= offset) lo = mid;
      else hi = mid - 1;
    }
    return [lo, offset - lineStarts[lo]];
  };
  const key = node => at(...position(node.start));
  const span = node => [...position(node.start), ...position(node.start + node.length)];

  const facts = {
    callees: new Set(), // positions of called names
    decorators: new Map(), // position of a decorator's name -> name position of the decorated declaration
    // per `import` module and per `from ... import` statement: {span, module, names}; module and each imported name
    // {key, position, name}, the module's name with its leading dots
    imports: [],
    heritage: new Set(), // positions of the names in a class's base list
    diagnostics: errors.map(e => ({ line: e.range.start.line, column: e.range.start.character, message: e.message })),
  };
  // `..pkg.mod` -> {key, position, name: '..pkg.mod'}
  const moduleName = m => ({ key: key(m), position: position(m.start),
    name: '.'.repeat(m.leadingDots ?? 0) + m.nameParts.map(p => p.value).join('.') });
  // the name a call or decorator expression ends in: f, a.b.f
  const calleeName = e => {
    if (e?.nodeType === T.Name) return e;
    if (e?.nodeType === T.MemberAccess) return e.memberName;
    return null;
  };
  const visit = node => {
    switch (node.nodeType) {
      case T.Call: {
        const name = calleeName(node.leftExpression);
        if (name) facts.callees.add(key(name));
        break;
      }
      case T.Decorator: {
        const e = node.expression?.nodeType === T.Call ? node.expression.leftExpression : node.expression;
        const name = calleeName(e);
        if (name && node.parent?.name) facts.decorators.set(key(name), key(node.parent.name));
        break;
      }
      case T.Class:
        for (const argument of node.arguments ?? []) {
          if (argument.name) continue; // metaclass=..., keywords
          const name = calleeName(argument.valueExpression);
          if (name) facts.heritage.add(key(name));
        }
        break;
      case T.Import:
        for (const entry of node.list ?? []) {
          facts.imports.push({ span: span(entry), module: moduleName(entry.module), names: [] });
        }
        break;
      case T.ImportFrom:
        facts.imports.push({ span: span(node), module: moduleName(node.module),
          names: (node.imports ?? []).map(n => ({ key: key(n.name), position: position(n.name.start),
            name: n.name.value })) });
        break;
    }
    for (const k of Object.keys(node)) {
      if (k === 'parent') continue;
      const v = node[k];
      if (Array.isArray(v)) {
        for (const child of v) if (child && typeof child === 'object' && 'nodeType' in child) visit(child);
      } else if (v && typeof v === 'object' && 'nodeType' in v) visit(v);
    }
  };
  visit(result.parseTree);
  return facts;
}
