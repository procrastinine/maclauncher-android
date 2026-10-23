
#[cfg(test)]mod physical_binding_tests{
 use super::*;
 #[test]fn foreign_entry_restores_physical_jit_view(){
  const NAME:&str="sys::jit_user_copy::physical_binding_tests::foreign_entry_restores_physical_jit_view";
  if super::super::fdtab::isolated_kernel_test(NAME){return;}
  super::super::signal::install_host_handlers();
  for expected in [true,false]{
   unsafe{libc::pthread_jit_write_protect_np(i32::from(expected));}
   let host=crate::context::ForeignHostBinding::capture().unwrap();
   assert_eq!(protected().unwrap(),expected);
   unsafe{libc::pthread_jit_write_protect_np(i32::from(!expected));}
   host.restore().unwrap();assert_eq!(protected().unwrap(),expected,"physical WX view changed across returning entry");
  }
  unsafe{libc::pthread_jit_write_protect_np(1);}
 }
}
