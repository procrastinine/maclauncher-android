const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),os=require('node:os');
const {execFileSync,spawnSync}=require('node:child_process');
test('WebKit rule descriptor diagnostics preserve IO and omit unrelated paths and bytes',t=>{
 if(process.platform!=='darwin')return t.skip('Requires Darwin interposition');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-webkit-rule-io-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const source=path.join(root,'probe.c'),program=path.join(root,'probe'),library=path.join(root,'trace.dylib');
 fs.writeFileSync(source,'#include <fcntl.h>\n#include <unistd.h>\n#include <sys/uio.h>\n#include <errno.h>\nint main(int n,char**v){if(n!=2)return 9;int fd=open(v[1],O_CREAT|O_TRUNC|O_RDWR,0600);if(fd<0)return 10;errno=EDOM;if(write(fd,"hidden-content",14)!=14||errno!=EDOM)return 11;struct iovec i={"x",1};if(writev(fd,&i,1)!=1||errno!=EDOM)return 12;if(pwrite(fd,"y",1,0)!=1||errno!=EDOM)return 13;if(lseek(fd,0,SEEK_SET)!=0||errno!=EDOM)return 14;return close(fd);}\n');
 execFileSync('clang',['-dynamiclib','-arch','arm64','-mmacosx-version-min=13.0','-O2','-Wall','-Wextra','-Werror','scripts/android-test/native-exit.c','-o',library],{stdio:'pipe'});
 execFileSync('clang',['-arch','arm64','-mmacosx-version-min=13.0','-O0','-Wall','-Wextra','-Werror',source,'-o',program],{stdio:'pipe'});
 for(const enabled of [false,true])for(const selected of [false,true]){
  const file=path.join(root,selected?'ContentRuleList-fixture':'unrelated');
  const r=spawnSync(program,[file],{encoding:'utf8',env:{...process.env,DYLD_INSERT_LIBRARIES:library,MACLAUNCHER_TEST_NATIVE_WEBKIT_IO_TRACE:enabled?'1':'0',MACLAUNCHER_TEST_NATIVE_EXIT_TRACE:'0',MACLAUNCHER_TEST_NATIVE_IO_TRACE:'0'}});
  assert.equal(r.status,0,JSON.stringify({enabled,selected,signal:r.signal,error:r.error?.message,stderr:r.stderr}));assert.equal(r.signal,null);assert.doesNotMatch(r.stderr,/hidden-content|unrelated/);
  assert.equal(r.stderr.includes('op=write '),enabled&&selected,r.stderr);
  if(enabled&&selected)for(const operation of ['write','writev','pwrite','lseek','close'])assert.match(r.stderr,new RegExp('op='+operation+' .*result=[0-9]+ errno=0'));
 }
});
test('WebKit automaton diagnostics retain creation and mapping results without file contents',t=>{
 if(process.platform!=='darwin')return t.skip('Requires Darwin interposition');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-webkit-nfa-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const source=path.join(root,'probe.c'),program=path.join(root,'probe'),library=path.join(root,'trace.dylib');
 fs.writeFileSync(source,'#include <stdlib.h>\n#include <unistd.h>\n#include <fcntl.h>\n#include <sys/mman.h>\n#include <errno.h>\n#include <stdio.h>\nint main(int n,char**v){if(n!=2)return 9;char temp[1024],directory[1024];if(!confstr(_CS_DARWIN_USER_TEMP_DIR,directory,sizeof directory))return 10;snprintf(temp,sizeof temp,"%s/SerializedNFA-XXXXXX",v[1]);errno=EDOM;int fd=mkostemps(temp,0,O_CLOEXEC);if(fd<0||errno!=EDOM)return 11;if(write(fd,"hidden-content",14)!=14||errno!=EDOM)return 12;void*p=mmap(0,14,PROT_READ,MAP_PRIVATE,fd,0);if(p==MAP_FAILED||errno!=EDOM)return 13;if(munmap(p,14)||close(fd))return 14;return unlink(temp); }\n');
 execFileSync('clang',['-dynamiclib','-arch','arm64','-mmacosx-version-min=13.0','-O2','-Wall','-Wextra','-Werror','scripts/android-test/native-exit.c','-o',library],{stdio:'pipe'});
 execFileSync('clang',['-arch','arm64','-mmacosx-version-min=13.0','-Wall','-Wextra','-Werror',source,'-o',program],{stdio:'pipe'});
 for(const enabled of [false,true]){
  const r=spawnSync(program,[root],{encoding:'utf8',env:{...process.env,DYLD_INSERT_LIBRARIES:library,MACLAUNCHER_TEST_NATIVE_WEBKIT_IO_TRACE:enabled?'1':'0',MACLAUNCHER_TEST_NATIVE_EXIT_TRACE:'0',MACLAUNCHER_TEST_NATIVE_IO_TRACE:'0'}});
  assert.equal(r.status,0,r.stderr);assert.doesNotMatch(r.stderr,/hidden-content/);
  assert.equal(r.stderr.includes('op=mkostemps-nfa '),enabled,r.stderr);
  if(enabled){assert.match(r.stderr,/op=confstr-temp/);assert.match(r.stderr,/op=mmap .*result=0 errno=0/);assert.match(r.stderr,/op=write .*result=14 errno=0/);}
 }
});
test('Native termination diagnostics retain exit status and original shutdown semantics',t=>{
 if(process.platform!=='darwin')return t.skip('Requires Darwin interposition');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-native-exit-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const source=path.join(root,'probe.c'),program=path.join(root,'probe'),library=path.join(root,'trace.dylib');
 fs.writeFileSync(source,'#include <stdio.h>\n#include <stdlib.h>\n#include <unistd.h>\n#include <signal.h>\n#include <errno.h>\nstatic void retired(void){fputs("original atexit\\n",stderr);}\nstatic void aborted(int sig){(void)sig;if(errno==EDOM)(void)write(1,"original abort handler\\n",23);}\n__attribute__((noinline)) static void caller(int mode){if(mode==1)exit(23);if(mode==3){fclose(stderr);exit(25);}if(mode==4){fclose(stderr);errno=EDOM;abort();}_exit(24);}\nint main(int n,char**v){(void)v;atexit(retired);signal(SIGABRT,aborted);caller(n==2?1:n==3?3:n==4?4:2);}\n');
 execFileSync('clang',['-dynamiclib','-arch','arm64','-mmacosx-version-min=13.0','-O2','-Wall','-Wextra','-Werror','scripts/android-test/native-exit.c','-o',library],{stdio:'pipe'});
 execFileSync('clang',['-arch','arm64','-mmacosx-version-min=13.0','-O0','-fno-omit-frame-pointer','-Wall','-Wextra','-Werror',source,'-o',program],{stdio:'pipe'});
 for(const enabled of [false,true])for(const immediate of [false,true]){
  const result=spawnSync(program,immediate?[]:['exit'],{encoding:'utf8',env:{...process.env,DYLD_INSERT_LIBRARIES:library,MACLAUNCHER_TEST_NATIVE_EXIT_TRACE:enabled?'1':'0'}});
  assert.equal(result.signal,null);assert.equal(result.status,immediate?24:23,result.stderr);
  assert.equal(result.stderr.includes('original atexit'),!immediate,result.stderr);
  assert.equal(result.stderr.includes('[android-test] native termination'),enabled,result.stderr);
  if(enabled){assert.match(result.stderr,new RegExp('kind='+(immediate?'_exit':'exit')+' status='+(immediate?24:23)));assert.match(result.stderr,/native exit #\d+ pc=0x[0-9a-f]+ image=.+probe\+0x[0-9a-f]+/);}
 }
 // Guest code owns its stderr and may close it before an error. The test
 // observer retains its inherited output separately without reopening fd2.
 const closed=spawnSync(program,['close','stderr'],{encoding:'utf8',env:{...process.env,DYLD_INSERT_LIBRARIES:library,MACLAUNCHER_TEST_NATIVE_EXIT_TRACE:'1'}});
 assert.equal(closed.status,25);assert.match(closed.stderr,/kind=exit status=25/);assert.doesNotMatch(closed.stderr,/original atexit/);
 for(const enabled of [false,true]){
  const aborted=spawnSync(program,['close','stderr','abort'],{encoding:'utf8',env:{...process.env,DYLD_INSERT_LIBRARIES:library,MACLAUNCHER_TEST_NATIVE_EXIT_TRACE:enabled?'1':'0'}});
  assert.equal(aborted.status,null);assert.equal(aborted.signal,'SIGABRT');assert.equal(aborted.stdout,'original abort handler\n');assert.doesNotMatch(aborted.stderr,/original atexit/);
  assert.equal(aborted.stderr.includes('native termination'),enabled);
  if(enabled){assert.match(aborted.stderr,/kind=abort status=6/);assert.match(aborted.stderr,/native exit #\d+ pc=0x[0-9a-f]+ image=.+probe\+0x[0-9a-f]+/);}
 }
});

test('Native stdio diagnostics preserve exact IO results after stderr retirement',t=>{
 if(process.platform!=='darwin')return t.skip('Requires Darwin interposition');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-native-io-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const source=path.join(root,'probe.c'),program=path.join(root,'probe'),library=path.join(root,'trace.dylib'),input=path.join(root,'asset.apk');
 fs.writeFileSync(input,'APK-header<strings>original</strings>');
 fs.writeFileSync(source,'#include <errno.h>\n#include <stdio.h>\n#include <string.h>\nint main(int n,char**v){if(n!=2)return 9;fclose(stderr);errno=EDOM;FILE*f=fopen(v[1],"rb");if(!f||errno!=EDOM)return 10;if(fseek(f,10,SEEK_SET)||errno!=EDOM)return 11;char b[10]={0};if(fread(b,1,9,f)!=9||memcmp(b,"<strings>",9)||errno!=EDOM)return 12;if(fseeko(f,0,SEEK_END)||errno!=EDOM)return 13;if(fread(b,1,1,f)!=0||errno!=EDOM)return 14;return fclose(f);}\n');
 execFileSync('clang',['-dynamiclib','-arch','arm64','-mmacosx-version-min=13.0','-O2','-Wall','-Wextra','-Werror','scripts/android-test/native-exit.c','-o',library],{stdio:'pipe'});
 execFileSync('clang',['-arch','arm64','-mmacosx-version-min=13.0','-O0','-Wall','-Wextra','-Werror',source,'-o',program],{stdio:'pipe'});
 for(const enabled of [false,true]){
  const result=spawnSync(program,[input],{encoding:'utf8',env:{...process.env,DYLD_INSERT_LIBRARIES:library,MACLAUNCHER_TEST_NATIVE_EXIT_TRACE:'0',MACLAUNCHER_TEST_NATIVE_IO_TRACE:enabled?'1':'0'}});
  assert.equal(result.signal,null);assert.equal(result.status,0,result.stderr);
  assert.equal(result.stderr.includes('native io'),enabled,result.stderr);
  if(enabled){assert.match(result.stderr,/op=fopen .*asset\.apk.*stream=0x/);assert.match(result.stderr,/op=fseek .*offset=10 whence=0 result=0/);assert.match(result.stderr,/op=fread .*size=1 count=9 result=9/);assert.match(result.stderr,/op=fread .*size=1 count=1 result=0/);assert.match(result.stderr,/op=fseeko .*result=0/);assert.doesNotMatch(result.stderr,/<strings>/);}
 }
});
test('Native SIGABRT delivery diagnostics preserve original callbacks, masks and errno',t=>{
 if(process.platform!=='darwin')return t.skip('Requires Darwin interposition');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-native-abort-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const source=path.join(root,'probe.c'),program=path.join(root,'probe'),library=path.join(root,'trace.dylib');
 fs.writeFileSync(source,'#include <signal.h>\n#include <pthread.h>\n#include <mach/mach.h>\n#include <unistd.h>\n#include <errno.h>\n#include <stdio.h>\nextern int __pthread_kill(mach_port_t,int);\nstatic volatile sig_atomic_t calls;static void original(int s){sigset_t m;sigprocmask(0,0,&m);if(s==SIGABRT&&errno==EDOM&&sigismember(&m,SIGUSR1)&&sigismember(&m,SIGABRT))calls++;}\nint main(int n,char**v){(void)v;if(n>1){kill(getpid(),SIGABRT);return 99;}struct sigaction a={0},q;a.sa_handler=original;a.sa_flags=SA_RESTART;sigemptyset(&a.sa_mask);sigaddset(&a.sa_mask,SIGUSR1);if(sigaction(SIGABRT,&a,0))return 1;errno=EDOM;if(raise(SIGABRT)||errno!=EDOM)return 2;if(pthread_kill(pthread_self(),SIGABRT)||errno!=EDOM)return 3;if(__pthread_kill(pthread_mach_thread_np(pthread_self()),SIGABRT)||errno!=EDOM)return 4;if(kill(getpid(),SIGABRT)||errno!=EDOM)return 5;if(sigaction(SIGABRT,0,&q)||q.sa_handler!=original||!(q.sa_flags&SA_RESTART)||!sigismember(&q.sa_mask,SIGUSR1)||calls!=4)return 6;puts("PASS original SIGABRT callbacks/masks/disposition/errno");return 0;}\n');
 execFileSync('clang',['-dynamiclib','-arch','arm64','-mmacosx-version-min=13.0','-O2','-Wall','-Wextra','-Werror','scripts/android-test/native-exit.c','-o',library],{stdio:'pipe'});
 execFileSync('clang',['-arch','arm64','-mmacosx-version-min=13.0','-O0','-fno-omit-frame-pointer','-Wall','-Wextra','-Werror',source,'-o',program],{stdio:'pipe'});
 for(const enabled of [false,true]){
  const env={...process.env,DYLD_INSERT_LIBRARIES:library,MACLAUNCHER_TEST_NATIVE_EXIT_TRACE:enabled?'1':'0'};
  const result=spawnSync(program,[],{encoding:'utf8',env});assert.equal(result.status,0,result.stderr);assert.equal(result.signal,null);assert.match(result.stdout,/PASS original SIGABRT/);
  assert.equal(result.stderr.includes('native termination'),enabled);
  if(enabled)for(const kind of ['raise','pthread_kill','__pthread_kill','kill'])assert.match(result.stderr,new RegExp('kind='+kind+'-SIGABRT status=6 pid=[0-9]+ thread=[0-9]+'));
  const fatal=spawnSync(program,['default'],{encoding:'utf8',env});assert.equal(fatal.status,null);assert.equal(fatal.signal,'SIGABRT');
  if(enabled)assert.match(fatal.stderr,/kind=kill-SIGABRT/);else assert.doesNotMatch(fatal.stderr,/native termination/);
 }
});
