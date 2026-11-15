/* C11 keywords that C++ spells differently. The runtime is C11, but a few C++ files include its headers
 * (gpu_dlssg.cpp: NVIDIA Streamline's headers are C++). g++ stops on `_Noreturn` and `_Thread_local` (issue #12);
 * clang++ takes them as extensions. C++ has `[[noreturn]]` and `thread_local` in the same places.
 *
 * YW4_NORETURN: a function that never returns, in either language; the runtime's headers use it.
 *
 * For C++ only, `_Noreturn` and `_Thread_local` themselves are defined as the C++ words, so the headers that keep the
 * C11 spelling work too when this one comes first: runtime/include/yw4rt.h (the generated code's header: a change
 * there makes every player's build recompile all of the generated code) and the thread-locals in nvn.h and
 * gpu_internal.h. */
#ifndef YW4_C11_KEYWORDS_H
#define YW4_C11_KEYWORDS_H

#if defined(__cplusplus)
#define YW4_NORETURN [[noreturn]]
#ifndef _Noreturn
#define _Noreturn [[noreturn]]
#endif
#ifndef _Thread_local
#define _Thread_local thread_local
#endif
#else
#define YW4_NORETURN _Noreturn
#endif

#endif
