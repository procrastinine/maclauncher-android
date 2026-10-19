import {includeDependencies} from '../upstream/source-includes.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {prepareSourceTree,copyChanged,writeChanged} from '../upstream/source-files.mjs';
import {configureCmake} from '../upstream/native-configure.mjs';

const root=import.meta.dirname;
const replace=(text,from,to)=>{if(text.split(from).length!==2)throw Error('Pinned descriptor supplier boundary changed: '+from);return text.replace(from,to);};
function adapt(source){
  let header=path.join(source,'src/epoll_shim_ctx.h'),declarations=fs.readFileSync(header,'utf8');
  declarations=replace(declarations,'struct file_description_vtable {','struct file_description_vtable {\n\tuint32_t kind;\n\tbool native_vectors;\n\tsize_t (*metadata_fun)(FileDescription *, int[4]);\n\tsize_t vector_read_limit;\n\tsize_t vector_write_size;');
  declarations=replace(declarations,'\tunion {','\tunion {\n\t\tvoid *native_context;');
  fs.writeFileSync(header,declarations);
  let file=path.join(source,'src/epoll_shim_ctx.c'),text=fs.readFileSync(file,'utf8');
  text=includeDependencies(text,'#include <sys/resource.h>');
  text=replace(text,'\t\tfun(desc, (int)i, arg);',
    '\t\tbool seen=false;for(unsigned j=0;j<i;j++)if(epoll_shim_ctx->open_files[j]==desc){seen=true;break;}\n\t\tif(!seen)fun(desc, (int)i, arg);');
  text=replace(text,'epoll_shim_ctx_remove_desc(EpollShimCtx *epoll_shim_ctx, int fd)',
    'epoll_shim_ctx_remove_desc(EpollShimCtx *epoll_shim_ctx, int fd, int (*release)(void *), void *context)');
  text=replace(text,'errno_t ec_local = real_close(fd) < 0 ? errno : 0;',
    'errno_t ec_local = (release ? release(context) : real_close(fd)) < 0 ? errno : 0;');
  text=replace(text,'epoll_shim_ctx_remove_desc(epoll_shim_ctx, fd);','epoll_shim_ctx_remove_desc(epoll_shim_ctx, fd, NULL, NULL);');
  text+='\n#include '+JSON.stringify(path.join(root,'bridge.h'))+'\n#include '+JSON.stringify(path.join(root,'../directory-cursor.c'))+'\n';
  text+='\n'+fs.readFileSync(path.join(root,'descriptor.inc'),'utf8');
  text+='\n'+fs.readFileSync(path.join(root,'directory-description.inc'),'utf8');
  text+='\n'+fs.readFileSync(path.join(root,'readonly-description.inc'),'utf8');
  text=replace(text,'\terrno_t ec;\n\tint result = 0;',
    '\tif (desc->vtable->native_vectors) {\n\t\tint arg=0; if(cmd==F_SETFL){va_start(ap,cmd); arg=va_arg(ap,int); va_end(ap); }\n\t\tint value=real_fcntl(fd,cmd,arg),error=errno; file_description_unref(&desc); errno=error; return value;\n\t}\n\terrno_t ec;\n\tint result = 0;');
  fs.writeFileSync(file,text);
  file=path.join(source,'src/eventfd.c');text=fs.readFileSync(file,'utf8');
  text=text.replace('if (nbytes != sizeof(uint64_t)) {','if (nbytes < sizeof(uint64_t)) {');
  text=replace(text,'static struct file_description_vtable const eventfd_vtable = {','static struct file_description_vtable const eventfd_vtable = {\n\t.vector_read_limit = sizeof(uint64_t),\n\t.vector_write_size = sizeof(uint64_t),');
  fs.writeFileSync(file,text);
  file=path.join(source,'src/eventfd_ctx.c');text=fs.readFileSync(file,'utf8');
  text=replace(text,'\tec = kqueue_event_trigger(&eventfd->kqueue_event_, kq);','\tif (new_value == 0) return 0;\n\tec = kqueue_event_trigger(&eventfd->kqueue_event_, kq);');
  fs.writeFileSync(file,text);
  for(const name of ['timerfd.c','timerfd_ctx.c']){
    file=path.join(source,'src',name);text=fs.readFileSync(file,'utf8');
    text=text.replace('clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME','clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME && clockid != CLOCK_UPTIME_RAW && clockid != CLOCK_MONOTONIC_RAW');
    text=text.replace('clockid == CLOCK_MONOTONIC || clockid == CLOCK_REALTIME','clockid == CLOCK_MONOTONIC || clockid == CLOCK_REALTIME || clockid == CLOCK_UPTIME_RAW || clockid == CLOCK_MONOTONIC_RAW');
    if(name==='timerfd.c')text=replace(text,'static struct file_description_vtable const timerfd_vtable = {','static struct file_description_vtable const timerfd_vtable = {\n\t.vector_read_limit = sizeof(uint64_t),');
    fs.writeFileSync(file,text);
  }
  file=path.join(source,'src/signalfd.c');text=fs.readFileSync(file,'utf8');
  text=replace(text,'static struct file_description_vtable const signalfd_vtable = {','static struct file_description_vtable const signalfd_vtable = {\n\t.vector_read_limit = 256 * sizeof(struct signalfd_siginfo),');
  const conversion='\t\tint signal=siginfo.ssi_signo;\n\t\tsiginfo.ssi_signo=klj_signal_to_guest(signal);\n\t\tsiginfo.ssi_errno=kl_errno_to_linux(siginfo.ssi_errno);\n\t\tsiginfo.ssi_uid=klj_process_uid_from_host(siginfo.ssi_uid);\n\t\tif(signal==SIGCHLD&&(siginfo.ssi_code==CLD_KILLED||siginfo.ssi_code==CLD_DUMPED||siginfo.ssi_code==CLD_STOPPED||siginfo.ssi_code==CLD_TRAPPED))siginfo.ssi_status=klj_signal_to_guest(siginfo.ssi_status);\n';
  text=replace(text,'\t\tmemcpy(buf, &siginfo, sizeof(siginfo));',conversion+'\t\tmemcpy(buf, &siginfo, sizeof(siginfo));');
  text=includeDependencies(text,'#include '+JSON.stringify(path.join(root,'../signal-bridge.h'))+'\n#include '+JSON.stringify(path.join(root,'../process-identity.h'))+'\nextern int kl_errno_to_linux(int);');fs.writeFileSync(file,text);
}
export function buildPollableDescriptors({checkout,output,ninja=process.env.NINJA||'ninja',jobs=4}) {
  if(!Number.isInteger(jobs)||jobs<1)throw Error('Descriptor build jobs must be a positive integer');
  const revision=JSON.parse(fs.readFileSync(path.join(root,'../source-pins.json'))).epollShim;
  const source=path.join(output,'source'),build=path.join(output,'build');
  const paths=execFileSync('git',['-C',checkout,'ls-tree','-r','--name-only','-z',revision],{encoding:'utf8'}).split('\0').filter(Boolean);
  const object=path.resolve(output,'bridge.o');
  const quote=value=>'[==['+path.resolve(value)+']==]';
  prepareSourceTree(source,temporary=>{
    exportSources(checkout,revision,['.'],temporary);adapt(temporary);
    const cmake=path.join(temporary,'CMakeLists.txt');
    fs.appendFileSync(cmake,'\n# Shared native descriptor operations use the original build dependency graph.\n'+
      'add_library(maclauncher-descriptor-bridge OBJECT '+quote(path.join(root,'bridge.c'))+')\n'+
      'target_compile_options(maclauncher-descriptor-bridge PRIVATE -O2 -UNDEBUG -Wall -Wextra -Werror)\n'+
      'target_include_directories(maclauncher-descriptor-bridge PRIVATE '+quote(path.join(build,'install-include'))+')\n'+
      'add_custom_command(OUTPUT '+quote(object)+' COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_OBJECTS:maclauncher-descriptor-bridge> '+quote(object)+
        ' DEPENDS maclauncher-descriptor-bridge $<TARGET_OBJECTS:maclauncher-descriptor-bridge> COMMAND_EXPAND_LISTS VERBATIM)\n'+
      'add_custom_target(maclauncher-descriptors DEPENDS epoll-shim '+quote(object)+')\n');
  });
  configureCmake({source,build,ninja,args:['-DBUILD_TESTING=OFF','-DBUILD_SHARED_LIBS=OFF','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_OSX_ARCHITECTURES=arm64','-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0']});
  execFileSync('cmake',['--build',build,'--target','maclauncher-descriptors','--parallel',String(jobs)],{stdio:'inherit'});
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});
  for(const file of paths.filter(file=>/(^|\/)(LICENSE[^/]*|COPYING[^/]*)$/i.test(file))){const target=path.join(notices,file);fs.mkdirSync(path.dirname(target),{recursive:true});copyChanged(path.join(source,file),target);}
  writeChanged(path.join(notices,'source.json'),JSON.stringify({project:'epoll-shim',url:'https://github.com/jiixyj/epoll-shim',revision},null,2)+'\n');
  return {object,archive:path.join(build,'src/libepoll-shim.a'),include:path.join(build,'install-include'),notices,source,build};
}
