// Finds Angular HttpClient calls (`<expr>.get/post/put/patch/delete(url, ...)`) and resolves their URL argument with
// a little constant propagation: string literals, template strings, `+` concatenation, `this.<field>` initialisers,
// `const`s in scope, single-return helper methods of the same class, and `getEndpointFor(x)`, which returns x.

export const HTTP_VERBS = ['get', 'post', 'put', 'patch', 'delete'];
const MAX_DEPTH = 4;
const HOLE = '\u0000'; // an unresolved part of a URL while it is being built

// Every `<receiver>.<verb>(<first argument>, ...)` call in a file; whether the receiver is an HttpClient is decided by
// the caller (from the SCIP symbol of the verb).
export function httpCallCandidates(ts, sourceFile) {
  const out = [];
  const visit = node => {
    if (ts.isCallExpression(node) && ts.isPropertyAccessExpression(node.expression)
      && HTTP_VERBS.includes(node.expression.name.text) && node.arguments.length > 0) {
      const name = node.expression.name;
      const at = sourceFile.getLineAndCharacterOfPosition(name.getStart(sourceFile));
      out.push({ line: at.line, column: at.character, verb: name.text.toUpperCase(),
        receiver: node.expression.expression.getText(sourceFile), url: node.arguments[0] });
    }
    ts.forEachChild(node, visit);
  };
  visit(sourceFile);
  return out;
}

// {url, resolved}: the URL as a string with path variables as `{}`, or the expression's text when it cannot be
// resolved. An unresolved part counts as a path variable only when it is a whole path segment; anything else
// (an unknown base URL, a partial segment) leaves the whole URL unresolved, so it is never guessed.
export function resolveUrl(ts, sourceFile, expression) {
  const value = resolve(ts, expression, new Map(), 0);
  const url = value === null ? null : pathVariables(value);
  if (url === null) return { url: expression.getText(sourceFile).replace(/\s+/g, ' '), resolved: false };
  return { url, resolved: true };
}

function pathVariables(value) {
  const query = value.indexOf('?');
  for (let i = value.indexOf(HOLE); i >= 0; i = value.indexOf(HOLE, i + 1)) {
    if (query >= 0 && i > query) break;
    const before = value[i - 1], next = value[i + 1];
    if (before !== '/' || !(next === undefined || next === '/' || next === '?')) return null;
  }
  return value.replaceAll(HOLE, '{}');
}

function unwrap(ts, e) {
  while (ts.isParenthesizedExpression(e) || ts.isAsExpression(e) || ts.isNonNullExpression(e)
    || ts.isTypeAssertionExpression?.(e) || ts.isSatisfiesExpression?.(e)) e = e.expression;
  return e;
}

// env: parameter name -> {expression, env} of a helper method being resolved for one call
function resolve(ts, e, env, depth) {
  if (depth > MAX_DEPTH) return null;
  e = unwrap(ts, e);
  if (ts.isStringLiteral(e) || ts.isNoSubstitutionTemplateLiteral(e)) return e.text;
  if (ts.isTemplateExpression(e)) {
    let out = e.head.text;
    for (const span of e.templateSpans) out += (resolve(ts, span.expression, env, depth + 1) ?? HOLE) + span.literal.text;
    return out;
  }
  if (ts.isBinaryExpression(e) && e.operatorToken.kind === ts.SyntaxKind.PlusToken) {
    return (resolve(ts, e.left, env, depth + 1) ?? HOLE) + (resolve(ts, e.right, env, depth + 1) ?? HOLE);
  }
  if (ts.isCallExpression(e)) {
    const callee = unwrap(ts, e.expression);
    const name = ts.isIdentifier(callee) ? callee.text : ts.isPropertyAccessExpression(callee) ? callee.name.text : null;
    if (name === 'getEndpointFor' && e.arguments.length > 0) return resolve(ts, e.arguments[0], env, depth + 1);
    if (ts.isPropertyAccessExpression(callee) && callee.expression.kind === ts.SyntaxKind.ThisKeyword) {
      const helper = classMembers(ts, e).find(m => ts.isMethodDeclaration(m) && m.name?.getText() === name);
      const body = helper?.body?.statements;
      if (body?.length === 1 && ts.isReturnStatement(body[0]) && body[0].expression) {
        const bound = new Map();
        helper.parameters.forEach((p, i) => {
          if (ts.isIdentifier(p.name) && e.arguments[i]) bound.set(p.name.text, { expression: e.arguments[i], env });
        });
        return resolve(ts, body[0].expression, bound, depth + 1);
      }
    }
    return null;
  }
  if (ts.isPropertyAccessExpression(e) && e.expression.kind === ts.SyntaxKind.ThisKeyword) {
    const value = fieldValue(ts, e, e.name.text);
    return value ? resolve(ts, value, new Map(), depth + 1) : null;
  }
  if (ts.isIdentifier(e)) {
    const argument = env.get(e.text);
    if (argument) return resolve(ts, argument.expression, argument.env, depth + 1);
    const value = constInScope(ts, e);
    return value ? resolve(ts, value, env, depth + 1) : null;
  }
  return null;
}

function classMembers(ts, node) {
  for (let n = node.parent; n; n = n.parent) if (ts.isClassDeclaration(n) || ts.isClassExpression(n)) return [...n.members];
  return [];
}

// The initialiser of a class field, or the value of its single `this.<name> = ...` in the constructor.
function fieldValue(ts, node, name) {
  const members = classMembers(ts, node);
  const field = members.find(m => ts.isPropertyDeclaration(m) && m.name?.getText() === name);
  if (field?.initializer) return field.initializer;
  const assigned = [];
  for (const ctor of members.filter(ts.isConstructorDeclaration)) {
    for (const s of ctor.body?.statements ?? []) {
      const x = ts.isExpressionStatement(s) && s.expression;
      if (x && ts.isBinaryExpression(x) && x.operatorToken.kind === ts.SyntaxKind.EqualsToken
        && ts.isPropertyAccessExpression(x.left) && x.left.expression.kind === ts.SyntaxKind.ThisKeyword
        && x.left.name.text === name) assigned.push(x.right);
    }
  }
  return assigned.length === 1 ? assigned[0] : null;
}

// The initialiser of a `const <name>` declared in an enclosing block, function body or the file.
function constInScope(ts, identifier) {
  for (let n = identifier.parent; n; n = n.parent) {
    if (!ts.isBlock(n) && !ts.isSourceFile(n)) continue;
    for (const s of n.statements) {
      if (!ts.isVariableStatement(s) || !(s.declarationList.flags & ts.NodeFlags.Const)) continue;
      for (const d of s.declarationList.declarations) {
        if (ts.isIdentifier(d.name) && d.name.text === identifier.text && d.initializer && d.initializer !== identifier) {
          return d.initializer;
        }
      }
    }
  }
  return null;
}
