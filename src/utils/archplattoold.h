// Architecture / Platform / Toolchain dependent
#ifndef __LIBAPTH_UTILS_ARCHPLATTOOLD_H
#define __LIBAPTH_UTILS_ARCHPLATTOOLD_H

#define apth_expect(x, val) __builtin_expect(!!(x), (val))
#define apth_likely(x) __builtin_expect(!!(x), 1)
#define apth_unlikely(x) __builtin_expect(!!(x), 0)

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define MAYBE_UNUSED [[maybe_unused]]
#elif defined(__GNUC__) || defined(__clang__)
#define MAYBE_UNUSED __attribute__((unused))
#else
#define MAYBE_UNUSED
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define NORETURN [[noreturn]]
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#include <stdnoreturn.h>
#define NORETURN noreturn
#elif defined(__GNUC__) || defined(__clang__)
#define NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define NORETURN __declspec(noreturn)
#else
#define NORETURN
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef APTH_BUILDING_DLL
#ifdef __GNUC__
#define APTH_API __attribute__((dllexport))
#else
#define APTH_API __declspec(dllexport)
#endif
#else
#ifdef __GNUC__
#define APTH_API __attribute__((dllimport))
#else
#define APTH_API __declspec(dllimport)
#endif
#endif
#define APTH_INTERNAL
#else
#if __GNUC__ >= 4
#define APTH_API __attribute__((visibility("default")))
#define APTH_INTERNAL __attribute__((visibility("hidden")))
#else
#define APTH_API
#define APTH_INTERNAL
#endif
#endif

// #if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
// #include <stdalign.h>
// #define ALIGNED(n) alignas(n)
#if defined(__GNUC__) || defined(__clang__)
#define ALIGNED(n) __attribute__((aligned(n)))
#elif defined(_MSC_VER)
#define ALIGNED(n) __declspec(align(n))
#else
#define ALIGNED(n)
#endif

#endif // __LIBAPTH_UTILS_ARCHPLATTOOLD_H
