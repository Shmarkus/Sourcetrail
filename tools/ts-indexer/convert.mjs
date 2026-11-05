// Converts a SCIP index written by scip-typescript into Sourcetrail nodes, edges and locations.
//
// Names use `.` as delimiter: directories below the webapp root are packages, each file is a module named after its
// stem, an npm package is one package node. Which declaration a reference belongs to comes from SCIP's enclosing
// ranges plus declaration extents from a syntax-only TypeScript parse; call versus usage, decorators and imports
// come from that parse too.
import fs from 'node:fs';
import path from 'node:path';

import { httpCallCandidates, resolveUrl } from './http.mjs';
import { DEFINITION, EDGE, LOCATION, NODE } from './sqlite.mjs';

const DELIMITER = '.';
const CALLABLE = new Set([NODE.METHOD, NODE.FUNCTION]);
const TYPES = new Set([NODE.CLASS, NODE.INTERFACE, NODE.TYPEDEF, NODE.ENUM, NODE.TYPE_PARAMETER]);
const HTTP_CLIENT_METHOD = /HttpClient#(get|post|put|patch|delete)\(\)\.$/;
const DEFINITION_ROLE = 1;

// ---- SCIP symbols ----
// "<scheme> <manager> <package> <version> <descriptors>"; descriptor names may be `backticked`.
export function parseSymbol(symbol) {
  if (symbol.startsWith('local ')) return null;
  let i = 0, spaces = 0;
  while (spaces < 4 && i < symbol.length) {
    if (symbol[i] === ' ' && symbol[i + 1] !== ' ') spaces++;
    i++;
  }
  const head = symbol.slice(0, i);
  const pkg = head.trim().split(' ')[2];
  const s = symbol.slice(i);
  const parts = [];
  let p = 0;
  const readName = () => {
    if (s[p] === '`') {
      const e = s.indexOf('`', p + 1);
      const name = s.slice(p + 1, e);
      p = e + 1;
      return name;
    }
    const from = p;
    while (p < s.length && !'/#.:([`!'.includes(s[p])) p++;
    return s.slice(from, p);
  };
  while (p < s.length) {
    if (s[p] === '[' || s[p] === '(') {
      const e = s.indexOf(s[p] === '[' ? ']' : ')', p);
      parts.push({ name: s.slice(p + 1, e), d: s[p] === '[' ? 'typeparam' : 'param', end: e + 1 });
      p = e + 1;
      continue;
    }
    const name = readName();
    const c = s[p];
    let d;
    if (c === '/') d = 'ns';
    else if (c === '#') d = 'type';
    else if (c === '.') d = 'term';
    else if (c === ':') d = 'meta';
    else if (c === '!') d = 'macro';
    else if (c === '(') {
      p = s.indexOf(')', p);
      if (s[p + 1] === '.') p++;
      d = 'method';
    } else break;
    p++;
    parts.push({ name, d, end: p });
  }
  return { pkg, head, parts, prefix: k => head + s.slice(0, parts[k].end) };
}

// "a: X, b: Y<Z, W>" -> ["X", "Y<Z, W>"]
function parameterTypes(text) {
  const params = [];
  let depth = 0, current = '';
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if ('<([{'.includes(c)) depth++;
    else if ('>)]}'.includes(c) && text[i - 1] !== '=') depth--;
    if (c === ',' && depth === 0) {
      params.push(current);
      current = '';
    } else current += c;
  }
  if (current.trim()) params.push(current);
  return params.map(param => {
    let d = 0;
    for (let i = 0; i < param.length; i++) {
      const c = param[i];
      if ('<([{'.includes(c)) d++;
      else if ('>)]}'.includes(c) && param[i - 1] !== '=') d--;
      else if (c === ':' && d === 0) return param.slice(i + 1).trim();
    }
    return param.trim();
  });
}

