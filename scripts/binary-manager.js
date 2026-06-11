const https = require('https');
const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');
const { execFileSync } = require('child_process');

const REPO = process.env.GHOST_RELEASE_REPO || 'farhankhan197/ghost';
const BIN_DIR = path.join(__dirname, '..', 'bin');
const DEFAULT_TIMEOUT_MS = 30000;
const DEFAULT_AUTO_UPDATE_TIMEOUT_MS = 2500;
const DEFAULT_CHECK_INTERVAL_MS = 60 * 60 * 1000;

function getPlatform() {
  const platform = os.platform();
  const arch = os.arch();

  let osName;
  if (platform === 'win32') osName = 'windows';
  else if (platform === 'linux') osName = 'linux';
  else return null;

  let archName;
  if (arch === 'x64') archName = 'x86_64';
  else return null;

  return { osName, archName };
}

function binaryName(kind = 'ghost') {
  const ext = os.platform() === 'win32' ? '.exe' : '';
  return `${kind}${ext}`;
}

function binaryPath(kind = 'ghost') {
  return path.join(BIN_DIR, binaryName(kind));
}

function releaseAssetName(kind, platform = getPlatform()) {
  if (!platform) return null;
  const ext = platform.osName === 'windows' ? '.exe' : '';
  return `${kind}-${platform.osName}-${platform.archName}${ext}`;
}

function requestText(url, { timeoutMs = DEFAULT_TIMEOUT_MS } = {}) {
  return new Promise((resolve, reject) => {
    const client = url.startsWith('https') ? https : http;
    const req = client.get(url, { headers: { 'User-Agent': 'ghost-installer' } }, (res) => {
      if (res.statusCode === 301 || res.statusCode === 302 || res.statusCode === 307 || res.statusCode === 308) {
        res.resume();
        const next = new URL(res.headers.location, url).toString();
        requestText(next, { timeoutMs }).then(resolve).catch(reject);
        return;
      }

      if (res.statusCode < 200 || res.statusCode >= 300) {
        res.resume();
        reject(new Error(`HTTP ${res.statusCode}: ${url}`));
        return;
      }

      let data = '';
      res.setEncoding('utf8');
      res.on('data', (chunk) => { data += chunk; });
      res.on('end', () => resolve(data));
    });

    req.setTimeout(timeoutMs, () => {
      req.destroy(new Error(`Timed out after ${timeoutMs}ms`));
    });
    req.on('error', reject);
  });
}

function downloadFile(url, dest, { timeoutMs = DEFAULT_TIMEOUT_MS } = {}) {
  return new Promise((resolve, reject) => {
    const client = url.startsWith('https') ? https : http;
    const req = client.get(url, { headers: { 'User-Agent': 'ghost-installer' } }, (res) => {
      if (res.statusCode === 301 || res.statusCode === 302 || res.statusCode === 307 || res.statusCode === 308) {
        res.resume();
        const next = new URL(res.headers.location, url).toString();
        downloadFile(next, dest, { timeoutMs }).then(resolve).catch(reject);
        return;
      }

      if (res.statusCode !== 200) {
        res.resume();
        reject(new Error(`HTTP ${res.statusCode}: ${url}`));
        return;
      }

      fs.mkdirSync(path.dirname(dest), { recursive: true });
      const tmp = `${dest}.download`;
      try {
        if (fs.existsSync(tmp)) fs.unlinkSync(tmp);
      } catch (_) {}

      const file = fs.createWriteStream(tmp);
      res.pipe(file);
      file.on('finish', () => {
        file.close(() => {
          try {
            fs.chmodSync(tmp, 0o755);
            fs.renameSync(tmp, dest);
            fs.chmodSync(dest, 0o755);
            resolve();
          } catch (err) {
            reject(err);
          }
        });
      });
      file.on('error', reject);
    });

    req.setTimeout(timeoutMs, () => {
      req.destroy(new Error(`Timed out after ${timeoutMs}ms`));
    });
    req.on('error', reject);
  });
}

async function fetchLatestRelease({ timeoutMs = DEFAULT_TIMEOUT_MS } = {}) {
  const text = await requestText(`https://api.github.com/repos/${REPO}/releases/latest`, { timeoutMs });
  const json = JSON.parse(text);
  if (!json.tag_name) throw new Error('Latest release did not include tag_name');
  return json.tag_name;
}

function normalizeVersion(value) {
  return String(value || '').trim().replace(/^v/i, '');
}

function compareVersions(left, right) {
  const a = normalizeVersion(left).split(/[.-]/).slice(0, 3).map((part) => parseInt(part, 10) || 0);
  const b = normalizeVersion(right).split(/[.-]/).slice(0, 3).map((part) => parseInt(part, 10) || 0);
  for (let i = 0; i < 3; i++) {
    if (a[i] > b[i]) return 1;
    if (a[i] < b[i]) return -1;
  }
  return 0;
}

function currentBinaryVersion(kind = 'ghost') {
  const binary = binaryPath(kind);
  if (!fs.existsSync(binary)) return null;
  try {
    const output = execFileSync(binary, ['--version'], {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore'],
      timeout: 5000
    });
    const match = output.match(/v?(\d+\.\d+\.\d+(?:[-+][^\s]+)?)/);
    return match ? match[1] : null;
  } catch (_) {
    return null;
  }
}

function homeBinDir() {
  const home = os.homedir();
  return home ? path.join(home, '.ghost', 'bin') : null;
}

