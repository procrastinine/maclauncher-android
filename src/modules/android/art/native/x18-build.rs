use std::{env, path::PathBuf, process::Command};

fn build_x18() {
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let source = PathBuf::from("src/x18-core");
    let object = out.join("x18-core.o");
    let archive = out.join("libx18_core.a");
    for file in ["x18-core.c", "kl_x18.h", "instruction-context.h", "instruction-evidence.h", "image-elf-types.h", "image-code-metadata.h", "image-code-ranges.h", "image-instruction-evidence.h", "x18-stack-resume.h"] {
        println!("cargo:rerun-if-changed={}", source.join(file).display());
    }
    let compiler = env::var_os("CC").unwrap_or_else(|| "clang".into());
    let mut cc = Command::new(compiler);
    cc.args(["-std=gnu11", "-O2", "-fPIC", "-g0", "-Werror", "-c"])
        .arg(source.join("x18-core.c")).arg("-o").arg(&object);
    let cwd = env::current_dir().unwrap();
    cc.arg(format!("-ffile-prefix-map={}=/src/aim-linux-abi", cwd.display()));
    assert!(cc.status().expect("compile shared register translator").success());
    assert!(Command::new(env::var_os("AR").unwrap_or_else(|| "ar".into()))
        .arg("crs").arg(&archive).arg(&object).status()
        .expect("archive shared register translator").success());
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=x18_core");
}
