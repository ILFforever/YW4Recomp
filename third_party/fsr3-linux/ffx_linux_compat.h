// Force-included (-include) when building FidelityFX SDK v1.1.4 sources on Linux.
// Supplies the MSVC CRT names the SDK uses without editing AMD's sources.
#pragma once

#ifdef __cplusplus
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <codecvt>
#include <locale>
#include <string>

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif

inline int wcscpy_s(wchar_t* dst, size_t dstCount, const wchar_t* src)
{
    if (!dst || dstCount == 0)
        return 1;
    wcsncpy(dst, src ? src : L"", dstCount - 1);
    dst[dstCount - 1] = L'\0';
    return 0;
}
template <size_t N> inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) { return wcscpy_s(dst, N, src); }

inline int strcpy_s(char* dst, size_t dstCount, const char* src)
{
    if (!dst || dstCount == 0)
        return 1;
    strncpy(dst, src ? src : "", dstCount - 1);
    dst[dstCount - 1] = '\0';
    return 0;
}
template <size_t N> inline int strcpy_s(char (&dst)[N], const char* src) { return strcpy_s(dst, N, src); }

inline int wcsncpy_s(wchar_t* dst, size_t dstCount, const wchar_t* src, size_t count)
{
    if (!dst || dstCount == 0)
        return 1;
    size_t n = count < dstCount - 1 ? count : dstCount - 1;
    wcsncpy(dst, src ? src : L"", n);
    dst[n] = L'\0';
    return 0;
}
template <size_t N> inline int wcsncpy_s(wchar_t (&dst)[N], const wchar_t* src, size_t count) { return wcsncpy_s(dst, N, src, count); }

template <typename... Args> inline int sprintf_s(char* dst, size_t dstCount, const char* format, Args... args)
{
    return snprintf(dst, dstCount, format, args...);
}
template <size_t N, typename... Args> inline int sprintf_s(char (&dst)[N], const char* format, Args... args)
{
    return snprintf(dst, N, format, args...);
}
template <typename... Args> inline int swprintf_s(wchar_t* dst, size_t dstCount, const wchar_t* format, Args... args)
{
    return swprintf(dst, dstCount, format, args...);
}
#endif