function syncHomeBinaries() {
  const dir = homeBinDir();
  if (!dir) return;
  fs.mkdirSync(dir, { recursive: true });
  for (const kind of ['ghost', 'ghost-checkpoint']) {
    const src = binaryPath(kind);
    if (!fs.existsSync(src)) continue;
    const dst = path.join(dir, binaryName(kind));
    fs.copyFileSync(src, dst);
    fs.chmodSync(dst, 0o755);
  }
}

async function installRelease(tag, { quiet = false, timeoutMs = DEFAULT_TIMEOUT_MS, syncHome = true } = {}) {
  const platform = getPlatform();
  if (!platform) {
    throw new Error(`Unsupported platform: ${os.platform()} ${os.arch()}`);
  }

  fs.mkdirSync(BIN_DIR, { recursive: true });
  const baseUrl = `https://github.com/${REPO}/releases/download/${tag}`;

  for (const kind of ['ghost', 'ghost-checkpoint']) {
    const remote = releaseAssetName(kind, platform);
    const local = binaryName(kind);
    const dest = path.join(BIN_DIR, local);
    if (!quiet) console.log(`  downloading ${local}...`);
    await downloadFile(`${baseUrl}/${remote}`, dest, { timeoutMs });
  }

  if (syncHome) {
    try {
      syncHomeBinaries();
    } catch (err) {
      if (!quiet) console.error(`  warning: could not sync ~/.ghost/bin: ${err.message}`);
    }
  }
}

async function installLatest(options = {}) {
  const { quiet = false, timeoutMs = DEFAULT_TIMEOUT_MS } = options;
  const platform = getPlatform();
  if (!platform) {
    throw new Error(`Unsupported platform: ${os.platform()} ${os.arch()}`);
  }

  if (!quiet) console.log('  fetching latest release...');
  const tag = await fetchLatestRelease({ timeoutMs });
  if (!quiet) console.log(`  latest: ${tag}`);
  await installRelease(tag, options);
  writeUpdateCache({ checkedAt: Date.now(), latestTag: tag, installedVersion: normalizeVersion(tag) });
  return tag;
}

function updateCachePath() {
  const dir = homeBinDir();
  return dir ? path.join(dir, 'npm-auto-update.json') : null;
}

function readUpdateCache() {
  const cachePath = updateCachePath();
  if (!cachePath || !fs.existsSync(cachePath)) return {};
  try {
    return JSON.parse(fs.readFileSync(cachePath, 'utf8'));
  } catch (_) {
    return {};
  }
}

function writeUpdateCache(cache) {
  const cachePath = updateCachePath();
  if (!cachePath) return;
  try {
    fs.mkdirSync(path.dirname(cachePath), { recursive: true });
    fs.writeFileSync(cachePath, JSON.stringify(cache, null, 2));
  } catch (_) {}
}

function autoUpdateIntervalMs() {
  if (process.env.GHOST_AUTO_UPDATE_INTERVAL_MS !== undefined) {
    const parsed = Number(process.env.GHOST_AUTO_UPDATE_INTERVAL_MS);
    if (Number.isFinite(parsed) && parsed >= 0) return parsed;
  }
  return DEFAULT_CHECK_INTERVAL_MS;
}

function shouldSkipAutoUpdate(args = []) {
  if (process.env.GHOST_AUTO_UPDATE === '0' || process.env.GHOST_NO_AUTO_UPDATE === '1') return true;
  const command = args.find((arg) => !arg.startsWith('-')) || '';
  return new Set(['post-commit', 'rewrite-log', 'working-state']).has(command);
}

async function maybeAutoUpdate({ quiet = true, args = [], timeoutMs = DEFAULT_AUTO_UPDATE_TIMEOUT_MS, force = false } = {}) {
  if (shouldSkipAutoUpdate(args)) return { checked: false, skipped: true };

  const cache = readUpdateCache();
  const now = Date.now();
  const interval = autoUpdateIntervalMs();
  if (!force && cache.checkedAt && now - cache.checkedAt < interval) {
    return { checked: false, skipped: true };
  }

  try {
    const latestTag = await fetchLatestRelease({ timeoutMs });
    const current = currentBinaryVersion('ghost');
    if (!current || compareVersions(current, latestTag) < 0) {
      if (!quiet) console.log(`Ghost update available: ${current || 'missing'} -> ${latestTag}`);
      await installRelease(latestTag, { quiet, timeoutMs: DEFAULT_TIMEOUT_MS, syncHome: true });
      writeUpdateCache({ checkedAt: now, latestTag, installedVersion: normalizeVersion(latestTag) });
      return { checked: true, updated: true, latestTag, previousVersion: current };
    }

    writeUpdateCache({ checkedAt: now, latestTag, installedVersion: current });
    return { checked: true, updated: false, latestTag, currentVersion: current };
  } catch (err) {
    writeUpdateCache({ checkedAt: now, error: err.message });
    if (!quiet) console.error(`Ghost auto-update skipped: ${err.message}`);
    return { checked: true, updated: false, error: err };
  }
}

module.exports = {
  BIN_DIR,
  binaryName,
  binaryPath,
  compareVersions,
  currentBinaryVersion,
  fetchLatestRelease,
  getPlatform,
  installLatest,
  installRelease,
  maybeAutoUpdate,
  normalizeVersion,
  releaseAssetName,
  shouldSkipAutoUpdate,
  syncHomeBinaries
};
