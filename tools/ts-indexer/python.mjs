// Converts a SCIP index written by scip-python into Sourcetrail nodes, edges and locations.
//
// Names use `.` as delimiter and follow the import path: scip-python's module descriptor (`shop.models`) gives the
// packages and the module, then classes, functions, methods and attributes. Library symbols are named the same way
// (`dataclasses.dataclass`); the distribution they come from is dropped. Which declaration a reference belongs to comes
// from SCIP's enclosing ranges; call versus usage, decorators, imports and base class lists come from a syntax-only
// parse with Pyright's parser, which also reports the syntax errors.
import fs from 'node:fs';
import path from 'node:path';

import { at, cmp, parseSymbol, serialize, start, within } from './convert.mjs';
import { DEFINITION, EDGE, LOCATION, NODE } from './sqlite.mjs';

const CALLABLE = new Set([NODE.METHOD, NODE.FUNCTION]);
const TYPES = new Set([NODE.CLASS, NODE.TYPE_PARAMETER]);
const DEFINITION_ROLE = 1;
// Folders whose Python files are not project code (build output, environments, caches)
export const SKIPPED_DIRECTORIES = new Set(['node_modules', '.sourcetrail', 'build', 'dist', '.git', '.venv', 'venv',
  '__pycache__', '.tox', '.mypy_cache']);

// "a: int, b=2, *args, c: Dict[str, int] = {}" -> ["int", "b", "*args", "Dict[str, int]"]
function parameterTypes(text) {
  const params = [];
  let depth = 0, current = '';
  for (const c of text) {
    if ('[({'.includes(c)) depth++;
    else if ('])}'.includes(c)) depth--;
    if (c === ',' && depth === 0) {
      params.push(current);
      current = '';
    } else current += c;
  }
  if (current.trim()) params.push(current);
  return params.map(param => {
    let d = 0, colon = -1, equals = -1;
    for (let i = 0; i < param.length; i++) {
      const c = param[i];
      if ('[({'.includes(c)) d++;
      else if ('])}'.includes(c)) d--;
      else if (d === 0 && c === ':' && colon < 0) colon = i;
      else if (d === 0 && c === '=' && equals < 0) equals = i;
    }
    const head = equals < 0 ? param : param.slice(0, equals);
    return (colon < 0 || (equals >= 0 && equals < colon) ? head : head.slice(colon + 1)).trim();
  }).filter(p => p && p !== '/' && p !== '*');
}

