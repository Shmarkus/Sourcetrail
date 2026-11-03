// Finds Angular HttpClient calls (`<expr>.get/post/put/patch/delete(url, ...)`) and resolves their URL argument with
// a little constant propagation: string literals, template strings, `+` concatenation, `this.<field>` set once,
// `const`s in scope, single-return helper methods of the same class, and `getEndpointFor(x)`, which returns x.
// Whatever cannot be resolved for certain stays unresolved: a URL is never guessed.

export const HTTP_VERBS = ['get', 'post', 'put', 'patch', 'delete'];
const MAX_DEPTH = 4;
// Unresolved parts of a URL while it is being built: request data (a parameter or local of the calling code), which
// can be a path variable, and anything else (a field, an import, a global), which leaves the URL unresolved.
const DATA = '\u0001';
const UNKNOWN = '\u0000';

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
// resolved. An unresolved part is a path variable only when it is request data filling a whole path segment; an
// unknown part anywhere in the path (a base URL, a prefix, a partial segment) leaves the whole URL unresolved.
export function resolveUrl(ts, sourceFile, expression) {
  const value = resolve(ts, expression, new Map(), 0);
  const url = value === null ? null : pathVariables(value);
  if (url === null) return { url: expression.getText(sourceFile).replace(/\s+/g, ' '), resolved: false };
  return { url, resolved: true };
}

function pathVariables(value) {
  const query = value.indexOf('?');
  const pathPart = query < 0 ? value : value.slice(0, query);
  if (pathPart.includes(UNKNOWN)) return null;
  for (let i = pathPart.indexOf(DATA); i >= 0; i = pathPart.indexOf(DATA, i + 1)) {
    const before = value[i - 1], next = value[i + 1];
    if (before !== '/' || !(next === undefined || next === '/' || next === '?')) return null;
  }
  return value.replaceAll(DATA, '{}').replaceAll(UNKNOWN, '{}');
}

function unwrap(ts, e) {
  while (ts.isParenthesizedExpression(e) || ts.isAsExpression(e) || ts.isNonNullExpression(e)
    || ts.isTypeAssertionExpression?.(e) || ts.isSatisfiesExpression?.(e)) e = e.expression;
  return e;
}

// The placeholder for an unresolved part: DATA if it reads a parameter or local of the calling code, else UNKNOWN.
function hole(ts, e) {
  let data = false;
  const visit = node => {
    if (data) return;
    if (ts.isIdentifier(node) && !(ts.isPropertyAccessExpression(node.parent) && node.parent.name === node)) {
      const binding = lookupBinding(ts, node);
      data = binding !== null && !binding.inFile;
    }
    ts.forEachChild(node, visit);
  };
  visit(e);
  return data ? DATA : UNKNOWN;
}

