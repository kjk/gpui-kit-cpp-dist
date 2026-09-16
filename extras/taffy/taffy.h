#ifndef TAFFY_AMALGAM_H_
#define TAFFY_AMALGAM_H_
#ifndef GPUI_INCLUDE_PRIVATE_API
#define GPUI_INCLUDE_PRIVATE_API 0
#endif
#ifndef GPUI_BASE_H_
#define GPUI_BASE_H_
#line 1 "src/base.h"

#ifndef UNICODE
#define UNICODE
#endif

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <algorithm>
#include <utility>

#if defined(__EMSCRIPTEN__)
#define GPUI_OS_WINDOWS 0
#define GPUI_OS_LINUX 0
#define GPUI_OS_MAC 0
#define GPUI_OS_IOS 0
#define GPUI_OS_ANDROID 0
#define GPUI_OS_WASM 1
#elif defined(_WIN32)
#define GPUI_OS_WINDOWS 1
#define GPUI_OS_LINUX 0
#define GPUI_OS_MAC 0
#define GPUI_OS_IOS 0
#define GPUI_OS_ANDROID 0
#define GPUI_OS_WASM 0
#elif defined(__ANDROID__)
#define GPUI_OS_WINDOWS 0
#define GPUI_OS_LINUX 0
#define GPUI_OS_MAC 0
#define GPUI_OS_IOS 0
#define GPUI_OS_ANDROID 1
#define GPUI_OS_WASM 0
#elif defined(__APPLE__) && \
    defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
#define GPUI_OS_WINDOWS 0
#define GPUI_OS_LINUX 0
#define GPUI_OS_MAC 0
#define GPUI_OS_IOS 1
#define GPUI_OS_ANDROID 0
#define GPUI_OS_WASM 0
#elif defined(__APPLE__)
#define GPUI_OS_WINDOWS 0
#define GPUI_OS_LINUX 0
#define GPUI_OS_MAC 1
#define GPUI_OS_IOS 0
#define GPUI_OS_ANDROID 0
#define GPUI_OS_WASM 0
#elif defined(__linux__)
#define GPUI_OS_WINDOWS 0
#define GPUI_OS_LINUX 1
#define GPUI_OS_MAC 0
#define GPUI_OS_IOS 0
#define GPUI_OS_ANDROID 0
#define GPUI_OS_WASM 0
#else
#error "unsupported platform"
#endif

#define GPUI_OS_POSIX (!GPUI_OS_WINDOWS)

#if GPUI_OS_WINDOWS
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#else
#include <pthread.h>
#include <limits.h>
#endif

namespace base {

template <typename T, size_t N>
char (&DimofSizeHelper(T (&array)[N]) noexcept)[N];
#ifndef dimof
#define dimof(array) (sizeof(base::DimofSizeHelper(array)))
#endif

enum : uint16_t {
    kMaxPath = 1024
};

struct Arena;

struct Str {
    char* s;
    int len;

    constexpr Str() noexcept : s(nullptr), len(0) {}

    explicit Str(const char* s_) : s((char*)s_), len(0) {
        len = s_ ? (int)strlen(s_) : 0;
    }
    constexpr explicit Str(const char* s_, int len_) noexcept
        : s((char*)s_), len(len_) {}
    explicit Str(char* s_) : s(s_), len(0) { len = s ? (int)strlen(s) : 0; }
    constexpr explicit Str(char* s_, int len_) noexcept : s(s_), len(len_) {}

    explicit operator bool() const { return len > 0 && s; }
};

float StrToFloatUnchecked(Str s);

void log(Str s);

using TempStr = Str;

#define StrL(lit) ::base::Str{(char*)(lit), (int)dimof(lit) - 1}

TempStr AllocStrTemp(int size);
TempStr StrDupTemp(Str s);
TempStr ReadBoundedFileTemp(Str path, int limit);

#if GPUI_OS_WINDOWS

WCHAR* ToCWstrTemp(Str s);
#endif

uint64_t PlatPageSize();
uint64_t PlatLargePageSize();

uint64_t PlatArenaReserveSize();

void* PlatMemReserve(uint64_t size);
bool PlatMemCommit(void* base, uint64_t size, bool largePages);
void* PlatMemReserveCommit(uint64_t size, bool largePages);
void PlatMemRelease(void* base, uint64_t size);

int StrCmpI(const char* a, const char* b);
int StrCmpNI(const char* a, const char* b, int n);

void StrCopyZ(char* dst, int cap, const char* src);

bool PlatDirExists(const char* path);

bool PlatFileExists(const char* path);
void PlatGetCwd(char* out, int cap);

bool PlatCanonicalPath(const char* path, char* out, int cap);

void PlatGetExeDir(char* out, int cap);

struct DirEntry {
    char name[260] = {};
    bool isDir = false;
    bool isFile = false;
    bool isSymlink = false;
    uint64_t size = 0;
    uint64_t modified = 0;
};

int PlatListDir(const char* dir, DirEntry* out, int max);

int PlatCoreCount();

bool PlatSelfUsage(uint64_t* cpu100ns, uint64_t* memBytes);

void* AllocZero(int count, int size);

template <typename T>
inline T* AllocArray(int n) {
    return (T*)AllocZero(n, (int)sizeof(T));
}

template <typename T>
inline void ZeroStruct(T* s) {
    memset((void*)s, 0, sizeof(T));
}

struct Func0 {

    static constexpr uintptr_t kFuncNoArg = ~(uintptr_t)1;

    void* fn = nullptr;
    uintptr_t userData = 0;

    Func0() = default;

    bool IsValid() const { return fn != nullptr; }
    void Call() const {
        if (!fn) {
            return;
        }
        if (userData == kFuncNoArg) {
            auto func = (void (*)())fn;
            func();
            return;
        }
        auto func = (void (*)(uintptr_t))fn;
        func(userData);
    }
};

template <typename T>
Func0 MkFunc0(void (*fn)(T*), T* d) {
    auto res = Func0{};
    res.fn = (void*)fn;
    res.userData = (uintptr_t)d;
    return res;
}

inline Func0 MkFunc0Void(void (*fn)()) {
    auto res = Func0{};
    res.fn = (void*)fn;
    res.userData = Func0::kFuncNoArg;
    return res;
}

template <typename T>
struct Func1 {

    static constexpr uintptr_t kDropsArgBit = 1;
    static constexpr uintptr_t kFuncNoArg = Func0::kFuncNoArg;

    void* fn = nullptr;
    uintptr_t userData = 0;

    Func1() = default;

    Func1(const Func0& that) {
        this->fn = that.fn;
        this->SetData(that.userData, true);
    }

    void SetData(uintptr_t d, bool dropsArg) {
        userData = d | (dropsArg ? kDropsArgBit : 0);
    }
    bool IsValid() const { return fn != nullptr; }
    void Call(T arg) const {
        if (!fn) {
            return;
        }
        uintptr_t d = userData & ~kDropsArgBit;
        if (userData & kDropsArgBit) {
            if (d == kFuncNoArg) {
                auto func = (void (*)())fn;
                func();
            } else {
                auto func = (void (*)(uintptr_t))fn;
                func(d);
            }
            return;
        }
        if (d == kFuncNoArg) {
            auto func = (void (*)(T))fn;
            func(arg);
            return;
        }
        auto func = (void (*)(uintptr_t, T))fn;
        func(d, arg);
    }
};

template <typename T1, typename T2>
Func1<T2> MkFunc1(void (*fn)(T1*, T2), T1* d) {
    auto res = Func1<T2>{};
    res.fn = (void*)fn;
    res.SetData((uintptr_t)d, false);
    return res;
}

template <typename T2>
Func1<T2> MkFunc1Void(void (*fn)(T2)) {
    auto res = Func1<T2>{};
    res.fn = (void*)fn;
    res.SetData(Func1<T2>::kFuncNoArg, false);
    return res;
}

struct Mutex {
#if GPUI_OS_WINDOWS
    SRWLOCK lock = SRWLOCK_INIT;
    void Lock() { AcquireSRWLockExclusive(&lock); }
    void Unlock() { ReleaseSRWLockExclusive(&lock); }
#else
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    void Lock() { pthread_mutex_lock(&lock); }
    void Unlock() { pthread_mutex_unlock(&lock); }
#endif
    Mutex() = default;
    ~Mutex() = default;
};

struct CondVar {
#if GPUI_OS_WINDOWS
    CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
    void Wait(Mutex* m, int timeoutMs) {
        DWORD t = timeoutMs < 0 ? INFINITE : (DWORD)timeoutMs;
        SleepConditionVariableSRW(&cv, &m->lock, t, 0);
    }
    void WakeOne() { WakeConditionVariable(&cv); }
    void WakeAll() { WakeAllConditionVariable(&cv); }
#else
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    void Wait(Mutex* m, int timeoutMs);
    void WakeOne() { pthread_cond_signal(&cv); }
    void WakeAll() { pthread_cond_broadcast(&cv); }
#endif
    CondVar() = default;
    ~CondVar() = default;
};

bool PlatThreadRun(Func0 f);

uint64_t PlatThreadId();
void PlatSleepMs(int ms);

static const uint64_t kArenaHeaderSize = 256;

struct Arena {
    Arena* prev;
    Arena* current;
    uint64_t flags;
    uint64_t commitChunkSize;
    uint64_t reserveChunkSize;
    uint64_t basePos;
    uint64_t pos;
    uint64_t committed;
    uint64_t reserved;
    const char* allocationSiteFile;
    int allocationSiteLine;
    const char* name;
    bool usesExternalBuffer;
    Mutex lock;
    uint64_t nAllocsLifetime;
    uint64_t peakBytesLifetime;
    uint64_t nAllocsSinceReset;
    uint64_t peakBytesSinceReset;

    void* Alloc(int size);
    void Reset();
    void* Push(uint64_t size, uint64_t align = 8, bool zero = true);
    void PopTo(uint64_t pos);

    Arena() = delete;
    ~Arena() = delete;
};

Arena* ArenaNew();
void ArenaDelete(Arena* arena);

uint64_t ArenaUsed(Arena* arena);

int VarintSize(uint32_t v);

int VarintPut(char* dst, uint32_t v);

int VarintGet(const char* src, uint32_t* out);

using ArenaStr = uint32_t;

constexpr ArenaStr kArenaStrNone = 0;

constexpr bool ArenaStrIsSet(ArenaStr s) {
    return s != kArenaStrNone;
}

ArenaStr ArenaStrDup(Arena* a, Str src);

uint32_t ArenaStrLen(Arena* a, ArenaStr s);

ArenaStr ArenaStrAppend(Arena* a, ArenaStr s, Str more);

Str ArenaStrGet(Arena* a, ArenaStr s);

constexpr uint32_t kArenaPtrNone = 0;

uint32_t ArenaOffsetOf(Arena* a, const void* p);

inline void* ArenaAtOffset(Arena* a, uint32_t off) {
    if (off == kArenaPtrNone || !a) {
        return nullptr;
    }
    Arena* node = a->current;

    if (node && node->basePos <= (uint64_t)off) {
        return (char*)node + ((uint64_t)off - node->basePos);
    }
    while (node && node->basePos > (uint64_t)off) {
        node = node->prev;
    }
    return node ? (char*)node + ((uint64_t)off - node->basePos) : nullptr;
}

template <typename T>
struct ArenaPtr {
    uint32_t off = kArenaPtrNone;

