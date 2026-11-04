#!/usr/bin/env node
// Indexes a TypeScript project into a Sourcetrail database: runs scip-typescript on the project's tsconfig, converts
// its SCIP index into Sourcetrail rows and writes the project's HttpClient calls to a JSON sidecar.
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
async function checkNodeVersion() {
  try {
    await import('node:sqlite');
  } catch {
    throw new Failure(3, `ts-indexer: Node.js 22.13 or newer is required (found ${process.version})`);
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

main(process.argv.slice(2)).catch(e => {
  process.stderr.write(`${e instanceof Failure ? e.message : `ts-indexer: ${e.stack ?? e}`}\n`);
  process.exitCode = e instanceof Failure ? e.code : 1;
});
