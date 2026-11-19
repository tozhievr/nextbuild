import { readFileSync, writeFileSync } from 'node:fs';

const versionFile = new URL('../VERSION', import.meta.url);
const packageFile = new URL('../extension/package.json', import.meta.url);
const lockFile = new URL('../extension/package-lock.json', import.meta.url);
const parts = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/;

function readVersion() {
    const version = readFileSync(versionFile, 'utf8').trim();
    if (!parts.test(version)) throw new Error('VERSION must contain a MAJOR.MINOR.PATCH version');
    return version;
}

function readJson(file) {
    return JSON.parse(readFileSync(file, 'utf8'));
}

function writeJson(file, value) {
    writeFileSync(file, JSON.stringify(value, null, 2) + '\n');
}

function verify(version, manifest, lock) {
    if (manifest.version !== version || lock.version !== version || lock.packages[''].version !== version) {
        throw new Error('VERSION and the extension metadata disagree');
    }
}

function main() {
    const [action = 'read', increment] = process.argv.slice(2);
    const version = readVersion();
    if (action === 'read') {
        console.log(version);
        return;
    }
    const manifest = readJson(packageFile);
    const lock = readJson(lockFile);
    verify(version, manifest, lock);
    if (action === 'verify') {
        console.log(version);
        return;
    }
    const position = ['major', 'minor', 'patch'].indexOf(increment);
    if (action !== 'bump' || position === -1) {
        throw new Error('usage: node tools/version.mjs read|verify|bump [major|minor|patch]');
    }
    const next = version.split('.').map((part, index) => {
        if (index < position) return part;
        return index === position ? String(BigInt(part) + 1n) : '0';
    }).join('.');
    manifest.version = next;
    lock.version = next;
    lock.packages[''].version = next;
    writeJson(packageFile, manifest);
    writeJson(lockFile, lock);
    writeFileSync(versionFile, next + '\n');
    console.log(next);
}

try {
    main();
} catch (error) {
    console.error(error.message);
    process.exitCode = 1;
}