    bool IsSet() const { return off != kArenaPtrNone; }
    bool operator==(const ArenaPtr<T>& o) const { return off == o.off; }
    bool operator!=(const ArenaPtr<T>& o) const { return off != o.off; }
};

template <typename T>
ArenaPtr<T> ArenaPtrOf(Arena* a, const T* p) {
    return ArenaPtr<T>{ArenaOffsetOf(a, p)};
}

template <typename T>
T* ArenaPtrGet(Arena* a, ArenaPtr<T> p) {
    return (T*)ArenaAtOffset(a, p.off);
}

Arena* GetTempArena();
void ResetTempArena();
void DestroyTempArena();

void* Alloc(struct Arena* arena, int size);
void Free(struct Arena* arena, void* mem);

template <typename T, typename... Args>
T* ArenaNew(Arena* arena, Args&&... args) {
    void* mem = Alloc(arena, (int)sizeof(T));
    return new (mem) T(std::forward<Args>(args)...);
}

#if GPUI_OS_WINDOWS
#define GPUI_NOINLINE __declspec(noinline)
#else
#define GPUI_NOINLINE __attribute__((noinline))
#endif

void* ArenaVecAlloc(struct Arena* a, int count, int elSize, int align,
                    int hdrSize = 0);

GPUI_NOINLINE bool VecRealloc(struct Arena* a, void** els, int len, int* cap,
                              int newCap, int elSize);

#if defined(DEBUG)
int VecDbgBirth(const char* file, int line, const char* func, char kind,
                int elSize) noexcept;
void VecDbgGrow(int id, int len, int oldCap, int needed, int newCap) noexcept;
void VecDbgSegment(int id, int len, int want, int lastSegCap, int newSegCap,
                   int totalCap, bool reused) noexcept;
void VecDbgDeath(int id, int len, int cap) noexcept;
void VecDbgArenaDeath(int id, int len, int totalCap, int segCount) noexcept;

#define GPUI_VEC_DBG_ARGS0                                            \
    const char *dbgF = __builtin_FILE(), int dbgL = __builtin_LINE(), \
               const char *dbgFn = __builtin_FUNCTION()
#define GPUI_VEC_DBG_ARGS , GPUI_VEC_DBG_ARGS0
#define GPUI_VEC_DBG_INIT(kind) \
    : dbgId(VecDbgBirth(dbgF, dbgL, dbgFn, kind, (int)sizeof(T)))
#else
#define GPUI_VEC_DBG_ARGS0
#define GPUI_VEC_DBG_ARGS
#define GPUI_VEC_DBG_INIT(kind)
#endif

template <typename T>
struct Vec;

template <typename T>
struct VecIdentity {
    using type = T;
};
template <typename T>
using VecIdentityT = typename VecIdentity<T>::type;

#if defined(__GNUC__) || defined(__clang__)

struct __attribute__((__may_alias__)) VecNonTemplated {
#else
struct VecNonTemplated {
#endif
    int len;
    int cap;
    void* els;
};

bool VecReserveNT(Arena* arena, VecNonTemplated* v, int elSize, int wantedSize);
void* VecInsertSpaceNT(VecNonTemplated* v, int elSize, int idx, int count);
bool VecResizeNT(VecNonTemplated* v, int elSize, int newSize);
void VecRemoveAtNT(VecNonTemplated* v, int elSize, int idx, int count);
void VecRemoveAtFastNT(VecNonTemplated* v, int elSize, int idx);
void VecFreeElementsNT(VecNonTemplated* v);
void VecClearNT(VecNonTemplated* v, int elSize);
void* VecTakeNT(VecNonTemplated* v, int elSize);
void VecCopyFromNT(VecNonTemplated* v, int elSize, int srcLen,
                   const void* srcEls, bool zeroTail);

template <typename T>
VecNonTemplated* VecNT(Vec<T>& v);

template <typename T>
auto VecReserve(Arena* arena, T& v, int n) -> decltype(v.els);

template <typename T>
inline T* VecReserve(Vec<T>& v, int n);

template <typename T>
bool VecResize(Vec<T>& v, int newSize);

template <typename T>
T* VecInsertSpace(Vec<T>& v, int idx, int count);

template <typename T>
void VecClear(Vec<T>& v);

template <typename T>
void VecReset(Vec<T>& v);

template <typename T>
void VecFreeMembers(Vec<T>& v);

template <typename T>
T* VecTake(Vec<T>& v);

template <typename T>
T* VecData(const Vec<T>& v);

template <typename T>
bool VecAppend(Vec<T>& v, const VecIdentityT<T>& el);

template <typename T>
bool VecAppendVec(Vec<T>& v, const Vec<T>& other);

template <typename T>
bool VecAppendN(Vec<T>& v, const T* src, int count);

template <typename T>
T* VecAppendBlanks(Vec<T>& v, int count);

template <typename T>
bool VecInsertAt(Vec<T>& v, int idx, const VecIdentityT<T>& el);

template <typename T, typename E>
bool VecPush(Arena* arena, T& v, E el);

template <typename T>
void VecRemoveAtN(Vec<T>& v, int idx, int count);

template <typename T>
void VecRemoveAt(Vec<T>& v, int idx);

template <typename T>
T VecPopAt(Vec<T>& v, int idx);

template <typename T>
void VecRemoveAtFast(Vec<T>& v, int idx);

template <typename T>
void VecRemoveLast(Vec<T>& v);

template <typename T>
T VecPop(Vec<T>& v);

template <typename T>
int VecRemove(Vec<T>& v, const T& el);

template <typename T>
bool VecIsValidIndex(const Vec<T>& v, int idx);

template <typename T>
T& VecLast(const Vec<T>& v);

template <typename T>
int VecFind(const Vec<T>& v, const T& el, int startAt = 0);

template <typename T>
bool VecContains(const Vec<T>& v, const T& el);

template <typename T>
struct Vec {
    int len = 0;

    int cap = 0;
    T* els = nullptr;
#if defined(DEBUG)
    int dbgId = 0;
#endif

    explicit Vec(GPUI_VEC_DBG_ARGS0) noexcept GPUI_VEC_DBG_INIT('V') {}

    Vec(const Vec& other GPUI_VEC_DBG_ARGS) GPUI_VEC_DBG_INIT('V') {
        VecCopyFromNT(VecNT(*this), (int)sizeof(T), other.len,
                      (const void*)other.els, false);
    }

    Vec& operator=(const Vec& other) {
        if (this == &other) {
            return *this;
        }
        VecReset(*this);
        VecCopyFromNT(VecNT(*this), (int)sizeof(T), other.len,
                      (const void*)other.els, true);
        return *this;
    }

    ~Vec() {
#if defined(DEBUG)
        VecDbgDeath(dbgId, len, cap < 0 ? -cap : cap);
#endif
        VecReset(*this);
    }

    T& operator[](int idx) const { return els[idx]; }

    using iterator = T*;
    using const_iterator = const T*;
    iterator begin() { return els; }
    const_iterator begin() const { return els; }
    iterator end() { return els ? els + len : nullptr; }
    const_iterator end() const { return els ? els + len : nullptr; }
};

static_assert(offsetof(Vec<char>, len) == offsetof(VecNonTemplated, len));
static_assert(offsetof(Vec<char>, cap) == offsetof(VecNonTemplated, cap));
static_assert(offsetof(Vec<char>, els) == offsetof(VecNonTemplated, els));
static_assert(offsetof(Vec<double>, els) == offsetof(VecNonTemplated, els));
#if !defined(DEBUG)
static_assert(sizeof(Vec<char>) == sizeof(VecNonTemplated));
static_assert(sizeof(Vec<double>) == sizeof(VecNonTemplated));
#endif

template <typename T>
inline int len(const Vec<T>& v) {
    return v.len;
}

template <typename T>
VecNonTemplated* VecNT(Vec<T>& v) {
    return (VecNonTemplated*)&v;
}

template <typename T>
auto VecReserve(Arena* arena, T& v, int n) -> decltype(v.els) {
    static_assert(offsetof(T, len) == offsetof(VecNonTemplated, len));
    static_assert(offsetof(T, cap) == offsetof(VecNonTemplated, cap));
    static_assert(offsetof(T, els) == offsetof(VecNonTemplated, els));
    if (!VecReserveNT(arena, (VecNonTemplated*)&v, (int)sizeof(*v.els), n)) {
        return nullptr;
    }
    return v.els;
}

template <typename T, int N>
inline void VecUseExternalBuffer(Vec<T>& v, T (&buf)[N]) {
    v.els = buf;
    v.cap = -N;
    v.len = 0;
}

template <typename T>
inline T* VecReserve(Vec<T>& v, int n) {
#if defined(DEBUG)
    int curCap = v.cap < 0 ? -v.cap : v.cap;
    if (n > curCap) {
        int floorCap = sizeof(T) == 1 ? 8 : sizeof(T) <= 1024 ? 4 : 1;
        int next =
            curCap == 0 ? std::max(floorCap, n) : std::max(curCap * 2, n);
        VecDbgGrow(v.dbgId, v.len, curCap, n, next);
    }
#endif
    return VecReserve(nullptr, v, n);
}

template <typename T>
T* VecInsertSpace(Vec<T>& v, int idx, int count) {
    return (T*)VecInsertSpaceNT(VecNT(v), (int)sizeof(T), idx, count);
}

template <typename T>
bool VecResize(Vec<T>& v, int newSize) {
    return VecResizeNT(VecNT(v), (int)sizeof(T), newSize);
}

template <typename T>
void VecClear(Vec<T>& v) {
    VecClearNT(VecNT(v), (int)sizeof(T));
}

template <typename T>
void VecReset(Vec<T>& v) {
    VecFreeElementsNT(VecNT(v));
}

template <typename T>
void VecFreeMembers(Vec<T>& v) {
    for (int i = 0; i < v.len; i++) {
        free(v.els[i]);
    }
    VecReset(v);
}

template <typename T>
T* VecTake(Vec<T>& v) {
    return (T*)VecTakeNT(VecNT(v), (int)sizeof(T));
}

template <typename T>
T* VecData(const Vec<T>& v) {
    return v.els;
}

template <typename T>
bool VecAppend(Vec<T>& v, const VecIdentityT<T>& el) {
    return VecInsertAt(v, v.len, el);
}

template <typename T>
bool VecAppendVec(Vec<T>& v, const Vec<T>& other) {
    return VecAppendN(v, other.els, other.len);
}

template <typename T>
bool VecAppendN(Vec<T>& v, const T* src, int count) {
    if (count == 0) {
        return true;
    }
    T* dst = VecInsertSpace(v, v.len, count);
    if (!dst) {
        return false;
    }
    memcpy((void*)dst, (const void*)src, (size_t)count * sizeof(T));
    return true;
}

template <typename T>
T* VecAppendBlanks(Vec<T>& v, int count) {
    return VecInsertSpace(v, v.len, count);
}

template <typename T>
bool VecInsertAt(Vec<T>& v, int idx, const VecIdentityT<T>& el) {
    T* p = VecInsertSpace(v, idx, 1);
    if (!p) {
        return false;
    }
    p[0] = el;
    return true;
}

template <typename T, typename E>
bool VecPush(Arena* arena, T& v, E el) {
    if (!VecReserve(arena, v, v.len + 1)) {
        return false;
    }
    v.els[v.len++] = el;
    return true;
}

template <typename T>
void VecRemoveAtN(Vec<T>& v, int idx, int count) {
    VecRemoveAtNT(VecNT(v), (int)sizeof(T), idx, count);
}

template <typename T>
void VecRemoveAt(Vec<T>& v, int idx) {
    VecRemoveAtN(v, idx, 1);
}

template <typename T>
T VecPopAt(Vec<T>& v, int idx) {
    T el = v.els[idx];
    VecRemoveAt(v, idx);
    return el;
}

template <typename T>
void VecRemoveAtFast(Vec<T>& v, int idx) {
    VecRemoveAtFastNT(VecNT(v), (int)sizeof(T), idx);
}

template <typename T>
void VecRemoveLast(Vec<T>& v) {
    if (v.len > 0) {
        VecRemoveAt(v, v.len - 1);
    }
}

template <typename T>
T VecPop(Vec<T>& v) {
    T el = v.els[v.len - 1];
    VecRemoveAtFast(v, v.len - 1);
    return el;
}

template <typename T>
int VecRemove(Vec<T>& v, const T& el) {
    int i = VecFind(v, el);
    if (i >= 0) {
        VecRemoveAt(v, i);
    }
    return i;
}

template <typename T>
inline void DeleteVecMembers(Vec<T>& v) {
    for (T& el : v) {
        delete el;
    }
    VecClear(v);
}

template <typename T>
bool VecIsValidIndex(const Vec<T>& v, int idx) {
    return idx >= 0 && idx < v.len;
}

template <typename T>
T& VecLast(const Vec<T>& v) {
    return v.els[v.len - 1];
}

template <typename T>
int VecFind(const Vec<T>& v, const T& el, int startAt) {
    for (int i = startAt; i < v.len; i++) {
        if (v.els[i] == el) {
            return i;
        }
    }
    return -1;
}

template <typename T>
bool VecContains(const Vec<T>& v, const T& el) {
    return VecFind(v, el) >= 0;
}

template <typename T>
struct VecSortCmp {
    using Fn = int (*)(const T* a, const T* b);
};

template <typename T>
void VecSort(Vec<T>& v, typename VecSortCmp<T>::Fn cmpFunc) {
    if (v.len > 0) {
        auto cmp = (int (*)(const void*, const void*))cmpFunc;
        qsort((void*)v.els, (size_t)v.len, sizeof(T), cmp);
    }
}

template <typename T>
void VecReverse(Vec<T>& v) {
    for (int i = 0; i < v.len / 2; i++) {
        std::swap(v.els[i], v.els[v.len - i - 1]);
    }
}

template <typename T>
struct ArenaVecSegment {

    ArenaPtr<ArenaVecSegment<T>> next;

    int base;
    int len;

    int cap;

    static constexpr int HeaderSize() {
        return ((int)sizeof(ArenaVecSegment<T>) + (int)alignof(T) - 1) &
               ~((int)alignof(T) - 1);
    }

    T* Els() const { return (T*)((char*)(void*)this + HeaderSize()); }
};

constexpr int kArenaVecCap0 = 4;
constexpr int kArenaVecCap1 = 16;
constexpr int kArenaVecCap2 = 64;
constexpr int kArenaVecBytes0 = 64;
constexpr int kArenaVecBytes1 = 256;
constexpr int kArenaVecBytes2 = 1024;

template <typename T>
struct ArenaVec {
    using Segment = ArenaVecSegment<T>;

    Arena* a = nullptr;
    ArenaPtr<Segment> first = {};
    ArenaPtr<Segment> last = {};
    int len = 0;
#if defined(DEBUG)

    int dbgId = 0;

    int dbgTotalCap = 0;
    int dbgSegs = 0;

    ArenaVec(GPUI_VEC_DBG_ARGS0) noexcept GPUI_VEC_DBG_INIT('A') {}

    ~ArenaVec() { VecDbgArenaDeath(dbgId, len, dbgTotalCap, dbgSegs); }
#endif

    T& operator[](int idx) const {

        Segment* seg = ArenaPtrGet(a, first);
        if (first == last) {
            return seg->Els()[idx];
        }
        while (idx >= seg->base + seg->len) {
            seg = ArenaPtrGet(a, seg->next);
        }
        return seg->Els()[idx - seg->base];
    }

    struct Iter {

        Arena* a;

        Segment* seg;
        int idx;

        void Normalize() {
            while (seg && idx >= seg->len) {
                seg = ArenaPtrGet(a, seg->next);
                idx = 0;
            }
        }
        T& operator*() const { return seg->Els()[idx]; }
        T* operator->() const { return &seg->Els()[idx]; }
        Iter& operator++() {
            idx++;
            if (idx >= seg->len) {
                seg = ArenaPtrGet(a, seg->next);
                idx = 0;
                Normalize();
            }
            return *this;
        }
        bool operator!=(const Iter& o) const {
            return seg != o.seg || idx != o.idx;
        }
    };

    Iter begin() const {
        Iter it = {a, ArenaPtrGet(a, first), 0};
        it.Normalize();
        return it;
    }
    Iter end() const { return Iter{a, nullptr, 0}; }

