"""Compile Bionic's unchanged ICU4X wrapper from its pinned Android crate closure."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tomllib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', required=True, type=Path)
parser.add_argument('--bionic', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--rustc', required=True, type=Path)
parser.add_argument('--cargo', required=True, type=Path)
args = parser.parse_args()
work = args.output.resolve()
work.mkdir(parents=True, exist_ok=True)
git = args.source.resolve()
pin = 'c991cad10330f37e93d82926bd6dae7f4f6647f6'
vendor = work / 'source/rust-vendor'
vendor.mkdir(parents=True, exist_ok=True)
cache = work / 'source-cache'
cache.mkdir(exist_ok=True)
pending = ['icu_casemap', 'icu_collections', 'icu_properties']
records = []
requested = {name: {'default'} for name in pending}
processed = {}
packages = {}
env = dict(os.environ, GIT_NO_LAZY_FETCH='1', PYTHONDONTWRITEBYTECODE='1')

while pending:
    name = pending.pop(0)
    features_requested = set(requested.get(name, set()))
    if processed.get(name) == features_requested:
        continue
    processed[name] = features_requested
    prefix = 'crates/' + name
    manifest = subprocess.check_output(['git', '-C', str(git), 'show', pin + ':' + prefix + '/Cargo.toml'], env=env)
    metadata = tomllib.loads(manifest.decode())
    version = metadata['package']['version']
    dest = vendor / (name + '-' + version)
    archive = cache / (name + '-' + version + '.tar.gz')
    if not (dest / '.cargo-checksum.json').exists():
        if not archive.exists():
            data = subprocess.check_output(['git', '-C', str(git), 'archive', pin + ':' + prefix], env=env)
            if len(data) > 32 * 1024 * 1024:
                raise RuntimeError('Single crate source bound exceeded: ' + name)
            archive.write_bytes(gzip.compress(data, mtime=0))
        with tarfile.open(archive) as stream:
            members = stream.getmembers()
            if sum(m.size for m in members) > 32 * 1024 * 1024:
                raise RuntimeError('Single crate source bound exceeded: ' + name)
            if any(m.name.startswith('/') or '..' in Path(m.name).parts for m in members):
                raise RuntimeError('Unsafe original crate archive member')
            dest.mkdir(exist_ok=True)
            stream.extractall(dest, filter='data')
        if (dest / 'Cargo.toml').read_bytes() != manifest:
            raise RuntimeError('Original crate manifest differs from pinned Git blob: ' + name)
        checksums = {str(p.relative_to(dest)): hashlib.sha256(p.read_bytes()).hexdigest() for p in dest.rglob('*') if p.is_file() and p.name != '.cargo-checksum.json'}
        (dest / '.cargo-checksum.json').write_text(json.dumps({'files': checksums, 'package': None}) + '\n')
    dependency_tables = {kind: metadata.get(kind, {}) for kind in ['dependencies', 'build-dependencies']}
    # These crates are platform-neutral ICU data/code or host proc macros. Any
    # target-specific dependency needs explicit handling instead of blind merge.
    if any(table.get('dependencies') or table.get('build-dependencies') for table in metadata.get('target', {}).values()):
        raise RuntimeError('Target-specific crate dependencies require selection: ' + name)
    aliases = {alias: (value if isinstance(value, dict) else {'version': value}) for dependencies in dependency_tables.values() for alias, value in dependencies.items()}
    active_deps = {alias for alias, value in aliases.items() if not value.get('optional', False)}
    active_features = {feature for feature in features_requested if feature != 'default' or 'default' in metadata.get('features', {})}
    cross = []
    feature_queue = list(active_features)
    while feature_queue:
        feature = feature_queue.pop(0)
        for entry in metadata.get('features', {}).get(feature, []):
            if entry.startswith('dep:'):
                active_deps.add(entry[4:])
            elif '/' in entry:
                alias, child_feature = entry.split('/', 1)
                cross.append((alias.rstrip('?'), child_feature, alias.endswith('?')))
                if not alias.endswith('?'):
                    active_deps.add(alias)
            elif entry in aliases and entry not in metadata.get('features', {}):
                active_deps.add(entry)
                active_features.add(entry)
            elif entry not in active_features:
                active_features.add(entry)
                feature_queue.append(entry)
    for alias in active_deps:
        value = aliases[alias]
        child = value.get('package', alias)
        child_features = set(value.get('features', []))
        if value.get('default-features', True):
            child_features.add('default')
        child_features.update(feature for dep, feature, _ in cross if dep == alias)
        before = set(requested.get(child, set()))
        requested.setdefault(child, set()).update(child_features)
        if child not in processed or before != requested[child]:
            pending.append(child)
    packages[name] = {'metadata': metadata, 'source': dest, 'activeFeatures': active_features, 'activeDependencies': active_deps, 'aliases': aliases, 'tables': dependency_tables, 'version': version, 'manifestSha256': hashlib.sha256(manifest).hexdigest()}
    source_bytes = sum(p.stat().st_size for p in vendor.rglob('*') if p.is_file())
    if source_bytes > 64 * 1024 * 1024:
        raise RuntimeError('Private ICU source closure exceeds64MiB')
    print(json.dumps({'crate': name, 'version': version, 'sourceBytes': source_bytes}), flush=True)

# Cargo is only the compiler driver here: AOSP's selected, pinned source
# versions/features replace registry resolution. Inactive datagen/dev packages
# are absent in the original Android source pool. Preserve their original
# manifests separately; no implementation source is changed.
for name, package in packages.items():
    dest = package['source']
    original = subprocess.check_output(['git', '-C', str(git), 'show', pin + ':crates/' + name + '/Cargo.toml'], env=env)
    (dest / 'Cargo.original.toml').write_bytes(original)
    meta = package['metadata']
    text = ['[package]', 'name = ' + json.dumps(name), 'version = ' + json.dumps(package['version']), 'edition = ' + json.dumps(meta['package'].get('edition', '2015'))]
    for key in ['build', 'links']:
        if key in meta['package']:
            value = meta['package'][key]
            text.append(key + ' = ' + (str(value).lower() if isinstance(value, bool) else json.dumps(value)))
    text.append('[lib]')
    for key, value in meta.get('lib', {}).items():
        if key in ['name', 'path', 'proc-macro']:
            text.append(key + ' = ' + (str(value).lower() if isinstance(value, bool) else json.dumps(value)))
    text.append('[features]')
    text.extend(json.dumps(feature) + ' = []' for feature in sorted(package['activeFeatures']))
    for kind, dependencies in package['tables'].items():
        for alias in dependencies:
            if alias not in package['activeDependencies']:
                continue
            value = package['aliases'][alias]
            child = value.get('package', alias)
            text.extend(['[' + kind + '.' + json.dumps(alias) + ']', 'package = ' + json.dumps(child), 'path = ' + json.dumps(str(packages[child]['source'])), 'default-features = false', 'features = ' + json.dumps(sorted(packages[child]['activeFeatures']))])
    (dest / 'Cargo.toml').write_text('\n'.join(text) + '\n')
    records.append({'name': name, 'version': package['version'], 'source': str(dest), 'originalManifestSha256': package['manifestSha256'], 'buildManifestSha256': hashlib.sha256((dest / 'Cargo.toml').read_bytes()).hexdigest(), 'features': sorted(package['activeFeatures'])})

component = work / 'source/icu-bionic'
component.mkdir(exist_ok=True)
source = args.bionic.resolve() / 'libc/bionic/icu4x.rs'
manifest = f'''[package]
name = "maclauncher-original-bionic-icu"
version = "0.0.0"
edition = "2021"
[lib]
name = "icu4x_bionic"
crate-type = ["staticlib"]
path = {json.dumps(str(source))}
[profile.release]
opt-level = 2
debug = 0
panic = "abort"
codegen-units = 1
'''
for name in ['icu_casemap', 'icu_collections', 'icu_properties']:
    manifest += f'\n[dependencies.{name}]\npath = {json.dumps(str(packages[name]["source"]))}\ndefault-features = false\nfeatures = {json.dumps(sorted(packages[name]["activeFeatures"]))}\n'
(component / 'Cargo.toml').write_text(manifest)
home = work / 'cargo-home'
home.mkdir(exist_ok=True)
(home / 'config.toml').write_text('''[net]
offline = true
''')
target = work / 'build/icu-target'
rustc = args.rustc.resolve()
cargo = args.cargo.resolve()
build_env = dict(os.environ, CARGO_HOME=str(home), CARGO_TARGET_DIR=str(target), CARGO_NET_OFFLINE='true', RUSTC=str(rustc), CARGO_ENCODED_RUSTFLAGS='\x1f'.join(['-C', 'relocation-model=pic', '--remap-path-prefix='+str(work)+'=/build/icu', '--remap-path-prefix='+str(args.bionic.resolve())+'=/src/bionic']))
subprocess.run([str(cargo), 'build', '--manifest-path', str(component / 'Cargo.toml'), '--offline', '--release', '--target', 'aarch64-linux-android', '-j4'], check=True, env=build_env)
library = target / 'aarch64-linux-android/release/libicu4x_bionic.a'
record = {'repository': 'https://android.googlesource.com/platform/external/rust/android-crates-io', 'revision': pin, 'wrapper': {'path': str(source), 'sha256': hashlib.sha256(source.read_bytes()).hexdigest()}, 'crates': records, 'library': {'path': str(library), 'sha256': hashlib.sha256(library.read_bytes()).hexdigest(), 'size': library.stat().st_size}, 'targetDirectoryShared': False}
(work / 'icu-build-result.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record['library']))
