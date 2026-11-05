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
// Folders whose Python files are not project code (build output, environments, caches; Gradle, Maven and IDE output
// for a Python part next to a Java build)
export const SKIPPED_DIRECTORIES = new Set(['node_modules', '.sourcetrail', 'build', 'dist', '.git', '.venv', 'venv',
  '__pycache__', '.tox', '.mypy_cache', '.gradle', 'target', 'out', '.idea']);

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

// A src layout: the packages live in src/, which is no package itself, and are imported without the `src.`.
// Only Python right below src/ counts (a module, or a folder holding .py files): a Java tree's src/main/java is none.
export function isSrcLayout(projectRoot) {
  const src = path.join(projectRoot, 'src');
  if (!fs.statSync(src, { throwIfNoEntry: false })?.isDirectory() || fs.existsSync(path.join(src, '__init__.py'))) {
    return false;
  }
  const isPython = entry => entry.isFile() && entry.name.endsWith('.py');
  return fs.readdirSync(src, { withFileTypes: true }).some(entry => isPython(entry) || (entry.isDirectory()
    && !SKIPPED_DIRECTORIES.has(entry.name)
    && fs.readdirSync(path.join(src, entry.name), { withFileTypes: true }).some(isPython)));
}

// Debian packaging copies the sources to debian/<package>/ (and debian/tmp/): a file below a folder of a `debian`
// folder that holds a `control` file is a copy, not the project's.
function inPackagingStage(projectRoot, relativePath) {
  const dirs = relativePath.split('/').slice(0, -1);
  return dirs.some((dir, i) => dir === 'debian' && i + 1 < dirs.length
    && fs.existsSync(path.join(projectRoot, ...dirs.slice(0, i + 1), 'control')));
}

// The package of a project file's module symbol: `scip-python python <package> <version> \`mod\`/__init__:`.
function projectPackageOf(documents) {
  for (const doc of documents) {
    for (const o of doc.occurrences) {
      if ((o.symbol_roles & DEFINITION_ROLE) && o.symbol.endsWith('/__init__:') && !o.symbol.startsWith('local ')) {
        return parseSymbol(o.symbol).pkg;
      }
    }
  }
  return null;
}