    bool Append(Arena* arena, const T& el) {
        Segment* seg = ArenaPtrGet(a, last);
        if (!seg || seg->len >= seg->cap) {
            seg = NextSegment(arena, 1);
            if (!seg) {
                return false;
            }
        }
        seg->Els()[seg->len++] = el;
        len++;
        return true;
    }

    bool AppendMany(Arena* arena, const T* src, int n) {
        while (n > 0) {
            Segment* seg = ArenaPtrGet(a, last);
            if (!seg || seg->len >= seg->cap) {
                seg = NextSegment(arena, n);
                if (!seg) {
                    return false;
                }
            }
            int room = seg->cap - seg->len;
            int take = n < room ? n : room;
            for (int i = 0; i < take; i++) {
                seg->Els()[seg->len + i] = src[i];
            }
            seg->len += take;
            len += take;
            src += take;
            n -= take;
        }
        return true;
    }

    bool Reserve(Arena* arena, int n) {
        Segment* seg = ArenaPtrGet(a, last);
        if (seg && seg->cap - seg->len >= n) {
            return true;
        }
        return NextSegment(arena, n) != nullptr;
    }

    void Truncate(int newLen) {
        if (newLen < 0) {
            newLen = 0;
        }
        if (newLen >= len) {
            return;
        }
        ArenaPtr<Segment> at = first;
        Segment* seg = ArenaPtrGet(a, at);
        while (seg && seg->base + seg->len <= newLen) {
            at = seg->next;
            seg = ArenaPtrGet(a, at);
        }
        if (seg) {
            seg->len = newLen - seg->base;
            last = at;
            for (Segment* s = ArenaPtrGet(a, seg->next); s;
                 s = ArenaPtrGet(a, s->next)) {
                s->len = 0;
            }
        }
        len = newLen;
    }

    void Pop() { Truncate(len - 1); }

    T* Flatten(Arena* into) const {
        if (len == 0) {
            return nullptr;
        }
        if (first == last) {
            return ArenaPtrGet(a, first)->Els();
        }
        T* out = (T*)ArenaVecAlloc(into, len, (int)sizeof(T), (int)alignof(T));
        if (!out) {
            return nullptr;
        }
        int at = 0;
        for (const T& el : *this) {
            out[at++] = el;
        }
        return out;
    }

    static constexpr int CapFor(int count, int bytes) {
        return bytes / (int)sizeof(T) < count
                   ? (bytes / (int)sizeof(T) > 0 ? bytes / (int)sizeof(T) : 1)
                   : count;
    }

    static int NextCap(int prevCap) {
        if (prevCap < CapFor(kArenaVecCap0, kArenaVecBytes0)) {
            return CapFor(kArenaVecCap0, kArenaVecBytes0);
        }
        if (prevCap < CapFor(kArenaVecCap1, kArenaVecBytes1)) {
            return CapFor(kArenaVecCap1, kArenaVecBytes1);
        }
        if (prevCap < CapFor(kArenaVecCap2, kArenaVecBytes2)) {
            return CapFor(kArenaVecCap2, kArenaVecBytes2);
        }
        return prevCap * 2;
    }

    GPUI_NOINLINE Segment* NextSegment(Arena* arena, int want) {
        a = arena;
#if defined(DEBUG)
        if (dbgId == 0) {
            dbgId = VecDbgBirth("<no-constructor>", 0, "", 'A', (int)sizeof(T));
        }
#endif
        Segment* lastSeg = ArenaPtrGet(a, last);
#if defined(DEBUG)
        int dbgPrevCap = lastSeg ? lastSeg->cap : 0;
#endif
        ArenaPtr<Segment> reuseAt =
            lastSeg ? lastSeg->next : ArenaPtr<Segment>{};
        Segment* reuse = ArenaPtrGet(a, reuseAt);
        if (reuse && reuse->cap >= want) {
            reuse->len = 0;
            reuse->base = len;
            last = reuseAt;
#if defined(DEBUG)
            VecDbgSegment(dbgId, len, want, dbgPrevCap, reuse->cap, dbgTotalCap,
                          true);
#endif
            return reuse;
        }
        int cap = NextCap(lastSeg ? lastSeg->cap : 0);
        if (cap < want) {
            cap = want;
        }
        int align = (int)alignof(T) > 8 ? (int)alignof(T) : 8;
        void* mem =
            ArenaVecAlloc(a, cap, (int)sizeof(T), align, Segment::HeaderSize());
        if (!mem) {
            return nullptr;
        }
        Segment* seg = (Segment*)mem;
        seg->next = {};
        seg->base = len;
        seg->len = 0;
        seg->cap = cap;
        ArenaPtr<Segment> at = ArenaPtrOf(a, seg);
        if (lastSeg) {
            lastSeg->next = at;
        } else {
            first = at;
        }
        last = at;
#if defined(DEBUG)
        dbgTotalCap += cap;
        dbgSegs++;
        VecDbgSegment(dbgId, len, want, dbgPrevCap, cap, dbgTotalCap, false);
#endif
        return seg;
    }
};

struct PointF {
    float x = 0.0f;
    float y = 0.0f;

    static constexpr PointF Zero() { return {0.0f, 0.0f}; }
};

constexpr bool operator==(PointF a, PointF b) {
    return a.x == b.x && a.y == b.y;
}

constexpr bool operator!=(PointF a, PointF b) {
    return !(a == b);
}

constexpr PointF operator+(PointF a, PointF b) {
    return {a.x + b.x, a.y + b.y};
}

struct SizeF {
    float w = 0.0f;
    float h = 0.0f;

    static constexpr SizeF Zero() { return {0.0f, 0.0f}; }
};

constexpr bool operator==(SizeF a, SizeF b) {
    return a.w == b.w && a.h == b.h;
}

constexpr bool operator!=(SizeF a, SizeF b) {
    return !(a == b);
}

constexpr SizeF operator+(SizeF a, SizeF b) {
    return {a.w + b.w, a.h + b.h};
}

constexpr SizeF operator-(SizeF a, SizeF b) {
    return {a.w - b.w, a.h - b.h};
}

struct RectF {
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;

    static constexpr RectF Zero() { return {0.0f, 0.0f, 0.0f, 0.0f}; }
    static constexpr RectF New(float l, float r, float t, float b) {
        return {l, r, t, b};
    }

    constexpr float HorizontalAxisSum() const { return left + right; }
    constexpr float VerticalAxisSum() const { return top + bottom; }
    constexpr SizeF SumAxes() const { return {left + right, top + bottom}; }
};

constexpr bool operator==(RectF a, RectF b) {
    return a.left == b.left && a.right == b.right && a.top == b.top &&
           a.bottom == b.bottom;
}

constexpr bool operator!=(RectF a, RectF b) {
    return !(a == b);
}

constexpr RectF operator+(RectF a, RectF b) {
    return {a.left + b.left, a.right + b.right, a.top + b.top,
            a.bottom + b.bottom};
}

struct LocalDate {
    int year = 0;
    int month = 0;
    int day = 0;
};

LocalDate DateToday();

LocalDate DateAddDays(LocalDate base, int days);

void StrFree(Str s);
void StrFree(const char*) = delete;

Str StrDup(Arena*, Str str);
Str StrDup(Str s);

void StrDup2(Str s1, Str s2, Str& s1Out, Str& s2Out);
void StrFree2(Str s);

GPUI_NOINLINE bool StrEqRest(Str s1, Str s2);
inline bool StrEq(Str s1, Str s2) {
    if (s1.len != s2.len) {
        return false;
    }
    return StrEqRest(s1, s2);
}
bool StrEq(Str s1, const char* s2);
int StrCmp(Str s1, Str s2);
GPUI_NOINLINE bool StrEqIRest(Str s1, Str s2);
inline bool StrEqI(Str s1, Str s2) {
    if (s1.len != s2.len) {
        return false;
    }
    return StrEqIRest(s1, s2);
}
bool StrEqI(Str s1, const char* s2);
bool StrStartsWith(Str s, Str prefix);
bool StrStartsWith(Str s, const char* prefix);
bool StrStartsWithAny(Str s, const char* chars);
inline bool StrStartsWithI(Str s, Str prefix) {
    if (prefix.len > s.len) {
        return false;
    }
    return StrEqI(Str(s.s, prefix.len), prefix);
}
bool StrStartsWithI(Str s, const char* prefix);
bool StrEndsWith(Str s, Str suffix);
bool StrEndsWith(Str s, const char* suffix);
bool StrEndsWithI(Str s, Str suffix);
bool StrEndsWithI(Str s, const char* suffix);
int StrFind(Str s, Str sub);
int StrFind(Str s, const char* sub);
int StrFindI(Str s, Str sub);
int StrFindI(Str s, const char* sub);
bool StrContains(Str s, Str sub);
bool StrContainsI(Str s, Str sub);

Str StrTrimAscii(Str s);

Str StrReplaceAll(Str value, Str from, Str to);

using SeqStrings = const char*;

Str SeqStrFirst(SeqStrings strs);

Str SeqStrNext(Str s);

int SeqStrIndex(SeqStrings strs, Str toFind);
int SeqStrIndexIS(SeqStrings strs, Str toFind);

bool SeqStrContainsI(SeqStrings strs, Str toFind);

Str SeqStrByIndex(SeqStrings strs, int idx);

int SeqStrCount(SeqStrings strs);

void StrLowerAscii(char* s);

struct StrBuilder : Vec<char> {
    Arena* a = nullptr;

    explicit StrBuilder(Arena* arena = nullptr) : a(arena) {}

    void Reset(Str s = {});
    bool Reserve(int cap);
    bool AppendChar(char c);
    bool Append(Str src);
    char RemoveAt(int idx, int count = 1);
    char RemoveLast();
    Str TakeStr();
    char LastChar() const;
};

void StrBuilderUseExternalBuffer(StrBuilder& b, Str buf);

struct FmtArg {
    enum class Kind : uint8_t {
        Char,
        Int,
        Ptr,
        Float,
        Double,
        Str,
        RawStr,
        Any,
        None,
    };

    Kind t{Kind::None};
    union {
        Str str;
        char c;
        int64_t i;
        float f;
        double d;
        const void* ptr;
    };

