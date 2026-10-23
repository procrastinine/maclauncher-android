# Bionic call frames

`call-packet.h` defines the versioned AAPCS64 input/output frame. The guest `klj_call_packet` entry accepts a trusted frame containing the target, x0–x8, q0–q7, and exact converted stack bytes. It copies spills onto the original guest stack, calls the target, and records x0–x1 and q0–q3 results. x8 carries indirect aggregate result storage. Invalid size, version, target or stack shape returns negative EINVAL without changing result storage. The stack limit follows the existing NativeABI descriptor record capacity.

`call-frame.c` reuses the complete existing `nativeabi.c` descriptor parser, immutable closure pages and cache with separate Bionic closure names. It converts compact Darwin spills using those original records. `call-frame.S` captures the incoming JNI register frame before C code and restores integer/FP return registers afterward. Existing environment-normalizer dispatch and signed-page symbols remain supplied by the normal native ABI component.

The embedding JNI owner implements `klj_bionic_call_frame_invoke` for the typed Bionic image and physical Java thread. It owns admission, pending Java exceptions and target lifetime. This component does not initialize a process, publish a library, bind JNI imports, run constructors or choose a runtime. Source files are additive; normal load and ARM32 routes are unchanged.
