include Makefile
# Java lifecycle and objects belong to the selected JVM or ART. These upstream
# bootstrap drivers implement a separate Java environment for standalone hosts.
MACLAUNCHER_STANDALONE_JAVA := runtime/kl_target.c \
    runtime/guest/kl_nativeactivity.c runtime/guest/kl_slink.c \
    runtime/guest/kl_ue4.c runtime/guest/kl_jkxr.c runtime/guest/kl_native.c \
    runtime/guest/kl_sdl2.c runtime/guest/kl_gles3jni.c \
    runtime/guest/kl_driver.c runtime/guest/kl_mono.c
.PHONY: maclauncher-source-list
maclauncher-source-list:
	@printf '%s\n' $(filter-out $(RUNTIME_JNI) $(MACLAUNCHER_STANDALONE_JAVA),$(RUNTIME_SHIP))
