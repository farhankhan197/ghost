const https = require('https');
const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');

const REPO = 'farhankhan197/ghost';
const BIN_DIR = path.join(__dirname, '..', 'bin');

function getPlatform() {
  const platform = os.platform();
  const arch = os.arch();

  let osName;
  if (platform === 'win32') osName = 'windows';
  else if (platform === 'darwin') osName = 'macos';
  else if (platform === 'linux') osName = 'linux';
  else return null;

  let archName;
  if (arch === 'x64') archName = 'x86_64';
  else if (arch === 'arm64') archName = 'arm64';
  else return null;

  return { osName, archName };
}

function fetchLatestRelease() {
  return new Promise((resolve, reject) => {
    const url = `https://api.github.com/repos/${REPO}/releases/latest`;
    const client = url.startsWith('https') ? https : http;

    client.get(url, { headers: { 'User-Agent': 'ghost-installer' } }, (res) => {
      if (res.statusCode === 302 || res.statusCode === 301) {
        return fetchUrl(res.headers.location).then(resolve).catch(reject);
      }

      let data = '';
      res.on('data', (chunk) => data += chunk);
      res.on('end', () => {
        try {
          const json = JSON.parse(data);
          resolve(json.tag_name);
        } catch (e) {
          reject(new Error('Failed to parse release info'));
        }
      });
    }).on('error', reject);
  });
}

function fetchUrl(url) {
  return new Promise((resolve, reject) => {
    const client = url.startsWith('https') ? https : http;
    client.get(url, { headers: { 'User-Agent': 'ghost-installer' } }, (res) => {
      if (res.statusCode === 302 || res.statusCode === 301) {
        return fetchUrl(res.headers.location).then(resolve).catch(reject);
      }
      let data = '';
      res.on('data', (chunk) => data += chunk);
      res.on('end', () => resolve(data));
    }).on('error', reject);
  });
}

function downloadFile(url, dest) {
  return new Promise((resolve, reject) => {
    const client = url.startsWith('https') ? https : http;
    client.get(url, { headers: { 'User-Agent': 'ghost-installer' } }, (res) => {
      if (res.statusCode === 302 || res.statusCode === 301) {
        return downloadFile(res.headers.location, dest).then(resolve).catch(reject);
      }
      if (res.statusCode !== 200) {
        return reject(new Error(`HTTP ${res.statusCode}: ${url}`));
      }
      const file = fs.createWriteStream(dest);
      res.pipe(file);
      file.on('finish', () => {
        file.close();
        fs.chmodSync(dest, 0o755);
        resolve();
      });
    }).on('error', reject);
  });
}

async function main() {
  const platform = getPlatform();
  if (!platform) {
    console.error('Unsupported platform:', os.platform(), os.arch());
    process.exit(1);
  }

  console.log('▖ installing ghost binaries...');

  // Create bin directory
  if (!fs.existsSync(BIN_DIR)) {
    fs.mkdirSync(BIN_DIR, { recursive: true });
  }

  // Get latest release
  let tag;
  try {
    tag = await fetchLatestRelease();
  } catch (e) {
    console.error('Failed to fetch latest release:', e.message);
    process.exit(1);
  }

  const baseUrl = `https://github.com/${REPO}/releases/download/${tag}`;
  const ext = platform.osName === 'windows' ? '.exe' : '';

  const files = [
    {
      remote: `ghost-${platform.osName}-${platform.archName}${ext}`,
      local: `ghost${ext}`
    },
    {
      remote: `ghost-checkpoint-${platform.osName}-${platform.archName}${ext}`,
      local: `ghost-checkpoint${ext}`
    }
  ];

  for (const file of files) {
    const url = `${baseUrl}/${file.remote}`;
    const dest = path.join(BIN_DIR, file.local);

    console.log(`  downloading ${file.local}...`);
    try {
      await downloadFile(url, dest);
    } catch (e) {
      console.error(`Failed to download ${file.local}:`, e.message);
      process.exit(1);
    }
  }

  console.log(`  installed: ${tag}`);
  console.log('  run "ghost install" in your repo to set up tracking');
}

main();
