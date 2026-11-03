// `node --test tools/ts-indexer` resolves the package's `main` to this file: run every *.test.mjs beside it.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
for (const file of fs.readdirSync(here).filter(f => f.endsWith('.test.mjs')).sort()) {
  await import(pathToFileURL(path.join(here, file)).href);
}