// SCIP hover "(method) save(account: Omit<Account, "imageUrl">): Observable<{}>"
//   -> {prefix: 'Observable<{}>', postfix: '(Omit<Account, "imageUrl">)'}
function signatureFromHover(hover) {
  const h = hover.replace(/\s+/g, ' ');
  const open = h.indexOf('(', h.startsWith('(') ? h.indexOf(')') + 1 : 0);
  if (open < 0) return { prefix: '', postfix: '()' };
  let depth = 0, close = -1;
  for (let i = open; i < h.length; i++) {
    if (h[i] === '(') depth++;
    else if (h[i] === ')' && --depth === 0) {
      close = i;
      break;
    }
  }
  if (close < 0) return { prefix: '', postfix: '()' };
  return { prefix: h.slice(close + 1).replace(/^\s*:\s*/, '').trim(),
    postfix: `(${parameterTypes(h.slice(open + 1, close)).join(', ')})` };
}

export const serialize = elements =>
  DELIMITER + '\tm' + elements.map(e => `${e.name}\ts${e.prefix || ''}\tp${e.postfix || ''}`).join('\tn');

export const cmp = (a, b) => a[0] - b[0] || a[1] - b[1];
export const start = r => [r[0], r[1]];
const end = r => (r.length === 3 ? [r[0], r[2]] : [r[2], r[3]]);
export const within = (pos, r) => cmp(pos, start(r)) >= 0 && cmp(pos, end(r)) <= 0;
export const at = (line, column) => `${line}|${column}`;