    FmtArg() : i{0} {}
    explicit FmtArg(char c_) : t{Kind::Char}, c{c_} {}
    explicit FmtArg(int arg) : t{Kind::Int}, i{(int64_t)arg} {}
    explicit FmtArg(unsigned int arg) : t{Kind::Int}, i{(int64_t)arg} {}
    explicit FmtArg(long arg) : t{Kind::Int}, i{(int64_t)arg} {}
    explicit FmtArg(unsigned long arg) : t{Kind::Int}, i{(int64_t)arg} {}
    explicit FmtArg(long long arg) : t{Kind::Int}, i{(int64_t)arg} {}
    explicit FmtArg(unsigned long long arg) : t{Kind::Int}, i{(int64_t)arg} {}
    explicit FmtArg(float f_) : t{Kind::Float}, f{f_} {}
    explicit FmtArg(double d_) : t{Kind::Double}, d{d_} {}
    explicit FmtArg(Str arg) : t{Kind::Str}, str{arg} {}
    explicit FmtArg(const void* p) : t{Kind::Ptr}, ptr{p} {}
    FmtArg(char*) = delete;
    FmtArg(const char*) = delete;
    FmtArg(wchar_t*) = delete;
    FmtArg(const wchar_t*) = delete;
};

TempStr FormatTempArgs(const char* fmt, const FmtArg** args, int nArgs);

inline TempStr FormatTemp(const char* fmt) {
    return FormatTempArgs(fmt, nullptr, 0);
}

template <typename... TArgs>
TempStr FormatTemp(const char* fmt, const TArgs&... args) {
    const FmtArg argv[] = {FmtArg(args)...};
    const FmtArg* argp[sizeof...(TArgs)];
    int n = (int)sizeof...(TArgs);
    for (int i = 0; i < n; i++) {
        argp[i] = &argv[i];
    }
    return FormatTempArgs(fmt, argp, n);
}

template <typename... TArgs>
inline TempStr fmt(const char* format, const TArgs&... args) {
    return FormatTemp(format, args...);
}

template <typename... TArgs>
inline void logf(const char* format, const TArgs&... args) {
    log(FormatTemp(format, args...));
}
}

#endif

#line 1 "src/taffy/geometry.h"

#include <cmath>
#include <cfloat>

namespace taffy {

using base::Arena;
using base::PointF;
using base::RectF;
using base::SizeF;
using base::Str;
using base::Vec;

using Optf = float;

inline constexpr uint32_t kOptfNoneBits = 0x7fc0beefu;

constexpr Optf None() {
    return __builtin_bit_cast(float, kOptfNoneBits);
}

constexpr Optf Some(float v) {
    return v;
}

constexpr bool IsNone(Optf v) {
    return __builtin_bit_cast(uint32_t, v) == kOptfNoneBits;
}

constexpr bool IsSome(Optf v) {
    return __builtin_bit_cast(uint32_t, v) != kOptfNoneBits;
}

constexpr float Unwrap(Optf v) {
    return v;
}

constexpr float UnwrapOr(Optf v, float alt) {
    return IsSome(v) ? v : alt;
}

constexpr Optf Or(Optf v, Optf alt) {
    return IsSome(v) ? v : alt;
}

constexpr bool OptfEq(Optf a, Optf b) {
    return IsNone(a) ? IsNone(b) : (a == b);
}

constexpr bool OptfNe(Optf a, Optf b) {
    return !OptfEq(a, b);
}

inline bool F32IsNan(float v) {
    return (__builtin_bit_cast(uint32_t, v) & 0x7fffffffu) > 0x7f800000u;
}

inline float F32Max(float a, float b) {
    if (F32IsNan(a)) {
        return b;
    }
    if (F32IsNan(b)) {
        return a;
    }
    return a > b ? a : b;
}

inline float F32Min(float a, float b) {
    if (F32IsNan(a)) {
        return b;
    }
    if (F32IsNan(b)) {
        return a;
    }
    return a < b ? a : b;
}

inline float F32Round(float v) {
    return floorf(v + 0.5f);
}

enum class AbsoluteAxis : uint8_t {
    Horizontal,
    Vertical
};

constexpr AbsoluteAxis OtherAxis(AbsoluteAxis axis) {
    return axis == AbsoluteAxis::Horizontal ? AbsoluteAxis::Vertical
                                            : AbsoluteAxis::Horizontal;
}

enum class AbstractAxis : uint8_t {
    Inline,
    Block
};

constexpr AbstractAxis Other(AbstractAxis axis) {
    return axis == AbstractAxis::Inline ? AbstractAxis::Block
                                        : AbstractAxis::Inline;
}

constexpr AbsoluteAxis AsAbsNaive(AbstractAxis axis) {
    return axis == AbstractAxis::Inline ? AbsoluteAxis::Horizontal
                                        : AbsoluteAxis::Vertical;
}

enum class FlexDirection : uint8_t {
    Row,
    Column,
    RowReverse,
    ColumnReverse
};

constexpr bool IsRow(FlexDirection d) {
    return d == FlexDirection::Row || d == FlexDirection::RowReverse;
}

constexpr bool IsColumn(FlexDirection d) {
    return d == FlexDirection::Column || d == FlexDirection::ColumnReverse;
}

constexpr bool IsReverse(FlexDirection d) {
    return d == FlexDirection::RowReverse || d == FlexDirection::ColumnReverse;
}

constexpr AbsoluteAxis MainAxis(FlexDirection d) {
    return IsRow(d) ? AbsoluteAxis::Horizontal : AbsoluteAxis::Vertical;
}

constexpr AbsoluteAxis CrossAxis(FlexDirection d) {
    return IsRow(d) ? AbsoluteAxis::Vertical : AbsoluteAxis::Horizontal;
}

constexpr float GetAbs(SizeF s, AbsoluteAxis a) {
    return a == AbsoluteAxis::Horizontal ? s.w : s.h;
}
constexpr float Get(SizeF s, AbstractAxis a) {
    return a == AbstractAxis::Inline ? s.w : s.h;
}
constexpr void Set(SizeF* s, AbstractAxis a, float v) {
    if (a == AbstractAxis::Inline) {
        s->w = v;
    } else {
        s->h = v;
    }
}
constexpr float Main(SizeF s, FlexDirection d) {
    return IsRow(d) ? s.w : s.h;
}
constexpr float Cross(SizeF s, FlexDirection d) {
    return IsRow(d) ? s.h : s.w;
}
constexpr void SetMain(SizeF* s, FlexDirection d, float v) {
    if (IsRow(d)) {
        s->w = v;
    } else {
        s->h = v;
    }
}
constexpr void SetCross(SizeF* s, FlexDirection d, float v) {
    if (IsRow(d)) {
        s->h = v;
    } else {
        s->w = v;
    }
}
constexpr SizeF WithMain(SizeF s, FlexDirection d, float v) {
    SetMain(&s, d, v);
    return s;
}
constexpr SizeF WithCross(SizeF s, FlexDirection d, float v) {
    SetCross(&s, d, v);
    return s;
}

inline SizeF Max(SizeF a, SizeF b) {
    return {F32Max(a.w, b.w), F32Max(a.h, b.h)};
}
inline SizeF Min(SizeF a, SizeF b) {
    return {F32Min(a.w, b.w), F32Min(a.h, b.h)};
}
constexpr bool HasNonZeroArea(SizeF s) {
    return s.w > 0.0f && s.h > 0.0f;
}

constexpr float Get(PointF p, AbstractAxis a) {
    return a == AbstractAxis::Inline ? p.x : p.y;
}
constexpr void Set(PointF* p, AbstractAxis a, float v) {
    if (a == AbstractAxis::Inline) {
        p->x = v;
    } else {
        p->y = v;
    }
}
constexpr float Main(PointF p, FlexDirection d) {
    return IsRow(d) ? p.x : p.y;
}
constexpr float Cross(PointF p, FlexDirection d) {
    return IsRow(d) ? p.y : p.x;
}
constexpr PointF Transpose(PointF p) {
    return {p.y, p.x};
}
constexpr SizeF IntoSize(PointF p) {
    return {p.x, p.y};
}

constexpr float GridAxisSum(RectF r, AbsoluteAxis a) {
    return a == AbsoluteAxis::Horizontal ? r.left + r.right : r.top + r.bottom;
}
constexpr float MainAxisSum(RectF r, FlexDirection d) {
    return IsRow(d) ? r.left + r.right : r.top + r.bottom;
}
constexpr float CrossAxisSum(RectF r, FlexDirection d) {
    return IsRow(d) ? r.top + r.bottom : r.left + r.right;
}
constexpr float MainStart(RectF r, FlexDirection d) {
    return IsRow(d) ? r.left : r.top;
}
constexpr float MainEnd(RectF r, FlexDirection d) {
    return IsRow(d) ? r.right : r.bottom;
}
constexpr float CrossStart(RectF r, FlexDirection d) {
    return IsRow(d) ? r.top : r.left;
}
constexpr float CrossEnd(RectF r, FlexDirection d) {
    return IsRow(d) ? r.bottom : r.right;
}

using SizeFOpt = SizeF;
using PointFOpt = PointF;
using RectFOpt = RectF;

constexpr SizeFOpt SizeFOptNone() {
    return {None(), None()};
}

constexpr PointFOpt PointFOptNone() {
    return {None(), None()};
}

constexpr RectFOpt RectFOptNone() {
    return {None(), None(), None(), None()};
}

constexpr SizeFOpt SizeFOptFromCross(FlexDirection d, Optf v) {
    return IsRow(d) ? SizeFOpt{None(), v} : SizeFOpt{v, None()};
}

constexpr SizeF UnwrapOr(SizeFOpt s, SizeF alt) {
    return {UnwrapOr(s.w, alt.w), UnwrapOr(s.h, alt.h)};
}

constexpr SizeFOpt Or(SizeFOpt s, SizeFOpt alt) {
    return {Or(s.w, alt.w), Or(s.h, alt.h)};
}

constexpr bool BothAxisDefined(SizeFOpt s) {
    return IsSome(s.w) && IsSome(s.h);
}

constexpr SizeFOpt MaybeApplyAspectRatio(SizeFOpt s, Optf aspectRatio) {
    if (IsNone(aspectRatio)) {
        return s;
    }
    if (IsSome(s.w) && IsNone(s.h)) {
        return {s.w, s.w / aspectRatio};
    }
    if (IsNone(s.w) && IsSome(s.h)) {
        return {s.h * aspectRatio, s.h};
    }
    return s;
}

constexpr bool SizeFOptEq(SizeFOpt a, SizeFOpt b) {
    return OptfEq(a.w, b.w) && OptfEq(a.h, b.h);
}

struct LineF {
    float start = 0.0f;
    float end = 0.0f;

    constexpr float Sum() const { return start + end; }
};

struct LineBool {
    bool start = false;
    bool end = false;

    static constexpr LineBool True() { return {true, true}; }
    static constexpr LineBool False() { return {false, false}; }
};

template <typename T>
struct Slice {
    T* els = nullptr;
    int len = 0;

    T& operator[](int i) const { return els[i]; }
    bool IsEmpty() const { return len == 0; }
    T* begin() const { return els; }
    T* end() const { return els + len; }
};

template <typename T>
Slice<T> SliceNew(Arena* a, int n) {
    if (n <= 0) {
        return {};
    }
    return {(T*)a->Alloc((int)sizeof(T) * n), n};
}

template <typename T>
Slice<T> SliceDup(Arena* a, const T* src, int n) {
    Slice<T> out = SliceNew<T>(a, n);
    if (out.els && src) {
        memcpy((void*)out.els, (const void*)src, sizeof(T) * (size_t)n);
    }
    return out;
}

template <typename T>
Slice<T> SliceOne(Arena* a, const T& v) {
    Slice<T> out = SliceNew<T>(a, 1);
    if (out.els) {
        out.els[0] = v;
    }
    return out;
}

}

#line 1 "src/taffy/style.h"

namespace taffy {

struct CompactLength {
    uint64_t bits = 0;

    static constexpr uint64_t kCalcTag = 0;
    static constexpr uint64_t kLengthTag = 0x01;
    static constexpr uint64_t kPercentTag = 0x02;
    static constexpr uint64_t kAutoTag = 0x03;
    static constexpr uint64_t kFrTag = 0x04;
    static constexpr uint64_t kMinContentTag = 0x07;
    static constexpr uint64_t kMaxContentTag = 0x0f;
    static constexpr uint64_t kFitContentPxTag = 0x17;
    static constexpr uint64_t kFitContentPercentTag = 0x1f;

    static constexpr uint64_t kTagMask = 0xff;
    static constexpr uint64_t kCalcTagMask = 0x07;

    uint64_t Tag() const { return bits & kTagMask; }

    float Value() const {
        return __builtin_bit_cast(float, (uint32_t)(bits >> 32));
    }
    const void* CalcValue() const { return (const void*)(uintptr_t)bits; }

    bool IsCalc() const { return (bits & kCalcTagMask) == 0; }
    bool IsZero() const { return bits == FromVal(0.0f, kLengthTag).bits; }
    bool IsLengthOrPercentage() const {
        uint64_t t = Tag();
        return t == kLengthTag || t == kPercentTag;
    }
    bool IsAuto() const { return Tag() == kAutoTag; }
    bool IsMinContent() const { return Tag() == kMinContentTag; }
    bool IsMaxContent() const { return Tag() == kMaxContentTag; }
    bool IsFitContent() const {
        uint64_t t = Tag();
        return t == kFitContentPxTag || t == kFitContentPercentTag;
    }
    bool IsMaxOrFitContent() const {
        uint64_t t = Tag();
        return t == kMaxContentTag || t == kFitContentPxTag ||
               t == kFitContentPercentTag;
    }

    bool IsMaxContentAlike() const {
        uint64_t t = Tag();
        return t == kAutoTag || t == kMaxContentTag || t == kFitContentPxTag ||
               t == kFitContentPercentTag;
    }
    bool IsMinOrMaxContent() const {
        uint64_t t = Tag();
        return t == kMinContentTag || t == kMaxContentTag;
    }
    bool IsIntrinsic() const {
        uint64_t t = Tag();
        return t == kAutoTag || t == kMinContentTag || t == kMaxContentTag ||
               t == kFitContentPxTag || t == kFitContentPercentTag;
    }
    bool IsFr() const { return Tag() == kFrTag; }

    bool UsesPercentage() const {
        uint64_t t = Tag();
        return t == kPercentTag || t == kFitContentPercentTag || IsCalc();
    }

    static CompactLength FromVal(float v, uint64_t tag) {
        return CompactLength{((uint64_t)__builtin_bit_cast(uint32_t, v) << 32) |
                             tag};
    }
    static CompactLength FromTag(uint64_t tag) { return CompactLength{tag}; }
    static CompactLength FromPtr(const void* p) {
        return CompactLength{(uint64_t)(uintptr_t)p};
    }

    static CompactLength Length(float v) { return FromVal(v, kLengthTag); }

    static CompactLength Percent(float v) { return FromVal(v, kPercentTag); }
    static CompactLength Auto() { return FromTag(kAutoTag); }
    static CompactLength Fr(float v) { return FromVal(v, kFrTag); }
    static CompactLength MinContent() { return FromTag(kMinContentTag); }
    static CompactLength MaxContent() { return FromTag(kMaxContentTag); }
    static CompactLength FitContentPx(float limit) {
        return FromVal(limit, kFitContentPxTag);
    }
    static CompactLength FitContentPercent(float limit) {
        return FromVal(limit, kFitContentPercentTag);
    }
    static CompactLength Zero() { return Length(0.0f); }

    static CompactLength Calc(const void* p) { return FromPtr(p); }
};

inline bool operator==(CompactLength a, CompactLength b) {
    return a.bits == b.bits;
}

inline bool operator!=(CompactLength a, CompactLength b) {
    return a.bits != b.bits;
}

using CalcResolverFn = float (*)(const void* handle, float parentSize,
                                 void* ctx);

struct CalcResolver {
    CalcResolverFn fn = nullptr;
    void* ctx = nullptr;

    float Resolve(const void* handle, float parentSize) const {
        return fn ? fn(handle, parentSize, ctx) : 0.0f;
    }
};

inline Optf ResolvedPercentageSize(CompactLength cl, float parentSize,
                                   CalcResolver calc) {
    if (cl.Tag() == CompactLength::kPercentTag) {
        return Some(cl.Value() * parentSize);
    }
    if (cl.IsCalc()) {
        return Some(calc.Resolve(cl.CalcValue(), parentSize));
    }
    return None();
}

struct LengthPercentage {
    CompactLength raw = CompactLength::Zero();

    static LengthPercentage Length(float v) {
        return {CompactLength::Length(v)};
    }
    static LengthPercentage Percent(float v) {
        return {CompactLength::Percent(v)};
    }
    static LengthPercentage Calc(const void* p) {
        return {CompactLength::Calc(p)};
    }
    static LengthPercentage Zero() { return {CompactLength::Zero()}; }
};

struct LengthPercentageAuto {
    CompactLength raw = CompactLength::Zero();

    static LengthPercentageAuto Length(float v) {
        return {CompactLength::Length(v)};
    }
    static LengthPercentageAuto Percent(float v) {
        return {CompactLength::Percent(v)};
    }
    static LengthPercentageAuto Auto() { return {CompactLength::Auto()}; }
    static LengthPercentageAuto Calc(const void* p) {
        return {CompactLength::Calc(p)};
    }
    static LengthPercentageAuto Zero() { return {CompactLength::Zero()}; }
    static LengthPercentageAuto From(LengthPercentage lp) { return {lp.raw}; }