// SCIP hover "@logged\ndef place(\n  self,\n  name: str\n) -> Order:" -> {prefix: 'Order', postfix: '(str)'}; a
// method's first parameter (self, cls) is left out, and so is a return type Pyright only inferred.
function signatureFromHover(hover, isMethod) {
  const h = hover.replace(/^```python\n/, '').replace(/\n```$/, '').replace(/\s+/g, ' ');
  const def = /\bdef\s+[\w]+\s*\(/.exec(h);
  if (!def) return { prefix: '', postfix: '()' };
  const open = def.index + def[0].length - 1;
  let depth = 0, close = -1;
  for (let i = open; i < h.length; i++) {
    if ('[({'.includes(h[i])) depth++;
    else if ('])}'.includes(h[i]) && --depth === 0) {
      close = i;
      break;
    }
  }
  if (close < 0) return { prefix: '', postfix: '()' };
  let params = parameterTypes(h.slice(open + 1, close));
  if (isMethod && !/@staticmethod\b/.test(h)) params = params.slice(1);
  const returns = /^\s*->\s*(.*?)\s*:\s*$/.exec(h.slice(close + 1));
  return { prefix: returns ? returns[1] : '', postfix: `(${params.join(', ')})` };
}

// `shop.models` -> shop (package) . models (module)
const moduleElements = name => {
  const segments = name.split('.');
  return segments.map((s, i) => ({ name: s, kind: i === segments.length - 1 ? NODE.MODULE : NODE.PACKAGE }));
};

// "from ..x import y" in module `current` (a package's __init__ when `isPackage`) -> the absolute module name
function absoluteModule(relative, current, isPackage) {
  const dots = /^\.*/.exec(relative)[0].length;
  const base = current.split('.');
  if (!isPackage) base.pop();
  base.splice(base.length - (dots - 1));
  const rest = relative.slice(dots);
  return [...base, ...(rest ? [rest] : [])].join('.');
}

// Converts `index` (a scip.Index from scip-python) into `writer` (an IndexWriter).
//   projectRoot: absolute folder the SCIP paths are relative to
//   parser: loadPythonParser()'s result
// Returns {files, errors}.
export function convertPython({ index, projectRoot, writer, parser }) {
  const projectDocuments = index.documents.filter(doc =>
    !doc.relative_path.split('/').slice(0, -1).some(dir => SKIPPED_DIRECTORIES.has(dir)));

  const info = new Map();
  for (const doc of index.documents) for (const si of doc.symbols) info.set(si.symbol, si);
  for (const si of index.external_symbols) info.set(si.symbol, si);

  // A src layout's modules are imported without the `src.`, which scip-python keeps.
  const srcLayout = fs.statSync(path.join(projectRoot, 'src'), { throwIfNoEntry: false })?.isDirectory()
    && !fs.existsSync(path.join(projectRoot, 'src', '__init__.py'));
  const moduleNameOf = parts => {
    const name = parts.map(p => p.name).join('.');
    return srcLayout && name.startsWith('src.') ? name.slice(4) : name;
  };
  const leadingNamespaces = parsed => {
    let n = 0;
    while (parsed && n < parsed.parts.length && parsed.parts[n].d === 'ns') n++;
    return n;
  };

  // ---- names ----
  const elementCache = new Map(); // scip symbol -> {els, kind, module} | null
  function elementsFor(symbol) {
    if (elementCache.has(symbol)) return elementCache.get(symbol);
    const parsed = parseSymbol(symbol);
    let result = null;
    const nsCount = leadingNamespaces(parsed);
    if (nsCount) {
      const { parts } = parsed;
      const module = moduleNameOf(parts.slice(0, nsCount));
      const rest = parts.slice(nsCount);
      const last = rest.at(-1);
      if (rest.length === 1 && last.d === 'meta' && last.name === '__init__') {
        result = { els: moduleElements(module), kind: NODE.MODULE, module };
      } else if (rest.length && !['param', 'meta'].includes(last.d)) {
        const parent = rest.length === 1 ? { els: moduleElements(module) }
          : elementsFor(parsed.prefix(parts.length - 2));
        const parentKind = parent?.els.at(-1).kind;
        let kind = null;
        if (last.d === 'type') kind = NODE.CLASS;
        else if (last.d === 'method') kind = parentKind === NODE.CLASS ? NODE.METHOD : NODE.FUNCTION;
        else if (last.d === 'term') {
          // an attribute of a function is no node
          if (parentKind === NODE.CLASS) kind = NODE.FIELD;
          else if (parentKind === NODE.MODULE || parentKind === NODE.PACKAGE) kind = NODE.GLOBAL_VARIABLE;
        } else if (last.d === 'typeparam') kind = NODE.TYPE_PARAMETER;
        if (parent && kind) {
          const el = { name: last.name, kind };
          if (CALLABLE.has(kind)) {
            Object.assign(el, signatureFromHover(info.get(symbol)?.documentation?.[0] ?? '', kind === NODE.METHOD));
          }
          result = { els: [...parent.els, el], kind, module };
        }
      }
    }
    elementCache.set(symbol, result);
    return result;
  }

  // Parameters and `local N` (function variables, and names an import could not resolve) are local symbols.
  const isLocalish = symbol => symbol.startsWith('local ') || /\.\([^()]*\)$/.test(symbol);

  const serialized = new Map(); // node id -> serialized name
  function nodeFor(els) {
    const name = serialize(els);
    const kind = els.at(-1).kind;
    const parentId = writer.nodes.has(name) || els.length === 1 ? undefined : nodeFor(els.slice(0, -1));
    const id = writer.node(name, kind, parentId);
    serialized.set(id, name);
    return id;
  }

  // ---- modules and classes of the project ----
  const projectModules = new Set();
  const bases = new Map(); // class symbol -> base class symbols
  for (const doc of projectDocuments) {
    for (const si of doc.symbols) {
      const r = elementsFor(si.symbol);
      if (r?.kind === NODE.MODULE) projectModules.add(r.module);
      if (r?.kind === NODE.CLASS) {
        bases.set(si.symbol, si.relationships.filter(rel => rel.is_implementation && rel.symbol.endsWith('#'))
          .map(rel => rel.symbol));
      }
    }
  }
  // Calling a class runs its __init__, its own or the nearest base's.
  const initOf = (classSymbol, seen = new Set()) => {
    if (seen.has(classSymbol)) return null;
    seen.add(classSymbol);
    const own = `${classSymbol}__init__().`;
    if (info.has(own) && elementsFor(own)) return own;
    for (const base of bases.get(classSymbol) ?? []) {
      const inherited = initOf(base, seen);
      if (inherited) return inherited;
    }
    return null;
  };

  // ---- one file at a time ----
  for (const doc of projectDocuments) {
    const abs = path.join(projectRoot, doc.relative_path);
    const exists = fs.existsSync(abs);
    const text = exists ? fs.readFileSync(abs, 'utf8') : '';
    const facts = parser.parse(text);
    const fileId = writer.file(abs, text, exists ? fs.statSync(abs).mtime : new Date());
    if (!exists) writer.error(fileId, abs, 0, 0, 'file not found');
    for (const d of facts.diagnostics) writer.error(fileId, abs, d.line, d.column, d.message);

    const occurrenceAt = new Map(); // position -> occurrence
    for (const o of doc.occurrences) occurrenceAt.set(at(o.range[0], o.range[1]), o);

    // definitions: {symbol, ids, kind, pos, key, scope}
    const defs = [];
    for (const o of doc.occurrences) {
      if (!(o.symbol_roles & DEFINITION_ROLE) || isLocalish(o.symbol)) continue;
      const r = elementsFor(o.symbol);
      if (!r) continue;
      const id = nodeFor(r.els);
      writer.symbol(id, DEFINITION.EXPLICIT);
      writer.location(fileId, o.range, LOCATION.TOKEN, id);
      const scope = o.enclosing_range.length ? o.enclosing_range : null;
      if (scope) writer.location(fileId, scope, LOCATION.SCOPE, id);
      defs.push({ symbol: o.symbol, ids: [id], kind: r.kind, pos: start(o.range), key: at(o.range[0], o.range[1]),
        scope });
    }
    defs.sort((a, b) => cmp(a.pos, b.pos));
    const scopes = defs.filter(d => d.scope && d.kind !== NODE.MODULE);
    const moduleDef = defs.find(d => d.kind === NODE.MODULE);
    const currentModule = moduleDef && elementsFor(moduleDef.symbol).module;

    // Imports. A name an import cannot resolve is a local symbol: the import is an error, and its parts are not
    // linked to invented library nodes. scip-python names a module imported relatively (`from .models import User`,
    // `from . import helpers`) without its package: `aliases` maps those names to the real module.
    const aliases = new Map();
    const imports = facts.imports.map(imp => {
      const local = key => { // without an occurrence, scip-python left the name out: not known to be unresolved
        const symbol = occurrenceAt.get(key)?.symbol;
        return symbol !== undefined && isLocalish(symbol);
      };
      // `from .. import x` gets a local for `..` even when x resolves: the names decide
      const moduleFound = imp.names.length ? !imp.names.every(n => local(n.key)) : !local(imp.module.key);
      let module = imp.module.name;
      if (module.startsWith('.') && currentModule) {
        module = absoluteModule(module, currentModule, path.basename(doc.relative_path) === '__init__.py');
        const rest = imp.module.name.replace(/^\.+/, '');
        if (rest) aliases.set(rest, module);
        else for (const n of imp.names) aliases.set(n.name, `${module}.${n.name}`);
      }
      if (!moduleFound) writer.error(fileId, abs, ...imp.module.position, `unresolved import: ${module}`);
      else {
        for (const n of imp.names.filter(n => local(n.key))) {
          writer.error(fileId, abs, ...n.position, `unresolved import: ${module}.${n.name}`);
        }
      }
      return { span: imp.span, unresolved: !moduleFound || imp.names.some(n => local(n.key)) };
    });
    const elementsIn = symbol => {
      const parsed = parseSymbol(symbol);
      const nsCount = leadingNamespaces(parsed);
      if (!nsCount) return elementsFor(symbol);
      const module = moduleNameOf(parsed.parts.slice(0, nsCount));
      const real = aliases.get(module);
      if (!real || projectModules.has(module) || !projectModules.has(real)) return elementsFor(symbol);
      const rest = symbol.slice(parsed.prefix(nsCount - 1).length);
      return elementsFor(`${parsed.head}\`${real}\`/${rest}`);
    };
    const importAt = pos => imports.find(imp => within(pos, imp.span));

    // st's trail graph drops links whose source is a module, so module-level code gets a synthetic function
    // `<module>.toplevel()` as its source.
    let topLevelDef = null;
    const topLevel = () => {
      if (topLevelDef || !moduleDef) return topLevelDef ?? moduleDef;
      const id = nodeFor([...elementsFor(moduleDef.symbol).els,
        { name: 'toplevel', kind: NODE.FUNCTION, postfix: '()' }]);
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
      const imp = importAt(pos);
      const target = elementsIn(o.symbol);
      if (!target) continue;
      if (imp) {
        // import x / from x import y: file -> the module, file -> y
        if (imp.unresolved && !projectModules.has(target.module)) continue;
        writer.location(fileId, o.range, LOCATION.TOKEN, writer.edge(EDGE.IMPORT, fileId, nodeFor(target.els)));
        continue;
      }
      const targetId = nodeFor(target.els);
      let source, type;
      if (facts.decorators.has(key)) {
        const decorated = facts.decorators.get(key);
        source = defs.find(d => d.key === decorated) || container(pos);
        type = EDGE.ANNOTATION_USAGE;
      } else {
        source = container(pos);
        if (CALLABLE.has(target.kind)) type = facts.callees.has(key) ? EDGE.CALL : EDGE.USAGE;
        else if (TYPES.has(target.kind)) {
          if (facts.heritage.has(key)) continue; // a base class: an inheritance edge, from the relationships
          type = EDGE.TYPE_USAGE;
        } else type = EDGE.USAGE;
      }
      if (!source) continue;
      // calling a class: a type usage of it and a call of its __init__
      const init = type === EDGE.TYPE_USAGE && facts.callees.has(key) ? initOf(o.symbol) : null;
      const initId = init && nodeFor(elementsFor(init).els);
      for (const from of source.ids) {
        writer.location(fileId, o.range, LOCATION.TOKEN, writer.edge(type, from, targetId));
        if (initId) writer.location(fileId, o.range, LOCATION.TOKEN, writer.edge(EDGE.CALL, from, initId));
      }
    }

    // relationships: implementation -> inheritance (classes) or override (methods)
    for (const si of doc.symbols) {
      if (isLocalish(si.symbol)) continue;
      const from = elementsFor(si.symbol);
      if (!from) continue;
      const def = defs.find(d => d.symbol === si.symbol);
      for (const rel of si.relationships) {
        if (!rel.is_implementation) continue;
        const to = elementsFor(rel.symbol);
        if (!to) continue;
        const type = TYPES.has(from.kind) ? EDGE.INHERITANCE : EDGE.OVERRIDE;
        const edgeId = writer.edge(type, nodeFor(from.els), nodeFor(to.els));
        if (type !== EDGE.INHERITANCE || !def) continue;
        for (const o of doc.occurrences) {
          if (o.symbol === rel.symbol && o.range[0] >= def.pos[0] && o.range[0] <= def.pos[0] + 2) {
            writer.location(fileId, o.range, LOCATION.TOKEN, edgeId);
          }
        }
      }
    }
  }

  return { files: writer.fileCount, errors: writer.errorCount };
}