// Converts `index` (a scip.Index from scip-python) into `writer` (an IndexWriter).
//   projectRoot: absolute folder the SCIP paths are relative to
//   parser: loadPythonParser()'s result
// Returns {files, errors, unresolvedModules}: the top-level modules imports could not resolve, in order of first use.
export function convertPython({ index, projectRoot, writer, parser }) {
  const projectDocuments = index.documents.filter(doc =>
    !doc.relative_path.split('/').slice(0, -1).some(dir => SKIPPED_DIRECTORIES.has(dir))
    && !inPackagingStage(projectRoot, doc.relative_path));

  // the package scip-python names the project's own symbols with (--project-name)
  const projectPackage = projectPackageOf(projectDocuments);

  const info = new Map();
  for (const doc of index.documents) for (const si of doc.symbols) info.set(si.symbol, si);
  for (const si of index.external_symbols) info.set(si.symbol, si);

  // Without src/ on its search path (no interpreter), scip-python names a src layout's modules `src.app.core`. Only
  // a name whose next part is in src/ is the project's.
  const srcLayout = isSrcLayout(projectRoot);
  const inSrc = new Map(); // first part after `src.` -> whether src/ holds it
  const moduleNameOf = parts => {
    const name = parts.map(p => p.name).join('.');
    if (!srcLayout || parts.length < 1 || !name.startsWith('src.')) return name;
    const first = name.split('.')[1];
    if (!inSrc.has(first)) {
      inSrc.set(first, fs.existsSync(path.join(projectRoot, 'src', first))
        || fs.existsSync(path.join(projectRoot, 'src', `${first}.py`)));
    }
    return inSrc.get(first) ? name.slice(4) : name;
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
  const projectModules = new Set(); // the project's modules and the packages around them
  const unresolvedModules = new Set(); // top-level modules an import could not resolve, reported once
  // Each file's text and syntax facts, parsed once.
  const sources = new Map(projectDocuments.map(doc => {
    const abs = path.join(projectRoot, doc.relative_path);
    const exists = fs.existsSync(abs);
    const text = exists ? fs.readFileSync(abs, 'utf8') : '';
    return [doc, { abs, exists, text, facts: parser.parse(text) }];
  }));
  const occurrencesAt = doc => {
    const map = new Map(); // position -> occurrence
    for (const o of doc.occurrences) map.set(at(o.range[0], o.range[1]), o);
    return map;
  };
  // A class's bases are the names in its base list. scip-python's relationships list the bases of the class's
  // type, which a decorator returning another type (`Type[Base]`) replaces; they serve classes whose base list
  // holds no plain name.
  const bases = new Map(); // class symbol -> base class symbols
  for (const doc of projectDocuments) {
    const occurrenceAt = occurrencesAt(doc);
    for (const [baseKey, classKey] of sources.get(doc).facts.heritage) {
      const cls = occurrenceAt.get(classKey)?.symbol;
      const base = occurrenceAt.get(baseKey)?.symbol;
      if (!cls || isLocalish(cls) || !base || isLocalish(base)) continue;
      if (!bases.has(cls)) bases.set(cls, []);
      bases.get(cls).push(base);
    }
  }
  const fromBaseList = new Set(bases.keys());
  for (const doc of projectDocuments) {
    for (const si of doc.symbols) {
      const r = elementsFor(si.symbol);
      if (r?.kind === NODE.MODULE) {
        const segments = r.module.split('.');
        for (let i = 1; i <= segments.length; i++) projectModules.add(segments.slice(0, i).join('.'));
      }
      if (r?.kind === NODE.CLASS && !fromBaseList.has(si.symbol)) {
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
    const { abs, exists, text, facts } = sources.get(doc);
    const fileId = writer.file(abs, text, exists ? fs.statSync(abs).mtime : new Date());
    if (!exists) writer.error(fileId, abs, 0, 0, 'file not found');
    for (const d of facts.diagnostics) writer.error(fileId, abs, d.line, d.column, d.message);

    const occurrenceAt = occurrencesAt(doc);

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
      // A module scip-python found in a library or the standard library is found, even when it left an imported name
      // out (`from logging.handlers import RotatingFileHandler`). A dotted module it cannot find gets the project's
      // package, and `from .. import x` gets a local for `..` even when x resolves: there the names decide.
      const moduleSymbol = occurrenceAt.get(imp.module.key)?.symbol;
      const moduleResolved = moduleSymbol !== undefined && !isLocalish(moduleSymbol)
        && parseSymbol(moduleSymbol).pkg !== projectPackage;
      const moduleFound = moduleResolved
        || (imp.names.length ? !imp.names.every(n => local(n.key)) : !local(imp.module.key));
      let module = imp.module.name;
      if (module.startsWith('.') && currentModule) {
        module = absoluteModule(module, currentModule, path.basename(doc.relative_path) === '__init__.py');
        const rest = imp.module.name.replace(/^\.+/, '');
        if (rest) aliases.set(rest, module);
        else for (const n of imp.names) aliases.set(n.name, `${module}.${n.name}`);
      }
      const unresolved = !moduleFound || (!moduleResolved && imp.names.some(n => local(n.key)));
      // one error per missing top-level module and run, at its first import; the project's own never count
      const top = module.split('.')[0];
      if (unresolved && !projectModules.has(top) && !unresolvedModules.has(top)) {
        unresolvedModules.add(top);
        writer.error(fileId, abs, ...imp.module.position, `unresolved import: ${top}`);
      }
      return { span: imp.span, unresolved };
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
          if (facts.heritage.has(key)) { // a base class: the class inherits it
            const cls = defs.find(d => d.key === facts.heritage.get(key));
            if (cls && fromBaseList.has(cls.symbol)) {
              writer.location(fileId, o.range, LOCATION.TOKEN,
                writer.edge(EDGE.INHERITANCE, cls.ids[0], targetId));
            }
            continue;
          }
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
        if (type === EDGE.INHERITANCE && fromBaseList.has(si.symbol)) continue; // from its base list, above
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

  return { files: writer.fileCount, errors: writer.errorCount, unresolvedModules: [...unresolvedModules] };
}