    bool IsAuto() const { return raw.IsAuto(); }

    Optf ResolveToOption(float context, CalcResolver calc) const;

    Optf MaybeResolve(Optf context, CalcResolver calc) const;
};

struct Dimension {
    CompactLength raw = CompactLength::Zero();

    static Dimension Length(float v) { return {CompactLength::Length(v)}; }
    static Dimension Percent(float v) { return {CompactLength::Percent(v)}; }
    static Dimension Auto() { return {CompactLength::Auto()}; }
    static Dimension Calc(const void* p) { return {CompactLength::Calc(p)}; }
    static Dimension Zero() { return {CompactLength::Zero()}; }
    static Dimension From(LengthPercentage lp) { return {lp.raw}; }
    static Dimension From(LengthPercentageAuto lp) { return {lp.raw}; }

    bool IsAuto() const { return raw.IsAuto(); }
    uint64_t Tag() const { return raw.Tag(); }
    float Value() const { return raw.Value(); }

    Optf MaybeResolve(Optf context, CalcResolver calc) const;

    Optf IntoOption() const {
        return raw.Tag() == CompactLength::kLengthTag ? Some(raw.Value())
                                                      : None();
    }
};

inline bool operator==(LengthPercentage a, LengthPercentage b) {
    return a.raw == b.raw;
}
inline bool operator!=(LengthPercentage a, LengthPercentage b) {
    return a.raw != b.raw;
}
inline bool operator==(LengthPercentageAuto a, LengthPercentageAuto b) {
    return a.raw == b.raw;
}
inline bool operator!=(LengthPercentageAuto a, LengthPercentageAuto b) {
    return a.raw != b.raw;
}
inline bool operator==(Dimension a, Dimension b) {
    return a.raw == b.raw;
}
inline bool operator!=(Dimension a, Dimension b) {
    return a.raw != b.raw;
}

struct AvailableSpace {
    enum class Kind : uint8_t {
        Definite,
        MinContent,
        MaxContent
    };

    Kind kind = Kind::MaxContent;
    float value = 0.0f;

    static AvailableSpace Definite(float v) { return {Kind::Definite, v}; }
    static AvailableSpace MinContent() { return {Kind::MinContent, 0.0f}; }
    static AvailableSpace MaxContent() { return {Kind::MaxContent, 0.0f}; }
    static AvailableSpace Zero() { return Definite(0.0f); }

    static AvailableSpace From(Optf v) {
        return IsSome(v) ? Definite(v) : MaxContent();
    }

    bool IsDefinite() const { return kind == Kind::Definite; }
    Optf IntoOption() const {
        return kind == Kind::Definite ? Some(value) : None();
    }
    float UnwrapOr(float def) const {
        return kind == Kind::Definite ? value : def;
    }

    float Unwrap() const { return value; }
    AvailableSpace Or(AvailableSpace def) const {
        return kind == Kind::Definite ? *this : def;
    }
    AvailableSpace MaybeSet(Optf v) const {
        return IsSome(v) ? Definite(v) : *this;
    }
    float ComputeFreeSpace(float usedSpace) const {
        switch (kind) {
            case Kind::MaxContent:
                return INFINITY;
            case Kind::MinContent:
                return 0.0f;
            default:
                return value - usedSpace;
        }
    }

    bool IsRoughlyEqual(AvailableSpace other) const {
        if (kind != other.kind) {
            return false;
        }
        if (kind != Kind::Definite) {
            return true;
        }
        return fabsf(value - other.value) < FLT_EPSILON;
    }
};

inline bool operator==(AvailableSpace a, AvailableSpace b) {
    return a.kind == b.kind &&
           (a.kind != AvailableSpace::Kind::Definite || a.value == b.value);
}

inline bool operator!=(AvailableSpace a, AvailableSpace b) {
    return !(a == b);
}

enum class AlignItemsKeyword : uint8_t {
    Start,
    End,
    FlexStart,
    FlexEnd,
    SelfStart,
    SelfEnd,
    Center,
    Baseline,
    Stretch
};

enum class AlignContentKeyword : uint8_t {
    Start,
    End,
    FlexStart,
    FlexEnd,
    Center,
    Stretch,
    SpaceBetween,
    SpaceEvenly,
    SpaceAround
};

constexpr AlignContentKeyword Reversed(AlignContentKeyword k) {
    switch (k) {
        case AlignContentKeyword::Start:
            return AlignContentKeyword::End;
        case AlignContentKeyword::End:
            return AlignContentKeyword::Start;
        case AlignContentKeyword::FlexStart:
            return AlignContentKeyword::FlexEnd;
        case AlignContentKeyword::FlexEnd:
            return AlignContentKeyword::FlexStart;
        case AlignContentKeyword::Stretch:
            return AlignContentKeyword::End;
        default:
            return k;
    }
}

enum class AlignmentSafety : uint8_t {
    Unsafe,
    Safe
};

struct AlignItems {
    AlignItemsKeyword keyword = AlignItemsKeyword::Start;
    AlignmentSafety safety = AlignmentSafety::Unsafe;

    constexpr bool IsSafe() const { return safety == AlignmentSafety::Safe; }
    constexpr AlignItemsKeyword Keyword() const { return keyword; }
};

struct AlignContent {
    AlignContentKeyword keyword = AlignContentKeyword::Start;
    AlignmentSafety safety = AlignmentSafety::Unsafe;

    constexpr bool IsSafe() const { return safety == AlignmentSafety::Safe; }
    constexpr AlignContentKeyword Keyword() const { return keyword; }
};

constexpr bool operator==(AlignItems a, AlignItems b) {
    return a.keyword == b.keyword && a.safety == b.safety;
}
constexpr bool operator!=(AlignItems a, AlignItems b) {
    return !(a == b);
}
constexpr bool operator==(AlignContent a, AlignContent b) {
    return a.keyword == b.keyword && a.safety == b.safety;
}
constexpr bool operator!=(AlignContent a, AlignContent b) {
    return !(a == b);
}

using JustifyItems = AlignItems;
using AlignSelf = AlignItems;
using JustifySelf = AlignItems;
using JustifyContent = AlignContent;

struct OptAlignItems {
    AlignItems val;
    bool has = false;

    constexpr OptAlignItems() = default;
    constexpr explicit OptAlignItems(AlignItems v) : val(v), has(true) {}

    constexpr bool IsSome() const { return has; }
    constexpr AlignItems UnwrapOr(AlignItems alt) const {
        return has ? val : alt;
    }
    constexpr OptAlignItems Or(OptAlignItems alt) const {
        return has ? *this : alt;
    }
};

using OptAlignSelf = OptAlignItems;

struct OptAlignContent {
    AlignContent val;
    bool has = false;

    constexpr OptAlignContent() = default;
    constexpr explicit OptAlignContent(AlignContent v) : val(v), has(true) {}

    constexpr bool IsSome() const { return has; }
    constexpr AlignContent UnwrapOr(AlignContent alt) const {
        return has ? val : alt;
    }
};

using OptJustifyContent = OptAlignContent;

constexpr bool operator==(OptAlignItems a, OptAlignItems b) {
    return a.has == b.has && (!a.has || a.val == b.val);
}
constexpr bool operator!=(OptAlignItems a, OptAlignItems b) {
    return !(a == b);
}
constexpr bool operator==(OptAlignContent a, OptAlignContent b) {
    return a.has == b.has && (!a.has || a.val == b.val);
}
constexpr bool operator!=(OptAlignContent a, OptAlignContent b) {
    return !(a == b);
}

enum class Display : uint8_t {
    Block,
    FlowRoot,
    Flex,
    Grid,
    None
};

enum class BoxGenerationMode : uint8_t {
    Normal,
    None
};

enum class Position : uint8_t {
    Relative,
    Absolute
};

enum class BoxSizing : uint8_t {
    BorderBox,
    ContentBox
};

enum class Overflow : uint8_t {
    Visible,
    Clip,
    Hidden,
    Scroll
};

constexpr bool IsScrollContainer(Overflow o) {
    return o == Overflow::Hidden || o == Overflow::Scroll;
}

constexpr Optf MaybeIntoAutomaticMinSize(Overflow o) {
    return IsScrollContainer(o) ? Some(0.0f) : None();
}

enum class Direction : uint8_t {
    Ltr,
    Rtl
};

constexpr bool IsRtl(Direction d) {
    return d == Direction::Rtl;
}

constexpr AlignItems ResolveSelfRelative(AlignItems value,
                                         Direction itemDirection,
                                         Direction containerDirection,
                                         bool axisIsInline) {
    bool flip = axisIsInline && itemDirection != containerDirection;
    if (value.keyword == AlignItemsKeyword::SelfStart) {
        value.keyword = flip ? AlignItemsKeyword::End
                             : AlignItemsKeyword::Start;
    } else if (value.keyword == AlignItemsKeyword::SelfEnd) {
        value.keyword = flip ? AlignItemsKeyword::Start
                             : AlignItemsKeyword::End;
    }
    return value;
}

enum class TextAlign : uint8_t {
    Auto,
    LegacyLeft,
    LegacyRight,
    LegacyCenter
};

enum class FlexWrap : uint8_t {
    NoWrap,
    Wrap,
    WrapReverse
};

enum class Float : uint8_t {
    Left,
    Right,
    None
};

enum class FloatDirection : uint8_t {
    Left = 0,
    Right = 1
};

constexpr bool IsFloated(Float f) {
    return f == Float::Left || f == Float::Right;
}

struct OptFloatDirection {
    FloatDirection val = FloatDirection::Left;
    bool has = false;

    constexpr OptFloatDirection() = default;
    constexpr explicit OptFloatDirection(FloatDirection v)
        : val(v), has(true) {}

    constexpr bool IsSome() const { return has; }
};

constexpr OptFloatDirection FloatDir(Float f) {
    switch (f) {
        case Float::Left:
            return OptFloatDirection(FloatDirection::Left);
        case Float::Right:
            return OptFloatDirection(FloatDirection::Right);
        default:
            return OptFloatDirection();
    }
}

enum class Clear : uint8_t {
    Left,
    Right,
    Both,
    None
};

struct SizeDim {
    Dimension width = Dimension::Auto();
    Dimension height = Dimension::Auto();

    static SizeDim Auto() { return {}; }
    static SizeDim FromLengths(float w, float h) {
        return {Dimension::Length(w), Dimension::Length(h)};
    }
    static SizeDim FromPercent(float w, float h) {
        return {Dimension::Percent(w), Dimension::Percent(h)};
    }

    Dimension GetAbs(AbsoluteAxis a) const {
        return a == AbsoluteAxis::Horizontal ? width : height;
    }
    Dimension Get(AbstractAxis a) const {
        return a == AbstractAxis::Inline ? width : height;
    }
    Dimension Main(FlexDirection d) const { return IsRow(d) ? width : height; }
    Dimension Cross(FlexDirection d) const { return IsRow(d) ? height : width; }

    SizeFOpt MaybeResolve(SizeFOpt context, CalcResolver calc) const;
    SizeF ResolveOrZero(SizeFOpt context, CalcResolver calc) const;
};

struct SizeLp {
    LengthPercentage width = LengthPercentage::Zero();
    LengthPercentage height = LengthPercentage::Zero();

    static SizeLp Zero() { return {}; }

    LengthPercentage GetAbs(AbsoluteAxis a) const {
        return a == AbsoluteAxis::Horizontal ? width : height;
    }
    LengthPercentage Get(AbstractAxis a) const {
        return a == AbstractAxis::Inline ? width : height;
    }
    LengthPercentage Main(FlexDirection d) const {
        return IsRow(d) ? width : height;
    }
    LengthPercentage Cross(FlexDirection d) const {
        return IsRow(d) ? height : width;
    }
    SizeF ResolveOrZero(SizeFOpt context, CalcResolver calc) const;
    SizeF ResolveOrZero(Optf context, CalcResolver calc) const;
};

struct SizeAvail {
    AvailableSpace width = AvailableSpace::MaxContent();
    AvailableSpace height = AvailableSpace::MaxContent();

    static SizeAvail MaxContent() {
        return {AvailableSpace::MaxContent(), AvailableSpace::MaxContent()};
    }
    static SizeAvail MinContent() {
        return {AvailableSpace::MinContent(), AvailableSpace::MinContent()};
    }
    static SizeAvail Definite(SizeF s) {
        return {AvailableSpace::Definite(s.w),
                AvailableSpace::Definite(s.h)};
    }
    static SizeAvail From(SizeFOpt s) {
        return {AvailableSpace::From(s.w), AvailableSpace::From(s.h)};
    }

    AvailableSpace GetAbs(AbsoluteAxis a) const {
        return a == AbsoluteAxis::Horizontal ? width : height;
    }
    AvailableSpace Get(AbstractAxis a) const {
        return a == AbstractAxis::Inline ? width : height;
    }
    void Set(AbstractAxis a, AvailableSpace v) {
        if (a == AbstractAxis::Inline) {
            width = v;
        } else {
            height = v;
        }
    }
    AvailableSpace Main(FlexDirection d) const {
        return IsRow(d) ? width : height;
    }
    AvailableSpace Cross(FlexDirection d) const {
        return IsRow(d) ? height : width;
    }
    void SetMain(FlexDirection d, AvailableSpace v) {
        if (IsRow(d)) {
            width = v;
        } else {
            height = v;
        }
    }
    void SetCross(FlexDirection d, AvailableSpace v) {
        if (IsRow(d)) {
            height = v;
        } else {
            width = v;
        }
    }
    SizeFOpt IntoOptions() const {
        return {width.IntoOption(), height.IntoOption()};
    }
    SizeAvail MaybeSet(SizeFOpt v) const {
        return {width.MaybeSet(v.w), height.MaybeSet(v.h)};
    }
};

inline bool operator==(SizeAvail a, SizeAvail b) {
    return a.width == b.width && a.height == b.height;
}

inline bool operator!=(SizeAvail a, SizeAvail b) {
    return !(a == b);
}

struct RectLp {
    LengthPercentage left = LengthPercentage::Zero();
    LengthPercentage right = LengthPercentage::Zero();
    LengthPercentage top = LengthPercentage::Zero();
    LengthPercentage bottom = LengthPercentage::Zero();

