import { after, before, describe, test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

import { EDGE, NODE, openIndex, runIndexer, scratchDir } from './helpers.mjs';

const fixture = path.resolve(import.meta.dirname, '..', '..', '..', 'testing', 'cli', 'python');
const MODELS = 'shop.models';
const SERVICE = 'shop.services.OrderService';

const python3 = (() => {
  const r = spawnSync('python3', ['--version'], { encoding: 'utf8' });
  return r.status === 0 ? 'python3' : null;
})();

const indexPython = (project, out, extra = []) =>
  runIndexer(['--language', 'python', '--project', project, '--out', out, ...extra]);

const errorsOf = index => index.db.prepare(`SELECT e.message, l.start_line, f.path FROM error e
  JOIN occurrence o ON o.element_id = e.id JOIN source_location l ON l.id = o.source_location_id
  JOIN file f ON f.id = l.file_node_id`).all().map(r => ({ ...r }));

// A copy of the fixture plus `files` ({relative path: text}), indexed into its own database.
function indexCopy(prefix, files, extra = [], setup = () => {}) {
  const dir = scratchDir(prefix);
  const project = path.join(dir, 'project');
  fs.cpSync(fixture, project, { recursive: true });
  for (const [file, text] of Object.entries(files)) {
    fs.mkdirSync(path.dirname(path.join(project, file)), { recursive: true });
    fs.writeFileSync(path.join(project, file), text);
  }
  setup(project);
  const out = path.join(dir, 'py.srctrldb');
  const run = indexPython(project, out, extra);
  return { dir, project, run, index: run.code === 0 ? openIndex(out) : null };
}

const PAYMENTS = `import paylink


def settle(total: float) -> bool:
    return paylink.charge(total)
`;

describe('the Python fixture index', () => {
  let dir, run, index;

  before(() => {
    dir = scratchDir('python');
    run = indexPython(fixture, path.join(dir, 'py.srctrldb'));
    index = openIndex(path.join(dir, 'py.srctrldb'));
  });
  after(() => {
    index?.close();
    fs.rmSync(dir, { recursive: true, force: true });
  });

  test('runsCleanly', () => {
    assert.equal(run.code, 0, run.stderr);
    assert.equal(run.stderr, '');
    const files = index.db.prepare('SELECT count(*) AS n FROM file').get().n;
    assert.equal(files, 4);
    assert.match(run.stdout, new RegExp(`scip-indexer: ${files} files, 0 errors\n$`));
    for (const f of index.db.prepare('SELECT language, indexed, complete FROM file').all()) {
      assert.deepEqual({ ...f }, { language: 'python', indexed: 1, complete: 1 });
    }
  });

  test('namesFollowTheImportPath', () => {
    assert.deepEqual(index.named(`${MODELS}.User`).map(n => n.type), [NODE.CLASS]);
    assert.deepEqual(index.named(`${SERVICE}.place`).map(n => n.type), [NODE.METHOD]);
    assert.deepEqual(index.named('shop.services.logged').map(n => n.type), [NODE.FUNCTION]);
    assert.deepEqual(index.named(MODELS).map(n => n.type), [NODE.MODULE]);
    assert.deepEqual(index.named('shop').map(n => n.type), [NODE.MODULE], 'shop/__init__.py');
    assert.deepEqual(index.named(`${MODELS}.User.email`).map(n => n.type), [NODE.FIELD]);
    assert.deepEqual(index.named(`${MODELS}.DEFAULT_COUNTRY`).map(n => n.type), [NODE.GLOBAL_VARIABLE]);
    assert.deepEqual(index.named(`${MODELS}.Order.owner`).map(n => n.type), [NODE.FIELD], 'a dataclass field');
    assert.deepEqual(index.named(`${MODELS}.User.display_name`).map(n => n.type), [NODE.METHOD], 'a property');
    const [place] = index.named(`${SERVICE}.place`);
    assert.ok(place.serialized_name.endsWith('\tsOrder\tp(str, str, str)'), place.serialized_name);
    assert.equal(index.named(`${SERVICE}.place.self`).length, 0, 'parameters are local symbols');
  });

  test('callsReachConstructorsAndMethods', () => {
    const place = `${SERVICE}.place`;
    assert.deepEqual(index.edgesBetween(place, `${MODELS}.User.__init__`).map(e => e.type), [EDGE.CALL]);
    assert.deepEqual(index.edgesBetween(place, `${MODELS}.User`).map(e => e.type), [EDGE.TYPE_USAGE]);
    assert.deepEqual(index.edgesBetween(place, `${MODELS}.User.greet`).map(e => e.type), [EDGE.CALL]);
    assert.deepEqual(index.edgesBetween(place, `${MODELS}.Order.add`).map(e => e.type), [EDGE.CALL]);
    // a dataclass has no __init__ of its own to call
    assert.deepEqual(index.edgesBetween(place, `${MODELS}.Order`).map(e => e.type), [EDGE.TYPE_USAGE]);
    // super().__init__(...)
    assert.deepEqual(index.edgesBetween(`${MODELS}.Admin.__init__`, `${MODELS}.User.__init__`).map(e => e.type),
      [EDGE.CALL]);
    // a property read is a usage, not a call
    assert.deepEqual(index.edgesBetween(`${MODELS}.User.greet`, `${MODELS}.User.display_name`).map(e => e.type),
      [EDGE.USAGE]);
  });

  test('inheritanceAndOverride', () => {
    assert.deepEqual(index.edgesBetween(`${MODELS}.Admin`, `${MODELS}.User`).map(e => e.type), [EDGE.INHERITANCE]);
    assert.deepEqual(index.edgesBetween(`${MODELS}.Admin.can_refund`, `${MODELS}.User.can_refund`).map(e => e.type),
      [EDGE.OVERRIDE]);
  });

  test('decoratorsAreAnnotationUsages', () => {
    assert.deepEqual(index.edgesBetween(`${SERVICE}.place`, 'shop.services.logged').map(e => e.type),
      [EDGE.ANNOTATION_USAGE]);
    assert.deepEqual(index.edgesBetween(`${MODELS}.Order`, 'dataclasses.dataclass').map(e => e.type),
      [EDGE.ANNOTATION_USAGE]);
    assert.deepEqual(index.edgesBetween(`${MODELS}.User.display_name`, 'builtins.property').map(e => e.type),
      [EDGE.ANNOTATION_USAGE]);
  });

  test('reExportsLandOnTheRealClass', () => {
    // services.py: `from shop import User`, re-exported by shop/__init__.py: `from .models import ... User`
    const services = index.nodes.find(n => n.type === NODE.FILE && n.name.endsWith('/shop/services.py'));
    const imports = index.edges.filter(e => e.type === EDGE.IMPORT && e.source === services).map(e => e.target.name);
    assert.ok(imports.includes(`${MODELS}.User`), imports.join(', '));
    assert.deepEqual(index.nodes.filter(n => n.name === 'shop.User').map(n => n.name), [], 'no node for the re-export');
    // the package imports the real classes and, for `from .models`, the module
    const init = index.nodes.find(n => n.type === NODE.FILE && n.name.endsWith('/shop/__init__.py'));
    const initImports = index.edges.filter(e => e.type === EDGE.IMPORT && e.source === init).map(e => e.target.name);
    assert.deepEqual([...new Set(initImports)].sort(),
      [MODELS, `${MODELS}.Admin`, `${MODELS}.Order`, `${MODELS}.User`]);
    assert.deepEqual(index.named('models').map(n => n.name), [], 'the relative import names the real module');
  });

  test('testFunctionsAreIndexed', () => {
    assert.deepEqual(index.named('tests.test_services.test_place').map(n => n.type), [NODE.FUNCTION]);
    assert.deepEqual(index.edgesBetween('tests.test_services.test_place', `${SERVICE}.place`).map(e => e.type),
      [EDGE.CALL]);
  });

  test('moduleCodeGoesToToplevel', () => {
    const fromModules = index.edges.filter(e => e.source.type === NODE.MODULE && e.type !== EDGE.MEMBER);
    assert.deepEqual(fromModules.map(e => `${e.source.name} -> ${e.target.name}`), []);
    assert.deepEqual(index.edgesBetween('shop.models.toplevel', 'dataclasses.dataclass').map(e => e.type), []);
  });

  test('definitionsHaveLocations', () => {
    const [place] = index.named(`${SERVICE}.place`);
    const locations = index.db.prepare(`SELECT l.start_line, l.start_column, l.end_line, l.end_column, l.type, f.path
      FROM occurrence o JOIN source_location l ON l.id = o.source_location_id JOIN file f ON f.id = l.file_node_id
      WHERE o.element_id = ? ORDER BY l.type`).all(place.id).map(r => ({ ...r }));
    const file = path.join(fixture, 'shop/services.py');
    assert.deepEqual(locations, [
      { start_line: 20, start_column: 9, end_line: 20, end_column: 13, type: 0, path: file },
      { start_line: 19, start_column: 5, end_line: 26, end_column: 20, type: 1, path: file },
    ]);
  });
});

describe('Python projects', () => {
  test('aSyntaxErrorIsAnErrorRowAndTheRestIndexes', () => {
    const { dir, project, run, index } = indexCopy('pysyntax', { 'shop/broken.py': 'def broken(:\n    return 2\n' });
    try {
      assert.equal(run.code, 0, run.stderr);
      const errors = errorsOf(index);
      assert.ok(errors.length >= 1);
      assert.ok(errors.every(e => e.path === path.join(project, 'shop/broken.py')), JSON.stringify(errors));
      assert.equal(errors[0].start_line, 1);
      assert.match(run.stdout, new RegExp(`scip-indexer: 5 files, ${errors.length} errors\n$`));
      assert.equal(index.named(`${SERVICE}.place`).length, 1, 'the other files are indexed');
    } finally {
      index?.close();
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });

  test('withoutAnEnvironmentLibraryCallsAreUnresolved', () => {
    const { dir, project, run, index } = indexCopy('pynoenv', { 'shop/payments.py': PAYMENTS }, ['--python',
      '/nonexistent/python3']);
    try {
      assert.equal(run.code, 0, run.stderr);
      assert.equal(run.stderr, '');
      assert.equal(run.stdout, 'scip-indexer: /nonexistent/python3 not found; library symbols unresolved\n'
        + 'scip-indexer: 1 module unresolved (paylink)\nscip-indexer: 5 files, 1 errors\n');
      assert.deepEqual(errorsOf(index), [{ message: 'unresolved import: paylink', start_line: 1,
        path: path.join(project, 'shop/payments.py') }]);
      assert.deepEqual(index.nodes.filter(n => n.name.startsWith('paylink')).map(n => n.name), [], 'nothing invented');
      assert.deepEqual(index.named('shop.payments.settle').map(n => n.type), [NODE.FUNCTION]);
      // the standard library still resolves, from the typeshed stubs scip-python ships
      assert.deepEqual(index.edgesBetween(`${MODELS}.Order`, 'dataclasses.dataclass').map(e => e.type),
        [EDGE.ANNOTATION_USAGE]);
    } finally {
      index?.close();
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });

  test('aProjectVenvResolvesLibraryCalls', { skip: !python3 && 'python3 not found' }, () => {
    const venv = project => {
      const venvDir = path.join(project, '.venv');
      assert.equal(spawnSync(python3, ['-m', 'venv', '--without-pip', venvDir]).status, 0);
      const site = spawnSync(path.join(venvDir, 'bin', 'python'), ['-c',
        'import sysconfig; print(sysconfig.get_paths()["purelib"])'], { encoding: 'utf8' }).stdout.trim();
      fs.mkdirSync(path.join(site, 'paylink'));
      fs.writeFileSync(path.join(site, 'paylink', '__init__.py'),
        'def charge(amount: float) -> bool:\n    return True\n');
      const info = path.join(site, 'paylink-1.2.0.dist-info');
      fs.mkdirSync(info);
      fs.writeFileSync(path.join(info, 'METADATA'), 'Metadata-Version: 2.1\nName: paylink\nVersion: 1.2.0\n');
      fs.writeFileSync(path.join(info, 'RECORD'), 'paylink/__init__.py,,\npaylink-1.2.0.dist-info/METADATA,,\n');
    };
    const { dir, run, index } = indexCopy('pyvenv', { 'shop/payments.py': PAYMENTS }, [], venv);
    try {
      assert.equal(run.code, 0, run.stderr);
      assert.equal(run.stdout, 'scip-indexer: 5 files, 0 errors\n');
      assert.deepEqual(errorsOf(index), []);
      assert.deepEqual(index.named('paylink.charge').map(n => n.type), [NODE.FUNCTION]);
      assert.deepEqual(index.edgesBetween('shop.payments.settle', 'paylink.charge').map(e => e.type), [EDGE.CALL]);
      const files = index.db.prepare("SELECT path FROM file WHERE path LIKE '%.venv%'").all();
      assert.deepEqual(files, [], 'the environment is not project code');
    } finally {
      index?.close();
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });

  test('anUnresolvedModuleIsOneErrorPerRun', () => {
    const files = {};
    for (const name of ['a', 'b', 'c', 'd']) {
      files[`shop/uses_${name}.py`] = 'import paylink\nfrom stockbook.api import level\n\n\n'
        + 'def f():\n    return level()\n';
    }
    files['shop/known.py'] = 'from shop.models import NoSuchName\n';
    const { dir, project, run, index } = indexCopy('pyunresolved', files, ['--python', '/nonexistent/python3']);
    try {
      assert.equal(run.code, 0, run.stderr);
      const errors = errorsOf(index).sort((x, y) => x.message.localeCompare(y.message));
      assert.deepEqual(errors.map(e => e.message), ['unresolved import: paylink', 'unresolved import: stockbook']);
      assert.ok(errors.every(e => e.path === path.join(project, 'shop/uses_a.py')), 'at the first import');
      assert.match(run.stdout, /^scip-indexer: 2 modules unresolved \(paylink, stockbook\)\n/m);
      assert.match(run.stdout, /scip-indexer: 9 files, 2 errors\n$/);
      assert.deepEqual(index.nodes.filter(n => /^(paylink|stockbook)\b/.test(n.name)).map(n => n.name), []);
    } finally {
      index?.close();
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });

  test('theUnresolvedListIsCapped', () => {
    const imports = Array.from({ length: 12 }, (_, i) => `import lib${String(i).padStart(2, '0')}\n`).join('');
    const { dir, run, index } = indexCopy('pycap', { 'shop/many.py': imports }, ['--python', '/nonexistent/python3']);
    try {
      assert.equal(run.code, 0, run.stderr);
      const shown = Array.from({ length: 10 }, (_, i) => `lib0${i}`).join(', ');
      assert.ok(run.stdout.includes(`scip-indexer: 12 modules unresolved (${shown}, …)\n`), run.stdout);
    } finally {
      index?.close();
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });

  const SRC_LAYOUT = { 'src/app/__init__.py': '', 'src/app/core.py': 'def run():\n    return 1\n',
    'tests/test_core.py': 'from app.core import run\n\n\ndef test_run():\n    run()\n' };
  const srcLayout = (prefix, extra) => {
    const dir = scratchDir(prefix);
    for (const [file, text] of Object.entries(SRC_LAYOUT)) {
      fs.mkdirSync(path.dirname(path.join(dir, file)), { recursive: true });
      fs.writeFileSync(path.join(dir, file), text);
    }
    fs.writeFileSync(path.join(dir, 'pyproject.toml'), '[project]\nname = "app"\n');
    const out = path.join(dir, 'py.srctrldb');
    const run = indexPython(dir, out, extra);
    return { dir, run, index: run.code === 0 ? openIndex(out) : null };
  };

  test('aSrcLayoutLinksTestsToTheProject', { skip: !python3 && 'python3 not found' }, () => {
    const { dir, run, index } = srcLayout('pysrc', []);
    try {
      assert.equal(run.code, 0, run.stderr);
      assert.equal(run.stdout, 'scip-indexer: 3 files, 0 errors\n');
      assert.deepEqual(index.named('app.core.run').map(n => n.type), [NODE.FUNCTION]);
      assert.deepEqual(index.edgesBetween('tests.test_core.test_run', 'app.core.run').map(e => e.type), [EDGE.CALL]);
    } finally {
      index?.close();
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });

  test('aProjectModuleIsNeverUnresolved', () => {
    // without an interpreter, src/ is not on the search path: the import does not link, but it is no error
    const { dir, run, index } = srcLayout('pysrcnopy', ['--python', '/nonexistent/python3']);
    try {
      assert.equal(run.code, 0, run.stderr);
      assert.deepEqual(errorsOf(index), []);
      assert.deepEqual(index.named('app.core.run').map(n => n.type), [NODE.FUNCTION]);
    } finally {
      index?.close();
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });

  test('skippedFoldersAreNotIndexed', () => {
    const { dir, run, index } = indexCopy('pyskip', { 'build/lib/shop/models.py': 'class Copy:\n    pass\n',
      'dist/x.py': 'y = 1\n' });
    try {
      assert.equal(run.code, 0, run.stderr);
      const skipped = "SELECT count(*) AS n FROM file WHERE path LIKE '%/build/%' OR path LIKE '%/dist/%'";
      assert.equal(index.db.prepare(skipped).get().n, 0);
    } finally {
      index?.close();
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });

  test('appendKeepsExistingRows', () => {
    const dir = scratchDir('pyappend');
    try {
      const out = path.join(dir, 'all.srctrldb');
      assert.equal(indexPython(fixture, out).code, 0);
      const r = indexPython(fixture, out, ['--append']);
      assert.equal(r.code, 0, r.stderr);
      const index = openIndex(out);
      try {
        assert.equal(index.named(`${MODELS}.User`).length, 1, 'nodes are reused');
      } finally {
        index.close();
      }
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });

  test('usageErrorsExit2', () => {
    const r = runIndexer(['--language', 'python', '--out', 'x.srctrldb']);
    assert.equal(r.code, 2);
    assert.match(r.stderr, /^scip-indexer: usage: /);
    assert.equal(runIndexer(['--language', 'cobol', '--project', fixture, '--out', 'x']).code, 2);
  });
});
