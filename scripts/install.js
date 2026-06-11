const { installLatest } = require('./binary-manager');

async function main() {
  console.log('installing Ghost binaries...');
  try {
    const tag = await installLatest({ quiet: false });
    console.log(`  installed: ${tag}`);
    console.log('  run "ghost init" in your repo to set up tracking');
  } catch (err) {
    console.error('Failed to install Ghost binaries:', err.message);
    process.exit(1);
  }
}

main();