// env: parameter name -> {expression, env} of a helper method being resolved for one call
function resolve(ts, e, env, depth) {
  if (depth > MAX_DEPTH) return null;
  e = unwrap(ts, e);
  const part = x => resolve(ts, x, env, depth + 1) ?? hole(ts, x);
  if (ts.isStringLiteral(e) || ts.isNoSubstitutionTemplateLiteral(e)) return e.text;
  if (ts.isTemplateExpression(e)) {
    let out = e.head.text;
    for (const span of e.templateSpans) out += part(span.expression) + span.literal.text;
    return out;
  }
  if (ts.isBinaryExpression(e) && e.operatorToken.kind === ts.SyntaxKind.PlusToken) return part(e.left) + part(e.right);
  if (ts.isCallExpression(e)) {
    const callee = unwrap(ts, e.expression);
    const name = ts.isIdentifier(callee) ? callee.text : ts.isPropertyAccessExpression(callee) ? callee.name.text : null;
    if (name === 'getEndpointFor' && e.arguments.length > 0) return resolve(ts, e.arguments[0], env, depth + 1);
    if (ts.isPropertyAccessExpression(callee) && callee.expression.kind === ts.SyntaxKind.ThisKeyword) {
      const helper = classOf(ts, e)?.members.find(m => ts.isMethodDeclaration(m) && m.name?.getText() === name);
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
    const binding = lookupBinding(ts, e);
    return binding?.initializer ? resolve(ts, binding.initializer, env, depth + 1) : null;
  }
  return null;
}

function classOf(ts, node) {
  for (let n = node.parent; n; n = n.parent) if (ts.isClassDeclaration(n) || ts.isClassExpression(n)) return n;
  return null;
}

// The one value a class field is ever given: its initialiser, or its single `this.<name> = ...` statement in the
// constructor. A field given a value twice, or written anywhere else in the file, has no single value. (A readonly
// field cannot be written outside the constructor, so writes elsewhere to a `.name` are to other objects.)
function fieldValue(ts, node, name) {
  const cls = classOf(ts, node);
  if (!cls) return null;
  const field = cls.members.find(m => ts.isPropertyDeclaration(m) && m.name?.getText() === name);
  const readonly = field?.modifiers?.some(m => m.kind === ts.SyntaxKind.ReadonlyKeyword) ?? false;
  const constructorBodies = cls.members.filter(ts.isConstructorDeclaration).map(c => c.body).filter(Boolean);
  const inConstructor = n => constructorBodies.some(body => n.pos >= body.pos && n.end <= body.end);
  const values = field?.initializer ? [field.initializer] : [];
  let writtenElsewhere = false;
  const visit = n => {
    let target = null;
    if (ts.isBinaryExpression(n) && n.operatorToken.kind >= ts.SyntaxKind.FirstAssignment
      && n.operatorToken.kind <= ts.SyntaxKind.LastAssignment) target = n.left;
    else if ((ts.isPrefixUnaryExpression(n) || ts.isPostfixUnaryExpression(n))
      && (n.operator === ts.SyntaxKind.PlusPlusToken || n.operator === ts.SyntaxKind.MinusMinusToken)) target = n.operand;
    if (target && ts.isPropertyAccessExpression(target) && target.name.text === name) {
      const onThis = target.expression.kind === ts.SyntaxKind.ThisKeyword;
      const plainConstructorStatement = onThis && ts.isBinaryExpression(n)
        && n.operatorToken.kind === ts.SyntaxKind.EqualsToken && ts.isExpressionStatement(n.parent)
        && constructorBodies.includes(n.parent.parent);
      if (plainConstructorStatement) values.push(n.right);
      else if (!readonly || (onThis && inConstructor(n))) writtenElsewhere = true;
    }
    if (!writtenElsewhere) ts.forEachChild(n, visit);
  };
  visit(cls.getSourceFile());
  return !writtenElsewhere && values.length === 1 ? values[0] : null;
}

const isFunctionLike = (ts, n) => ts.isFunctionDeclaration(n) || ts.isFunctionExpression(n) || ts.isArrowFunction(n)
  || ts.isMethodDeclaration(n) || ts.isConstructorDeclaration(n) || ts.isGetAccessor(n) || ts.isSetAccessor(n);

function bindsName(ts, name, text) {
  if (ts.isIdentifier(name)) return name.text === text;
  if (ts.isObjectBindingPattern(name) || ts.isArrayBindingPattern(name)) {
    return name.elements.some(e => !ts.isOmittedExpression(e) && bindsName(ts, e.name, text));
  }
  return false;
}

// The innermost declaration of the name an identifier refers to: {initializer, inFile}, where initializer is set
// only for a `const <name> = <value>`, and inFile says the declaration is at the top of the file (not inside
// a function or block). null when the name is not declared in the file (a global).
function lookupBinding(ts, identifier) {
  const text = identifier.text;
  const inFile = n => ts.isSourceFile(n);
  for (let n = identifier.parent; n; n = n.parent) {
    if (isFunctionLike(ts, n) && n.parameters.some(p => bindsName(ts, p.name, text))) return { inFile: false };
    if (isFunctionLike(ts, n) && n.name && ts.isIdentifier(n.name) && n.name.text === text
      && !ts.isFunctionDeclaration(n)) return { inFile: false };
    if (ts.isCatchClause(n) && n.variableDeclaration && bindsName(ts, n.variableDeclaration.name, text)) {
      return { inFile: false };
    }
    if ((ts.isForStatement(n) || ts.isForOfStatement(n) || ts.isForInStatement(n)) && n.initializer
      && ts.isVariableDeclarationList(n.initializer)
      && n.initializer.declarations.some(d => bindsName(ts, d.name, text))) return { inFile: false };
    if (!(ts.isBlock(n) || ts.isSourceFile(n) || ts.isModuleBlock(n) || ts.isCaseClause(n) || ts.isDefaultClause(n))) {
      continue;
    }
    for (const s of n.statements) {
      if (ts.isVariableStatement(s)) {
        const d = s.declarationList.declarations.find(x => bindsName(ts, x.name, text));
        if (!d) continue;
        const isConst = (s.declarationList.flags & ts.NodeFlags.Const) !== 0;
        return { inFile: inFile(n), initializer: isConst && ts.isIdentifier(d.name) ? d.initializer ?? null : null };
      }
      if ((ts.isFunctionDeclaration(s) || ts.isClassDeclaration(s) || ts.isEnumDeclaration(s)) && s.name?.text === text) {
        return { inFile: inFile(n) };
      }
      const clause = ts.isImportDeclaration(s) && s.importClause;
      if (clause && (clause.name?.text === text || (clause.namedBindings && (ts.isNamespaceImport(clause.namedBindings)
        ? clause.namedBindings.name.text === text
        : clause.namedBindings.elements.some(e => e.name.text === text))))) return { inFile: true };
    }
  }
  return null;
}
