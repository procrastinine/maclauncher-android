# Bionic thread entry

This component runs original Bionic thread code on the Java threads of an existing host process.

`thread-source.mjs` supplies `prepareBionicThreads(aim)` for the original AIM Linux ABI, with the guest-callback and shared process-identity adaptations. Preparation copies the thread implementation maintained here and applies it at exact source anchors. Its source set contains the native adapters and the original upstream inputs. The helper has no side effects on runtime selection.

An explicitly loaded host module receives `KljBionicAdmissionApi`, the interface that lets existing host threads enter Bionic, through `maclauncher_bionic_admission_install`. A nonce belongs to one physical host thread. The host reserves the nonce before constructors run, and an original Bionic coordinator then arms it around the original `pthread_create`. The original clone publishes the Android TID that the target thread already holds through a kernel-locked lease before its parent and child TID words and process metadata. A collision with a live Linux thread record returns `EBUSY`. Ordinary clones keep their original ID allocator and physical thread creation.

A finite entry runs the original Bionic child on the Java thread that owns it, with the original TLS, pthread state and syscall handling. Nested JNI calls keep the same `JNIEnv`, Java references and physical thread. Before returning, each entry restores the host signal mask, the effective alternate signal stack, errno and the TSD slots it owns. To restore a disabled alternate stack, the entry issues a valid Darwin minimum-size request and ignores the disabled pointer and size fields.

`retire` completes the original Bionic thread exit and TLS destructors after a Java thread's final entry. The shared lease on its Android thread ID stays with the real Mach thread through later native destructors and is released after the kernel thread dies. It is never released as though it were the ID of a newly allocated clone. A repeated `retire` is rejected.

Nested entries return through their active JNI callbacks. A cooperative yield or guest exit at an active callback depth returns `EBUSY`; structured propagation of a Java exception that terminates that callback is the job of the embedding JNI boundary. This component provides finite entry and ownership. Process bootstrap, original-linker handle routing and complete JNI-table integration are separate APIs.

## Native pthread C calls

Threads created by the original Bionic call pure C NDK, Bitmap, Looper and EGL/GLES APIs without attaching to Java. Functions that take JNI arguments need the real calling thread's authenticated environment. Synchronous guest callbacks reenter the existing host-call scope and keep the checks on executable image, class loader, physical thread and descriptor ownership. The VM keeps reporting `JNI_EDETACHED` for a native thread until that thread attaches explicitly.

Native descriptor leases require the original live context, the canonical Android identity and a matching physical thread registration. Looper registrations keep their original host owner until the physical thread shuts down. Their pthread key stores that owner directly, and a retained Looper reference keeps it available regardless of TLS destructor order. Java thread shutdown and native physical-thread shutdown share the same idempotent cleanup.

Native `dlopen` authenticates its calling application image before publishing the original linker graph. Pure C threads borrow a VM attachment only while they hold JNI metadata references and release it before the original constructors run. Existing caller attachments, original namespace selection and native pthread ownership are preserved.

## Process and signal entry

`process-source.mjs` supplies `prepareBionicProcess(aim)`, which runs after the thread preparation. It keeps the original process initialization, ELF loading and syscall dispatcher, and adds a finite `klj_bionic_process_prepare` entry for an existing Java process. The initial guest thread uses the process leader identity on the physical first Java thread. Its nonce is held for the life of the process and is separate from ordinary Java thread shutdown. Original Java and JLI options and the existing native ABI routes stay with their callers.

`jsig-bridge.c` uses the selected JDK's original signal-chaining library without changing the process-wide symbol namespace. The launcher sets `KLJ_BIONIC_JSIG_LIBRARY` to the exact canonical, already verified `lib/libjsig.dylib` path and includes the same library in its early preload list. The bridge accepts only that already loaded file and verifies that the original JVM signal hooks resolve to it. The existing storage, process and graphics preloads stay in the list.

The bridge forwards signal registration to the original libjsig. A bounded thread-local guard allows the real OS call only from the original library's text range, and the bridge's own libc binding bypasses interposition. `jsig-bridge.h` defines the versioned native query contract, including the original kernel action entry and numeric call counters. The embedding signal adapter keeps the JVM's asynchronous handlers and AIM's separate SIGEMT signal path. With libjsig initialized, HotSpot handles its synchronous signals first and chains unrecognized faults to the original Linux ABI handler.

## EGL and GLES debug callbacks

The EGL/GLES carrier, the generated guest library described in [egl/README.md](egl/README.md), returns typed guest procedures. Core and KHR debug callbacks keep the original GL context's debug-state lifetime and callback query values. Synchronous messages reenter the existing guest invocation scope. Replacing or clearing a callback, or destroying the debug state, releases its native lease. If callback allocation fails, the previous registration stays in place and the carrier reports `GL_OUT_OF_MEMORY`.
