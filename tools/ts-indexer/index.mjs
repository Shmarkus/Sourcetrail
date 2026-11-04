#!/usr/bin/env node
// Indexes a TypeScript project into a Sourcetrail database: runs scip-typescript on the project's tsconfig, converts
// its SCIP index into Sourcetrail rows and writes the project's HttpClient calls to a JSON sidecar.
// With `--language python`, indexes a Python project the same way with scip-python.
//
// Exit codes: 0 indexed; 1 failed; 2 bad usage or tool not installed; 3 the project's node_modules are missing, or
// Node is too old for node:sqlite; 4 several TypeScript apps in one folder.
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// node:sqlite still announces itself as experimental; Sourcetrail reads anything on stderr as a failed command.
process.removeAllListeners('warning');
process.on('warning', warning => {
  if (warning.name !== 'ExperimentalWarning') process.stderr.write(`${warning.name}: ${warning.message}\n`);
});

const toolDir = path.dirname(fileURLToPath(import.meta.url));
const USAGE = 'ts-indexer: usage: index.mjs --project <dir> --tsconfig <path> --out <db> [--append] [--http-out <file>]';
const PYTHON_USAGE = 'scip-indexer: usage: index.mjs --language python --project <dir> --out <db> [--append] '
  + '[--python <python3>]';
const SKIPPED_DIRECTORIES = new Set(['node_modules', 'dist', 'build', 'target', 'out', 'coverage', 'bin', 'obj']);

class Failure extends Error {
  constructor(code, message) {
    super(message);
    this.code = code;
  }
}

function parseArguments(argv) {
  const options = { append: false };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    const value = () => {
      if (i + 1 >= argv.length) throw new Failure(2, USAGE);
      return argv[++i];
    };
    if (arg === '--project') options.project = path.resolve(value());
    else if (arg === '--tsconfig') options.tsconfig = path.resolve(value());
    else if (arg === '--out') options.out = path.resolve(value());
    else if (arg === '--http-out') options.httpOut = path.resolve(value());
    else if (arg === '--append') options.append = true;
    else throw new Failure(2, USAGE);
  }
  if (!options.project || !options.tsconfig || !options.out) throw new Failure(2, USAGE);
  options.httpOut ??= path.join(path.dirname(options.out), 'ts-http-calls.json');
  return options;
}

// node:sqlite is there without a flag from Node 22.13 on.
async function checkNodeVersion(tool = 'ts-indexer') {
  try {
    await import('node:sqlite');
  } catch {
    throw new Failure(3, `${tool}: Node.js 22.13 or newer is required (found ${process.version})`);
  }
}

function loadTools() {
  const require = createRequire(path.join(toolDir, 'index.mjs'));
  try {
    return {
      ts: require('typescript'),
      scip: require('@sourcegraph/scip-typescript/dist/src/scip.js').scip,
      scipMain: require.resolve('@sourcegraph/scip-typescript/dist/src/main.js'),
    };
  } catch {
    throw new Failure(2, `ts-indexer: dependencies missing in ${toolDir}; run npm ci`);
  }
}

const posix = p => p.split(path.sep).join('/');
const relativeInside = (from, to) => {
  const r = path.relative(from, to);
  return r.startsWith('..') || path.isAbsolute(r) ? null : posix(r);
};

function readTsconfig(ts, tsconfig) {
  const read = ts.readConfigFile(tsconfig, ts.sys.readFile);
  if (read.error) {
    throw new Failure(2, `ts-indexer: cannot read ${tsconfig}: ${ts.flattenDiagnosticMessageText(read.error.messageText, ' ')}`);
  }
  const parsed = ts.parseJsonConfigFileContent(read.config, ts.sys, path.dirname(tsconfig), undefined, tsconfig);
  return { raw: read.config, parsed };
}

// Folders below `root` (other than root itself) that hold a tsconfig*.json.
function tsconfigFolders(root, depth = 4) {
  const found = new Set();
  const walk = (dir, level) => {
    let entries;
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const e of entries) {
      if (e.isFile() && dir !== root && /^tsconfig.*\.json$/.test(e.name)) found.add(dir);
      if (e.isDirectory() && level < depth && !e.name.startsWith('.') && !SKIPPED_DIRECTORIES.has(e.name)) {
        walk(path.join(dir, e.name), level + 1);
      }
    }
  };
  walk(root, 0);
  return found;
}