// ---- syntax-only facts about one file ----
function parseFile(ts, relativePath, text) {
  const sf = ts.createSourceFile(relativePath, text, ts.ScriptTarget.Latest, true);
  const lc = node => {
    const p = sf.getLineAndCharacterOfPosition(node.getStart(sf));
    return [p.line, p.character];
  };
  const span = node => {
    const a = sf.getLineAndCharacterOfPosition(node.getStart(sf)), b = sf.getLineAndCharacterOfPosition(node.getEnd());
    return [a.line, a.character, b.line, b.character];
  };
  const facts = {
    sourceFile: sf,
    extents: new Map(), // name position -> extent of a variable/property/enum member declaration
    functionValued: new Set(), // name positions of declarations initialised with a function
    parameterProperties: new Set(), // name positions of constructor parameter properties
    functionLike: new Map(), // name position -> {hasBody, params, returns, minArgs, maxArgs, extent}
    callees: new Map(), // position of the called name -> argument count
    decorators: new Map(), // position of a decorator's name -> name position of the decorated declaration, or null
    imports: [], // spans of import and `export ... from` statements
    heritage: new Set(), // positions of the type names in `extends`/`implements` clauses
    diagnostics: (sf.parseDiagnostics ?? []).map(d => {
      const p = sf.getLineAndCharacterOfPosition(d.start ?? 0);
      return { line: p.line, column: p.character, message: ts.flattenDiagnosticMessageText(d.messageText, '\n') };
    }),
  };
  const calleeName = expression => {
    let e = expression;
    while (ts.isParenthesizedExpression(e) || ts.isNonNullExpression(e)) e = e.expression;
    if (ts.isIdentifier(e) || e.kind === ts.SyntaxKind.SuperKeyword) return e;
    if (ts.isPropertyAccessExpression(e)) return e.name;
    if (ts.isElementAccessExpression(e) && ts.isStringLiteralLike(e.argumentExpression)) return e.argumentExpression;
    return null;
  };
  const markCallee = (expression, argumentCount) => {
    const name = calleeName(expression);
    if (!name) return;
    const [l, c] = lc(name);
    // a string-literal element access `x['name']()`: SCIP's range starts inside the quotes
    facts.callees.set(at(l, ts.isStringLiteralLike(name) ? c + 1 : c), argumentCount);
  };
  const visit = node => {
    if ((ts.isVariableDeclaration(node) || ts.isPropertyDeclaration(node) || ts.isPropertyAssignment(node)
      || ts.isEnumMember(node)) && node.name) {
      let extentNode = node;
      if (ts.isVariableDeclaration(node) && node.parent?.declarations?.length === 1
        && ts.isVariableStatement(node.parent.parent)) extentNode = node.parent.parent;
      const key = at(...lc(node.name));
      facts.extents.set(key, span(extentNode));
      const init = node.initializer;
      if (init && (ts.isArrowFunction(init) || ts.isFunctionExpression(init))) facts.functionValued.add(key);
    }
    if (ts.isParameter(node) && ts.isParameterPropertyDeclaration(node, node.parent)) {
      facts.parameterProperties.add(at(...lc(node.name)));
    }
    if ((ts.isMethodDeclaration(node) || ts.isMethodSignature(node) || ts.isFunctionDeclaration(node)
      || ts.isConstructorDeclaration(node)) && (node.name || ts.isConstructorDeclaration(node))) {
      const nameNode = node.name ?? node.getChildren(sf).find(c => c.kind === ts.SyntaxKind.ConstructorKeyword);
      const params = node.parameters.filter(p => !(ts.isIdentifier(p.name) && p.name.text === 'this'));
      const rest = params.some(p => p.dotDotDotToken);
      facts.functionLike.set(at(...lc(nameNode)), {
        hasBody: !!node.body,
        params: params.map(p => (p.dotDotDotToken ? '...' : '')
          + (p.type ? p.type.getText(sf).replace(/\s+/g, ' ') : 'any')),
        returns: node.type ? node.type.getText(sf).replace(/\s+/g, ' ') : '',
        minArgs: params.filter(p => !p.questionToken && !p.initializer && !p.dotDotDotToken).length,
        maxArgs: rest ? Infinity : params.length,
        extent: span(node),
      });
    }
    if (ts.isCallExpression(node) || ts.isNewExpression(node)) markCallee(node.expression, node.arguments?.length ?? 0);
    if (ts.isTaggedTemplateExpression(node)) markCallee(node.tag, 1);
    if (ts.isDecorator(node)) {
      const e = ts.isCallExpression(node.expression) ? node.expression.expression : node.expression;
      const name = calleeName(e);
      const decorated = node.parent?.name;
      if (name) facts.decorators.set(at(...lc(name)), decorated ? at(...lc(decorated)) : null);
    }
    if (ts.isHeritageClause(node)) {
      for (const t of node.types) {
        const name = calleeName(t.expression);
        if (name) facts.heritage.add(at(...lc(name)));
      }
    }
    if ((ts.isImportDeclaration(node) || (ts.isExportDeclaration(node) && node.moduleSpecifier)
      || ts.isImportEqualsDeclaration(node))) facts.imports.push(span(node));
    ts.forEachChild(node, visit);
  };
  visit(sf);
  return facts;
}