    static RectLp Zero() { return {}; }

    RectF ResolveOrZero(SizeFOpt context, CalcResolver calc) const;
    RectF ResolveOrZero(Optf context, CalcResolver calc) const;
};

struct RectLpa {
    LengthPercentageAuto left = LengthPercentageAuto::Zero();
    LengthPercentageAuto right = LengthPercentageAuto::Zero();
    LengthPercentageAuto top = LengthPercentageAuto::Zero();
    LengthPercentageAuto bottom = LengthPercentageAuto::Zero();

    static RectLpa Zero() { return {}; }
    static RectLpa Auto() {
        return {LengthPercentageAuto::Auto(), LengthPercentageAuto::Auto(),
                LengthPercentageAuto::Auto(), LengthPercentageAuto::Auto()};
    }

    LengthPercentageAuto MainStart(FlexDirection d) const {
        return IsRow(d) ? left : top;
    }
    LengthPercentageAuto MainEnd(FlexDirection d) const {
        return IsRow(d) ? right : bottom;
    }
    LengthPercentageAuto CrossStart(FlexDirection d) const {
        return IsRow(d) ? top : left;
    }
    LengthPercentageAuto CrossEnd(FlexDirection d) const {
        return IsRow(d) ? bottom : right;
    }
    RectF ResolveOrZero(SizeFOpt context, CalcResolver calc) const;
    RectF ResolveOrZero(Optf context, CalcResolver calc) const;

    RectFOpt MaybeResolve(Optf context, CalcResolver calc) const;

    RectFOpt MaybeResolveZip(SizeFOpt context, CalcResolver calc) const;
};

struct PointOverflow {
    Overflow x = Overflow::Visible;
    Overflow y = Overflow::Visible;

    Overflow Get(AbstractAxis a) const {
        return a == AbstractAxis::Inline ? x : y;
    }
    Overflow Main(FlexDirection d) const { return IsRow(d) ? x : y; }
    Overflow Cross(FlexDirection d) const { return IsRow(d) ? y : x; }
    PointOverflow Transpose() const { return {y, x}; }
};

inline bool operator==(SizeDim a, SizeDim b) {
    return a.width == b.width && a.height == b.height;
}
inline bool operator!=(SizeDim a, SizeDim b) {
    return !(a == b);
}
inline bool operator==(SizeLp a, SizeLp b) {
    return a.width == b.width && a.height == b.height;
}
inline bool operator!=(SizeLp a, SizeLp b) {
    return !(a == b);
}
inline bool operator==(RectLp a, RectLp b) {
    return a.left == b.left && a.right == b.right && a.top == b.top &&
           a.bottom == b.bottom;
}
inline bool operator!=(RectLp a, RectLp b) {
    return !(a == b);
}
inline bool operator==(RectLpa a, RectLpa b) {
    return a.left == b.left && a.right == b.right && a.top == b.top &&
           a.bottom == b.bottom;
}
inline bool operator!=(RectLpa a, RectLpa b) {
    return !(a == b);
}
inline bool operator==(PointOverflow a, PointOverflow b) {
    return a.x == b.x && a.y == b.y;
}
inline bool operator!=(PointOverflow a, PointOverflow b) {
    return !(a == b);
}

struct GridLine {
    int16_t v = 0;

    int16_t AsI16() const { return v; }
};

struct OriginZeroLine {
    int16_t v = 0;
};

constexpr bool operator==(GridLine a, GridLine b) {
    return a.v == b.v;
}
constexpr bool operator!=(GridLine a, GridLine b) {
    return a.v != b.v;
}
constexpr bool operator==(OriginZeroLine a, OriginZeroLine b) {
    return a.v == b.v;
}
constexpr bool operator!=(OriginZeroLine a, OriginZeroLine b) {
    return a.v != b.v;
}
constexpr bool operator<(OriginZeroLine a, OriginZeroLine b) {
    return a.v < b.v;
}
constexpr bool operator>(OriginZeroLine a, OriginZeroLine b) {
    return a.v > b.v;
}
constexpr bool operator<=(OriginZeroLine a, OriginZeroLine b) {
    return a.v <= b.v;
}
constexpr bool operator>=(OriginZeroLine a, OriginZeroLine b) {
    return a.v >= b.v;
}
constexpr OriginZeroLine operator+(OriginZeroLine a, OriginZeroLine b) {
    return {(int16_t)(a.v + b.v)};
}
constexpr OriginZeroLine operator-(OriginZeroLine a, OriginZeroLine b) {
    return {(int16_t)(a.v - b.v)};
}
constexpr OriginZeroLine operator+(OriginZeroLine a, uint16_t b) {
    return {(int16_t)(a.v + (int16_t)b)};
}
constexpr OriginZeroLine operator-(OriginZeroLine a, uint16_t b) {
    return {(int16_t)(a.v - (int16_t)b)};
}

struct OptOriginZeroLine {
    OriginZeroLine val;
    bool has = false;

    constexpr OptOriginZeroLine() = default;
    constexpr explicit OptOriginZeroLine(OriginZeroLine v)
        : val(v), has(true) {}

    constexpr bool IsSome() const { return has; }
};

constexpr uint16_t kMaxGridTracks = 10000;
constexpr int16_t kMinOzLine = -(int16_t)kMaxGridTracks;
constexpr int16_t kMaxOzLine = (int16_t)kMaxGridTracks;

OriginZeroLine IntoOriginZeroLine(GridLine line, uint16_t explicitTrackCount);

constexpr uint16_t ImpliedNegativeImplicitTracks(OriginZeroLine l) {
    return l.v < 0 ? (uint16_t)(-(int32_t)l.v) : (uint16_t)0;
}

constexpr uint16_t ImpliedPositiveImplicitTracks(OriginZeroLine l,
                                                 uint16_t explicitTrackCount) {
    return l.v > (int16_t)explicitTrackCount
               ? (uint16_t)((uint16_t)l.v - explicitTrackCount)
               : (uint16_t)0;
}

struct LineOzl {
    OriginZeroLine start;
    OriginZeroLine end;

    uint16_t Span() const {
        int32_t d = (int32_t)end.v - (int32_t)start.v;
        return d > 0 ? (uint16_t)d : (uint16_t)0;
    }
};

struct LineOptOzl {
    OptOriginZeroLine start;
    OptOriginZeroLine end;
};

enum class GridAutoFlow : uint8_t {
    Row,
    Column,
    RowDense,
    ColumnDense
};

constexpr bool IsDense(GridAutoFlow f) {
    return f == GridAutoFlow::RowDense || f == GridAutoFlow::ColumnDense;
}

constexpr AbsoluteAxis PrimaryAxis(GridAutoFlow f) {
    return (f == GridAutoFlow::Row || f == GridAutoFlow::RowDense)
               ? AbsoluteAxis::Horizontal
               : AbsoluteAxis::Vertical;
}

enum class GridPlacementKind : uint8_t {
    Auto,
    Line,
    Span,
    NamedLine,
    NamedSpan
};

struct GridPlacement {
    GridPlacementKind kind = GridPlacementKind::Auto;
    int16_t line = 0;
    uint16_t span = 0;
    Str name;

    static GridPlacement Auto() { return {}; }
    static GridPlacement FromLineIndex(int16_t index) {
        GridPlacement p;
        p.kind = GridPlacementKind::Line;
        p.line = index;
        return p;
    }
    static GridPlacement FromSpan(uint16_t span) {
        GridPlacement p;
        p.kind = GridPlacementKind::Span;
        p.span = span;
        return p;
    }
    static GridPlacement FromNamedLine(Str name, int16_t index) {
        GridPlacement p;
        p.kind = GridPlacementKind::NamedLine;
        p.name = name;
        p.line = index;
        return p;
    }
    static GridPlacement FromNamedSpan(Str name, uint16_t span) {
        GridPlacement p;
        p.kind = GridPlacementKind::NamedSpan;
        p.name = name;
        p.span = span;
        return p;
    }
};

struct PlainPlacement {
    GridPlacementKind kind = GridPlacementKind::Auto;
    int16_t line = 0;
    uint16_t span = 0;

    static PlainPlacement Auto() { return {}; }
    static PlainPlacement AtLine(int16_t l) {
        return {GridPlacementKind::Line, l, 0};
    }
    static PlainPlacement Spanning(uint16_t s) {
        return {GridPlacementKind::Span, 0, s};
    }

    bool IsAuto() const { return kind == GridPlacementKind::Auto; }
    bool IsLine() const { return kind == GridPlacementKind::Line; }
    bool IsSpan() const { return kind == GridPlacementKind::Span; }
    OriginZeroLine Ozl() const { return OriginZeroLine{line}; }
};

constexpr bool operator==(PlainPlacement a, PlainPlacement b) {
    return a.kind == b.kind && a.line == b.line && a.span == b.span;
}
constexpr bool operator!=(PlainPlacement a, PlainPlacement b) {
    return !(a == b);
}

struct LinePlacement {
    GridPlacement start;
    GridPlacement end;

    bool IsDefinite() const;

    struct LinePlain IntoOriginZeroIgnoringNamed(
        uint16_t explicitTrackCount) const;
};

struct LinePlain {
    PlainPlacement start;
    PlainPlacement end;

    bool IsDefinite() const;

    bool IsDefiniteGridLine() const;

    LinePlain IntoOriginZero(uint16_t explicitTrackCount) const;

    uint16_t IndefiniteSpan() const;

    LineOzl ResolveDefiniteGridLines() const;

    LineOptOzl ResolveAbsolutelyPositionedGridTracks() const;

    LineOzl ResolveIndefiniteGridTracks(OriginZeroLine start) const;
};

struct MaxTrackSizingFunction {
    CompactLength raw = CompactLength::Auto();

    static MaxTrackSizingFunction Length(float v) {
        return {CompactLength::Length(v)};
    }
    static MaxTrackSizingFunction Percent(float v) {
        return {CompactLength::Percent(v)};
    }
    static MaxTrackSizingFunction Auto() { return {CompactLength::Auto()}; }
    static MaxTrackSizingFunction MinContent() {
        return {CompactLength::MinContent()};
    }
    static MaxTrackSizingFunction MaxContent() {
        return {CompactLength::MaxContent()};
    }
    static MaxTrackSizingFunction FitContentPx(float v) {
        return {CompactLength::FitContentPx(v)};
    }
    static MaxTrackSizingFunction FitContentPercent(float v) {
        return {CompactLength::FitContentPercent(v)};
    }
    static MaxTrackSizingFunction FitContent(LengthPercentage lp) {
        return lp.raw.Tag() == CompactLength::kPercentTag
                   ? FitContentPercent(lp.raw.Value())
                   : FitContentPx(lp.raw.Value());
    }
    static MaxTrackSizingFunction Fr(float v) { return {CompactLength::Fr(v)}; }
    static MaxTrackSizingFunction Zero() { return {CompactLength::Zero()}; }
    static MaxTrackSizingFunction From(LengthPercentage v) { return {v.raw}; }
    static MaxTrackSizingFunction From(LengthPercentageAuto v) {
        return {v.raw};
    }
    static MaxTrackSizingFunction From(Dimension v) { return {v.raw}; }

    bool IsIntrinsic() const { return raw.IsIntrinsic(); }
    bool IsMaxContentAlike() const { return raw.IsMaxContentAlike(); }
    bool IsFr() const { return raw.IsFr(); }
    bool IsAuto() const { return raw.IsAuto(); }
    bool IsMinContent() const { return raw.IsMinContent(); }
    bool IsMaxContent() const { return raw.IsMaxContent(); }
    bool IsFitContent() const { return raw.IsFitContent(); }
    bool IsMaxOrFitContent() const { return raw.IsMaxOrFitContent(); }
    bool UsesPercentage() const { return raw.UsesPercentage(); }

    bool HasDefiniteValue(Optf parentSize) const;
    Optf DefiniteValue(Optf parentSize, CalcResolver calc) const;

    Optf DefiniteLimit(Optf parentSize, CalcResolver calc) const;
    Optf ResolvedPercentageSize(float parentSize, CalcResolver calc) const {
        return taffy::ResolvedPercentageSize(raw, parentSize, calc);
    }
};

struct MinTrackSizingFunction {
    CompactLength raw = CompactLength::Auto();

    static MinTrackSizingFunction Length(float v) {
        return {CompactLength::Length(v)};
    }
    static MinTrackSizingFunction Percent(float v) {
        return {CompactLength::Percent(v)};
    }
    static MinTrackSizingFunction Auto() { return {CompactLength::Auto()}; }
    static MinTrackSizingFunction MinContent() {
        return {CompactLength::MinContent()};
    }
    static MinTrackSizingFunction MaxContent() {
        return {CompactLength::MaxContent()};
    }
    static MinTrackSizingFunction Zero() { return {CompactLength::Zero()}; }
    static MinTrackSizingFunction From(LengthPercentage v) { return {v.raw}; }
    static MinTrackSizingFunction From(LengthPercentageAuto v) {
        return {v.raw};
    }
    static MinTrackSizingFunction From(Dimension v) { return {v.raw}; }

    static MinTrackSizingFunction From(MaxTrackSizingFunction v) {
        if (v.raw.IsFr() || v.raw.IsFitContent()) {
            return Auto();
        }
        return {v.raw};
    }

