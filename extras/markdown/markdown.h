#ifndef MARKDOWN_AMALGAM_H_
#define MARKDOWN_AMALGAM_H_
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

constexpr int len(Str s) noexcept {
    return s.len;
}

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
    uint64_t nAllocsSinceReset;

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

inline int VecNextCap(int cap, int wanted, int elSize) {
    if (cap == 0) {
        int floorCap = elSize == 1 ? 8 : elSize <= 1024 ? 4 : 1;
        return std::max(floorCap, wanted);
    }
    return std::max(cap * 2, wanted);
}

inline int VecAbsCap(int cap) {
    return cap < 0 ? -cap : cap;
}

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
        VecDbgDeath(dbgId, len, VecAbsCap(cap));
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
VecNonTemplated* VecNT(Vec<T>& v) {
    return (VecNonTemplated*)&v;
}

template <typename T>
inline int len(const Vec<T>& v) {
    return v.len;
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
    int curCap = VecAbsCap(v.cap);
    if (n > curCap) {
        VecDbgGrow(v.dbgId, len(v), curCap, n,
                   VecNextCap(curCap, n, (int)sizeof(T)));
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
T* VecTake(Vec<T>& v) {
    return (T*)VecTakeNT(VecNT(v), (int)sizeof(T));
}

template <typename T>
bool VecAppend(Vec<T>& v, const VecIdentityT<T>& el) {
    return VecInsertAt(v, len(v), el);
}

template <typename T>
bool VecAppendVec(Vec<T>& v, const Vec<T>& other) {
    return VecAppendN(v, other.els, len(other));
}

template <typename T>
bool VecAppendN(Vec<T>& v, const T* src, int count) {
    if (count == 0) {
        return true;
    }
    T* dst = VecInsertSpace(v, len(v), count);
    if (!dst) {
        return false;
    }
    memcpy((void*)dst, (const void*)src, (size_t)count * sizeof(T));
    return true;
}

template <typename T>
T* VecAppendBlanks(Vec<T>& v, int count) {
    return VecInsertSpace(v, len(v), count);
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
    if (len(v) > 0) {
        VecRemoveAt(v, len(v) - 1);
    }
}

template <typename T>
T VecPop(Vec<T>& v) {
    T el = v.els[len(v) - 1];
    VecRemoveAtFast(v, len(v) - 1);
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
bool VecIsValidIndex(const Vec<T>& v, int idx) {
    return idx >= 0 && idx < len(v);
}

template <typename T>
T& VecLast(const Vec<T>& v) {
    return v.els[len(v) - 1];
}

template <typename T>
int VecFind(const Vec<T>& v, const T& el, int startAt = 0) {
    for (int i = startAt; i < len(v); i++) {
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

    bool Append(Arena* arena, const T& el) { return AppendMany(arena, &el, 1); }

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
        int byBytes = bytes / (int)sizeof(T);
        if (byBytes < 1) {
            byBytes = 1;
        }
        return byBytes < count ? byBytes : count;
    }

    static int NextCap(int prevCap) {
        const int steps[3] = {
            CapFor(kArenaVecCap0, kArenaVecBytes0),
            CapFor(kArenaVecCap1, kArenaVecBytes1),
            CapFor(kArenaVecCap2, kArenaVecBytes2),
        };
        for (int s : steps) {
            if (prevCap < s) {
                return s;
            }
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

template <typename T>
inline int len(const ArenaVec<T>& v) {
    return v.len;
}

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

GPUI_NOINLINE bool StrEqRest(Str s1, Str s2);
inline bool StrEq(Str s1, Str s2) {
    if (len(s1) != len(s2)) {
        return false;
    }
    return StrEqRest(s1, s2);
}
inline bool StrEq(Str s1, const char* s2) {
    return StrEq(s1, Str(s2));
}
int StrCmp(Str s1, Str s2);
GPUI_NOINLINE bool StrEqIRest(Str s1, Str s2);
inline bool StrEqI(Str s1, Str s2) {
    if (len(s1) != len(s2)) {
        return false;
    }
    return StrEqIRest(s1, s2);
}
inline bool StrEqI(Str s1, const char* s2) {
    return StrEqI(s1, Str(s2));
}
bool StrStartsWith(Str s, Str prefix);
inline bool StrStartsWith(Str s, const char* prefix) {
    return StrStartsWith(s, Str(prefix));
}
bool StrStartsWithAny(Str s, const char* chars);
bool StrStartsWithI(Str s, Str prefix);
inline bool StrStartsWithI(Str s, const char* prefix) {
    return StrStartsWithI(s, Str(prefix));
}
bool StrEndsWith(Str s, Str suffix);
inline bool StrEndsWith(Str s, const char* suffix) {
    return StrEndsWith(s, Str(suffix));
}
bool StrEndsWithI(Str s, Str suffix);
inline bool StrEndsWithI(Str s, const char* suffix) {
    return StrEndsWithI(s, Str(suffix));
}
int StrFind(Str s, Str sub);
inline int StrFind(Str s, const char* sub) {
    return StrFind(s, Str(sub));
}
int StrFindI(Str s, Str sub);
inline int StrFindI(Str s, const char* sub) {
    return StrFindI(s, Str(sub));
}
inline bool StrContains(Str s, Str sub) {
    return StrFind(s, sub) >= 0;
}
inline bool StrContainsI(Str s, Str sub) {
    return StrFindI(s, sub) >= 0;
}
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

#if GPUI_INCLUDE_PRIVATE_API
#line 1 "src/markdown/constant.h"

namespace markdown {

using base::SeqStrFirst;
using base::SeqStrings;
using base::SeqStrNext;
using base::Str;

constexpr int kAutolinkSchemeSizeMax = 32;

constexpr int kAutolinkDomainSizeMax = 63;

constexpr int kCharacterReferenceDecimalSizeMax = 7;

constexpr int kCharacterReferenceHexadecimalSizeMax = 6;

constexpr int kCharacterReferenceNamedSizeMax = 31;

constexpr int kCodeFencedSequenceSizeMin = 3;

constexpr int kFrontmatterSequenceSize = 3;

constexpr int kHardBreakPrefixSizeMin = 2;

constexpr int kHeadingAtxOpeningFenceSizeMax = 6;

extern const Str kHtmlCdataPrefix;

constexpr int kHtmlRawSizeMax = 8;

constexpr int kLinkReferenceSizeMax = 999;

constexpr int kListItemValueSizeMax = 10;

constexpr int kMathFlowSequenceSizeMin = 2;

constexpr int kResourceDestinationBalanceMax = 32;

constexpr int kTabSize = 4;

constexpr int kThematicBreakMarkerCountMin = 3;

extern const char kHtmlBlockNames[];
extern const char kHtmlRawNames[];

struct CharacterReference {
    int32_t nameOff;
    int32_t valueOff;
};
extern const char kCharacterReferenceNames[];
extern const char kCharacterReferenceValues[];
extern const CharacterReference kCharacterReferences[2125];

}

#endif

#line 1 "src/markdown/mdast.h"

namespace markdown {

using base::Arena;
using base::ArenaPtr;
using base::ArenaPtrGet;
using base::ArenaPtrOf;
using base::ArenaStr;
using base::ArenaStrGet;
using base::ArenaVec;
using base::kArenaStrNone;
using base::Str;

struct Node;

using ArenaNode = ArenaPtr<Node>;

struct UnistPoint {
    int32_t line = 1;
    int32_t column = 1;
    int32_t offset = 0;
};

struct UnistPosition {
    UnistPoint start = {};
    UnistPoint end = {};
};

UnistPosition GetUnistPosition(Str md, uint32_t start, uint32_t end);

enum class ReferenceKind : uint8_t {
    Shortcut,
    Collapsed,
    Full,
};

enum class AlignKind : uint8_t {
    Left,
    Right,
    Center,
    None,
};

using ArenaAlign = uint32_t;
constexpr ArenaAlign kArenaAlignNone = 0;

static_assert((int)AlignKind::None < 4, "an alignment has to fit in 2 bits");

ArenaAlign ArenaAlignNew(Arena* a, int32_t count);
int32_t ArenaAlignCount(Arena* a, ArenaAlign al);
AlignKind ArenaAlignAt(Arena* a, ArenaAlign al, int32_t i);
void ArenaAlignSet(Arena* a, ArenaAlign al, int32_t i, AlignKind k);

enum class NodeKind : uint8_t {
    Root,
    Blockquote,
    FootnoteDefinition,
    List,
    Toml,
    Yaml,
    Break,
    InlineCode,
    InlineMath,
    Delete,
    Emphasis,
    FootnoteReference,
    Html,
    Image,
    ImageReference,
    Link,
    LinkReference,
    Strong,
    Text,
    Code,
    Math,
    Heading,
    Table,
    ThematicBreak,
    TableRow,
    TableCell,
    ListItem,
    Definition,
    Paragraph,
};

enum class NodeStrKind : uint8_t {

    Value,

    Url,

    Title,

    Alt,

    Identifier,

    Label,

    Lang,
    Meta,

    PerKind,
};

enum NodeFlag : uint8_t {

    NodeHasStart = 1 << 0,

    NodeOrdered = 1 << 1,

    NodeSpread = 1 << 2,

    NodeChecked = 1 << 3,
    NodeHasChecked = 1 << 4,

    NodeRefKindMask = 3 << 6,
};

struct Node {

    ArenaNode lastKid = {};

    ArenaNode sibling = {};

    ArenaStr firstStr = kArenaStrNone;

    NodeKind kind = NodeKind::Root;

    uint8_t flags = 0;

    bool Has(NodeFlag f) const { return (flags & f) != 0; }
    void Set(NodeFlag f, bool on) {
        flags = on ? (uint8_t)(flags | f) : (uint8_t)(flags & ~f);
    }
};

static_assert(sizeof(Node) == 3 * 4 + 4,
              "Node has picked up padding; order the fields largest first");

static_assert(alignof(Node) == 4, "a Node holds nothing wider than a word");

uint32_t NodePerKind(Arena* a, const Node* n);
void NodeSetPerKind(Arena* a, Node* n, uint32_t word);

Str NodeGetStr(Arena* a, const Node* n, NodeStrKind k);

int32_t NodeGetStrLen(Arena* a, const Node* n, NodeStrKind k);

bool NodeHasStr(Arena* a, const Node* n, NodeStrKind k);

void NodeSetStr(Arena* a, Node* n, NodeStrKind k, Str s);

void NodeClearStr(Arena* a, Node* n, NodeStrKind k);

void NodeGrowStr(Arena* a, Node* n, NodeStrKind k, Str more);

inline ReferenceKind NodeRefKind(const Node* n) {
    return (ReferenceKind)((n->flags & NodeRefKindMask) >> 6);
}

inline void NodeSetRefKind(Node* n, ReferenceKind k) {
    n->flags =
        (uint8_t)((n->flags & ~NodeRefKindMask) | (((uint8_t)k & 3) << 6));
}

Node* NodeNew(Arena* a, NodeKind kind);

inline void NodeAddChild(Arena* a, Node* parent, Node* child) {
    ArenaNode at = ArenaPtrOf(a, child);
    if (parent->lastKid.IsSet()) {
        Node* last = ArenaPtrGet(a, parent->lastKid);
        child->sibling = last->sibling;
        last->sibling = at;
    } else {

        child->sibling = at;
    }
    parent->lastKid = at;
}

inline Node* NodeLastChild(Arena* a, const Node* n) {
    return n->lastKid.IsSet() ? ArenaPtrGet(a, n->lastKid) : nullptr;
}

inline ArenaNode NodeFirstKid(Arena* a, const Node* n) {
    Node* last = NodeLastChild(a, n);
    return last ? last->sibling : ArenaNode{};
}

inline Node* NodeFirstChild(Arena* a, const Node* n) {
    ArenaNode first = NodeFirstKid(a, n);
    return first.IsSet() ? ArenaPtrGet(a, first) : nullptr;
}

inline Node* NodeChild(Arena* a, const Node* n, int i) {
    if (i < 0 || !n->lastKid.IsSet()) {
        return nullptr;
    }
    Node* last = ArenaPtrGet(a, n->lastKid);
    Node* at = ArenaPtrGet(a, last->sibling);
    for (int k = 0; k < i; k++) {
        if (at == last) {
            return nullptr;
        }
        at = ArenaPtrGet(a, at->sibling);
    }
    return at;
}

inline int NodeChildCount(Arena* a, const Node* n) {
    if (!n->lastKid.IsSet()) {
        return 0;
    }
    Node* last = ArenaPtrGet(a, n->lastKid);
    int count = 1;
    for (Node* at = ArenaPtrGet(a, last->sibling); at != last;
         at = ArenaPtrGet(a, at->sibling)) {
        count++;
    }
    return count;
}

struct NodeKidsRange {
    Arena* a;
    ArenaNode at;
    ArenaNode last;

    struct Iter {
        Arena* a;
        ArenaNode at;
        ArenaNode last;

        Node* operator*() const { return ArenaPtrGet(a, at); }
        Iter& operator++() {
            at = at == last ? ArenaNode{} : ArenaPtrGet(a, at)->sibling;
            return *this;
        }
        bool operator!=(const Iter& o) const { return at != o.at; }
    };

    Iter begin() const { return Iter{a, at, last}; }
    Iter end() const { return Iter{a, ArenaNode{}, last}; }
};

inline NodeKidsRange NodeKids(Arena* a, const Node* n) {
    return NodeKidsRange{a, NodeFirstKid(a, n), n->lastKid};
}

inline Str NodeStr(Arena* a, ArenaStr s) {
    return ArenaStrGet(a, s);
}

bool NodeHasChildren(NodeKind kind);

Str NodeToString(Arena* a, const Node* node);

}

#line 1 "src/markdown/markdown.h"

namespace markdown {

struct Constructs {
    bool attention = true;
    bool autolink = true;
    bool blockQuote = true;
    bool characterEscape = true;
    bool characterReference = true;
    bool codeIndented = true;
    bool codeFenced = true;
    bool codeText = true;
    bool definition = true;
    bool frontmatter = false;
    bool gfmAutolinkLiteral = false;
    bool gfmFootnoteDefinition = false;
    bool gfmLabelStartFootnote = false;
    bool gfmStrikethrough = false;
    bool gfmTable = false;
    bool gfmTaskListItem = false;
    bool hardBreakEscape = true;
    bool hardBreakTrailing = true;
    bool headingAtx = true;
    bool headingSetext = true;
    bool htmlFlow = true;
    bool htmlText = true;
    bool labelStartImage = true;
    bool labelStartLink = true;
    bool labelEnd = true;
    bool listItem = true;
    bool mathFlow = false;
    bool mathText = false;
    bool thematicBreak = true;

    static Constructs Gfm();
};

struct ParseOptions {
    Constructs constructs = {};
    bool gfmStrikethroughSingleTilde = true;
    bool mathTextSingleDollar = true;

    static ParseOptions Gfm();
};

Node* ToMdast(Arena* a, Str source, const ParseOptions& options);

Str DecodeNamed(Arena* a, Str name);

Str DecodeNumeric(Arena* a, Str value, int radix);

}

#if GPUI_INCLUDE_PRIVATE_API
#line 1 "src/markdown/state.h"

namespace markdown {

struct Tokenizer;

enum class StateName : uint16_t {
    AttentionStart,
    AttentionInside,
    AutolinkStart,
    AutolinkOpen,
    AutolinkSchemeOrEmailAtext,
    AutolinkSchemeInsideOrEmailAtext,
    AutolinkUrlInside,
    AutolinkEmailAtSignOrDot,
    AutolinkEmailAtext,
    AutolinkEmailValue,
    AutolinkEmailLabel,
    BlankLineStart,
    BlankLineAfter,
    BlockQuoteStart,
    BlockQuoteContStart,
    BlockQuoteContBefore,
    BlockQuoteContAfter,
    BomStart,
    BomInside,
    CharacterEscapeStart,
    CharacterEscapeInside,
    CharacterReferenceStart,
    CharacterReferenceOpen,
    CharacterReferenceNumeric,
    CharacterReferenceValue,
    CodeIndentedStart,
    CodeIndentedAtBreak,
    CodeIndentedAfter,
    CodeIndentedFurtherStart,
    CodeIndentedInside,
    CodeIndentedFurtherBegin,
    CodeIndentedFurtherAfter,
    ContentChunkStart,
    ContentChunkInside,
    ContentDefinitionBefore,
    ContentDefinitionAfter,
    DataStart,
    DataInside,
    DataAtBreak,
    DefinitionStart,
    DefinitionBefore,
    DefinitionLabelAfter,
    DefinitionLabelNok,
    DefinitionMarkerAfter,
    DefinitionDestinationBefore,
    DefinitionDestinationAfter,
    DefinitionDestinationMissing,
    DefinitionTitleBefore,
    DefinitionAfter,
    DefinitionAfterWhitespace,
    DefinitionTitleBeforeMarker,
    DefinitionTitleAfter,
    DefinitionTitleAfterOptionalWhitespace,
    DestinationStart,
    DestinationEnclosedBefore,
    DestinationEnclosed,
    DestinationEnclosedEscape,
    DestinationRaw,
    DestinationRawEscape,
    DocumentStart,
    DocumentBeforeFrontmatter,
    DocumentContainerExistingBefore,
    DocumentContainerExistingAfter,
    DocumentContainerNewBefore,
    DocumentContainerNewBeforeNotBlockQuote,
    DocumentContainerNewBeforeNotList,
    DocumentContainerNewBeforeNotGfmFootnoteDefinition,
    DocumentContainerNewAfter,
    DocumentContainersAfter,
    DocumentFlowInside,
    DocumentFlowEnd,
    FlowStart,
    FlowBeforeGfmTable,
    FlowBeforeCodeIndented,
    FlowBeforeRaw,
    FlowBeforeHtml,
    FlowBeforeHeadingAtx,
    FlowBeforeHeadingSetext,
    FlowBeforeThematicBreak,
    FlowAfter,
    FlowBlankLineBefore,
    FlowBlankLineAfter,
    FlowBeforeContent,
    FrontmatterStart,
    FrontmatterOpenSequence,
    FrontmatterOpenAfter,
    FrontmatterAfter,
    FrontmatterContentStart,
    FrontmatterContentInside,
    FrontmatterContentEnd,
    FrontmatterCloseStart,
    FrontmatterCloseSequence,
    FrontmatterCloseAfter,
    GfmAutolinkLiteralProtocolStart,
    GfmAutolinkLiteralProtocolAfter,
    GfmAutolinkLiteralProtocolPrefixInside,
    GfmAutolinkLiteralProtocolSlashesInside,
    GfmAutolinkLiteralWwwStart,
    GfmAutolinkLiteralWwwAfter,
    GfmAutolinkLiteralWwwPrefixInside,
    GfmAutolinkLiteralWwwPrefixAfter,
    GfmAutolinkLiteralDomainInside,
    GfmAutolinkLiteralDomainAtPunctuation,
    GfmAutolinkLiteralDomainAfter,
    GfmAutolinkLiteralPathInside,
    GfmAutolinkLiteralPathAtPunctuation,
    GfmAutolinkLiteralPathAfter,
    GfmAutolinkLiteralTrail,
    GfmAutolinkLiteralTrailCharRefInside,
    GfmAutolinkLiteralTrailCharRefStart,
    GfmAutolinkLiteralTrailBracketAfter,
    GfmFootnoteDefinitionStart,
    GfmFootnoteDefinitionLabelBefore,
    GfmFootnoteDefinitionLabelAtMarker,
    GfmFootnoteDefinitionLabelInside,
    GfmFootnoteDefinitionLabelEscape,
    GfmFootnoteDefinitionLabelAfter,
    GfmFootnoteDefinitionWhitespaceAfter,
    GfmFootnoteDefinitionContStart,
    GfmFootnoteDefinitionContBlank,
    GfmFootnoteDefinitionContFilled,
    GfmLabelStartFootnoteStart,
    GfmLabelStartFootnoteOpen,
    GfmTaskListItemCheckStart,
    GfmTaskListItemCheckInside,
    GfmTaskListItemCheckClose,
    GfmTaskListItemCheckAfter,
    GfmTaskListItemCheckAfterSpaceOrTab,
    GfmTableStart,
    GfmTableHeadRowBefore,
    GfmTableHeadRowStart,
    GfmTableHeadRowBreak,
    GfmTableHeadRowData,
    GfmTableHeadRowEscape,
    GfmTableHeadDelimiterStart,
    GfmTableHeadDelimiterBefore,
    GfmTableHeadDelimiterCellBefore,
    GfmTableHeadDelimiterValueBefore,
    GfmTableHeadDelimiterLeftAlignmentAfter,
    GfmTableHeadDelimiterFiller,
    GfmTableHeadDelimiterRightAlignmentAfter,
    GfmTableHeadDelimiterCellAfter,
    GfmTableHeadDelimiterNok,
    GfmTableBodyRowStart,
    GfmTableBodyRowBreak,
    GfmTableBodyRowData,
    GfmTableBodyRowEscape,
    HardBreakEscapeStart,
    HardBreakEscapeAfter,
    HeadingAtxStart,
    HeadingAtxBefore,
    HeadingAtxSequenceOpen,
    HeadingAtxAtBreak,
    HeadingAtxSequenceFurther,
    HeadingAtxData,
    HeadingSetextStart,
    HeadingSetextBefore,
    HeadingSetextInside,
    HeadingSetextAfter,
    HtmlFlowStart,
    HtmlFlowBefore,
    HtmlFlowOpen,
    HtmlFlowDeclarationOpen,
    HtmlFlowCommentOpenInside,
    HtmlFlowCdataOpenInside,
    HtmlFlowTagCloseStart,
    HtmlFlowTagName,
    HtmlFlowBasicSelfClosing,
    HtmlFlowCompleteClosingTagAfter,
    HtmlFlowCompleteEnd,
    HtmlFlowCompleteAttributeNameBefore,
    HtmlFlowCompleteAttributeName,
    HtmlFlowCompleteAttributeNameAfter,
    HtmlFlowCompleteAttributeValueBefore,
    HtmlFlowCompleteAttributeValueQuoted,
    HtmlFlowCompleteAttributeValueQuotedAfter,
    HtmlFlowCompleteAttributeValueUnquoted,
    HtmlFlowCompleteAfter,
    HtmlFlowBlankLineBefore,
    HtmlFlowContinuation,
    HtmlFlowContinuationDeclarationInside,
    HtmlFlowContinuationAfter,
    HtmlFlowContinuationStart,
    HtmlFlowContinuationBefore,
    HtmlFlowContinuationCommentInside,
    HtmlFlowContinuationRawTagOpen,
    HtmlFlowContinuationRawEndTag,
    HtmlFlowContinuationClose,
    HtmlFlowContinuationCdataInside,
    HtmlFlowContinuationStartNonLazy,
    HtmlTextStart,
    HtmlTextOpen,
    HtmlTextDeclarationOpen,
    HtmlTextTagCloseStart,
    HtmlTextTagClose,
    HtmlTextTagCloseBetween,
    HtmlTextTagOpen,
    HtmlTextTagOpenBetween,
    HtmlTextTagOpenAttributeName,
    HtmlTextTagOpenAttributeNameAfter,
    HtmlTextTagOpenAttributeValueBefore,
    HtmlTextTagOpenAttributeValueQuoted,
    HtmlTextTagOpenAttributeValueQuotedAfter,
    HtmlTextTagOpenAttributeValueUnquoted,
    HtmlTextCdata,
    HtmlTextCdataOpenInside,
    HtmlTextCdataClose,
    HtmlTextCdataEnd,
    HtmlTextCommentOpenInside,
    HtmlTextComment,
    HtmlTextCommentClose,
    HtmlTextCommentEnd,
    HtmlTextDeclaration,
    HtmlTextEnd,
    HtmlTextInstruction,
    HtmlTextInstructionClose,
    HtmlTextLineEndingBefore,
    HtmlTextLineEndingAfter,
    HtmlTextLineEndingAfterPrefix,
    LabelStart,
    LabelAtBreak,
    LabelEolAfter,
    LabelEscape,
    LabelInside,
    LabelNok,
    LabelEndStart,
    LabelEndAfter,
    LabelEndResourceStart,
    LabelEndResourceBefore,
    LabelEndResourceOpen,
    LabelEndResourceDestinationAfter,
    LabelEndResourceDestinationMissing,
    LabelEndResourceBetween,
    LabelEndResourceTitleAfter,
    LabelEndResourceEnd,
    LabelEndOk,
    LabelEndNok,
    LabelEndReferenceFull,
    LabelEndReferenceFullAfter,
    LabelEndReferenceFullMissing,
    LabelEndReferenceNotFull,
    LabelEndReferenceCollapsed,
    LabelEndReferenceCollapsedOpen,
    LabelStartImageStart,
    LabelStartImageOpen,
    LabelStartImageAfter,
    LabelStartLinkStart,
    ListItemStart,
    ListItemBefore,
    ListItemBeforeOrdered,
    ListItemBeforeUnordered,
    ListItemValue,
    ListItemMarker,
    ListItemMarkerAfter,
    ListItemAfter,
    ListItemMarkerAfterFilled,
    ListItemWhitespace,
    ListItemPrefixOther,
    ListItemWhitespaceAfter,
    ListItemContStart,
    ListItemContBlank,
    ListItemContFilled,
    NonLazyContinuationStart,
    NonLazyContinuationAfter,
    ParagraphStart,
    ParagraphLineStart,
    ParagraphInside,
    RawFlowStart,
    RawFlowBeforeSequenceOpen,
    RawFlowSequenceOpen,
    RawFlowInfoBefore,
    RawFlowInfo,
    RawFlowMetaBefore,
    RawFlowMeta,
    RawFlowAtNonLazyBreak,
    RawFlowCloseStart,
    RawFlowBeforeSequenceClose,
    RawFlowSequenceClose,
    RawFlowAfterSequenceClose,
    RawFlowContentBefore,
    RawFlowContentStart,
    RawFlowBeforeContentChunk,
    RawFlowContentChunk,
    RawFlowAfter,
    RawTextStart,
    RawTextSequenceOpen,
    RawTextBetween,
    RawTextData,
    RawTextSequenceClose,
    SpaceOrTabStart,
    SpaceOrTabInside,
    SpaceOrTabAfter,
    SpaceOrTabEolStart,
    SpaceOrTabEolAfterFirst,
    SpaceOrTabEolAfterEol,
    SpaceOrTabEolAtEol,
    SpaceOrTabEolAfterMore,
    StringStart,
    StringBefore,
    StringBeforeData,
    TextStart,
    TextBefore,
    TextBeforeHtml,
    TextBeforeHardBreakEscape,
    TextBeforeLabelStartLink,
    TextBeforeData,
    ThematicBreakStart,
    ThematicBreakBefore,
    ThematicBreakSequence,
    ThematicBreakAtBreak,
    TitleStart,
    TitleBegin,
    TitleAfterEol,
    TitleAtBreak,
    TitleEscape,
    TitleInside,
    TitleNok,
    Count,
};

struct State {
    enum class Kind : uint8_t {
        Ok,
        Nok,
        Next,
        Retry,
    };

    Kind kind = Kind::Ok;
    StateName name = StateName::AttentionStart;

    bool operator==(const State& other) const {
        if (kind != other.kind) {
            return false;
        }
        if (kind == Kind::Next || kind == Kind::Retry) {
            return name == other.name;
        }
        return true;
    }
    bool operator!=(const State& other) const { return !(*this == other); }
};

inline State StateNext(StateName name) {
    return State{State::Kind::Next, name};
}
inline State StateRetry(StateName name) {
    return State{State::Kind::Retry, name};
}
inline State StateOk() {
    return State{State::Kind::Ok, StateName::AttentionStart};
}
inline State StateNok() {
    return State{State::Kind::Nok, StateName::AttentionStart};
}

State Call(Tokenizer* tokenizer, StateName name);

}

#endif

#if GPUI_INCLUDE_PRIVATE_API
#line 1 "src/markdown/event.h"

namespace markdown {

using base::Str;
using base::Vec;

enum class Name : uint8_t {
    AttentionSequence,
    Autolink,
    AutolinkEmail,
    AutolinkMarker,
    AutolinkProtocol,
    BlankLineEnding,
    BlockQuote,
    BlockQuoteMarker,
    BlockQuotePrefix,
    ByteOrderMark,
    CharacterEscape,
    CharacterEscapeMarker,
    CharacterEscapeValue,
    CharacterReference,
    CharacterReferenceMarker,
    CharacterReferenceMarkerHexadecimal,
    CharacterReferenceMarkerNumeric,
    CharacterReferenceMarkerSemi,
    CharacterReferenceValue,
    CodeFenced,
    CodeFencedFence,
    CodeFencedFenceInfo,
    CodeFencedFenceMeta,
    CodeFencedFenceSequence,
    CodeFlowChunk,
    CodeIndented,
    CodeText,
    CodeTextData,
    CodeTextSequence,
    Content,
    Data,
    Definition,
    DefinitionDestination,
    DefinitionDestinationLiteral,
    DefinitionDestinationLiteralMarker,
    DefinitionDestinationRaw,
    DefinitionDestinationString,
    DefinitionLabel,
    DefinitionLabelMarker,
    DefinitionLabelString,
    DefinitionMarker,
    DefinitionTitle,
    DefinitionTitleMarker,
    DefinitionTitleString,
    Emphasis,
    EmphasisSequence,
    EmphasisText,
    Frontmatter,
    FrontmatterChunk,
    FrontmatterFence,
    FrontmatterSequence,
    GfmAutolinkLiteralEmail,
    GfmAutolinkLiteralMailto,
    GfmAutolinkLiteralProtocol,
    GfmAutolinkLiteralWww,
    GfmAutolinkLiteralXmpp,
    GfmFootnoteCall,
    GfmFootnoteCallLabel,
    GfmFootnoteCallMarker,
    GfmFootnoteDefinition,
    GfmFootnoteDefinitionPrefix,
    GfmFootnoteDefinitionLabel,
    GfmFootnoteDefinitionLabelMarker,
    GfmFootnoteDefinitionLabelString,
    GfmFootnoteDefinitionMarker,
    GfmStrikethrough,
    GfmStrikethroughSequence,
    GfmStrikethroughText,
    GfmTable,
    GfmTableBody,
    GfmTableCell,
    GfmTableCellText,
    GfmTableCellDivider,
    GfmTableDelimiterRow,
    GfmTableDelimiterMarker,
    GfmTableDelimiterCell,
    GfmTableDelimiterCellValue,
    GfmTableDelimiterFiller,
    GfmTableHead,
    GfmTableRow,
    GfmTaskListItemCheck,
    GfmTaskListItemMarker,
    GfmTaskListItemValueChecked,
    GfmTaskListItemValueUnchecked,
    HardBreakEscape,
    HardBreakTrailing,
    HeadingAtx,
    HeadingAtxSequence,
    HeadingAtxText,
    HeadingSetext,
    HeadingSetextText,
    HeadingSetextUnderline,
    HeadingSetextUnderlineSequence,
    HtmlFlow,
    HtmlFlowData,
    HtmlText,
    HtmlTextData,
    Image,
    Label,
    LabelEnd,
    LabelImage,
    LabelImageMarker,
    LabelLink,
    LabelMarker,
    LabelText,
    LineEnding,
    Link,
    ListItem,
    ListItemMarker,
    ListItemPrefix,
    ListItemValue,
    ListOrdered,
    ListUnordered,
    MathFlow,
    MathFlowFence,
    MathFlowFenceMeta,
    MathFlowFenceSequence,
    MathFlowChunk,
    MathText,
    MathTextData,
    MathTextSequence,
    Paragraph,
    Reference,
    ReferenceMarker,
    ReferenceString,
    Resource,
    ResourceDestination,
    ResourceDestinationLiteral,
    ResourceDestinationLiteralMarker,
    ResourceDestinationRaw,
    ResourceDestinationString,
    ResourceMarker,
    ResourceTitle,
    ResourceTitleMarker,
    ResourceTitleString,
    SpaceOrTab,
    Strong,
    StrongSequence,
    StrongText,
    ThematicBreak,
    ThematicBreakSequence,
    LinePrefix,
};

bool IsVoidEvent(Name name);

enum class ContentKind : uint8_t {
    Flow,
    Content,
    String,
    Text,
};

struct Link {
    int32_t previous = -1;
    int32_t next = -1;
    ContentKind content = ContentKind::Flow;
};

struct Point {
    int32_t line = 1;
    int32_t column = 1;
    int32_t index = 0;
    int32_t vs = 0;
};

Point PointShiftTo(const Point& from, Str bytes, int32_t index);

enum class Kind : uint8_t {
    Enter,
    Exit,
};

struct Event {
    Kind kind = Kind::Enter;
    Name name = Name::Data;
    bool hasLink = false;
    Point point = {};
    Link link = {};
};

}

#endif

#if GPUI_INCLUDE_PRIVATE_API
#line 1 "src/markdown/util.h"

namespace markdown {

using base::Arena;
using base::ArenaVec;

Str StrOwn(Arena* a, Str s);
Str StrOwn(Arena* a, const char* s, int32_t len);

enum class CharKind : uint8_t {
    Whitespace,
    Punctuation,
    Other,
};

bool IsUnicodePunctuation(uint32_t cp);

int32_t CharBeforeIndex(Str bytes, int32_t index);
int32_t CharAfterIndex(Str bytes, int32_t index);

CharKind Classify(int32_t cp);
CharKind KindAfterIndex(Str bytes, int32_t index);

inline bool IsAsciiWhitespace(uint8_t b) {
    return b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == 0x0c;
}
inline bool IsAsciiPunctuation(uint8_t b) {
    return (b >= '!' && b <= '/') || (b >= ':' && b <= '@') ||
           (b >= '[' && b <= '`') || (b >= '{' && b <= '~');
}
inline bool IsAsciiDigit(uint8_t b) {
    return b >= '0' && b <= '9';
}
inline bool IsAsciiHexDigit(uint8_t b) {
    return IsAsciiDigit(b) || (b >= 'a' && b <= 'f') || (b >= 'A' && b <= 'F');
}
inline bool IsAsciiAlpha(uint8_t b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z');
}
inline bool IsAsciiAlphanumeric(uint8_t b) {
    return IsAsciiAlpha(b) || IsAsciiDigit(b);
}
inline bool IsAsciiControl(uint8_t b) {
    return b < 0x20 || b == 0x7f;
}

int32_t Utf8Encode(char* out, uint32_t cp);

struct Position {
    Point start = {};
    Point end = {};
};

Position PositionFromExitEvent(const Vec<Event>& events, int32_t index);

struct Slice {
    Str bytes = {};
    int32_t before = 0;
    int32_t after = 0;

    int32_t Len() const { return len(bytes) + before + after; }
};

Slice SliceFromPosition(Str bytes, const Position& position);
Slice SliceFromIndices(Str bytes, int32_t start, int32_t end);

Str SliceSerialize(Arena* a, const Slice& slice);

struct EditMap {
    struct Entry {
        int32_t at = 0;
        int32_t remove = 0;
        ArenaVec<Event> add{};
    };

    Arena* a = nullptr;
    Vec<Entry> map;

    Vec<int32_t> buckets;
};

void EditMapAdd(EditMap& map, int32_t index, int32_t remove, const Event* add,
                int32_t addLen);
void EditMapAddBefore(EditMap& map, int32_t index, int32_t remove,
                      const Event* add, int32_t addLen);
inline bool EditMapEmpty(const EditMap& map) {
    return map.map.len == 0;
}
void EditMapConsume(EditMap& map, Vec<Event>& events);

int32_t SkipOpt(const Vec<Event>& events, int32_t index, const Name* names,
                int32_t namesLen);
int32_t SkipOptBack(const Vec<Event>& events, int32_t index, const Name* names,
                    int32_t namesLen);
int32_t SkipTo(const Vec<Event>& events, int32_t index, const Name* names,
               int32_t namesLen);
int32_t SkipToBack(const Vec<Event>& events, int32_t index, const Name* names,
                   int32_t namesLen);

Str NormalizeIdentifier(Arena* a, Str value);

bool ListLoose(const Vec<Event>& events, int32_t index, bool includeItems);
bool ListItemLoose(const Vec<Event>& events, int32_t index);

ArenaAlign GfmTableAlign(const Vec<Event>& events, int32_t index, Arena* a);

int32_t CharacterReferenceValueMax(uint8_t marker);
bool CharacterReferenceValueTest(uint8_t marker, uint8_t byte);

Str CharacterReferenceDecode(Arena* a, Str value, uint8_t marker);

base::TempStr CharacterReferenceDecodeTemp(Str value, uint8_t marker);

}

#endif

#if GPUI_INCLUDE_PRIVATE_API
#line 1 "src/markdown/tokenizer.h"

namespace markdown {

enum class Container : uint8_t {
    BlockQuote,
    ListItem,
    GfmFootnoteDefinition,
};

struct ContainerState {
    Container kind = Container::BlockQuote;
    bool blankInitial = false;
    int32_t size = 0;
};

enum class LabelKind : uint8_t {
    Image,
    Link,
    GfmFootnote,
    GfmUndefinedFootnote,
};

struct LabelStartMark {
    LabelKind kind = LabelKind::Image;
    int32_t startA = 0;
    int32_t startB = 0;
    bool inactive = false;
};

struct Label {
    LabelKind kind = LabelKind::Image;
    int32_t startA = 0;
    int32_t startB = 0;
    int32_t endA = 0;
    int32_t endB = 0;
};

enum class ResolveName : uint8_t {
    Label,
    Attention,
    GfmTable,
    HeadingAtx,
    HeadingSetext,
    ListItem,
    Content,
    Data,
    String,
    Text,
};

struct Subresult {
    bool done = false;
    Vec<Str> gfmFootnoteDefinitions;
    Vec<Str> definitions;
};

void SubresultAppend(Subresult& dst, Subresult& src);

struct Tokenizer;

struct ParseState {

    Arena* a = nullptr;

    Arena* scratch = nullptr;
    const ParseOptions* options = nullptr;
    Str bytes = {};
    Vec<Str> definitions;
    Vec<Str> gfmFootnoteDefinitions;
};

struct TokenizeState {
    Tokenizer* documentChild = nullptr;
    State documentChildState = {};
    bool documentChildStateSome = false;
    Vec<ContainerState> documentContainerStack;
    int32_t documentContinued = 0;

    int32_t documentDataIndex = -1;

    Vec<ArenaVec<Event>> documentExits;
    bool documentLazyAcceptingBefore = false;
    bool documentAtFirstParagraphOfListItem = false;

    ContentKind spaceOrTabEolContent = ContentKind::Flow;
    bool spaceOrTabEolContentSome = false;
    bool spaceOrTabEolConnect = false;
    bool spaceOrTabEolOk = false;
    bool spaceOrTabConnect = false;
    ContentKind spaceOrTabContent = ContentKind::Flow;
    bool spaceOrTabContentSome = false;
    int32_t spaceOrTabMin = 0;
    int32_t spaceOrTabMax = 0;
    int32_t spaceOrTabSize = 0;
    Name spaceOrTabToken = Name::SpaceOrTab;

    Vec<LabelStartMark> labelStarts;
    Vec<LabelStartMark> labelStartsLoose;
    Vec<Label> labels;
    Vec<Str> definitions;
    Vec<Str> gfmFootnoteDefinitions;

    bool connect = false;
    uint8_t marker = 0;
    uint8_t markerB = 0;
    const uint8_t* markers = nullptr;
    int32_t markersLen = 0;
    bool seen = false;
    int32_t size = 0;
    int32_t sizeB = 0;
    int32_t sizeC = 0;
    int32_t start = 0;
    int32_t end = 0;
    Name token1 = Name::Data;
    Name token2 = Name::Data;
    Name token3 = Name::Data;
    Name token4 = Name::Data;
    Name token5 = Name::Data;
    Name token6 = Name::Data;
};

struct IndexVs {
    int32_t index = 0;
    int32_t vs = 0;
};

struct Progress {
    int32_t eventsLen = 0;
    int32_t stackLen = 0;
    int32_t previous = -1;
    int32_t current = -1;
    Point point = {};
};

struct Attempt {
    State ok = {};
    State nok = {};

    bool check = false;
    bool hasProgress = false;
    Progress progress = {};
};

struct Tokenizer {

    Vec<IndexVs> columnStart;
    int32_t firstLine = 1;
    Point lineStart = {};
    bool consumed = true;
    Vec<Attempt> attempts;

    int32_t current = -1;
    int32_t previous = -1;
    Point point = {};
    Vec<Event> events;
    Vec<Name> stack;
    EditMap map;
    Vec<ResolveName> resolvers;
    ParseState* parseState = nullptr;
    TokenizeState tokenizeState;
    bool interrupt = false;
    bool concrete = false;
    bool pierce = false;
    bool lazy = false;
};

Tokenizer* TokenizerNew(Point point, ParseState* parseState);
void TokenizerFree(Tokenizer* tokenizer);

void RegisterResolver(Tokenizer* t, ResolveName name);
void RegisterResolverBefore(Tokenizer* t, ResolveName name);
void DefineSkip(Tokenizer* t, Point point);
void Consume(Tokenizer* t);
void Enter(Tokenizer* t, Name name);
void EnterLink(Tokenizer* t, Name name, Link link);
void Exit(Tokenizer* t, Name name);

void TokenizerCheck(Tokenizer* t, State ok, State nok);
void TokenizerAttempt(Tokenizer* t, State ok, State nok);

State Push(Tokenizer* t, int32_t fromIndex, int32_t fromVs, int32_t toIndex,
           int32_t toVs, State state);
Subresult Flush(Tokenizer* t, State state, bool resolve);

bool ResolveCall(Tokenizer* t, ResolveName name, Subresult* out);

void SubtokenizeLink(Vec<Event>& events, int32_t index);
void SubtokenizeLinkTo(Vec<Event>& events, int32_t previous, int32_t next);

Subresult Subtokenize(Vec<Event>& events, ParseState* parseState,
                      bool hasFilter, ContentKind filter);
void DivideEvents(EditMap& map, const Vec<Event>& events, int32_t linkIndex,
                  Vec<Event>& childEvents, int32_t* accA, int32_t* accB);

Vec<Event> Parse(ParseState* parseState);

Node* ToMdastCompile(const Vec<Event>& events, ParseState* parseState);

}

#endif

#if GPUI_INCLUDE_PRIVATE_API
#line 1 "src/markdown/construct.h"

namespace markdown {

State AttentionStart(Tokenizer* t);
State AttentionInside(Tokenizer* t);
State AutolinkStart(Tokenizer* t);
State AutolinkOpen(Tokenizer* t);
State AutolinkSchemeOrEmailAtext(Tokenizer* t);
State AutolinkSchemeInsideOrEmailAtext(Tokenizer* t);
State AutolinkUrlInside(Tokenizer* t);
State AutolinkEmailAtSignOrDot(Tokenizer* t);
State AutolinkEmailAtext(Tokenizer* t);
State AutolinkEmailValue(Tokenizer* t);
State AutolinkEmailLabel(Tokenizer* t);
State BlankLineStart(Tokenizer* t);
State BlankLineAfter(Tokenizer* t);
State BlockQuoteStart(Tokenizer* t);
State BlockQuoteContStart(Tokenizer* t);
State BlockQuoteContBefore(Tokenizer* t);
State BlockQuoteContAfter(Tokenizer* t);
State BomStart(Tokenizer* t);
State BomInside(Tokenizer* t);
State CharacterEscapeStart(Tokenizer* t);
State CharacterEscapeInside(Tokenizer* t);
State CharacterReferenceStart(Tokenizer* t);
State CharacterReferenceOpen(Tokenizer* t);
State CharacterReferenceNumeric(Tokenizer* t);
State CharacterReferenceValue(Tokenizer* t);
State CodeIndentedStart(Tokenizer* t);
State CodeIndentedAtBreak(Tokenizer* t);
State CodeIndentedAfter(Tokenizer* t);
State CodeIndentedFurtherStart(Tokenizer* t);
State CodeIndentedInside(Tokenizer* t);
State CodeIndentedFurtherBegin(Tokenizer* t);
State CodeIndentedFurtherAfter(Tokenizer* t);
State ContentChunkStart(Tokenizer* t);
State ContentChunkInside(Tokenizer* t);
State ContentDefinitionBefore(Tokenizer* t);
State ContentDefinitionAfter(Tokenizer* t);
State DataStart(Tokenizer* t);
State DataInside(Tokenizer* t);
State DataAtBreak(Tokenizer* t);
State DefinitionStart(Tokenizer* t);
State DefinitionBefore(Tokenizer* t);
State DefinitionLabelAfter(Tokenizer* t);
State DefinitionLabelNok(Tokenizer* t);
State DefinitionMarkerAfter(Tokenizer* t);
State DefinitionDestinationBefore(Tokenizer* t);
State DefinitionDestinationAfter(Tokenizer* t);
State DefinitionDestinationMissing(Tokenizer* t);
State DefinitionTitleBefore(Tokenizer* t);
State DefinitionAfter(Tokenizer* t);
State DefinitionAfterWhitespace(Tokenizer* t);
State DefinitionTitleBeforeMarker(Tokenizer* t);
State DefinitionTitleAfter(Tokenizer* t);
State DefinitionTitleAfterOptionalWhitespace(Tokenizer* t);
State DestinationStart(Tokenizer* t);
State DestinationEnclosedBefore(Tokenizer* t);
State DestinationEnclosed(Tokenizer* t);
State DestinationEnclosedEscape(Tokenizer* t);
State DestinationRaw(Tokenizer* t);
State DestinationRawEscape(Tokenizer* t);
State DocumentStart(Tokenizer* t);
State DocumentBeforeFrontmatter(Tokenizer* t);
State DocumentContainerExistingBefore(Tokenizer* t);
State DocumentContainerExistingAfter(Tokenizer* t);
State DocumentContainerNewBefore(Tokenizer* t);
State DocumentContainerNewBeforeNotBlockQuote(Tokenizer* t);
State DocumentContainerNewBeforeNotList(Tokenizer* t);
State DocumentContainerNewBeforeNotGfmFootnoteDefinition(Tokenizer* t);
State DocumentContainerNewAfter(Tokenizer* t);
State DocumentContainersAfter(Tokenizer* t);
State DocumentFlowInside(Tokenizer* t);
State DocumentFlowEnd(Tokenizer* t);
State FlowStart(Tokenizer* t);
State FlowBeforeGfmTable(Tokenizer* t);
State FlowBeforeCodeIndented(Tokenizer* t);
State FlowBeforeRaw(Tokenizer* t);
State FlowBeforeHtml(Tokenizer* t);
State FlowBeforeHeadingAtx(Tokenizer* t);
State FlowBeforeHeadingSetext(Tokenizer* t);
State FlowBeforeThematicBreak(Tokenizer* t);
State FlowAfter(Tokenizer* t);
State FlowBlankLineBefore(Tokenizer* t);
State FlowBlankLineAfter(Tokenizer* t);
State FlowBeforeContent(Tokenizer* t);
State FrontmatterStart(Tokenizer* t);
State FrontmatterOpenSequence(Tokenizer* t);
State FrontmatterOpenAfter(Tokenizer* t);
State FrontmatterAfter(Tokenizer* t);
State FrontmatterContentStart(Tokenizer* t);
State FrontmatterContentInside(Tokenizer* t);
State FrontmatterContentEnd(Tokenizer* t);
State FrontmatterCloseStart(Tokenizer* t);
State FrontmatterCloseSequence(Tokenizer* t);
State FrontmatterCloseAfter(Tokenizer* t);
State GfmAutolinkLiteralProtocolStart(Tokenizer* t);
State GfmAutolinkLiteralProtocolAfter(Tokenizer* t);
State GfmAutolinkLiteralProtocolPrefixInside(Tokenizer* t);
State GfmAutolinkLiteralProtocolSlashesInside(Tokenizer* t);
State GfmAutolinkLiteralWwwStart(Tokenizer* t);
State GfmAutolinkLiteralWwwAfter(Tokenizer* t);
State GfmAutolinkLiteralWwwPrefixInside(Tokenizer* t);
State GfmAutolinkLiteralWwwPrefixAfter(Tokenizer* t);
State GfmAutolinkLiteralDomainInside(Tokenizer* t);
State GfmAutolinkLiteralDomainAtPunctuation(Tokenizer* t);
State GfmAutolinkLiteralDomainAfter(Tokenizer* t);
State GfmAutolinkLiteralPathInside(Tokenizer* t);
State GfmAutolinkLiteralPathAtPunctuation(Tokenizer* t);
State GfmAutolinkLiteralPathAfter(Tokenizer* t);
State GfmAutolinkLiteralTrail(Tokenizer* t);
State GfmAutolinkLiteralTrailCharRefInside(Tokenizer* t);
State GfmAutolinkLiteralTrailCharRefStart(Tokenizer* t);
State GfmAutolinkLiteralTrailBracketAfter(Tokenizer* t);
State GfmFootnoteDefinitionStart(Tokenizer* t);
State GfmFootnoteDefinitionLabelBefore(Tokenizer* t);
State GfmFootnoteDefinitionLabelAtMarker(Tokenizer* t);
State GfmFootnoteDefinitionLabelInside(Tokenizer* t);
State GfmFootnoteDefinitionLabelEscape(Tokenizer* t);
State GfmFootnoteDefinitionLabelAfter(Tokenizer* t);
State GfmFootnoteDefinitionWhitespaceAfter(Tokenizer* t);
State GfmFootnoteDefinitionContStart(Tokenizer* t);
State GfmFootnoteDefinitionContBlank(Tokenizer* t);
State GfmFootnoteDefinitionContFilled(Tokenizer* t);
State GfmLabelStartFootnoteStart(Tokenizer* t);
State GfmLabelStartFootnoteOpen(Tokenizer* t);
State GfmTaskListItemCheckStart(Tokenizer* t);
State GfmTaskListItemCheckInside(Tokenizer* t);
State GfmTaskListItemCheckClose(Tokenizer* t);
State GfmTaskListItemCheckAfter(Tokenizer* t);
State GfmTaskListItemCheckAfterSpaceOrTab(Tokenizer* t);
State GfmTableStart(Tokenizer* t);
State GfmTableHeadRowBefore(Tokenizer* t);
State GfmTableHeadRowStart(Tokenizer* t);
State GfmTableHeadRowBreak(Tokenizer* t);
State GfmTableHeadRowData(Tokenizer* t);
State GfmTableHeadRowEscape(Tokenizer* t);
State GfmTableHeadDelimiterStart(Tokenizer* t);
State GfmTableHeadDelimiterBefore(Tokenizer* t);
State GfmTableHeadDelimiterCellBefore(Tokenizer* t);
State GfmTableHeadDelimiterValueBefore(Tokenizer* t);
State GfmTableHeadDelimiterLeftAlignmentAfter(Tokenizer* t);
State GfmTableHeadDelimiterFiller(Tokenizer* t);
State GfmTableHeadDelimiterRightAlignmentAfter(Tokenizer* t);
State GfmTableHeadDelimiterCellAfter(Tokenizer* t);
State GfmTableHeadDelimiterNok(Tokenizer* t);
State GfmTableBodyRowStart(Tokenizer* t);
State GfmTableBodyRowBreak(Tokenizer* t);
State GfmTableBodyRowData(Tokenizer* t);
State GfmTableBodyRowEscape(Tokenizer* t);
State HardBreakEscapeStart(Tokenizer* t);
State HardBreakEscapeAfter(Tokenizer* t);
State HeadingAtxStart(Tokenizer* t);
State HeadingAtxBefore(Tokenizer* t);
State HeadingAtxSequenceOpen(Tokenizer* t);
State HeadingAtxAtBreak(Tokenizer* t);
State HeadingAtxSequenceFurther(Tokenizer* t);
State HeadingAtxData(Tokenizer* t);
State HeadingSetextStart(Tokenizer* t);
State HeadingSetextBefore(Tokenizer* t);
State HeadingSetextInside(Tokenizer* t);
State HeadingSetextAfter(Tokenizer* t);
State HtmlFlowStart(Tokenizer* t);
State HtmlFlowBefore(Tokenizer* t);
State HtmlFlowOpen(Tokenizer* t);
State HtmlFlowDeclarationOpen(Tokenizer* t);
State HtmlFlowCommentOpenInside(Tokenizer* t);
State HtmlFlowCdataOpenInside(Tokenizer* t);
State HtmlFlowTagCloseStart(Tokenizer* t);
State HtmlFlowTagName(Tokenizer* t);
State HtmlFlowBasicSelfClosing(Tokenizer* t);
State HtmlFlowCompleteClosingTagAfter(Tokenizer* t);
State HtmlFlowCompleteEnd(Tokenizer* t);
State HtmlFlowCompleteAttributeNameBefore(Tokenizer* t);
State HtmlFlowCompleteAttributeName(Tokenizer* t);
State HtmlFlowCompleteAttributeNameAfter(Tokenizer* t);
State HtmlFlowCompleteAttributeValueBefore(Tokenizer* t);
State HtmlFlowCompleteAttributeValueQuoted(Tokenizer* t);
State HtmlFlowCompleteAttributeValueQuotedAfter(Tokenizer* t);
State HtmlFlowCompleteAttributeValueUnquoted(Tokenizer* t);
State HtmlFlowCompleteAfter(Tokenizer* t);
State HtmlFlowBlankLineBefore(Tokenizer* t);
State HtmlFlowContinuation(Tokenizer* t);
State HtmlFlowContinuationDeclarationInside(Tokenizer* t);
State HtmlFlowContinuationAfter(Tokenizer* t);
State HtmlFlowContinuationStart(Tokenizer* t);
State HtmlFlowContinuationBefore(Tokenizer* t);
State HtmlFlowContinuationCommentInside(Tokenizer* t);
State HtmlFlowContinuationRawTagOpen(Tokenizer* t);
State HtmlFlowContinuationRawEndTag(Tokenizer* t);
State HtmlFlowContinuationClose(Tokenizer* t);
State HtmlFlowContinuationCdataInside(Tokenizer* t);
State HtmlFlowContinuationStartNonLazy(Tokenizer* t);
State HtmlTextStart(Tokenizer* t);
State HtmlTextOpen(Tokenizer* t);
State HtmlTextDeclarationOpen(Tokenizer* t);
State HtmlTextTagCloseStart(Tokenizer* t);
State HtmlTextTagClose(Tokenizer* t);
State HtmlTextTagCloseBetween(Tokenizer* t);
State HtmlTextTagOpen(Tokenizer* t);
State HtmlTextTagOpenBetween(Tokenizer* t);
State HtmlTextTagOpenAttributeName(Tokenizer* t);
State HtmlTextTagOpenAttributeNameAfter(Tokenizer* t);
State HtmlTextTagOpenAttributeValueBefore(Tokenizer* t);
State HtmlTextTagOpenAttributeValueQuoted(Tokenizer* t);
State HtmlTextTagOpenAttributeValueQuotedAfter(Tokenizer* t);
State HtmlTextTagOpenAttributeValueUnquoted(Tokenizer* t);
State HtmlTextCdata(Tokenizer* t);
State HtmlTextCdataOpenInside(Tokenizer* t);
State HtmlTextCdataClose(Tokenizer* t);
State HtmlTextCdataEnd(Tokenizer* t);
State HtmlTextCommentOpenInside(Tokenizer* t);
State HtmlTextComment(Tokenizer* t);
State HtmlTextCommentClose(Tokenizer* t);
State HtmlTextCommentEnd(Tokenizer* t);
State HtmlTextDeclaration(Tokenizer* t);
State HtmlTextEnd(Tokenizer* t);
State HtmlTextInstruction(Tokenizer* t);
State HtmlTextInstructionClose(Tokenizer* t);
State HtmlTextLineEndingBefore(Tokenizer* t);
State HtmlTextLineEndingAfter(Tokenizer* t);
State HtmlTextLineEndingAfterPrefix(Tokenizer* t);
State LabelStart(Tokenizer* t);
State LabelAtBreak(Tokenizer* t);
State LabelEolAfter(Tokenizer* t);
State LabelEscape(Tokenizer* t);
State LabelInside(Tokenizer* t);
State LabelNok(Tokenizer* t);
State LabelEndStart(Tokenizer* t);
State LabelEndAfter(Tokenizer* t);
State LabelEndResourceStart(Tokenizer* t);
State LabelEndResourceBefore(Tokenizer* t);
State LabelEndResourceOpen(Tokenizer* t);
State LabelEndResourceDestinationAfter(Tokenizer* t);
State LabelEndResourceDestinationMissing(Tokenizer* t);
State LabelEndResourceBetween(Tokenizer* t);
State LabelEndResourceTitleAfter(Tokenizer* t);
State LabelEndResourceEnd(Tokenizer* t);
State LabelEndOk(Tokenizer* t);
State LabelEndNok(Tokenizer* t);
State LabelEndReferenceFull(Tokenizer* t);
State LabelEndReferenceFullAfter(Tokenizer* t);
State LabelEndReferenceFullMissing(Tokenizer* t);
State LabelEndReferenceNotFull(Tokenizer* t);
State LabelEndReferenceCollapsed(Tokenizer* t);
State LabelEndReferenceCollapsedOpen(Tokenizer* t);
State LabelStartImageStart(Tokenizer* t);
State LabelStartImageOpen(Tokenizer* t);
State LabelStartImageAfter(Tokenizer* t);
State LabelStartLinkStart(Tokenizer* t);
State ListItemStart(Tokenizer* t);
State ListItemBefore(Tokenizer* t);
State ListItemBeforeOrdered(Tokenizer* t);
State ListItemBeforeUnordered(Tokenizer* t);
State ListItemValue(Tokenizer* t);
State ListItemMarker(Tokenizer* t);
State ListItemMarkerAfter(Tokenizer* t);
State ListItemAfter(Tokenizer* t);
State ListItemMarkerAfterFilled(Tokenizer* t);
State ListItemWhitespace(Tokenizer* t);
State ListItemPrefixOther(Tokenizer* t);
State ListItemWhitespaceAfter(Tokenizer* t);
State ListItemContStart(Tokenizer* t);
State ListItemContBlank(Tokenizer* t);
State ListItemContFilled(Tokenizer* t);
State NonLazyContinuationStart(Tokenizer* t);
State NonLazyContinuationAfter(Tokenizer* t);
State ParagraphStart(Tokenizer* t);
State ParagraphLineStart(Tokenizer* t);
State ParagraphInside(Tokenizer* t);
State RawFlowStart(Tokenizer* t);
State RawFlowBeforeSequenceOpen(Tokenizer* t);
State RawFlowSequenceOpen(Tokenizer* t);
State RawFlowInfoBefore(Tokenizer* t);
State RawFlowInfo(Tokenizer* t);
State RawFlowMetaBefore(Tokenizer* t);
State RawFlowMeta(Tokenizer* t);
State RawFlowAtNonLazyBreak(Tokenizer* t);
State RawFlowCloseStart(Tokenizer* t);
State RawFlowBeforeSequenceClose(Tokenizer* t);
State RawFlowSequenceClose(Tokenizer* t);
State RawFlowAfterSequenceClose(Tokenizer* t);
State RawFlowContentBefore(Tokenizer* t);
State RawFlowContentStart(Tokenizer* t);
State RawFlowBeforeContentChunk(Tokenizer* t);
State RawFlowContentChunk(Tokenizer* t);
State RawFlowAfter(Tokenizer* t);
State RawTextStart(Tokenizer* t);
State RawTextSequenceOpen(Tokenizer* t);
State RawTextBetween(Tokenizer* t);
State RawTextData(Tokenizer* t);
State RawTextSequenceClose(Tokenizer* t);
State SpaceOrTabStart(Tokenizer* t);
State SpaceOrTabInside(Tokenizer* t);
State SpaceOrTabAfter(Tokenizer* t);
State SpaceOrTabEolStart(Tokenizer* t);
State SpaceOrTabEolAfterFirst(Tokenizer* t);
State SpaceOrTabEolAfterEol(Tokenizer* t);
State SpaceOrTabEolAtEol(Tokenizer* t);
State SpaceOrTabEolAfterMore(Tokenizer* t);
State StringStart(Tokenizer* t);
State StringBefore(Tokenizer* t);
State StringBeforeData(Tokenizer* t);
State TextStart(Tokenizer* t);
State TextBefore(Tokenizer* t);
State TextBeforeHtml(Tokenizer* t);
State TextBeforeHardBreakEscape(Tokenizer* t);
State TextBeforeLabelStartLink(Tokenizer* t);
State TextBeforeData(Tokenizer* t);
State ThematicBreakStart(Tokenizer* t);
State ThematicBreakBefore(Tokenizer* t);
State ThematicBreakSequence(Tokenizer* t);
State ThematicBreakAtBreak(Tokenizer* t);
State TitleStart(Tokenizer* t);
State TitleBegin(Tokenizer* t);
State TitleAfterEol(Tokenizer* t);
State TitleAtBreak(Tokenizer* t);
State TitleEscape(Tokenizer* t);
State TitleInside(Tokenizer* t);
State TitleNok(Tokenizer* t);

constexpr int32_t kSizeMax = 0x7fffffff;

struct SpaceOrTabOptions {
    int32_t min = 0;
    int32_t max = 0;
    Name kind = Name::SpaceOrTab;
    bool connect = false;
    ContentKind content = ContentKind::Flow;
    bool contentSome = false;
};

StateName SpaceOrTab(Tokenizer* t);
StateName SpaceOrTabMinMax(Tokenizer* t, int32_t min, int32_t max);
StateName SpaceOrTabWithOptions(Tokenizer* t, const SpaceOrTabOptions& options);

struct SpaceOrTabEolOptions {
    bool connect = false;
    ContentKind content = ContentKind::Flow;
    bool contentSome = false;
};

StateName SpaceOrTabEol(Tokenizer* t);
StateName SpaceOrTabEolWithOptions(Tokenizer* t,
                                   const SpaceOrTabEolOptions& options);

void ResolveWhitespace(Tokenizer* t, bool hardBreak, bool trimWhole);

void GfmAutolinkLiteralResolve(Tokenizer* t);

bool LabelEndResolve(Tokenizer* t, Subresult* out);
bool AttentionResolve(Tokenizer* t, Subresult* out);
bool GfmTableResolve(Tokenizer* t, Subresult* out);
bool HeadingAtxResolve(Tokenizer* t, Subresult* out);
bool HeadingSetextResolve(Tokenizer* t, Subresult* out);
bool ListItemResolve(Tokenizer* t, Subresult* out);
bool ContentResolve(Tokenizer* t, Subresult* out);
bool DataResolve(Tokenizer* t, Subresult* out);
bool StringResolve(Tokenizer* t, Subresult* out);
bool TextResolve(Tokenizer* t, Subresult* out);

}

#endif

#endif
