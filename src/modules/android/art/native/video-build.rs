use std::{env, path::PathBuf, process::Command};
fn main(){
    let source=PathBuf::from("src/mac-video");let out=PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let mut objects=Vec::new();
    for name in ["video-image.mm","video-pixels.mm","egl-cache.mm"] {
        let object=out.join(format!("{name}.o"));
        let mut cc=Command::new(env::var_os("CXX").unwrap_or_else(||"clang++".into()));
        cc.args(["-std=c++20","-O2","-fPIC","-fobjc-arc","-fblocks","-Wno-deprecated-declarations","-mmacosx-version-min=13.0","-c"])
            .arg(format!("-I{}",source.join("include").display()))
            .arg(source.join(name)).arg("-o").arg(&object)
            .arg(format!("-ffile-prefix-map={}=/src/aim-host-gpu",env::current_dir().unwrap().display()));
        assert!(cc.status().expect("compile shared video image").success());objects.push(object);
    }
    for name in ["video-image.mm","video-image.h","video-pixels.mm","video-pixels.hpp","angle-metal.hpp","egl-cache.mm","native-egl-cache.hpp","include/EGL","include/KHR"] {
        println!("cargo:rerun-if-changed={}",source.join(name).display());
    }
    let archive=out.join("libandroid_video.a");
    assert!(Command::new(env::var_os("AR").unwrap_or_else(||"ar".into())).arg("crs").arg(&archive).args(&objects).status().expect("archive shared video image").success());
    println!("cargo:rustc-link-search=native={}",out.display());
    println!("cargo:rustc-link-lib=static=android_video");println!("cargo:rustc-link-lib=c++");
    for name in ["Foundation","Metal","CoreImage","CoreGraphics","CoreVideo","Accelerate"] {println!("cargo:rustc-link-lib=framework={name}");}
}