// One TypeScript project per folder: a root tsconfig that names no files of its own while several sub-folders have
// their own tsconfig is a multi-app layout (Nx, Angular workspaces with several projects).
function checkSingleApp(raw, tsconfig, project) {
  const ownFiles = raw.include !== undefined || (Array.isArray(raw.files) && raw.files.length > 0);
  if (!ownFiles && tsconfigFolders(path.dirname(tsconfig)).size >= 2) {
    throw new Failure(4, `ts-indexer: several TypeScript apps in ${project}; not supported yet`);
  }
}

function referencedConfigs(ts, parsed) {
  const configs = [];
  for (const reference of parsed.projectReferences ?? []) {
    const file = ts.resolveProjectReferencePath(reference);
    if (fs.existsSync(file)) configs.push(readTsconfig(ts, file).parsed);
  }
  return configs;
}

// Without node_modules, type resolution can only start if every imported package resolves some other way (for
// example through `paths` to committed declaration stubs).
function checkTypeResolution(ts, configs, project) {
  if (fs.existsSync(path.join(project, 'node_modules'))) return;
  const missing = () => new Failure(3, `ts-indexer: node_modules missing in ${project}; run npm install`);
  for (const { options, fileNames } of configs) {
    for (const type of options.types ?? []) {
      const containingFile = options.configFilePath ?? path.join(project, 'tsconfig.json');
      if (!ts.resolveTypeReferenceDirective(type, containingFile, options, ts.sys)
        .resolvedTypeReferenceDirective) throw missing();
    }
    for (const file of fileNames) {
      const imports = ts.preProcessFile(fs.readFileSync(file, 'utf8'), true, true).importedFiles;
      for (const { fileName } of imports) {
        if (fileName.startsWith('.') || path.isAbsolute(fileName)) continue;
        if (!ts.resolveModuleName(fileName, file, options, ts.sys).resolvedModule) throw missing();
      }
    }
  }
}

// The folder names are rooted at: the tsconfig's baseUrl or rootDir (or a referenced config's), relative to the
// project; '' for the project folder itself.
function webappRootOf(configs, project) {
  for (const { options } of configs) {
    for (const dir of [options.baseUrl, options.rootDir]) {
      if (dir) return relativeInside(project, dir) ?? '';
    }
  }
  return '';
}

// tsconfig `paths` entries without a wildcard whose target holds only declaration files stand in for an npm
// package, for example "@angular/core": ["types/@angular/core"].
function stubPackagesOf(configs, project) {
  const stubs = [];
  const declarationsOnly = dir => fs.readdirSync(dir, { withFileTypes: true, recursive: true })
    .every(e => !e.isFile() || e.name.endsWith('.d.ts'));
  for (const { options } of configs) {
    const base = options.baseUrl ?? options.pathsBasePath;
    for (const [key, targets] of Object.entries(options.paths ?? {})) {
      if (key.includes('*') || key.startsWith('.')) continue;
      const pkg = key.startsWith('@') ? key.split('/').slice(0, 2).join('/') : key.split('/')[0];
      for (const target of targets) {
        const abs = path.resolve(base ?? project, target);
        let stubPath = null;
        if (fs.existsSync(abs) && fs.statSync(abs).isDirectory()) stubPath = declarationsOnly(abs) ? abs : null;
        else if (fs.existsSync(`${abs}.d.ts`)) stubPath = `${abs}.d.ts`;
        else if (abs.endsWith('.d.ts') && fs.existsSync(abs)) stubPath = abs;
        const relative = stubPath && relativeInside(project, stubPath);
        if (relative) stubs.push({ path: relative, pkg });
      }
    }
  }
  return stubs;
}

