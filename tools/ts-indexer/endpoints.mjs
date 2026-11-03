// Spring MVC request mappings read from Java source, and the matching of frontend HTTP calls to them.
//
// A handler is a method of a @RestController/@Controller class with @Get/Post/Put/Patch/Delete/RequestMapping; its
// path is the class's @RequestMapping path joined with the method's. Interfaces (Feign clients, API interfaces) hold no
// handlers. A mapping whose path is not made of string literals and the file's own string constants is reported, not
// guessed.

export const VERBS = ['GET', 'POST', 'PUT', 'PATCH', 'DELETE'];
// The verb of a @RequestMapping without `method`: it takes every verb, but a mapping naming the verb comes first.
export const ANY = 'ANY';

// "/api/orders/{}" for "api/orders/{id}", "/api//orders/", "/api/orders/${id}?x=1": path variables become `{}`, the
// query goes, slashes are collapsed, one leading slash and no trailing one.
export function normalizePath(path) {
  const withoutQuery = path.split('?')[0];
  const variables = withoutQuery.replace(/\$\{[^}]*\}/g, '{}').replace(/\{[^}]*\}/g, '{}');
  return `/${variables.split('/').filter(Boolean).join('/')}`;
}

// The text with comments blanked out (newlines kept), so that positions stay the same and commented-out code is gone.
export function blankComments(text) {
  let out = '';
  for (let i = 0; i < text.length;) {
    const c = text[i];
    if (c === '/' && text[i + 1] === '/') {
      const end = text.indexOf('\n', i);
      const stop = end < 0 ? text.length : end;
      out += ' '.repeat(stop - i);
      i = stop;
    } else if (c === '/' && text[i + 1] === '*') {
      const end = text.indexOf('*/', i + 2);
      const stop = end < 0 ? text.length : end + 2;
      out += text.slice(i, stop).replace(/[^\n]/g, ' ');
      i = stop;
    } else if (c === '"' || c === "'") {
      const stop = literalEnd(text, i);
      out += text.slice(i, stop);
      i = stop;
    } else {
      out += c;
      i++;
    }
  }
  return out;
}

// The index after the string, text block or char literal starting at `start`.
function literalEnd(text, start) {
  const quote = text[start];
  if (quote === '"' && text.startsWith('"""', start)) {
    const end = text.indexOf('"""', start + 3);
    return end < 0 ? text.length : end + 3;
  }
  for (let i = start + 1; i < text.length; i++) {
    if (text[i] === '\\') i++;
    else if (text[i] === quote || text[i] === '\n') return i + 1;
  }
  return text.length;
}

// The index of the parenthesis closing the one at `open`, or -1.
function closingParen(text, open) {
  let depth = 0;
  for (let i = open; i < text.length; i++) {
    const c = text[i];
    if (c === '"' || c === "'") i = literalEnd(text, i) - 1;
    else if (c === '(') depth++;
    else if (c === ')' && --depth === 0) return i;
  }
  return -1;
}

// Splits at top-level commas.
function splitTopLevel(text) {
  const parts = [];
  let depth = 0, start = 0;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === '"' || c === "'") i = literalEnd(text, i) - 1;
    else if ('({['.includes(c)) depth++;
    else if (')}]'.includes(c)) depth--;
    else if (c === ',' && depth === 0) {
      parts.push(text.slice(start, i));
      start = i + 1;
    }
  }
  parts.push(text.slice(start));
  return parts.map(p => p.trim()).filter(Boolean);
}

// The string of `"a"` or `"a" + B`, where B is one of `constants`; null otherwise.
function stringValue(expression, constants) {
  let value = '';
  for (const part of expression.split(/\s*\+\s*/).map(p => p.trim())) {
    const m = /^"((?:[^"\\]|\\.)*)"$/.exec(part);
    if (m) value += m[1].replace(/\\(.)/g, '$1');
    else if (constants.has(part)) value += constants.get(part);
    else return null;
  }
  return value;
}

// The strings of `"a"`, `"a" + B` or `{"a", B}`; null if any part is neither a string literal nor one of `constants`.
function stringValues(expression, constants) {
  const list = expression.startsWith('{') && expression.endsWith('}');
  const values = (list ? splitTopLevel(expression.slice(1, -1)) : [expression]).map(i => stringValue(i, constants));
  return values.includes(null) ? null : values;
}

