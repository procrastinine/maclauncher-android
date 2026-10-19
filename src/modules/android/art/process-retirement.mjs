const retirements = new WeakMap();

export const processRunning = child => child && child.pid && child.exitCode == null && child.signalCode == null;

function waitForExit(child, milliseconds) {
  if (!processRunning(child)) return Promise.resolve(true);
  return new Promise(resolve => {
    let timer;
    const done = () => {
      clearTimeout(timer);
      child.removeListener('exit', done);
      child.removeListener('close', done);
      resolve(!processRunning(child));
    };
    child.once('exit', done);
    child.once('close', done);
    timer = setTimeout(done, milliseconds);
    if (!processRunning(child)) done();
  });
}

function signalProcess(child, signal) {
  if (!processRunning(child)) return;
  try { process.kill(-child.pid, signal); }
  catch { if (processRunning(child)) child.kill(signal); }
}

/** Runtime files and descriptors outlive every process that can write them.
 * A successful kill request alone does not establish process retirement. */
export function stopProcess(child, { graceMs = 2000, killMs = 2000 } = {}) {
  if (!processRunning(child)) return Promise.resolve();
  const existing = retirements.get(child);
  if (existing) return existing;
  const retired = (async () => {
    signalProcess(child, 'SIGTERM');
    if (await waitForExit(child, graceMs)) return;
    signalProcess(child, 'SIGKILL');
    if (!await waitForExit(child, killMs)) throw Error('Android process did not exit.');
  })();
  retirements.set(child, retired);
  return retired;
}