    bool IsAuto() const { return raw.IsAuto(); }
    bool IsMinContent() const { return raw.IsMinContent(); }
    bool IsMaxContent() const { return raw.IsMaxContent(); }
    bool IsMinOrMaxContent() const { return raw.IsMinOrMaxContent(); }
    bool UsesPercentage() const {
        return raw.Tag() == CompactLength::kPercentTag || raw.IsCalc();
    }

    Optf DefiniteValue(Optf parentSize, CalcResolver calc) const;
    Optf ResolvedPercentageSize(float parentSize, CalcResolver calc) const {
        return taffy::ResolvedPercentageSize(raw, parentSize, calc);
    }
};

struct TrackSizingFunction {
    MinTrackSizingFunction min = MinTrackSizingFunction::Auto();
    MaxTrackSizingFunction max = MaxTrackSizingFunction::Auto();

    static TrackSizingFunction Auto() { return {}; }
    static TrackSizingFunction MinContent() {
        return {MinTrackSizingFunction::MinContent(),
                MaxTrackSizingFunction::MinContent()};
    }
    static TrackSizingFunction MaxContent() {
        return {MinTrackSizingFunction::MaxContent(),
                MaxTrackSizingFunction::MaxContent()};
    }
    static TrackSizingFunction Zero() {
        return {MinTrackSizingFunction::Zero(), MaxTrackSizingFunction::Zero()};
    }
    static TrackSizingFunction Length(float v) {
        return {MinTrackSizingFunction::Length(v),
                MaxTrackSizingFunction::Length(v)};
    }
    static TrackSizingFunction Percent(float v) {
        return {MinTrackSizingFunction::Percent(v),
                MaxTrackSizingFunction::Percent(v)};
    }
    static TrackSizingFunction Fr(float v) {
        return {MinTrackSizingFunction::Auto(), MaxTrackSizingFunction::Fr(v)};
    }
    static TrackSizingFunction FitContent(LengthPercentage lp) {
        return {MinTrackSizingFunction::Auto(),
                MaxTrackSizingFunction::FitContent(lp)};
    }
    static TrackSizingFunction MinMax(MinTrackSizingFunction mn,
                                      MaxTrackSizingFunction mx) {
        return {mn, mx};
    }
    static TrackSizingFunction From(LengthPercentage v) {
        return {MinTrackSizingFunction::From(v),
                MaxTrackSizingFunction::From(v)};
    }

    MinTrackSizingFunction MinSizingFunction() const { return min; }
    MaxTrackSizingFunction MaxSizingFunction() const { return max; }

    bool HasFixedComponent() const {
        return min.raw.IsLengthOrPercentage() || max.raw.IsLengthOrPercentage();
    }
};

struct RepetitionCount {
    enum class Kind : uint8_t {
        AutoFill,
        AutoFit,
        Count
    };
    Kind kind = Kind::Count;
    uint16_t count = 1;

    static RepetitionCount AutoFill() { return {Kind::AutoFill, 0}; }
    static RepetitionCount AutoFit() { return {Kind::AutoFit, 0}; }
    static RepetitionCount Exactly(uint16_t n) { return {Kind::Count, n}; }

    bool IsAuto() const {
        return kind == Kind::AutoFill || kind == Kind::AutoFit;
    }
};

constexpr bool operator==(RepetitionCount a, RepetitionCount b) {
    return a.kind == b.kind &&
           (a.kind != RepetitionCount::Kind::Count || a.count == b.count);
}

struct LineNameSet {
    Slice<Str> names;
};

struct GridTemplateRepetition {
    RepetitionCount count;
    Slice<TrackSizingFunction> tracks;
    Slice<LineNameSet> lineNames;

    uint16_t TrackCount() const { return (uint16_t)tracks.len; }
};

struct GridTemplateComponent {
    bool isRepeat = false;
    TrackSizingFunction single;
    GridTemplateRepetition repeat;

    static GridTemplateComponent Single(TrackSizingFunction t) {
        GridTemplateComponent c;
        c.single = t;
        return c;
    }
    static GridTemplateComponent Repeat(GridTemplateRepetition r) {
        GridTemplateComponent c;
        c.isRepeat = true;
        c.repeat = r;
        return c;
    }

    bool IsAutoRepetition() const { return isRepeat && repeat.count.IsAuto(); }
};

struct GridTemplateArea {
    Str name;
    uint16_t rowStart = 0;
    uint16_t rowEnd = 0;
    uint16_t columnStart = 0;
    uint16_t columnEnd = 0;
};

struct GridTemplateAreas {
    Slice<GridTemplateArea> areas;
    uint16_t rowCount = 0;
    uint16_t columnCount = 0;
};

enum class GridAreaAxis : uint8_t {
    Row,
    Column
};

enum class GridAreaEnd : uint8_t {
    Start,
    End
};

struct Style {
    Display display = Display::Flex;

    bool itemIsTable = false;

    bool itemIsReplaced = false;
    BoxSizing boxSizing = BoxSizing::BorderBox;
    Direction direction = Direction::Ltr;

    PointOverflow overflow;
    float scrollbarWidth = 0.0f;

    Float floatMode = Float::None;
    Clear clear = Clear::None;

    Position position = Position::Relative;
    RectLpa inset = RectLpa::Auto();

    SizeDim size;
    SizeDim minSize;
    SizeDim maxSize;
    Optf aspectRatio = None();

    RectLpa margin;
    RectLp padding;
    RectLp border;

    OptAlignItems alignItems;
    OptAlignSelf alignSelf;
    OptAlignItems justifyItems;
    OptAlignSelf justifySelf;
    OptAlignContent alignContent;
    OptJustifyContent justifyContent;
    SizeLp gap;

    TextAlign textAlign = TextAlign::Auto;

    FlexDirection flexDirection = FlexDirection::Row;
    FlexWrap flexWrap = FlexWrap::NoWrap;

    Dimension flexBasis = Dimension::Auto();
    float flexGrow = 0.0f;
    float flexShrink = 1.0f;

    Slice<GridTemplateComponent> gridTemplateRows;
    Slice<GridTemplateComponent> gridTemplateColumns;
    Slice<TrackSizingFunction> gridAutoRows;
    Slice<TrackSizingFunction> gridAutoColumns;
    GridAutoFlow gridAutoFlow = GridAutoFlow::Row;

    GridTemplateAreas gridTemplateAreas;
    Slice<LineNameSet> gridTemplateColumnNames;
    Slice<LineNameSet> gridTemplateRowNames;

    LinePlacement gridRow;
    LinePlacement gridColumn;

    BoxGenerationMode BoxGenMode() const {
        return display == Display::None ? BoxGenerationMode::None
                                        : BoxGenerationMode::Normal;
    }
    bool IsBlock() const { return display == Display::Block; }
    bool IsCompressibleReplaced() const { return itemIsReplaced; }
};

bool operator==(const Style& a, const Style& b);

inline bool operator!=(const Style& a, const Style& b) {
    return !(a == b);
}

inline bool SameOptf(Optf a, Optf b) {
    uint32_t ab = 0;
    uint32_t bb = 0;
    memcpy(&ab, &a, sizeof(ab));
    memcpy(&bb, &b, sizeof(bb));
    return ab == bb;
}

inline bool SameFloatBits(float a, float b) {
    uint32_t ab = 0;
    uint32_t bb = 0;
    memcpy(&ab, &a, sizeof(ab));
    memcpy(&bb, &b, sizeof(bb));
    return ab == bb;
}

inline bool operator==(MinTrackSizingFunction a, MinTrackSizingFunction b) {
    return a.raw == b.raw;
}

inline bool operator==(MaxTrackSizingFunction a, MaxTrackSizingFunction b) {
    return a.raw == b.raw;
}

inline bool operator==(TrackSizingFunction a, TrackSizingFunction b) {
    return a.min == b.min && a.max == b.max;
}

inline bool operator==(const GridPlacement& a, const GridPlacement& b) {
    return a.kind == b.kind && a.line == b.line && a.span == b.span &&
           base::StrEq(a.name, b.name);
}

inline bool operator==(const LinePlacement& a, const LinePlacement& b) {
    return a.start == b.start && a.end == b.end;
}

inline bool operator==(const GridTemplateArea& a, const GridTemplateArea& b) {
    return base::StrEq(a.name, b.name) && a.rowStart == b.rowStart &&
           a.rowEnd == b.rowEnd && a.columnStart == b.columnStart &&
           a.columnEnd == b.columnEnd;
}

template <typename T>
inline bool SameSlice(Slice<T> a, Slice<T> b) {
    if (a.len != b.len) {
        return false;
    }
    for (int i = 0; i < a.len; i++) {
        if (!(a.els[i] == b.els[i])) {
            return false;
        }
    }
    return true;
}

inline bool SameNames(Slice<base::Str> a, Slice<base::Str> b) {
    if (a.len != b.len) {
        return false;
    }
    for (int i = 0; i < a.len; i++) {
        if (!base::StrEq(a.els[i], b.els[i])) {
            return false;
        }
    }
    return true;
}

inline bool operator==(const LineNameSet& a, const LineNameSet& b) {
    return SameNames(a.names, b.names);
}

inline bool operator==(const GridTemplateRepetition& a,
                       const GridTemplateRepetition& b) {
    return a.count == b.count && SameSlice(a.tracks, b.tracks) &&
           SameSlice(a.lineNames, b.lineNames);
}

inline bool operator==(const GridTemplateComponent& a,
                       const GridTemplateComponent& b) {
    if (a.isRepeat != b.isRepeat) {
        return false;
    }
    return a.isRepeat ? a.repeat == b.repeat : a.single == b.single;
}

}

#if GPUI_INCLUDE_PRIVATE_API
#line 1 "src/taffy/taffy_math.h"

#if defined(_MSC_VER)
#define TAFFY_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define TAFFY_INLINE inline __attribute__((always_inline))
#else
#define TAFFY_INLINE inline
#endif

namespace taffy {

inline Optf MaybeMin(Optf a, Optf b) {
    if (IsNone(a)) {
        return None();
    }
    return IsSome(b) ? F32Min(a, b) : a;
}

inline Optf MaybeMax(Optf a, Optf b) {
    if (IsNone(a)) {
        return None();
    }
    return IsSome(b) ? F32Max(a, b) : a;
}

inline Optf MaybeAdd(Optf a, Optf b) {
    if (IsNone(a)) {
        return None();
    }
    return IsSome(b) ? a + b : a;
}

inline Optf MaybeSub(Optf a, Optf b) {
    if (IsNone(a)) {
        return None();
    }
    return IsSome(b) ? a - b : a;
}

inline Optf MaybeClamp(Optf a, Optf lo, Optf hi) {
    if (IsNone(a)) {
        return None();
    }
    float v = a;
    if (IsSome(hi)) {
        v = F32Min(v, hi);
    }
    if (IsSome(lo)) {
        v = F32Max(v, lo);
    }
    return v;
}

inline AvailableSpace MaybeMin(AvailableSpace a, Optf b) {
    if (a.kind == AvailableSpace::Kind::Definite) {
        return IsSome(b) ? AvailableSpace::Definite(F32Min(a.value, b)) : a;
    }
    return IsSome(b) ? AvailableSpace::Definite(b) : a;
}

inline AvailableSpace MaybeMax(AvailableSpace a, Optf b) {
    if (a.kind == AvailableSpace::Kind::Definite && IsSome(b)) {
        return AvailableSpace::Definite(F32Max(a.value, b));
    }
    return a;
}

inline AvailableSpace MaybeAdd(AvailableSpace a, Optf b) {
    if (a.kind == AvailableSpace::Kind::Definite && IsSome(b)) {
        return AvailableSpace::Definite(a.value + b);
    }
    return a;
}

inline AvailableSpace MaybeSub(AvailableSpace a, Optf b) {
    if (a.kind == AvailableSpace::Kind::Definite && IsSome(b)) {
        return AvailableSpace::Definite(a.value - b);
    }
    return a;
}

inline AvailableSpace MaybeClamp(AvailableSpace a, Optf lo, Optf hi) {
    if (a.kind != AvailableSpace::Kind::Definite) {
        return a;
    }
    float v = a.value;
    if (IsSome(hi)) {
        v = F32Min(v, hi);
    }
    if (IsSome(lo)) {
        v = F32Max(v, lo);
    }
    return AvailableSpace::Definite(v);
}

TAFFY_INLINE SizeFOpt MaybeMin(SizeFOpt a, SizeFOpt b) {
    return {MaybeMin(a.w, b.w), MaybeMin(a.h, b.h)};
}

TAFFY_INLINE SizeFOpt MaybeMax(SizeFOpt a, SizeFOpt b) {
    return {MaybeMax(a.w, b.w), MaybeMax(a.h, b.h)};
}

TAFFY_INLINE SizeFOpt MaybeAdd(SizeFOpt a, SizeFOpt b) {
    return {MaybeAdd(a.w, b.w), MaybeAdd(a.h, b.h)};
}

TAFFY_INLINE SizeFOpt MaybeSub(SizeFOpt a, SizeFOpt b) {
    return {MaybeSub(a.w, b.w), MaybeSub(a.h, b.h)};
}

TAFFY_INLINE SizeFOpt MaybeClamp(SizeFOpt a, SizeFOpt lo, SizeFOpt hi) {
    return {MaybeClamp(a.w, lo.w, hi.w), MaybeClamp(a.h, lo.h, hi.h)};
}

TAFFY_INLINE SizeAvail MaybeSub(SizeAvail a, SizeFOpt b) {
    return {MaybeSub(a.width, b.w), MaybeSub(a.height, b.h)};
}

TAFFY_INLINE SizeAvail MaybeClamp(SizeAvail a, SizeFOpt lo, SizeFOpt hi) {
    return {MaybeClamp(a.width, lo.w, hi.w), MaybeClamp(a.height, lo.h, hi.h)};
}

TAFFY_INLINE SizeAvail MaybeSet(SizeAvail a, SizeFOpt v) {
    return a.MaybeSet(v);
}

constexpr SizeFOpt AsOptional(SizeF s) {
    return s;
}

}

#endif

#line 1 "src/taffy/tree.h"

namespace taffy {

struct NodeId {
    uint64_t raw = 0;