// Converts `index` (a scip.Index) into `writer` (an IndexWriter).
//   projectRoot: absolute folder the SCIP paths are relative to
//   webappRoot: project-relative folder names are rooted at ('' for the project root), e.g. 'src/main/webapp'
//   stubPackages: [{path, pkg}] project-relative declaration folders or files that stand in for an npm package
//     (tsconfig `paths` entries); they are named like that package and not indexed as project files
// Returns {files, errors, httpCalls}.
export function convert({ ts, scip, index, projectRoot, webappRoot = '', stubPackages = [], writer }) {
  const Kind = scip.SymbolInformation.Kind;
  const webappPrefix = webappRoot ? webappRoot.replace(/\/*$/, '/') : '';

  const stubPackageOf = relativePath => {
    for (const stub of stubPackages) {
      if (relativePath === stub.path || relativePath.startsWith(stub.path.replace(/\/*$/, '/'))) return stub.pkg;
    }
    return null;
  };
  const isStubDocument = doc => /\.d\.ts$/.test(doc.relative_path) && stubPackageOf(doc.relative_path) !== null;
  const projectDocuments = index.documents.filter(doc => !isStubDocument(doc));

  const info = new Map();
  for (const doc of index.documents) for (const si of doc.symbols) info.set(si.symbol, si);
  for (const si of index.external_symbols) info.set(si.symbol, si);
  let projectPkg;
  for (const doc of projectDocuments) {
    const o = doc.occurrences.find(x => x.symbol_roles & DEFINITION_ROLE && !x.symbol.startsWith('local'));
    if (o) {
      projectPkg = parseSymbol(o.symbol).pkg;
      break;
    }
  }

  // ---- syntax pass over every project file ----
  const files = new Map(); // relative path -> {abs, text, facts}
  const functionValued = new Set(); // scip symbols declared with a function initialiser
  const parameterProperties = new Set();
  const overloads = new Map(); // scip symbol -> [{relativePath, key, params, returns, minArgs, maxArgs}]
  const implementations = new Set(); // "relativePath|key" of an overloaded function's implementation
  for (const doc of projectDocuments) {
    const abs = path.join(projectRoot, doc.relative_path);
    const exists = fs.existsSync(abs);
    const text = exists ? fs.readFileSync(abs, 'utf8') : '';
    const facts = parseFile(ts, doc.relative_path, text);
    files.set(doc.relative_path, { abs, exists, text, facts });
    const definitions = new Map(); // symbol -> function-like declarations
    for (const o of doc.occurrences) {
      if (!(o.symbol_roles & DEFINITION_ROLE)) continue;
      const key = at(o.range[0], o.range[1]);
      if (facts.functionValued.has(key)) functionValued.add(o.symbol);
      if (facts.parameterProperties.has(key)) parameterProperties.add(o.symbol);
      const fn = facts.functionLike.get(key);
      if (fn) {
        if (!definitions.has(o.symbol)) definitions.set(o.symbol, []);
        definitions.get(o.symbol).push({ key, ...fn });
      }
    }
    for (const [symbol, decls] of definitions) {
      const signatures = decls.filter(d => !d.hasBody);
      if (decls.length < 2 || signatures.length < 2) continue;
      overloads.set(symbol, signatures.map(d => ({ relativePath: doc.relative_path, ...d })));
      for (const d of decls) if (d.hasBody) implementations.add(`${doc.relative_path}|${d.key}`);
    }
  }

  // ---- names ----
  const kindOf = (symbol, part, parentKind) => {
    const inType = parentKind === NODE.CLASS || parentKind === NODE.INTERFACE;
    if (functionValued.has(symbol)) return inType ? NODE.METHOD : NODE.FUNCTION;
    switch (Kind[info.get(symbol)?.kind]) {
      case 'Class': return NODE.CLASS;
      case 'Interface': return NODE.INTERFACE;
      case 'TypeAlias': return NODE.TYPEDEF;
      case 'Enum': return NODE.ENUM;
      case 'EnumMember': return NODE.ENUM_CONSTANT;
      case 'Method': case 'StaticMethod': case 'Constructor': case 'Getter': case 'Setter': return NODE.METHOD;
      case 'Function': return inType ? NODE.METHOD : NODE.FUNCTION;
      case 'Property': case 'StaticProperty': return NODE.FIELD;
      case 'Constant': case 'Variable':
        return parentKind === NODE.MODULE || parentKind === NODE.PACKAGE ? NODE.GLOBAL_VARIABLE : NODE.FIELD;
      case 'TypeParameter': return NODE.TYPE_PARAMETER;
      case 'File': case 'Module': return NODE.MODULE;
    }
    switch (part.d) { // not described (a library symbol): guess from the descriptor
      case 'ns': return NODE.PACKAGE;
      case 'type': return NODE.CLASS;
      case 'method': return inType ? NODE.METHOD : NODE.FUNCTION;
      case 'term': return inType || parentKind === NODE.ENUM ? NODE.FIELD : NODE.GLOBAL_VARIABLE;
      case 'typeparam': return NODE.TYPE_PARAMETER;
    }
    return NODE.SYMBOL;
  };

  // Project:  app . core . auth . account.service   (folders below the webapp root = packages, file = module)
  // Library:  @angular/common                         (npm package; the file path inside it is dropped)
  const moduleElements = (pkg, nsNames) => {
    const filePath = nsNames.join('/');
    const stub = pkg === projectPkg ? stubPackageOf(filePath) : null;
    if (pkg !== projectPkg || stub) return [{ name: stub ?? pkg, kind: NODE.PACKAGE }];
    const relative = filePath.startsWith(webappPrefix) ? filePath.slice(webappPrefix.length) : filePath;
    const segments = relative.split('/').filter(Boolean);
    const file = segments.pop();
    const els = segments.map(name => ({ name, kind: NODE.PACKAGE }));
    if (file) els.push({ name: file.replace(/(\.d)?\.[cm]?tsx?$/, ''), kind: NODE.MODULE });
    return els;
  };

  // Parameters and `local N` are local symbols, not nodes (constructor parameter properties are fields).
  const isLocalish = symbol => symbol.startsWith('local ')
    || (!parameterProperties.has(symbol) && /\.\([^()]*\)$/.test(symbol));

  const elementCache = new Map(); // scip symbol -> {els, kind} | null
  function elementsFor(symbol) {
    if (elementCache.has(symbol)) return elementCache.get(symbol);
    const parsed = parseSymbol(symbol);
    let result = null;
    if (parsed && parameterProperties.has(symbol) && parsed.parts.length >= 3) {
      // Class#`<constructor>`().(name) -> Class.name
      const owner = elementsFor(parsed.prefix(parsed.parts.length - 3));
      if (owner) result = { els: [...owner.els, { name: parsed.parts.at(-1).name, kind: NODE.FIELD }], kind: NODE.FIELD };
    } else if (parsed && parsed.parts.length && !['param', 'meta'].includes(parsed.parts.at(-1).d)) {
      const { pkg, parts } = parsed;
      let nsCount = 0;
      while (nsCount < parts.length && parts[nsCount].d === 'ns') nsCount++;
      let els = null;
      if (nsCount >= parts.length) {
        els = moduleElements(pkg, parts.map(p => p.name)); // a module (file) symbol
      } else {
        const parent = parts.length - 1 === nsCount
          ? { els: moduleElements(pkg, parts.slice(0, nsCount).map(p => p.name)) }
          : elementsFor(parsed.prefix(parts.length - 2));
        if (parent?.els) {
          const part = parts.at(-1);
          const parentKind = parent.els.length ? parent.els.at(-1).kind : NODE.PACKAGE;
          const kind = kindOf(symbol, part, parentKind);
          const el = { name: part.name === '<constructor>' ? 'constructor' : part.name, kind };
          if (CALLABLE.has(kind)) {
            const hover = (info.get(symbol)?.documentation?.[0] ?? '').replace(/^```ts\n/, '').replace(/\n```$/, '');
            Object.assign(el, signatureFromHover(hover));
          }
          els = [...parent.els, el];
        }
      }
      if (els?.length) result = { els, kind: els.at(-1).kind };
    }
    elementCache.set(symbol, result);
    return result;
  }

  const overloadElements = (symbol, overload) => {
    const base = elementsFor(symbol);
    if (!base) return null;
    const last = base.els.at(-1);
    return [...base.els.slice(0, -1),
      { name: last.name, kind: last.kind, prefix: overload.returns, postfix: `(${overload.params.join(', ')})` }];
  };

  // The element lists a reference to `symbol` stands for: one, or the matching overloads.
  function targetsFor(symbol, argumentCount) {
    const signatures = overloads.get(symbol);
    if (!signatures) {
      const r = elementsFor(symbol);
      return r ? [r.els] : [];
    }
    const matching = argumentCount === undefined ? []
      : signatures.filter(s => s.minArgs <= argumentCount && argumentCount <= s.maxArgs);
    return (matching.length ? matching : signatures).map(s => overloadElements(symbol, s)).filter(Boolean);
  }

  const serialized = new Map(); // node id -> serialized name
  function nodeFor(els) {
    const name = serialize(els);
    const kind = els.at(-1).kind;
    const parentId = writer.nodes.has(name) || els.length === 1 ? undefined : nodeFor(els.slice(0, -1));
    const id = writer.node(name, kind, parentId);
    serialized.set(id, name);
    return id;
  }

  // ---- one file at a time ----
  const httpCalls = [];
  for (const doc of projectDocuments) {
    const { abs, exists, text, facts } = files.get(doc.relative_path);
    const fileId = writer.file(abs, text, exists ? fs.statSync(abs).mtime : new Date());
    if (!exists) writer.error(fileId, abs, 0, 0, 'file not found');
    for (const d of facts.diagnostics) writer.error(fileId, abs, d.line, d.column, d.message);

    const inImport = pos => facts.imports.some(r => within(pos, r));

    // definitions: {symbol, ids, kind, pos, scope}; an overloaded function's implementation stands for all overloads
    const defs = [];
    for (const o of doc.occurrences) {
      if (!(o.symbol_roles & DEFINITION_ROLE) || isLocalish(o.symbol)) continue;
      const key = at(o.range[0], o.range[1]);
      const declared = facts.functionLike.get(key);
      const scope = o.enclosing_range.length ? o.enclosing_range : facts.extents.get(key) ?? declared?.extent ?? null;
      let ids, kind;
      const signatures = overloads.get(o.symbol);
      if (signatures && implementations.has(`${doc.relative_path}|${key}`)) {
        ids = targetsFor(o.symbol).map(nodeFor);
        kind = elementsFor(o.symbol)?.kind;
      } else {
        const overload = signatures?.find(s => s.relativePath === doc.relative_path && s.key === key);
        const els = overload ? overloadElements(o.symbol, overload) : elementsFor(o.symbol)?.els;
        if (!els) continue;
        const id = nodeFor(els);
        writer.symbol(id, DEFINITION.EXPLICIT);
        writer.location(fileId, o.range, LOCATION.TOKEN, id);
        if (scope) writer.location(fileId, scope, LOCATION.SCOPE, id);
        ids = [id];
        kind = els.at(-1).kind;
      }
      defs.push({ symbol: o.symbol, ids, kind, pos: start(o.range), key, scope });
    }
    defs.sort((a, b) => cmp(a.pos, b.pos));
    const scopes = defs.filter(d => d.scope && d.kind !== NODE.MODULE);
    const moduleDef = defs.find(d => d.kind === NODE.MODULE);

    // st's trail graph drops links whose source is a module, so module-level code (spec describe/it blocks,
    // bootstrap calls) gets a synthetic function `<module>.toplevel()` as its source.
    let topLevelDef = null;
    const topLevel = () => {
      if (topLevelDef || !moduleDef) return topLevelDef ?? moduleDef;
      const id = nodeFor([...elementsFor(moduleDef.symbol).els, { name: 'toplevel', kind: NODE.FUNCTION, postfix: '()' }]);
      writer.symbol(id, DEFINITION.IMPLICIT);
      writer.location(fileId, [0, 0, 0], LOCATION.TOKEN, id);
      topLevelDef = { ids: [id], kind: NODE.FUNCTION };
      return topLevelDef;
    };
    // the innermost declaration around a position; module-level statements belong to toplevel()
    const container = pos => {
      let inner = null;
      for (const s of scopes) {
        if (within(pos, s.scope) && (!inner || cmp(start(s.scope), start(inner.scope)) >= 0)) inner = s;
      }
      return inner ?? topLevel();
    };

    const locals = new Map();
    for (const o of doc.occurrences) {
      if (isLocalish(o.symbol)) {
        let id = locals.get(o.symbol);
        if (id === undefined) {
          id = writer.local(`${path.basename(abs)}<${o.range[0] + 1}:${o.range[1] + 1}>`);
          locals.set(o.symbol, id);
        }
        writer.location(fileId, o.range, LOCATION.LOCAL_SYMBOL, id);
        continue;
      }
      if (o.symbol_roles & DEFINITION_ROLE) continue;
      const pos = start(o.range);
      const key = at(...pos);
      const argumentCount = facts.callees.get(key);
      const targets = targetsFor(o.symbol, argumentCount).map(nodeFor);
      if (!targets.length) continue;
      if (inImport(pos)) {
        // import { X } from '...': file -> X; the module specifier: file -> module
        for (const target of targets) writer.location(fileId, o.range, LOCATION.TOKEN, writer.edge(EDGE.IMPORT, fileId, target));
        continue;
      }
      let source, type;
      if (facts.decorators.has(key)) {
        const decorated = facts.decorators.get(key);
        source = (decorated && defs.find(d => d.key === decorated)) || container(pos);
        type = EDGE.ANNOTATION_USAGE;
      } else {
        source = container(pos);
        const kind = writer.nodes.get(serialized.get(targets[0])).kind;
        if (CALLABLE.has(kind)) type = argumentCount !== undefined ? EDGE.CALL : EDGE.USAGE;
        else if (TYPES.has(kind)) {
          if (facts.heritage.has(key)) continue; // extends/implements: an inheritance edge, from the relationships
          type = EDGE.TYPE_USAGE; // also `new X()` without a constructor, which names the class
        }
        else type = EDGE.USAGE;
      }
      if (!source) continue;
      for (const from of source.ids) {
        for (const target of targets) writer.location(fileId, o.range, LOCATION.TOKEN, writer.edge(type, from, target));
      }
    }

    // relationships: implementation -> inheritance (types) or override (members)
    for (const si of doc.symbols) {
      if (isLocalish(si.symbol)) continue;
      const from = targetsFor(si.symbol);
      if (!from.length) continue;
      const fromKind = from[0].at(-1).kind;
      const def = defs.find(d => d.symbol === si.symbol);
      for (const rel of si.relationships) {
        if (!rel.is_implementation) continue;
        const to = targetsFor(rel.symbol);
        const type = TYPES.has(fromKind) ? EDGE.INHERITANCE : EDGE.OVERRIDE;
        for (const f of from) {
          for (const t of to) {
            const edgeId = writer.edge(type, nodeFor(f), nodeFor(t));
            if (type !== EDGE.INHERITANCE || !def) continue;
            for (const o of doc.occurrences) {
              if (o.symbol === rel.symbol && o.range[0] >= def.pos[0] && o.range[0] <= def.pos[0] + 2) {
                writer.location(fileId, o.range, LOCATION.TOKEN, edgeId);
              }
            }
          }
        }
      }
    }

    // HttpClient calls, for the link to the backend; spec files only mock them
    if (/\.spec\.tsx?$/.test(doc.relative_path)) continue;
    const verbSymbols = new Map();
    for (const o of doc.occurrences) verbSymbols.set(at(o.range[0], o.range[1]), o.symbol);
    for (const call of httpCallCandidates(ts, facts.sourceFile)) {
      const symbol = verbSymbols.get(at(call.line, call.column));
      const isHttpClient = symbol ? HTTP_CLIENT_METHOD.test(symbol) : /(^|\.)http$/.test(call.receiver);
      if (!isHttpClient) continue;
      const { url, resolved } = resolveUrl(ts, facts.sourceFile, call.url);
      const source = container([call.line, call.column]);
      for (const id of source?.ids ?? []) {
        httpCalls.push({ source_name: serialized.get(id), verb: call.verb, url, url_resolved: resolved,
          file: doc.relative_path, line: call.line + 1, col: call.column + 1 });
      }
    }
  }

  return { files: writer.fileCount, errors: writer.errorCount, httpCalls };
}
