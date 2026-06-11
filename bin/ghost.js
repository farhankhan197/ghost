#!/usr/bin/env node
const { execFileSync } = require('child_process');
const { binaryPath, maybeAutoUpdate } = require('../scripts/binary-manager');

const args = process.argv.slice(2);

async function main() {
  try {
    await maybeAutoUpdate({ quiet: true, args });
  } catch (_) {}

  try {
    execFileSync(binaryPath('ghost'), args, { stdio: 'inherit' });
  } catch (e) {
    if (typeof e.status === 'number') process.exit(e.status);
    if (typeof e.code === 'number') process.exit(e.code);
    process.exit(1);
  }
}

main();
