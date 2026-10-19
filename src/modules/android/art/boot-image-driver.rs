use std::path::{Path,PathBuf};
mod graph {
    use super::*;
    pub struct Workspace(pub PathBuf);
    impl Workspace { pub fn host_bin(&self,_name:&str)->PathBuf{self.0.clone()} }
    pub struct Ctx {pub workspace:Workspace,pub files:Vec<(&'static str,PathBuf)>,pub boot_classpath:Vec<String>}
}
mod aim_paths {
    use super::*;
    fn input(name:&str)->PathBuf{PathBuf::from(std::env::var_os(name).expect(name))}
    pub fn original_image()->PathBuf{input("MACLAUNCHER_BOOT_IMAGE_SOURCE")}
    pub fn art()->PathBuf{input("MACLAUNCHER_BOOT_IMAGE_ART")}
    pub fn boot_image()->PathBuf{input("MACLAUNCHER_BOOT_IMAGE_OUTPUT")}
}
mod aim_android_image {
    pub mod assemble {
        use std::{fs,io,path::Path,os::unix::fs::PermissionsExt};
        const BUILD_DIR_MODE:u32=0o755;
        fn set_mode(path:&Path,mode:u32)->io::Result<()>{fs::set_permissions(path,fs::Permissions::from_mode(mode))}
        include!("remove.rs");
    }
}
#[path="log.rs"] mod log;
mod producer {include!("producer.rs");}
fn main(){
    let options:Vec<String>=std::env::args().skip(1).collect();
    let contents=std::fs::read_to_string(&options[1]).expect("boot image mappings");
    let files=contents.lines().map(|line|{let(guest,host)=line.split_once('\t').expect("boot image mapping");(Box::leak(guest.to_string().into_boxed_str()) as &'static str,PathBuf::from(host))}).collect();
    let classpath=std::env::var("MACLAUNCHER_BOOT_IMAGE_CLASSPATH").expect("boot class path").split(':').map(String::from).collect();
    let ctx=graph::Ctx{workspace:graph::Workspace(PathBuf::from(&options[0])),files,boot_classpath:classpath};
    let mut log=log::Log::create(Path::new(&options[2]).to_path_buf(),false).expect("boot image log");
    if let Err(error)=producer::run(&ctx,&mut log){eprintln!("{error}");std::process::exit(1)}
}