function runScipTypescript(scipMain, project, tsconfig, output) {
  const r = spawnSync(process.execPath,
    [scipMain, 'index', '--cwd', project, '--output', output, '--no-progress-bar', tsconfig],
    { cwd: project, encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
  if (r.status !== 0 || !fs.existsSync(output)) {
    throw new Failure(1, `ts-indexer: scip-typescript failed (${r.error?.message ?? `exit ${r.status}`})\n`
      + `${r.stderr ?? ''}${r.stdout ?? ''}`.trim());
  }
}

async function main(argv) {
  const options = parseArguments(argv);
  // Written again only once this run has indexed: a failed run must not leave an earlier run's calls behind.
  fs.rmSync(options.httpOut, { force: true });
  await checkNodeVersion();
  const { ts, scip, scipMain } = loadTools();
  if (!fs.existsSync(options.tsconfig)) throw new Failure(2, `ts-indexer: ${options.tsconfig} not found`);

  const root = readTsconfig(ts, options.tsconfig);
  checkSingleApp(root.raw, options.tsconfig, options.project);
  const configs = [root.parsed, ...referencedConfigs(ts, root.parsed)];
  checkTypeResolution(ts, configs, options.project);

  const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'ts-indexer-'));
  try {
    const scipFile = path.join(scratch, 'index.scip');
    runScipTypescript(scipMain, options.project, options.tsconfig, scipFile);
    const index = scip.Index.deserializeBinary(fs.readFileSync(scipFile));

    const { IndexWriter } = await import('./sqlite.mjs');
    const { convert } = await import('./convert.mjs');
    const writer = new IndexWriter(options.out, { append: options.append });
    let result;
    try {
      result = convert({ ts, scip, index, projectRoot: options.project, writer,
        webappRoot: webappRootOf(configs, options.project), stubPackages: stubPackagesOf(configs, options.project) });
      writer.finish();
    } catch (e) {
      writer.abort();
      if (!writer.append) fs.rmSync(options.out, { force: true });
      throw e;
    }
    fs.writeFileSync(options.httpOut, `${JSON.stringify(result.httpCalls, null, 1)}\n`);
    process.stdout.write(`ts-indexer: ${result.files} files, ${result.errors} errors\n`);
  } finally {
    fs.rmSync(scratch, { recursive: true, force: true });
  }
}

// ---- Python ----

function parsePythonArguments(argv) {
  const options = { append: false };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    const value = () => {
      if (i + 1 >= argv.length) throw new Failure(2, PYTHON_USAGE);
      return argv[++i];
    };
    if (arg === '--project') options.project = path.resolve(value());
    else if (arg === '--out') options.out = path.resolve(value());
    else if (arg === '--python') options.python = value();
    else if (arg === '--append') options.append = true;
    else throw new Failure(2, PYTHON_USAGE);
  }
  if (!options.project || !options.out) throw new Failure(2, PYTHON_USAGE);
  return options;
}

function loadPythonTools() {
  const require = createRequire(path.join(toolDir, 'index.mjs'));
  try {
    const scipPython = require.resolve('@sourcegraph/scip-python/index.js');
    return {
      scip: require('@sourcegraph/scip-typescript/dist/src/scip.js').scip,
      scipPython,
      dist: path.join(path.dirname(scipPython), 'dist'),
    };
  } catch {
    throw new Failure(2, `scip-indexer: dependencies missing in ${toolDir}; run npm ci`);
  }
}

const isExecutable = file => {
  try {
    fs.accessSync(file, fs.constants.X_OK);
    return fs.statSync(file).isFile();
  } catch {
    return false;
  }
};

const onPath = name => (process.env.PATH ?? '').split(path.delimiter).filter(Boolean).map(dir => path.join(dir, name));

// The interpreter whose site-packages library imports resolve against: --python (a path, or a name on PATH), else
// the active virtualenv, a .venv or venv in the project, python3 on PATH. {python} or {missing: what was looked for}.
function findPython(options) {
  if (options.python) {
    const python = (options.python.includes(path.sep) ? [path.resolve(options.python)] : onPath(options.python))
      .find(isExecutable);
    return python ? { python } : { missing: options.python };
  }
  const candidates = [];
  if (process.env.VIRTUAL_ENV) candidates.push(path.join(process.env.VIRTUAL_ENV, 'bin', 'python'));
  for (const venv of ['.venv', 'venv']) candidates.push(path.join(options.project, venv, 'bin', 'python'));
  const python = [...candidates, ...onPath('python3')].find(isExecutable);
  return python ? { python } : { missing: 'python3' };
}

