#pragma once
namespace maclauncher::arm32 {
// JNI values are tokens owned by the existing JNI width adapter, not guest
// memory buffers. The original declaration's typedef ancestry selects the kind.
enum class JniValue { None, Object, Method, Field, Environment, VM };
}
