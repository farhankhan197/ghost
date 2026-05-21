#!/usr/bin/env node
const { execFileSync } = require('child_process');
const path = require('path');
const os = require('os');

const binDir = path.join(__dirname);
const ext = os.platform() === 'win32' ? '.exe' : '';
const binary = path.join(binDir, `ghost${ext}`);

try {
  execFileSync(binary, process.argv.slice(2), { stdio: 'inherit' });
} catch (e) {
  process.exit(e.code || 1);
}
