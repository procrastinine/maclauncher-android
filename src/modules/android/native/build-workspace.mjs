import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {constants} from 'node:os';

/* Python remains the lock owner while the builder and its compiler process
 * group run. Parent death and builder exit are kernel events, not PID-file
 * guesses. After cancellation, the lock stays held through descendant exit. */
const supervisor=String.raw`import errno,fcntl,os,select,shutil,signal,stat,sys,time
parent=os.getppid()
queue=select.kqueue()
cancelled=0
def cancel(signum,frame):
    global cancelled
    if not cancelled: cancelled=signum
for signum in (signal.SIGINT,signal.SIGTERM,signal.SIGHUP): signal.signal(signum,cancel)
def observe(pid):
    queue.control([select.kevent(pid,filter=select.KQ_FILTER_PROC,flags=select.KQ_EV_ADD|select.KQ_EV_ONESHOT,fflags=select.KQ_NOTE_EXIT)],0,0)
try: observe(parent)
except ProcessLookupError: cancelled=signal.SIGTERM
if os.getppid()!=parent: cancelled=signal.SIGTERM
def events(timeout):
    global cancelled
    for event in queue.control(None,4,timeout):
        if event.ident==parent and event.fflags&select.KQ_NOTE_EXIT: cancelled=cancelled or signal.SIGTERM
def finish(status):
    queue.close()
    if status<0:
        signum=-status
        if signum not in (signal.SIGKILL,signal.SIGSTOP): signal.signal(signum,signal.SIG_DFL)
        os.kill(os.getpid(),signum)
        os._exit(128+signum)
    os._exit(status)
fd=os.open(sys.argv[1],os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW,0o600)
if not stat.S_ISREG(os.fstat(fd).st_mode): raise ValueError('Build workspace lock is not a regular file')
while not cancelled:
    try:
        fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        break
    except BlockingIOError: events(0.1)
if cancelled: finish(-cancelled)
# The supervisor owns only the output it creates exclusively for this build.
# Retaining the directory descriptor prevents inode reuse during cleanup.
output=sys.argv[2]
output_fd=None
if output:
    os.makedirs(os.path.dirname(output),exist_ok=True)
    os.mkdir(output)
    output_fd=os.open(output,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
def release_output(completed):
    if output_fd is None: return
    try:
        if not completed:
            owned=os.fstat(output_fd)
            try: current=os.lstat(output)
            except FileNotFoundError: current=None
            if current is not None and stat.S_ISDIR(current.st_mode) and (current.st_dev,current.st_ino)==(owned.st_dev,owned.st_ino):
                shutil.rmtree(output)
    except OSError as error:
        print('Unable to remove incomplete Android build output: '+str(error),file=sys.stderr)
    finally: os.close(output_fd)
argv=sys.argv[3:]+['--workspace-fd',str(fd)]
if output_fd is not None: argv+=['--output-fd',str(output_fd)]
try: child=os.fork()
except BaseException:
    release_output(False)
    raise
if not child:
    try:
        os.setpgid(0,0)
        for signum in (signal.SIGINT,signal.SIGTERM,signal.SIGHUP): signal.signal(signum,signal.SIG_DFL)
        queue.close()
        os.set_inheritable(fd,True)
        if output_fd is not None: os.set_inheritable(output_fd,True)
        os.execv(argv[0],argv)
    except BaseException:
        import traceback
        traceback.print_exc()
        os._exit(127)
status=None
def group_signal(signum):
    try: os.killpg(child,signum); return True
    except ProcessLookupError: return False
try:
    try: os.setpgid(child,child)
    except OSError as error:
        if error.errno not in (errno.EACCES,errno.ESRCH): raise
    try: observe(child)
    except ProcessLookupError: pass
    while status is None and not cancelled:
        waited,value=os.waitpid(child,os.WNOHANG)
        if waited: status=value
        else: events(1)
finally:
    # Reap the leader and retire compiler children even if observing the build
    # fails. A live group retains its pgid; stop using it when the group is gone.
    if not group_signal(signal.SIGTERM) and status is None:
        try: os.kill(child,signal.SIGTERM)
        except ProcessLookupError: pass
    deadline=time.monotonic()+2
    while True:
        if status is None:
            waited,value=os.waitpid(child,os.WNOHANG)
            if waited: status=value
        if not group_signal(0): break
        if time.monotonic()>=deadline: group_signal(signal.SIGKILL)
        time.sleep(0.02)
    if status is None: _,status=os.waitpid(child,0)
    release_output(os.WIFEXITED(status) and os.WEXITSTATUS(status)==0)
# Keep fd open until all owned descendants have left the workspace.
if cancelled: finish(-cancelled)
finish(-os.WTERMSIG(status) if os.WIFSIGNALED(status) else os.WEXITSTATUS(status))
`;

/** Compiler work has one stable, kernel-locked path; each completed runtime
 * has its own output. The builder also inherits the same locked descriptor. */
export function enterBuildWorkspace({directory,output,python,entry,args,fd,outputFd,manageOutput=false}){
  directory=path.resolve(directory);output=path.resolve(output);
  if(output===directory||output.startsWith(directory+path.sep))throw Error('Android runtime output must be outside its compiler workspace');
  if(manageOutput&&directory.startsWith(output+path.sep))throw Error('Android compiler workspace must be outside its runtime output');
  fs.mkdirSync(directory,{recursive:true,mode:0o700});
  const lock=path.join(directory,'build.lock');
  if(fd!==undefined){
    const actual=fs.fstatSync(fd),expected=fs.statSync(lock);
    if(!actual.isFile()||actual.dev!==expected.dev||actual.ino!==expected.ino)throw Error('Android build workspace lock is invalid');
    if(manageOutput){
      if(!Number.isInteger(outputFd)||outputFd<0)throw Error('Android build output ownership is missing');
      const owned=fs.fstatSync(outputFd),current=fs.lstatSync(output);
      if(!owned.isDirectory()||!current.isDirectory()||owned.dev!==current.dev||owned.ino!==current.ino)throw Error('Android build output ownership is invalid');
    }
    return true;
  }
  try{execFileSync(python,['-c',supervisor,lock,manageOutput?output:'',process.execPath,entry,...args],{stdio:'inherit'});}
  catch(error){
    if(error.signal){
      process.removeAllListeners(error.signal);
      process.kill(process.pid,error.signal);
      // Default termination normally happens in kill(). Keep the original
      // signal status even if a host installs a deferred signal handler.
      Atomics.wait(new Int32Array(new SharedArrayBuffer(4)),0,0,1000);
      process.exit(128+(constants.signals[error.signal]||0));
    }
    if(Number.isInteger(error.status))process.exit(error.status);
    throw error;
  }
  return false;
}
