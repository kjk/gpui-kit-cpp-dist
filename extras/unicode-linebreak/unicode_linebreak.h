#ifndef UNICODE_LINEBREAK_AMALGAM_H_
#define UNICODE_LINEBREAK_AMALGAM_H_
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

using PanicHook = void (*)(const char* msg);

PanicHook SetPanicHook(PanicHook hook);
void Panic(const char* msg);

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

char PlatPathSep();
bool PlatPathsCaseFold();
bool PlatIsWindows();
bool PlatSecondaryIsCommand();
bool PlatShowsWindowControls();
float PlatCaretWidth();
bool PlatScrollBounce();
const char* PlatMonoFontName();
const char* PlatShellDataDir();
const char* PlatShellPlatformName();
bool PlatBlockSelectUsesControl();
bool PlatScrollGestureLocks();
bool PlatAsyncIo();
float PlatWindowShadowSize();

void* AllocZero(int count, int size);

template <typename T>
inline T* AllocArray(int n) {
    return (T*)AllocZero(n, (int)sizeof(T));
}

template <typename T>
inline void ZeroStruct(T* s) {
    memset((void*)s, 0, sizeof(T));
}

constexpr int ClampI(int x, int lo, int hi) {
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    return x;
}
constexpr float ClampF(float x, float lo, float hi) {
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    return x;
}
constexpr double ClampD(double x, double lo, double hi) {
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    return x;
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

struct LocalTime {
    int hour = 0;
    int minute = 0;
    int second = 0;
};

inline bool operator==(LocalTime a, LocalTime b) {
    return a.hour == b.hour && a.minute == b.minute && a.second == b.second;
}
inline bool operator!=(LocalTime a, LocalTime b) {
    return !(a == b);
}

LocalTime TimeOfDayNow();

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

Str StrTrim(Str s);
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
#line 1 "src/unicode-linebreak/tables.h"

namespace unicode_linebreak {
constexpr uint32_t kBreakPropTrieHighStart = 918016;
static const uint16_t kBreakPropTrieIndex[2844] = {
    0,     64,    127,   191,   247,   247,   247,   247,   247,   247,   247,
    304,   368,   417,   481,   247,   247,   247,   542,   247,   558,   607,
    662,   726,   790,   843,   247,   892,   950,   1003,  1029,  1093,  1157,
    1221,  1270,  1324,  1384,  1446,  1509,  1571,  1634,  1696,  1759,  1821,
    1885,  1947,  2009,  2071,  2135,  2197,  2260,  2322,  2386,  2448,  2512,
    2576,  2639,  2703,  2766,  2830,  2894,  2958,  3016,  3080,  3144,  3208,
    3256,  3314,  3378,  3410,  3442,  3482,  247,   3546,  3601,  3663,  3710,
    3747,  3782,  3814,  3878,  247,   247,   247,   247,   247,   247,   247,
    247,   247,   3942,  3974,  4038,  4102,  3144,  4166,  4230,  4262,  4326,
    4374,  4438,  4502,  4566,  4620,  4661,  4694,  4758,  4807,  4871,  4930,
    4994,  5052,  5112,  5176,  5240,  5301,  247,   247,   247,   5365,  247,
    247,   247,   247,   5429,  5487,  553,   5551,  5615,  5677,  5741,  5803,
    5867,  5911,  5969,  6015,  6079,  6141,  6203,  6267,  6323,  247,   247,
    6366,  6418,  6482,  6514,  6515,  6514,  6566,  6630,  6690,  6754,  6818,
    6882,  6943,  7004,  7045,  7099,  7158,  247,   247,   247,   247,   247,
    247,   7219,  7259,  247,   247,   247,   247,   247,   7321,  7375,  247,
    247,   247,   247,   7398,  7462,  7510,  7574,  7606,  7670,  7734,  7798,
    7825,  7889,  7889,  7889,  7931,  7995,  8059,  8120,  8181,  8245,  7889,
    7809,  8294,  8262,  8358,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  247,   7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  8401,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  8452,  8509,
    247,   247,   247,   247,   8573,  8637,  8699,  8731,  247,   247,   247,
    8795,  8857,  8921,  8985,  9043,  9107,  9164,  9228,  9291,  9355,  9419,
    3144,  9480,  9543,  9591,  247,   9639,  9703,  9711,  9719,  9727,  9707,
    9715,  9723,  9703,  9711,  9719,  9727,  9707,  9715,  9723,  9703,  9711,
    9719,  9727,  9707,  9715,  9723,  9703,  9711,  9719,  9727,  9707,  9715,
    9723,  9703,  9711,  9719,  9727,  9707,  9715,  9723,  9703,  9711,  9719,
    9727,  9707,  9715,  9723,  9703,  9711,  9719,  9727,  9707,  9715,  9723,
    9703,  9711,  9719,  9727,  9707,  9715,  9723,  9703,  9711,  9719,  9727,
    9707,  9715,  9723,  9703,  9711,  9719,  9727,  9707,  9715,  9723,  9703,
    9711,  9719,  9727,  9707,  9715,  9723,  9703,  9711,  9719,  9727,  9707,
    9715,  9723,  9703,  9711,  9719,  9727,  9707,  9715,  9723,  9703,  9711,
    9719,  9727,  9707,  9715,  9723,  9703,  9711,  9719,  9727,  9707,  9715,
    9723,  9703,  9711,  9719,  9727,  9707,  9715,  9723,  9703,  9711,  9719,
    9727,  9707,  9715,  9723,  9703,  9711,  9719,  9727,  9707,  9715,  9723,
    9703,  9711,  9719,  9727,  9707,  9715,  9723,  9703,  9711,  9719,  9727,
    9707,  9715,  9723,  9703,  9711,  9719,  9727,  9707,  9715,  9723,  9703,
    9711,  9719,  9727,  9707,  9715,  9723,  9703,  9711,  9719,  9727,  9707,
    9715,  9723,  9703,  9711,  9719,  9727,  9707,  9715,  9723,  9703,  9711,
    9719,  9727,  9707,  9715,  9779,  9836,  9900,  9900,  9900,  9900,  9900,
    9900,  9900,  9900,  9900,  9900,  9900,  9900,  9900,  9900,  9900,  9900,
    9900,  9900,  9900,  9900,  9900,  9900,  9900,  9900,  9900,  9900,  9900,
    9900,  9900,  9900,  9900,  9900,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  10028, 10092, 247,   10153, 247,   247,   247,   247,
    10172, 247,   10236, 10292, 10356, 10416, 247,   10470, 10534, 10596, 10645,
    10708, 2656,  2686,  2715,  2746,  2778,  2778,  2778,  2779,  2778,  2778,
    2778,  2779,  2811,  2811,  2811,  2811,  2811,  2811,  2811,  2811,  2811,
    2811,  2811,  2811,  2811,  2811,  2811,  2811,  2811,  2811,  2811,  2811,
    2811,  2811,  2811,  2811,  2811,  2811,  2811,  2811,  2811,  2811,  2811,
    2811,  2811,  2811,  2811,  2811,  2811,  2811,  2811,  2811,  2843,  503,
    247,   508,   10772, 616,   616,   9964,  9964,  247,   247,   247,   247,
    247,   247,   247,   1253,  10788, 247,   247,   2531,  247,   247,   247,
    247,   500,   2522,  1837,  9964,  9964,  247,   247,   10795, 9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  247,   2522,  247,   247,   247,
    1837,  551,   2055,  247,   247,   7593,  247,   1253,  247,   247,   10811,
    247,   10827, 247,   247,   9631,  10842, 9964,  9964,  247,   247,   247,
    247,   247,   247,   247,   247,   247,   616,   2235,  247,   247,   9631,
    247,   2055,  247,   247,   1995,  247,   247,   247,   10844, 504,   504,
    10859, 513,   10873, 9964,  9964,  9964,  9964,  247,   247,   247,   247,
    247,   247,   247,   247,   247,   247,   247,   247,   247,   247,   247,
    247,   247,   247,   247,   4421,  247,   4422,  1995,  9964,  509,   247,
    247,   10889, 9964,  9964,  9964,  9964,  10905, 247,   247,   10915, 247,
    10930, 247,   247,   247,   500,   783,   9964,  9964,  9964,  247,   10943,
    247,   10954, 247,   1254,  9964,  9964,  9964,  9964,  247,   247,   247,
    9627,  247,   630,   247,   247,   10970, 1769,  247,   10986, 4022,  11002,
    247,   247,   247,   247,   9964,  9964,  247,   247,   11018, 11034, 247,
    247,   247,   11050, 247,   624,   247,   1261,  247,   11066, 781,   9964,
    9964,  9964,  9964,  9964,  247,   247,   247,   247,   4022,  9964,  9964,
    9964,  247,   247,   247,   6454,  247,   247,   247,   4028,  247,   247,
    4052,  2235,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  247,   500,   247,   247,   11082, 6455,  9964,  9964,
    9964,  4858,  247,   247,   1995,  247,   362,   11098, 9964,  247,   11114,
    9964,  9964,  247,   2055,  9964,  247,   4421,  549,   247,   247,   360,
    11130, 630,   2920,  11146, 549,   247,   247,   11161, 11175, 247,   4022,
    2235,  549,   247,   361,   11191, 11207, 247,   247,   11223, 549,   247,
    247,   365,   11239, 11255, 494,   6452,  247,   513,   356,   11271, 11286,
    9964,  9964,  9964,  11302, 501,   11317, 247,   247,   353,   4811,  2235,
    11333, 629,   506,   11348, 1947,  11364, 11378, 4817,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  247,   247,   247,   363,   11394, 11410,
    6455,  9964,  247,   247,   247,   368,   11426, 2235,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  247,   247,   353,   11442,
    11457, 11466, 9964,  9964,  247,   247,   247,   368,   11482, 2235,  11498,
    9964,  247,   247,   357,   11514, 2235,  9964,  9964,  9964,  3144,  2817,
    2686,  11530, 9476,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  247,   247,   356,   1067,  9964,  9964,  9964,  9964,
    9964,  9964,  247,   247,   247,   247,   940,   10032, 11546, 11558, 247,
    11574, 11588, 2235,  9964,  9964,  9964,  9964,  622,   247,   247,   11604,
    11619, 9964,  8688,  247,   247,   11635, 11651, 11667, 247,   247,   358,
    11683, 11698, 247,   247,   247,   247,   4022,  11714, 9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  506,   247,   353,   3033,  11730, 940,   2522,  11746, 247,   3005,
    3032,  4815,  9964,  9964,  9964,  9964,  1801,  247,   247,   11761, 11776,
    2235,  11792, 247,   4674,  11808, 2235,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  247,   11824,
    11840, 493,   247,   11852, 11866, 2235,  9964,  9964,  9964,  9964,  9964,
    1837,  247,   11882, 11897, 11911, 247,   247,   247,   247,   247,   247,
    247,   247,   247,   247,   247,   247,   247,   247,   247,   247,   247,
    247,   247,   247,   247,   247,   247,   247,   247,   247,   247,   247,
    247,   247,   247,   247,   3798,  9964,  9964,  9964,  9964,  9964,  9964,
    247,   247,   247,   247,   247,   247,   500,   11926, 247,   247,   247,
    247,   247,   247,   247,   247,   247,   247,   247,   247,   6453,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  247,   247,   247,   247,   247,   247,   6454,  247,   247,
    247,   247,   247,   11942, 247,   247,   11956, 247,   247,   247,   247,
    247,   247,   247,   247,   247,   247,   247,   247,   247,   247,   11966,
    247,   247,   247,   247,   247,   247,   247,   247,   11982, 11998, 4816,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  247,   247,   247,   247,   247,   247,   247,
    247,   247,   247,   247,   247,   247,   247,   247,   247,   247,   247,
    247,   247,   247,   247,   247,   247,   247,   247,   247,   247,   3955,
    247,   247,   247,   247,   4421,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  247,
    247,   247,   4022,  247,   500,   12014, 247,   247,   247,   247,   500,
    2235,  247,   616,   12030, 247,   247,   247,   12046, 12058, 12074, 513,
    1256,  247,   9964,  9964,  9964,  9964,  9964,  9964,  9964,  247,   247,
    247,   247,   247,   12085, 9964,  9964,  9964,  9964,  9964,  9964,  247,
    247,   247,   247,   2056,  367,   368,   368,   12101, 549,   9964,  9964,
    9964,  9964,  12117, 4820,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7869,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  247,   247,   247,
    247,   247,   247,   247,   247,   247,   247,   247,   247,   247,   247,
    247,   247,   4422,  9964,  9964,  7868,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  1671,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7950,  12131, 9964,  12147, 12159, 7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7865,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  247,   247,   247,   247,   247,   247,
    1253,  2522,  4022,  12175, 4818,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  368,   368,   1000,  368,   4815,  247,   247,
    247,   247,   247,   247,   247,   6453,  9964,  9964,  9964,  247,   247,
    247,   247,   247,   247,   247,   247,   247,   247,   247,   247,   247,
    247,   247,   4422,  247,   247,   623,   247,   247,   247,   12191, 368,
    12204, 247,   12216, 247,   247,   247,   1253,  9964,  247,   247,   247,
    247,   12230, 9964,  9964,  9964,  9964,  9964,  9964,  9964,  247,   6453,
    247,   6453,  247,   247,   247,   247,   247,   4421,  247,   4022,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  247,   247,   247,   247,
    247,   510,   247,   247,   247,   502,   12244, 12258, 511,   247,   247,
    247,   3549,  1670,  247,   3600,  12271, 493,   247,   247,   247,   247,
    247,   247,   247,   247,   247,   247,   624,   247,   247,   247,   247,
    247,   247,   247,   247,   247,   247,   247,   247,   247,   247,   247,
    247,   247,   12281, 12295, 12295, 12295, 368,   368,   368,   11672, 368,
    368,   452,   12311, 12323, 4860,  678,   9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  247,   500,   12335, 9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  3033,
    12351, 12365, 247,   247,   247,   616,   9964,  4856,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  247,   247,   2522,  12381, 9307,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  247,   12397,
    9964,  247,   247,   356,   12413, 9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  247,
    356,   2235,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  12429,
    500,   247,   247,   247,   247,   247,   247,   247,   247,   247,   247,
    247,   247,   625,   4815,  9964,  9964,  247,   247,   247,   247,   12445,
    12461, 9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    494,   247,   247,   10340, 12477, 9964,  9964,  9964,  9964,  494,   247,
    247,   616,   9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  9964,  511,   247,   12492, 12505, 12519, 12535, 12549, 12557,
    505,   2055,  12572, 2055,  9964,  9964,  9964,  6455,  9964,  9964,  9964,
    9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    9964,  9964,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  12588, 6514,  6564,  6514,
    6514,  6514,  12604, 6514,  6514,  6514,  12588, 7889,  7889,  7889,  12617,
    12623, 7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  12639, 12645, 7889,  12652, 12666, 7889,  7889,  12679,
    7889,  7889,  7889,  7889,  12695, 12710, 12720, 12727, 12742, 12756, 12772,
    12786, 7889,  7889,  7889,  7889,  6936,  11875, 12802, 6416,  6933,  7889,
    7889,  12814, 7889,  12830, 7889,  7889,  7889,  12842, 7889,  12854, 7889,
    7889,  7889,  7889,  12865, 247,   247,   12881, 7889,  7889,  12641, 12897,
    12903, 7889,  7889,  7889,  247,   247,   247,   247,   247,   247,   247,
    12919, 247,   247,   247,   247,   247,   12802, 7889,  7889,  6402,  247,
    247,   247,   6935,  6933,  247,   247,   6935,  247,   6400,  7889,  7889,
    7889,  7889,  7889,  12935, 12718, 12751, 12950, 7889,  7889,  7889,  12750,
    7889,  7889,  7889,  12965, 12713, 12980, 7889,  7889,  247,   247,   247,
    247,   247,   12919, 7889,  7889,  7889,  7889,  7889,  7889,  12898, 7889,
    7889,  12702, 247,   247,   247,   247,   247,   247,   247,   247,   247,
    512,   247,   247,   1253,  9964,  9964,  2235,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,  7889,
    7889,  7889,  7889,  7889,  7889,  7863,  2093,  9964,  368,   368,   368,
    368,   368,   368,   9964,  9964,  9964,  9964,  9964,  9964,  9964,  9964,
    368,   368,   368,   368,   368,   368,   368,   368,   368,   368,   368,
    368,   368,   368,   368,   9964,  1077,  1109,  1141,  1173,  1205,  1237,
    1269,  1295,  1327,  1359,  1391,  1423,  1455,  1487,  1519,  1546,  1578,
    1585,  1617,  896,   896,   896,   896,   1638,  1578,  1670,  1699,  896,
    896,   896,   896,   896,   1731,  1760,  896,   896,   896,   896,   896,
    896,   896,   896,   896,   896,   896,   896,   896,   896,   896,   896,
    1578,  1792,  896,   1820,  202,   202,   202,   202,   202,   202,   202,
    202,   1852,  202,   1884,  1903,  896,   896,   896,   896,   896,   896,
    896,   896,   896,   896,   896,   896,   896,   896,   896,   896,   1920,
    1952,  1975,  896,   896,   896,   896,   2007,  896,   896,   896,   896,
    896,   896,   896,   2023,  2055,  2087,  2119,  2141,  1578,  2173,  896,
    2189,  2221,  2244,  2263,  2279,  2311,  896,   2336,  2368,  2400,  2432,
    2464,  2496,  2528,  2560,  202,   2592,  202,   202,   202,   202,   202,
    202,   202,   202,   202,   202,   202,   202,   202,   202,   202,   202,
    202,   202,   202,   202,   202,   202,   202,   202,   202,   202,   202,
    202,   202,   202,   202,   202,   2592,  896,   896,   896,   896,   896,
    896,   896,   896,   896,   896,   896,   896,   896,   896,   896,   896,
    896,   896,   896,   896,   896,   896,   896,   896,   896,   896,   896,
    896,   896,   896,   896,   896,   2624,
};
static const uint8_t kBreakPropTrieData[12996] = {
    3,  3,  3,  3,  3,  3,  3,  3,  3,  12, 2,  0,  0,  1,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  9,  18, 22, 29, 26, 25,
    29, 22, 21, 17, 29, 26, 23, 14, 23, 27, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 23, 23, 29, 29, 29, 18, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 21, 26, 17, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 21, 12, 16, 29, 3,  3,  3,  3,  3,  4,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  8,  21, 25, 26, 26, 26, 29, 28, 28, 29, 28, 22,
    29, 12, 29, 29, 25, 26, 28, 28, 13, 29, 28, 28, 28, 28, 28, 22, 28, 28, 28,
    21, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 28, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 28, 13, 28, 28, 28, 13, 28, 29, 29, 28, 29, 29,
    29, 29, 29, 29, 29, 28, 28, 28, 28, 29, 28, 29, 13, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  8,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  8,  8,  8,  8,  8,  8,  8,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  29, 29, 29, 29, 29, 29, 29, 29, 42, 42,
    29, 29, 29, 29, 23, 29, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 42, 29,
    42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  3,  3,
    3,  29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 23, 12, 42, 42, 29, 29, 26, 42, 3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  12, 3,  29, 3,  3,  29, 3,  3,  18, 3,  42, 42, 42, 42, 42, 42, 42,
    42, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
    35, 35, 35, 35, 35, 35, 35, 35, 35, 42, 42, 42, 42, 35, 35, 35, 35, 29, 29,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 25, 25, 25, 23, 23, 29, 29, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    18, 3,  18, 18, 18, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 25, 24, 24, 29, 29, 29, 3,  29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    18, 29, 3,  3,  3,  3,  3,  3,  3,  29, 29, 3,  3,  3,  3,  3,  3,  29, 29,
    3,  3,  29, 3,  3,  3,  3,  29, 29, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 3,  29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  29, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  3,
    3,  3,  3,  3,  3,  29, 29, 29, 29, 23, 18, 29, 42, 42, 3,  26, 26, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 3,  3,  3,  3,  29, 3,  3,  3,  3,  3,  3,  3,  3,  3,  29, 3,  3,  3,
    29, 3,  3,  3,  3,  3,  42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  42, 42, 29, 42, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 42, 42, 42, 42,
    42, 42, 3,  3,  3,  3,  3,  3,  3,  3,  29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  29, 3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,
    3,  29, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  29,
    3,  3,  3,  3,  3,  3,  3,  29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,
    12, 12, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  42, 29, 29, 29, 29, 29, 29,
    29, 29, 42, 42, 29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29,
    42, 29, 42, 42, 42, 29, 29, 29, 29, 42, 42, 3,  29, 3,  3,  3,  3,  3,  42,
    42, 3,  3,  42, 42, 3,  3,  3,  29, 42, 42, 42, 42, 42, 42, 42, 42, 3,  42,
    42, 42, 42, 29, 29, 42, 29, 29, 29, 3,  3,  42, 42, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 29, 29, 25, 25, 29, 29, 29, 29, 29, 25, 29, 26, 29, 29, 3,
    42, 3,  3,  3,  42, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 29, 29, 42, 42,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 42, 29, 29, 42, 29,
    29, 42, 42, 3,  42, 3,  3,  3,  42, 42, 42, 42, 3,  3,  42, 42, 3,  3,  3,
    42, 42, 42, 3,  42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 42, 29, 42, 42,
    42, 42, 42, 42, 42, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 3,  3,  29, 29,
    29, 3,  29, 42, 42, 42, 42, 42, 42, 42, 42, 42, 3,  3,  3,  42, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29,
    29, 29, 29, 42, 29, 29, 42, 29, 29, 29, 29, 29, 42, 42, 3,  29, 3,  3,  3,
    3,  3,  3,  42, 3,  3,  3,  42, 3,  3,  3,  42, 42, 29, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 3,  3,  42, 42, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 29, 26, 42, 42, 42, 42, 42, 42, 42, 29, 3,  3,
    3,  3,  3,  3,  42, 3,  3,  3,  42, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42,
    29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 42,
    29, 29, 29, 29, 29, 42, 42, 3,  29, 3,  3,  3,  3,  3,  42, 42, 3,  3,  42,
    42, 3,  3,  3,  42, 42, 42, 42, 42, 42, 42, 3,  3,  3,  42, 42, 42, 42, 29,
    29, 42, 29, 29, 29, 3,  3,  42, 42, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 42, 42, 42, 3,  29, 42,
    29, 29, 29, 29, 29, 29, 42, 42, 42, 29, 29, 29, 42, 29, 29, 29, 29, 42, 42,
    42, 29, 29, 42, 29, 42, 29, 29, 42, 42, 42, 29, 29, 42, 42, 42, 29, 29, 29,
    42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42,
    3,  3,  3,  42, 42, 42, 3,  3,  3,  42, 3,  3,  3,  3,  42, 42, 29, 42, 42,
    42, 42, 42, 42, 3,  42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    26, 29, 42, 42, 42, 42, 42, 3,  3,  3,  3,  3,  29, 29, 29, 29, 29, 29, 29,
    29, 42, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 3,  29, 3,  3,  3,  3,  3,  42, 3,
    3,  3,  42, 3,  3,  3,  3,  42, 42, 42, 42, 42, 42, 42, 3,  3,  42, 29, 29,
    29, 42, 42, 29, 42, 42, 29, 29, 3,  3,  42, 42, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 42, 42, 42, 42, 42, 42, 42, 13, 29, 29, 29, 29, 29, 29, 29, 29,
    3,  3,  3,  13, 29, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 42, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29, 29,
    42, 42, 3,  29, 3,  3,  3,  3,  3,  42, 3,  3,  3,  42, 3,  3,  3,  3,  42,
    42, 42, 42, 42, 42, 42, 3,  3,  42, 42, 42, 42, 42, 42, 29, 29, 42, 29, 29,
    3,  3,  42, 42, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 42, 29, 29, 3,  42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 3,  3,  3,  3,  29, 29, 29, 29,
    29, 29, 29, 29, 29, 42, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  29, 3,  3,  3,
    3,  3,  42, 3,  3,  3,  42, 3,  3,  3,  3,  29, 29, 42, 42, 42, 42, 29, 29,
    29, 3,  29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  42, 42, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 29, 29, 29, 29, 29, 29, 29, 29, 29, 25, 29, 29,
    29, 29, 29, 29, 42, 3,  3,  3,  42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 42, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 42,
    42, 42, 3,  42, 42, 42, 42, 3,  3,  3,  3,  3,  3,  42, 3,  42, 3,  3,  3,
    3,  3,  3,  3,  3,  42, 42, 42, 42, 42, 42, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 42, 42, 3,  3,  29, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    42, 42, 42, 42, 26, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 29, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 12, 12, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 41, 41, 42, 41, 42, 41, 41,
    41, 41, 41, 42, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 42, 41, 42, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 42, 42, 41,
    41, 41, 41, 41, 42, 41, 42, 41, 41, 41, 41, 41, 41, 41, 42, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 42, 42, 41, 41, 41, 41, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 29, 13, 13, 13, 13, 29, 13, 13, 8,  13, 13, 12, 8,
    18, 18, 18, 18, 18, 8,  29, 18, 29, 29, 29, 3,  3,  29, 29, 29, 29, 29, 29,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 12, 3,  29, 3,  29, 3,  21, 16, 21, 16, 3,  3,  29, 29, 29, 29, 29, 29,
    29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 42, 42, 42, 42, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    12, 3,  3,  29, 29, 29, 29, 29, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    42, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  42,
    12, 12, 29, 29, 29, 29, 29, 29, 3,  29, 29, 29, 29, 29, 29, 42, 29, 29, 13,
    13, 12, 13, 29, 29, 29, 29, 29, 8,  8,  42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 12, 12, 29, 29, 29, 29, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 41, 41, 41, 41, 41,
    41, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 29, 42, 42, 42,
    42, 42, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38,
    38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38,
    38, 38, 38, 38, 38, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39,
    39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39,
    39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39,
    39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 42, 29, 29, 29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 42, 29, 42,
    29, 29, 29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    42, 29, 29, 29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 42, 29, 29, 29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29,
    29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 42, 29, 29, 29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 3,  3,  3,  29, 12,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 29, 29, 29, 29, 29, 29,
    42, 42, 12, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 12, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 21, 16,
    42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 12, 12, 12, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  42, 42, 42, 42, 42, 42,
    42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 3,  3,  3,  12, 12, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 42, 29, 29, 29, 42, 3,  3,  42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 12, 12, 20, 41, 12, 29, 12, 26, 41, 41, 42, 42, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 42, 29, 29, 18, 18, 12, 12, 13,
    29, 18, 18, 29, 3,  3,  3,  8,  3,  24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 3,
    3,  29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  29, 42,
    42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 42, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  42, 42,
    42, 42, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  42, 42, 42, 42, 29,
    42, 42, 42, 18, 18, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 42, 42, 41, 41, 41, 41, 41, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 42, 42, 42, 42, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 42, 42, 42, 42, 42, 42,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 41, 42, 42, 42, 41, 41, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  3,  42, 42, 29, 29,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 42, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 42, 42, 3,  24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 42,
    42, 42, 42, 42, 42, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 42, 42, 42, 42,
    42, 42, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 42, 42, 3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 3,  3,  3,  3,  3,  29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  29, 29, 29, 29, 29,
    29, 29, 29, 42, 42, 42, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 12, 12, 29,
    12, 12, 12, 12, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  3,
    3,  3,  3,  3,  29, 29, 29, 29, 29, 29, 29, 29, 29, 12, 12, 42, 3,  3,  3,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  29, 29, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  42, 42, 42, 42, 42, 42, 42,
    42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    42, 42, 42, 12, 12, 12, 12, 12, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 42,
    42, 42, 29, 29, 29, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 12, 12, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42,
    29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 42, 42, 42, 3,  3,  3,
    29, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  29, 29, 29, 29, 3,  29, 29, 29, 29, 29, 29, 3,  29, 29, 3,  3,
    3,  29, 42, 42, 42, 42, 42, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  8,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  8,  3,  3,  3,  29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42,
    29, 29, 29, 29, 29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29,
    29, 42, 29, 42, 29, 42, 29, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 42, 42, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 29, 29, 29, 42,
    29, 29, 29, 29, 29, 29, 29, 13, 29, 42, 12, 12, 12, 12, 12, 12, 12, 8,  12,
    12, 12, 7,  3,  10, 3,  3,  12, 8,  12, 12, 11, 28, 28, 29, 22, 22, 21, 22,
    22, 22, 21, 22, 28, 28, 29, 29, 19, 19, 19, 12, 0,  0,  3,  3,  3,  3,  3,
    8,  25, 25, 25, 25, 25, 25, 25, 25, 29, 22, 22, 28, 20, 20, 29, 29, 29, 29,
    23, 21, 16, 20, 20, 20, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 12,
    25, 12, 12, 12, 12, 29, 12, 12, 12, 6,  29, 29, 29, 29, 42, 3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  29, 29, 42, 42, 28, 29, 29, 29, 29, 29, 29, 29, 29,
    21, 16, 28, 29, 28, 28, 28, 28, 29, 29, 29, 29, 29, 29, 29, 29, 21, 16, 42,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42, 26, 26, 26,
    26, 26, 26, 26, 25, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    25, 26, 26, 26, 26, 25, 26, 26, 25, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 25,
    29, 28, 29, 29, 29, 25, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28, 29, 29, 26,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28, 28, 29, 29, 29, 29, 29, 29, 29,
    29, 28, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 28, 28, 29, 29, 29, 29, 29, 28, 29, 29, 28, 29, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 29, 29, 29, 29, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28, 29, 29, 42, 42, 42, 42,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28, 29, 28, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28,
    29, 28, 28, 29, 29, 29, 28, 28, 29, 29, 28, 29, 29, 29, 28, 29, 28, 26, 26,
    29, 28, 29, 29, 29, 29, 28, 29, 29, 28, 28, 28, 28, 29, 29, 28, 29, 28, 29,
    28, 28, 28, 28, 28, 28, 29, 28, 29, 29, 29, 29, 29, 28, 28, 28, 28, 29, 29,
    29, 29, 28, 28, 29, 29, 29, 29, 29, 29, 29, 29, 28, 29, 29, 29, 28, 29, 29,
    29, 29, 29, 28, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28, 28,
    29, 29, 28, 28, 28, 28, 29, 29, 28, 28, 29, 29, 28, 28, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28, 28, 29, 29, 28, 28, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28, 29, 29, 29, 28, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 28, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 19, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 21, 16, 21, 16, 29, 29, 29, 29, 29, 29, 28, 29, 29, 29, 29,
    29, 29, 29, 36, 36, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 21,
    16, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 36, 36, 36, 36, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 29, 29, 29, 29, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 29, 29, 28, 28,
    28, 28, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28, 28, 29, 28, 28, 28, 28,
    28, 28, 28, 29, 29, 29, 29, 29, 29, 29, 29, 28, 28, 29, 29, 28, 28, 29, 29,
    29, 29, 28, 28, 29, 29, 29, 29, 28, 28, 28, 29, 29, 28, 29, 29, 28, 28, 28,
    28, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28, 28,
    28, 28, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 36, 36, 36, 36, 29, 28, 28, 29, 29, 28,
    29, 29, 29, 29, 28, 28, 29, 29, 29, 29, 36, 36, 28, 28, 36, 29, 36, 36, 36,
    31, 36, 36, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 36, 36, 36, 29, 29, 29, 29, 28, 29, 28,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28, 28, 29, 28, 28, 28, 29, 28, 36,
    28, 28, 29, 28, 28, 29, 28, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 36, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 28, 28, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 36, 36, 36, 36, 36, 36, 36, 36, 36, 28, 28,
    28, 28, 36, 29, 36, 36, 36, 28, 36, 36, 28, 28, 28, 36, 36, 28, 28, 36, 28,
    28, 36, 36, 36, 29, 28, 29, 29, 29, 29, 28, 28, 36, 28, 28, 28, 28, 28, 28,
    36, 36, 36, 36, 36, 28, 36, 36, 31, 36, 28, 28, 36, 36, 36, 36, 36, 29, 29,
    29, 36, 36, 31, 31, 31, 31, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    28, 29, 29, 29, 22, 22, 22, 22, 22, 22, 29, 18, 18, 36, 29, 29, 29, 21, 16,
    21, 16, 21, 16, 21, 16, 21, 16, 21, 16, 21, 16, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    21, 16, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 21, 16, 21, 16, 21,
    16, 21, 16, 21, 16, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 21, 16, 21, 16, 21, 16, 21, 16, 21, 16, 21, 16, 21, 16, 21, 16, 21,
    16, 21, 16, 21, 16, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 21, 16, 21, 16, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 21, 16, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 28, 28, 28, 28, 28, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,
    29, 29, 42, 42, 42, 42, 42, 18, 12, 12, 12, 29, 18, 12, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 29, 42, 42, 42,
    42, 42, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 42, 42, 29, 12, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 3,  29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29, 29,
    29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 42,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  22, 22, 22, 22, 22, 22,
    22, 22, 22, 22, 22, 22, 22, 22, 12, 12, 12, 12, 12, 12, 12, 12, 29, 12, 21,
    12, 29, 29, 22, 22, 29, 29, 22, 22, 21, 16, 21, 16, 21, 16, 21, 16, 12, 12,
    12, 12, 18, 29, 12, 12, 29, 12, 12, 29, 29, 29, 29, 29, 11, 11, 12, 12, 12,
    29, 12, 12, 21, 12, 12, 12, 12, 12, 12, 12, 12, 29, 12, 29, 12, 12, 29, 29,
    29, 18, 18, 21, 16, 21, 16, 21, 16, 21, 16, 12, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 42, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 42, 42, 42, 42, 12, 16, 16, 36,
    36, 20, 36, 36, 21, 16, 21, 16, 21, 16, 21, 16, 21, 16, 36, 36, 21, 16, 21,
    16, 21, 16, 21, 16, 20, 21, 16, 16, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    3,  3,  3,  3,  3,  3,  36, 36, 36, 36, 36, 3,  36, 36, 36, 36, 36, 20, 20,
    36, 36, 36, 42, 30, 36, 30, 36, 30, 36, 30, 36, 30, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    30, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 30, 36, 30, 36, 30, 36, 36, 36, 36,
    36, 36, 30, 36, 36, 36, 36, 36, 36, 30, 30, 42, 42, 3,  3,  20, 20, 20, 20,
    36, 20, 30, 36, 30, 36, 30, 36, 30, 36, 30, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 30, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 30, 36, 30, 36, 30, 36, 36, 36, 36, 36, 36,
    30, 36, 36, 36, 36, 36, 36, 30, 30, 36, 36, 36, 36, 20, 30, 20, 20, 36, 42,
    42, 42, 42, 42, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 42, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 36, 36,
    36, 36, 36, 36, 36, 36, 28, 28, 28, 28, 28, 28, 28, 28, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 20, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 42, 42, 42, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 12, 12, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 12, 18,
    12, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 29, 29, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 3,  3,  3,  3,  29, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 3,  3,  29, 12, 12, 12, 12, 12, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 29, 29, 42, 29, 42,
    29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 3,  29, 29, 29, 3,  29, 29, 29, 29, 3,  29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    3,  3,  3,  3,  3,  29, 29, 29, 29, 3,  42, 42, 42, 29, 29, 29, 29, 29, 29,
    29, 29, 25, 29, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 13, 13, 18, 18, 42, 42, 42, 42, 42, 42, 42, 42, 3,  3,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  42, 42, 42, 42, 42, 42, 42, 42, 12, 12, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 42, 42, 42, 42, 42, 42, 3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 13, 29, 29, 3,  24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  3,  3,  3,  3,  12, 12, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 42, 42, 42, 3,  3,  3,  3,  29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  29, 29, 29, 29, 29, 29, 12, 12, 12, 29, 29, 29, 29, 42, 29, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 42, 42, 42, 42, 29, 29, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 41, 41, 41, 41, 41, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 3,  29,
    29, 29, 29, 29, 29, 29, 29, 3,  3,  42, 42, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 42, 42, 29, 12, 12, 12, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41,
    41, 41, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 41, 41, 41, 41, 41, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 3,  3,  3,  3,  3,  12, 12, 29, 29, 29, 3,  3,  42, 42, 42,
    42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 42, 42, 29, 29, 29, 29, 29,
    29, 42, 42, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29,
    29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 3,  3,  3,  3,  3,  3,  3,  3,  12, 3,  3,  42, 42, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 42, 42, 42, 42, 42, 42, 33, 34, 34, 34, 34, 34,
    34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34,
    34, 34, 34, 33, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34,
    34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 33, 34, 34, 34, 34, 34, 34,
    34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34,
    34, 34, 33, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34,
    34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38,
    38, 42, 42, 42, 42, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39,
    39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39,
    39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 42, 42, 42,
    42, 5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
    5,  5,  5,  5,  5,  5,  5,  5,  42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29,
    29, 29, 29, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29,
    29, 42, 42, 42, 42, 42, 35, 3,  35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 29,
    35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 42, 35, 35, 35, 35, 35,
    42, 35, 42, 35, 35, 42, 35, 35, 42, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 16, 21, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 42, 42, 42, 42, 42, 42, 42, 29, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 25, 29, 29,
    29, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  23, 16,
    16, 23, 23, 18, 18, 21, 16, 19, 42, 42, 42, 42, 42, 42, 3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  36, 36, 36, 36, 36, 21, 16, 21,
    16, 21, 16, 21, 16, 21, 16, 21, 16, 36, 36, 21, 16, 36, 36, 36, 36, 36, 36,
    36, 16, 36, 16, 42, 20, 20, 18, 18, 36, 21, 16, 21, 16, 21, 16, 36, 36, 36,
    36, 36, 36, 36, 36, 42, 36, 26, 25, 36, 42, 42, 42, 42, 29, 29, 29, 29, 29,
    42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 42, 42, 6,  42, 18, 36, 36, 26, 25, 36, 36, 21, 16, 36,
    36, 16, 36, 16, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 20, 20, 36, 36,
    36, 18, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 21, 36, 16, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 21, 36, 16, 36, 21, 16, 16, 21, 16, 16, 20, 36, 30, 30, 30, 30, 30,
    30, 30, 30, 30, 30, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 20, 20, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 42, 42, 36, 36, 36, 36, 36, 36,
    42, 42, 36, 36, 36, 36, 36, 36, 42, 42, 36, 36, 36, 36, 36, 36, 42, 42, 36,
    36, 36, 42, 42, 42, 25, 26, 36, 36, 36, 26, 26, 42, 29, 29, 29, 29, 29, 29,
    29, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 3,  3,  3,  15, 28, 42, 42, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 42, 29, 12, 12, 12, 42,
    42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  42, 42,
    29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  3,  42, 42, 42, 42, 42, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 12, 29, 29, 29, 29, 29, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 42, 29, 29, 42, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 42,
    42, 42, 29, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 29,
    29, 29, 29, 29, 29, 42, 42, 29, 42, 29, 29, 29, 29, 29, 29, 42, 29, 29, 42,
    42, 42, 29, 42, 42, 29, 29, 29, 29, 29, 29, 42, 12, 29, 29, 29, 29, 29, 29,
    29, 29, 42, 29, 29, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 42, 42, 42, 12, 29, 3,  3,  3,  42, 3,  3,  42, 42, 42, 42, 42,
    3,  3,  3,  3,  29, 29, 29, 29, 29, 29, 42, 42, 3,  3,  3,  42, 42, 42, 42,
    3,  12, 12, 12, 12, 12, 12, 12, 12, 29, 42, 42, 42, 42, 42, 42, 42, 29, 29,
    29, 29, 29, 3,  3,  42, 42, 42, 42, 29, 29, 29, 29, 29, 12, 12, 12, 12, 12,
    12, 19, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 42, 42,
    42, 12, 12, 12, 12, 12, 12, 12, 29, 29, 42, 42, 42, 42, 42, 42, 42, 29, 29,
    29, 29, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 3,  3,  12,
    42, 42, 3,  29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42, 42, 42, 29,
    29, 3,  3,  3,  3,  29, 29, 29, 29, 42, 42, 42, 42, 42, 42, 3,  3,  3,  3,
    3,  3,  3,  12, 12, 29, 29, 29, 29, 29, 42, 42, 3,  29, 29, 3,  3,  29, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    29, 29, 29, 12, 12, 3,  42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 42, 42,
    3,  3,  3,  3,  3,  42, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 12, 12, 12,
    12, 29, 3,  3,  29, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 3,  29, 13,
    29, 42, 42, 42, 42, 42, 42, 42, 42, 42, 3,  29, 29, 29, 29, 12, 12, 29, 12,
    3,  3,  3,  3,  29, 3,  3,  24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 29, 13,
    29, 12, 12, 12, 3,  3,  3,  3,  3,  3,  3,  3,  12, 12, 29, 12, 12, 29, 3,
    29, 3,  42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29,
    29, 29, 29, 29, 42, 29, 42, 29, 29, 29, 29, 42, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 12, 42, 42, 42, 42, 42, 42, 3,  3,  3,  3,  42, 29, 29, 29, 29, 29,
    29, 29, 29, 42, 42, 29, 42, 29, 29, 42, 29, 29, 29, 29, 29, 42, 3,  3,  29,
    3,  3,  29, 42, 42, 42, 42, 42, 42, 3,  42, 42, 42, 42, 42, 29, 29, 29, 3,
    3,  42, 42, 3,  3,  3,  3,  3,  3,  3,  42, 42, 42, 3,  3,  3,  3,  3,  3,
    3,  29, 29, 29, 29, 12, 12, 12, 12, 29, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 12, 12, 42, 29, 3,  29, 3,  3,  3,  3,  29, 29, 29, 29, 42, 42, 42, 42,
    42, 42, 42, 42, 3,  3,  3,  3,  3,  3,  42, 42, 3,  3,  3,  3,  3,  3,  3,
    3,  13, 12, 12, 18, 18, 29, 29, 29, 12, 12, 12, 12, 12, 12, 12, 12, 29, 29,
    29, 29, 3,  3,  42, 42, 3,  12, 12, 29, 29, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 42, 42, 42,
    3,  3,  3,  3,  3,  3,  3,  3,  29, 29, 42, 42, 42, 42, 42, 42, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 41, 41, 12, 12, 12, 41, 29, 29, 29, 29, 29, 29,
    29, 42, 42, 29, 42, 42, 29, 29, 29, 29, 42, 29, 29, 42, 29, 29, 29, 29, 29,
    29, 29, 29, 3,  3,  3,  3,  3,  3,  42, 3,  3,  42, 42, 3,  3,  3,  3,  29,
    3,  3,  12, 12, 12, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 3,  3,  3,  3,
    3,  3,  3,  42, 42, 3,  3,  3,  3,  3,  3,  29, 13, 29, 3,  42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 3,  3,  3,  3,  3,  3,  3,  29, 3,
    3,  3,  3,  13, 29, 12, 12, 12, 12, 13, 29, 3,  42, 42, 42, 42, 42, 42, 42,
    42, 29, 3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  29, 29, 29, 29, 3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  12, 12, 12, 29, 13, 13, 12, 12, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    13, 42, 42, 42, 42, 42, 42, 29, 12, 12, 12, 12, 12, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 13, 18, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 3,  3,  3,  3,  3,  3,  42, 42, 42, 3,  42, 3,  3,  42, 3,  3,  3,  3,
    3,  3,  29, 3,  42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 42,
    29, 29, 42, 29, 29, 29, 29, 29, 29, 3,  3,  42, 3,  3,  3,  3,  3,  29, 42,
    42, 42, 42, 42, 42, 42, 29, 29, 29, 3,  3,  3,  3,  29, 29, 42, 42, 42, 42,
    42, 42, 42, 3,  3,  29, 3,  29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    3,  3,  3,  3,  3,  3,  3,  42, 42, 42, 3,  3,  3,  12, 12, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 25, 25, 25, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 12, 12, 12, 12, 12, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 21,
    21, 21, 16, 16, 16, 29, 29, 16, 29, 29, 29, 21, 16, 21, 16, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 21, 16, 16, 29, 29, 29, 29, 8,  8,  8,  8,  8,  8,  8,
    21, 16, 8,  8,  8,  21, 16, 21, 16, 3,  29, 29, 29, 29, 29, 29, 3,  3,  3,
    3,  3,  3,  3,  3,  3,  24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 42, 42, 42,
    42, 12, 12, 3,  3,  3,  3,  3,  12, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    3,  3,  3,  3,  3,  3,  3,  12, 12, 12, 29, 29, 29, 29, 29, 29, 12, 29, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    42, 29, 29, 29, 29, 29, 29, 29, 12, 12, 29, 29, 42, 42, 42, 42, 42, 3,  3,
    3,  3,  3,  3,  3,  3,  42, 42, 42, 42, 42, 42, 42, 3,  20, 20, 20, 20, 8,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 30, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 42, 42, 42, 30, 30, 30, 42, 42, 30, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 30, 30, 30, 30, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 42, 42, 29, 3,  3,  12, 29, 29, 29, 29, 29, 3,  3,
    3,  3,  3,  29, 29, 29, 3,  3,  3,  29, 29, 3,  3,  3,  3,  3,  3,  3,  29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  3,  3,  3,  29, 29, 3,  3,  3,  29,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 42, 42, 29, 29, 42, 42, 29, 29,
    29, 29, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 42, 29, 42, 29, 29, 29,
    29, 29, 42, 29, 42, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    42, 42, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 29,
    29, 29, 29, 29, 3,  29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 3,  29, 29, 12,
    12, 12, 12, 29, 42, 42, 42, 42, 42, 29, 29, 29, 29, 29, 29, 42, 42, 42, 42,
    42, 3,  3,  3,  3,  3,  3,  3,  3,  3,  42, 42, 3,  3,  3,  3,  3,  42, 3,
    3,  42, 3,  3,  3,  3,  3,  42, 42, 42, 42, 42, 3,  3,  3,  3,  3,  3,  3,
    29, 29, 29, 29, 29, 29, 29, 42, 42, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 3,  42, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 42, 42, 42,
    42, 42, 26, 29, 29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29, 42, 29, 29, 42,
    29, 29, 29, 29, 3,  3,  3,  3,  3,  3,  3,  29, 42, 42, 42, 42, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 42, 42, 42, 42, 21, 21, 25, 29, 29, 29, 29, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 29, 29, 42, 29, 42, 42, 29, 42, 29,
    29, 29, 29, 29, 29, 29, 42, 29, 29, 29, 29, 42, 29, 42, 29, 42, 42, 42, 42,
    29, 42, 42, 42, 42, 29, 42, 29, 42, 29, 42, 29, 29, 29, 42, 29, 29, 42, 29,
    42, 42, 29, 42, 29, 42, 29, 42, 29, 42, 29, 29, 42, 29, 42, 42, 29, 29, 29,
    29, 42, 29, 29, 29, 29, 42, 29, 29, 29, 29, 42, 29, 42, 29, 29, 29, 42, 29,
    29, 29, 29, 29, 42, 29, 29, 29, 29, 29, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 36, 36, 36, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 29, 29,
    29, 36, 36, 36, 36, 36, 36, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40,
    40, 40, 40, 40, 36, 36, 36, 36, 36, 31, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 29, 29, 36, 36, 36, 36, 36, 29, 36, 36, 36, 31, 31, 31, 36, 36,
    31, 36, 36, 31, 31, 31, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 32, 32,
    32, 32, 32, 36, 36, 31, 31, 36, 36, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 31, 31, 31, 31,
    31, 31, 31, 31, 31, 31, 36, 36, 36, 31, 36, 36, 36, 31, 31, 31, 36, 31, 31,
    31, 36, 36, 36, 36, 36, 36, 36, 31, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 29, 36, 29, 36, 29, 36, 36, 36, 36, 36, 31, 36, 36, 36, 36,
    29, 29, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 29, 29, 29, 29,
    29, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 31, 31, 36, 36, 36, 36, 31,
    36, 36, 36, 36, 36, 31, 36, 36, 36, 36, 31, 31, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 29, 29, 29, 29, 29, 29, 29, 29, 36, 36, 36, 36, 29, 29, 29, 29, 29,
    29, 36, 36, 36, 36, 36, 36, 31, 31, 31, 36, 36, 36, 31, 31, 31, 31, 31, 29,
    29, 29, 29, 29, 29, 22, 22, 22, 20, 20, 20, 29, 29, 29, 29, 36, 36, 36, 36,
    31, 31, 31, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 31, 36, 36, 36, 29,
    29, 29, 29, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 31, 36, 36, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 31, 36, 36, 31, 31, 31, 36, 36, 36, 36, 36, 31, 31, 36, 31, 31, 36, 31,
    36, 36, 36, 36, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 36, 36,
};
static const uint8_t kPairTable[53][44] = {
    {192, 193, 194, 221, 196, 221, 198, 199, 200, 201, 221, 203, 204, 205, 206,
     207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 221, 221,
     212, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 221, 221, 235},
    {192, 193, 2,   221, 196, 221, 198, 199, 200, 201, 221, 203, 204, 205, 206,
     207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 221, 221,
     212, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 221, 221, 235},
    {192, 193, 194, 221, 196, 221, 198, 199, 200, 201, 221, 203, 204, 205, 206,
     207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 221, 221,
     212, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 221, 221, 235},
    {0,   1,   2,   3,   4,   29, 6,   7,   8,   9,   3,   139, 12, 141, 14,
     143, 16,  17,  18,  19,  20, 21,  22,  23,  24,  25,  26,  27, 29,  29,
     20,  159, 160, 161, 162, 35, 164, 165, 166, 167, 168, 29,  29, 235},
    {192, 193, 194, 221, 196, 221, 198, 199, 200, 201, 221, 203, 204, 205, 206,
     207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 221, 221,
     212, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 221, 221, 235},
    {0,   1,   2,   29,  4,   29, 6,   7,   8,   9,   29,  139, 12, 141, 14,
     143, 16,  17,  18,  19,  20, 21,  22,  23,  24,  25,  26,  27, 29,  29,
     20,  159, 160, 161, 162, 35, 164, 165, 166, 167, 168, 29,  29, 235},
    {0,  1,  2,  6,  4,  29, 6,  7,  8,  9,  6,  11, 12, 13, 14,
     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 29, 29,
     20, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 29, 29, 235},
    {0,   1,   2,   157, 4,   157, 134, 7,   136, 45,  157, 139, 140, 141, 142,
     143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 157, 157,
     148, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,  1,  2,  8,  4,  29, 6,  7,  8,  9,  8,  11, 12, 13, 14,
     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 29, 29,
     20, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 29, 29, 235},
    {0,   1,   2,   157, 4,   157, 6,   7,   136, 9,   157, 139, 140, 141, 142,
     143, 16,  17,  18,  147, 148, 149, 150, 23,  152, 153, 154, 27,  157, 157,
     148, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   10,  4,   29, 6,   7,   8,   9,   10,  139, 12, 141, 14,
     143, 16,  17,  18,  19,  20, 21,  22,  23,  24,  25,  26,  27, 29,  29,
     20,  159, 160, 161, 162, 35, 164, 165, 166, 167, 168, 29,  29, 235},
    {0,   1,   2,   11,  4,   157, 6,   7,   8,   50,  11,  11,  12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 153, 154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   12,  4,   157, 6,   7,   136, 9,   12,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 153, 154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,  2,  13, 4,  29, 6,  7,  8,  9,  13, 11, 12, 13, 14,
     143, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 29, 29,
     20,  31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 29, 29, 235},
    {0,   1,   2,   14,  4,   157, 6,   7,   136, 9,   14,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  24,  153, 154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   15,  4,   157, 6,   7,   8,   9,   15,  139, 140, 141, 142,
     143, 16,  17,  18,  147, 148, 149, 22,  23,  152, 153, 154, 27,  157, 157,
     148, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   16,  4,   157, 6,   7,   8,   48,  16,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 25,  26,  27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   17,  4,   29, 6,   7,   8,   49,  17,  139, 12, 141, 14,
     143, 16,  17,  18,  19,  20, 149, 22,  23,  24,  25,  26,  27, 29,  29,
     20,  159, 160, 161, 162, 35, 164, 165, 166, 167, 168, 29,  29, 235},
    {0,   1,   2,   18,  4,   157, 6,   7,   8,   9,   18,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 153, 154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   19,  4,   157, 6,   7,   8,   9,   19,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 153, 154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   20,  4,   157, 6,   7,   8,   9,   20,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 153, 154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,  1,  2,  21, 4,  29, 6,  7,  8,  46, 21, 11, 12, 13, 14,
     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 29, 29,
     20, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 29, 29, 235},
    {0,  1,  2,  22, 4,  29, 6,  7,  8,  47, 22, 11, 12, 13, 14,
     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 29, 29,
     20, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 29, 29, 235},
    {0,   1,   2,   23,  4,   29, 6,   7,   8,   9,   23,  139, 12, 141, 14,
     143, 16,  17,  18,  19,  20, 149, 22,  23,  24,  153, 154, 27, 29,  29,
     20,  159, 160, 161, 162, 35, 164, 165, 166, 167, 168, 29,  29, 235},
    {0,   1,   2,   24,  4,   29, 6,   7,   8,   9,   24,  139, 12, 141, 14,
     143, 16,  17,  18,  19,  20, 21,  22,  23,  24,  25,  26,  27, 29,  29,
     20,  159, 160, 161, 162, 35, 164, 165, 166, 167, 168, 29,  29, 235},
    {0,   1,   2,   25,  4,   29, 6,   7,   8,   9,   25,  139, 12, 141, 14,
     143, 16,  17,  18,  19,  20, 21,  22,  23,  24,  153, 154, 27, 29,  29,
     20,  159, 160, 161, 162, 35, 164, 165, 166, 167, 168, 29,  29, 235},
    {0,   1,  2,  26, 4,  29, 6,  7,  8,  9,  26,  139, 12, 141, 14,
     143, 16, 17, 18, 19, 20, 21, 22, 23, 24, 153, 154, 27, 29,  29,
     20,  31, 32, 33, 34, 35, 36, 37, 38, 39, 168, 29,  29, 235},
    {0,   1,   2,   27,  4,   157, 6,   7,   8,   9,   27,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  24,  153, 154, 27,  157, 157,
     20,  159, 160, 161, 162, 35,  164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   29,  4,   29, 6,   7,   8,   9,   29,  139, 12, 141, 14,
     143, 16,  17,  18,  19,  20, 21,  22,  23,  24,  25,  26,  27, 29,  29,
     20,  159, 160, 161, 162, 35, 164, 165, 166, 167, 168, 29,  29, 235},
    {0,   1,   2,   29,  4,   29, 6,   7,   8,   9,   29,  139, 12, 141, 14,
     143, 16,  17,  18,  19,  20, 21,  22,  23,  24,  25,  26,  27, 29,  29,
     20,  159, 160, 161, 162, 35, 164, 165, 166, 167, 168, 29,  29, 235},
    {0,   1,   2,   20,  4,   157, 6,   7,   8,   9,   20,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 153, 154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,  31,  4,   157, 6,   7,   8,   9,   31,  139, 12,  141, 14,
     143, 16,  17, 18,  19,  20,  149, 22,  23,  152, 25,  154, 27,  157, 157,
     20,  159, 32, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   32,  4,   157, 6,   7,   8,   9,   32,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 25,  154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   33,  4,   157, 6,   7,   8,  9,   33,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23, 152, 25,  154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 38, 39,  168, 157, 157, 235},
    {0,   1,   2,   34,  4,   157, 6,   7,   8,   9,   34,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 25,  154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 39,  168, 157, 157, 235},
    {0,   1,   2,   35,  4,   29, 6,   7,   8,   9,   35,  139, 51, 141, 51,
     143, 16,  17,  18,  19,  20, 21,  22,  23,  24,  25,  26,  27, 29,  29,
     20,  159, 160, 161, 162, 35, 164, 165, 166, 167, 168, 29,  29, 235},
    {0,   1,   2,   36,  4,   157, 6,   7,   8,   9,   36,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 25,  154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   37, 4,  157, 6,   7,  8,  9,   37,  139, 12,  141, 14,
     143, 16,  17,  18, 19, 20,  149, 22, 23, 152, 25,  154, 27,  157, 157,
     20,  159, 160, 33, 34, 163, 164, 37, 38, 167, 168, 157, 157, 235},
    {0,   1,   2,   38,  4,   157, 6,   7,   8,  9,   38,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23, 152, 25,  154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 38, 39,  168, 157, 157, 235},
    {0,   1,   2,   39,  4,   157, 6,   7,   8,   9,   39,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 25,  154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 39,  168, 157, 157, 235},
    {0,   1,   2,   40,  4,   157, 6,   7,   8,   9,   40,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 153, 154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 52,  157, 157, 235},
    {0,   1,   2,   29,  4,   29, 6,   7,   8,   9,   29,  139, 12, 141, 14,
     143, 16,  17,  18,  19,  20, 21,  22,  23,  24,  25,  26,  27, 29,  29,
     20,  159, 160, 161, 162, 35, 164, 165, 166, 167, 168, 29,  29, 235},
    {0,   1,   2,   29,  4,   29, 6,   7,   8,   9,   29,  139, 12, 141, 14,
     143, 16,  17,  18,  19,  20, 21,  22,  23,  24,  25,  26,  27, 29,  29,
     20,  159, 160, 161, 162, 35, 164, 165, 166, 167, 168, 29,  29, 235},
    {0,   1,   2,   157, 4,   157, 6,   7,   136, 9,   157, 139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 153, 154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,  1,  2,  29, 4,  29, 6,  7,  8,  9,  29, 11, 12, 13, 14,
     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 29, 29,
     20, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 29, 29, 43},
    {0,   1,   2,   157, 4,   157, 134, 7,   136, 45,  157, 139, 140, 141, 142,
     143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 157, 157,
     148, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,  1,  2,  29, 4,  29, 6,  7,  8,  46, 29, 11, 12, 13, 14,
     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 29, 29,
     20, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 29, 29, 235},
    {0,   1,   2,   157, 4,   157, 6,   7,   136, 47,  157, 139, 140, 141, 142,
     143, 16,  17,  18,  147, 148, 21,  150, 23,  152, 153, 154, 27,  157, 157,
     148, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   157, 4,   157, 6,   7,   136, 48,  157, 139, 140, 141, 142,
     143, 16,  17,  18,  147, 20,  149, 150, 23,  152, 153, 154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   157, 4,   157, 6,   7,   136, 49,  157, 139, 140, 141, 142,
     143, 16,  17,  18,  147, 20,  149, 150, 23,  152, 153, 154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,   2,   157, 4,   157, 6,   7,   136, 50,  157, 11,  140, 141, 142,
     143, 16,  17,  18,  147, 148, 149, 150, 23,  152, 153, 154, 27,  157, 157,
     148, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
    {0,   1,  2,  51, 4,  29, 6,  7,  8,  9,  51, 11, 12, 13, 14,
     143, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 29, 29,
     20,  31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 29, 29, 235},
    {0,   1,   2,   52,  4,   157, 6,   7,   8,   9,   52,  139, 12,  141, 14,
     143, 16,  17,  18,  19,  20,  149, 22,  23,  152, 153, 154, 27,  157, 157,
     20,  159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 157, 157, 235},
};
static const uint64_t kSafePairs[43] = {
    0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL, 0x8c700d7ULL,
    0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL,
    0x7ffffffffffULL, 0x97ULL,          0x8c700d7ULL,     0x7ffffffffffULL,
    0x48dfd2d7ULL,    0x7ffffffffffULL, 0x49dfd2d7ULL,    0x7ffffffffffULL,
    0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL,
    0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL,
    0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL,
    0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL,
    0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL,
    0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL, 0x7ffffffffffULL,
    0x6fffffffbf7ULL, 0x7ffffffffffULL, 0x7ffffffffffULL,
};
}

#endif

#line 1 "src/unicode-linebreak/unicode_linebreak.h"

namespace unicode_linebreak {
using namespace base;

constexpr uint8_t kUnicodeVersion[3] = {15, 0, 0};

enum class BreakClass : uint8_t {
    Mandatory,
    CarriageReturn,
    LineFeed,
    CombiningMark,
    NextLine,
    Surrogate,
    WordJoiner,
    ZeroWidthSpace,
    NonBreakingGlue,
    Space,
    ZeroWidthJoiner,
    BeforeAndAfter,
    After,
    Before,
    Hyphen,
    Contingent,
    ClosePunctuation,
    CloseParenthesis,
    Exclamation,
    Inseparable,
    NonStarter,
    OpenPunctuation,
    Quotation,
    InfixSeparator,
    Numeric,
    Postfix,
    Prefix,
    Symbol,
    Ambiguous,
    Alphabetic,
    ConditionalJapaneseStarter,
    EmojiBase,
    EmojiModifier,
    HangulLvSyllable,
    HangulLvtSyllable,
    HebrewLetter,
    Ideographic,
    HangulLJamo,
    HangulVJamo,
    HangulTJamo,
    RegionalIndicator,
    ComplexContext,
    Unknown,
};

BreakClass BreakProperty(uint32_t codepoint);

enum class BreakOpportunity : uint8_t {
    Mandatory,
    Allowed
};
struct LineBreak {
    int32_t offset = 0;
    BreakOpportunity opportunity = BreakOpportunity::Allowed;
};

struct LineBreakIterator {
    Str text;
    int32_t offset = 0;
    uint8_t state = 44;
    bool afterZwj = false;
    bool finished = false;

    bool Next(LineBreak* result);
};

LineBreakIterator LineBreaks(Str text);

struct SafeSplit {
    Str previous;
    Str safe;
};

SafeSplit SplitAtSafe(Str text);

}

#endif