    static constexpr NodeId New(uint64_t v) { return NodeId{v}; }
    constexpr uint64_t AsU64() const { return raw; }
};

constexpr bool operator==(NodeId a, NodeId b) {
    return a.raw == b.raw;
}

constexpr bool operator!=(NodeId a, NodeId b) {
    return a.raw != b.raw;
}

enum class RunMode : uint8_t {

    PerformLayout,

    ComputeSize,

    PerformHiddenLayout
};

enum class SizingMode : uint8_t {

    ContentSize,

    InherentSize
};

enum class RequestedAxis : uint8_t {
    Horizontal,
    Vertical,
    Both
};

constexpr RequestedAxis ToRequestedAxis(AbsoluteAxis a) {
    return a == AbsoluteAxis::Horizontal ? RequestedAxis::Horizontal
                                         : RequestedAxis::Vertical;
}

struct CollapsibleMarginSet {

    float positive = 0.0f;

    float negative = 0.0f;

    static constexpr CollapsibleMarginSet Zero() { return {}; }

    static CollapsibleMarginSet FromMargin(float margin) {
        if (margin >= 0.0f) {
            return {margin, 0.0f};
        }
        return {0.0f, margin};
    }
    CollapsibleMarginSet CollapseWithMargin(float margin) const {
        CollapsibleMarginSet out = *this;
        if (margin >= 0.0f) {
            out.positive = F32Max(out.positive, margin);
        } else {
            out.negative = F32Min(out.negative, margin);
        }
        return out;
    }
    CollapsibleMarginSet CollapseWithSet(CollapsibleMarginSet other) const {
        return {F32Max(positive, other.positive),
                F32Min(negative, other.negative)};
    }

    float Resolve() const { return positive + negative; }
};

struct LayoutInput {

    RunMode runMode = RunMode::PerformLayout;

    SizingMode sizingMode = SizingMode::InherentSize;

    RequestedAxis axis = RequestedAxis::Both;

    SizeFOpt knownDimensions = SizeFOptNone();

    SizeFOpt parentSize = SizeFOptNone();

    SizeAvail availableSpace = SizeAvail::MaxContent();

    LineBool verticalMarginsAreCollapsible;

    static LayoutInput Hidden() {
        LayoutInput in;
        in.runMode = RunMode::PerformHiddenLayout;
        return in;
    }
};

struct LayoutOutput {
    SizeF size;

    SizeF contentSize;

    PointFOpt firstBaselines = PointFOptNone();

    CollapsibleMarginSet topMargin;
    CollapsibleMarginSet bottomMargin;

    bool marginsCanCollapseThrough = false;

    static LayoutOutput Hidden() { return {}; }

    static LayoutOutput FromSizesAndBaselines(SizeF size, SizeF contentSize,
                                              PointFOpt firstBaselines) {
        LayoutOutput out;
        out.size = size;
        out.contentSize = contentSize;
        out.firstBaselines = firstBaselines;
        return out;
    }
    static LayoutOutput FromSizes(SizeF size, SizeF contentSize) {
        return FromSizesAndBaselines(size, contentSize, PointFOptNone());
    }
    static LayoutOutput FromOuterSize(SizeF size) {
        return FromSizes(size, SizeF::Zero());
    }
};

struct Layout {

    uint32_t order = 0;

    PointF location;
    SizeF size;

    SizeF contentSize;

    SizeF scrollbarSize;
    RectF border;
    RectF padding;
    RectF margin;

    static Layout New() { return {}; }
    static Layout WithOrder(uint32_t order) {
        Layout l;
        l.order = order;
        return l;
    }

    float ContentBoxWidth() const {
        return size.w - padding.left - padding.right - border.left -
               border.right;
    }
    float ContentBoxHeight() const {
        return size.h - padding.top - padding.bottom - border.top -
               border.bottom;
    }
    SizeF ContentBoxSize() const {
        return {ContentBoxWidth(), ContentBoxHeight()};
    }

    float ContentBoxX() const {
        return location.x + border.left + padding.left;
    }
    float ContentBoxY() const { return location.y + border.top + padding.top; }

    float ScrollWidth() const {
        return F32Max(0.0f, contentSize.w +
                                F32Min(scrollbarSize.w, size.w) -
                                size.w + border.left + border.right);
    }
    float ScrollHeight() const {
        return F32Max(0.0f, contentSize.h +
                                F32Min(scrollbarSize.h, size.h) -
                                size.h + border.top + border.bottom);
    }
};

constexpr int kCacheSize = 9;

struct CacheKey {
    uint64_t kdAvailableSpace = 0;
    uint64_t parentSize = 0;

    static CacheKey From(const LayoutInput& input);

    uint64_t XAxisParentSize() const;
};

constexpr bool operator==(CacheKey a, CacheKey b) {
    return a.kdAvailableSpace == b.kdAvailableSpace &&
           a.parentSize == b.parentSize;
}

template <typename T>
struct CacheEntry {
    CacheKey key;
    T content{};
};

struct Cache {
    CacheEntry<LayoutOutput> finalLayoutEntry;
    CacheEntry<SizeF> measureEntries[kCacheSize];

    uint16_t presentMask = 0;

    bool isEmpty = true;

    static constexpr uint16_t kFinalBit = 1;
    static constexpr uint16_t MeasureBit(int i) {
        return (uint16_t)(1u << (i + 1));
    }

    bool Get(const LayoutInput& input, LayoutOutput* out) const;
    void Store(const LayoutInput& input, const LayoutOutput& output);

    bool GetWithKey(CacheKey key, RunMode runMode, LayoutOutput* out) const;
    void StoreWithKey(CacheKey key, const LayoutInput& input,
                      const LayoutOutput& output);

    bool Clear();
    bool IsEmpty() const;
};

}

#line 1 "src/taffy/taffy_tree.h"

namespace taffy {

struct BlockContext;

using MeasureFn = SizeF (*)(SizeFOpt knownDimensions, SizeAvail availableSpace,
                            NodeId node, void* nodeContext, const Style* style,
                            void* userData);

struct NodeData {
    Style style;

    Layout unroundedLayout;

    Layout finalLayout;

    bool hasContext = false;
    void* context = nullptr;

    Cache cache;

    uint32_t generation = 0;
    bool alive = false;

    Vec<NodeId> children;
    NodeId parent;
    bool hasParent = false;
};

struct TaffyTree {

    Vec<NodeData*> slots;
    Vec<int32_t> freeSlots;
    int32_t liveCount = 0;

    int32_t allocs = 0;

    bool useRounding = true;

    MeasureFn measureFn = nullptr;
    void* measureUserData = nullptr;

    Arena* styleArena = nullptr;

    CalcResolver calc;

    void Init(int capacity = 16);
    void Free();

    void EnableRounding() { useRounding = true; }
    void DisableRounding() { useRounding = false; }

    NodeId NewLeaf(const Style& style);
    NodeId NewLeafWithContext(const Style& style, void* context);
    NodeId NewWithChildren(const Style& style, const NodeId* children, int n);

    void Clear();

    void Remove(NodeId node);

    void EachUnreachable(NodeId root, void (*fn)(NodeId, void*), void* user);

    void SetNodeContext(NodeId node, void* context, bool hasContext);
    void* GetNodeContext(NodeId node) const;

    void AddChild(NodeId parent, NodeId child);

    bool InsertChildAtIndex(NodeId parent, int childIndex, NodeId child);
    void SetChildren(NodeId parent, const NodeId* children, int n);

    NodeId RemoveChild(NodeId parent, NodeId child);

    NodeId RemoveChildAtIndex(NodeId parent, int childIndex);
    void RemoveChildrenRange(NodeId parent, int start, int end);

    NodeId ReplaceChildAtIndex(NodeId parent, int childIndex, NodeId newChild);

    NodeId ChildAtIndex(NodeId parent, int childIndex) const;
    int TotalNodeCount() const { return liveCount; }

    int SlotCount() const { return slots.len; }

    NodeId Parent(NodeId child, bool* hasParent) const;

    void SetStyle(NodeId node, const Style& style);
    const Style& GetStyle(NodeId node) const;

    const Layout& GetLayout(NodeId node) const;
    const Layout& UnroundedLayout(NodeId node) const;

    void MarkDirty(NodeId node);
    bool Dirty(NodeId node) const;

    void ComputeLayoutWithMeasure(NodeId node, SizeAvail availableSpace,
                                  MeasureFn measure, void* userData);
    void ComputeLayout(NodeId node, SizeAvail availableSpace);

    void PrintTree(NodeId root);

    int ChildCount(NodeId parent) const;
    NodeId GetChildId(NodeId parent, int index) const;

    void SetUnroundedLayout(NodeId node, const Layout& layout);
    Layout GetUnroundedLayout(NodeId node) const;
    void SetFinalLayout(NodeId node, const Layout& layout);
    Layout GetFinalLayout(NodeId node) const;

    const char* GetDebugLabel(NodeId node) const;

    bool CacheGet(NodeId node, const LayoutInput& input,
                  LayoutOutput* out) const;
    void CacheStore(NodeId node, const LayoutInput& input,
                    const LayoutOutput& output);
    void CacheClear(NodeId node);

    float ResolveCalcValue(const void* handle, float basis) const {
        return calc.Resolve(handle, basis);
    }

    LayoutOutput ComputeChildLayout(NodeId node, LayoutInput inputs);
    LayoutOutput ComputeBlockChildLayout(NodeId node, LayoutInput inputs,
                                         BlockContext* blockCtx);

    float MeasureChildSize(NodeId node, SizeFOpt knownDimensions,
                           SizeFOpt parentSize, SizeAvail availableSpace,
                           SizingMode sizingMode, AbsoluteAxis axis,
                           LineBool verticalMarginsAreCollapsible);
    SizeF MeasureChildSizeBoth(NodeId node, SizeFOpt knownDimensions,
                               SizeFOpt parentSize, SizeAvail availableSpace,
                               SizingMode sizingMode,
                               LineBool verticalMarginsAreCollapsible);
    LayoutOutput PerformChildLayout(NodeId node, SizeFOpt knownDimensions,
                                    SizeFOpt parentSize,
                                    SizeAvail availableSpace,
                                    SizingMode sizingMode,
                                    LineBool verticalMarginsAreCollapsible);

    NodeData* Get(NodeId node) const;
};

}

#if GPUI_INCLUDE_PRIVATE_API
#line 1 "src/taffy/compute.h"

namespace taffy {

void ComputeRootLayout(TaffyTree* tree, NodeId root, SizeAvail availableSpace);

LayoutOutput ComputeHiddenLayout(TaffyTree* tree, NodeId node);

void RoundLayout(TaffyTree* tree, NodeId node);

using LeafMeasureFn = SizeF (*)(SizeFOpt knownDimensions,
                                SizeAvail availableSpace, void* ctx);

LayoutOutput ComputeLeafLayout(const LayoutInput& inputs, const Style& style,
                               CalcResolver calc, LeafMeasureFn measure,
                               void* measureCtx);

LayoutOutput ComputeFlexboxLayout(TaffyTree* tree, NodeId node,
                                  const LayoutInput& inputs);

LayoutOutput ComputeGridLayout(TaffyTree* tree, NodeId node,
                               const LayoutInput& inputs);

LayoutOutput ComputeBlockLayout(TaffyTree* tree, NodeId node,
                                const LayoutInput& inputs,
                                BlockContext* blockCtx);

inline AlignItemsKeyword ResolveSelfAlignmentSafety(AlignItems alignment,
                                                    bool overflows) {
    if (alignment.safety == AlignmentSafety::Safe && overflows) {
        return AlignItemsKeyword::Start;
    }
    return alignment.keyword;
}

AlignContentKeyword ApplyAlignmentFallback(float freeSpace, int numItems,
                                           AlignContent alignmentMode);

float ComputeAlignmentOffset(float freeSpace, int numItems, float gap,
                             AlignContentKeyword alignmentMode,
                             bool layoutIsFlexReversed, bool isFirst);

void GridExplicitSizeForTest(const Style& style, Optf autoFitContainerSize,
                             bool maxRepetitions, AbsoluteAxis axis,
                             CalcResolver calc, uint16_t* outAutoRepetitions,
                             uint16_t* outTrackCount);

void GridChildMinMaxSpanForTest(LinePlacement line, uint16_t explicitTrackCount,
                                int16_t* outMinLine, int16_t* outMaxLine,
                                uint16_t* outSpan);

void GridSizeEstimateForTest(uint16_t explicitColCount,
                             uint16_t explicitRowCount, Direction direction,
                             const LinePlacement* columns,
                             const LinePlacement* rows, int n,
                             uint16_t* outColCounts, uint16_t* outRowCounts);

struct GridTrackForTest {
    bool isGutter = false;
    bool isCollapsed = false;
    CompactLength min;
    CompactLength max;
};

int GridInitTracksForTest(const Style& style, AbsoluteAxis axis,
                          uint16_t negativeImplicit, uint16_t explicitCount,
                          uint16_t positiveImplicit, GridTrackForTest* out,
                          int cap);

struct GridPlacementForTest {
    int16_t columnStart = 0;
    int16_t columnEnd = 0;
    int16_t rowStart = 0;
    int16_t rowEnd = 0;
};

int GridPlaceForTest(TaffyTree* tree, NodeId parent, uint16_t explicitColCount,
                     uint16_t explicitRowCount, GridAutoFlow flow,
                     GridPlacementForTest* out, int cap, uint16_t* outColCounts,
                     uint16_t* outRowCounts);

SizeF ComputeContentSizeContribution(PointF location, SizeF size,
                                     SizeF contentSize, PointOverflow overflow);

}

#endif

#endif