// scip-python (Pyright) asks `python3` on PATH for its search paths: a folder whose only python3 runs the chosen
// interpreter (a wrapper, not a link, so that a virtualenv stays one), or none. In a src layout the wrapper puts src/
// on PYTHONPATH, so that tests outside src/ resolve the project's packages. A /bin/sh script: Linux and macOS only.
function interpreterPath(scratch, python, srcDir) {
  const bin = path.join(scratch, 'bin');
  fs.mkdirSync(bin);
  if (python) {
    const quote = text => `'${text.replaceAll("'", "'\\''")}'`;
    const pythonPath = srcDir ? `PYTHONPATH=${quote(srcDir)}\${PYTHONPATH:+:\$PYTHONPATH} ` : '';
    fs.writeFileSync(path.join(bin, 'python3'), `#!/bin/sh\n${pythonPath}exec ${quote(python)} "$@"\n`,
      { mode: 0o755 });
  }
  return bin;
}

function runScipPython(scipPython, project, bin, environment, output) {
  const r = spawnSync(process.execPath,
    [scipPython, 'index', '--cwd', project, '--output', output, '--quiet', '--project-name', path.basename(project),
      '--project-version', '0', '--environment', environment],
    { cwd: project, encoding: 'utf8', maxBuffer: 64 * 1024 * 1024, env: { ...process.env, PATH: bin } });
  if (r.status !== 0 || !fs.existsSync(output)) {
    throw new Failure(1, `scip-indexer: scip-python failed (${r.error?.message ?? `exit ${r.status}`})\n`
      + `${r.stderr ?? ''}${r.stdout ?? ''}`.trim());
  }
}

async function mainPython(argv) {
  const options = parsePythonArguments(argv);
  await checkNodeVersion('scip-indexer');
  const { scip, scipPython, dist } = loadPythonTools();
  if (!fs.existsSync(options.project)) throw new Failure(2, `scip-indexer: ${options.project} not found`);
  const { python, missing } = findPython(options);

  const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'scip-indexer-'));
  try {
    const scipFile = path.join(scratch, 'index.scip');
    // Library symbols are named from their module path, so the distributions they come from need not be listed
    // (scip-python would otherwise ask pip for them).
    const environment = path.join(scratch, 'environment.json');
    fs.writeFileSync(environment, '[]\n');
    const { isSrcLayout } = await import('./python.mjs');
    const srcDir = isSrcLayout(options.project) ? path.join(options.project, 'src') : null;
    runScipPython(scipPython, options.project, interpreterPath(scratch, python, srcDir), environment, scipFile);
    const index = scip.Index.deserializeBinary(fs.readFileSync(scipFile));

    const { IndexWriter } = await import('./sqlite.mjs');
    const { convertPython } = await import('./python.mjs');
    const { loadPythonParser } = await import('./pyright.mjs');
    const parser = loadPythonParser(dist);
    const writer = new IndexWriter(options.out, { append: options.append, language: 'python' });
    let result;
    try {
      result = convertPython({ index, projectRoot: options.project, writer, parser });
      writer.finish();
    } catch (e) {
      writer.abort();
      if (!writer.append) fs.rmSync(options.out, { force: true });
      throw e;
    }
    if (missing) process.stdout.write(`scip-indexer: ${missing} not found; library symbols unresolved\n`);
    const unresolved = result.unresolvedModules;
    if (unresolved.length) {
      const shown = unresolved.slice(0, 10).join(', ') + (unresolved.length > 10 ? ', …' : '');
      process.stdout.write(`scip-indexer: ${unresolved.length} module${unresolved.length === 1 ? '' : 's'} unresolved `
        + `(${shown})\n`);
    }
    process.stdout.write(`scip-indexer: ${result.files} files, ${result.errors} errors\n`);
  } finally {
    fs.rmSync(scratch, { recursive: true, force: true });
  }
}

// --language typescript (the default) or python, anywhere on the command line
function dispatch(argv) {
  const i = argv.indexOf('--language');
  if (i < 0) return main(argv);
  const language = argv[i + 1];
  const rest = [...argv.slice(0, i), ...argv.slice(i + 2)];
  if (language === 'typescript') return main(rest);
  if (language === 'python') return mainPython(rest);
  return Promise.reject(new Failure(2, `ts-indexer: unknown language ${language ?? ''}; typescript or python`));
}

const tool = process.argv[process.argv.indexOf('--language') + 1] === 'python' ? 'scip-indexer' : 'ts-indexer';
dispatch(process.argv.slice(2)).catch(e => {
  process.stderr.write(`${e instanceof Failure ? e.message : `${tool}: ${e.stack ?? e}`}\n`);
  process.exitCode = e instanceof Failure ? e.code : 1;
});
