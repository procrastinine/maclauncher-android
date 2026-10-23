# Bionic thread entry

`thread-source.mjs` supplies `prepareBionicThreads(aim)` for the original AIM Linux ABI with the guest-callback and shared process-identity adaptations. Preparation copies the owned thread implementation and applies exact source anchors. Its source closure contains the native adapters and original supplier inputs. The helper has no runtime-selection side effects.

An explicitly loaded host module receives `KljBionicAdmissionApi` through `maclauncher_bionic_admission_install`. A nonce belongs to one physical host thread. The host reserves it before constructors, then an original Bionic coordinator arms it around original `pthread_create`. The original clone publishes the target thread's existing kernel-leased Android TID before its parent/child TID words and process metadata. A live Linux thread-record collision returns `EBUSY`. Ordinary clones retain their original ID allocator and physical thread creation.

Finite entries run the original Bionic child on the owning Java thread, with original TLS, pthread state and syscall handling. Nested JNI calls retain the same `JNIEnv`, Java references and physical thread. Each entry restores the host signal mask, effective alternate signal stack, errno and owned TSD slots before returning. Disabled alternate-stack restoration uses a valid Darwin minimum-size request; disabled pointer and size fields are ignored.

`retire` completes the original Bionic thread exit and TLS destructors after a Java thread's final entry. The shared Android identity lease remains owned by the real Mach thread through later native destructors and retires after kernel thread death. It is never released as a newly allocated clone identity. Repeated retirement is rejected.

Nested entries return through their active JNI callbacks. Cooperative yield and guest exit at an active callback depth return `EBUSY`; structured Java exception propagation for terminating that callback belongs to the embedding JNI boundary. This component provides finite entry and ownership, while process bootstrap, original-linker handle routing and complete JNI-table integration remain separate APIs.

## Native pthread C calls

Original Bionic-created threads call pure C NDK, Bitmap, Looper and EGL/GLES APIs without attaching to Java. JNI-bearing functions require the real calling thread's authenticated environment. Synchronous guest callbacks reenter the existing hostcall scope and retain executable-image, classloader, physical-thread and descriptor ownership checks. The VM continues to report `JNI_EDETACHED` for a native thread until it explicitly attaches.

Native descriptor leases require the original live context, canonical Android identity and matching physical thread registration. Looper registrations retain their original host owner until physical thread retirement. Their pthread key stores the owner directly, and a retained Looper reference preserves it across TLS destructor ordering; Java retirement and native physical retirement share the same idempotent cleanup.

Native `dlopen` authenticates its calling application image before publishing the original linker graph. Pure C threads borrow a VM attachment only while retaining JNI metadata references; that attachment is released before original constructors run. Existing caller attachments, original namespace selection and native pthread ownership are preserved.

## Process and signal entry

`process-source.mjs` supplies `prepareBionicProcess(aim)` after the thread preparation. It retains the original process initialization, ELF loading and syscall dispatcher, and adds a finite `klj_bionic_process_prepare` entry for an existing Java process. The initial guest thread uses the authoritative process leader identity on the physical Java first thread. Its nonce remains a process-lifetime owner and is distinct from ordinary Java-thread retirement. Original Java/JLI options and the existing native ABI routes remain owned by their callers.

`jsig-bridge.c` uses the selected JDK's original signal-chaining library without changing the process-wide symbol namespace. The launcher supplies `KLJ_BIONIC_JSIG_LIBRARY` as the exact canonical, already-verified `lib/libjsig.dylib` path and includes that same library in its early preload list. The bridge accepts only that already-loaded file and verifies the original JVM signal hooks resolve to it. Existing storage, process and graphics preloads remain in the list.

The bridge forwards signal registration to original libjsig. A bounded thread-local guard permits its real OS call only from the original library's text range; the bridge's own libc binding bypasses interposition. `jsig-bridge.h` defines the versioned native query contract, including the original kernel action entry and numeric call counters. The embedding signal adapter retains JVM asynchronous handlers and AIM's separate SIGEMT carrier. With initialized libjsig, HotSpot handles its synchronous signals first and chains unrecognized faults to the original Linux ABI handler.

The EGL/GLES carrier returns typed guest procedures. Core and KHR debug callbacks retain the original GL context's debug-state lifetime and callback query values. Synchronous messages reenter the existing guest invocation scope; replacing or clearing a callback and destroying the debug state release its native lease. Callback allocation failure preserves the previous registration and reports `GL_OUT_OF_MEMORY`.