// The file's `static final String` constants made of string literals, by name: one level of constant propagation.
function stringConstants(text) {
  const constants = new Map();
  const re = /\b(?=[\w\s]*\bstatic\b)(?=[\w\s]*\bfinal\b)(?:\w+\s+)*String\s+(\w+)\s*=\s*([^;]*);/g;
  for (const m of text.matchAll(re)) {
    const value = stringValue(m[2].trim(), new Map());
    if (value !== null) constants.set(m[1], value);
  }
  return constants;
}

// {paths, verbs} of an annotation's arguments, or {error}; `paths` is [''] without a path, `verbs` null without
// `method`.
function mappingArguments(args, constants) {
  let paths = [''], verbs = null;
  for (const part of splitTopLevel(args)) {
    const named = /^(\w+)\s*=\s*([\s\S]*)$/.exec(part);
    const [name, value] = named ? [named[1], named[2].trim()] : ['value', part];
    if (name === 'value' || name === 'path') {
      paths = stringValues(value, constants);
      if (paths === null) return { error: `path is not a string literal: ${value.replace(/\s+/g, ' ')}` };
      if (paths.length === 0) paths = [''];
    } else if (name === 'method') {
      const list = value.startsWith('{') ? splitTopLevel(value.slice(1, -1)) : [value];
      verbs = list.map(v => v.replace(/^(?:[\w.]*\.)?RequestMethod\./, ''));
      if (verbs.some(v => !/^[A-Z]+$/.test(v))) return { error: `method is not a RequestMethod: ${value}` };
    }
  }
  return { paths, verbs };
}

const lineColumn = (text, index) => {
  const before = text.slice(0, index);
  const line = before.split('\n').length;
  return { line, column: index - before.lastIndexOf('\n') };
};

// The mappings a Java file declares: {handlers: [{verb, path, start, end}], problems: [{line, message}]}. Paths are
// string literals, or the file's own string constants (see stringConstants). `start` and `end` are the annotation's
// 1-based line and column (end inclusive); the handler method is the first one declared after `end`.
export function springHandlers(source) {
  const text = blankComments(source);
  // Searched for declarations and annotations: the text with string contents blanked too.
  const structure = text.replace(/"(?:[^"\\\n]|\\.)*"/g, s => `"${' '.repeat(s.length - 2)}"`);
  const handlers = [], problems = [];
  const constants = stringConstants(text);
  const type = /\b(class|interface|enum|record)\s+\w+/.exec(structure);
  if (!type || type[1] === 'interface') return { handlers, problems };
  const header = structure.slice(0, type.index);
  if (!/@(?:[\w.]+\.)?(?:RestController|Controller)\b/.test(header)) return { handlers, problems };

  const annotations = (from, to) => {
    const found = [];
    const re = /@(?:[\w.]+\.)?(Get|Post|Put|Patch|Delete|Request)Mapping\b\s*/g;
    re.lastIndex = from;
    for (let m; (m = re.exec(structure)) && m.index < to;) {
      let args = '', last = m.index + m[0].trimEnd().length - 1;
      if (structure[re.lastIndex] === '(') {
        const close = closingParen(text, re.lastIndex);
        if (close < 0) break;
        args = text.slice(re.lastIndex + 1, close);
        last = close;
        re.lastIndex = close + 1;
      }
      found.push({ kind: m[1], args, index: m.index, last });
    }
    return found;
  };

  let prefixes = [''];
  const classMapping = annotations(0, type.index).find(a => a.kind === 'Request');
  if (classMapping) {
    const parsed = mappingArguments(classMapping.args, constants);
    if (parsed.error) {
      problems.push({ line: lineColumn(text, classMapping.index).line, message: parsed.error });
      return { handlers, problems };
    }
    prefixes = parsed.paths;
  }

  for (const a of annotations(type.index, text.length)) {
    const start = lineColumn(text, a.index), end = lineColumn(text, a.last);
    const parsed = mappingArguments(a.args, constants);
    if (parsed.error) {
      problems.push({ line: start.line, message: parsed.error });
      continue;
    }
    const verbs = a.kind === 'Request' ? parsed.verbs ?? [ANY] : [a.kind.toUpperCase()];
    for (const prefix of prefixes) {
      for (const path of parsed.paths) {
        for (const verb of verbs) handlers.push({ verb, path: normalizePath(`${prefix}/${path}`), start, end });
      }
    }
  }
  return { handlers, problems };
}

// The handlers a call with `verb` and normalised `path` reaches: those naming the verb, else the verb-less ones.
export function matchingHandlers(handlers, verb, path) {
  const onPath = handlers.filter(h => h.path === path);
  const exact = onPath.filter(h => h.verb === verb);
  return exact.length > 0 ? exact : onPath.filter(h => h.verb === ANY);
}
