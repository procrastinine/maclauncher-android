import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {prepareSourceTree,writeChanged,copyChanged} from '../../native/upstream/source-files.mjs';
import {nativeRustPathEnvironment} from '../../native/upstream/native-paths.mjs';

import {generateArtHalAidl} from '../hal-aidl.mjs';

/** Adapt the pinned complete AIM ISensors service at its physical source.
 * Its original AIDL, FMQ, activation, flush and Binder implementations remain. */
export function prepareArtSensors({aim,output}){
 aim=path.resolve(aim);output=path.resolve(output);
 const lock=fs.readFileSync(path.join(aim,'Cargo.lock'));
 const previous=path.join(output,'.upstream-lock'),resolved=fs.existsSync(previous)&&fs.readFileSync(previous).equals(lock)&&fs.existsSync(path.join(output,'Cargo.lock'))?fs.readFileSync(path.join(output,'Cargo.lock')):lock;
 prepareSourceTree(output,temporary=>{
  const original=path.join(aim,'hal/sensors');
  for(const file of ['main.rs','service.rs','fmq.rs','logger.rs'])copyChanged(path.join(original,'src',file),path.join(temporary,'src',file));
  copyChanged(path.join(import.meta.dirname,'source.rs'),path.join(temporary,'src/source.rs'));
  let main=fs.readFileSync(path.join(temporary,'src/main.rs'),'utf8');
  main=main.replace('mod fmq;','mod fmq;\nmod source;').replace('use aim_hostcall::{guest, module, sensors};','');
  const begin=main.indexOf('    match guest::version('),end=main.indexOf('    binder::ProcessState::',begin);
  if(begin<0||end<0)throw Error('Original sensor HAL startup changed');
  main=main.slice(0,begin)+`    let directory=std::env::args().nth(1).expect("Sensor source directory is required");\n    let source=source::Source::open(std::path::Path::new(&directory)).expect("Shared sensor source");\n`+main.slice(end);
  main=main.replace('service::Sensors::new(present)','service::Sensors::new(source)');writeChanged(path.join(temporary,'src/main.rs'),main);
  let service=fs.readFileSync(path.join(temporary,'src/service.rs'),'utf8');
  service=service.replace('use aim_hostcall::guest;','').replace('use aim_hostcall::sensors::{Readings, present};','use crate::source::Source;');
  service=service.replace('SensorInfo::{SENSOR_FLAG_BITS_ON_CHANGE_MODE, SensorInfo}','SensorInfo::SensorInfo');
  service=service.replace('pub const MIN_PERIOD: Duration = Duration::from_millis(200);','pub const MIN_PERIOD: Duration = Duration::from_micros(16_667);');
  const a=service.indexOf('/// The sensors a host'),b=service.indexOf('fn event(',a);
  if(a<0||b<0)throw Error('Original sensor HAL source boundary changed');service=service.slice(0,a)+service.slice(b);
  service=service.replace('last: Option<f32>,','last: Option<[f32;16]>,\n    next: std::time::Instant,\n    flags:i32,');
  service=service.replace('present: u32,','source: Source,');
  service=service.replace('pub fn new(present: u32) -> Self {','pub fn new(source: Source) -> Self {').replace('let sensors = sensor_list(present)','let sensors = source.list()');
  service=service.replace('last: None,','last: None,\n                next:std::time::Instant::now(),\n                flags:info.flags,');
  service=service.replace('                present,','                source,');
  const poll=service.indexOf('        let Ok(r) = guest::sensors()'),finish=service.indexOf('        period\n',poll);
  if(poll<0||finish<0)throw Error('Original sensor HAL sample boundary changed');
  service=service.slice(0,poll)+`        let Some((timestamp,rows))=state.source.snapshot() else{return period;};
        let now=std::time::Instant::now();let mut events=Vec::new();
        for index in 0..state.sensors.len(){
            let s=&mut state.sensors[index];if !s.active||now<s.next{continue;}
            let changed=s.last!=Some(rows[index]);s.next=now+s.period;
            if s.flags&14==0||changed{let handle=s.handle;let kind=s.r#type;s.last=Some(rows[index]);
                events.push(Event{timestamp,sensorHandle:handle,sensorType:kind,payload:state.source.payload(index,rows[index])});}
        }
        if !events.is_empty(){state.send(&events);}
`+service.slice(finish);
  service=service.replace('Ok(sensor_list(self.state.lock().unwrap().present))','Ok(self.state.lock().unwrap().source.list())');
  // Upstream source tests exercise its Mac light/lid source. Adapted source
  // integration checks live in the repository tests, outside shipped code.
  service=service.split('#[cfg(test)]')[0];
  writeChanged(path.join(temporary,'src/service.rs'),service);
  let cargo=fs.readFileSync(path.join(original,'Cargo.toml'),'utf8');
  cargo=cargo.replace(/path = "([^"]+)"/g,(match,value)=>value.startsWith('src/')?match:'path = '+JSON.stringify(path.resolve(original,value)));
  const workspace=fs.readFileSync(path.join(aim,'Cargo.toml'),'utf8').split('[workspace.dependencies]')[1].split(/\n\[/)[0]
   .replace(/path = "([^"]+)"/g,(_match,value)=>'path = '+JSON.stringify(path.resolve(aim,value)));
  writeChanged(path.join(temporary,'Cargo.toml'),cargo+'\n[workspace]\n[workspace.dependencies]\n'+workspace);
  writeChanged(path.join(temporary,'Cargo.lock'),resolved);writeChanged(path.join(temporary,'.upstream-lock'),lock);
 });
 return {source:output,manifest:path.join(aim,'hal/sensors/android.hardware.sensors-service.aim.xml')};
}

export function buildArtSensors({aim,output,cargo='cargo',env=process.env,linker,libraryDirectories=[],aidl,python}){
 generateArtHalAidl({aim,aidl,python,hal:'sensors'});
 const projected=prepareArtSensors({aim,output:path.join(output,'source')}),target=path.resolve(output,'cargo');
 const remaps=nativeRustPathEnvironment({source:import.meta.dirname,build:output,roots:[[aim,'/src/aim'],...(env.CARGO_HOME?[[env.CARGO_HOME,'/src/cargo']]:[])]},env);
 execFileSync(cargo,['build','--offline','--release','--target','aarch64-linux-android','--manifest-path',path.join(projected.source,'Cargo.toml'),'--target-dir',target,'-j','2'],
  {cwd:projected.source,env:{...env,...remaps,...(linker?{CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER:linker}:{}),CARGO_ENCODED_RUSTFLAGS:[remaps.CARGO_ENCODED_RUSTFLAGS,...libraryDirectories.map(directory=>'-Lnative='+path.resolve(directory))].join('\x1f')},stdio:'inherit'});
 return {sensorsHal:path.join(target,'aarch64-linux-android/release/sensors-service'),sensorsManifest:projected.manifest};
}
