const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawnSync} = require('node:child_process');

test('JVM terminal exit preserves shutdown hooks, VM death, status and Invocation API callbacks', async t => {
  if (process.platform !== 'darwin' || process.arch !== 'arm64') return t.skip('Requires macOS arm64');
  const found = process.env.ANDROID_JDK ? {status: 0, stdout: process.env.ANDROID_JDK} :
    spawnSync('/usr/libexec/java_home', ['-v', '21'], {encoding: 'utf8'});
  assert.equal(found.status, 0, found.stderr);
  const jdk = process.env.ANDROID_JDK || found.stdout.trim(), directory = fs.mkdtempSync(path.join(os.tmpdir(), 'android-jvm-process-'));
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}));
  const native = path.resolve(process.env.ANDROID_NATIVE_SOURCE || path.join(__dirname, '../src/modules/android/native'), 'jvm-process.c');
  const fixtures = path.join(__dirname, 'fixtures/android-jvm-process');
  const helper = process.env.ANDROID_JVM_PROCESS_PRODUCT || path.join(directory, 'process.dylib'), agent = path.join(directory, 'agent.dylib');
  const probe = path.join(directory, 'probe');
  const flags = ['-std=c11', '-arch', 'arm64', '-mmacosx-version-min=13.0', '-O2', '-Wall', '-Wextra', '-Werror',
    '-I' + path.join(jdk, 'include'), '-I' + path.join(jdk, 'include/darwin')];
  function run(command, args, options = {}) {
    const result = spawnSync(command, args, {encoding: 'utf8', timeout: 20000, ...options});
    assert.equal(result.error, undefined, result.error?.message);
    assert.equal(result.signal, null, result.stderr);
    return result;
  }
  const libraries = [[path.join(fixtures, 'probe.c'), agent, ['-DPROCESS_AGENT']]];
  if (!process.env.ANDROID_JVM_PROCESS_PRODUCT) libraries.unshift([native, helper, ['-Wl,-U,_JNI_CreateJavaVM']]);
  for (const [source, output, extra] of libraries) {
    const result = run('clang', [...flags, '-dynamiclib', ...extra, source, '-o', output]);
    assert.equal(result.status, 0, result.stderr);
  }
  let result = run('clang', [...flags, path.join(fixtures, 'probe.c'), '-o', probe]);
  assert.equal(result.status, 0, result.stderr);
  result = run(path.join(jdk, 'bin/javac'), ['-d', directory, path.join(fixtures, 'ExitProbe.java')]);
  assert.equal(result.status, 0, result.stderr);
  const env = {...process.env, DYLD_INSERT_LIBRARIES: path.join(jdk, 'lib/server/libjvm.dylib') + path.delimiter + helper};
  const javaArgs = ['-agentpath:' + agent, '-cp', directory, 'ExitProbe'];
  const baseline = run(path.join(jdk, 'bin/java'), javaArgs);
  assert.equal(baseline.status, 37, baseline.stderr);
  assert.match(baseline.stdout, /JAVA_HOOK\nVM_DEATH\nHOST_FINALIZER\n/);
  const adapted = run(path.join(jdk, 'bin/java'), javaArgs, {env});
  assert.equal(adapted.status, 37, adapted.stderr);
  assert.equal(adapted.stdout, 'JAVA_HOOK\nVM_DEATH\n');
  const embedded = run(probe, [path.join(jdk, 'lib/server/libjvm.dylib'), '-Djava.class.path=' + directory, '-agentpath:' + agent], {env});
  assert.equal(embedded.status, 37, embedded.stderr);
  assert.equal(embedded.stdout, 'JAVA_HOOK\nVM_DEATH\nEXISTING_EXIT 37\n');
  const nativeExit = run(probe, ['native'], {env});
  assert.equal(nativeExit.status, 23, nativeExit.stderr);
  assert.equal(nativeExit.stdout, 'HOST_FINALIZER\n');
  const missingVm = run(probe, ['native'], {env: {...process.env, DYLD_INSERT_LIBRARIES: helper}});
  assert.equal(missingVm.status, 78, missingVm.stderr);
  assert.match(missingVm.stderr, /Android Java VM process initialization failed/);
  assert.equal(missingVm.stdout, '');
  const previousProduct = process.env.ANDROID_JVM_PROCESS_BASELINE;
  if (previousProduct) {
    const previousEnv = {...process.env, DYLD_INSERT_LIBRARIES: path.join(jdk, 'lib/server/libjvm.dylib') + path.delimiter + previousProduct};
    const previous = run(path.join(jdk, 'bin/java'), javaArgs, {env: previousEnv});
    assert.equal(previous.status, 37, previous.stderr);assert.equal(previous.stdout, 'JAVA_HOOK\nVM_DEATH\n');
    const previousEmbedded = run(probe, [path.join(jdk, 'lib/server/libjvm.dylib'), '-Djava.class.path=' + directory, '-agentpath:' + agent], {env: previousEnv});
    assert.equal(previousEmbedded.status, 37, previousEmbedded.stderr);assert.equal(previousEmbedded.stdout, 'JAVA_HOOK\nVM_DEATH\nEXISTING_EXIT 37\n');
  }
});
