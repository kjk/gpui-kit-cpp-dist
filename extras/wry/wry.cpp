#define GPUI_INCLUDE_PRIVATE_API 1
#include "wry.h"

#include <climits>
#include <cstdarg>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#line 1 "src/base.cpp"

namespace base {

static int VsnprintfUtf8(Str buf, const char* fmt, va_list args);
static int VscprintfUtf8(const char* fmt, va_list args);

static PanicHook gPanicHook = nullptr;

PanicHook SetPanicHook(PanicHook hook) {
    PanicHook was = gPanicHook;
    gPanicHook = hook;
    return was;
}

void Panic(const char* msg) {
    if (gPanicHook) {
        gPanicHook(msg);
        return;
    }
    log(Str(msg ? msg : "panic"));
    abort();
}

float StrToFloatUnchecked(Str s) {
    if (!s.s || len(s) <= 0) {
        return 0;
    }
    TempStr text = StrDupTemp(s);
    return text.s ? strtof(text.s, nullptr) : 0;
}

int StrToIntUnchecked(Str s) {
    if (!s.s || len(s) <= 0) {
        return 0;
    }
    int i = 0;
    while (i < len(s) && s.s[i] <= ' ') {
        i++;
    }
    bool negative = false;
    Str rest = Str(s.s + i, len(s) - i);
    if (StrStartsWithAny(rest, "+-")) {
        negative = rest.s[0] == '-';
        i++;
    }
    uint64_t value = 0;
    while (i < len(s) && s.s[i] >= '0' && s.s[i] <= '9') {
        value = value * 10 + (uint64_t)(s.s[i] - '0');
        i++;
    }
    int64_t signedValue = negative ? -(int64_t)value : (int64_t)value;
    return (int)signedValue;
}

void* AllocZero(int count, int size) {
    return calloc(count, size);
}

static_assert(sizeof(Arena) <= kArenaHeaderSize,
              "Arena header must fit in reserved header bytes");

using ArenaFlags = uint64_t;
enum : uint8_t {
    ArenaFlagNoChain = 1ull << 0,
    ArenaFlagLargePages = 1ull << 1,
};

struct ArenaParams {
    ArenaFlags flags = 0;
    uint64_t reserveSize = 0;
    uint64_t commitSize = 0;
    void* optionalBackingBuffer = nullptr;
    const char* allocationSiteFile = nullptr;
    int allocationSiteLine = 0;
    const char* name = nullptr;
};

static uint64_t ArenaDefaultReserveSize() {
    static uint64_t sz = 0;
    if (sz == 0) {
        sz = PlatArenaReserveSize();
    }
    return sz;
}
static uint64_t gArenaDefaultCommitSize = 64ull * 1024ull;
static ArenaFlags gArenaDefaultFlags = 0;

static uint64_t ArenaAlignPow2(uint64_t value, uint64_t align) {
    if (align <= 1) {
        return value;
    }
    return (value + align - 1) & ~(align - 1);
}

static uint64_t ArenaPosAfter(Arena* current, uint64_t size, uint64_t align) {
    if (align == 0) {
        align = 1;
    }
    return ArenaAlignPow2(current->pos, align) + size;
}

static Arena* ArenaAlloc(const ArenaParams& params);

static void ArenaRelease(Arena* arena) {
    PlatMemRelease(arena, arena->reserved);
}

static bool ArenaPushWouldChainLocked(Arena* arena, uint64_t size,
                                      uint64_t align) {
    if (!arena || (arena->flags & ArenaFlagNoChain)) {
        return false;
    }
    return arena->current
               ->reserved < ArenaPosAfter(arena->current, size, align);
}

static void* ArenaPushLocked(Arena* arena, uint64_t size, uint64_t align,
                             bool zero) {
    if (!arena) {
        return nullptr;
    }
    Arena* current = arena->current;
    uint64_t posPost = ArenaPosAfter(current, size, align);
    uint64_t posPre = posPost - size;

    uint64_t sizeToZero = 0;
    if (zero && current->committed > posPre) {
        sizeToZero = std::min(current->committed, posPost) - posPre;
    }

    if (current->reserved < posPost && !(arena->flags & ArenaFlagNoChain)) {

        uint64_t reserveChunkSize = arena->reserveChunkSize;
        uint64_t commitChunkSize = arena->commitChunkSize;
        if (size + kArenaHeaderSize > reserveChunkSize) {
            reserveChunkSize = ArenaAlignPow2(size + kArenaHeaderSize,
                                              std::max(align, PlatPageSize()));
            commitChunkSize = reserveChunkSize;
        }

        ArenaParams newParams = {};
        newParams.flags = current->flags;
        newParams.reserveSize = reserveChunkSize;
        newParams.commitSize = commitChunkSize;
        newParams.allocationSiteFile = current->allocationSiteFile;
        newParams.allocationSiteLine = current->allocationSiteLine;
        newParams.name = current->name;

        Arena* newBlock = ArenaAlloc(newParams);
        if (!newBlock) {
            return nullptr;
        }

        newBlock->basePos = current->basePos + current->reserved;
        newBlock->prev = current;
        arena->current = newBlock;
        current = newBlock;
        posPost = ArenaPosAfter(current, size, align);
        posPre = posPost - size;
        sizeToZero = 0;
    }

    if (current->committed < posPost) {
        if (current->flags & ArenaFlagLargePages) {
            return nullptr;
        }

        uint64_t commitEnd = ArenaAlignPow2(posPost, current->commitChunkSize);
        uint64_t commitClamped = std::min(commitEnd, current->reserved);
        uint64_t commitSize = commitClamped - current->committed;
        void* commitPtr = (char*)current + current->committed;
        if (!PlatMemCommit(commitPtr, commitSize, false)) {
            return nullptr;
        }
        current->committed = commitClamped;
    }

    if (current->committed < posPost) {
        return nullptr;
    }

    void* result = (char*)current + posPre;
    current->pos = posPost;
    arena->nAllocsSinceReset++;

    if (sizeToZero) {
        memset(result, 0, (size_t)sizeToZero);
    }
    return result;
}

static ArenaParams ArenaDefaultParams() {
    ArenaParams params = {};
    params.flags = gArenaDefaultFlags;
    params.reserveSize = ArenaDefaultReserveSize();
    params.commitSize = gArenaDefaultCommitSize;
    return params;
}

Arena* ArenaNew() {
    return ArenaAlloc(ArenaDefaultParams());
}

static Arena* ArenaAlloc(const ArenaParams& srcParams) {
    ArenaParams params = srcParams;
    if (params.reserveSize == 0) {
        params.reserveSize = ArenaDefaultReserveSize();
    }
    if (params.commitSize == 0) {
        params.commitSize = gArenaDefaultCommitSize;
    }

    bool useLargePages = (params.flags & ArenaFlagLargePages) != 0;
    const uint64_t pageSize =
        useLargePages ? PlatLargePageSize() : PlatPageSize();
    uint64_t reserveSize = ArenaAlignPow2(
        std::max(params.reserveSize, kArenaHeaderSize), pageSize);
    uint64_t commitSize =
        ArenaAlignPow2(std::max(params.commitSize, kArenaHeaderSize), pageSize);
    commitSize = std::min(commitSize, reserveSize);

    void* base = params.optionalBackingBuffer;
    bool usesExternalBuffer = (base != nullptr);
    ArenaFlags actualFlags = params.flags;

    if (!usesExternalBuffer) {
        if (useLargePages) {
            base = PlatMemReserveCommit(reserveSize, true);
            if (base) {
                commitSize = reserveSize;
            } else {
                actualFlags &= ~ArenaFlagLargePages;
                useLargePages = false;
                reserveSize = ArenaAlignPow2(reserveSize, PlatPageSize());
                commitSize = ArenaAlignPow2(commitSize, PlatPageSize());
            }
        }

        if (!base) {
            base = PlatMemReserve(reserveSize);
            if (base && !PlatMemCommit(base, commitSize, false)) {
                PlatMemRelease(base, reserveSize);
                base = nullptr;
            }
        }
    } else {
        commitSize = reserveSize;
    }

    if (!base) {
        return nullptr;
    }

    memset(base, 0, (size_t)std::min<uint64_t>(commitSize, kArenaHeaderSize));
    Arena* arena = (Arena*)base;
    arena->prev = nullptr;
    arena->current = arena;
    arena->flags = actualFlags;
    arena->commitChunkSize = useLargePages ? reserveSize : commitSize;
    arena->reserveChunkSize = reserveSize;
    arena->basePos = 0;
    arena->pos = kArenaHeaderSize;
    arena->committed = commitSize;
    arena->reserved = reserveSize;
    arena->allocationSiteFile = params.allocationSiteFile;
    arena->allocationSiteLine = params.allocationSiteLine;
    arena->name = params.name;
    arena->usesExternalBuffer = usesExternalBuffer;
    arena->nAllocsSinceReset = 0;
    return arena;
}

void ArenaDelete(Arena* arena) {
    if (!arena) {
        return;
    }

    Arena* node = arena->current;
    while (node) {
        Arena* prev = node->prev;
        if (!node->usesExternalBuffer) {
            ArenaRelease(node);
        }
        node = prev;
    }
}

void* Arena::Push(uint64_t size, uint64_t align, bool zero) {
    lock.Lock();
    void* mem = ArenaPushLocked(this, size, align, zero);
    lock.Unlock();
    return mem;
}

void Arena::PopTo(uint64_t popPos) {
    Arena* arena = this;
    lock.Lock();

    uint64_t bigPos = std::max(kArenaHeaderSize, popPos);
    Arena* node = arena->current;
    while (node && node->basePos >= bigPos) {
        Arena* prevNode = node->prev;
        if (!node->usesExternalBuffer) {
            ArenaRelease(node);
        } else {
            node->pos = kArenaHeaderSize;
        }
        node = prevNode;
    }

    if (!node) {
        lock.Unlock();
        return;
    }

    arena->current = node;
    uint64_t newPos = bigPos - node->basePos;
    node->pos = newPos;
    lock.Unlock();
}

uint64_t ArenaUsed(Arena* arena) {
    if (!arena) {
        return 0;
    }
    Arena* cur = arena->current;
    return cur ? cur->basePos + cur->pos : 0;
}

int VarintSize(uint32_t v) {
    int n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

int VarintPut(char* dst, uint32_t v) {
    int n = 0;
    while (v >= 0x80) {
        dst[n++] = (char)(v | 0x80);
        v >>= 7;
    }
    dst[n++] = (char)v;
    return n;
}

int VarintGet(const char* src, uint32_t* out) {
    uint32_t v = 0;
    int shift = 0;
    int n = 0;
    for (;;) {
        uint8_t b = (uint8_t)src[n++];
        v |= (uint32_t)(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            break;
        }
        shift += 7;
    }
    *out = v;
    return n;
}

static char* ArenaStrAt(Arena* a, ArenaStr s) {
    return (char*)ArenaAtOffset(a, s);
}

static uint64_t ArenaBlockOff(Arena* block, const void* p) {
    return block->basePos + (uint64_t)((const char*)p - (const char*)block);
}

ArenaStr ArenaStrDup(Arena* a, Str src) {
    if (!a || !src.s || len(src) <= 0) {
        return kArenaStrNone;
    }
    uint32_t n = (uint32_t)len(src);
    int vlen = VarintSize(n);
    a->lock.Lock();
    char* dst = (char*)ArenaPushLocked(a, (uint64_t)vlen + n + 1, 1, false);
    uint64_t at = dst ? ArenaBlockOff(a->current, dst) : 0;
    a->lock.Unlock();
    if (!dst) {
        return kArenaStrNone;
    }
    VarintPut(dst, n);
    memcpy(dst + vlen, src.s, (size_t)n);
    dst[vlen + n] = 0;
    if (at > UINT32_MAX) {
        return kArenaStrNone;
    }
    return (ArenaStr)at;
}

uint32_t ArenaStrLen(Arena* a, ArenaStr s) {
    if (!ArenaStrIsSet(s)) {
        return 0;
    }
    const char* p = ArenaStrAt(a, s);
    if (!p) {
        return 0;
    }
    uint32_t len = 0;
    VarintGet(p, &len);
    return len;
}

ArenaStr ArenaStrAppend(Arena* a, ArenaStr s, Str more) {
    if (!a || !more.s || len(more) <= 0) {
        return s;
    }
    if (!ArenaStrIsSet(s)) {
        return ArenaStrDup(a, more);
    }

    a->lock.Lock();
    char* p = ArenaStrAt(a, s);
    uint32_t len = 0;
    int vlen = p ? VarintGet(p, &len) : 0;
    Arena* cur = a->current;
    uint64_t used = cur ? cur->basePos + cur->pos : 0;

    bool newest = p && (uint64_t)s + vlen + len + 1 == used;
    uint32_t nlen = len + (uint32_t)more.len;
    int nvlen = VarintSize(nlen);
    uint64_t want = (uint64_t)nvlen + nlen + 1;
    if (newest && !ArenaPushWouldChainLocked(
                      a, (uint64_t)(nvlen - vlen) + (uint64_t)more.len, 1)) {
        want = (uint64_t)(nvlen - vlen) + (uint64_t)more.len;
    } else {
        newest = false;
    }
    char* dst = (char*)ArenaPushLocked(a, want, 1, false);
    uint64_t at = dst ? ArenaBlockOff(a->current, dst) : 0;
    a->lock.Unlock();
    if (!dst) {
        return s;
    }

    if (newest) {
        if (nvlen != vlen) {
            memmove(p + nvlen, p + vlen, (size_t)len);
        }
        VarintPut(p, nlen);
        memcpy(p + nvlen + len, more.s, (size_t)more.len);
        p[nvlen + nlen] = 0;
        return s;
    }

    VarintPut(dst, nlen);
    if (len > 0) {
        memcpy(dst + nvlen, p + vlen, (size_t)len);
    }
    memcpy(dst + nvlen + len, more.s, (size_t)more.len);
    dst[nvlen + nlen] = 0;
    if (at > UINT32_MAX) {

        return s;
    }
    return (ArenaStr)at;
}

Str ArenaStrGet(Arena* a, ArenaStr s) {
    if (!ArenaStrIsSet(s)) {
        return {};
    }
    char* p = ArenaStrAt(a, s);
    if (!p) {
        return {};
    }
    uint32_t len = 0;
    int vlen = VarintGet(p, &len);
    return Str(p + vlen, (int)len);
}

uint32_t ArenaOffsetOf(Arena* a, const void* p) {
    if (!a || !p) {
        return kArenaPtrNone;
    }
    const char* at = (const char*)p;
    for (Arena* node = a->current; node; node = node->prev) {
        const char* lo = (const char*)node;
        if (at < lo || at >= lo + node->pos) {
            continue;
        }
        uint64_t off = ArenaBlockOff(node, at);
        if (off > UINT32_MAX) {

            return kArenaPtrNone;
        }
        return (uint32_t)off;
    }
    return kArenaPtrNone;
}

static void* AllocBytes(Arena* arena, uint64_t size) {
    if (size == 0) {
        return nullptr;
    }
    if (!arena) {
        return malloc((size_t)size);
    }
    return arena->Push(size, 8, false);
}

void* Arena::Alloc(int size) {
    return AllocBytes(this, size <= 0 ? 0 : (uint64_t)size);
}

void Arena::Reset() {
    PopTo(0);
    nAllocsSinceReset = 0;
}

void* Alloc(Arena* arena, int size) {
    return AllocBytes(arena, size <= 0 ? 0 : (uint64_t)size);
}

void Free(Arena* arena, void* mem) {
    if (!arena) {
        free(mem);
    }
}

static void* Alloc(Arena* arena, size_t size) {
    return AllocBytes(arena, (uint64_t)size);
}

static void* Realloc(Arena* arena, void* mem, size_t newSize, size_t copySize) {
    if (!arena) {
        return realloc(mem, newSize);
    }

    if (newSize == 0) {
        return nullptr;
    }
    void* newMem = arena->Push((uint64_t)newSize, 8, false);
    if (newMem && mem && copySize > 0) {

        size_t n = copySize;
        n = std::min(n, newSize);
        memmove(newMem, mem, n);
    }
    return newMem;
}

static void* MemDup(Arena* arena, const void* mem, size_t size,
                    size_t extraBytes = 0) {
    void* newMem = Alloc(arena, size + extraBytes);
    if (!newMem) {
        return nullptr;
    }
    if (mem && size) {
        memcpy(newMem, mem, size);
    }

    if (extraBytes > 0) {
        memset((char*)newMem + size, 0, extraBytes);
    }
    return newMem;
}

static thread_local Arena* gTempArena = nullptr;

Arena* GetTempArena() {
    if (!gTempArena) {
        gTempArena = ArenaNew();
    }
    return gTempArena;
}

void ResetTempArena() {
    if (gTempArena) {
        gTempArena->Reset();
    }
}

void DestroyTempArena() {
    ArenaDelete(gTempArena);
    gTempArena = nullptr;
}

TempStr AllocStrTemp(int size) {

    if (size <= 0) {
        return {};
    }
    Arena* arena = GetTempArena();
    char* res = (char*)arena->Push((uint64_t)size + 1, 1, false);
    if (!res) {
        return {};
    }
    res[size] = 0;
    return Str(res, size);
}

TempStr StrDupTemp(Str s) {
    return StrDup(GetTempArena(), s);
}

TempStr ReadBoundedFileTemp(Str path, int limit) {
    if (!path || limit <= 0) {
        return {};
    }
    TempStr pathZ = StrDupTemp(path);
    FILE* file = fopen(pathZ.s, "rb");
    if (!file) {
        return {};
    }
    TempStr result = AllocStrTemp(limit);
    size_t n = fread(result.s, 1, (size_t)limit + 1, file);
    bool ok = !ferror(file) && n <= (size_t)limit;
    fclose(file);
    if (!ok) {
        return {};
    }
    result.s[n] = 0;
    result.len = (int)n;
    return result;
}

GPUI_NOINLINE void* ArenaVecAlloc(Arena* a, int count, int elSize, int align,
                                  int hdrSize) {
    if (!a || count <= 0 || elSize <= 0 || hdrSize < 0) {
        return nullptr;
    }
    if (align < 8) {
        align = 8;
    }
    if (count > (INT_MAX - hdrSize) / elSize) {
        return nullptr;
    }
    return a->Push((uint64_t)hdrSize + (uint64_t)count * (uint64_t)elSize,
                   (uint64_t)align, false);
}

GPUI_NOINLINE bool VecRealloc(Arena* a, void** els, int len, int* cap,
                              int newCap, int elSize) {

    if (elSize <= 0 || newCap < 0 || newCap > INT_MAX - 1) {
        return false;
    }
    int newElCount = newCap + 1;
    if (newElCount > INT_MAX / elSize) {
        return false;
    }

    int keep = len;
    keep = std::max(keep, 0);
    keep = std::min(keep, newCap);
    int oldSize = keep * elSize;
    int allocSize = newElCount * elSize;

    void* newEls = Realloc(a, *els, (size_t)allocSize, (size_t)oldSize);
    if (!newEls) {
        return false;
    }
    int tail = allocSize - oldSize;
    if (tail > 0) {
        memset((char*)newEls + oldSize, 0, (size_t)tail);
    }
    *els = newEls;
    *cap = newCap;
    return true;
}

GPUI_NOINLINE bool VecReserveNT(Arena* arena, VecNonTemplated* v, int elSize,
                                int wantedSize) {
    int cap = v->cap;
    int curCap = VecAbsCap(cap);
    if (wantedSize <= curCap) {
        return true;
    }
    int newCap = VecNextCap(curCap, wantedSize, elSize);
    if (cap < 0) {
        void* borrowed = v->els;
        v->els = nullptr;
        v->cap = 0;
        if (!VecRealloc(arena, &v->els, 0, &v->cap, newCap, elSize)) {
            v->els = borrowed;
            v->cap = -curCap;
            return false;
        }
        if (v->len > 0) {
            memcpy(v->els, borrowed, (size_t)v->len * (size_t)elSize);
        }
        return true;
    }
    return VecRealloc(arena, &v->els, v->len, &v->cap, newCap, elSize);
}

GPUI_NOINLINE void* VecInsertSpaceNT(VecNonTemplated* v, int elSize, int idx,
                                     int count) {
    int oldLen = v->len;
    int newLen = std::max(oldLen, idx) + count;
    if (!VecReserveNT(nullptr, v, elSize, newLen)) {
        return nullptr;
    }
    char* res = (char*)v->els + (size_t)idx * (size_t)elSize;
    if (oldLen > idx) {
        char* dst = res + (size_t)count * (size_t)elSize;
        memmove(dst, res, (size_t)(oldLen - idx) * (size_t)elSize);
    }
    v->len = newLen;
    return res;
}

GPUI_NOINLINE bool VecResizeNT(VecNonTemplated* v, int elSize, int newSize) {
    if (newSize < 0) {
        return false;
    }
    int curCap = VecAbsCap(v->cap);
    if (newSize > curCap) {
        if (!VecReserveNT(nullptr, v, elSize, newSize)) {
            return false;
        }
        curCap = VecAbsCap(v->cap);
    }
    v->len = newSize;
    if (v->els && curCap > newSize) {
        char* tail = (char*)v->els + (size_t)newSize * (size_t)elSize;
        memset(tail, 0, (size_t)(curCap - newSize) * (size_t)elSize);
    }
    return true;
}

GPUI_NOINLINE void VecRemoveAtNT(VecNonTemplated* v, int elSize, int idx,
                                 int count) {
    int oldLen = v->len;
    char* els = (char*)v->els;
    if (oldLen > idx + count) {
        char* dst = els + (size_t)idx * (size_t)elSize;
        char* src = els + (size_t)(idx + count) * (size_t)elSize;
        memmove(dst, src, (size_t)(oldLen - idx - count) * (size_t)elSize);
    }
    int newLen = oldLen - count;
    memset(els + (size_t)newLen * (size_t)elSize, 0,
           (size_t)count * (size_t)elSize);
    v->len = newLen;
}

GPUI_NOINLINE void VecRemoveAtFastNT(VecNonTemplated* v, int elSize, int idx) {
    int oldLen = v->len;
    if (idx >= oldLen) {
        return;
    }
    char* els = (char*)v->els;
    char* removed = els + (size_t)idx * (size_t)elSize;
    char* last = els + (size_t)(oldLen - 1) * (size_t)elSize;
    if (removed != last) {
        memcpy(removed, last, (size_t)elSize);
    }
    memset(last, 0, (size_t)elSize);
    v->len = oldLen - 1;
}

GPUI_NOINLINE void VecFreeElementsNT(VecNonTemplated* v) {
    v->len = 0;
    if (!v->els) {
        v->cap = 0;
        return;
    }
    if (v->cap > 0) {
        Free(nullptr, v->els);
    }
    v->cap = 0;
    v->els = nullptr;
}

GPUI_NOINLINE void VecClearNT(VecNonTemplated* v, int elSize) {
    v->len = 0;
    int curCap = VecAbsCap(v->cap);
    if (v->els && curCap > 0) {
        memset(v->els, 0, (size_t)curCap * (size_t)elSize);
    }
}

GPUI_NOINLINE void* VecTakeNT(VecNonTemplated* v, int elSize) {
    void* els = v->els;
    if (v->cap < 0) {
        int n = v->len;
        v->els = nullptr;
        v->cap = 0;
        v->len = 0;
        if (n <= 0) {
            return nullptr;
        }
        if (!VecRealloc(nullptr, &v->els, 0, &v->cap, n, elSize)) {
            return nullptr;
        }
        void* result = v->els;
        memcpy(result, els, (size_t)n * (size_t)elSize);
        v->els = nullptr;
        v->cap = 0;
        return result;
    }
    v->els = nullptr;
    v->len = 0;
    v->cap = 0;
    return els;
}

GPUI_NOINLINE void VecCopyFromNT(VecNonTemplated* v, int elSize, int srcLen,
                                 const void* srcEls, bool zeroTail) {
    VecReserveNT(nullptr, v, elSize, srcLen);
    v->len = srcLen;
    if (srcLen > 0 && srcEls && v->els) {
        memcpy(v->els, srcEls, (size_t)srcLen * (size_t)elSize);
    }
    if (zeroTail && v->els) {
        int curCap = VecAbsCap(v->cap);
        if (curCap > srcLen) {
            char* tail = (char*)v->els + (size_t)srcLen * (size_t)elSize;
            memset(tail, 0, (size_t)(curCap - srcLen) * (size_t)elSize);
        }
    }
}

#if defined(DEBUG)
static FILE* gVecDbgFile = nullptr;
static bool gVecDbgOpened = false;
static int gVecDbgNextId = 1;

static void VecDbgClose() {
    if (gVecDbgFile) {
        fclose(gVecDbgFile);
        gVecDbgFile = nullptr;
    }
}

static FILE* VecDbgOut() {
    if (!gVecDbgOpened) {
        gVecDbgOpened = true;
        const char* path = getenv("GPUI_VEC_LOG");
        if (path && *path) {
            gVecDbgFile = fopen(path, "wb");
            if (gVecDbgFile) {
                atexit(VecDbgClose);
            }
        }
    }
    return gVecDbgFile;
}

int VecDbgBirth(const char* file, int line, const char* func, char kind,
                int elSize) noexcept {
    int id = gVecDbgNextId++;
    FILE* f = VecDbgOut();
    if (f) {
        fprintf(f, "B %d %c %d %s %s:%d\n", id, kind, elSize,
                (func && *func) ? func : "-", file ? file : "<null>", line);
    }
    return id;
}

void VecDbgGrow(int id, int len, int oldCap, int needed, int newCap) noexcept {
    FILE* f = VecDbgOut();
    if (f) {
        fprintf(f, "G %d %d %d %d %d\n", id, len, oldCap, needed, newCap);
    }
}

void VecDbgSegment(int id, int len, int want, int lastSegCap, int newSegCap,
                   int totalCap, bool reused) noexcept {
    FILE* f = VecDbgOut();
    if (f) {
        fprintf(f, "S %d %d %d %d %d %d %d\n", id, len, want, lastSegCap,
                newSegCap, totalCap, reused ? 1 : 0);
    }
}

void VecDbgDeath(int id, int len, int cap) noexcept {
    FILE* f = VecDbgOut();
    if (f) {
        fprintf(f, "D %d %d %d\n", id, len, cap);
    }
}

void VecDbgArenaDeath(int id, int len, int totalCap, int segCount) noexcept {
    FILE* f = VecDbgOut();
    if (f) {
        fprintf(f, "E %d %d %d %d\n", id, len, totalCap, segCount);
    }
}
#endif

Str StrDup(Arena* a, Str s) {
    if (!s.s || len(s) < 0) {
        return {};
    }
    char* p =
        (char*)MemDup(a, s.s, (size_t)len(s) * sizeof(char), sizeof(char));
    return p ? Str(p, len(s)) : Str{};
}

Str StrDup(Str s) {
    return StrDup(nullptr, s);
}

void StrDup2(Str s1, Str s2, Str& s1Out, Str& s2Out) {
    s1Out = {};
    s2Out = {};
    int n1 = (!s1.s || len(s1) < 0) ? 0 : len(s1);
    int n2 = (!s2.s || len(s2) < 0) ? 0 : len(s2);
    if (n2 > INT_MAX - 2 - n1) {
        return;
    }
    int n = n1 + n2 + 2;
    char* p = (char*)Alloc(nullptr, n);
    if (!p) {
        return;
    }
    if (n1 > 0) {
        memcpy(p, s1.s, (size_t)n1);
    }
    p[n1] = 0;
    if (n2 > 0) {
        memcpy(p + n1 + 1, s2.s, (size_t)n2);
    }
    p[n1 + 1 + n2] = 0;
    s1Out = Str(p, n1);
    s2Out = Str(p + n1 + 1, n2);
}

void StrFree(Str s) {
    free(s.s);
}

int StrCopyCapped(char* dst, int cap, Str src) {
    if (!dst || cap <= 0) {
        return 0;
    }
    int n = std::min(len(src), cap - 1);
    if (n > 0) {
        memcpy(dst, src.s, (size_t)n);
    } else {
        n = 0;
    }
    dst[n] = 0;
    return n;
}

void StrCopyZ(char* dst, int cap, const char* src) {
    StrCopyCapped(dst, cap, src ? Str(src) : Str{});
}

void StrVecFree(Vec<Str>* v) {
    if (!v) {
        return;
    }
    for (int i = 0; i < len(*v); i++) {
        StrFree((*v)[i]);
    }
    VecReset(*v);
}

static bool DateParseIso(const char* s, LocalDate* out) {
    int part[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        if (i > 0) {
            if (*s != '-') {
                return false;
            }
            s++;
        }
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            part[i] = part[i] * 10 + (*s - '0');
            s++;
            digits++;
        }
        if (digits == 0 || digits > 4) {
            return false;
        }
    }
    if (*s != 0) {
        return false;
    }
    if (part[0] < 1 || part[1] < 1 || part[1] > 12 || part[2] < 1 ||
        part[2] > 31) {
        return false;
    }
    out->year = part[0];
    out->month = part[1];
    out->day = part[2];
    return true;
}

static bool gTodayChecked = false;
static LocalDate gTodayPinned = {};

LocalDate DateToday() {
    if (!gTodayChecked) {
        const char* env = getenv("GPUI_TODAY");
        if (env) {
            LocalDate pinned;
            if (DateParseIso(env, &pinned)) {
                gTodayPinned = pinned;
            }
        }

        gTodayChecked = true;
    }
    if (gTodayPinned.year != 0) {
        return gTodayPinned;
    }
    LocalDate out;
    time_t now = time(nullptr);
    struct tm* lt = localtime(&now);
    if (!lt) {
        return out;
    }
    out.year = lt->tm_year + 1900;
    out.month = lt->tm_mon + 1;
    out.day = lt->tm_mday;
    return out;
}

LocalTime TimeOfDayNow() {
    LocalTime out;
    (void)DateToday();
    if (gTodayPinned.year != 0) {
        return out;
    }
    time_t now = time(nullptr);
    struct tm* lt = localtime(&now);
    if (!lt) {
        return out;
    }
    out.hour = lt->tm_hour;
    out.minute = lt->tm_min;
    out.second = lt->tm_sec > 59 ? 59 : lt->tm_sec;
    return out;
}

LocalDate DateAddDays(LocalDate base, int days) {
    struct tm t = {};
    t.tm_year = base.year - 1900;
    t.tm_mon = base.month - 1;
    t.tm_mday = base.day + days;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    time_t stamp = mktime(&t);
    if (stamp == (time_t)-1) {
        return base;
    }
    LocalDate out;
    out.year = t.tm_year + 1900;
    out.month = t.tm_mon + 1;
    out.day = t.tm_mday;
    return out;
}

void StrLowerAscii(char* s) {
    if (!s) {
        return;
    }
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s - 'A' + 'a');
        }
    }
}

static bool StrEqRestCommon(Str s1, Str s2, bool ignoreCase) {
    if (s1.s == s2.s || len(s1) == 0) {
        return true;
    }
    if (!s1.s || !s2.s) {
        return false;
    }
    return ignoreCase ? StrCmpNI(s1.s, s2.s, len(s1)) == 0
                      : memcmp(s1.s, s2.s, (size_t)len(s1)) == 0;
}

GPUI_NOINLINE bool StrEqRest(Str s1, Str s2) {
    return StrEqRestCommon(s1, s2, false);
}

int StrCmp(Str s1, Str s2) {
    int common = std::min(len(s1), len(s2));
    int cmp = common > 0 ? memcmp(s1.s, s2.s, (size_t)common) : 0;
    if (cmp != 0) {
        return cmp;
    }
    return len(s1) < len(s2) ? -1 : len(s1) > len(s2) ? 1 : 0;
}

GPUI_NOINLINE bool StrEqIRest(Str s1, Str s2) {
    return StrEqRestCommon(s1, s2, true);
}

static bool StrHasAffix(Str s, Str affix, bool fromEnd, bool ignoreCase) {
    if (len(affix) > len(s)) {
        return false;
    }
    if (len(affix) == 0) {
        return true;
    }
    if (!s.s || !affix.s) {
        return false;
    }
    Str slice(s.s + (fromEnd ? len(s) - len(affix) : 0), len(affix));
    return ignoreCase ? StrEqI(slice, affix) : StrEq(slice, affix);
}

bool StrStartsWith(Str s, Str prefix) {
    return StrHasAffix(s, prefix, false, false);
}

bool StrStartsWithAny(Str s, const char* chars) {
    if (!s || !chars) {
        return false;
    }
    for (; *chars; chars++) {
        if (s.s[0] == *chars) {
            return true;
        }
    }
    return false;
}

bool StrStartsWithI(Str s, Str prefix) {
    return StrHasAffix(s, prefix, false, true);
}

bool StrEndsWith(Str s, Str suffix) {
    return StrHasAffix(s, suffix, true, false);
}

bool StrEndsWithI(Str s, Str suffix) {
    return StrHasAffix(s, suffix, true, true);
}

static int StrFindCommon(Str s, Str sub, bool ignoreCase) {
    if (!s.s || !sub.s || len(sub) <= 0 || len(sub) > len(s)) {
        return -1;
    }
    for (int off = 0; off + len(sub) <= len(s); off++) {
        Str slice(s.s + off, len(sub));
        if (ignoreCase ? StrEqI(slice, sub) : StrEq(slice, sub)) {
            return off;
        }
    }
    return -1;
}

int StrFind(Str s, Str sub) {
    return StrFindCommon(s, sub, false);
}

int StrFindI(Str s, Str sub) {
    return StrFindCommon(s, sub, true);
}

static bool IsStrTrimAscii(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

Str StrTrimAscii(Str s) {
    if (!s.s || len(s) <= 0) {
        return s;
    }
    int start = 0;
    int end = len(s);
    while (start < end && IsStrTrimAscii(s.s[start])) {
        start++;
    }
    while (end > start && IsStrTrimAscii(s.s[end - 1])) {
        end--;
    }
    return Str(s.s + start, end - start);
}

void HexEncode(const void* bytes, int n, char* out) {
    static const char kDigits[] = "0123456789abcdef";
    const uint8_t* p = (const uint8_t*)bytes;
    for (int i = 0; i < n; i++) {
        out[i * 2] = kDigits[p[i] >> 4];
        out[i * 2 + 1] = kDigits[p[i] & 15];
    }
}

bool ParseHexRgba(Str digits, uint32_t* rgba) {
    int n = len(digits);
    if (!digits.s || (n != 3 && n != 4 && n != 6 && n != 8)) {
        return false;
    }
    int d[8];
    for (int i = 0; i < n; i++) {
        d[i] = HexDigitValue(digits.s[i]);
        if (d[i] < 0) {
            return false;
        }
    }
    uint32_t v[4] = {0, 0, 0, 255};
    bool doubled = n <= 4;
    for (int i = 0; i < (doubled ? n : n / 2); i++) {
        v[i] = doubled ? (uint32_t)(d[i] * 17)
                       : (uint32_t)(d[i * 2] * 16 + d[i * 2 + 1]);
    }
    *rgba = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
    return true;
}

int PercentDecode(Str s, char* out) {
    int n = 0;
    for (int i = 0; i < len(s); i++) {
        char c = s.s[i];
        if (c == '%' && i + 2 < len(s)) {
            int hi = HexDigitValue(s.s[i + 1]);
            int lo = HexDigitValue(s.s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                c = (char)((hi << 4) | lo);
                i += 2;
            }
        }
        out[n++] = c;
    }
    return n;
}

int Utf8Encode(char* out, uint32_t cp) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int Utf8StrictAt(const uint8_t* b, int avail, uint32_t* cp) {
    if (avail <= 0) {
        return 0;
    }
    uint8_t lead = b[0];
    if (lead < 0x80) {
        if (cp) {
            *cp = lead;
        }
        return 1;
    }

    int need = 0;
    uint8_t lo = 0x80;
    uint8_t hi = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        need = 2;
    } else if (lead == 0xE0) {
        need = 3;
        lo = 0xA0;
    } else if (lead == 0xED) {
        need = 3;
        hi = 0x9F;
    } else if (lead >= 0xE1 && lead <= 0xEF) {
        need = 3;
    } else if (lead == 0xF0) {
        need = 4;
        lo = 0x90;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
        need = 4;
    } else if (lead == 0xF4) {
        need = 4;
        hi = 0x8F;
    } else {
        return 0;
    }
    uint32_t value = lead & (0x7F >> need);
    for (int k = 1; k < need; k++) {
        if (k >= avail) {
            return -1;
        }
        uint8_t byte = b[k];
        uint8_t min = k == 1 ? lo : (uint8_t)0x80;
        uint8_t max = k == 1 ? hi : (uint8_t)0xBF;
        if (byte < min || byte > max) {
            return 0;
        }
        value = (value << 6) | (byte & 0x3F);
    }
    if (cp) {
        *cp = value;
    }
    return need;
}

int Utf8At(Str s, int i, uint32_t* out) {
    const uint8_t* p = (const uint8_t*)s.s + i;
    uint8_t c = p[0];
    if (c < 0x80) {
        *out = c;
        return 1;
    }
    int n = Utf8LeadLen(c);
    if (n == 1 || i + n > len(s)) {
        *out = c;
        return 1;
    }
    uint32_t cp = (uint32_t)(c & (0xFF >> (n + 1)));
    for (int k = 1; k < n; k++) {
        if ((p[k] & 0xC0) != 0x80) {
            *out = c;
            return 1;
        }
        cp = (cp << 6) | (uint32_t)(p[k] & 0x3F);
    }
    *out = cp;
    return n;
}

int Utf8Prev(Str s, int i) {
    int j = i - 1;
    while (j > 0 && ((uint8_t)s.s[j] & 0xC0) == 0x80) {
        j--;
    }
    return j < 0 ? 0 : j;
}

bool Utf8IsCharBoundary(Str s, int i) {
    if (i < 0 || i > len(s)) {
        return false;
    }
    if (i == 0 || i == len(s)) {
        return true;
    }
    return ((uint8_t)s.s[i] & 0xC0) != 0x80;
}

int Utf8CharCount(Str s) {
    int n = 0;
    for (int i = 0; i < len(s); i++) {
        if (((uint8_t)s.s[i] & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

Str StrTrim(Str s) {
    if (!s.s || len(s) <= 0) {
        return s;
    }
    int start = 0;
    int end = len(s);
    while (start < end) {
        uint32_t cp = 0;
        int n = Utf8At(s, start, &cp);
        if (!IsUnicodeWhitespace(cp)) {
            break;
        }
        start += n;
    }
    while (end > start) {

        int at = end - 1;
        while (at > start && ((uint8_t)s.s[at] & 0xC0) == 0x80 &&
               end - at < 4) {
            at--;
        }
        uint32_t cp = 0;
        int n = Utf8At(s, at, &cp);
        if (at + n != end || !IsUnicodeWhitespace(cp)) {
            break;
        }
        end = at;
    }
    return Str(s.s + start, end - start);
}

Str StrReplaceAll(Str value, Str from, Str to) {
    if (len(from) == 0 || len(from) > len(value)) {
        return value;
    }
    int count = 0;
    for (int i = 0; i <= len(value) - len(from);) {
        int at = StrFind(Str(value.s + i, len(value) - i), from);
        if (at < 0) {
            break;
        }
        count++;
        i += at + len(from);
    }
    if (count == 0) {
        return value;
    }

    int64_t grown = (int64_t)len(value) +
                    (int64_t)count * ((int64_t)len(to) - (int64_t)len(from));
    if (grown < 0 || grown > (int64_t)INT_MAX - 1) {
        return value;
    }
    int resultLen = (int)grown;
    Str result = AllocStrTemp(resultLen + 1);
    if (!result.s) {
        return value;
    }
    int src = 0;
    int dst = 0;
    while (src < len(value)) {
        int remain = len(value) - src;
        int at = remain >= len(from) ? StrFind(Str(value.s + src, remain), from)
                                     : -1;
        if (at < 0) {
            memcpy(result.s + dst, value.s + src, (size_t)remain);
            dst += remain;
            break;
        }
        if (at > 0) {
            memcpy(result.s + dst, value.s + src, (size_t)at);
            dst += at;
            src += at;
        }
        if (len(to) > 0) {
            memcpy(result.s + dst, to.s, (size_t)len(to));
            dst += len(to);
        }
        src += len(from);
    }
    result.s[dst] = 0;
    result.len = dst;
    return result;
}

Str SeqStrFirst(SeqStrings strs) {
    if (!strs || !strs[0]) {
        return {};
    }
    return Str(strs);
}

Str SeqStrNext(Str s) {
    if (len(s) == 0) {
        return {};
    }
    const char* next = s.s + len(s) + 1;
    return next[0] ? Str(next) : Str{};
}

static int SeqStrIndexCmp(SeqStrings strs, Str toFind, bool ignoreCase) {
    if (!strs || !toFind) {
        return -1;
    }
    int idx = 0;
    for (Str cand = SeqStrFirst(strs); len(cand) > 0;
         cand = SeqStrNext(cand), idx++) {
        if (ignoreCase ? StrEqI(cand, toFind) : StrEq(cand, toFind)) {
            return idx;
        }
    }
    return -1;
}

int SeqStrIndex(SeqStrings strs, Str toFind) {
    return SeqStrIndexCmp(strs, toFind, false);
}

int SeqStrIndexIS(SeqStrings strs, Str toFind) {
    return SeqStrIndexCmp(strs, toFind, true);
}

bool SeqStrContainsI(SeqStrings strs, Str toFind) {
    return SeqStrIndexCmp(strs, toFind, true) >= 0;
}

Str SeqStrByIndex(SeqStrings strs, int idx) {
    if (idx < 0) {
        return {};
    }
    Str s = SeqStrFirst(strs);
    while (idx > 0 && len(s) > 0) {
        s = SeqStrNext(s);
        idx--;
    }
    return s;
}

int SeqStrCount(SeqStrings strs) {
    int n = 0;
    for (Str s = SeqStrFirst(strs); len(s) > 0; s = SeqStrNext(s)) {
        n++;
    }
    return n;
}

static bool IsDigit(char c) {
    return ('0' <= c) && (c <= '9');
}

static constexpr int kPadding = 1;

static bool IsNotOurHeapBlock(const StrBuilder& b) {
    return !b.els || b.cap < 0;
}

static void StrBuilderTerminate(StrBuilder& b) {
    if (b.els) {
        b.els[b.len] = 0;
    }
}

static char* StrBuilderEnsureCap(StrBuilder& b, int needed) {

    Vec<char>& storage = b;
    char* els = VecReserve(b.a, storage, needed);
    if (!els) {
        return nullptr;
    }
    if (b.a && b.cap > 0) {
        b.cap = -b.cap;
    }
    return els;
}

void StrBuilder::Reset(Str s) {

    len = 0;
    StrBuilderTerminate(*this);
    Append(s);
}

void StrBuilderUseExternalBuffer(StrBuilder& b, Str buf) {
    if (b.els || b.len != 0) {
        return;
    }
    if (buf.s && len(buf) > kPadding) {
        b.els = buf.s;
        b.cap = -(len(buf) - kPadding);
        b.els[0] = 0;
    }
}

bool StrBuilder::Reserve(int capacity) {
    if (!StrBuilderEnsureCap(*this, capacity)) {
        return false;
    }
    StrBuilderTerminate(*this);
    return true;
}

bool StrBuilder::AppendChar(char c) {
    if (!StrBuilderEnsureCap(*this, len + 1)) {
        return false;
    }
    els[len++] = c;
    StrBuilderTerminate(*this);
    return true;
}

bool StrBuilder::Append(Str src) {
    if (!src.s || src.len == 0) {
        return true;
    }
    if (!StrBuilderEnsureCap(*this, len + src.len)) {
        return false;
    }
    memcpy(els + len, src.s, (size_t)src.len);
    len += src.len;
    StrBuilderTerminate(*this);
    return true;
}

bool StrBuilder::AppendUtf8(uint32_t cp) {
    char buf[4];
    int n = Utf8Encode(buf, cp);
    return Append(Str(buf, n));
}

void AppendHtmlEscaped(StrBuilder& out, Str value, bool escapeGt,
                       bool escapeQuote) {
    for (int i = 0; i < len(value); i++) {
        char c = value.s[i];
        if (c == '&') {
            out.Append(StrL("&amp;"));
        } else if (c == '<') {
            out.Append(StrL("&lt;"));
        } else if (c == '>' && escapeGt) {
            out.Append(StrL("&gt;"));
        } else if (c == '"' && escapeQuote) {
            out.Append(StrL("&quot;"));
        } else {
            out.AppendChar(c);
        }
    }
}

char StrBuilder::RemoveAt(int idx, int count) {
    char result = els[idx];

    VecRemoveAtN(*this, idx, count);
    return result;
}

char StrBuilder::RemoveLast() {
    return len == 0 ? 0 : RemoveAt(len - 1);
}

Str StrBuilder::TakeStr() {
    int n = len;
    char* res = els;
    if (!els || n == 0) {
        Reset();
        return Str{};
    }
    if (IsNotOurHeapBlock(*this)) {

        res = (char*)MemDup(a, els, (size_t)n + kPadding);
    } else {

        els = nullptr;
        cap = 0;
    }
    Reset();
    return Str(res, n);
}

char StrBuilder::LastChar() const {
    return len == 0 ? 0 : els[len - 1];
}

struct Inst {
    FmtArg::Kind t = FmtArg::Kind::None;
    int argNo = 0;
    int rawOff = 0;

    int sLen = 0;

    char conv = 0;
    int intBits = 0;
    int fwpOff = 0;
    int fwpLen = 0;
    int width = 0;
    int prec = -1;
    bool leftJust = false;
};

struct Fmt {
    explicit Fmt(Arena* a) : res(a) {}
    ~Fmt() = default;

    bool Eval(const FmtArg** args, int nArgs);

    bool isOk =
        true;

    Str format;
    Inst instructions[32]{};
    int nInst = 0;

    int currArgNo = 0;
    int currPercArgNo = 0;
    StrBuilder res;

    char buf[256] = {};
};

static int parseUintAt(Str f, int* off) {
    int n = 0;
    while (*off < len(f) && IsDigit(f.s[*off])) {
        n = (n * 10) + (f.s[*off] - '0');
        (*off)++;
    }
    return n;
}

static void addRawStr(Fmt& fmt, int off, size_t n) {
    if (n == 0) {
        return;
    }
    if (fmt.nInst >= (int)dimof(fmt.instructions)) {
        fmt.isOk = false;
        return;
    }
    auto& i = fmt.instructions[fmt.nInst++];
    i.t = FmtArg::Kind::RawStr;
    i.rawOff = off;
    i.sLen = (int)n;
    i.argNo = -1;
}

static int parseArgDefBrace(Fmt& fmt, int off) {
    off++;
    int n = 0;
    bool positional = false;
    if (off < len(fmt.format) && IsDigit(fmt.format.s[off])) {
        n = parseUintAt(fmt.format, &off);
        positional = true;
    }
    while (off < len(fmt.format) && fmt.format.s[off] != '}') {
        if (!IsDigit(fmt.format.s[off])) {
            fmt.isOk = false;
            return off;
        }
        off++;
    }
    if (off >= len(fmt.format)) {
        fmt.isOk = false;
        return off;
    }
    if (fmt.nInst >= (int)dimof(fmt.instructions)) {
        fmt.isOk = false;
        return off;
    }
    auto& i = fmt.instructions[fmt.nInst++];
    i.t = FmtArg::Kind::Any;

    i.argNo = positional ? n : fmt.currPercArgNo++;
    return off + 1;
}

static FmtArg::Kind typeFromConv(char c) {
    switch (c) {
        case 'c':
            return FmtArg::Kind::Char;
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            return FmtArg::Kind::Int;
        case 'p':
            return FmtArg::Kind::Ptr;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            return FmtArg::Kind::Float;
        case 's':
        case 'S':
            return FmtArg::Kind::Str;
        case 'v':
            return FmtArg::Kind::Any;
        default:
            break;
    }
    return FmtArg::Kind::None;
}

static bool startsWith(Str s, int off, const char* prefix) {
    int i = 0;
    while (prefix[i]) {
        if (off + i >= len(s) || s.s[off + i] != prefix[i]) {
            return false;
        }
        i++;
    }
    return true;
}

static int parseLenMod(Str f, int off, int* bits) {
    *bits = 32;
    struct Mod {
        const char* s;
        int n;
        int wide;
    };
    static const Mod kMods[] = {
        {"I64", 3, 64},
        {"I32", 3, 32},
        {"ll", 2, 64},
        {"hh", 2, 32},
    };
    for (const Mod& m : kMods) {
        if (startsWith(f, off, m.s)) {
            *bits = m.wide;
            return off + m.n;
        }
    }
    char c = off < len(f) ? f.s[off] : 0;
    if (c == 'l' || c == 'h' || c == 'L' || c == 'w') {
        return off + 1;
    }
    if (c == 'z' || c == 'j' || c == 't' || c == 'I') {
        *bits = 64;
        return off + 1;
    }
    return off;
}

static int parseArgDefPerc(Fmt& fmt, int off) {
    Str f = fmt.format;
    off++;
    int fwpStart = off;
    bool leftJust = false;
    while (off < len(f) &&
           (f.s[off] == '-' || f.s[off] == '+' || f.s[off] == ' ' ||
            f.s[off] == '0' || f.s[off] == '#')) {
        if (f.s[off] == '-') {
            leftJust = true;
        }
        off++;
    }
    int width = parseUintAt(f, &off);
    int prec = -1;
    if (off < len(f) && f.s[off] == '.') {
        off++;
        prec = parseUintAt(f, &off);
    }
    int fwpEnd = off;
    int bits = 32;
    off = parseLenMod(f, off, &bits);
    char conv = (off < len(f)) ? f.s[off] : 0;
    off++;

    if (fmt.nInst >= (int)dimof(fmt.instructions)) {
        fmt.isOk = false;
        return off;
    }
    auto& i = fmt.instructions[fmt.nInst++];
    i.t = typeFromConv(conv);
    i.argNo = fmt.currPercArgNo++;
    i.conv = conv;
    i.intBits = bits;
    i.fwpOff = fwpStart;
    i.fwpLen = fwpEnd - fwpStart;
    i.width = width;
    i.prec = prec;
    i.leftJust = leftJust;
    return off;
}

static bool hasInstructionWithArgNo(Inst* insts, int nInst, int argNo) {
    for (int i = 0; i < nInst; i++) {
        if (insts[i].argNo == argNo) {
            return true;
        }
    }
    return false;
}

static bool isIntLike(FmtArg::Kind t) {
    return t == FmtArg::Kind::Char || t == FmtArg::Kind::Int ||
           t == FmtArg::Kind::Ptr;
}

static bool validArgTypes(FmtArg::Kind instType, FmtArg::Kind argType) {
    if (instType == FmtArg::Kind::Any || instType == FmtArg::Kind::RawStr) {
        return true;
    }

    if (instType == FmtArg::Kind::Char || instType == FmtArg::Kind::Int ||
        instType == FmtArg::Kind::Ptr) {
        return isIntLike(argType);
    }
    if (instType == FmtArg::Kind::Float) {
        return argType == FmtArg::Kind::Float ||
               argType == FmtArg::Kind::Double;
    }
    if (instType == FmtArg::Kind::Str) {
        return argType == FmtArg::Kind::Str;
    }
    return false;
}

static bool ParseFormat(Fmt& o, Str fmtStr) {
    o.format = fmtStr;
    o.nInst = 0;
    o.currPercArgNo = 0;
    o.currArgNo = 0;
    o.res.Reset();

    int start = 0;
    int off = 0;
    while (off < len(fmtStr) && fmtStr.s[off]) {
        char c = fmtStr.s[off];
        if ('%' == c) {

            if (off + 1 < len(fmtStr) && '%' == fmtStr.s[off + 1]) {
                addRawStr(o, start, off - start);
                start = off + 1;
                off += 2;
                continue;
            }
            addRawStr(o, start, off - start);
            if (off + 1 < len(fmtStr) && '{' == fmtStr.s[off + 1]) {
                off = parseArgDefBrace(o, off + 1);
            } else {
                off = parseArgDefPerc(o, off);
            }
            start = off;
            continue;
        }
        off++;
    }
    addRawStr(o, start, off - start);

    int maxArgNo = -1;

    for (int i = 0; i < o.nInst; i++) {
        if (o.instructions[i].t == FmtArg::Kind::RawStr) {
            continue;
        }
        maxArgNo = std::max(o.instructions[i].argNo, maxArgNo);
    }

    for (int i = 0; i <= maxArgNo; i++) {
        bool isOk = hasInstructionWithArgNo(o.instructions, o.nInst, i);
        if (!isOk) {
            return false;
        }
    }
    return true;
}

static bool appendConv(Fmt& fmt, const char* spec, ...) {
    va_list args;
    va_start(args, spec);
    va_list retry;
    va_copy(retry, args);
    Str bufS(fmt.buf, (int)dimof(fmt.buf));
    int n = VsnprintfUtf8(bufS, spec, args);
    va_end(args);
    fmt.buf[dimof(fmt.buf) - 1] = 0;
    if (n >= 0 && n < len(bufS)) {
        va_end(retry);
        return fmt.res.Append(Str(fmt.buf, n));
    }

    va_list write;
    va_copy(write, retry);
    int need = VscprintfUtf8(spec, retry);
    va_end(retry);
    bool ok = false;
    StrBuilder& res = fmt.res;
    int at = res.len;
    if (need >= 0 && need < INT_MAX - at - 1 && res.Reserve(at + need + 1)) {
        Str dst(res.els + at, need + 1);
        if (VsnprintfUtf8(dst, spec, write) == need) {
            res.len = at + need;
            StrBuilderTerminate(res);
            ok = true;
        }
    }
    va_end(write);
    return ok;
}

static bool evalDefault(Fmt& fmt, const FmtArg& arg) {
    switch (arg.t) {
        case FmtArg::Kind::Char:
            return fmt.res.AppendChar(arg.c);
        case FmtArg::Kind::Int:
            return appendConv(fmt, "%lld", (long long)arg.i);
        case FmtArg::Kind::Ptr:
            return appendConv(fmt, "%p", arg.ptr);
        case FmtArg::Kind::Float:

            return appendConv(fmt, "%G", (double)arg.f);
        case FmtArg::Kind::Double:
            return appendConv(fmt, "%G", arg.d);
        case FmtArg::Kind::Str:
            return fmt.res.Append(arg.str);
        default:
            return true;
    }
}

static int64_t argToI64(const FmtArg& arg) {
    switch (arg.t) {
        case FmtArg::Kind::Char:
            return (int64_t)arg.c;
        case FmtArg::Kind::Ptr:
            return (int64_t)(int64_t)arg.ptr;
        default:
            return arg.i;
    }
}

static bool appendSpaces(Fmt& fmt, int n) {
    for (int j = 0; j < n; j++) {
        if (!fmt.res.AppendChar(' ')) {
            return false;
        }
    }
    return true;
}

static bool isFloatConv(char c) {
    return c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' ||
           c == 'G' || c == 'a' || c == 'A';
}

static bool isUnsignedConv(char c) {
    return c == 'u' || c == 'o' || c == 'x' || c == 'X';
}

static bool evalPercInst(Fmt& fmt, const Inst& inst, const FmtArg& arg) {
    if (inst.conv == 's' || inst.conv == 'S') {
        int slen = arg.str.len;
        if (inst.prec >= 0 && inst.prec < slen) {
            slen = inst.prec;
        }
        int pad = std::max(inst.width - slen, 0);
        if (!inst.leftJust && !appendSpaces(fmt, pad)) {
            return false;
        }
        if (!fmt.res.Append(Str(arg.str.s, slen))) {
            return false;
        }
        return inst.leftJust ? appendSpaces(fmt, pad) : true;
    }
    if (inst.conv == 'p') {
        const void* pv = arg.t == FmtArg::Kind::Ptr
                             ? arg.ptr
                             : (const void*)(int64_t)argToI64(arg);
        return appendConv(fmt, "%p", pv);
    }

    char fbuf[64];
    int k = 0;
    fbuf[k++] = '%';
    for (int j = 0; j < inst.fwpLen && k < (int)dimof(fbuf) - 5; j++) {
        fbuf[k++] = fmt.format.s[inst.fwpOff + j];
    }
    bool wideInt =
        inst.intBits == 64 &&
        (inst.conv == 'd' || inst.conv == 'i' || isUnsignedConv(inst.conv));
    if (wideInt) {
        fbuf[k++] = 'l';
        fbuf[k++] = 'l';
    }
    fbuf[k++] = inst.conv == 'i' ? 'd' : inst.conv;
    fbuf[k] = 0;

    if (isFloatConv(inst.conv)) {
        double dv = arg.t == FmtArg::Kind::Double ? arg.d : (double)arg.f;
        return appendConv(fmt, fbuf, dv);
    }
    int64_t ival = argToI64(arg);
    if (isUnsignedConv(inst.conv)) {
        if (wideInt) {
            return appendConv(fmt, fbuf, (unsigned long long)ival);
        }
        return appendConv(fmt, fbuf, (unsigned int)(unsigned long long)ival);
    }
    if (wideInt) {
        return appendConv(fmt, fbuf, (long long)ival);
    }
    return appendConv(fmt, fbuf, (int)ival);
}

bool Fmt::Eval(const FmtArg** args, int nArgs) {
    if (!isOk) {

        return false;
    }

    for (int n = 0; n < nInst; n++) {
        auto& inst = instructions[n];

        if (inst.t == FmtArg::Kind::RawStr) {
            if (!res.Append(Str(format.s + inst.rawOff, inst.sLen))) {
                isOk = false;
                return false;
            }
            continue;
        }

        int argNo = inst.argNo;
        if (argNo < 0 || argNo >= nArgs) {
            isOk = false;
            return false;
        }

        const FmtArg& arg = *args[argNo];
        isOk = validArgTypes(inst.t, arg.t);
        if (!isOk) {
            return false;
        }

        bool appended = (inst.t == FmtArg::Kind::Any)
                            ? evalDefault(*this, arg)
                            : evalPercInst(*this, inst, arg);
        if (!appended) {
            isOk = false;
            return false;
        }
    }
    return true;
}

static Str FormatArgs(Arena* a, const char* fmt, const FmtArg** args,
                      int nArgs) {

    while (nArgs > 0 && args[nArgs - 1]->t == FmtArg::Kind::None) {
        nArgs--;
    }

    if (nArgs == 0) {

        bool hasDirective = false;
        for (const char* p = fmt; p && *p; p++) {
            if (*p == '%') {
                hasDirective = true;
                break;
            }
        }
        if (!hasDirective) {
            return StrDup(a, Str(fmt));
        }
    }

    Fmt f(a);

    bool ok = ParseFormat(f, Str(fmt));
    if (!ok) {
        return {};
    }
    ok = f.Eval(args, nArgs);
    if (!ok) {
        return {};
    }
    return f.res.TakeStr();
}

TempStr FormatTempArgs(const char* fmt, const FmtArg** args, int nArgs) {
    return FormatArgs(GetTempArena(), fmt, args, nArgs);
}

#if defined(_MSC_VER)
static _locale_t GetUtf8FormatLocale() {

    struct Locale {
        _locale_t loc = _create_locale(LC_ALL, ".UTF-8");
        ~Locale() {
            if (loc) {
                _free_locale(loc);
                loc = nullptr;
            }
        }
    };
    static Locale l;
    return l.loc;
}
#endif

static int VscprintfUtf8(const char* fmt, va_list args) {
#if defined(_MSC_VER)
    _locale_t loc = GetUtf8FormatLocale();
    if (loc) {
        return _vscprintf_l(fmt, loc, args);
    }
    return _vscprintf(fmt, args);
#else
    return vsnprintf(nullptr, 0, fmt, args);
#endif
}

static int VsnprintfUtf8(Str buf, const char* fmt, va_list args) {
#if defined(_MSC_VER)
    _locale_t loc = GetUtf8FormatLocale();
    if (loc) {
        return _vsnprintf_l(buf.s, (size_t)len(buf), fmt, loc, args);
    }
#endif
    return vsnprintf(buf.s, (size_t)len(buf), fmt, args);
}

char PlatPathSep() {
    return kPlatFacts.pathSep;
}
bool PlatPathsCaseFold() {
    return kPlatFacts.pathsCaseFold;
}
bool PlatIsWindows() {
    return kPlatFacts.isWindows;
}
bool PlatSecondaryIsCommand() {
    return kPlatFacts.secondaryIsCommand;
}
bool PlatShowsWindowControls() {
    return kPlatFacts.showsWindowControls;
}
bool PlatTitleBarCloseIsElement() {
    return kPlatFacts.titleBarCloseIsElement;
}
float PlatCaretWidth() {
    return kPlatFacts.caretWidth;
}
bool PlatScrollBounce() {
    return kPlatFacts.scrollBounce;
}
bool PlatCleanOnEscape() {
    return kPlatFacts.cleanOnEscape;
}
bool PlatScrollBounceTouch() {
    return kPlatFacts.scrollBounceTouch;
}
bool PlatOverscrollPhaselessStops() {
    return kPlatFacts.overscrollPhaselessStops;
}
const char* PlatMonoFontName() {
    return kPlatFacts.monoFontName;
}
const char* PlatShellDataDir() {
    return kPlatFacts.shellDataDir;
}
const char* PlatShellPlatformName() {
    return kPlatFacts.shellPlatformName;
}
bool PlatBlockSelectUsesControl() {
    return kPlatFacts.blockSelectUsesControl;
}
bool PlatScrollGestureLocks() {
    return kPlatFacts.scrollGestureLocks;
}
bool PlatAsyncIo() {
    return kPlatFacts.asyncIo;
}
float PlatWindowShadowSize() {
    return kPlatFacts.windowShadowSize;
}
uint64_t PlatArenaReserveSize() {
    return kPlatFacts.arenaReserveSize;
}

bool PlatGetExePath(char* out, int cap) {
    if (!out || cap <= 0) {
        return false;
    }
    out[0] = 0;
    return kPlatFacts.exePath && kPlatFacts.exePath(out, cap);
}

bool PlatDisclaimSpeechResponsibility(int argc, char** argv, int* status) {
    if (!kPlatFacts.disclaimSpeechResponsibility) {
        return true;
    }
    return kPlatFacts.disclaimSpeechResponsibility(argc, argv, status);
}

void PlatDirNameInPlace(char* path) {
    if (!path) {
        return;
    }
    int i = (int)strlen(path);
    while (i > 0 && path[i - 1] != '/') {
        path[--i] = 0;
    }
    while (i > 1 && path[i - 1] == '/') {
        path[--i] = 0;
    }
}

void PlatExeDirFromExePath(char* out, int cap) {
    if (PlatGetExePath(out, cap)) {
        PlatDirNameInPlace(out);
    }
}

bool PlatMemCommit(void* base, uint64_t size, bool largePages) {
    if (size == 0) {
        return true;
    }
    if (!base) {
        return false;
    }
    return PlatMemCommitPages(base, size, largePages);
}
}

#line 1 "src/base_entities.cpp"

namespace base {

static const char kHtmlEntityNames[] =
    "AElig\0"
    "AMP\0"
    "Aacute\0"
    "Abreve\0"
    "Acirc\0"
    "Acy\0"
    "Afr\0"
    "Agrave\0"
    "Alpha\0"
    "Amacr\0"
    "And\0"
    "Aogon\0"
    "Aopf\0"
    "ApplyFunction\0"
    "Aring\0"
    "Ascr\0"
    "Assign\0"
    "Atilde\0"
    "Auml\0"
    "Backslash\0"
    "Barv\0"
    "Barwed\0"
    "Bcy\0"
    "Because\0"
    "Bernoullis\0"
    "Beta\0"
    "Bfr\0"
    "Bopf\0"
    "Breve\0"
    "Bscr\0"
    "Bumpeq\0"
    "CHcy\0"
    "COPY\0"
    "Cacute\0"
    "Cap\0"
    "CapitalDifferentialD\0"
    "Cayleys\0"
    "Ccaron\0"
    "Ccedil\0"
    "Ccirc\0"
    "Cconint\0"
    "Cdot\0"
    "Cedilla\0"
    "CenterDot\0"
    "Cfr\0"
    "Chi\0"
    "CircleDot\0"
    "CircleMinus\0"
    "CirclePlus\0"
    "CircleTimes\0"
    "ClockwiseContourIntegral\0"
    "CloseCurlyDoubleQuote\0"
    "CloseCurlyQuote\0"
    "Colon\0"
    "Colone\0"
    "Congruent\0"
    "Conint\0"
    "ContourIntegral\0"
    "Copf\0"
    "Coproduct\0"
    "CounterClockwiseContourIntegral\0"
    "Cross\0"
    "Cscr\0"
    "Cup\0"
    "CupCap\0"
    "DD\0"
    "DDotrahd\0"
    "DJcy\0"
    "DScy\0"
    "DZcy\0"
    "Dagger\0"
    "Darr\0"
    "Dashv\0"
    "Dcaron\0"
    "Dcy\0"
    "Del\0"
    "Delta\0"
    "Dfr\0"
    "DiacriticalAcute\0"
    "DiacriticalDot\0"
    "DiacriticalDoubleAcute\0"
    "DiacriticalGrave\0"
    "DiacriticalTilde\0"
    "Diamond\0"
    "DifferentialD\0"
    "Dopf\0"
    "Dot\0"
    "DotDot\0"
    "DotEqual\0"
    "DoubleContourIntegral\0"
    "DoubleDot\0"
    "DoubleDownArrow\0"
    "DoubleLeftArrow\0"
    "DoubleLeftRightArrow\0"
    "DoubleLeftTee\0"
    "DoubleLongLeftArrow\0"
    "DoubleLongLeftRightArrow\0"
    "DoubleLongRightArrow\0"
    "DoubleRightArrow\0"
    "DoubleRightTee\0"
    "DoubleUpArrow\0"
    "DoubleUpDownArrow\0"
    "DoubleVerticalBar\0"
    "DownArrow\0"
    "DownArrowBar\0"
    "DownArrowUpArrow\0"
    "DownBreve\0"
    "DownLeftRightVector\0"
    "DownLeftTeeVector\0"
    "DownLeftVector\0"
    "DownLeftVectorBar\0"
    "DownRightTeeVector\0"
    "DownRightVector\0"
    "DownRightVectorBar\0"
    "DownTee\0"
    "DownTeeArrow\0"
    "Downarrow\0"
    "Dscr\0"
    "Dstrok\0"
    "ENG\0"
    "ETH\0"
    "Eacute\0"
    "Ecaron\0"
    "Ecirc\0"
    "Ecy\0"
    "Edot\0"
    "Efr\0"
    "Egrave\0"
    "Element\0"
    "Emacr\0"
    "EmptySmallSquare\0"
    "EmptyVerySmallSquare\0"
    "Eogon\0"
    "Eopf\0"
    "Epsilon\0"
    "Equal\0"
    "EqualTilde\0"
    "Equilibrium\0"
    "Escr\0"
    "Esim\0"
    "Eta\0"
    "Euml\0"
    "Exists\0"
    "ExponentialE\0"
    "Fcy\0"
    "Ffr\0"
    "FilledSmallSquare\0"
    "FilledVerySmallSquare\0"
    "Fopf\0"
    "ForAll\0"
    "Fouriertrf\0"
    "Fscr\0"
    "GJcy\0"
    "GT\0"
    "Gamma\0"
    "Gammad\0"
    "Gbreve\0"
    "Gcedil\0"
    "Gcirc\0"
    "Gcy\0"
    "Gdot\0"
    "Gfr\0"
    "Gg\0"
    "Gopf\0"
    "GreaterEqual\0"
    "GreaterEqualLess\0"
    "GreaterFullEqual\0"
    "GreaterGreater\0"
    "GreaterLess\0"
    "GreaterSlantEqual\0"
    "GreaterTilde\0"
    "Gscr\0"
    "Gt\0"
    "HARDcy\0"
    "Hacek\0"
    "Hat\0"
    "Hcirc\0"
    "Hfr\0"
    "HilbertSpace\0"
    "Hopf\0"
    "HorizontalLine\0"
    "Hscr\0"
    "Hstrok\0"
    "HumpDownHump\0"
    "HumpEqual\0"
    "IEcy\0"
    "IJlig\0"
    "IOcy\0"
    "Iacute\0"
    "Icirc\0"
    "Icy\0"
    "Idot\0"
    "Ifr\0"
    "Igrave\0"
    "Im\0"
    "Imacr\0"
    "ImaginaryI\0"
    "Implies\0"
    "Int\0"
    "Integral\0"
    "Intersection\0"
    "InvisibleComma\0"
    "InvisibleTimes\0"
    "Iogon\0"
    "Iopf\0"
    "Iota\0"
    "Iscr\0"
    "Itilde\0"
    "Iukcy\0"
    "Iuml\0"
    "Jcirc\0"
    "Jcy\0"
    "Jfr\0"
    "Jopf\0"
    "Jscr\0"
    "Jsercy\0"
    "Jukcy\0"
    "KHcy\0"
    "KJcy\0"
    "Kappa\0"
    "Kcedil\0"
    "Kcy\0"
    "Kfr\0"
    "Kopf\0"
    "Kscr\0"
    "LJcy\0"
    "LT\0"
    "Lacute\0"
    "Lambda\0"
    "Lang\0"
    "Laplacetrf\0"
    "Larr\0"
    "Lcaron\0"
    "Lcedil\0"
    "Lcy\0"
    "LeftAngleBracket\0"
    "LeftArrow\0"
    "LeftArrowBar\0"
    "LeftArrowRightArrow\0"
    "LeftCeiling\0"
    "LeftDoubleBracket\0"
    "LeftDownTeeVector\0"
    "LeftDownVector\0"
    "LeftDownVectorBar\0"
    "LeftFloor\0"
    "LeftRightArrow\0"
    "LeftRightVector\0"
    "LeftTee\0"
    "LeftTeeArrow\0"
    "LeftTeeVector\0"
    "LeftTriangle\0"
    "LeftTriangleBar\0"
    "LeftTriangleEqual\0"
    "LeftUpDownVector\0"
    "LeftUpTeeVector\0"
    "LeftUpVector\0"
    "LeftUpVectorBar\0"
    "LeftVector\0"
    "LeftVectorBar\0"
    "Leftarrow\0"
    "Leftrightarrow\0"
    "LessEqualGreater\0"
    "LessFullEqual\0"
    "LessGreater\0"
    "LessLess\0"
    "LessSlantEqual\0"
    "LessTilde\0"
    "Lfr\0"
    "Ll\0"
    "Lleftarrow\0"
    "Lmidot\0"
    "LongLeftArrow\0"
    "LongLeftRightArrow\0"
    "LongRightArrow\0"
    "Longleftarrow\0"
    "Longleftrightarrow\0"
    "Longrightarrow\0"
    "Lopf\0"
    "LowerLeftArrow\0"
    "LowerRightArrow\0"
    "Lscr\0"
    "Lsh\0"
    "Lstrok\0"
    "Lt\0"
    "Map\0"
    "Mcy\0"
    "MediumSpace\0"
    "Mellintrf\0"
    "Mfr\0"
    "MinusPlus\0"
    "Mopf\0"
    "Mscr\0"
    "Mu\0"
    "NJcy\0"
    "Nacute\0"
    "Ncaron\0"
    "Ncedil\0"
    "Ncy\0"
    "NegativeMediumSpace\0"
    "NegativeThickSpace\0"
    "NegativeThinSpace\0"
    "NegativeVeryThinSpace\0"
    "NestedGreaterGreater\0"
    "NestedLessLess\0"
    "NewLine\0"
    "Nfr\0"
    "NoBreak\0"
    "NonBreakingSpace\0"
    "Nopf\0"
    "Not\0"
    "NotCongruent\0"
    "NotCupCap\0"
    "NotDoubleVerticalBar\0"
    "NotElement\0"
    "NotEqual\0"
    "NotEqualTilde\0"
    "NotExists\0"
    "NotGreater\0"
    "NotGreaterEqual\0"
    "NotGreaterFullEqual\0"
    "NotGreaterGreater\0"
    "NotGreaterLess\0"
    "NotGreaterSlantEqual\0"
    "NotGreaterTilde\0"
    "NotHumpDownHump\0"
    "NotHumpEqual\0"
    "NotLeftTriangle\0"
    "NotLeftTriangleBar\0"
    "NotLeftTriangleEqual\0"
    "NotLess\0"
    "NotLessEqual\0"
    "NotLessGreater\0"
    "NotLessLess\0"
    "NotLessSlantEqual\0"
    "NotLessTilde\0"
    "NotNestedGreaterGreater\0"
    "NotNestedLessLess\0"
    "NotPrecedes\0"
    "NotPrecedesEqual\0"
    "NotPrecedesSlantEqual\0"
    "NotReverseElement\0"
    "NotRightTriangle\0"
    "NotRightTriangleBar\0"
    "NotRightTriangleEqual\0"
    "NotSquareSubset\0"
    "NotSquareSubsetEqual\0"
    "NotSquareSuperset\0"
    "NotSquareSupersetEqual\0"
    "NotSubset\0"
    "NotSubsetEqual\0"
    "NotSucceeds\0"
    "NotSucceedsEqual\0"
    "NotSucceedsSlantEqual\0"
    "NotSucceedsTilde\0"
    "NotSuperset\0"
    "NotSupersetEqual\0"
    "NotTilde\0"
    "NotTildeEqual\0"
    "NotTildeFullEqual\0"
    "NotTildeTilde\0"
    "NotVerticalBar\0"
    "Nscr\0"
    "Ntilde\0"
    "Nu\0"
    "OElig\0"
    "Oacute\0"
    "Ocirc\0"
    "Ocy\0"
    "Odblac\0"
    "Ofr\0"
    "Ograve\0"
    "Omacr\0"
    "Omega\0"
    "Omicron\0"
    "Oopf\0"
    "OpenCurlyDoubleQuote\0"
    "OpenCurlyQuote\0"
    "Or\0"
    "Oscr\0"
    "Oslash\0"
    "Otilde\0"
    "Otimes\0"
    "Ouml\0"
    "OverBar\0"
    "OverBrace\0"
    "OverBracket\0"
    "OverParenthesis\0"
    "PartialD\0"
    "Pcy\0"
    "Pfr\0"
    "Phi\0"
    "Pi\0"
    "PlusMinus\0"
    "Poincareplane\0"
    "Popf\0"
    "Pr\0"
    "Precedes\0"
    "PrecedesEqual\0"
    "PrecedesSlantEqual\0"
    "PrecedesTilde\0"
    "Prime\0"
    "Product\0"
    "Proportion\0"
    "Proportional\0"
    "Pscr\0"
    "Psi\0"
    "QUOT\0"
    "Qfr\0"
    "Qopf\0"
    "Qscr\0"
    "RBarr\0"
    "REG\0"
    "Racute\0"
    "Rang\0"
    "Rarr\0"
    "Rarrtl\0"
    "Rcaron\0"
    "Rcedil\0"
    "Rcy\0"
    "Re\0"
    "ReverseElement\0"
    "ReverseEquilibrium\0"
    "ReverseUpEquilibrium\0"
    "Rfr\0"
    "Rho\0"
    "RightAngleBracket\0"
    "RightArrow\0"
    "RightArrowBar\0"
    "RightArrowLeftArrow\0"
    "RightCeiling\0"
    "RightDoubleBracket\0"
    "RightDownTeeVector\0"
    "RightDownVector\0"
    "RightDownVectorBar\0"
    "RightFloor\0"
    "RightTee\0"
    "RightTeeArrow\0"
    "RightTeeVector\0"
    "RightTriangle\0"
    "RightTriangleBar\0"
    "RightTriangleEqual\0"
    "RightUpDownVector\0"
    "RightUpTeeVector\0"
    "RightUpVector\0"
    "RightUpVectorBar\0"
    "RightVector\0"
    "RightVectorBar\0"
    "Rightarrow\0"
    "Ropf\0"
    "RoundImplies\0"
    "Rrightarrow\0"
    "Rscr\0"
    "Rsh\0"
    "RuleDelayed\0"
    "SHCHcy\0"
    "SHcy\0"
    "SOFTcy\0"
    "Sacute\0"
    "Sc\0"
    "Scaron\0"
    "Scedil\0"
    "Scirc\0"
    "Scy\0"
    "Sfr\0"
    "ShortDownArrow\0"
    "ShortLeftArrow\0"
    "ShortRightArrow\0"
    "ShortUpArrow\0"
    "Sigma\0"
    "SmallCircle\0"
    "Sopf\0"
    "Sqrt\0"
    "Square\0"
    "SquareIntersection\0"
    "SquareSubset\0"
    "SquareSubsetEqual\0"
    "SquareSuperset\0"
    "SquareSupersetEqual\0"
    "SquareUnion\0"
    "Sscr\0"
    "Star\0"
    "Sub\0"
    "Subset\0"
    "SubsetEqual\0"
    "Succeeds\0"
    "SucceedsEqual\0"
    "SucceedsSlantEqual\0"
    "SucceedsTilde\0"
    "SuchThat\0"
    "Sum\0"
    "Sup\0"
    "Superset\0"
    "SupersetEqual\0"
    "Supset\0"
    "THORN\0"
    "TRADE\0"
    "TSHcy\0"
    "TScy\0"
    "Tab\0"
    "Tau\0"
    "Tcaron\0"
    "Tcedil\0"
    "Tcy\0"
    "Tfr\0"
    "Therefore\0"
    "Theta\0"
    "ThickSpace\0"
    "ThinSpace\0"
    "Tilde\0"
    "TildeEqual\0"
    "TildeFullEqual\0"
    "TildeTilde\0"
    "Topf\0"
    "TripleDot\0"
    "Tscr\0"
    "Tstrok\0"
    "Uacute\0"
    "Uarr\0"
    "Uarrocir\0"
    "Ubrcy\0"
    "Ubreve\0"
    "Ucirc\0"
    "Ucy\0"
    "Udblac\0"
    "Ufr\0"
    "Ugrave\0"
    "Umacr\0"
    "UnderBar\0"
    "UnderBrace\0"
    "UnderBracket\0"
    "UnderParenthesis\0"
    "Union\0"
    "UnionPlus\0"
    "Uogon\0"
    "Uopf\0"
    "UpArrow\0"
    "UpArrowBar\0"
    "UpArrowDownArrow\0"
    "UpDownArrow\0"
    "UpEquilibrium\0"
    "UpTee\0"
    "UpTeeArrow\0"
    "Uparrow\0"
    "Updownarrow\0"
    "UpperLeftArrow\0"
    "UpperRightArrow\0"
    "Upsi\0"
    "Upsilon\0"
    "Uring\0"
    "Uscr\0"
    "Utilde\0"
    "Uuml\0"
    "VDash\0"
    "Vbar\0"
    "Vcy\0"
    "Vdash\0"
    "Vdashl\0"
    "Vee\0"
    "Verbar\0"
    "Vert\0"
    "VerticalBar\0"
    "VerticalLine\0"
    "VerticalSeparator\0"
    "VerticalTilde\0"
    "VeryThinSpace\0"
    "Vfr\0"
    "Vopf\0"
    "Vscr\0"
    "Vvdash\0"
    "Wcirc\0"
    "Wedge\0"
    "Wfr\0"
    "Wopf\0"
    "Wscr\0"
    "Xfr\0"
    "Xi\0"
    "Xopf\0"
    "Xscr\0"
    "YAcy\0"
    "YIcy\0"
    "YUcy\0"
    "Yacute\0"
    "Ycirc\0"
    "Ycy\0"
    "Yfr\0"
    "Yopf\0"
    "Yscr\0"
    "Yuml\0"
    "ZHcy\0"
    "Zacute\0"
    "Zcaron\0"
    "Zcy\0"
    "Zdot\0"
    "ZeroWidthSpace\0"
    "Zeta\0"
    "Zfr\0"
    "Zopf\0"
    "Zscr\0"
    "aacute\0"
    "abreve\0"
    "ac\0"
    "acE\0"
    "acd\0"
    "acirc\0"
    "acute\0"
    "acy\0"
    "aelig\0"
    "af\0"
    "afr\0"
    "agrave\0"
    "alefsym\0"
    "aleph\0"
    "alpha\0"
    "amacr\0"
    "amalg\0"
    "amp\0"
    "and\0"
    "andand\0"
    "andd\0"
    "andslope\0"
    "andv\0"
    "ang\0"
    "ange\0"
    "angle\0"
    "angmsd\0"
    "angmsdaa\0"
    "angmsdab\0"
    "angmsdac\0"
    "angmsdad\0"
    "angmsdae\0"
    "angmsdaf\0"
    "angmsdag\0"
    "angmsdah\0"
    "angrt\0"
    "angrtvb\0"
    "angrtvbd\0"
    "angsph\0"
    "angst\0"
    "angzarr\0"
    "aogon\0"
    "aopf\0"
    "ap\0"
    "apE\0"
    "apacir\0"
    "ape\0"
    "apid\0"
    "apos\0"
    "approx\0"
    "approxeq\0"
    "aring\0"
    "ascr\0"
    "ast\0"
    "asymp\0"
    "asympeq\0"
    "atilde\0"
    "auml\0"
    "awconint\0"
    "awint\0"
    "bNot\0"
    "backcong\0"
    "backepsilon\0"
    "backprime\0"
    "backsim\0"
    "backsimeq\0"
    "barvee\0"
    "barwed\0"
    "barwedge\0"
    "bbrk\0"
    "bbrktbrk\0"
    "bcong\0"
    "bcy\0"
    "bdquo\0"
    "becaus\0"
    "because\0"
    "bemptyv\0"
    "bepsi\0"
    "bernou\0"
    "beta\0"
    "beth\0"
    "between\0"
    "bfr\0"
    "bigcap\0"
    "bigcirc\0"
    "bigcup\0"
    "bigodot\0"
    "bigoplus\0"
    "bigotimes\0"
    "bigsqcup\0"
    "bigstar\0"
    "bigtriangledown\0"
    "bigtriangleup\0"
    "biguplus\0"
    "bigvee\0"
    "bigwedge\0"
    "bkarow\0"
    "blacklozenge\0"
    "blacksquare\0"
    "blacktriangle\0"
    "blacktriangledown\0"
    "blacktriangleleft\0"
    "blacktriangleright\0"
    "blank\0"
    "blk12\0"
    "blk14\0"
    "blk34\0"
    "block\0"
    "bne\0"
    "bnequiv\0"
    "bnot\0"
    "bopf\0"
    "bot\0"
    "bottom\0"
    "bowtie\0"
    "boxDL\0"
    "boxDR\0"
    "boxDl\0"
    "boxDr\0"
    "boxH\0"
    "boxHD\0"
    "boxHU\0"
    "boxHd\0"
    "boxHu\0"
    "boxUL\0"
    "boxUR\0"
    "boxUl\0"
    "boxUr\0"
    "boxV\0"
    "boxVH\0"
    "boxVL\0"
    "boxVR\0"
    "boxVh\0"
    "boxVl\0"
    "boxVr\0"
    "boxbox\0"
    "boxdL\0"
    "boxdR\0"
    "boxdl\0"
    "boxdr\0"
    "boxh\0"
    "boxhD\0"
    "boxhU\0"
    "boxhd\0"
    "boxhu\0"
    "boxminus\0"
    "boxplus\0"
    "boxtimes\0"
    "boxuL\0"
    "boxuR\0"
    "boxul\0"
    "boxur\0"
    "boxv\0"
    "boxvH\0"
    "boxvL\0"
    "boxvR\0"
    "boxvh\0"
    "boxvl\0"
    "boxvr\0"
    "bprime\0"
    "breve\0"
    "brvbar\0"
    "bscr\0"
    "bsemi\0"
    "bsim\0"
    "bsime\0"
    "bsol\0"
    "bsolb\0"
    "bsolhsub\0"
    "bull\0"
    "bullet\0"
    "bump\0"
    "bumpE\0"
    "bumpe\0"
    "bumpeq\0"
    "cacute\0"
    "cap\0"
    "capand\0"
    "capbrcup\0"
    "capcap\0"
    "capcup\0"
    "capdot\0"
    "caps\0"
    "caret\0"
    "caron\0"
    "ccaps\0"
    "ccaron\0"
    "ccedil\0"
    "ccirc\0"
    "ccups\0"
    "ccupssm\0"
    "cdot\0"
    "cedil\0"
    "cemptyv\0"
    "cent\0"
    "centerdot\0"
    "cfr\0"
    "chcy\0"
    "check\0"
    "checkmark\0"
    "chi\0"
    "cir\0"
    "cirE\0"
    "circ\0"
    "circeq\0"
    "circlearrowleft\0"
    "circlearrowright\0"
    "circledR\0"
    "circledS\0"
    "circledast\0"
    "circledcirc\0"
    "circleddash\0"
    "cire\0"
    "cirfnint\0"
    "cirmid\0"
    "cirscir\0"
    "clubs\0"
    "clubsuit\0"
    "colon\0"
    "colone\0"
    "coloneq\0"
    "comma\0"
    "commat\0"
    "comp\0"
    "compfn\0"
    "complement\0"
    "complexes\0"
    "cong\0"
    "congdot\0"
    "conint\0"
    "copf\0"
    "coprod\0"
    "copy\0"
    "copysr\0"
    "crarr\0"
    "cross\0"
    "cscr\0"
    "csub\0"
    "csube\0"
    "csup\0"
    "csupe\0"
    "ctdot\0"
    "cudarrl\0"
    "cudarrr\0"
    "cuepr\0"
    "cuesc\0"
    "cularr\0"
    "cularrp\0"
    "cup\0"
    "cupbrcap\0"
    "cupcap\0"
    "cupcup\0"
    "cupdot\0"
    "cupor\0"
    "cups\0"
    "curarr\0"
    "curarrm\0"
    "curlyeqprec\0"
    "curlyeqsucc\0"
    "curlyvee\0"
    "curlywedge\0"
    "curren\0"
    "curvearrowleft\0"
    "curvearrowright\0"
    "cuvee\0"
    "cuwed\0"
    "cwconint\0"
    "cwint\0"
    "cylcty\0"
    "dArr\0"
    "dHar\0"
    "dagger\0"
    "daleth\0"
    "darr\0"
    "dash\0"
    "dashv\0"
    "dbkarow\0"
    "dblac\0"
    "dcaron\0"
    "dcy\0"
    "dd\0"
    "ddagger\0"
    "ddarr\0"
    "ddotseq\0"
    "deg\0"
    "delta\0"
    "demptyv\0"
    "dfisht\0"
    "dfr\0"
    "dharl\0"
    "dharr\0"
    "diam\0"
    "diamond\0"
    "diamondsuit\0"
    "diams\0"
    "die\0"
    "digamma\0"
    "disin\0"
    "div\0"
    "divide\0"
    "divideontimes\0"
    "divonx\0"
    "djcy\0"
    "dlcorn\0"
    "dlcrop\0"
    "dollar\0"
    "dopf\0"
    "dot\0"
    "doteq\0"
    "doteqdot\0"
    "dotminus\0"
    "dotplus\0"
    "dotsquare\0"
    "doublebarwedge\0"
    "downarrow\0"
    "downdownarrows\0"
    "downharpoonleft\0"
    "downharpoonright\0"
    "drbkarow\0"
    "drcorn\0"
    "drcrop\0"
    "dscr\0"
    "dscy\0"
    "dsol\0"
    "dstrok\0"
    "dtdot\0"
    "dtri\0"
    "dtrif\0"
    "duarr\0"
    "duhar\0"
    "dwangle\0"
    "dzcy\0"
    "dzigrarr\0"
    "eDDot\0"
    "eDot\0"
    "eacute\0"
    "easter\0"
    "ecaron\0"
    "ecir\0"
    "ecirc\0"
    "ecolon\0"
    "ecy\0"
    "edot\0"
    "ee\0"
    "efDot\0"
    "efr\0"
    "eg\0"
    "egrave\0"
    "egs\0"
    "egsdot\0"
    "el\0"
    "elinters\0"
    "ell\0"
    "els\0"
    "elsdot\0"
    "emacr\0"
    "empty\0"
    "emptyset\0"
    "emptyv\0"
    "emsp\0"
    "emsp13\0"
    "emsp14\0"
    "eng\0"
    "ensp\0"
    "eogon\0"
    "eopf\0"
    "epar\0"
    "eparsl\0"
    "eplus\0"
    "epsi\0"
    "epsilon\0"
    "epsiv\0"
    "eqcirc\0"
    "eqcolon\0"
    "eqsim\0"
    "eqslantgtr\0"
    "eqslantless\0"
    "equals\0"
    "equest\0"
    "equiv\0"
    "equivDD\0"
    "eqvparsl\0"
    "erDot\0"
    "erarr\0"
    "escr\0"
    "esdot\0"
    "esim\0"
    "eta\0"
    "eth\0"
    "euml\0"
    "euro\0"
    "excl\0"
    "exist\0"
    "expectation\0"
    "exponentiale\0"
    "fallingdotseq\0"
    "fcy\0"
    "female\0"
    "ffilig\0"
    "fflig\0"
    "ffllig\0"
    "ffr\0"
    "filig\0"
    "fjlig\0"
    "flat\0"
    "fllig\0"
    "fltns\0"
    "fnof\0"
    "fopf\0"
    "forall\0"
    "fork\0"
    "forkv\0"
    "fpartint\0"
    "frac12\0"
    "frac13\0"
    "frac14\0"
    "frac15\0"
    "frac16\0"
    "frac18\0"
    "frac23\0"
    "frac25\0"
    "frac34\0"
    "frac35\0"
    "frac38\0"
    "frac45\0"
    "frac56\0"
    "frac58\0"
    "frac78\0"
    "frasl\0"
    "frown\0"
    "fscr\0"
    "gE\0"
    "gEl\0"
    "gacute\0"
    "gamma\0"
    "gammad\0"
    "gap\0"
    "gbreve\0"
    "gcirc\0"
    "gcy\0"
    "gdot\0"
    "ge\0"
    "gel\0"
    "geq\0"
    "geqq\0"
    "geqslant\0"
    "ges\0"
    "gescc\0"
    "gesdot\0"
    "gesdoto\0"
    "gesdotol\0"
    "gesl\0"
    "gesles\0"
    "gfr\0"
    "gg\0"
    "ggg\0"
    "gimel\0"
    "gjcy\0"
    "gl\0"
    "glE\0"
    "gla\0"
    "glj\0"
    "gnE\0"
    "gnap\0"
    "gnapprox\0"
    "gne\0"
    "gneq\0"
    "gneqq\0"
    "gnsim\0"
    "gopf\0"
    "grave\0"
    "gscr\0"
    "gsim\0"
    "gsime\0"
    "gsiml\0"
    "gt\0"
    "gtcc\0"
    "gtcir\0"
    "gtdot\0"
    "gtlPar\0"
    "gtquest\0"
    "gtrapprox\0"
    "gtrarr\0"
    "gtrdot\0"
    "gtreqless\0"
    "gtreqqless\0"
    "gtrless\0"
    "gtrsim\0"
    "gvertneqq\0"
    "gvnE\0"
    "hArr\0"
    "hairsp\0"
    "half\0"
    "hamilt\0"
    "hardcy\0"
    "harr\0"
    "harrcir\0"
    "harrw\0"
    "hbar\0"
    "hcirc\0"
    "hearts\0"
    "heartsuit\0"
    "hellip\0"
    "hercon\0"
    "hfr\0"
    "hksearow\0"
    "hkswarow\0"
    "hoarr\0"
    "homtht\0"
    "hookleftarrow\0"
    "hookrightarrow\0"
    "hopf\0"
    "horbar\0"
    "hscr\0"
    "hslash\0"
    "hstrok\0"
    "hybull\0"
    "hyphen\0"
    "iacute\0"
    "ic\0"
    "icirc\0"
    "icy\0"
    "iecy\0"
    "iexcl\0"
    "iff\0"
    "ifr\0"
    "igrave\0"
    "ii\0"
    "iiiint\0"
    "iiint\0"
    "iinfin\0"
    "iiota\0"
    "ijlig\0"
    "imacr\0"
    "image\0"
    "imagline\0"
    "imagpart\0"
    "imath\0"
    "imof\0"
    "imped\0"
    "in\0"
    "incare\0"
    "infin\0"
    "infintie\0"
    "inodot\0"
    "int\0"
    "intcal\0"
    "integers\0"
    "intercal\0"
    "intlarhk\0"
    "intprod\0"
    "iocy\0"
    "iogon\0"
    "iopf\0"
    "iota\0"
    "iprod\0"
    "iquest\0"
    "iscr\0"
    "isin\0"
    "isinE\0"
    "isindot\0"
    "isins\0"
    "isinsv\0"
    "isinv\0"
    "it\0"
    "itilde\0"
    "iukcy\0"
    "iuml\0"
    "jcirc\0"
    "jcy\0"
    "jfr\0"
    "jmath\0"
    "jopf\0"
    "jscr\0"
    "jsercy\0"
    "jukcy\0"
    "kappa\0"
    "kappav\0"
    "kcedil\0"
    "kcy\0"
    "kfr\0"
    "kgreen\0"
    "khcy\0"
    "kjcy\0"
    "kopf\0"
    "kscr\0"
    "lAarr\0"
    "lArr\0"
    "lAtail\0"
    "lBarr\0"
    "lE\0"
    "lEg\0"
    "lHar\0"
    "lacute\0"
    "laemptyv\0"
    "lagran\0"
    "lambda\0"
    "lang\0"
    "langd\0"
    "langle\0"
    "lap\0"
    "laquo\0"
    "larr\0"
    "larrb\0"
    "larrbfs\0"
    "larrfs\0"
    "larrhk\0"
    "larrlp\0"
    "larrpl\0"
    "larrsim\0"
    "larrtl\0"
    "lat\0"
    "latail\0"
    "late\0"
    "lates\0"
    "lbarr\0"
    "lbbrk\0"
    "lbrace\0"
    "lbrack\0"
    "lbrke\0"
    "lbrksld\0"
    "lbrkslu\0"
    "lcaron\0"
    "lcedil\0"
    "lceil\0"
    "lcub\0"
    "lcy\0"
    "ldca\0"
    "ldquo\0"
    "ldquor\0"
    "ldrdhar\0"
    "ldrushar\0"
    "ldsh\0"
    "le\0"
    "leftarrow\0"
    "leftarrowtail\0"
    "leftharpoondown\0"
    "leftharpoonup\0"
    "leftleftarrows\0"
    "leftrightarrow\0"
    "leftrightarrows\0"
    "leftrightharpoons\0"
    "leftrightsquigarrow\0"
    "leftthreetimes\0"
    "leg\0"
    "leq\0"
    "leqq\0"
    "leqslant\0"
    "les\0"
    "lescc\0"
    "lesdot\0"
    "lesdoto\0"
    "lesdotor\0"
    "lesg\0"
    "lesges\0"
    "lessapprox\0"
    "lessdot\0"
    "lesseqgtr\0"
    "lesseqqgtr\0"
    "lessgtr\0"
    "lesssim\0"
    "lfisht\0"
    "lfloor\0"
    "lfr\0"
    "lg\0"
    "lgE\0"
    "lhard\0"
    "lharu\0"
    "lharul\0"
    "lhblk\0"
    "ljcy\0"
    "ll\0"
    "llarr\0"
    "llcorner\0"
    "llhard\0"
    "lltri\0"
    "lmidot\0"
    "lmoust\0"
    "lmoustache\0"
    "lnE\0"
    "lnap\0"
    "lnapprox\0"
    "lne\0"
    "lneq\0"
    "lneqq\0"
    "lnsim\0"
    "loang\0"
    "loarr\0"
    "lobrk\0"
    "longleftarrow\0"
    "longleftrightarrow\0"
    "longmapsto\0"
    "longrightarrow\0"
    "looparrowleft\0"
    "looparrowright\0"
    "lopar\0"
    "lopf\0"
    "loplus\0"
    "lotimes\0"
    "lowast\0"
    "lowbar\0"
    "loz\0"
    "lozenge\0"
    "lozf\0"
    "lpar\0"
    "lparlt\0"
    "lrarr\0"
    "lrcorner\0"
    "lrhar\0"
    "lrhard\0"
    "lrm\0"
    "lrtri\0"
    "lsaquo\0"
    "lscr\0"
    "lsh\0"
    "lsim\0"
    "lsime\0"
    "lsimg\0"
    "lsqb\0"
    "lsquo\0"
    "lsquor\0"
    "lstrok\0"
    "lt\0"
    "ltcc\0"
    "ltcir\0"
    "ltdot\0"
    "lthree\0"
    "ltimes\0"
    "ltlarr\0"
    "ltquest\0"
    "ltrPar\0"
    "ltri\0"
    "ltrie\0"
    "ltrif\0"
    "lurdshar\0"
    "luruhar\0"
    "lvertneqq\0"
    "lvnE\0"
    "mDDot\0"
    "macr\0"
    "male\0"
    "malt\0"
    "maltese\0"
    "map\0"
    "mapsto\0"
    "mapstodown\0"
    "mapstoleft\0"
    "mapstoup\0"
    "marker\0"
    "mcomma\0"
    "mcy\0"
    "mdash\0"
    "measuredangle\0"
    "mfr\0"
    "mho\0"
    "micro\0"
    "mid\0"
    "midast\0"
    "midcir\0"
    "middot\0"
    "minus\0"
    "minusb\0"
    "minusd\0"
    "minusdu\0"
    "mlcp\0"
    "mldr\0"
    "mnplus\0"
    "models\0"
    "mopf\0"
    "mp\0"
    "mscr\0"
    "mstpos\0"
    "mu\0"
    "multimap\0"
    "mumap\0"
    "nGg\0"
    "nGt\0"
    "nGtv\0"
    "nLeftarrow\0"
    "nLeftrightarrow\0"
    "nLl\0"
    "nLt\0"
    "nLtv\0"
    "nRightarrow\0"
    "nVDash\0"
    "nVdash\0"
    "nabla\0"
    "nacute\0"
    "nang\0"
    "nap\0"
    "napE\0"
    "napid\0"
    "napos\0"
    "napprox\0"
    "natur\0"
    "natural\0"
    "naturals\0"
    "nbsp\0"
    "nbump\0"
    "nbumpe\0"
    "ncap\0"
    "ncaron\0"
    "ncedil\0"
    "ncong\0"
    "ncongdot\0"
    "ncup\0"
    "ncy\0"
    "ndash\0"
    "ne\0"
    "neArr\0"
    "nearhk\0"
    "nearr\0"
    "nearrow\0"
    "nedot\0"
    "nequiv\0"
    "nesear\0"
    "nesim\0"
    "nexist\0"
    "nexists\0"
    "nfr\0"
    "ngE\0"
    "nge\0"
    "ngeq\0"
    "ngeqq\0"
    "ngeqslant\0"
    "nges\0"
    "ngsim\0"
    "ngt\0"
    "ngtr\0"
    "nhArr\0"
    "nharr\0"
    "nhpar\0"
    "ni\0"
    "nis\0"
    "nisd\0"
    "niv\0"
    "njcy\0"
    "nlArr\0"
    "nlE\0"
    "nlarr\0"
    "nldr\0"
    "nle\0"
    "nleftarrow\0"
    "nleftrightarrow\0"
    "nleq\0"
    "nleqq\0"
    "nleqslant\0"
    "nles\0"
    "nless\0"
    "nlsim\0"
    "nlt\0"
    "nltri\0"
    "nltrie\0"
    "nmid\0"
    "nopf\0"
    "not\0"
    "notin\0"
    "notinE\0"
    "notindot\0"
    "notinva\0"
    "notinvb\0"
    "notinvc\0"
    "notni\0"
    "notniva\0"
    "notnivb\0"
    "notnivc\0"
    "npar\0"
    "nparallel\0"
    "nparsl\0"
    "npart\0"
    "npolint\0"
    "npr\0"
    "nprcue\0"
    "npre\0"
    "nprec\0"
    "npreceq\0"
    "nrArr\0"
    "nrarr\0"
    "nrarrc\0"
    "nrarrw\0"
    "nrightarrow\0"
    "nrtri\0"
    "nrtrie\0"
    "nsc\0"
    "nsccue\0"
    "nsce\0"
    "nscr\0"
    "nshortmid\0"
    "nshortparallel\0"
    "nsim\0"
    "nsime\0"
    "nsimeq\0"
    "nsmid\0"
    "nspar\0"
    "nsqsube\0"
    "nsqsupe\0"
    "nsub\0"
    "nsubE\0"
    "nsube\0"
    "nsubset\0"
    "nsubseteq\0"
    "nsubseteqq\0"
    "nsucc\0"
    "nsucceq\0"
    "nsup\0"
    "nsupE\0"
    "nsupe\0"
    "nsupset\0"
    "nsupseteq\0"
    "nsupseteqq\0"
    "ntgl\0"
    "ntilde\0"
    "ntlg\0"
    "ntriangleleft\0"
    "ntrianglelefteq\0"
    "ntriangleright\0"
    "ntrianglerighteq\0"
    "nu\0"
    "num\0"
    "numero\0"
    "numsp\0"
    "nvDash\0"
    "nvHarr\0"
    "nvap\0"
    "nvdash\0"
    "nvge\0"
    "nvgt\0"
    "nvinfin\0"
    "nvlArr\0"
    "nvle\0"
    "nvlt\0"
    "nvltrie\0"
    "nvrArr\0"
    "nvrtrie\0"
    "nvsim\0"
    "nwArr\0"
    "nwarhk\0"
    "nwarr\0"
    "nwarrow\0"
    "nwnear\0"
    "oS\0"
    "oacute\0"
    "oast\0"
    "ocir\0"
    "ocirc\0"
    "ocy\0"
    "odash\0"
    "odblac\0"
    "odiv\0"
    "odot\0"
    "odsold\0"
    "oelig\0"
    "ofcir\0"
    "ofr\0"
    "ogon\0"
    "ograve\0"
    "ogt\0"
    "ohbar\0"
    "ohm\0"
    "oint\0"
    "olarr\0"
    "olcir\0"
    "olcross\0"
    "oline\0"
    "olt\0"
    "omacr\0"
    "omega\0"
    "omicron\0"
    "omid\0"
    "ominus\0"
    "oopf\0"
    "opar\0"
    "operp\0"
    "oplus\0"
    "or\0"
    "orarr\0"
    "ord\0"
    "order\0"
    "orderof\0"
    "ordf\0"
    "ordm\0"
    "origof\0"
    "oror\0"
    "orslope\0"
    "orv\0"
    "oscr\0"
    "oslash\0"
    "osol\0"
    "otilde\0"
    "otimes\0"
    "otimesas\0"
    "ouml\0"
    "ovbar\0"
    "par\0"
    "para\0"
    "parallel\0"
    "parsim\0"
    "parsl\0"
    "part\0"
    "pcy\0"
    "percnt\0"
    "period\0"
    "permil\0"
    "perp\0"
    "pertenk\0"
    "pfr\0"
    "phi\0"
    "phiv\0"
    "phmmat\0"
    "phone\0"
    "pi\0"
    "pitchfork\0"
    "piv\0"
    "planck\0"
    "planckh\0"
    "plankv\0"
    "plus\0"
    "plusacir\0"
    "plusb\0"
    "pluscir\0"
    "plusdo\0"
    "plusdu\0"
    "pluse\0"
    "plusmn\0"
    "plussim\0"
    "plustwo\0"
    "pm\0"
    "pointint\0"
    "popf\0"
    "pound\0"
    "pr\0"
    "prE\0"
    "prap\0"
    "prcue\0"
    "pre\0"
    "prec\0"
    "precapprox\0"
    "preccurlyeq\0"
    "preceq\0"
    "precnapprox\0"
    "precneqq\0"
    "precnsim\0"
    "precsim\0"
    "prime\0"
    "primes\0"
    "prnE\0"
    "prnap\0"
    "prnsim\0"
    "prod\0"
    "profalar\0"
    "profline\0"
    "profsurf\0"
    "prop\0"
    "propto\0"
    "prsim\0"
    "prurel\0"
    "pscr\0"
    "psi\0"
    "puncsp\0"
    "qfr\0"
    "qint\0"
    "qopf\0"
    "qprime\0"
    "qscr\0"
    "quaternions\0"
    "quatint\0"
    "quest\0"
    "questeq\0"
    "quot\0"
    "rAarr\0"
    "rArr\0"
    "rAtail\0"
    "rBarr\0"
    "rHar\0"
    "race\0"
    "racute\0"
    "radic\0"
    "raemptyv\0"
    "rang\0"
    "rangd\0"
    "range\0"
    "rangle\0"
    "raquo\0"
    "rarr\0"
    "rarrap\0"
    "rarrb\0"
    "rarrbfs\0"
    "rarrc\0"
    "rarrfs\0"
    "rarrhk\0"
    "rarrlp\0"
    "rarrpl\0"
    "rarrsim\0"
    "rarrtl\0"
    "rarrw\0"
    "ratail\0"
    "ratio\0"
    "rationals\0"
    "rbarr\0"
    "rbbrk\0"
    "rbrace\0"
    "rbrack\0"
    "rbrke\0"
    "rbrksld\0"
    "rbrkslu\0"
    "rcaron\0"
    "rcedil\0"
    "rceil\0"
    "rcub\0"
    "rcy\0"
    "rdca\0"
    "rdldhar\0"
    "rdquo\0"
    "rdquor\0"
    "rdsh\0"
    "real\0"
    "realine\0"
    "realpart\0"
    "reals\0"
    "rect\0"
    "reg\0"
    "rfisht\0"
    "rfloor\0"
    "rfr\0"
    "rhard\0"
    "rharu\0"
    "rharul\0"
    "rho\0"
    "rhov\0"
    "rightarrow\0"
    "rightarrowtail\0"
    "rightharpoondown\0"
    "rightharpoonup\0"
    "rightleftarrows\0"
    "rightleftharpoons\0"
    "rightrightarrows\0"
    "rightsquigarrow\0"
    "rightthreetimes\0"
    "ring\0"
    "risingdotseq\0"
    "rlarr\0"
    "rlhar\0"
    "rlm\0"
    "rmoust\0"
    "rmoustache\0"
    "rnmid\0"
    "roang\0"
    "roarr\0"
    "robrk\0"
    "ropar\0"
    "ropf\0"
    "roplus\0"
    "rotimes\0"
    "rpar\0"
    "rpargt\0"
    "rppolint\0"
    "rrarr\0"
    "rsaquo\0"
    "rscr\0"
    "rsh\0"
    "rsqb\0"
    "rsquo\0"
    "rsquor\0"
    "rthree\0"
    "rtimes\0"
    "rtri\0"
    "rtrie\0"
    "rtrif\0"
    "rtriltri\0"
    "ruluhar\0"
    "rx\0"
    "sacute\0"
    "sbquo\0"
    "sc\0"
    "scE\0"
    "scap\0"
    "scaron\0"
    "sccue\0"
    "sce\0"
    "scedil\0"
    "scirc\0"
    "scnE\0"
    "scnap\0"
    "scnsim\0"
    "scpolint\0"
    "scsim\0"
    "scy\0"
    "sdot\0"
    "sdotb\0"
    "sdote\0"
    "seArr\0"
    "searhk\0"
    "searr\0"
    "searrow\0"
    "sect\0"
    "semi\0"
    "seswar\0"
    "setminus\0"
    "setmn\0"
    "sext\0"
    "sfr\0"
    "sfrown\0"
    "sharp\0"
    "shchcy\0"
    "shcy\0"
    "shortmid\0"
    "shortparallel\0"
    "shy\0"
    "sigma\0"
    "sigmaf\0"
    "sigmav\0"
    "sim\0"
    "simdot\0"
    "sime\0"
    "simeq\0"
    "simg\0"
    "simgE\0"
    "siml\0"
    "simlE\0"
    "simne\0"
    "simplus\0"
    "simrarr\0"
    "slarr\0"
    "smallsetminus\0"
    "smashp\0"
    "smeparsl\0"
    "smid\0"
    "smile\0"
    "smt\0"
    "smte\0"
    "smtes\0"
    "softcy\0"
    "sol\0"
    "solb\0"
    "solbar\0"
    "sopf\0"
    "spades\0"
    "spadesuit\0"
    "spar\0"
    "sqcap\0"
    "sqcaps\0"
    "sqcup\0"
    "sqcups\0"
    "sqsub\0"
    "sqsube\0"
    "sqsubset\0"
    "sqsubseteq\0"
    "sqsup\0"
    "sqsupe\0"
    "sqsupset\0"
    "sqsupseteq\0"
    "squ\0"
    "square\0"
    "squarf\0"
    "squf\0"
    "srarr\0"
    "sscr\0"
    "ssetmn\0"
    "ssmile\0"
    "sstarf\0"
    "star\0"
    "starf\0"
    "straightepsilon\0"
    "straightphi\0"
    "strns\0"
    "sub\0"
    "subE\0"
    "subdot\0"
    "sube\0"
    "subedot\0"
    "submult\0"
    "subnE\0"
    "subne\0"
    "subplus\0"
    "subrarr\0"
    "subset\0"
    "subseteq\0"
    "subseteqq\0"
    "subsetneq\0"
    "subsetneqq\0"
    "subsim\0"
    "subsub\0"
    "subsup\0"
    "succ\0"
    "succapprox\0"
    "succcurlyeq\0"
    "succeq\0"
    "succnapprox\0"
    "succneqq\0"
    "succnsim\0"
    "succsim\0"
    "sum\0"
    "sung\0"
    "sup\0"
    "sup1\0"
    "sup2\0"
    "sup3\0"
    "supE\0"
    "supdot\0"
    "supdsub\0"
    "supe\0"
    "supedot\0"
    "suphsol\0"
    "suphsub\0"
    "suplarr\0"
    "supmult\0"
    "supnE\0"
    "supne\0"
    "supplus\0"
    "supset\0"
    "supseteq\0"
    "supseteqq\0"
    "supsetneq\0"
    "supsetneqq\0"
    "supsim\0"
    "supsub\0"
    "supsup\0"
    "swArr\0"
    "swarhk\0"
    "swarr\0"
    "swarrow\0"
    "swnwar\0"
    "szlig\0"
    "target\0"
    "tau\0"
    "tbrk\0"
    "tcaron\0"
    "tcedil\0"
    "tcy\0"
    "tdot\0"
    "telrec\0"
    "tfr\0"
    "there4\0"
    "therefore\0"
    "theta\0"
    "thetasym\0"
    "thetav\0"
    "thickapprox\0"
    "thicksim\0"
    "thinsp\0"
    "thkap\0"
    "thksim\0"
    "thorn\0"
    "tilde\0"
    "times\0"
    "timesb\0"
    "timesbar\0"
    "timesd\0"
    "tint\0"
    "toea\0"
    "top\0"
    "topbot\0"
    "topcir\0"
    "topf\0"
    "topfork\0"
    "tosa\0"
    "tprime\0"
    "trade\0"
    "triangle\0"
    "triangledown\0"
    "triangleleft\0"
    "trianglelefteq\0"
    "triangleq\0"
    "triangleright\0"
    "trianglerighteq\0"
    "tridot\0"
    "trie\0"
    "triminus\0"
    "triplus\0"
    "trisb\0"
    "tritime\0"
    "trpezium\0"
    "tscr\0"
    "tscy\0"
    "tshcy\0"
    "tstrok\0"
    "twixt\0"
    "twoheadleftarrow\0"
    "twoheadrightarrow\0"
    "uArr\0"
    "uHar\0"
    "uacute\0"
    "uarr\0"
    "ubrcy\0"
    "ubreve\0"
    "ucirc\0"
    "ucy\0"
    "udarr\0"
    "udblac\0"
    "udhar\0"
    "ufisht\0"
    "ufr\0"
    "ugrave\0"
    "uharl\0"
    "uharr\0"
    "uhblk\0"
    "ulcorn\0"
    "ulcorner\0"
    "ulcrop\0"
    "ultri\0"
    "umacr\0"
    "uml\0"
    "uogon\0"
    "uopf\0"
    "uparrow\0"
    "updownarrow\0"
    "upharpoonleft\0"
    "upharpoonright\0"
    "uplus\0"
    "upsi\0"
    "upsih\0"
    "upsilon\0"
    "upuparrows\0"
    "urcorn\0"
    "urcorner\0"
    "urcrop\0"
    "uring\0"
    "urtri\0"
    "uscr\0"
    "utdot\0"
    "utilde\0"
    "utri\0"
    "utrif\0"
    "uuarr\0"
    "uuml\0"
    "uwangle\0"
    "vArr\0"
    "vBar\0"
    "vBarv\0"
    "vDash\0"
    "vangrt\0"
    "varepsilon\0"
    "varkappa\0"
    "varnothing\0"
    "varphi\0"
    "varpi\0"
    "varpropto\0"
    "varr\0"
    "varrho\0"
    "varsigma\0"
    "varsubsetneq\0"
    "varsubsetneqq\0"
    "varsupsetneq\0"
    "varsupsetneqq\0"
    "vartheta\0"
    "vartriangleleft\0"
    "vartriangleright\0"
    "vcy\0"
    "vdash\0"
    "vee\0"
    "veebar\0"
    "veeeq\0"
    "vellip\0"
    "verbar\0"
    "vert\0"
    "vfr\0"
    "vltri\0"
    "vnsub\0"
    "vnsup\0"
    "vopf\0"
    "vprop\0"
    "vrtri\0"
    "vscr\0"
    "vsubnE\0"
    "vsubne\0"
    "vsupnE\0"
    "vsupne\0"
    "vzigzag\0"
    "wcirc\0"
    "wedbar\0"
    "wedge\0"
    "wedgeq\0"
    "weierp\0"
    "wfr\0"
    "wopf\0"
    "wp\0"
    "wr\0"
    "wreath\0"
    "wscr\0"
    "xcap\0"
    "xcirc\0"
    "xcup\0"
    "xdtri\0"
    "xfr\0"
    "xhArr\0"
    "xharr\0"
    "xi\0"
    "xlArr\0"
    "xlarr\0"
    "xmap\0"
    "xnis\0"
    "xodot\0"
    "xopf\0"
    "xoplus\0"
    "xotime\0"
    "xrArr\0"
    "xrarr\0"
    "xscr\0"
    "xsqcup\0"
    "xuplus\0"
    "xutri\0"
    "xvee\0"
    "xwedge\0"
    "yacute\0"
    "yacy\0"
    "ycirc\0"
    "ycy\0"
    "yen\0"
    "yfr\0"
    "yicy\0"
    "yopf\0"
    "yscr\0"
    "yucy\0"
    "yuml\0"
    "zacute\0"
    "zcaron\0"
    "zcy\0"
    "zdot\0"
    "zeetrf\0"
    "zeta\0"
    "zfr\0"
    "zhcy\0"
    "zigrarr\0"
    "zopf\0"
    "zscr\0"
    "zwj\0"
    "zwnj\0";

static const char kHtmlEntityValues[] =
    "Æ\0"
    "&\0"
    "Á\0"
    "Ă\0"
    "Â\0"
    "А\0"
    "𝔄\0"
    "À\0"
    "Α\0"
    "Ā\0"
    "⩓\0"
    "Ą\0"
    "𝔸\0"
    "⁡\0"
    "Å\0"
    "𝒜\0"
    "≔\0"
    "Ã\0"
    "Ä\0"
    "∖\0"
    "⫧\0"
    "⌆\0"
    "Б\0"
    "∵\0"
    "ℬ\0"
    "Β\0"
    "𝔅\0"
    "𝔹\0"
    "˘\0"
    "ℬ\0"
    "≎\0"
    "Ч\0"
    "©\0"
    "Ć\0"
    "⋒\0"
    "ⅅ\0"
    "ℭ\0"
    "Č\0"
    "Ç\0"
    "Ĉ\0"
    "∰\0"
    "Ċ\0"
    "¸\0"
    "·\0"
    "ℭ\0"
    "Χ\0"
    "⊙\0"
    "⊖\0"
    "⊕\0"
    "⊗\0"
    "∲\0"
    "”\0"
    "’\0"
    "∷\0"
    "⩴\0"
    "≡\0"
    "∯\0"
    "∮\0"
    "ℂ\0"
    "∐\0"
    "∳\0"
    "⨯\0"
    "𝒞\0"
    "⋓\0"
    "≍\0"
    "ⅅ\0"
    "⤑\0"
    "Ђ\0"
    "Ѕ\0"
    "Џ\0"
    "‡\0"
    "↡\0"
    "⫤\0"
    "Ď\0"
    "Д\0"
    "∇\0"
    "Δ\0"
    "𝔇\0"
    "´\0"
    "˙\0"
    "˝\0"
    "`\0"
    "˜\0"
    "⋄\0"
    "ⅆ\0"
    "𝔻\0"
    "¨\0"
    "⃜\0"
    "≐\0"
    "∯\0"
    "¨\0"
    "⇓\0"
    "⇐\0"
    "⇔\0"
    "⫤\0"
    "⟸\0"
    "⟺\0"
    "⟹\0"
    "⇒\0"
    "⊨\0"
    "⇑\0"
    "⇕\0"
    "∥\0"
    "↓\0"
    "⤓\0"
    "⇵\0"
    "̑\0"
    "⥐\0"
    "⥞\0"
    "↽\0"
    "⥖\0"
    "⥟\0"
    "⇁\0"
    "⥗\0"
    "⊤\0"
    "↧\0"
    "⇓\0"
    "𝒟\0"
    "Đ\0"
    "Ŋ\0"
    "Ð\0"
    "É\0"
    "Ě\0"
    "Ê\0"
    "Э\0"
    "Ė\0"
    "𝔈\0"
    "È\0"
    "∈\0"
    "Ē\0"
    "◻\0"
    "▫\0"
    "Ę\0"
    "𝔼\0"
    "Ε\0"
    "⩵\0"
    "≂\0"
    "⇌\0"
    "ℰ\0"
    "⩳\0"
    "Η\0"
    "Ë\0"
    "∃\0"
    "ⅇ\0"
    "Ф\0"
    "𝔉\0"
    "◼\0"
    "▪\0"
    "𝔽\0"
    "∀\0"
    "ℱ\0"
    "ℱ\0"
    "Ѓ\0"
    ">\0"
    "Γ\0"
    "Ϝ\0"
    "Ğ\0"
    "Ģ\0"
    "Ĝ\0"
    "Г\0"
    "Ġ\0"
    "𝔊\0"
    "⋙\0"
    "𝔾\0"
    "≥\0"
    "⋛\0"
    "≧\0"
    "⪢\0"
    "≷\0"
    "⩾\0"
    "≳\0"
    "𝒢\0"
    "≫\0"
    "Ъ\0"
    "ˇ\0"
    "^\0"
    "Ĥ\0"
    "ℌ\0"
    "ℋ\0"
    "ℍ\0"
    "─\0"
    "ℋ\0"
    "Ħ\0"
    "≎\0"
    "≏\0"
    "Е\0"
    "Ĳ\0"
    "Ё\0"
    "Í\0"
    "Î\0"
    "И\0"
    "İ\0"
    "ℑ\0"
    "Ì\0"
    "ℑ\0"
    "Ī\0"
    "ⅈ\0"
    "⇒\0"
    "∬\0"
    "∫\0"
    "⋂\0"
    "⁣\0"
    "⁢\0"
    "Į\0"
    "𝕀\0"
    "Ι\0"
    "ℐ\0"
    "Ĩ\0"
    "І\0"
    "Ï\0"
    "Ĵ\0"
    "Й\0"
    "𝔍\0"
    "𝕁\0"
    "𝒥\0"
    "Ј\0"
    "Є\0"
    "Х\0"
    "Ќ\0"
    "Κ\0"
    "Ķ\0"
    "К\0"
    "𝔎\0"
    "𝕂\0"
    "𝒦\0"
    "Љ\0"
    "<\0"
    "Ĺ\0"
    "Λ\0"
    "⟪\0"
    "ℒ\0"
    "↞\0"
    "Ľ\0"
    "Ļ\0"
    "Л\0"
    "⟨\0"
    "←\0"
    "⇤\0"
    "⇆\0"
    "⌈\0"
    "⟦\0"
    "⥡\0"
    "⇃\0"
    "⥙\0"
    "⌊\0"
    "↔\0"
    "⥎\0"
    "⊣\0"
    "↤\0"
    "⥚\0"
    "⊲\0"
    "⧏\0"
    "⊴\0"
    "⥑\0"
    "⥠\0"
    "↿\0"
    "⥘\0"
    "↼\0"
    "⥒\0"
    "⇐\0"
    "⇔\0"
    "⋚\0"
    "≦\0"
    "≶\0"
    "⪡\0"
    "⩽\0"
    "≲\0"
    "𝔏\0"
    "⋘\0"
    "⇚\0"
    "Ŀ\0"
    "⟵\0"
    "⟷\0"
    "⟶\0"
    "⟸\0"
    "⟺\0"
    "⟹\0"
    "𝕃\0"
    "↙\0"
    "↘\0"
    "ℒ\0"
    "↰\0"
    "Ł\0"
    "≪\0"
    "⤅\0"
    "М\0"
    " \0"
    "ℳ\0"
    "𝔐\0"
    "∓\0"
    "𝕄\0"
    "ℳ\0"
    "Μ\0"
    "Њ\0"
    "Ń\0"
    "Ň\0"
    "Ņ\0"
    "Н\0"
    "​\0"
    "​\0"
    "​\0"
    "​\0"
    "≫\0"
    "≪\0"
    "\n\0"
    "𝔑\0"
    "⁠\0"
    " \0"
    "ℕ\0"
    "⫬\0"
    "≢\0"
    "≭\0"
    "∦\0"
    "∉\0"
    "≠\0"
    "≂̸\0"
    "∄\0"
    "≯\0"
    "≱\0"
    "≧̸\0"
    "≫̸\0"
    "≹\0"
    "⩾̸\0"
    "≵\0"
    "≎̸\0"
    "≏̸\0"
    "⋪\0"
    "⧏̸\0"
    "⋬\0"
    "≮\0"
    "≰\0"
    "≸\0"
    "≪̸\0"
    "⩽̸\0"
    "≴\0"
    "⪢̸\0"
    "⪡̸\0"
    "⊀\0"
    "⪯̸\0"
    "⋠\0"
    "∌\0"
    "⋫\0"
    "⧐̸\0"
    "⋭\0"
    "⊏̸\0"
    "⋢\0"
    "⊐̸\0"
    "⋣\0"
    "⊂⃒\0"
    "⊈\0"
    "⊁\0"
    "⪰̸\0"
    "⋡\0"
    "≿̸\0"
    "⊃⃒\0"
    "⊉\0"
    "≁\0"
    "≄\0"
    "≇\0"
    "≉\0"
    "∤\0"
    "𝒩\0"
    "Ñ\0"
    "Ν\0"
    "Œ\0"
    "Ó\0"
    "Ô\0"
    "О\0"
    "Ő\0"
    "𝔒\0"
    "Ò\0"
    "Ō\0"
    "Ω\0"
    "Ο\0"
    "𝕆\0"
    "“\0"
    "‘\0"
    "⩔\0"
    "𝒪\0"
    "Ø\0"
    "Õ\0"
    "⨷\0"
    "Ö\0"
    "‾\0"
    "⏞\0"
    "⎴\0"
    "⏜\0"
    "∂\0"
    "П\0"
    "𝔓\0"
    "Φ\0"
    "Π\0"
    "±\0"
    "ℌ\0"
    "ℙ\0"
    "⪻\0"
    "≺\0"
    "⪯\0"
    "≼\0"
    "≾\0"
    "″\0"
    "∏\0"
    "∷\0"
    "∝\0"
    "𝒫\0"
    "Ψ\0"
    "\"\0"
    "𝔔\0"
    "ℚ\0"
    "𝒬\0"
    "⤐\0"
    "®\0"
    "Ŕ\0"
    "⟫\0"
    "↠\0"
    "⤖\0"
    "Ř\0"
    "Ŗ\0"
    "Р\0"
    "ℜ\0"
    "∋\0"
    "⇋\0"
    "⥯\0"
    "ℜ\0"
    "Ρ\0"
    "⟩\0"
    "→\0"
    "⇥\0"
    "⇄\0"
    "⌉\0"
    "⟧\0"
    "⥝\0"
    "⇂\0"
    "⥕\0"
    "⌋\0"
    "⊢\0"
    "↦\0"
    "⥛\0"
    "⊳\0"
    "⧐\0"
    "⊵\0"
    "⥏\0"
    "⥜\0"
    "↾\0"
    "⥔\0"
    "⇀\0"
    "⥓\0"
    "⇒\0"
    "ℝ\0"
    "⥰\0"
    "⇛\0"
    "ℛ\0"
    "↱\0"
    "⧴\0"
    "Щ\0"
    "Ш\0"
    "Ь\0"
    "Ś\0"
    "⪼\0"
    "Š\0"
    "Ş\0"
    "Ŝ\0"
    "С\0"
    "𝔖\0"
    "↓\0"
    "←\0"
    "→\0"
    "↑\0"
    "Σ\0"
    "∘\0"
    "𝕊\0"
    "√\0"
    "□\0"
    "⊓\0"
    "⊏\0"
    "⊑\0"
    "⊐\0"
    "⊒\0"
    "⊔\0"
    "𝒮\0"
    "⋆\0"
    "⋐\0"
    "⋐\0"
    "⊆\0"
    "≻\0"
    "⪰\0"
    "≽\0"
    "≿\0"
    "∋\0"
    "∑\0"
    "⋑\0"
    "⊃\0"
    "⊇\0"
    "⋑\0"
    "Þ\0"
    "™\0"
    "Ћ\0"
    "Ц\0"
    "\t\0"
    "Τ\0"
    "Ť\0"
    "Ţ\0"
    "Т\0"
    "𝔗\0"
    "∴\0"
    "Θ\0"
    "  \0"
    " \0"
    "∼\0"
    "≃\0"
    "≅\0"
    "≈\0"
    "𝕋\0"
    "⃛\0"
    "𝒯\0"
    "Ŧ\0"
    "Ú\0"
    "↟\0"
    "⥉\0"
    "Ў\0"
    "Ŭ\0"
    "Û\0"
    "У\0"
    "Ű\0"
    "𝔘\0"
    "Ù\0"
    "Ū\0"
    "_\0"
    "⏟\0"
    "⎵\0"
    "⏝\0"
    "⋃\0"
    "⊎\0"
    "Ų\0"
    "𝕌\0"
    "↑\0"
    "⤒\0"
    "⇅\0"
    "↕\0"
    "⥮\0"
    "⊥\0"
    "↥\0"
    "⇑\0"
    "⇕\0"
    "↖\0"
    "↗\0"
    "ϒ\0"
    "Υ\0"
    "Ů\0"
    "𝒰\0"
    "Ũ\0"
    "Ü\0"
    "⊫\0"
    "⫫\0"
    "В\0"
    "⊩\0"
    "⫦\0"
    "⋁\0"
    "‖\0"
    "‖\0"
    "∣\0"
    "|\0"
    "❘\0"
    "≀\0"
    " \0"
    "𝔙\0"
    "𝕍\0"
    "𝒱\0"
    "⊪\0"
    "Ŵ\0"
    "⋀\0"
    "𝔚\0"
    "𝕎\0"
    "𝒲\0"
    "𝔛\0"
    "Ξ\0"
    "𝕏\0"
    "𝒳\0"
    "Я\0"
    "Ї\0"
    "Ю\0"
    "Ý\0"
    "Ŷ\0"
    "Ы\0"
    "𝔜\0"
    "𝕐\0"
    "𝒴\0"
    "Ÿ\0"
    "Ж\0"
    "Ź\0"
    "Ž\0"
    "З\0"
    "Ż\0"
    "​\0"
    "Ζ\0"
    "ℨ\0"
    "ℤ\0"
    "𝒵\0"
    "á\0"
    "ă\0"
    "∾\0"
    "∾̳\0"
    "∿\0"
    "â\0"
    "´\0"
    "а\0"
    "æ\0"
    "⁡\0"
    "𝔞\0"
    "à\0"
    "ℵ\0"
    "ℵ\0"
    "α\0"
    "ā\0"
    "⨿\0"
    "&\0"
    "∧\0"
    "⩕\0"
    "⩜\0"
    "⩘\0"
    "⩚\0"
    "∠\0"
    "⦤\0"
    "∠\0"
    "∡\0"
    "⦨\0"
    "⦩\0"
    "⦪\0"
    "⦫\0"
    "⦬\0"
    "⦭\0"
    "⦮\0"
    "⦯\0"
    "∟\0"
    "⊾\0"
    "⦝\0"
    "∢\0"
    "Å\0"
    "⍼\0"
    "ą\0"
    "𝕒\0"
    "≈\0"
    "⩰\0"
    "⩯\0"
    "≊\0"
    "≋\0"
    "'\0"
    "≈\0"
    "≊\0"
    "å\0"
    "𝒶\0"
    "*\0"
    "≈\0"
    "≍\0"
    "ã\0"
    "ä\0"
    "∳\0"
    "⨑\0"
    "⫭\0"
    "≌\0"
    "϶\0"
    "‵\0"
    "∽\0"
    "⋍\0"
    "⊽\0"
    "⌅\0"
    "⌅\0"
    "⎵\0"
    "⎶\0"
    "≌\0"
    "б\0"
    "„\0"
    "∵\0"
    "∵\0"
    "⦰\0"
    "϶\0"
    "ℬ\0"
    "β\0"
    "ℶ\0"
    "≬\0"
    "𝔟\0"
    "⋂\0"
    "◯\0"
    "⋃\0"
    "⨀\0"
    "⨁\0"
    "⨂\0"
    "⨆\0"
    "★\0"
    "▽\0"
    "△\0"
    "⨄\0"
    "⋁\0"
    "⋀\0"
    "⤍\0"
    "⧫\0"
    "▪\0"
    "▴\0"
    "▾\0"
    "◂\0"
    "▸\0"
    "␣\0"
    "▒\0"
    "░\0"
    "▓\0"
    "█\0"
    "=⃥\0"
    "≡⃥\0"
    "⌐\0"
    "𝕓\0"
    "⊥\0"
    "⊥\0"
    "⋈\0"
    "╗\0"
    "╔\0"
    "╖\0"
    "╓\0"
    "═\0"
    "╦\0"
    "╩\0"
    "╤\0"
    "╧\0"
    "╝\0"
    "╚\0"
    "╜\0"
    "╙\0"
    "║\0"
    "╬\0"
    "╣\0"
    "╠\0"
    "╫\0"
    "╢\0"
    "╟\0"
    "⧉\0"
    "╕\0"
    "╒\0"
    "┐\0"
    "┌\0"
    "─\0"
    "╥\0"
    "╨\0"
    "┬\0"
    "┴\0"
    "⊟\0"
    "⊞\0"
    "⊠\0"
    "╛\0"
    "╘\0"
    "┘\0"
    "└\0"
    "│\0"
    "╪\0"
    "╡\0"
    "╞\0"
    "┼\0"
    "┤\0"
    "├\0"
    "‵\0"
    "˘\0"
    "¦\0"
    "𝒷\0"
    "⁏\0"
    "∽\0"
    "⋍\0"
    "\\\0"
    "⧅\0"
    "⟈\0"
    "•\0"
    "•\0"
    "≎\0"
    "⪮\0"
    "≏\0"
    "≏\0"
    "ć\0"
    "∩\0"
    "⩄\0"
    "⩉\0"
    "⩋\0"
    "⩇\0"
    "⩀\0"
    "∩︀\0"
    "⁁\0"
    "ˇ\0"
    "⩍\0"
    "č\0"
    "ç\0"
    "ĉ\0"
    "⩌\0"
    "⩐\0"
    "ċ\0"
    "¸\0"
    "⦲\0"
    "¢\0"
    "·\0"
    "𝔠\0"
    "ч\0"
    "✓\0"
    "✓\0"
    "χ\0"
    "○\0"
    "⧃\0"
    "ˆ\0"
    "≗\0"
    "↺\0"
    "↻\0"
    "®\0"
    "Ⓢ\0"
    "⊛\0"
    "⊚\0"
    "⊝\0"
    "≗\0"
    "⨐\0"
    "⫯\0"
    "⧂\0"
    "♣\0"
    "♣\0"
    ":\0"
    "≔\0"
    "≔\0"
    ",\0"
    "@\0"
    "∁\0"
    "∘\0"
    "∁\0"
    "ℂ\0"
    "≅\0"
    "⩭\0"
    "∮\0"
    "𝕔\0"
    "∐\0"
    "©\0"
    "℗\0"
    "↵\0"
    "✗\0"
    "𝒸\0"
    "⫏\0"
    "⫑\0"
    "⫐\0"
    "⫒\0"
    "⋯\0"
    "⤸\0"
    "⤵\0"
    "⋞\0"
    "⋟\0"
    "↶\0"
    "⤽\0"
    "∪\0"
    "⩈\0"
    "⩆\0"
    "⩊\0"
    "⊍\0"
    "⩅\0"
    "∪︀\0"
    "↷\0"
    "⤼\0"
    "⋞\0"
    "⋟\0"
    "⋎\0"
    "⋏\0"
    "¤\0"
    "↶\0"
    "↷\0"
    "⋎\0"
    "⋏\0"
    "∲\0"
    "∱\0"
    "⌭\0"
    "⇓\0"
    "⥥\0"
    "†\0"
    "ℸ\0"
    "↓\0"
    "‐\0"
    "⊣\0"
    "⤏\0"
    "˝\0"
    "ď\0"
    "д\0"
    "ⅆ\0"
    "‡\0"
    "⇊\0"
    "⩷\0"
    "°\0"
    "δ\0"
    "⦱\0"
    "⥿\0"
    "𝔡\0"
    "⇃\0"
    "⇂\0"
    "⋄\0"
    "⋄\0"
    "♦\0"
    "♦\0"
    "¨\0"
    "ϝ\0"
    "⋲\0"
    "÷\0"
    "÷\0"
    "⋇\0"
    "⋇\0"
    "ђ\0"
    "⌞\0"
    "⌍\0"
    "$\0"
    "𝕕\0"
    "˙\0"
    "≐\0"
    "≑\0"
    "∸\0"
    "∔\0"
    "⊡\0"
    "⌆\0"
    "↓\0"
    "⇊\0"
    "⇃\0"
    "⇂\0"
    "⤐\0"
    "⌟\0"
    "⌌\0"
    "𝒹\0"
    "ѕ\0"
    "⧶\0"
    "đ\0"
    "⋱\0"
    "▿\0"
    "▾\0"
    "⇵\0"
    "⥯\0"
    "⦦\0"
    "џ\0"
    "⟿\0"
    "⩷\0"
    "≑\0"
    "é\0"
    "⩮\0"
    "ě\0"
    "≖\0"
    "ê\0"
    "≕\0"
    "э\0"
    "ė\0"
    "ⅇ\0"
    "≒\0"
    "𝔢\0"
    "⪚\0"
    "è\0"
    "⪖\0"
    "⪘\0"
    "⪙\0"
    "⏧\0"
    "ℓ\0"
    "⪕\0"
    "⪗\0"
    "ē\0"
    "∅\0"
    "∅\0"
    "∅\0"
    " \0"
    " \0"
    " \0"
    "ŋ\0"
    " \0"
    "ę\0"
    "𝕖\0"
    "⋕\0"
    "⧣\0"
    "⩱\0"
    "ε\0"
    "ε\0"
    "ϵ\0"
    "≖\0"
    "≕\0"
    "≂\0"
    "⪖\0"
    "⪕\0"
    "=\0"
    "≟\0"
    "≡\0"
    "⩸\0"
    "⧥\0"
    "≓\0"
    "⥱\0"
    "ℯ\0"
    "≐\0"
    "≂\0"
    "η\0"
    "ð\0"
    "ë\0"
    "€\0"
    "!\0"
    "∃\0"
    "ℰ\0"
    "ⅇ\0"
    "≒\0"
    "ф\0"
    "♀\0"
    "ﬃ\0"
    "ﬀ\0"
    "ﬄ\0"
    "𝔣\0"
    "ﬁ\0"
    "fj\0"
    "♭\0"
    "ﬂ\0"
    "▱\0"
    "ƒ\0"
    "𝕗\0"
    "∀\0"
    "⋔\0"
    "⫙\0"
    "⨍\0"
    "½\0"
    "⅓\0"
    "¼\0"
    "⅕\0"
    "⅙\0"
    "⅛\0"
    "⅔\0"
    "⅖\0"
    "¾\0"
    "⅗\0"
    "⅜\0"
    "⅘\0"
    "⅚\0"
    "⅝\0"
    "⅞\0"
    "⁄\0"
    "⌢\0"
    "𝒻\0"
    "≧\0"
    "⪌\0"
    "ǵ\0"
    "γ\0"
    "ϝ\0"
    "⪆\0"
    "ğ\0"
    "ĝ\0"
    "г\0"
    "ġ\0"
    "≥\0"
    "⋛\0"
    "≥\0"
    "≧\0"
    "⩾\0"
    "⩾\0"
    "⪩\0"
    "⪀\0"
    "⪂\0"
    "⪄\0"
    "⋛︀\0"
    "⪔\0"
    "𝔤\0"
    "≫\0"
    "⋙\0"
    "ℷ\0"
    "ѓ\0"
    "≷\0"
    "⪒\0"
    "⪥\0"
    "⪤\0"
    "≩\0"
    "⪊\0"
    "⪊\0"
    "⪈\0"
    "⪈\0"
    "≩\0"
    "⋧\0"
    "𝕘\0"
    "`\0"
    "ℊ\0"
    "≳\0"
    "⪎\0"
    "⪐\0"
    ">\0"
    "⪧\0"
    "⩺\0"
    "⋗\0"
    "⦕\0"
    "⩼\0"
    "⪆\0"
    "⥸\0"
    "⋗\0"
    "⋛\0"
    "⪌\0"
    "≷\0"
    "≳\0"
    "≩︀\0"
    "≩︀\0"
    "⇔\0"
    " \0"
    "½\0"
    "ℋ\0"
    "ъ\0"
    "↔\0"
    "⥈\0"
    "↭\0"
    "ℏ\0"
    "ĥ\0"
    "♥\0"
    "♥\0"
    "…\0"
    "⊹\0"
    "𝔥\0"
    "⤥\0"
    "⤦\0"
    "⇿\0"
    "∻\0"
    "↩\0"
    "↪\0"
    "𝕙\0"
    "―\0"
    "𝒽\0"
    "ℏ\0"
    "ħ\0"
    "⁃\0"
    "‐\0"
    "í\0"
    "⁣\0"
    "î\0"
    "и\0"
    "е\0"
    "¡\0"
    "⇔\0"
    "𝔦\0"
    "ì\0"
    "ⅈ\0"
    "⨌\0"
    "∭\0"
    "⧜\0"
    "℩\0"
    "ĳ\0"
    "ī\0"
    "ℑ\0"
    "ℐ\0"
    "ℑ\0"
    "ı\0"
    "⊷\0"
    "Ƶ\0"
    "∈\0"
    "℅\0"
    "∞\0"
    "⧝\0"
    "ı\0"
    "∫\0"
    "⊺\0"
    "ℤ\0"
    "⊺\0"
    "⨗\0"
    "⨼\0"
    "ё\0"
    "į\0"
    "𝕚\0"
    "ι\0"
    "⨼\0"
    "¿\0"
    "𝒾\0"
    "∈\0"
    "⋹\0"
    "⋵\0"
    "⋴\0"
    "⋳\0"
    "∈\0"
    "⁢\0"
    "ĩ\0"
    "і\0"
    "ï\0"
    "ĵ\0"
    "й\0"
    "𝔧\0"
    "ȷ\0"
    "𝕛\0"
    "𝒿\0"
    "ј\0"
    "є\0"
    "κ\0"
    "ϰ\0"
    "ķ\0"
    "к\0"
    "𝔨\0"
    "ĸ\0"
    "х\0"
    "ќ\0"
    "𝕜\0"
    "𝓀\0"
    "⇚\0"
    "⇐\0"
    "⤛\0"
    "⤎\0"
    "≦\0"
    "⪋\0"
    "⥢\0"
    "ĺ\0"
    "⦴\0"
    "ℒ\0"
    "λ\0"
    "⟨\0"
    "⦑\0"
    "⟨\0"
    "⪅\0"
    "«\0"
    "←\0"
    "⇤\0"
    "⤟\0"
    "⤝\0"
    "↩\0"
    "↫\0"
    "⤹\0"
    "⥳\0"
    "↢\0"
    "⪫\0"
    "⤙\0"
    "⪭\0"
    "⪭︀\0"
    "⤌\0"
    "❲\0"
    "{\0"
    "[\0"
    "⦋\0"
    "⦏\0"
    "⦍\0"
    "ľ\0"
    "ļ\0"
    "⌈\0"
    "{\0"
    "л\0"
    "⤶\0"
    "“\0"
    "„\0"
    "⥧\0"
    "⥋\0"
    "↲\0"
    "≤\0"
    "←\0"
    "↢\0"
    "↽\0"
    "↼\0"
    "⇇\0"
    "↔\0"
    "⇆\0"
    "⇋\0"
    "↭\0"
    "⋋\0"
    "⋚\0"
    "≤\0"
    "≦\0"
    "⩽\0"
    "⩽\0"
    "⪨\0"
    "⩿\0"
    "⪁\0"
    "⪃\0"
    "⋚︀\0"
    "⪓\0"
    "⪅\0"
    "⋖\0"
    "⋚\0"
    "⪋\0"
    "≶\0"
    "≲\0"
    "⥼\0"
    "⌊\0"
    "𝔩\0"
    "≶\0"
    "⪑\0"
    "↽\0"
    "↼\0"
    "⥪\0"
    "▄\0"
    "љ\0"
    "≪\0"
    "⇇\0"
    "⌞\0"
    "⥫\0"
    "◺\0"
    "ŀ\0"
    "⎰\0"
    "⎰\0"
    "≨\0"
    "⪉\0"
    "⪉\0"
    "⪇\0"
    "⪇\0"
    "≨\0"
    "⋦\0"
    "⟬\0"
    "⇽\0"
    "⟦\0"
    "⟵\0"
    "⟷\0"
    "⟼\0"
    "⟶\0"
    "↫\0"
    "↬\0"
    "⦅\0"
    "𝕝\0"
    "⨭\0"
    "⨴\0"
    "∗\0"
    "_\0"
    "◊\0"
    "◊\0"
    "⧫\0"
    "(\0"
    "⦓\0"
    "⇆\0"
    "⌟\0"
    "⇋\0"
    "⥭\0"
    "\u200E\0"
    "⊿\0"
    "‹\0"
    "𝓁\0"
    "↰\0"
    "≲\0"
    "⪍\0"
    "⪏\0"
    "[\0"
    "‘\0"
    "‚\0"
    "ł\0"
    "<\0"
    "⪦\0"
    "⩹\0"
    "⋖\0"
    "⋋\0"
    "⋉\0"
    "⥶\0"
    "⩻\0"
    "⦖\0"
    "◃\0"
    "⊴\0"
    "◂\0"
    "⥊\0"
    "⥦\0"
    "≨︀\0"
    "≨︀\0"
    "∺\0"
    "¯\0"
    "♂\0"
    "✠\0"
    "✠\0"
    "↦\0"
    "↦\0"
    "↧\0"
    "↤\0"
    "↥\0"
    "▮\0"
    "⨩\0"
    "м\0"
    "—\0"
    "∡\0"
    "𝔪\0"
    "℧\0"
    "µ\0"
    "∣\0"
    "*\0"
    "⫰\0"
    "·\0"
    "−\0"
    "⊟\0"
    "∸\0"
    "⨪\0"
    "⫛\0"
    "…\0"
    "∓\0"
    "⊧\0"
    "𝕞\0"
    "∓\0"
    "𝓂\0"
    "∾\0"
    "μ\0"
    "⊸\0"
    "⊸\0"
    "⋙̸\0"
    "≫⃒\0"
    "≫̸\0"
    "⇍\0"
    "⇎\0"
    "⋘̸\0"
    "≪⃒\0"
    "≪̸\0"
    "⇏\0"
    "⊯\0"
    "⊮\0"
    "∇\0"
    "ń\0"
    "∠⃒\0"
    "≉\0"
    "⩰̸\0"
    "≋̸\0"
    "ŉ\0"
    "≉\0"
    "♮\0"
    "♮\0"
    "ℕ\0"
    " \0"
    "≎̸\0"
    "≏̸\0"
    "⩃\0"
    "ň\0"
    "ņ\0"
    "≇\0"
    "⩭̸\0"
    "⩂\0"
    "н\0"
    "–\0"
    "≠\0"
    "⇗\0"
    "⤤\0"
    "↗\0"
    "↗\0"
    "≐̸\0"
    "≢\0"
    "⤨\0"
    "≂̸\0"
    "∄\0"
    "∄\0"
    "𝔫\0"
    "≧̸\0"
    "≱\0"
    "≱\0"
    "≧̸\0"
    "⩾̸\0"
    "⩾̸\0"
    "≵\0"
    "≯\0"
    "≯\0"
    "⇎\0"
    "↮\0"
    "⫲\0"
    "∋\0"
    "⋼\0"
    "⋺\0"
    "∋\0"
    "њ\0"
    "⇍\0"
    "≦̸\0"
    "↚\0"
    "‥\0"
    "≰\0"
    "↚\0"
    "↮\0"
    "≰\0"
    "≦̸\0"
    "⩽̸\0"
    "⩽̸\0"
    "≮\0"
    "≴\0"
    "≮\0"
    "⋪\0"
    "⋬\0"
    "∤\0"
    "𝕟\0"
    "¬\0"
    "∉\0"
    "⋹̸\0"
    "⋵̸\0"
    "∉\0"
    "⋷\0"
    "⋶\0"
    "∌\0"
    "∌\0"
    "⋾\0"
    "⋽\0"
    "∦\0"
    "∦\0"
    "⫽⃥\0"
    "∂̸\0"
    "⨔\0"
    "⊀\0"
    "⋠\0"
    "⪯̸\0"
    "⊀\0"
    "⪯̸\0"
    "⇏\0"
    "↛\0"
    "⤳̸\0"
    "↝̸\0"
    "↛\0"
    "⋫\0"
    "⋭\0"
    "⊁\0"
    "⋡\0"
    "⪰̸\0"
    "𝓃\0"
    "∤\0"
    "∦\0"
    "≁\0"
    "≄\0"
    "≄\0"
    "∤\0"
    "∦\0"
    "⋢\0"
    "⋣\0"
    "⊄\0"
    "⫅̸\0"
    "⊈\0"
    "⊂⃒\0"
    "⊈\0"
    "⫅̸\0"
    "⊁\0"
    "⪰̸\0"
    "⊅\0"
    "⫆̸\0"
    "⊉\0"
    "⊃⃒\0"
    "⊉\0"
    "⫆̸\0"
    "≹\0"
    "ñ\0"
    "≸\0"
    "⋪\0"
    "⋬\0"
    "⋫\0"
    "⋭\0"
    "ν\0"
    "#\0"
    "№\0"
    " \0"
    "⊭\0"
    "⤄\0"
    "≍⃒\0"
    "⊬\0"
    "≥⃒\0"
    ">⃒\0"
    "⧞\0"
    "⤂\0"
    "≤⃒\0"
    "<⃒\0"
    "⊴⃒\0"
    "⤃\0"
    "⊵⃒\0"
    "∼⃒\0"
    "⇖\0"
    "⤣\0"
    "↖\0"
    "↖\0"
    "⤧\0"
    "Ⓢ\0"
    "ó\0"
    "⊛\0"
    "⊚\0"
    "ô\0"
    "о\0"
    "⊝\0"
    "ő\0"
    "⨸\0"
    "⊙\0"
    "⦼\0"
    "œ\0"
    "⦿\0"
    "𝔬\0"
    "˛\0"
    "ò\0"
    "⧁\0"
    "⦵\0"
    "Ω\0"
    "∮\0"
    "↺\0"
    "⦾\0"
    "⦻\0"
    "‾\0"
    "⧀\0"
    "ō\0"
    "ω\0"
    "ο\0"
    "⦶\0"
    "⊖\0"
    "𝕠\0"
    "⦷\0"
    "⦹\0"
    "⊕\0"
    "∨\0"
    "↻\0"
    "⩝\0"
    "ℴ\0"
    "ℴ\0"
    "ª\0"
    "º\0"
    "⊶\0"
    "⩖\0"
    "⩗\0"
    "⩛\0"
    "ℴ\0"
    "ø\0"
    "⊘\0"
    "õ\0"
    "⊗\0"
    "⨶\0"
    "ö\0"
    "⌽\0"
    "∥\0"
    "¶\0"
    "∥\0"
    "⫳\0"
    "⫽\0"
    "∂\0"
    "п\0"
    "%\0"
    ".\0"
    "‰\0"
    "⊥\0"
    "‱\0"
    "𝔭\0"
    "φ\0"
    "ϕ\0"
    "ℳ\0"
    "☎\0"
    "π\0"
    "⋔\0"
    "ϖ\0"
    "ℏ\0"
    "ℎ\0"
    "ℏ\0"
    "+\0"
    "⨣\0"
    "⊞\0"
    "⨢\0"
    "∔\0"
    "⨥\0"
    "⩲\0"
    "±\0"
    "⨦\0"
    "⨧\0"
    "±\0"
    "⨕\0"
    "𝕡\0"
    "£\0"
    "≺\0"
    "⪳\0"
    "⪷\0"
    "≼\0"
    "⪯\0"
    "≺\0"
    "⪷\0"
    "≼\0"
    "⪯\0"
    "⪹\0"
    "⪵\0"
    "⋨\0"
    "≾\0"
    "′\0"
    "ℙ\0"
    "⪵\0"
    "⪹\0"
    "⋨\0"
    "∏\0"
    "⌮\0"
    "⌒\0"
    "⌓\0"
    "∝\0"
    "∝\0"
    "≾\0"
    "⊰\0"
    "𝓅\0"
    "ψ\0"
    " \0"
    "𝔮\0"
    "⨌\0"
    "𝕢\0"
    "⁗\0"
    "𝓆\0"
    "ℍ\0"
    "⨖\0"
    "?\0"
    "≟\0"
    "\"\0"
    "⇛\0"
    "⇒\0"
    "⤜\0"
    "⤏\0"
    "⥤\0"
    "∽̱\0"
    "ŕ\0"
    "√\0"
    "⦳\0"
    "⟩\0"
    "⦒\0"
    "⦥\0"
    "⟩\0"
    "»\0"
    "→\0"
    "⥵\0"
    "⇥\0"
    "⤠\0"
    "⤳\0"
    "⤞\0"
    "↪\0"
    "↬\0"
    "⥅\0"
    "⥴\0"
    "↣\0"
    "↝\0"
    "⤚\0"
    "∶\0"
    "ℚ\0"
    "⤍\0"
    "❳\0"
    "}\0"
    "]\0"
    "⦌\0"
    "⦎\0"
    "⦐\0"
    "ř\0"
    "ŗ\0"
    "⌉\0"
    "}\0"
    "р\0"
    "⤷\0"
    "⥩\0"
    "”\0"
    "”\0"
    "↳\0"
    "ℜ\0"
    "ℛ\0"
    "ℜ\0"
    "ℝ\0"
    "▭\0"
    "®\0"
    "⥽\0"
    "⌋\0"
    "𝔯\0"
    "⇁\0"
    "⇀\0"
    "⥬\0"
    "ρ\0"
    "ϱ\0"
    "→\0"
    "↣\0"
    "⇁\0"
    "⇀\0"
    "⇄\0"
    "⇌\0"
    "⇉\0"
    "↝\0"
    "⋌\0"
    "˚\0"
    "≓\0"
    "⇄\0"
    "⇌\0"
    "\u200F\0"
    "⎱\0"
    "⎱\0"
    "⫮\0"
    "⟭\0"
    "⇾\0"
    "⟧\0"
    "⦆\0"
    "𝕣\0"
    "⨮\0"
    "⨵\0"
    ")\0"
    "⦔\0"
    "⨒\0"
    "⇉\0"
    "›\0"
    "𝓇\0"
    "↱\0"
    "]\0"
    "’\0"
    "’\0"
    "⋌\0"
    "⋊\0"
    "▹\0"
    "⊵\0"
    "▸\0"
    "⧎\0"
    "⥨\0"
    "℞\0"
    "ś\0"
    "‚\0"
    "≻\0"
    "⪴\0"
    "⪸\0"
    "š\0"
    "≽\0"
    "⪰\0"
    "ş\0"
    "ŝ\0"
    "⪶\0"
    "⪺\0"
    "⋩\0"
    "⨓\0"
    "≿\0"
    "с\0"
    "⋅\0"
    "⊡\0"
    "⩦\0"
    "⇘\0"
    "⤥\0"
    "↘\0"
    "↘\0"
    "§\0"
    ";\0"
    "⤩\0"
    "∖\0"
    "∖\0"
    "✶\0"
    "𝔰\0"
    "⌢\0"
    "♯\0"
    "щ\0"
    "ш\0"
    "∣\0"
    "∥\0"
    "­\0"
    "σ\0"
    "ς\0"
    "ς\0"
    "∼\0"
    "⩪\0"
    "≃\0"
    "≃\0"
    "⪞\0"
    "⪠\0"
    "⪝\0"
    "⪟\0"
    "≆\0"
    "⨤\0"
    "⥲\0"
    "←\0"
    "∖\0"
    "⨳\0"
    "⧤\0"
    "∣\0"
    "⌣\0"
    "⪪\0"
    "⪬\0"
    "⪬︀\0"
    "ь\0"
    "/\0"
    "⧄\0"
    "⌿\0"
    "𝕤\0"
    "♠\0"
    "♠\0"
    "∥\0"
    "⊓\0"
    "⊓︀\0"
    "⊔\0"
    "⊔︀\0"
    "⊏\0"
    "⊑\0"
    "⊏\0"
    "⊑\0"
    "⊐\0"
    "⊒\0"
    "⊐\0"
    "⊒\0"
    "□\0"
    "□\0"
    "▪\0"
    "▪\0"
    "→\0"
    "𝓈\0"
    "∖\0"
    "⌣\0"
    "⋆\0"
    "☆\0"
    "★\0"
    "ϵ\0"
    "ϕ\0"
    "¯\0"
    "⊂\0"
    "⫅\0"
    "⪽\0"
    "⊆\0"
    "⫃\0"
    "⫁\0"
    "⫋\0"
    "⊊\0"
    "⪿\0"
    "⥹\0"
    "⊂\0"
    "⊆\0"
    "⫅\0"
    "⊊\0"
    "⫋\0"
    "⫇\0"
    "⫕\0"
    "⫓\0"
    "≻\0"
    "⪸\0"
    "≽\0"
    "⪰\0"
    "⪺\0"
    "⪶\0"
    "⋩\0"
    "≿\0"
    "∑\0"
    "♪\0"
    "⊃\0"
    "¹\0"
    "²\0"
    "³\0"
    "⫆\0"
    "⪾\0"
    "⫘\0"
    "⊇\0"
    "⫄\0"
    "⟉\0"
    "⫗\0"
    "⥻\0"
    "⫂\0"
    "⫌\0"
    "⊋\0"
    "⫀\0"
    "⊃\0"
    "⊇\0"
    "⫆\0"
    "⊋\0"
    "⫌\0"
    "⫈\0"
    "⫔\0"
    "⫖\0"
    "⇙\0"
    "⤦\0"
    "↙\0"
    "↙\0"
    "⤪\0"
    "ß\0"
    "⌖\0"
    "τ\0"
    "⎴\0"
    "ť\0"
    "ţ\0"
    "т\0"
    "⃛\0"
    "⌕\0"
    "𝔱\0"
    "∴\0"
    "∴\0"
    "θ\0"
    "ϑ\0"
    "ϑ\0"
    "≈\0"
    "∼\0"
    " \0"
    "≈\0"
    "∼\0"
    "þ\0"
    "˜\0"
    "×\0"
    "⊠\0"
    "⨱\0"
    "⨰\0"
    "∭\0"
    "⤨\0"
    "⊤\0"
    "⌶\0"
    "⫱\0"
    "𝕥\0"
    "⫚\0"
    "⤩\0"
    "‴\0"
    "™\0"
    "▵\0"
    "▿\0"
    "◃\0"
    "⊴\0"
    "≜\0"
    "▹\0"
    "⊵\0"
    "◬\0"
    "≜\0"
    "⨺\0"
    "⨹\0"
    "⧍\0"
    "⨻\0"
    "⏢\0"
    "𝓉\0"
    "ц\0"
    "ћ\0"
    "ŧ\0"
    "≬\0"
    "↞\0"
    "↠\0"
    "⇑\0"
    "⥣\0"
    "ú\0"
    "↑\0"
    "ў\0"
    "ŭ\0"
    "û\0"
    "у\0"
    "⇅\0"
    "ű\0"
    "⥮\0"
    "⥾\0"
    "𝔲\0"
    "ù\0"
    "↿\0"
    "↾\0"
    "▀\0"
    "⌜\0"
    "⌜\0"
    "⌏\0"
    "◸\0"
    "ū\0"
    "¨\0"
    "ų\0"
    "𝕦\0"
    "↑\0"
    "↕\0"
    "↿\0"
    "↾\0"
    "⊎\0"
    "υ\0"
    "ϒ\0"
    "υ\0"
    "⇈\0"
    "⌝\0"
    "⌝\0"
    "⌎\0"
    "ů\0"
    "◹\0"
    "𝓊\0"
    "⋰\0"
    "ũ\0"
    "▵\0"
    "▴\0"
    "⇈\0"
    "ü\0"
    "⦧\0"
    "⇕\0"
    "⫨\0"
    "⫩\0"
    "⊨\0"
    "⦜\0"
    "ϵ\0"
    "ϰ\0"
    "∅\0"
    "ϕ\0"
    "ϖ\0"
    "∝\0"
    "↕\0"
    "ϱ\0"
    "ς\0"
    "⊊︀\0"
    "⫋︀\0"
    "⊋︀\0"
    "⫌︀\0"
    "ϑ\0"
    "⊲\0"
    "⊳\0"
    "в\0"
    "⊢\0"
    "∨\0"
    "⊻\0"
    "≚\0"
    "⋮\0"
    "|\0"
    "|\0"
    "𝔳\0"
    "⊲\0"
    "⊂⃒\0"
    "⊃⃒\0"
    "𝕧\0"
    "∝\0"
    "⊳\0"
    "𝓋\0"
    "⫋︀\0"
    "⊊︀\0"
    "⫌︀\0"
    "⊋︀\0"
    "⦚\0"
    "ŵ\0"
    "⩟\0"
    "∧\0"
    "≙\0"
    "℘\0"
    "𝔴\0"
    "𝕨\0"
    "℘\0"
    "≀\0"
    "≀\0"
    "𝓌\0"
    "⋂\0"
    "◯\0"
    "⋃\0"
    "▽\0"
    "𝔵\0"
    "⟺\0"
    "⟷\0"
    "ξ\0"
    "⟸\0"
    "⟵\0"
    "⟼\0"
    "⋻\0"
    "⨀\0"
    "𝕩\0"
    "⨁\0"
    "⨂\0"
    "⟹\0"
    "⟶\0"
    "𝓍\0"
    "⨆\0"
    "⨄\0"
    "△\0"
    "⋁\0"
    "⋀\0"
    "ý\0"
    "я\0"
    "ŷ\0"
    "ы\0"
    "¥\0"
    "𝔶\0"
    "ї\0"
    "𝕪\0"
    "𝓎\0"
    "ю\0"
    "ÿ\0"
    "ź\0"
    "ž\0"
    "з\0"
    "ż\0"
    "ℨ\0"
    "ζ\0"
    "𝔷\0"
    "ж\0"
    "⇝\0"
    "𝕫\0"
    "𝓏\0"
    "‍\0"
    "‌\0";

struct HtmlEntity {
    int32_t nameOff;
    int32_t valueOff;
};

static const HtmlEntity kHtmlEntities[2125] = {
    {0, 0},
    {6, 3},
    {10, 5},
    {17, 8},
    {24, 11},
    {30, 14},
    {34, 17},
    {38, 22},
    {45, 25},
    {51, 28},
    {57, 31},
    {61, 35},
    {67, 38},
    {72, 43},
    {86, 47},
    {92, 50},
    {97, 55},
    {104, 59},
    {111, 62},
    {116, 65},
    {126, 69},
    {131, 73},
    {138, 77},
    {142, 80},
    {150, 84},
    {161, 88},
    {166, 91},
    {170, 96},
    {175, 101},
    {181, 104},
    {186, 108},
    {193, 112},
    {198, 115},
    {203, 118},
    {210, 121},
    {214, 125},
    {235, 129},
    {243, 133},
    {250, 136},
    {257, 139},
    {263, 142},
    {271, 146},
    {276, 149},
    {284, 152},
    {294, 155},
    {298, 159},
    {302, 162},
    {312, 166},
    {324, 170},
    {335, 174},
    {347, 178},
    {372, 182},
    {394, 186},
    {410, 190},
    {416, 194},
    {423, 198},
    {433, 202},
    {440, 206},
    {456, 210},
    {461, 214},
    {471, 218},
    {503, 222},
    {509, 226},
    {514, 231},
    {518, 235},
    {525, 239},
    {528, 243},
    {537, 247},
    {542, 250},
    {547, 253},
    {552, 256},
    {559, 260},
    {564, 264},
    {570, 268},
    {577, 271},
    {581, 274},
    {585, 278},
    {591, 281},
    {595, 286},
    {612, 289},
    {627, 292},
    {650, 295},
    {667, 297},
    {684, 300},
    {692, 304},
    {706, 308},
    {711, 313},
    {715, 316},
    {722, 320},
    {731, 324},
    {753, 328},
    {763, 331},
    {779, 335},
    {795, 339},
    {816, 343},
    {830, 347},
    {850, 351},
    {875, 355},
    {896, 359},
    {913, 363},
    {928, 367},
    {942, 371},
    {960, 375},
    {978, 379},
    {988, 383},
    {1001, 387},
    {1018, 391},
    {1028, 394},
    {1048, 398},
    {1066, 402},
    {1081, 406},
    {1099, 410},
    {1118, 414},
    {1134, 418},
    {1153, 422},
    {1161, 426},
    {1174, 430},
    {1184, 434},
    {1189, 439},
    {1196, 442},
    {1200, 445},
    {1204, 448},
    {1211, 451},
    {1218, 454},
    {1224, 457},
    {1228, 460},
    {1233, 463},
    {1237, 468},
    {1244, 471},
    {1252, 475},
    {1258, 478},
    {1275, 482},
    {1296, 486},
    {1302, 489},
    {1307, 494},
    {1315, 497},
    {1321, 501},
    {1332, 505},
    {1344, 509},
    {1349, 513},
    {1354, 517},
    {1358, 520},
    {1363, 523},
    {1370, 527},
    {1383, 531},
    {1387, 534},
    {1391, 539},
    {1409, 543},
    {1431, 547},
    {1436, 552},
    {1443, 556},
    {1454, 560},
    {1459, 564},
    {1464, 567},
    {1467, 569},
    {1473, 572},
    {1480, 575},
    {1487, 578},
    {1494, 581},
    {1500, 584},
    {1504, 587},
    {1509, 590},
    {1513, 595},
    {1516, 599},
    {1521, 604},
    {1534, 608},
    {1551, 612},
    {1568, 616},
    {1583, 620},
    {1595, 624},
    {1613, 628},
    {1626, 632},
    {1631, 637},
    {1634, 641},
    {1641, 644},
    {1647, 647},
    {1651, 649},
    {1657, 652},
    {1661, 656},
    {1674, 660},
    {1679, 664},
    {1694, 668},
    {1699, 672},
    {1706, 675},
    {1719, 679},
    {1729, 683},
    {1734, 686},
    {1740, 689},
    {1745, 692},
    {1752, 695},
    {1758, 698},
    {1762, 701},
    {1767, 704},
    {1771, 708},
    {1778, 711},
    {1781, 715},
    {1787, 718},
    {1798, 722},
    {1806, 726},
    {1810, 730},
    {1819, 734},
    {1832, 738},
    {1847, 742},
    {1862, 746},
    {1868, 749},
    {1873, 754},
    {1878, 757},
    {1883, 761},
    {1890, 764},
    {1896, 767},
    {1901, 770},
    {1907, 773},
    {1911, 776},
    {1915, 781},
    {1920, 786},
    {1925, 791},
    {1932, 794},
    {1938, 797},
    {1943, 800},
    {1948, 803},
    {1954, 806},
    {1961, 809},
    {1965, 812},
    {1969, 817},
    {1974, 822},
    {1979, 827},
    {1984, 830},
    {1987, 832},
    {1994, 835},
    {2001, 838},
    {2006, 842},
    {2017, 846},
    {2022, 850},
    {2029, 853},
    {2036, 856},
    {2040, 859},
    {2057, 863},
    {2067, 867},
    {2080, 871},
    {2100, 875},
    {2112, 879},
    {2130, 883},
    {2148, 887},
    {2163, 891},
    {2181, 895},
    {2191, 899},
    {2206, 903},
    {2222, 907},
    {2230, 911},
    {2243, 915},
    {2257, 919},
    {2270, 923},
    {2286, 927},
    {2304, 931},
    {2321, 935},
    {2337, 939},
    {2350, 943},
    {2366, 947},
    {2377, 951},
    {2391, 955},
    {2401, 959},
    {2416, 963},
    {2433, 967},
    {2447, 971},
    {2459, 975},
    {2468, 979},
    {2483, 983},
    {2493, 987},
    {2497, 992},
    {2500, 996},
    {2511, 1000},
    {2518, 1003},
    {2532, 1007},
    {2551, 1011},
    {2566, 1015},
    {2580, 1019},
    {2599, 1023},
    {2614, 1027},
    {2619, 1032},
    {2634, 1036},
    {2650, 1040},
    {2655, 1044},
    {2659, 1048},
    {2666, 1051},
    {2669, 1055},
    {2673, 1059},
    {2677, 1062},
    {2689, 1066},
    {2699, 1070},
    {2703, 1075},
    {2713, 1079},
    {2718, 1084},
    {2723, 1088},
    {2726, 1091},
    {2731, 1094},
    {2738, 1097},
    {2745, 1100},
    {2752, 1103},
    {2756, 1106},
    {2776, 1110},
    {2795, 1114},
    {2813, 1118},
    {2835, 1122},
    {2856, 1126},
    {2871, 1130},
    {2879, 1132},
    {2883, 1137},
    {2891, 1141},
    {2908, 1144},
    {2913, 1148},
    {2917, 1152},
    {2930, 1156},
    {2940, 1160},
    {2961, 1164},
    {2972, 1168},
    {2981, 1172},
    {2995, 1178},
    {3005, 1182},
    {3016, 1186},
    {3032, 1190},
    {3052, 1196},
    {3070, 1202},
    {3085, 1206},
    {3106, 1212},
    {3122, 1216},
    {3138, 1222},
    {3151, 1228},
    {3167, 1232},
    {3186, 1238},
    {3207, 1242},
    {3215, 1246},
    {3228, 1250},
    {3243, 1254},
    {3255, 1260},
    {3273, 1266},
    {3286, 1270},
    {3310, 1276},
    {3328, 1282},
    {3340, 1286},
    {3357, 1292},
    {3379, 1296},
    {3397, 1300},
    {3414, 1304},
    {3434, 1310},
    {3456, 1314},
    {3472, 1320},
    {3493, 1324},
    {3511, 1330},
    {3534, 1334},
    {3544, 1341},
    {3559, 1345},
    {3571, 1349},
    {3588, 1355},
    {3610, 1359},
    {3627, 1365},
    {3639, 1372},
    {3656, 1376},
    {3665, 1380},
    {3679, 1384},
    {3697, 1388},
    {3711, 1392},
    {3726, 1396},
    {3731, 1401},
    {3738, 1404},
    {3741, 1407},
    {3747, 1410},
    {3754, 1413},
    {3760, 1416},
    {3764, 1419},
    {3771, 1422},
    {3775, 1427},
    {3782, 1430},
    {3788, 1433},
    {3794, 1436},
    {3802, 1439},
    {3807, 1444},
    {3828, 1448},
    {3843, 1452},
    {3846, 1456},
    {3851, 1461},
    {3858, 1464},
    {3865, 1467},
    {3872, 1471},
    {3877, 1474},
    {3885, 1478},
    {3895, 1482},
    {3907, 1486},
    {3923, 1490},
    {3932, 1494},
    {3936, 1497},
    {3940, 1502},
    {3944, 1505},
    {3947, 1508},
    {3957, 1511},
    {3971, 1515},
    {3976, 1519},
    {3979, 1523},
    {3988, 1527},
    {4002, 1531},
    {4021, 1535},
    {4035, 1539},
    {4041, 1543},
    {4049, 1547},
    {4060, 1551},
    {4073, 1555},
    {4078, 1560},
    {4082, 1563},
    {4087, 1565},
    {4091, 1570},
    {4096, 1574},
    {4101, 1579},
    {4107, 1583},
    {4111, 1586},
    {4118, 1589},
    {4123, 1593},
    {4128, 1597},
    {4135, 1601},
    {4142, 1604},
    {4149, 1607},
    {4153, 1610},
    {4156, 1614},
    {4171, 1618},
    {4190, 1622},
    {4211, 1626},
    {4215, 1630},
    {4219, 1633},
    {4237, 1637},
    {4248, 1641},
    {4262, 1645},
    {4282, 1649},
    {4295, 1653},
    {4314, 1657},
    {4333, 1661},
    {4349, 1665},
    {4368, 1669},
    {4379, 1673},
    {4388, 1677},
    {4402, 1681},
    {4417, 1685},
    {4431, 1689},
    {4448, 1693},
    {4467, 1697},
    {4485, 1701},
    {4502, 1705},
    {4516, 1709},
    {4533, 1713},
    {4545, 1717},
    {4560, 1721},
    {4571, 1725},
    {4576, 1729},
    {4589, 1733},
    {4601, 1737},
    {4606, 1741},
    {4610, 1745},
    {4622, 1749},
    {4629, 1752},
    {4634, 1755},
    {4641, 1758},
    {4648, 1761},
    {4651, 1765},
    {4658, 1768},
    {4665, 1771},
    {4671, 1774},
    {4675, 1777},
    {4679, 1782},
    {4694, 1786},
    {4709, 1790},
    {4725, 1794},
    {4738, 1798},
    {4744, 1801},
    {4756, 1805},
    {4761, 1810},
    {4766, 1814},
    {4773, 1818},
    {4792, 1822},
    {4805, 1826},
    {4823, 1830},
    {4838, 1834},
    {4858, 1838},
    {4870, 1842},
    {4875, 1847},
    {4880, 1851},
    {4884, 1855},
    {4891, 1859},
    {4903, 1863},
    {4912, 1867},
    {4926, 1871},
    {4945, 1875},
    {4959, 1879},
    {4968, 1883},
    {4972, 1887},
    {4976, 1891},
    {4985, 1895},
    {4999, 1899},
    {5006, 1903},
    {5012, 1906},
    {5018, 1910},
    {5024, 1913},
    {5029, 1916},
    {5033, 1918},
    {5037, 1921},
    {5044, 1924},
    {5051, 1927},
    {5055, 1930},
    {5059, 1935},
    {5069, 1939},
    {5075, 1942},
    {5086, 1949},
    {5096, 1953},
    {5102, 1957},
    {5113, 1961},
    {5128, 1965},
    {5139, 1969},
    {5144, 1974},
    {5154, 1978},
    {5159, 1983},
    {5166, 1986},
    {5173, 1989},
    {5178, 1993},
    {5187, 1997},
    {5193, 2000},
    {5200, 2003},
    {5206, 2006},
    {5210, 2009},
    {5217, 2012},
    {5221, 2017},
    {5228, 2020},
    {5234, 2023},
    {5243, 2025},
    {5254, 2029},
    {5267, 2033},
    {5284, 2037},
    {5290, 2041},
    {5300, 2045},
    {5306, 2048},
    {5311, 2053},
    {5319, 2057},
    {5330, 2061},
    {5347, 2065},
    {5359, 2069},
    {5373, 2073},
    {5379, 2077},
    {5390, 2081},
    {5398, 2085},
    {5410, 2089},
    {5425, 2093},
    {5441, 2097},
    {5446, 2100},
    {5454, 2103},
    {5460, 2106},
    {5465, 2111},
    {5472, 2114},
    {5477, 2117},
    {5483, 2121},
    {5488, 2125},
    {5492, 2128},
    {5498, 2132},
    {5505, 2136},
    {5509, 2140},
    {5516, 2144},
    {5521, 2148},
    {5533, 2152},
    {5546, 2154},
    {5564, 2158},
    {5578, 2162},
    {5592, 2166},
    {5596, 2171},
    {5601, 2176},
    {5606, 2181},
    {5613, 2185},
    {5619, 2188},
    {5625, 2192},
    {5629, 2197},
    {5634, 2202},
    {5639, 2207},
    {5643, 2212},
    {5646, 2215},
    {5651, 2220},
    {5656, 2225},
    {5661, 2228},
    {5666, 2231},
    {5671, 2234},
    {5678, 2237},
    {5684, 2240},
    {5688, 2243},
    {5692, 2248},
    {5697, 2253},
    {5702, 2258},
    {5707, 2261},
    {5712, 2264},
    {5719, 2267},
    {5726, 2270},
    {5730, 2273},
    {5735, 2276},
    {5750, 2280},
    {5755, 2283},
    {5759, 2287},
    {5764, 2291},
    {5769, 2296},
    {5776, 2299},
    {5783, 2302},
    {5786, 2306},
    {5790, 2312},
    {5794, 2316},
    {5800, 2319},
    {5806, 2322},
    {5810, 2325},
    {5816, 2328},
    {5819, 2332},
    {5823, 2337},
    {5830, 2340},
    {5838, 2344},
    {5844, 2348},
    {5850, 2351},
    {5856, 2354},
    {5862, 2358},
    {5866, 2360},
    {5870, 2364},
    {5877, 2368},
    {5882, 2372},
    {5891, 2376},
    {5896, 2380},
    {5900, 2384},
    {5905, 2388},
    {5911, 2392},
    {5918, 2396},
    {5927, 2400},
    {5936, 2404},
    {5945, 2408},
    {5954, 2412},
    {5963, 2416},
    {5972, 2420},
    {5981, 2424},
    {5990, 2428},
    {5996, 2432},
    {6004, 2436},
    {6013, 2440},
    {6020, 2444},
    {6026, 2447},
    {6034, 2451},
    {6040, 2454},
    {6045, 2459},
    {6048, 2463},
    {6052, 2467},
    {6059, 2471},
    {6063, 2475},
    {6068, 2479},
    {6073, 2481},
    {6080, 2485},
    {6089, 2489},
    {6095, 2492},
    {6100, 2497},
    {6104, 2499},
    {6110, 2503},
    {6118, 2507},
    {6125, 2510},
    {6130, 2513},
    {6139, 2517},
    {6145, 2521},
    {6150, 2525},
    {6159, 2529},
    {6171, 2532},
    {6181, 2536},
    {6189, 2540},
    {6199, 2544},
    {6206, 2548},
    {6213, 2552},
    {6222, 2556},
    {6227, 2560},
    {6236, 2564},
    {6242, 2568},
    {6246, 2571},
    {6252, 2575},
    {6259, 2579},
    {6267, 2583},
    {6275, 2587},
    {6281, 2590},
    {6288, 2594},
    {6293, 2597},
    {6298, 2601},
    {6306, 2605},
    {6310, 2610},
    {6317, 2614},
    {6325, 2618},
    {6332, 2622},
    {6340, 2626},
    {6349, 2630},
    {6359, 2634},
    {6368, 2638},
    {6376, 2642},
    {6392, 2646},
    {6406, 2650},
    {6415, 2654},
    {6422, 2658},
    {6431, 2662},
    {6438, 2666},
    {6451, 2670},
    {6463, 2674},
    {6477, 2678},
    {6495, 2682},
    {6513, 2686},
    {6532, 2690},
    {6538, 2694},
    {6544, 2698},
    {6550, 2702},
    {6556, 2706},
    {6562, 2710},
    {6566, 2715},
    {6574, 2722},
    {6579, 2726},
    {6584, 2731},
    {6588, 2735},
    {6595, 2739},
    {6602, 2743},
    {6608, 2747},
    {6614, 2751},
    {6620, 2755},
    {6626, 2759},
    {6631, 2763},
    {6637, 2767},
    {6643, 2771},
    {6649, 2775},
    {6655, 2779},
    {6661, 2783},
    {6667, 2787},
    {6673, 2791},
    {6679, 2795},
    {6684, 2799},
    {6690, 2803},
    {6696, 2807},
    {6702, 2811},
    {6708, 2815},
    {6714, 2819},
    {6720, 2823},
    {6727, 2827},
    {6733, 2831},
    {6739, 2835},
    {6745, 2839},
    {6751, 2843},
    {6756, 2847},
    {6762, 2851},
    {6768, 2855},
    {6774, 2859},
    {6780, 2863},
    {6789, 2867},
    {6797, 2871},
    {6806, 2875},
    {6812, 2879},
    {6818, 2883},
    {6824, 2887},
    {6830, 2891},
    {6835, 2895},
    {6841, 2899},
    {6847, 2903},
    {6853, 2907},
    {6859, 2911},
    {6865, 2915},
    {6871, 2919},
    {6878, 2923},
    {6884, 2926},
    {6891, 2929},
    {6896, 2934},
    {6902, 2938},
    {6907, 2942},
    {6913, 2946},
    {6918, 2948},
    {6924, 2952},
    {6933, 2956},
    {6938, 2960},
    {6945, 2964},
    {6950, 2968},
    {6956, 2972},
    {6962, 2976},
    {6969, 2980},
    {6976, 2983},
    {6980, 2987},
    {6987, 2991},
    {6996, 2995},
    {7003, 2999},
    {7010, 3003},
    {7017, 3007},
    {7022, 3014},
    {7028, 3018},
    {7034, 3021},
    {7040, 3025},
    {7047, 3028},
    {7054, 3031},
    {7060, 3034},
    {7066, 3038},
    {7074, 3042},
    {7079, 3045},
    {7085, 3048},
    {7093, 3052},
    {7098, 3055},
    {7108, 3058},
    {7112, 3063},
    {7117, 3066},
    {7123, 3070},
    {7133, 3074},
    {7137, 3077},
    {7141, 3081},
    {7146, 3085},
    {7151, 3088},
    {7158, 3092},
    {7174, 3096},
    {7191, 3100},
    {7200, 3103},
    {7209, 3107},
    {7220, 3111},
    {7232, 3115},
    {7244, 3119},
    {7249, 3123},
    {7258, 3127},
    {7265, 3131},
    {7273, 3135},
    {7279, 3139},
    {7288, 3143},
    {7294, 3145},
    {7301, 3149},
    {7309, 3153},
    {7315, 3155},
    {7322, 3157},
    {7327, 3161},
    {7334, 3165},
    {7345, 3169},
    {7355, 3173},
    {7360, 3177},
    {7368, 3181},
    {7375, 3185},
    {7380, 3190},
    {7387, 3194},
    {7392, 3197},
    {7399, 3201},
    {7405, 3205},
    {7411, 3209},
    {7416, 3214},
    {7421, 3218},
    {7427, 3222},
    {7432, 3226},
    {7438, 3230},
    {7444, 3234},
    {7452, 3238},
    {7460, 3242},
    {7466, 3246},
    {7472, 3250},
    {7479, 3254},
    {7487, 3258},
    {7491, 3262},
    {7500, 3266},
    {7507, 3270},
    {7514, 3274},
    {7521, 3278},
    {7527, 3282},
    {7532, 3289},
    {7539, 3293},
    {7547, 3297},
    {7559, 3301},
    {7571, 3305},
    {7580, 3309},
    {7591, 3313},
    {7598, 3316},
    {7613, 3320},
    {7629, 3324},
    {7635, 3328},
    {7641, 3332},
    {7650, 3336},
    {7656, 3340},
    {7663, 3344},
    {7668, 3348},
    {7673, 3352},
    {7680, 3356},
    {7687, 3360},
    {7692, 3364},
    {7697, 3368},
    {7703, 3372},
    {7711, 3376},
    {7717, 3379},
    {7724, 3382},
    {7728, 3385},
    {7731, 3389},
    {7739, 3393},
    {7745, 3397},
    {7753, 3401},
    {7757, 3404},
    {7763, 3407},
    {7771, 3411},
    {7778, 3415},
    {7782, 3420},
    {7788, 3424},
    {7794, 3428},
    {7799, 3432},
    {7807, 3436},
    {7819, 3440},
    {7825, 3444},
    {7829, 3447},
    {7837, 3450},
    {7843, 3454},
    {7847, 3457},
    {7854, 3460},
    {7868, 3464},
    {7875, 3468},
    {7880, 3471},
    {7887, 3475},
    {7894, 3479},
    {7901, 3481},
    {7906, 3486},
    {7910, 3489},
    {7916, 3493},
    {7925, 3497},
    {7934, 3501},
    {7942, 3505},
    {7952, 3509},
    {7967, 3513},
    {7977, 3517},
    {7992, 3521},
    {8008, 3525},
    {8025, 3529},
    {8034, 3533},
    {8041, 3537},
    {8048, 3541},
    {8053, 3546},
    {8058, 3549},
    {8063, 3553},
    {8070, 3556},
    {8076, 3560},
    {8081, 3564},
    {8087, 3568},
    {8093, 3572},
    {8099, 3576},
    {8107, 3580},
    {8112, 3583},
    {8121, 3587},
    {8127, 3591},
    {8132, 3595},
    {8139, 3598},
    {8146, 3602},
    {8153, 3605},
    {8158, 3609},
    {8164, 3612},
    {8171, 3616},
    {8175, 3619},
    {8180, 3622},
    {8183, 3626},
    {8189, 3630},
    {8193, 3635},
    {8196, 3639},
    {8203, 3642},
    {8207, 3646},
    {8214, 3650},
    {8217, 3654},
    {8226, 3658},
    {8230, 3662},
    {8234, 3666},
    {8241, 3670},
    {8247, 3673},
    {8253, 3677},
    {8262, 3681},
    {8269, 3685},
    {8274, 3689},
    {8281, 3693},
    {8288, 3697},
    {8292, 3700},
    {8297, 3704},
    {8303, 3707},
    {8308, 3712},
    {8313, 3716},
    {8320, 3720},
    {8326, 3724},
    {8331, 3727},
    {8339, 3730},
    {8345, 3733},
    {8352, 3737},
    {8360, 3741},
    {8366, 3745},
    {8377, 3749},
    {8389, 3753},
    {8396, 3755},
    {8403, 3759},
    {8409, 3763},
    {8417, 3767},
    {8426, 3771},
    {8432, 3775},
    {8438, 3779},
    {8443, 3783},
    {8449, 3787},
    {8454, 3791},
    {8458, 3794},
    {8462, 3797},
    {8467, 3800},
    {8472, 3804},
    {8477, 3806},
    {8483, 3810},
    {8495, 3814},
    {8508, 3818},
    {8522, 3822},
    {8526, 3825},
    {8533, 3829},
    {8540, 3833},
    {8546, 3837},
    {8553, 3841},
    {8557, 3846},
    {8563, 3850},
    {8569, 3853},
    {8574, 3857},
    {8580, 3861},
    {8586, 3865},
    {8591, 3868},
    {8596, 3873},
    {8603, 3877},
    {8608, 3881},
    {8614, 3885},
    {8623, 3889},
    {8630, 3892},
    {8637, 3896},
    {8644, 3899},
    {8651, 3903},
    {8658, 3907},
    {8665, 3911},
    {8672, 3915},
    {8679, 3919},
    {8686, 3922},
    {8693, 3926},
    {8700, 3930},
    {8707, 3934},
    {8714, 3938},
    {8721, 3942},
    {8728, 3946},
    {8734, 3950},
    {8740, 3954},
    {8745, 3959},
    {8748, 3963},
    {8752, 3967},
    {8759, 3970},
    {8765, 3973},
    {8772, 3976},
    {8776, 3980},
    {8783, 3983},
    {8789, 3986},
    {8793, 3989},
    {8798, 3992},
    {8801, 3996},
    {8805, 4000},
    {8809, 4004},
    {8814, 4008},
    {8823, 4012},
    {8827, 4016},
    {8833, 4020},
    {8840, 4024},
    {8848, 4028},
    {8857, 4032},
    {8862, 4039},
    {8869, 4043},
    {8873, 4048},
    {8876, 4052},
    {8880, 4056},
    {8886, 4060},
    {8891, 4063},
    {8894, 4067},
    {8898, 4071},
    {8902, 4075},
    {8906, 4079},
    {8910, 4083},
    {8915, 4087},
    {8924, 4091},
    {8928, 4095},
    {8933, 4099},
    {8939, 4103},
    {8945, 4107},
    {8950, 4112},
    {8956, 4114},
    {8961, 4118},
    {8966, 4122},
    {8972, 4126},
    {8978, 4130},
    {8981, 4132},
    {8986, 4136},
    {8992, 4140},
    {8998, 4144},
    {9005, 4148},
    {9013, 4152},
    {9023, 4156},
    {9030, 4160},
    {9037, 4164},
    {9047, 4168},
    {9058, 4172},
    {9066, 4176},
    {9073, 4180},
    {9083, 4187},
    {9088, 4194},
    {9093, 4198},
    {9100, 4202},
    {9105, 4205},
    {9112, 4209},
    {9119, 4212},
    {9124, 4216},
    {9132, 4220},
    {9138, 4224},
    {9143, 4228},
    {9149, 4231},
    {9156, 4235},
    {9166, 4239},
    {9173, 4243},
    {9180, 4247},
    {9184, 4252},
    {9193, 4256},
    {9202, 4260},
    {9208, 4264},
    {9215, 4268},
    {9229, 4272},
    {9244, 4276},
    {9249, 4281},
    {9256, 4285},
    {9261, 4290},
    {9268, 4294},
    {9275, 4297},
    {9282, 4301},
    {9289, 4305},
    {9296, 4308},
    {9299, 4312},
    {9305, 4315},
    {9309, 4318},
    {9314, 4321},
    {9320, 4324},
    {9324, 4328},
    {9328, 4333},
    {9335, 4336},
    {9338, 4340},
    {9345, 4344},
    {9351, 4348},
    {9358, 4352},
    {9364, 4356},
    {9370, 4359},
    {9376, 4362},
    {9382, 4366},
    {9391, 4370},
    {9400, 4374},
    {9406, 4377},
    {9411, 4381},
    {9417, 4384},
    {9420, 4388},
    {9427, 4392},
    {9433, 4396},
    {9442, 4400},
    {9449, 4403},
    {9453, 4407},
    {9460, 4411},
    {9469, 4415},
    {9478, 4419},
    {9487, 4423},
    {9495, 4427},
    {9500, 4430},
    {9506, 4433},
    {9511, 4438},
    {9516, 4441},
    {9522, 4445},
    {9529, 4448},
    {9534, 4453},
    {9539, 4457},
    {9545, 4461},
    {9553, 4465},
    {9559, 4469},
    {9566, 4473},
    {9572, 4477},
    {9575, 4481},
    {9582, 4484},
    {9588, 4487},
    {9593, 4490},
    {9599, 4493},
    {9603, 4496},
    {9607, 4501},
    {9613, 4504},
    {9618, 4509},
    {9623, 4514},
    {9630, 4517},
    {9636, 4520},
    {9642, 4523},
    {9649, 4526},
    {9656, 4529},
    {9660, 4532},
    {9664, 4537},
    {9671, 4540},
    {9676, 4543},
    {9681, 4546},
    {9686, 4551},
    {9691, 4556},
    {9697, 4560},
    {9702, 4564},
    {9709, 4568},
    {9715, 4572},
    {9718, 4576},
    {9722, 4580},
    {9727, 4584},
    {9734, 4587},
    {9743, 4591},
    {9750, 4595},
    {9757, 4598},
    {9762, 4602},
    {9768, 4606},
    {9775, 4610},
    {9779, 4614},
    {9785, 4617},
    {9790, 4621},
    {9796, 4625},
    {9804, 4629},
    {9811, 4633},
    {9818, 4637},
    {9825, 4641},
    {9832, 4645},
    {9840, 4649},
    {9847, 4653},
    {9851, 4657},
    {9858, 4661},
    {9863, 4665},
    {9869, 4672},
    {9875, 4676},
    {9881, 4680},
    {9888, 4682},
    {9895, 4684},
    {9901, 4688},
    {9909, 4692},
    {9917, 4696},
    {9924, 4699},
    {9931, 4702},
    {9937, 4706},
    {9942, 4708},
    {9946, 4711},
    {9951, 4715},
    {9957, 4719},
    {9964, 4723},
    {9972, 4727},
    {9981, 4731},
    {9986, 4735},
    {9989, 4739},
    {9999, 4743},
    {10013, 4747},
    {10029, 4751},
    {10043, 4755},
    {10058, 4759},
    {10073, 4763},
    {10089, 4767},
    {10107, 4771},
    {10127, 4775},
    {10142, 4779},
    {10146, 4783},
    {10150, 4787},
    {10155, 4791},
    {10164, 4795},
    {10168, 4799},
    {10174, 4803},
    {10181, 4807},
    {10189, 4811},
    {10198, 4815},
    {10203, 4822},
    {10210, 4826},
    {10221, 4830},
    {10229, 4834},
    {10239, 4838},
    {10250, 4842},
    {10258, 4846},
    {10266, 4850},
    {10273, 4854},
    {10280, 4858},
    {10284, 4863},
    {10287, 4867},
    {10291, 4871},
    {10297, 4875},
    {10303, 4879},
    {10310, 4883},
    {10316, 4887},
    {10321, 4890},
    {10324, 4894},
    {10330, 4898},
    {10339, 4902},
    {10346, 4906},
    {10352, 4910},
    {10359, 4913},
    {10366, 4917},
    {10377, 4921},
    {10381, 4925},
    {10386, 4929},
    {10395, 4933},
    {10399, 4937},
    {10404, 4941},
    {10410, 4945},
    {10416, 4949},
    {10422, 4953},
    {10428, 4957},
    {10434, 4961},
    {10448, 4965},
    {10467, 4969},
    {10478, 4973},
    {10493, 4977},
    {10507, 4981},
    {10522, 4985},
    {10528, 4989},
    {10533, 4994},
    {10540, 4998},
    {10548, 5002},
    {10555, 5006},
    {10562, 5008},
    {10566, 5012},
    {10574, 5016},
    {10579, 5020},
    {10584, 5022},
    {10591, 5026},
    {10597, 5030},
    {10606, 5034},
    {10612, 5038},
    {10619, 5042},
    {10623, 5046},
    {10629, 5050},
    {10636, 5054},
    {10641, 5059},
    {10645, 5063},
    {10650, 5067},
    {10656, 5071},
    {10662, 5075},
    {10667, 5077},
    {10673, 5081},
    {10680, 5085},
    {10687, 5088},
    {10690, 5090},
    {10695, 5094},
    {10701, 5098},
    {10707, 5102},
    {10714, 5106},
    {10721, 5110},
    {10728, 5114},
    {10736, 5118},
    {10743, 5122},
    {10748, 5126},
    {10754, 5130},
    {10760, 5134},
    {10769, 5138},
    {10777, 5142},
    {10787, 5149},
    {10792, 5156},
    {10798, 5160},
    {10803, 5163},
    {10808, 5167},
    {10813, 5171},
    {10821, 5175},
    {10825, 5179},
    {10832, 5183},
    {10843, 5187},
    {10854, 5191},
    {10863, 5195},
    {10870, 5199},
    {10877, 5203},
    {10881, 5206},
    {10887, 5210},
    {10901, 5214},
    {10905, 5219},
    {10909, 5223},
    {10915, 5226},
    {10919, 5230},
    {10926, 5232},
    {10933, 5236},
    {10940, 5239},
    {10946, 5243},
    {10953, 5247},
    {10960, 5251},
    {10968, 5255},
    {10973, 5259},
    {10978, 5263},
    {10985, 5267},
    {10992, 5271},
    {10997, 5276},
    {11000, 5280},
    {11005, 5285},
    {11012, 5289},
    {11015, 5292},
    {11024, 5296},
    {11030, 5300},
    {11034, 5306},
    {11038, 5313},
    {11043, 5319},
    {11054, 5323},
    {11070, 5327},
    {11074, 5333},
    {11078, 5340},
    {11083, 5346},
    {11095, 5350},
    {11102, 5354},
    {11109, 5358},
    {11115, 5362},
    {11122, 5365},
    {11127, 5372},
    {11131, 5376},
    {11136, 5382},
    {11142, 5388},
    {11148, 5391},
    {11156, 5395},
    {11162, 5399},
    {11170, 5403},
    {11179, 5407},
    {11184, 5410},
    {11190, 5416},
    {11197, 5422},
    {11202, 5426},
    {11209, 5429},
    {11216, 5432},
    {11222, 5436},
    {11231, 5442},
    {11236, 5446},
    {11240, 5449},
    {11246, 5453},
    {11249, 5457},
    {11255, 5461},
    {11262, 5465},
    {11268, 5469},
    {11276, 5473},
    {11282, 5479},
    {11289, 5483},
    {11296, 5487},
    {11302, 5493},
    {11309, 5497},
    {11317, 5501},
    {11321, 5506},
    {11325, 5512},
    {11329, 5516},
    {11334, 5520},
    {11340, 5526},
    {11350, 5532},
    {11355, 5538},
    {11361, 5542},
    {11365, 5546},
    {11370, 5550},
    {11376, 5554},
    {11382, 5558},
    {11388, 5562},
    {11391, 5566},
    {11395, 5570},
    {11400, 5574},
    {11404, 5578},
    {11409, 5581},
    {11415, 5585},
    {11419, 5591},
    {11425, 5595},
    {11430, 5599},
    {11434, 5603},
    {11445, 5607},
    {11461, 5611},
    {11466, 5615},
    {11472, 5621},
    {11482, 5627},
    {11487, 5633},
    {11493, 5637},
    {11499, 5641},
    {11503, 5645},
    {11509, 5649},
    {11516, 5653},
    {11521, 5657},
    {11526, 5662},
    {11530, 5665},
    {11536, 5669},
    {11543, 5675},
    {11552, 5681},
    {11560, 5685},
    {11568, 5689},
    {11576, 5693},
    {11582, 5697},
    {11590, 5701},
    {11598, 5705},
    {11606, 5709},
    {11611, 5713},
    {11621, 5717},
    {11628, 5724},
    {11634, 5730},
    {11642, 5734},
    {11646, 5738},
    {11653, 5742},
    {11658, 5748},
    {11664, 5752},
    {11672, 5758},
    {11678, 5762},
    {11684, 5766},
    {11691, 5772},
    {11698, 5778},
    {11710, 5782},
    {11716, 5786},
    {11723, 5790},
    {11727, 5794},
    {11734, 5798},
    {11739, 5804},
    {11744, 5809},
    {11754, 5813},
    {11769, 5817},
    {11774, 5821},
    {11780, 5825},
    {11787, 5829},
    {11793, 5833},
    {11799, 5837},
    {11807, 5841},
    {11815, 5845},
    {11820, 5849},
    {11826, 5855},
    {11832, 5859},
    {11840, 5866},
    {11850, 5870},
    {11861, 5876},
    {11867, 5880},
    {11875, 5886},
    {11880, 5890},
    {11886, 5896},
    {11892, 5900},
    {11900, 5907},
    {11910, 5911},
    {11921, 5917},
    {11926, 5921},
    {11933, 5924},
    {11938, 5928},
    {11952, 5932},
    {11968, 5936},
    {11983, 5940},
    {12000, 5944},
    {12003, 5947},
    {12007, 5949},
    {12014, 5953},
    {12020, 5957},
    {12027, 5961},
    {12034, 5965},
    {12039, 5972},
    {12046, 5976},
    {12051, 5983},
    {12056, 5988},
    {12064, 5992},
    {12071, 5996},
    {12076, 6003},
    {12081, 6008},
    {12089, 6015},
    {12096, 6019},
    {12104, 6026},
    {12110, 6033},
    {12116, 6037},
    {12123, 6041},
    {12129, 6045},
    {12137, 6049},
    {12144, 6053},
    {12147, 6057},
    {12154, 6060},
    {12159, 6064},
    {12164, 6068},
    {12170, 6071},
    {12174, 6074},
    {12180, 6078},
    {12187, 6081},
    {12192, 6085},
    {12197, 6089},
    {12204, 6093},
    {12210, 6096},
    {12216, 6100},
    {12220, 6105},
    {12225, 6108},
    {12232, 6111},
    {12236, 6115},
    {12242, 6119},
    {12246, 6122},
    {12251, 6126},
    {12257, 6130},
    {12263, 6134},
    {12271, 6138},
    {12277, 6142},
    {12281, 6146},
    {12287, 6149},
    {12293, 6152},
    {12301, 6155},
    {12306, 6159},
    {12313, 6163},
    {12318, 6168},
    {12323, 6172},
    {12329, 6176},
    {12335, 6180},
    {12338, 6184},
    {12344, 6188},
    {12348, 6192},
    {12354, 6196},
    {12362, 6200},
    {12367, 6203},
    {12372, 6206},
    {12379, 6210},
    {12384, 6214},
    {12392, 6218},
    {12396, 6222},
    {12401, 6226},
    {12408, 6229},
    {12413, 6233},
    {12420, 6236},
    {12427, 6240},
    {12436, 6244},
    {12441, 6247},
    {12447, 6251},
    {12451, 6255},
    {12456, 6258},
    {12465, 6262},
    {12472, 6266},
    {12478, 6270},
    {12483, 6274},
    {12487, 6277},
    {12494, 6279},
    {12501, 6281},
    {12508, 6285},
    {12513, 6289},
    {12521, 6293},
    {12525, 6298},
    {12529, 6301},
    {12534, 6304},
    {12541, 6308},
    {12547, 6312},
    {12550, 6315},
    {12560, 6319},
    {12564, 6322},
    {12571, 6326},
    {12579, 6330},
    {12586, 6334},
    {12591, 6336},
    {12600, 6340},
    {12606, 6344},
    {12614, 6348},
    {12621, 6352},
    {12628, 6356},
    {12634, 6360},
    {12641, 6363},
    {12649, 6367},
    {12657, 6371},
    {12660, 6374},
    {12669, 6378},
    {12674, 6383},
    {12680, 6386},
    {12683, 6390},
    {12687, 6394},
    {12692, 6398},
    {12698, 6402},
    {12702, 6406},
    {12707, 6410},
    {12718, 6414},
    {12730, 6418},
    {12737, 6422},
    {12749, 6426},
    {12758, 6430},
    {12767, 6434},
    {12775, 6438},
    {12781, 6442},
    {12788, 6446},
    {12793, 6450},
    {12799, 6454},
    {12806, 6458},
    {12811, 6462},
    {12820, 6466},
    {12829, 6470},
    {12838, 6474},
    {12843, 6478},
    {12850, 6482},
    {12856, 6486},
    {12863, 6490},
    {12868, 6495},
    {12872, 6498},
    {12879, 6502},
    {12883, 6507},
    {12888, 6511},
    {12893, 6516},
    {12900, 6520},
    {12905, 6525},
    {12917, 6529},
    {12925, 6533},
    {12931, 6535},
    {12939, 6539},
    {12944, 6541},
    {12950, 6545},
    {12955, 6549},
    {12962, 6553},
    {12968, 6557},
    {12973, 6561},
    {12978, 6567},
    {12985, 6570},
    {12991, 6574},
    {13000, 6578},
    {13005, 6582},
    {13011, 6586},
    {13017, 6590},
    {13024, 6594},
    {13030, 6597},
    {13035, 6601},
    {13042, 6605},
    {13048, 6609},
    {13056, 6613},
    {13062, 6617},
    {13069, 6621},
    {13076, 6625},
    {13083, 6629},
    {13090, 6633},
    {13098, 6637},
    {13105, 6641},
    {13111, 6645},
    {13118, 6649},
    {13124, 6653},
    {13134, 6657},
    {13140, 6661},
    {13146, 6665},
    {13153, 6667},
    {13160, 6669},
    {13166, 6673},
    {13174, 6677},
    {13182, 6681},
    {13189, 6684},
    {13196, 6687},
    {13202, 6691},
    {13207, 6693},
    {13211, 6696},
    {13216, 6700},
    {13224, 6704},
    {13230, 6708},
    {13237, 6712},
    {13242, 6716},
    {13247, 6720},
    {13255, 6724},
    {13264, 6728},
    {13270, 6732},
    {13275, 6736},
    {13279, 6739},
    {13286, 6743},
    {13293, 6747},
    {13297, 6752},
    {13303, 6756},
    {13309, 6760},
    {13316, 6764},
    {13320, 6767},
    {13325, 6770},
    {13336, 6774},
    {13351, 6778},
    {13368, 6782},
    {13383, 6786},
    {13399, 6790},
    {13417, 6794},
    {13434, 6798},
    {13450, 6802},
    {13466, 6806},
    {13471, 6809},
    {13484, 6813},
    {13490, 6817},
    {13496, 6821},
    {13500, 6825},
    {13507, 6829},
    {13518, 6833},
    {13524, 6837},
    {13530, 6841},
    {13536, 6845},
    {13542, 6849},
    {13548, 6853},
    {13553, 6858},
    {13560, 6862},
    {13568, 6866},
    {13573, 6868},
    {13580, 6872},
    {13589, 6876},
    {13595, 6880},
    {13602, 6884},
    {13607, 6889},
    {13611, 6893},
    {13616, 6895},
    {13622, 6899},
    {13629, 6903},
    {13636, 6907},
    {13643, 6911},
    {13648, 6915},
    {13654, 6919},
    {13660, 6923},
    {13669, 6927},
    {13677, 6931},
    {13680, 6935},
    {13687, 6938},
    {13693, 6942},
    {13696, 6946},
    {13700, 6950},
    {13705, 6954},
    {13712, 6957},
    {13718, 6961},
    {13722, 6965},
    {13729, 6968},
    {13735, 6971},
    {13740, 6975},
    {13746, 6979},
    {13753, 6983},
    {13762, 6987},
    {13768, 6991},
    {13772, 6994},
    {13777, 6998},
    {13783, 7002},
    {13789, 7006},
    {13795, 7010},
    {13802, 7014},
    {13808, 7018},
    {13816, 7022},
    {13821, 7025},
    {13826, 7027},
    {13833, 7031},
    {13842, 7035},
    {13848, 7039},
    {13853, 7043},
    {13857, 7048},
    {13864, 7052},
    {13870, 7056},
    {13877, 7059},
    {13882, 7062},
    {13891, 7066},
    {13905, 7070},
    {13909, 7073},
    {13915, 7076},
    {13922, 7079},
    {13929, 7082},
    {13933, 7086},
    {13940, 7090},
    {13945, 7094},
    {13951, 7098},
    {13956, 7102},
    {13962, 7106},
    {13967, 7110},
    {13973, 7114},
    {13979, 7118},
    {13987, 7122},
    {13995, 7126},
    {14001, 7130},
    {14015, 7134},
    {14022, 7138},
    {14031, 7142},
    {14036, 7146},
    {14042, 7150},
    {14046, 7154},
    {14051, 7158},
    {14057, 7165},
    {14064, 7168},
    {14068, 7170},
    {14073, 7174},
    {14080, 7178},
    {14085, 7183},
    {14092, 7187},
    {14102, 7191},
    {14107, 7195},
    {14113, 7199},
    {14120, 7206},
    {14126, 7210},
    {14133, 7217},
    {14139, 7221},
    {14146, 7225},
    {14155, 7229},
    {14166, 7233},
    {14172, 7237},
    {14179, 7241},
    {14188, 7245},
    {14199, 7249},
    {14203, 7253},
    {14210, 7257},
    {14217, 7261},
    {14222, 7265},
    {14228, 7269},
    {14233, 7274},
    {14240, 7278},
    {14247, 7282},
    {14254, 7286},
    {14259, 7290},
    {14265, 7294},
    {14281, 7297},
    {14293, 7300},
    {14299, 7303},
    {14303, 7307},
    {14308, 7311},
    {14315, 7315},
    {14320, 7319},
    {14328, 7323},
    {14336, 7327},
    {14342, 7331},
    {14348, 7335},
    {14356, 7339},
    {14364, 7343},
    {14371, 7347},
    {14380, 7351},
    {14390, 7355},
    {14400, 7359},
    {14411, 7363},
    {14418, 7367},
    {14425, 7371},
    {14432, 7375},
    {14437, 7379},
    {14448, 7383},
    {14460, 7387},
    {14467, 7391},
    {14479, 7395},
    {14488, 7399},
    {14497, 7403},
    {14505, 7407},
    {14509, 7411},
    {14514, 7415},
    {14518, 7419},
    {14523, 7422},
    {14528, 7425},
    {14533, 7428},
    {14538, 7432},
    {14545, 7436},
    {14553, 7440},
    {14558, 7444},
    {14566, 7448},
    {14574, 7452},
    {14582, 7456},
    {14590, 7460},
    {14598, 7464},
    {14604, 7468},
    {14610, 7472},
    {14618, 7476},
    {14625, 7480},
    {14634, 7484},
    {14644, 7488},
    {14654, 7492},
    {14665, 7496},
    {14672, 7500},
    {14679, 7504},
    {14686, 7508},
    {14692, 7512},
    {14699, 7516},
    {14705, 7520},
    {14713, 7524},
    {14720, 7528},
    {14726, 7531},
    {14733, 7535},
    {14737, 7538},
    {14742, 7542},
    {14749, 7545},
    {14756, 7548},
    {14760, 7551},
    {14765, 7555},
    {14772, 7559},
    {14776, 7564},
    {14783, 7568},
    {14793, 7572},
    {14799, 7575},
    {14808, 7578},
    {14815, 7581},
    {14827, 7585},
    {14836, 7589},
    {14843, 7593},
    {14849, 7597},
    {14856, 7601},
    {14862, 7604},
    {14868, 7607},
    {14874, 7610},
    {14881, 7614},
    {14890, 7618},
    {14897, 7622},
    {14902, 7626},
    {14907, 7630},
    {14911, 7634},
    {14918, 7638},
    {14925, 7642},
    {14930, 7647},
    {14938, 7651},
    {14943, 7655},
    {14950, 7659},
    {14956, 7663},
    {14965, 7667},
    {14978, 7671},
    {14991, 7675},
    {15006, 7679},
    {15016, 7683},
    {15030, 7687},
    {15046, 7691},
    {15053, 7695},
    {15058, 7699},
    {15067, 7703},
    {15075, 7707},
    {15081, 7711},
    {15089, 7715},
    {15098, 7719},
    {15103, 7724},
    {15108, 7727},
    {15114, 7730},
    {15121, 7733},
    {15127, 7737},
    {15144, 7741},
    {15162, 7745},
    {15167, 7749},
    {15172, 7753},
    {15179, 7756},
    {15184, 7760},
    {15190, 7763},
    {15197, 7766},
    {15203, 7769},
    {15207, 7772},
    {15213, 7776},
    {15220, 7779},
    {15226, 7783},
    {15233, 7787},
    {15237, 7792},
    {15244, 7795},
    {15250, 7799},
    {15256, 7803},
    {15262, 7807},
    {15269, 7811},
    {15278, 7815},
    {15285, 7819},
    {15291, 7823},
    {15297, 7826},
    {15301, 7829},
    {15307, 7832},
    {15312, 7837},
    {15320, 7841},
    {15332, 7845},
    {15346, 7849},
    {15361, 7853},
    {15367, 7857},
    {15372, 7860},
    {15378, 7863},
    {15386, 7866},
    {15397, 7870},
    {15404, 7874},
    {15413, 7878},
    {15420, 7882},
    {15426, 7885},
    {15432, 7889},
    {15437, 7894},
    {15443, 7898},
    {15450, 7901},
    {15455, 7905},
    {15461, 7909},
    {15467, 7913},
    {15472, 7916},
    {15480, 7920},
    {15485, 7924},
    {15490, 7928},
    {15496, 7932},
    {15502, 7936},
    {15509, 7940},
    {15520, 7943},
    {15529, 7946},
    {15540, 7950},
    {15547, 7953},
    {15553, 7956},
    {15563, 7960},
    {15568, 7964},
    {15575, 7967},
    {15584, 7970},
    {15597, 7977},
    {15611, 7984},
    {15624, 7991},
    {15638, 7998},
    {15647, 8001},
    {15663, 8005},
    {15680, 8009},
    {15684, 8012},
    {15690, 8016},
    {15694, 8020},
    {15701, 8024},
    {15707, 8028},
    {15714, 8032},
    {15721, 8034},
    {15726, 8036},
    {15730, 8041},
    {15736, 8045},
    {15742, 8052},
    {15748, 8059},
    {15753, 8064},
    {15759, 8068},
    {15765, 8072},
    {15770, 8077},
    {15777, 8084},
    {15784, 8091},
    {15791, 8098},
    {15798, 8105},
    {15806, 8109},
    {15812, 8112},
    {15819, 8116},
    {15825, 8120},
    {15832, 8124},
    {15839, 8128},
    {15843, 8133},
    {15848, 8138},
    {15851, 8142},
    {15854, 8146},
    {15861, 8150},
    {15866, 8155},
    {15871, 8159},
    {15877, 8163},
    {15882, 8167},
    {15888, 8171},
    {15892, 8176},
    {15898, 8180},
    {15904, 8184},
    {15907, 8187},
    {15913, 8191},
    {15919, 8195},
    {15924, 8199},
    {15929, 8203},
    {15935, 8207},
    {15940, 8212},
    {15947, 8216},
    {15954, 8220},
    {15960, 8224},
    {15966, 8228},
    {15971, 8233},
    {15978, 8237},
    {15985, 8241},
    {15991, 8245},
    {15996, 8249},
    {16003, 8253},
    {16010, 8256},
    {16015, 8259},
    {16021, 8262},
    {16025, 8265},
    {16029, 8268},
    {16033, 8273},
    {16038, 8276},
    {16043, 8281},
    {16048, 8286},
    {16053, 8289},
    {16058, 8292},
    {16065, 8295},
    {16072, 8298},
    {16076, 8301},
    {16081, 8304},
    {16088, 8308},
    {16093, 8311},
    {16097, 8316},
    {16102, 8319},
    {16110, 8323},
    {16115, 8328},
    {16120, 8333},
    {16124, 8337},
};

Str HtmlNamedCharRef(Str name) {
    int lo = 0;
    int hi = (int)dimof(kHtmlEntities) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char* candidate = kHtmlEntityNames + kHtmlEntities[mid].nameOff;
        int cmp = StrCmp(Str(candidate), name);
        if (cmp < 0) {
            lo = mid + 1;
        } else if (cmp > 0) {
            hi = mid - 1;
        } else {
            return Str(kHtmlEntityValues + kHtmlEntities[mid].valueOff);
        }
    }
    return {};
}

int HtmlNamedCharRefCount() {
    return (int)dimof(kHtmlEntities);
}

void HtmlNamedCharRefAt(int i, Str* name, Str* value) {
    *name = Str(kHtmlEntityNames + kHtmlEntities[i].nameOff);
    *value = Str(kHtmlEntityValues + kHtmlEntities[i].valueOff);
}

}

#line 1 "src/wry/wry_unsupported.cpp"

#if GPUI_WRY_UNSUPPORTED

namespace wry {

using base::logf;
using base::Str;

WebView* WebViewNew(void*, const WebViewAttributes*, bool) {
    WebViewSetLastError(GPUI_WRY_UNSUPPORTED_REASON);
    static bool said = false;
    if (!said) {
        said = true;
        logf("wry: " GPUI_WRY_UNSUPPORTED_REASON "\n");
    }
    return nullptr;
}
void WebViewFree(WebView*) {}
Str WebViewId(WebView*) {
    return {};
}
bool WebViewEval(WebView*, Str) {
    return false;
}
bool WebViewEvalWithCallback(WebView*, Str, void*, void (*)(void*, Str)) {
    return false;
}
Str WebViewUrlTemp(WebView*) {
    return {};
}
bool WebViewLoadUrl(WebView*, Str) {
    return false;
}
bool WebViewLoadUrlWithHeaders(WebView*, Str, const Header*, int) {
    return false;
}
bool WebViewLoadHtml(WebView*, Str) {
    return false;
}
bool WebViewReload(WebView*) {
    return false;
}
bool WebViewBounds(WebView*, Rect*) {
    return false;
}
bool WebViewSetBounds(WebView*, Rect) {
    return false;
}
bool WebViewSetVisible(WebView*, bool) {
    return false;
}
bool WebViewFocus(WebView*) {
    return false;
}
bool WebViewFocusParent(WebView*) {
    return false;
}
bool WebViewZoom(WebView*, double) {
    return false;
}
bool WebViewSetUserZoom(WebView*, double) {
    return false;
}
bool WebViewMatchPageScale(WebView*, float) {
    return false;
}
void WebViewSetPageClick(WebView*, WebViewPageClick, void*) {}
bool WebViewSetBackgroundColor(WebView*, Rgba) {
    return false;
}
bool WebViewSetTheme(WebView*, Theme) {
    return false;
}
bool WebViewSetMemoryUsageLevel(WebView*, MemoryUsageLevel) {
    return false;
}
bool WebViewReparent(WebView*, void*) {
    return false;
}
bool WebViewSetTrafficLightInset(WebView*, Position) {
    return false;
}
bool WebViewPrint(WebView*) {
    return false;
}
bool WebViewClearAllBrowsingData(WebView*) {
    return false;
}
bool WebViewCookies(WebView*, Vec<Cookie>* out) {
    CookieListFree(out);
    return false;
}
bool WebViewCookiesForUrl(WebView*, Str, Vec<Cookie>* out) {
    CookieListFree(out);
    return false;
}
bool WebViewSetCookie(WebView*, const Cookie*) {
    return false;
}
bool WebViewDeleteCookie(WebView*, const Cookie*) {
    return false;
}
void WebViewOpenDevtools(WebView*) {}
void WebViewCloseDevtools(WebView*) {}
bool WebViewIsDevtoolsOpen(WebView*) {
    return false;
}
void Respond(RequestResponder*, const Response*) {}

Str WebViewVersionTemp() {
    return {};
}
bool WebViewAvailable() {
    return false;
}

}

#endif

#line 1 "src/wry/wry.cpp"

namespace wry {

bool MacCookieMatchesUrl(const Cookie* cookie, Str scheme, Str domain) {
    if (!cookie || !domain.s || !base::StrEq(cookie->domain, domain))
        return false;
    if (!cookie->hasSecure || !cookie->secure) return true;
    return base::StrEq(scheme, StrL("https")) ||
           (base::StrEq(scheme, StrL("http")) &&
            base::StrEq(domain, StrL("localhost")));
}

Str MacDownloadFileNameTemp(Str suggested, int collision) {
    if (collision <= 0) return suggested;
    int dot = base::StrFind(suggested, ".");
    Str stem = dot < 0 ? suggested : Str(suggested.s, dot);
    Str extension =
        dot < 0 ? Str() : Str(suggested.s + dot, len(suggested) - dot);
    return base::FormatTemp("%s (%d)%s", stem, collision, extension);
}

void CookieListFree(Vec<Cookie>* cookies) {
    if (!cookies) {
        return;
    }
    for (int i = 0; i < cookies->len; i++) {
        Cookie& cookie = cookies->els[i];
        base::StrFree(cookie.name);
        base::StrFree(cookie.value);
        base::StrFree(cookie.domain);
        base::StrFree(cookie.path);
    }
    VecReset(*cookies);
}

Str WorkAroundUriPrefix(Str httpOrHttps, Str protocol) {
    return base::FormatTemp("%s://%s.", httpOrHttps, protocol);
}

bool IsWorkAroundUri(Str uri, Str httpOrHttps, Str protocol) {
    return base::StrStartsWith(uri, WorkAroundUriPrefix(httpOrHttps, protocol));
}

Str ApplyUriWorkAround(Str uri, Str httpOrHttps, Str protocol) {
    return base::StrReplaceAll(uri, base::FormatTemp("%s://", protocol),
                               WorkAroundUriPrefix(httpOrHttps, protocol));
}

Str RevertUriWorkAround(Str uri, Str httpOrHttps, Str protocol) {
    return base::StrReplaceAll(uri, WorkAroundUriPrefix(httpOrHttps, protocol),
                               base::FormatTemp("%s://", protocol));
}

Str ProxyUriTemp(const ProxyConfig* proxy) {
    if (!proxy || proxy->kind == ProxyKind::None) {
        return {};
    }
    const char* scheme = proxy->kind == ProxyKind::Socks5 ? "socks5" : "http";
    return base::FormatTemp("%s://%s:%s", Str(scheme), proxy->host,
                            proxy->port);
}

Str PathFromFileUriTemp(Str uri) {
    Str path = uri;
    if (base::StrStartsWith(path, "file://")) {
        path = Str(path.s + 7, len(path) - 7);
    }
    Str res = base::AllocStrTemp(len(path) + 1);
    if (!res.s) {
        return {};
    }
    int n = base::PercentDecode(path, res.s);
    res.s[n] = 0;
    res.len = n;
    return res;
}

void DownloadFileNameParts(Str uri, Str suggested, Str* stem, Str* ext) {

    Str name = suggested;
    Str dotExt;
    for (int i = 0; i < len(suggested); i++) {
        if (suggested.s[i] == '.') {
            name = Str(suggested.s, i);
            dotExt = Str(suggested.s + i, len(suggested) - i);
            break;
        }
    }

    if (base::StrStartsWith(uri, "data:")) {
        int slash = -1;
        for (int i = 0; i < len(uri); i++) {
            if (uri.s[i] == '/') {
                slash = i;
                break;
            }
        }
        if (slash >= 0) {
            Str rest = Str(uri.s + slash + 1, len(uri) - slash - 1);
            for (int i = 0; i < len(rest); i++) {
                if (rest.s[i] == ',') {
                    Str prefix = Str(rest.s, i + 1);
                    if (base::StrStartsWith(name, prefix)) {
                        name = StrL("Unknown");
                    }
                    break;
                }
            }
        }
    }
    *stem = base::StrDupTemp(name);
    *ext = base::StrDupTemp(dotExt);
}

double ScaleFactorFromScreen(int widthPx, int widthMm) {
    if (widthPx <= 0 || widthMm <= 0) {
        return 1.0;
    }
    return ((double)widthPx * 25.4 / (double)widthMm) / 96.0;
}

Str SyntheticMouseEventJsTemp(const SyntheticMouseEvent* ev) {
    base::StrBuilder b(base::GetTempArena());
    Str name = ev->pressed ? StrL("mousedown") : StrL("mouseup");
    Str x = base::FormatTemp("%d", ev->x);
    Str y = base::FormatTemp("%d", ev->y);
    auto boolStr = [](bool v) { return v ? StrL("true") : StrL("false"); };
    auto add = [&b](Str s) { b.Append(s); };
    add(StrL("(() => {\n        const el = document.elementFromPoint("));
    add(x);
    add(StrL(","));
    add(y);
    add(StrL(");\n        const ev = new MouseEvent('"));
    add(name);
    add(StrL("', {\n          view: window,\n          button: "));
    add(base::FormatTemp("%d", ev->button == 8 ? 3 : 4));
    add(StrL(",\n          buttons: "));
    add(base::FormatTemp("%d", ev->buttons));
    add(StrL(",\n          x: "));
    add(x);
    add(StrL(",\n          y: "));
    add(y);
    add(StrL(",\n          bubbles: true,\n          detail: "));
    add(base::FormatTemp("%d", ev->detail));
    add(
        StrL(",\n          cancelBubble: false,\n          cancelable: true,\n"
             "          clientX: "));
    add(x);
    add(StrL(",\n          clientY: "));
    add(y);
    add(StrL(",\n          composed: true,\n          layerX: "));
    add(x);
    add(StrL(",\n          layerY: "));
    add(y);
    add(StrL(",\n          pageX: "));
    add(x);
    add(StrL(",\n          pageY: "));
    add(y);
    add(StrL(",\n          screenX: window.screenX + "));
    add(x);
    add(StrL(",\n          screenY: window.screenY + "));
    add(y);
    add(StrL(",\n          ctrlKey: "));
    add(boolStr(ev->ctrlKey));
    add(StrL(",\n          metaKey: "));
    add(boolStr(ev->metaKey));
    add(StrL(",\n          shiftKey: "));
    add(boolStr(ev->shiftKey));
    add(StrL(",\n          altKey: "));
    add(boolStr(ev->altKey));
    add(
        StrL(",\n        });\n        el.dispatchEvent(ev)\n"
             "        if (!ev.defaultPrevented && \""));
    add(name);
    add(
        StrL("\" === \"mouseup\") {\n          if (ev.button === 3) {\n"
             "            window.history.back();\n          }\n"
             "          if (ev.button === 4) {\n"
             "            window.history.forward();\n          }\n"
             "        }\n      })()"));
    return b.TakeStr();
}

static const char* gWebViewLastError = nullptr;

Str WebViewLastError() {
    return gWebViewLastError ? Str(gWebViewLastError) : Str();
}

void WebViewSetLastError(const char* message) {
    gWebViewLastError = message;
}

int CustomProtocolsAppendCopy(Vec<CustomProtocol>* protocols,
                              const CustomProtocol& protocol) {
    CustomProtocol copy = protocol;
    copy.name = StrDup(protocol.name);
    int index = len(*protocols);
    VecAppend(*protocols, copy);
    return index;
}

void CustomProtocolsFree(Vec<CustomProtocol>* protocols) {
    for (int i = 0; i < len(*protocols); i++) {
        StrFree((*protocols)[i].name);
    }
    VecReset(*protocols);
}

void CustomProtocolDispatch(const CustomProtocol& protocol, Str id,
                            const Request* request,
                            RequestResponder* responder) {
    if (protocol.handler) {
        protocol.handler(protocol.ctx, id, request, responder);
        return;
    }
    Response response;
    response.status = 500;
    Respond(responder, &response);
}

}

#if GPUI_OS_ANDROID
#line 1 "src/base_android.cpp"

namespace base {

const PlatFacts kPlatFacts = {
    .scrollBounce = true,

    .cleanOnEscape = false,
    .scrollBounceTouch = true,
    .exePath = PlatProcSelfExePath,
};

}

#endif

#if GPUI_OS_IOS
#line 1 "src/base_ios.cpp"

namespace base {

const PlatFacts kPlatFacts = {
    .scrollBounce = true,
    .scrollBounceTouch = true,
};

}

#endif

#if GPUI_OS_LINUX
#line 1 "src/base_linux.cpp"

#include <stdio.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace base {

void PlatGetExeDir(char* out, int cap) {
    PlatExeDirFromExePath(out, cap);
}

bool PlatSelfUsage(uint64_t* cpu100ns, uint64_t* memBytes) {

    struct rusage ru = {};
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        return false;
    }
    if (cpu100ns) {
        uint64_t us = (uint64_t)ru.ru_utime.tv_sec * 1000000ull +
                      (uint64_t)ru.ru_utime.tv_usec +
                      (uint64_t)ru.ru_stime.tv_sec * 1000000ull +
                      (uint64_t)ru.ru_stime.tv_usec;
        *cpu100ns = us * 10ull;
    }
    if (memBytes) {
        *memBytes = 0;
        FILE* f = fopen("/proc/self/statm", "rb");
        if (f) {
            unsigned long total = 0, resident = 0;
            if (fscanf(f, "%lu %lu", &total, &resident) == 2) {
                long page = sysconf(_SC_PAGESIZE);
                *memBytes =
                    (uint64_t)resident * (uint64_t)(page > 0 ? page : 4096);
            }
            fclose(f);
        }
    }
    return true;
}

const PlatFacts kPlatFacts = {

    .titleBarCloseIsElement = true,
    .blockSelectUsesControl = true,

    .windowShadowSize = 20.f,
    .exePath = PlatProcSelfExePath,
};

}

#endif

#if GPUI_OS_MAC
#line 1 "src/base_mac.cpp"

#include <errno.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern "C" int responsibility_spawnattrs_setdisclaim(posix_spawnattr_t* attrs,
                                                     int disclaim);

extern "C" char** environ;

namespace base {

static bool ExePath(char* out, int cap) {
    uint32_t n = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &n) != 0) return false;
    out[cap - 1] = 0;
    return out[0] != 0;
}

static bool DisclaimSpeechResponsibility(int argc, char** argv, int* status) {
    if (getenv("SPEECH_RESPONSIBILITY_DISCLAIMED")) {
        return true;
    }
    char path[kMaxPath];
    if (!PlatGetExePath(path, kMaxPath)) {
        fprintf(stderr,
                "Speech could not disclaim the terminal's responsibility: "
                "current executable path is unknown\n");
        return true;
    }
    if (argc < 0) {
        argc = 0;
    }
    char** childArgv = new char*[argc + 1];
    for (int i = 0; i < argc; i++) {
        childArgv[i] = argv ? argv[i] : nullptr;
    }
    childArgv[argc] = nullptr;

    int nenv = 0;
    if (environ) {
        while (environ[nenv]) {
            nenv++;
        }
    }
    char** envp = new char*[nenv + 2];
    for (int i = 0; i < nenv; i++) {
        envp[i] = environ[i];
    }
    envp[nenv] = const_cast<char*>("SPEECH_RESPONSIBILITY_DISCLAIMED=1");
    envp[nenv + 1] = nullptr;

    posix_spawnattr_t attrs = nullptr;
    pid_t pid = 0;
    int err = posix_spawnattr_init(&attrs);
    if (err == 0) {
        err = responsibility_spawnattrs_setdisclaim(&attrs, 1);
        if (err == 0) {
            err = posix_spawn(&pid, path, nullptr, &attrs, childArgv, envp);
        }
        posix_spawnattr_destroy(&attrs);
    }
    delete[] childArgv;
    delete[] envp;
    if (err != 0) {
        fprintf(stderr,
                "Speech could not disclaim the terminal's responsibility: "
                "%s\n",
                strerror(err));
        return true;
    }

    int raw = 0;
    for (;;) {
        if (waitpid(pid, &raw, 0) != -1) {
            break;
        }
        if (errno != EINTR) {
            fprintf(stderr,
                    "Speech could not disclaim the terminal's responsibility: "
                    "%s\n",
                    strerror(errno));
            return true;
        }
    }
    int code = WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw);
    if (status) {
        *status = code;
    }
    return false;
}

void PlatGetExeDir(char* out, int cap) {
    PlatExeDirFromExePath(out, cap);
}

bool PlatSelfUsage(uint64_t* cpu100ns, uint64_t* memBytes) {

    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    task_basic_info_data_t basic = {};
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&basic,
                  &count) != KERN_SUCCESS) {
        return false;
    }
    if (memBytes) {
        *memBytes = (uint64_t)basic.resident_size;
    }
    if (cpu100ns) {
        uint64_t us = (uint64_t)basic.user_time.seconds * 1000000ull +
                      (uint64_t)basic.user_time.microseconds +
                      (uint64_t)basic.system_time.seconds * 1000000ull +
                      (uint64_t)basic.system_time.microseconds;
        count = TASK_THREAD_TIMES_INFO_COUNT;
        task_thread_times_info_data_t threads = {};
        if (task_info(mach_task_self(), TASK_THREAD_TIMES_INFO,
                      (task_info_t)&threads, &count) == KERN_SUCCESS) {
            us += (uint64_t)threads.user_time.seconds * 1000000ull +
                  (uint64_t)threads.user_time.microseconds +
                  (uint64_t)threads.system_time.seconds * 1000000ull +
                  (uint64_t)threads.system_time.microseconds;
        }
        *cpu100ns = us * 10ull;
    }
    return true;
}

const PlatFacts kPlatFacts = {
    .secondaryIsCommand = true,

    .showsWindowControls = false,
    .scrollBounce = true,
    .overscrollPhaselessStops = true,
    .monoFontName = "Menlo",
    .shellDataDir = "/Library/Application Support",
    .shellPlatformName = "macos",
    .exePath = ExePath,
    .disclaimSpeechResponsibility = DisclaimSpeechResponsibility,
};

}

#endif

#if GPUI_OS_LINUX || GPUI_OS_MAC || GPUI_OS_IOS || GPUI_OS_ANDROID
#line 1 "src/base_mem_posix.cpp"

#include <sys/mman.h>
#include <unistd.h>

namespace base {

uint64_t PlatPageSize() {
    static uint64_t pageSize = 0;
    if (pageSize == 0) {
        long n = sysconf(_SC_PAGESIZE);
        pageSize = n > 0 ? (uint64_t)n : 4096;
    }
    return pageSize;
}

uint64_t PlatLargePageSize() {
    return 2ull * 1024ull * 1024ull;
}

void* PlatMemReserve(uint64_t size) {
    if (size == 0) {
        return nullptr;
    }
    void* p = mmap(nullptr, (size_t)size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

bool PlatMemCommitPages(void* base, uint64_t size, bool largePages) {
    (void)largePages;

    uint64_t page = PlatPageSize();
    uintptr_t start = (uintptr_t)base & ~(uintptr_t)(page - 1);
    uintptr_t end =
        ((uintptr_t)base + (uintptr_t)size + page - 1) & ~(uintptr_t)(page - 1);
    return mprotect((void*)start, (size_t)(end - start),
                    PROT_READ | PROT_WRITE) == 0;
}

void* PlatMemReserveCommit(uint64_t size, bool largePages) {
    (void)largePages;
    if (size == 0) {
        return nullptr;
    }
    void* p = mmap(nullptr, (size_t)size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

void PlatMemRelease(void* base, uint64_t size) {
    if (base && size > 0) {
        munmap(base, (size_t)size);
    }
}

}

#endif

#if GPUI_OS_LINUX || GPUI_OS_MAC || GPUI_OS_IOS || GPUI_OS_ANDROID || GPUI_OS_WASM
#line 1 "src/base_posix.cpp"

#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace base {

template <typename S>
static auto StatMtime(const S* st, int) -> decltype(st->st_mtim) {
    return st->st_mtim;
}
template <typename S>
static auto StatMtime(const S* st, long) -> decltype(st->st_mtimespec) {
    return st->st_mtimespec;
}

uint64_t PlatStatModifiedNs(const struct stat* st) {
    auto t = StatMtime(st, 0);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

bool PlatProcSelfExePath(char* out, int cap) {
    ssize_t n = readlink("/proc/self/exe", out, (size_t)cap - 1);
    if (n <= 0) {
        return false;
    }
    out[n] = 0;
    return true;
}

int StrCmpI(const char* a, const char* b) {
    return strcasecmp(a ? a : "", b ? b : "");
}

int StrCmpNI(const char* a, const char* b, int n) {
    if (n <= 0) {
        return 0;
    }
    return strncasecmp(a ? a : "", b ? b : "", (size_t)n);
}

bool PlatDirExists(const char* path) {
    if (!path || !path[0]) {
        return false;
    }
    struct stat st = {};
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool PlatFileIsExecutable(const char* path) {
    if (!path || !path[0]) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return (st.st_mode & 0111) != 0;
}

bool PlatMakeExecutable(const char* path) {
    if (!path || !path[0]) return false;
    return chmod(path, 0755) == 0;
}

extern "C" char** environ;

bool PlatVisitEnv(void (*visit)(Str name, Str value, void* user), void* user) {
    if (!visit) return false;
    if (!environ) return true;
    for (char** at = environ; *at; at++) {
        char* eq = strchr(*at, '=');
        if (!eq || eq == *at) continue;
        visit(Str(*at, (int)(eq - *at)), Str(eq + 1), user);
    }
    return true;
}

bool PlatFileExists(const char* path) {
    if (!path || !path[0]) {
        return false;
    }
    struct stat st = {};
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

void PlatGetCwd(char* out, int cap) {
    if (!out || cap <= 0) {
        return;
    }
    out[0] = 0;
    if (!getcwd(out, (size_t)cap)) {
        out[0] = 0;
    }
}

bool PlatCanonicalPath(const char* path, char* out, int cap) {
    if (!path || !path[0] || !out || cap <= 0) {
        return false;
    }
    out[0] = 0;
    char* resolved = realpath(path, nullptr);
    if (!resolved) {
        return false;
    }
    int n = (int)strlen(resolved);
    if (n >= cap) {
        free(resolved);
        return false;
    }
    memcpy(out, resolved, (size_t)n + 1);
    free(resolved);
    return true;
}

int PlatListDir(const char* dir, DirEntry* out, int max) {
    if (!dir || !out || max <= 0) {
        return 0;
    }
    DIR* d = opendir(dir);
    if (!d) {
        return 0;
    }
    int n = 0;
    struct dirent* ent = nullptr;
    while (n < max && (ent = readdir(d)) != nullptr) {
        Str name = Str(ent->d_name);
        if (StrEq(name, StrL(".")) || StrEq(name, StrL(".."))) {
            continue;
        }
        DirEntry& e = out[n];
        StrCopyZ(e.name, (int)sizeof(e.name), ent->d_name);
        TempStr full = fmt("%s/%s", Str(dir), name);
        struct stat st = {};
        if (len(full) >= kMaxPath || lstat(full.s, &st) != 0) {
            continue;
        }
        e.isSymlink = S_ISLNK(st.st_mode);
        e.isDir = S_ISDIR(st.st_mode);
        e.isFile = S_ISREG(st.st_mode);
        e.size = e.isFile && st.st_size > 0 ? (uint64_t)st.st_size : 0;
        e.modified = PlatStatModifiedNs(&st);
        n++;
    }
    closedir(d);
    return n;
}

int PlatCoreCount() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

void CondVar::Wait(Mutex* m, int timeoutMs) {
    if (timeoutMs < 0) {
        pthread_cond_wait(&cv, &m->lock);
        return;
    }

    struct timespec ts = {};
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeoutMs / 1000;
    ts.tv_nsec += (long)(timeoutMs % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&cv, &m->lock, &ts);
}

static void* ThreadMain(void* arg) {
    auto* call = (Func0*)arg;
    call->Call();
    free(call);
    return nullptr;
}

bool PlatThreadRun(Func0 f) {
    auto* call = (Func0*)calloc(1, sizeof(Func0));
    if (!call) {
        return false;
    }
    *call = f;
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th = {};
    int err = pthread_create(&th, &attr, ThreadMain, call);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        free(call);
        return false;
    }
    return true;
}

uint64_t PlatThreadId() {

    pthread_t self = pthread_self();
    uint64_t id = 0;
    memcpy(&id, &self, sizeof(self) < sizeof(id) ? sizeof(self) : sizeof(id));
    return id;
}

uint64_t PlatProcessId() {
    return (uint64_t)getpid();
}

void PlatSleepMs(int ms) {
    if (ms <= 0) {
        return;
    }
    struct timespec ts = {};
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

}

#endif

#if GPUI_OS_WASM
#line 1 "src/base_wasm.cpp"

#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
#include <sys/stat.h>
#include <stdlib.h>

namespace base {

void PlatDirNameInPlace(char* path);

uint64_t PlatPageSize() {
    return 65536;
}

uint64_t PlatLargePageSize() {

    return 65536;
}

void* PlatMemReserve(uint64_t size) {
    if (size == 0) {
        return nullptr;
    }

    return aligned_alloc((size_t)PlatPageSize(), (size_t)size);
}

bool PlatMemCommitPages(void* base, uint64_t size, bool largePages) {
    (void)largePages;

    memset(base, 0, (size_t)size);
    return true;
}

void* PlatMemReserveCommit(uint64_t size, bool largePages) {
    if (largePages) {

        return nullptr;
    }
    void* p = PlatMemReserve(size);
    if (p) {
        memset(p, 0, (size_t)size);
    }
    return p;
}

void PlatMemRelease(void* base, uint64_t size) {
    (void)size;
    free(base);
}

void PlatGetExeDir(char* out, int cap) {
    StrCopyZ(out, cap, "/");
}

bool PlatSelfUsage(uint64_t* cpu100ns, uint64_t* memBytes) {
    if (cpu100ns) {

        *cpu100ns = (uint64_t)(emscripten_get_now() * 10000.0);
    }
    if (memBytes) {

        *memBytes = (uint64_t)emscripten_get_heap_size();
    }
    return true;
}

const PlatFacts kPlatFacts = {
    .shellPlatformName = "emscripten",
    .scrollGestureLocks = false,

    .asyncIo = true,

    .arenaReserveSize = 4ull * 1024ull * 1024ull,
};

}

#endif

#if GPUI_OS_WINDOWS
#line 1 "src/base_win.cpp"

#include <psapi.h>

namespace base {

uint64_t PlatPageSize() {
    static uint64_t pageSize = 0;
    if (pageSize == 0) {
        SYSTEM_INFO info = {};
        GetSystemInfo(&info);
        pageSize = info.dwPageSize;
    }
    return pageSize;
}

uint64_t PlatLargePageSize() {
    static uint64_t largePageSize = 0;
    if (largePageSize == 0) {
        SIZE_T size = GetLargePageMinimum();
        largePageSize = size ? (uint64_t)size : PlatPageSize();
    }
    return largePageSize;
}

bool PlatMemCommitPages(void* base, uint64_t size, bool largePages) {
    DWORD flags = MEM_COMMIT;
    if (largePages) {
        flags |= MEM_LARGE_PAGES;
    }
    return VirtualAlloc(base, (SIZE_T)size, flags, PAGE_READWRITE) != nullptr;
}

void* PlatMemReserve(uint64_t size) {
    return VirtualAlloc(nullptr, (SIZE_T)size, MEM_RESERVE, PAGE_READWRITE);
}

void* PlatMemReserveCommit(uint64_t size, bool largePages) {
    DWORD flags = MEM_RESERVE | MEM_COMMIT;
    if (largePages) {
        flags |= MEM_LARGE_PAGES;
    }
    return VirtualAlloc(nullptr, (SIZE_T)size, flags, PAGE_READWRITE);
}

void PlatMemRelease(void* base, uint64_t size) {
    (void)size;
    VirtualFree(base, 0, MEM_RELEASE);
}

int StrCmpI(const char* a, const char* b) {
    return _stricmp(a ? a : "", b ? b : "");
}

int StrCmpNI(const char* a, const char* b, int n) {
    if (n <= 0) {
        return 0;
    }
    return _strnicmp(a ? a : "", b ? b : "", (size_t)n);
}

WCHAR* ToCWstrTemp(Str s) {
    Arena* arena = GetTempArena();
    int n = 0;
    if (s.s && len(s) > 0) {
        n = MultiByteToWideChar(CP_UTF8, 0, s.s, len(s), nullptr, 0);
        if (n < 0) {
            n = 0;
        }
    }
    auto res = (WCHAR*)arena->Push((uint64_t)(n + 1) * sizeof(WCHAR),
                                   alignof(WCHAR), false);
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s.s, len(s), res, n);
    }
    res[n] = 0;
    return res;
}

bool PlatDirExists(const char* path) {
    if (!path || !path[0]) {
        return false;
    }
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool PlatFileExists(const char* path) {
    if (!path || !path[0]) {
        return false;
    }
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void PlatGetCwd(char* out, int cap) {
    if (!out || cap <= 0) {
        return;
    }
    out[0] = 0;
    GetCurrentDirectoryA((DWORD)cap, out);
}

bool PlatCanonicalPath(const char* path, char* out, int cap) {
    if (!path || !path[0] || !out || cap <= 0) {
        return false;
    }
    out[0] = 0;
    HANDLE file = CreateFileW(
        ToCWstrTemp(Str(path)), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    WCHAR wide[kMaxPath] = {};
    DWORD n = GetFinalPathNameByHandleW(file, wide, kMaxPath,
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(file);
    if (n == 0 || n >= kMaxPath) {
        return false;
    }
    const WCHAR* start = wide;
    int prefixBytes = 0;
    if (n >= 8 && wide[0] == L'\\' && wide[1] == L'\\' && wide[2] == L'?' &&
        wide[3] == L'\\' && wide[4] == L'U' && wide[5] == L'N' &&
        wide[6] == L'C' && wide[7] == L'\\') {
        start += 8;
        n -= 8;
        prefixBytes = 2;
    } else if (n >= 4 && wide[0] == L'\\' && wide[1] == L'\\' &&
               wide[2] == L'?' && wide[3] == L'\\') {
        start += 4;
        n -= 4;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, start, (int)n, nullptr, 0,
                                    nullptr, nullptr);
    if (bytes <= 0 || bytes + prefixBytes >= cap) {
        return false;
    }
    if (prefixBytes) {
        out[0] = '/';
        out[1] = '/';
    }
    WideCharToMultiByte(CP_UTF8, 0, start, (int)n, out + prefixBytes,
                        cap - prefixBytes - 1, nullptr, nullptr);
    bytes += prefixBytes;
    out[bytes] = 0;
    for (int i = 0; i < bytes; i++) {
        if (out[i] == '\\') {
            out[i] = '/';
        }
    }
    return true;
}

static bool ExePath(char* out, int cap) {
    DWORD n = GetModuleFileNameA(nullptr, out, (DWORD)cap);
    if (n == 0 || n >= (DWORD)cap) {
        out[0] = 0;
        return false;
    }

    for (DWORD i = 0; i < n; i++) {
        if (out[i] == '\\') out[i] = '/';
    }
    return true;
}

void PlatGetExeDir(char* out, int cap) {
    PlatExeDirFromExePath(out, cap);
}

bool PlatFileIsExecutable(const char* path) {
    (void)path;
    return false;
}

bool PlatMakeExecutable(const char* path) {
    (void)path;
    return true;
}

static char* WideToUtf8Alloc(const WCHAR* text, int wlen) {
    if (!text) return nullptr;
    if (wlen < 0) wlen = (int)wcslen(text);
    int bytes = wlen == 0
                    ? 0
                    : WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                                          wlen, nullptr, 0, nullptr, nullptr);
    if (wlen != 0 && bytes <= 0) return nullptr;
    char* out = (char*)malloc((size_t)bytes + 1);
    if (!out) return nullptr;
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, wlen, out,
                            bytes, nullptr, nullptr);
    }
    out[bytes] = 0;
    return out;
}

bool PlatVisitEnv(void (*visit)(Str name, Str value, void* user), void* user) {
    if (!visit) return false;
    LPWCH block = GetEnvironmentStringsW();
    if (!block) return false;
    for (WCHAR* at = block; *at;) {
        int n = (int)wcslen(at);
        WCHAR* eq = wcschr(at, L'=');

        if (eq && eq != at && at[0] != L'=') {
            char* name = WideToUtf8Alloc(at, (int)(eq - at));
            char* value = WideToUtf8Alloc(eq + 1, -1);
            if (name && value && name[0]) visit(Str(name), Str(value), user);
            free(name);
            free(value);
        }
        at += n + 1;
    }
    FreeEnvironmentStringsW(block);
    return true;
}

int PlatListDir(const char* dir, DirEntry* out, int max) {
    if (!dir || !out || max <= 0) {
        return 0;
    }
    TempStr pattern = fmt("%s\\*", Str(dir));
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(ToCWstrTemp(pattern), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int n = 0;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 ||
             (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
            continue;
        }
        DirEntry& e = out[n];
        int got = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, e.name,
                                      (int)sizeof(e.name), nullptr, nullptr);
        if (got <= 0) {
            continue;
        }
        e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.isFile = !e.isDir;
        e.isSymlink = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.modified = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
                     fd.ftLastWriteTime.dwLowDateTime;
        n++;
    } while (n < max && FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

int PlatCoreCount() {
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

bool PlatSelfUsage(uint64_t* cpu100ns, uint64_t* memBytes) {
    HANDLE self = GetCurrentProcess();
    FILETIME creation = {}, exit = {}, kernel = {}, user = {};
    if (!GetProcessTimes(self, &creation, &exit, &kernel, &user)) {
        return false;
    }
    ULARGE_INTEGER k = {}, u = {};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    if (cpu100ns) {
        *cpu100ns = k.QuadPart + u.QuadPart;
    }
    PROCESS_MEMORY_COUNTERS mem = {};
    mem.cb = sizeof(mem);
    if (!GetProcessMemoryInfo(self, &mem, sizeof(mem))) {
        return false;
    }
    if (memBytes) {
        *memBytes = (uint64_t)mem.WorkingSetSize;
    }
    return true;
}

static DWORD WINAPI ThreadMain(LPVOID arg) {
    auto* call = (Func0*)arg;
    call->Call();
    free(call);
    return 0;
}

bool PlatThreadRun(Func0 f) {
    auto* call = (Func0*)calloc(1, sizeof(Func0));
    if (!call) {
        return false;
    }
    *call = f;
    HANDLE h = CreateThread(nullptr, 0, ThreadMain, call, 0, nullptr);
    if (!h) {
        free(call);
        return false;
    }

    CloseHandle(h);
    return true;
}

uint64_t PlatThreadId() {
    return (uint64_t)GetCurrentThreadId();
}

uint64_t PlatProcessId() {
    return (uint64_t)GetCurrentProcessId();
}

void PlatSleepMs(int ms) {
    Sleep((DWORD)(ms < 0 ? 0 : ms));
}

const PlatFacts kPlatFacts = {
    .pathSep = '\\',
    .pathsCaseFold = true,
    .isWindows = true,
    .monoFontName = "Consolas",
    .shellDataDir = "\\AppData\\Roaming",
    .shellPlatformName = "windows",
    .exePath = ExePath,
};

}

#endif

#if GPUI_OS_LINUX
#line 1 "src/wry/wry_linux.cpp"

#if defined(GPUI_HAVE_WEBKITGTK) && GPUI_HAVE_WEBKITGTK

#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#include <webkit2/webkit2.h>
#include <X11/Xlib.h>
#include <math.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

namespace wry {

using base::logf;
using base::Str;
using base::StrDup;
using base::StrFree;

static constexpr CookieSameSite kSameSiteNone = CookieSameSite();

static const char* const kWebViewIdKey = "webview_id";
static const char* const kIpcScript =
    "Object.defineProperty(window, 'ipc', { value: Object.freeze({ "
    "postMessage: function(x) { "
    "window.webkit.messageHandlers['ipc'].postMessage(x) } }) })";

template <typename F>
static GCallback Cb(F f) {
    return (GCallback)(void*)f;
}

static const char* CStrTemp(Str s) {
    if (!s.s || len(s) <= 0) {
        return "";
    }
    Str z = base::StrDupTemp(s);
    return z.s ? z.s : "";
}

static const char* CStrOrNull(Str s) {
    return s.s ? CStrTemp(s) : nullptr;
}

static Str FromCTemp(const char* s) {
    return s ? base::StrDupTemp(Str(s)) : Str();
}

static bool gGtkTried = false;
static bool gGtkOk = false;

static bool EnsureGtk() {
    if (gGtkTried) {
        return gGtkOk;
    }
    gGtkTried = true;
    gdk_set_allowed_backends("x11");

    gtk_disable_setlocale();
    if (!gtk_init_check(nullptr, nullptr)) {

        WebViewSetLastError("GTK could not open an X11 display");
        logf("wry: GTK could not open the X display\n");
        return false;
    }
    GdkDisplay* display = gdk_display_get_default();
    if (!display || !GDK_IS_X11_DISPLAY(display)) {
        WebViewSetLastError("GTK did not open an X11 display");
        logf("wry: GTK did not open an X11 display\n");
        return false;
    }

    if (!g_main_context_acquire(g_main_context_default())) {
        logf("wry: GLib's main context belongs to another thread\n");
        return false;
    }
    gGtkOk = true;
    return true;
}

static ::Display* XDisplayOf(GdkDisplay* display) {
    return gdk_x11_display_get_xdisplay(GDK_X11_DISPLAY(display));
}

struct XTrap {
    GdkDisplay* display;
    explicit XTrap(GdkDisplay* d) : display(d) {
        gdk_x11_display_error_trap_push(display);
    }
    ~XTrap() { gdk_x11_display_error_trap_pop_ignored(display); }
};

static Vec<PollFd> gPollFds;
static int gPollCount = 0;
static int gMaxPriority = 0;
static bool gPrepared = false;

static_assert(sizeof(PollFd) == sizeof(GPollFD), "GPollFD layout");

int EventLoopPrepare(PollFd** fds, int* timeoutMs) {
    *fds = nullptr;
    if (!gGtkOk) {
        return 0;
    }
    GMainContext* context = g_main_context_default();
    g_main_context_prepare(context, &gMaxPriority);
    int timeout = -1;

    int room = gPollFds.cap > 0 ? gPollFds.cap : 0;
    for (;;) {
        int n = g_main_context_query(context, gMaxPriority, &timeout,
                                     (GPollFD*)gPollFds.els, room);
        if (n <= room) {
            gPollCount = n;
            break;
        }
        if (!VecReserve(gPollFds, n)) {
            gPollCount = 0;
            break;
        }
        room = n;
    }
    for (int i = 0; i < gPollCount; i++) {
        gPollFds.els[i].revents = 0;
    }
    gPrepared = true;
    if (timeout >= 0 && (*timeoutMs < 0 || timeout < *timeoutMs)) {
        *timeoutMs = timeout;
    }
    *fds = gPollFds.els;
    return gPollCount;
}

void EventLoopDispatch() {
    if (!gPrepared) {
        return;
    }
    gPrepared = false;
    GMainContext* context = g_main_context_default();
    if (g_main_context_check(context, gMaxPriority, (GPollFD*)gPollFds.els,
                             gPollCount)) {
        g_main_context_dispatch(context);
    }
}

static void PumpUntil(const bool* done) {
    gPrepared = false;
    while (!*done) {
        g_main_context_iteration(nullptr, TRUE);
    }
}

struct WebView;

struct RelatedWebView {
    GtkWidget* window;
    WebView* webview;
};

struct DownloadState;

enum class DragState : uint8_t {
    Left,
    Entered,
    Leaving,
};

struct WebView : WebViewHandlers {
    Str id;
    WebKitWebView* webview = nullptr;
    WebKitWebContext* context = nullptr;
    WebKitUserContentManager* manager = nullptr;
    GCancellable* cancellable = nullptr;

    bool hasX11 = false;
    bool isChild = false;
    GdkDisplay* display = nullptr;
    ::Display* xdisplay = nullptr;
    ::Window x11Window = 0;
    ::Window parentWindow = 0;
    GtkWidget* gtkWindow = nullptr;
    bool followsParent = false;

    bool inspectorOpen = false;

    double userZoom = 1;
    float matchedScale = 1;

    Vec<Str> pendingScripts;
    bool pendingOpen = true;

    WebViewPageClick pageClick = nullptr;
    void* pageClickUser = nullptr;

    DragState dragState = DragState::Left;
    Vec<Str> dragPaths;
    bool hasDragPaths = false;
    int dragX = 0;
    int dragY = 0;
    guint dragLeaveIdle = 0;

    uint8_t bfState = 0;

    Vec<RelatedWebView> related;
    Vec<DownloadState*> downloads;
};

static Str UriTemp(WebKitWebView* webview) {
    return FromCTemp(webkit_web_view_get_uri(webview));
}

struct EvalCall {
    void* ctx;
    void (*callback)(void* ctx, Str result);
};

static void OnEvalDone(GObject* source, GAsyncResult* result, gpointer data) {
    EvalCall* call = (EvalCall*)data;
    GError* error = nullptr;
#if WEBKIT_CHECK_VERSION(2, 40, 0)
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(source), result, &error);
#else
    WebKitJavascriptResult* js = webkit_web_view_run_javascript_finish(
        WEBKIT_WEB_VIEW(source), result, &error);
    JSCValue* value = nullptr;
    if (js) {
        value =
            (JSCValue*)g_object_ref(webkit_javascript_result_get_js_value(js));
        webkit_javascript_result_unref(js);
    }
#endif
    bool cancelled =
        error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

    char* json = value ? jsc_value_to_json(value, 0) : nullptr;
    if (call && call->callback && !cancelled) {
        call->callback(call->ctx, json ? Str(json) : Str());
    }
    g_free(json);
    if (value) {
        g_object_unref(value);
    }
    if (error) {
        g_error_free(error);
    }
    delete call;
}

static void RunJavascript(WebView* wv, Str js, void* ctx,
                          void (*callback)(void* ctx, Str result)) {
    EvalCall* call = nullptr;
    if (callback) {
        call = new EvalCall{ctx, callback};
    }
#if WEBKIT_CHECK_VERSION(2, 40, 0)
    webkit_web_view_evaluate_javascript(wv->webview, CStrTemp(js), -1, nullptr,
                                        nullptr, wv->cancellable,
                                        call ? OnEvalDone : nullptr, call);
#else
    webkit_web_view_run_javascript(wv->webview, CStrTemp(js), wv->cancellable,
                                   call ? OnEvalDone : nullptr, call);
#endif
}

static void FlushPendingScripts(WebView* wv) {
    if (!wv->pendingOpen) {
        return;
    }
    wv->pendingOpen = false;
    for (int i = 0; i < wv->pendingScripts.len; i++) {
        RunJavascript(wv, wv->pendingScripts[i], nullptr, nullptr);
    }
    StrVecFree(&wv->pendingScripts);
}

static bool AddUserScript(WebView* wv, Str js, bool forMainFrameOnly) {
    if (!wv->manager) {
        logf("wry: the webview has no user content manager\n");
        return false;
    }
    WebKitUserScript* script = webkit_user_script_new(
        CStrTemp(js),
        forMainFrameOnly ? WEBKIT_USER_CONTENT_INJECT_TOP_FRAME
                         : WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(wv->manager, script);
    webkit_user_script_unref(script);
    return true;
}

static ::Window PlugXid(WebView* wv) {
    if (!wv->gtkWindow) {
        return 0;
    }
    GdkWindow* window = gtk_widget_get_window(wv->gtkWindow);
    return window ? gdk_x11_window_get_xid(window) : 0;
}

static bool OwnsXFocus(WebView* wv) {
    ::Window focus = 0;
    int revert = 0;
    XGetInputFocus(wv->xdisplay, &focus, &revert);
    return focus != 0 && (focus == PlugXid(wv) || focus == wv->x11Window);
}

static void GiveXFocus(WebView* wv, Time time) {
    if (!wv->hasX11) {
        return;
    }
    ::Window plug = PlugXid(wv);
    if (!plug) {
        return;
    }
    XTrap trap(wv->display);
    XSetInputFocus(wv->xdisplay, plug, RevertToParent, time);
    XFlush(wv->xdisplay);
}

static void OnClose(WebKitWebView* webview, gpointer) {

    gtk_widget_destroy(GTK_WIDGET(webview));
}

static void OnTitle(GObject*, GParamSpec*, gpointer data) {
    WebView* wv = (WebView*)data;
    if (wv->documentTitleChangedHandler) {
        wv->documentTitleChangedHandler(
            wv->ctx, FromCTemp(webkit_web_view_get_title(wv->webview)));
    }
}

static void OnLoadChanged(WebKitWebView* webview, WebKitLoadEvent event,
                          gpointer data) {
    WebView* wv = (WebView*)data;
    if (event == WEBKIT_LOAD_COMMITTED) {
        if (wv->onPageLoadHandler) {
            wv->onPageLoadHandler(wv->ctx, PageLoadEvent::Started,
                                  UriTemp(webview));
        }
        FlushPendingScripts(wv);
    } else if (event == WEBKIT_LOAD_FINISHED && wv->onPageLoadHandler) {
        wv->onPageLoadHandler(wv->ctx, PageLoadEvent::Finished,
                              UriTemp(webview));
    }
}

static gboolean OnDecidePolicy(WebKitWebView*, WebKitPolicyDecision* decision,
                               WebKitPolicyDecisionType type, gpointer data) {
    WebView* wv = (WebView*)data;
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION ||
        !wv->navigationHandler) {
        return FALSE;
    }
    WebKitNavigationAction* action =
        webkit_navigation_policy_decision_get_navigation_action(
            WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    WebKitURIRequest* request =
        action ? webkit_navigation_action_get_request(action) : nullptr;
    const char* uri = request ? webkit_uri_request_get_uri(request) : nullptr;
    if (!uri) {
        return FALSE;
    }
    if (wv->navigationHandler(wv->ctx, FromCTemp(uri))) {
        webkit_policy_decision_use(decision);
    } else {
        webkit_policy_decision_ignore(decision);
    }
    return TRUE;
}

static void OnScriptMessage(WebKitUserContentManager*,
                            WebKitJavascriptResult* result, gpointer data) {
    WebView* wv = (WebView*)data;
    JSCValue* value =
        result ? webkit_javascript_result_get_js_value(result) : nullptr;
    if (!value || !wv->ipcHandler) {
        return;
    }
    char* body = jsc_value_to_string(value);
    wv->ipcHandler(wv->ctx, UriTemp(wv->webview), body ? Str(body) : Str());
    g_free(body);
}

static gboolean OnInspectorBringToFront(WebKitWebInspector*, gpointer data) {
    ((WebView*)data)->inspectorOpen = true;
    return FALSE;
}

static void OnInspectorClosed(WebKitWebInspector*, gpointer data) {
    ((WebView*)data)->inspectorOpen = false;
}

static const uint8_t kBack = 1;
static const uint8_t kForward = 2;

static void DispatchSyntheticMouse(WebView* wv, GdkEventButton* event,
                                   bool pressed) {
    SyntheticMouseEvent ev;
    ev.pressed = pressed;
    ev.button = (int)event->button;
    ev.x = (int)event->x;
    ev.y = (int)event->y;
    guint state = event->state;
    ev.buttons = ((state & GDK_BUTTON1_MASK) ? 1 : 0) +
                 ((state & GDK_BUTTON3_MASK) ? 2 : 0) +
                 ((state & GDK_BUTTON2_MASK) ? 4 : 0) +
                 ((wv->bfState & kBack) ? 8 : 0) +
                 ((wv->bfState & kForward) ? 16 : 0);

    guint clicks = 1;
    if (!gdk_event_get_click_count((GdkEvent*)event, &clicks)) {
        clicks = 1;
    }
    ev.detail = (int)clicks;
    ev.ctrlKey = (state & GDK_CONTROL_MASK) != 0;
    ev.altKey = (state & GDK_MOD1_MASK) != 0;
    ev.shiftKey = (state & GDK_SHIFT_MASK) != 0;
    ev.metaKey = (state & GDK_SUPER_MASK) != 0;
    RunJavascript(wv, SyntheticMouseEventJsTemp(&ev), nullptr, nullptr);
}

static gboolean OnButtonPress(GtkWidget*, GdkEventButton* event,
                              gpointer data) {
    WebView* wv = (WebView*)data;

    if (wv->hasX11) {
        GiveXFocus(wv, event->time);
        gtk_widget_grab_focus(GTK_WIDGET(wv->webview));
    }
    if (event->button == 8 || event->button == 9) {
        wv->bfState |= event->button == 8 ? kBack : kForward;
        DispatchSyntheticMouse(wv, event, true);
        return TRUE;
    }

    if (wv->pageClick && event->type == GDK_BUTTON_PRESS &&
        (event->button == 1 || event->button == 2 || event->button == 3)) {
        float scale =
            (float)gtk_widget_get_scale_factor(GTK_WIDGET(wv->webview));
        wv->pageClick(wv->pageClickUser, (int)event->button, (float)event->x,
                      (float)event->y, scale);
    }
    return FALSE;
}

static gboolean OnButtonRelease(GtkWidget*, GdkEventButton* event,
                                gpointer data) {
    WebView* wv = (WebView*)data;
    if (event->button == 8 || event->button == 9) {
        wv->bfState &= (uint8_t)~(event->button == 8 ? kBack : kForward);
        DispatchSyntheticMouse(wv, event, false);
        return TRUE;
    }
    return FALSE;
}

static void OnDragDataReceived(GtkWidget*, GdkDragContext*, gint, gint,
                               GtkSelectionData* data, guint info, guint,
                               gpointer user) {
    WebView* wv = (WebView*)user;
    if (info != 2) {
        return;
    }
    gchar** uris = gtk_selection_data_get_uris(data);
    Vec<Str> paths;
    for (int i = 0; uris && uris[i]; i++) {
        VecAppend(paths, StrDup(PathFromFileUriTemp(Str(uris[i]))));
    }
    g_strfreev(uris);
    wv->dragState = DragState::Entered;
    DragDropEvent ev;
    ev.kind = DragDropKind::Enter;
    ev.paths = paths.els;
    ev.pathCount = paths.len;
    ev.x = wv->dragX;
    ev.y = wv->dragY;
    wv->dragDropHandler(wv->ctx, &ev);
    StrVecFree(&wv->dragPaths);
    wv->dragPaths = paths;
    wv->hasDragPaths = true;
}

static gboolean OnDragMotion(GtkWidget*, GdkDragContext*, gint x, gint y, guint,
                             gpointer user) {
    WebView* wv = (WebView*)user;
    if (wv->dragState == DragState::Entered) {
        DragDropEvent ev;
        ev.kind = DragDropKind::Over;
        ev.x = x;
        ev.y = y;
        wv->dragDropHandler(wv->ctx, &ev);
    } else {
        wv->dragX = x;
        wv->dragY = y;
    }
    return FALSE;
}

static gboolean OnDragDrop(GtkWidget*, GdkDragContext* context, gint x, gint y,
                           guint time, gpointer user) {
    WebView* wv = (WebView*)user;
    if (wv->dragState != DragState::Leaving || !wv->hasDragPaths) {
        return FALSE;
    }
    Vec<Str> paths = wv->dragPaths;
    wv->dragPaths = Vec<Str>();
    wv->hasDragPaths = false;
    gdk_drop_finish(context, TRUE, time);
    wv->dragState = DragState::Left;
    DragDropEvent ev;
    ev.kind = DragDropKind::Drop;
    ev.paths = paths.els;
    ev.pathCount = paths.len;
    ev.x = x;
    ev.y = y;
    bool handled = wv->dragDropHandler(wv->ctx, &ev);
    StrVecFree(&paths);
    return handled ? TRUE : FALSE;
}

static gboolean OnDragLeaveIdle(gpointer user) {
    WebView* wv = (WebView*)user;
    wv->dragLeaveIdle = 0;
    if (wv->dragState == DragState::Leaving) {
        wv->dragState = DragState::Left;
        DragDropEvent ev;
        ev.kind = DragDropKind::Leave;
        wv->dragDropHandler(wv->ctx, &ev);
    }
    return G_SOURCE_REMOVE;
}

static void OnDragLeave(GtkWidget*, GdkDragContext*, guint, gpointer user) {
    WebView* wv = (WebView*)user;
    if (wv->dragState == DragState::Left) {
        return;
    }

    wv->dragState = DragState::Leaving;
    if (!wv->dragLeaveIdle) {
        wv->dragLeaveIdle = g_idle_add(OnDragLeaveIdle, wv);
    }
}

struct DownloadState {
    WebView* wv;
    WebKitDownload* download;
    bool failed;
};

static void ForgetDownload(DownloadState* state) {
    WebView* wv = state->wv;
    for (int i = 0; i < wv->downloads.len; i++) {
        if (wv->downloads[i] == state) {
            wv->downloads.els[i] = wv->downloads.els[wv->downloads.len - 1];
            wv->downloads.len--;
            break;
        }
    }
    g_signal_handlers_disconnect_by_data(state->download, state);
    g_object_unref(state->download);
    delete state;
}

static Str DownloadUriTemp(WebKitDownload* download) {
    WebKitURIRequest* request = webkit_download_get_request(download);
    return request ? FromCTemp(webkit_uri_request_get_uri(request)) : Str();
}

static gboolean OnDecideDestination(WebKitDownload* download, gchar* suggested,
                                    gpointer data) {
    DownloadState* state = (DownloadState*)data;
    WebView* wv = state->wv;
    Str uri = DownloadUriTemp(download);
    if (!uri.s || !wv->downloadStartedHandler) {
        return TRUE;
    }

    const char* dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    char* cwd = nullptr;
    if (!dir) {
        cwd = g_get_current_dir();
        dir = cwd;
    }
    Str stem;
    Str ext;
    DownloadFileNameParts(uri, suggested ? Str(suggested) : Str(), &stem, &ext);
    char* path = g_build_filename(
        dir, CStrTemp(base::FormatTemp("%s%s", stem, ext)), nullptr);

    for (int counter = 1; g_file_test(path, G_FILE_TEST_EXISTS); counter++) {
        g_free(path);
        path = g_build_filename(
            dir, CStrTemp(base::FormatTemp("%s (%d)%s", stem, counter, ext)),
            nullptr);
    }
    g_free(cwd);
    Str dest = FromCTemp(path);
    g_free(path);
    if (wv->downloadStartedHandler(wv->ctx, uri, &dest)) {
        webkit_download_set_destination(download, CStrTemp(dest));
    } else {
        webkit_download_cancel(download);
    }
    return TRUE;
}

static void OnDownloadFailed(WebKitDownload*, GError*, gpointer data) {
    ((DownloadState*)data)->failed = true;
}

static void OnDownloadFinished(WebKitDownload* download, gpointer data) {
    DownloadState* state = (DownloadState*)data;
    WebView* wv = state->wv;
    Str uri = DownloadUriTemp(download);
    if (wv->downloadCompletedHandler && uri.s) {
        Str path = state->failed
                       ? Str()
                       : FromCTemp(webkit_download_get_destination(download));
        wv->downloadCompletedHandler(wv->ctx, uri, path.s ? &path : nullptr,
                                     !state->failed);
    }
    ForgetDownload(state);
}

static void OnDownloadStarted(WebKitWebContext*, WebKitDownload* download,
                              gpointer data) {
    WebView* wv = (WebView*)data;
    DownloadState* state = new DownloadState{wv, download, false};
    g_object_ref(download);
    VecAppend(wv->downloads, state);
    g_signal_connect(download, "decide-destination", Cb(OnDecideDestination),
                     state);
    g_signal_connect(download, "failed", Cb(OnDownloadFailed), state);
    g_signal_connect(download, "finished", Cb(OnDownloadFinished), state);
}

struct ProtocolHandler {
    Str name;
    Str webviewId;
    void* ctx;
    void (*handler)(void* ctx, Str id, const Request* request,
                    RequestResponder* responder);
};

static void FreeProtocolHandler(gpointer data) {
    ProtocolHandler* p = (ProtocolHandler*)data;
    StrFree(p->name);
    StrFree(p->webviewId);
    delete p;
}

struct RequestResponder {
    WebKitURISchemeRequest* request;
};

static void CollectHeader(const char* name, const char* value, gpointer data) {
    Header h;
    h.name = FromCTemp(name);
    h.value = FromCTemp(value);
    VecAppend(*(Vec<Header>*)data, h);
}

static void OnUriScheme(WebKitURISchemeRequest* request, gpointer data) {
    ProtocolHandler* p = (ProtocolHandler*)data;
    const char* uri = webkit_uri_scheme_request_get_uri(request);
    if (!uri) {
        GError* error = g_error_new_literal(G_FILE_ERROR, G_FILE_ERROR_EXIST,
                                            "Could not get uri.");
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    Vec<Header> headers;
    SoupMessageHeaders* h = webkit_uri_scheme_request_get_http_headers(request);
    if (h) {
        soup_message_headers_foreach(h, CollectHeader, &headers);
    }
    const char* method = webkit_uri_scheme_request_get_http_method(request);
    Request req;
    req.uri = FromCTemp(uri);
    req.method = method ? FromCTemp(method) : StrL("GET");
    req.headers = headers.els;
    req.headerCount = headers.len;

    Str id;
    WebKitWebView* webview = webkit_uri_scheme_request_get_web_view(request);
    if (webview) {
        id = FromCTemp(
            (const char*)g_object_get_data(G_OBJECT(webview), kWebViewIdKey));
    }
    RequestResponder* responder = new RequestResponder{request};
    g_object_ref(request);
    p->handler(p->ctx, id, &req, responder);
    VecReset(headers);
}

struct PendingResponse {
    WebKitURISchemeRequest* request;
    int status;
    Vec<Header> headers;
    GBytes* body;
};

static gboolean FinishResponse(gpointer data) {
    PendingResponse* r = (PendingResponse*)data;
    GInputStream* input = g_memory_input_stream_new_from_bytes(r->body);
    WebKitURISchemeResponse* response = webkit_uri_scheme_response_new(
        input, (gint64)g_bytes_get_size(r->body));
    webkit_uri_scheme_response_set_status(response, (guint)r->status, nullptr);
    SoupMessageHeaders* headers =
        soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
    for (int i = 0; i < r->headers.len; i++) {
        const char* name = CStrTemp(r->headers[i].name);
        const char* value = CStrTemp(r->headers[i].value);
        if (g_ascii_strcasecmp(name, "content-type") == 0) {
            webkit_uri_scheme_response_set_content_type(response, value);
        }
        soup_message_headers_append(headers, name, value);
    }
    webkit_uri_scheme_response_set_http_headers(response, headers);
    webkit_uri_scheme_request_finish_with_response(r->request, response);
    g_object_unref(response);
    g_object_unref(input);
    g_object_unref(r->request);
    g_bytes_unref(r->body);
    for (int i = 0; i < r->headers.len; i++) {
        StrFree(r->headers[i].name);
        StrFree(r->headers[i].value);
    }
    VecReset(r->headers);
    delete r;
    return G_SOURCE_REMOVE;
}

void Respond(RequestResponder* responder, const Response* response) {
    if (!responder) {
        return;
    }

    PendingResponse* r = new PendingResponse();
    r->request = responder->request;
    r->status = response ? response->status : 500;
    if (response) {
        for (int i = 0; i < response->headerCount; i++) {
            Header h;
            h.name = StrDup(response->headers[i].name);
            h.value = StrDup(response->headers[i].value);
            VecAppend(r->headers, h);
        }
    }
    r->body = g_bytes_new(response ? response->body : nullptr,
                          response && response->body ? response->bodyLen : 0);
    delete responder;
    g_main_context_invoke(nullptr, FinishResponse, r);
}

static WebKitWebContext* NewWebContext(const WebViewAttributes* attrs) {
    WebKitWebContext* context = nullptr;
    if (attrs->incognito) {
        context = webkit_web_context_new_ephemeral();
    } else if (len(attrs->dataDirectory) > 0) {
        const char* dir = CStrTemp(attrs->dataDirectory);
        WebKitWebsiteDataManager* manager = webkit_website_data_manager_new(
            "base-data-directory", dir, nullptr);
        WebKitCookieManager* cookies =
            webkit_website_data_manager_get_cookie_manager(manager);
        if (cookies) {
            char* path = g_build_filename(dir, "cookies", nullptr);
            webkit_cookie_manager_set_persistent_storage(
                cookies, path, WEBKIT_COOKIE_PERSISTENT_STORAGE_TEXT);
            g_free(path);
        }
        context = webkit_web_context_new_with_website_data_manager(manager);
        g_object_unref(manager);
    } else {
        context = webkit_web_context_new();
    }

    webkit_web_context_set_automation_allowed(context, FALSE);
    return context;
}

static void RegisterProtocols(WebView* wv, const WebViewAttributes* attrs) {
    WebKitSecurityManager* security =
        webkit_web_context_get_security_manager(wv->context);
    for (int i = 0; i < attrs->customProtocolCount; i++) {
        const CustomProtocol& cp = attrs->customProtocols[i];
        bool duplicate = false;
        for (int j = 0; j < i; j++) {
            if (base::StrEq(attrs->customProtocols[j].name, cp.name)) {
                duplicate = true;
            }
        }
        if (duplicate || !cp.handler) {

            logf("wry: custom protocol '%s' skipped\n", cp.name);
            continue;
        }
        const char* scheme = CStrTemp(cp.name);
        if (security) {
            webkit_security_manager_register_uri_scheme_as_secure(security,
                                                                  scheme);
        }
        ProtocolHandler* p = new ProtocolHandler();
        p->name = StrDup(cp.name);
        p->webviewId = StrDup(wv->id);
        p->ctx = cp.ctx;
        p->handler = cp.handler;
        webkit_web_context_register_uri_scheme(wv->context, scheme, OnUriScheme,
                                               p, FreeProtocolHandler);
    }
}

static void SetWebviewSettings(WebView* wv, const WebViewAttributes* attrs) {

    WebKitInputMethodContext* im =
        webkit_web_view_get_input_method_context(wv->webview);
    if (im) {
        webkit_input_method_context_set_enable_preedit(im, FALSE);
    }
    webkit_web_context_set_use_system_appearance_for_scrollbars(wv->context,
                                                                FALSE);
    WebKitSettings* settings = webkit_web_view_get_settings(wv->webview);
    if (!settings) {
        return;
    }
    webkit_settings_set_enable_webgl(settings, TRUE);
    webkit_settings_set_enable_webaudio(settings, TRUE);
    webkit_settings_set_enable_back_forward_navigation_gestures(
        settings, attrs->backForwardNavigationGestures);
    if (attrs->clipboard) {
        webkit_settings_set_javascript_can_access_clipboard(settings, TRUE);
    }
    webkit_settings_set_enable_page_cache(settings, TRUE);
    webkit_settings_set_user_agent(settings, CStrOrNull(attrs->userAgent));
    if (attrs->devtools) {
        webkit_settings_set_enable_developer_extras(settings, TRUE);
    }
    if (attrs->javascriptDisabled) {
        webkit_settings_set_enable_javascript(settings, FALSE);
    }
}

static WebView* NewGtk(GtkWidget* container, const WebViewAttributes* attrs,
                       WebKitWebView* relatedView);

static void FreeRelated(WebView* wv, int index, bool destroyWindow);

static void OnRelatedWindowDestroy(GtkWidget* window, gpointer data) {
    WebView* wv = (WebView*)data;
    for (int i = 0; i < wv->related.len; i++) {
        if (wv->related[i].window == window) {
            FreeRelated(wv, i, false);
            return;
        }
    }
}

static GtkWidget* OnCreate(WebKitWebView* webview,
                           WebKitNavigationAction* action, gpointer data) {
    WebView* wv = (WebView*)data;
    WebKitURIRequest* request = webkit_navigation_action_get_request(action);
    const char* uri = request ? webkit_uri_request_get_uri(request) : nullptr;
    if (!uri || !wv->newWindowReqHandler) {
        return nullptr;
    }
    NewWindowFeatures features;
    features.opener = wv;
    WebView* created = nullptr;
    NewWindowResponse response =
        wv->newWindowReqHandler(wv->ctx, FromCTemp(uri), &features, &created);
    if (response == NewWindowResponse::Create) {
        return created && created->webview ? GTK_WIDGET(created->webview)
                                           : nullptr;
    }
    if (response != NewWindowResponse::Allow) {
        return nullptr;
    }

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), uri);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), box);
    gtk_widget_show_all(window);
    WebViewAttributes defaults;
    WebView* related = NewGtk(box, &defaults, webview);
    if (!related) {
        gtk_widget_destroy(window);
        return nullptr;
    }
    VecAppend(wv->related, RelatedWebView{window, related});
    g_signal_connect(window, "destroy", Cb(OnRelatedWindowDestroy), wv);
    return GTK_WIDGET(related->webview);
}

static WebView* NewGtk(GtkWidget* container, const WebViewAttributes* attrs,
                       WebKitWebView* relatedView) {
    WebView* wv = new WebView();
    WebViewHandlersSet(wv, attrs);
    wv->cancellable = g_cancellable_new();

    wv->context = relatedView ? webkit_web_view_get_context(relatedView)
                              : NewWebContext(attrs);
    if (relatedView) {
        g_object_ref(wv->context);
    }
    Str proxy = ProxyUriTemp(&attrs->proxyConfig);
    if (proxy.s) {
        WebKitWebsiteDataManager* manager =
            webkit_web_context_get_website_data_manager(wv->context);
        if (manager) {
            WebKitNetworkProxySettings* settings =
                webkit_network_proxy_settings_new(CStrTemp(proxy), nullptr);
            webkit_website_data_manager_set_network_proxy_settings(
                manager, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, settings);
            webkit_network_proxy_settings_free(settings);
        }
    }

    if (len(attrs->extensionPath) > 0) {
        webkit_web_context_set_web_extensions_directory(
            wv->context, CStrTemp(attrs->extensionPath));
    }

    wv->manager = webkit_user_content_manager_new();
    WebKitWebsitePolicies* policies = nullptr;
    if (attrs->autoplay) {
        policies = webkit_website_policies_new_with_policies(
            "autoplay", WEBKIT_AUTOPLAY_ALLOW, nullptr);
    }
    GObject* object =
        relatedView
            ? (GObject*)g_object_new(
                  WEBKIT_TYPE_WEB_VIEW, "user-content-manager", wv->manager,
                  "is-controlled-by-automation", FALSE, "website-policies",
                  policies, "related-view", relatedView, nullptr)
            : (GObject*)g_object_new(
                  WEBKIT_TYPE_WEB_VIEW, "user-content-manager", wv->manager,
                  "is-controlled-by-automation", FALSE, "website-policies",
                  policies, "web-context", wv->context, nullptr);
    if (policies) {
        g_object_unref(policies);
    }
    wv->webview = WEBKIT_WEB_VIEW(object);
    g_object_ref_sink(wv->webview);

    if (attrs->transparent) {
        GdkRGBA clear = {0, 0, 0, 0};
        webkit_web_view_set_background_color(wv->webview, &clear);
    } else if (attrs->hasBackgroundColor) {

        GdkRGBA c = {
            attrs->backgroundColor.r / 255.0, attrs->backgroundColor.g / 255.0,
            attrs->backgroundColor.b / 255.0, attrs->backgroundColor.a / 255.0};
        webkit_web_view_set_background_color(wv->webview, &c);
    }

    SetWebviewSettings(wv, attrs);

    GObject* view = G_OBJECT(wv->webview);
    g_signal_connect(view, "close", Cb(OnClose), wv);
    gtk_widget_add_events(GTK_WIDGET(wv->webview),
                          GDK_BUTTON1_MOTION_MASK | GDK_BUTTON_PRESS_MASK);
    g_signal_connect(view, "button-press-event", Cb(OnButtonPress), wv);
    g_signal_connect(view, "button-release-event", Cb(OnButtonRelease), wv);
    if (wv->documentTitleChangedHandler) {
        g_signal_connect(view, "notify::title", Cb(OnTitle), wv);
    }
    g_signal_connect(view, "load-changed", Cb(OnLoadChanged), wv);
    if (wv->newWindowReqHandler) {
        g_signal_connect(view, "create", Cb(OnCreate), wv);
    }
    if (wv->navigationHandler) {
        g_signal_connect(view, "decide-policy", Cb(OnDecidePolicy), wv);
    }
    if (wv->downloadStartedHandler || wv->downloadCompletedHandler) {
        g_signal_connect(wv->context, "download-started", Cb(OnDownloadStarted),
                         wv);
    }

    g_signal_connect(wv->manager, "script-message-received::ipc",
                     Cb(OnScriptMessage), wv);
    webkit_user_content_manager_register_script_message_handler(wv->manager,
                                                                "ipc");

    if (wv->dragDropHandler) {
        g_signal_connect(view, "drag-data-received", Cb(OnDragDataReceived),
                         wv);
        g_signal_connect(view, "drag-motion", Cb(OnDragMotion), wv);
        g_signal_connect(view, "drag-drop", Cb(OnDragDrop), wv);
        g_signal_connect(view, "drag-leave", Cb(OnDragLeave), wv);
    }

    gtk_box_pack_start(GTK_BOX(container), GTK_WIDGET(wv->webview), TRUE, TRUE,
                       0);

    WebKitWebInspector* inspector = webkit_web_view_get_inspector(wv->webview);
    if (inspector) {
        g_signal_connect(inspector, "bring-to-front",
                         Cb(OnInspectorBringToFront), wv);
        g_signal_connect(inspector, "closed", Cb(OnInspectorClosed), wv);
    }

    wv->id =
        len(attrs->id) > 0
            ? StrDup(attrs->id)
            : StrDup(base::FormatTemp("%d", (long long)(intptr_t)wv->webview));
    g_object_set_data_full(G_OBJECT(wv->webview), kWebViewIdKey,
                           g_strdup(CStrTemp(wv->id)), g_free);

    AddUserScript(wv, Str(kIpcScript), true);
    for (int i = 0; i < attrs->initializationScriptCount; i++) {
        AddUserScript(wv, attrs->initializationScripts[i].script,
                      attrs->initializationScripts[i].forMainFrameOnly);
    }

    if (!relatedView) {
        RegisterProtocols(wv, attrs);
    }

    if (attrs->url.s) {
        if (attrs->headerCount > 0) {
            WebViewLoadUrlWithHeaders(wv, attrs->url, attrs->headers,
                                      attrs->headerCount);
        } else {
            webkit_web_view_load_uri(wv->webview, CStrTemp(attrs->url));
        }
    } else if (attrs->html.s) {
        webkit_web_view_load_html(wv->webview, CStrTemp(attrs->html), nullptr);
    }

    if (attrs->visible) {
        gtk_widget_show_all(GTK_WIDGET(wv->webview));
    }
    if (attrs->focused) {
        gtk_widget_grab_focus(GTK_WIDGET(wv->webview));
    }
    return wv;
}

static int ToPhysical(double v, bool logical, double scale) {
    return (int)lround(logical ? v * scale : v);
}

static int ToLogical(double v, bool logical, double scale) {
    return (int)lround(logical ? v : v / scale);
}

static ::Window CreateContainer(::Display* xdpy, ::Window parent,
                                const WebViewAttributes* attrs, bool asChild) {
    int x = 0;
    int y = 0;
    int w = 1;
    int h = 1;
    if (asChild) {
        XWindowAttributes pa = {};
        XGetWindowAttributes(xdpy, parent, &pa);
        double scale = pa.screen ? ScaleFactorFromScreen(pa.screen->width,
                                                         pa.screen->mwidth)
                                 : 1.0;
        if (attrs->hasBounds) {
            const Rect& b = attrs->bounds;
            x = ToPhysical(b.position.x, b.position.logical, scale);
            y = ToPhysical(b.position.y, b.position.logical, scale);
            w = ToPhysical(b.size.width, b.size.logical, scale);
            h = ToPhysical(b.size.height, b.size.logical, scale);
        }
    } else {

        XWindowAttributes pa = {};
        XGetWindowAttributes(xdpy, parent, &pa);
        w = pa.width;
        h = pa.height;
    }

    w = std::max(w, 1);
    h = std::max(h, 1);
    ::Window window = XCreateSimpleWindow(xdpy, parent, x, y, (unsigned)w,
                                          (unsigned)h, 0, 0, 0);
    if (attrs->visible) {
        XMapWindow(xdpy, window);
    }
    return window;
}

static void PlaceMappedPlug(WebView* wv) {
    ::Window plug = PlugXid(wv);
    if (!plug) {
        return;
    }
    XWindowAttributes ca = {};
    XTrap trap(wv->display);
    if (!XGetWindowAttributes(wv->xdisplay, wv->x11Window, &ca)) {
        return;
    }
    XMoveResizeWindow(wv->xdisplay, plug, 0, 0, (unsigned)std::max(ca.width, 1),
                      (unsigned)std::max(ca.height, 1));
    XMapWindow(wv->xdisplay, plug);
    XFlush(wv->xdisplay);
}

static void ResizeContainer(WebView* wv, int x, int y, int w, int h) {
    w = std::max(w, 1);
    h = std::max(h, 1);
    {
        XTrap trap(wv->display);
        XMoveResizeWindow(wv->xdisplay, wv->x11Window, x, y, (unsigned)w,
                          (unsigned)h);
        ::Window plug = PlugXid(wv);
        if (plug) {
            XMoveResizeWindow(wv->xdisplay, plug, 0, 0, (unsigned)w,
                              (unsigned)h);
        }
        XFlush(wv->xdisplay);
    }

    gtk_window_resize(GTK_WINDOW(wv->gtkWindow), w, h);
    GtkAllocation a = {0, 0, w, h};
    gtk_widget_size_allocate(wv->gtkWindow, &a);
}

static GdkFilterReturn ParentFilter(GdkXEvent* xevent, GdkEvent*,
                                    gpointer data) {
    WebView* wv = (WebView*)data;
    XEvent* ev = (XEvent*)xevent;
    if (ev->type == ConfigureNotify && ev->xconfigure
                                               .window == wv->parentWindow) {
        ResizeContainer(wv, 0, 0, ev->xconfigure.width, ev->xconfigure.height);
    }
    return GDK_FILTER_CONTINUE;
}

WebView* WebViewNew(void* parentWindow, const WebViewAttributes* attrs,
                    bool asChild) {
    if (!attrs) {
        return nullptr;
    }
    ::Window parent = (::Window)(uintptr_t)parentWindow;
    if (!parent) {

        WebViewSetLastError("a webview needs an X11 window to go in");
        logf("wry: a webview needs an X11 window to go in\n");
        return nullptr;
    }
    if (!EnsureGtk()) {
        return nullptr;
    }
    GdkDisplay* display = gdk_display_get_default();
    ::Display* xdpy = XDisplayOf(display);

    ::Window container = CreateContainer(xdpy, parent, attrs, asChild);
    if (!container) {
        WebViewSetLastError("could not make the webview's X11 window");
        logf("wry: could not make the webview's X11 window\n");
        return nullptr;
    }

    GtkWidget* plug = gtk_plug_new_for_display(display, container);
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(plug), vbox);

    WebView* wv = NewGtk(vbox, attrs, nullptr);

    gtk_widget_show_all(plug);
    wv->display = display;
    wv->xdisplay = xdpy;
    wv->x11Window = container;
    wv->parentWindow = parent;
    wv->gtkWindow = plug;
    wv->isChild = asChild;

    wv->hasX11 = true;
    PlaceMappedPlug(wv);
    if (!attrs->visible) {
        WebViewSetVisible(wv, false);
    }
    WebViewSetLastError(nullptr);
    if (!asChild) {
        XTrap trap(display);
        XSelectInput(xdpy, parent, StructureNotifyMask);
        gdk_window_add_filter(nullptr, ParentFilter, wv);
        wv->followsParent = true;
    }
    return wv;
}

static void FreeInner(WebView* wv) {
    if (!wv) {
        return;
    }
    g_cancellable_cancel(wv->cancellable);
    if (wv->followsParent) {
        gdk_window_remove_filter(nullptr, ParentFilter, wv);
    }
    while (wv->related.len > 0) {
        FreeRelated(wv, wv->related.len - 1, true);
    }
    while (wv->downloads.len > 0) {
        ForgetDownload(wv->downloads[wv->downloads.len - 1]);
    }
    if (wv->dragLeaveIdle) {
        g_source_remove(wv->dragLeaveIdle);
        wv->dragLeaveIdle = 0;
    }

    g_signal_handlers_disconnect_by_data(wv->webview, wv);
    g_signal_handlers_disconnect_by_data(wv->manager, wv);
    g_signal_handlers_disconnect_by_data(wv->context, wv);
    WebKitWebInspector* inspector = webkit_web_view_get_inspector(wv->webview);
    if (inspector) {
        g_signal_handlers_disconnect_by_data(inspector, wv);
    }

    gtk_widget_destroy(GTK_WIDGET(wv->webview));
    if (wv->hasX11) {
        gtk_widget_destroy(wv->gtkWindow);
        XTrap trap(wv->display);
        XDestroyWindow(wv->xdisplay, wv->x11Window);
        XFlush(wv->xdisplay);
    }
    g_object_unref(wv->webview);
    g_object_unref(wv->manager);
    g_object_unref(wv->context);
    g_object_unref(wv->cancellable);
    StrVecFree(&wv->pendingScripts);
    StrVecFree(&wv->dragPaths);
    VecReset(wv->related);
    VecReset(wv->downloads);
    StrFree(wv->id);
    delete wv;
}

static void FreeRelated(WebView* wv, int index, bool destroyWindow) {
    RelatedWebView r = wv->related[index];
    wv->related.els[index] = wv->related.els[wv->related.len - 1];
    wv->related.len--;
    g_signal_handlers_disconnect_by_data(r.window, wv);
    FreeInner(r.webview);
    if (destroyWindow) {
        gtk_widget_destroy(r.window);
    }
}

void WebViewFree(WebView* wv) {
    FreeInner(wv);
}

Str WebViewId(WebView* wv) {
    return wv ? wv->id : Str();
}

bool WebViewEval(WebView* wv, Str js) {
    return WebViewEvalWithCallback(wv, js, nullptr, nullptr);
}

bool WebViewEvalWithCallback(WebView* wv, Str js, void* ctx,
                             void (*callback)(void* ctx, Str result)) {
    if (!wv) {
        return false;
    }
    if (wv->pendingOpen) {

        VecAppend(wv->pendingScripts, StrDup(js));
        return true;
    }
    RunJavascript(wv, js, ctx, callback);
    return true;
}

Str WebViewUrlTemp(WebView* wv) {
    return wv ? UriTemp(wv->webview) : Str();
}

bool WebViewLoadUrl(WebView* wv, Str url) {
    if (!wv) {
        return false;
    }
    webkit_web_view_load_uri(wv->webview, CStrTemp(url));
    return true;
}

bool WebViewLoadUrlWithHeaders(WebView* wv, Str url, const Header* headers,
                               int headerCount) {
    if (!wv) {
        return false;
    }
    WebKitURIRequest* request = webkit_uri_request_new(CStrTemp(url));
    SoupMessageHeaders* h = webkit_uri_request_get_http_headers(request);
    if (h) {
        for (int i = 0; i < headerCount; i++) {
            soup_message_headers_append(h, CStrTemp(headers[i].name),
                                        CStrTemp(headers[i].value));
        }
    }
    webkit_web_view_load_request(wv->webview, request);
    g_object_unref(request);
    return true;
}

bool WebViewLoadHtml(WebView* wv, Str html) {
    if (!wv) {
        return false;
    }
    webkit_web_view_load_html(wv->webview, CStrTemp(html), nullptr);
    return true;
}

bool WebViewReload(WebView* wv) {
    if (!wv) {
        return false;
    }
    webkit_web_view_reload(wv->webview);
    return true;
}

bool WebViewBounds(WebView* wv, Rect* out) {
    if (!wv || !out) {
        return false;
    }
    *out = Rect{};
    if (wv->hasX11) {
        XWindowAttributes a = {};
        XTrap trap(wv->display);
        if (XGetWindowAttributes(wv->xdisplay, wv->x11Window, &a)) {
            out->position = LogicalPosition(a.x, a.y);
            out->size = LogicalSize(a.width, a.height);
        }
        return true;
    }
    GtkAllocation a = {};
    gtk_widget_get_allocation(GTK_WIDGET(wv->webview), &a);
    out->size = LogicalSize(a.width, a.height);
    return true;
}

bool WebViewSetBounds(WebView* wv, Rect bounds) {
    if (!wv) {
        return false;
    }
    double scale = gtk_widget_get_scale_factor(GTK_WIDGET(wv->webview));
    int w = ToLogical(bounds.size.width, bounds.size.logical, scale);
    int h = ToLogical(bounds.size.height, bounds.size.logical, scale);
    int x = ToLogical(bounds.position.x, bounds.position.logical, scale);
    int y = ToLogical(bounds.position.y, bounds.position.logical, scale);
    if (wv->hasX11 && !wv->followsParent) {

        int s = (int)scale;
        ResizeContainer(wv, x * s, y * s, w * s, h * s);
    }
    return true;
}

bool WebViewSetVisible(WebView* wv, bool visible) {
    if (!wv) {
        return false;
    }

    if (wv->hasX11 && wv->isChild) {
        XTrap trap(wv->display);
        if (visible) {
            XMapWindow(wv->xdisplay, wv->x11Window);
        } else {
            XUnmapWindow(wv->xdisplay, wv->x11Window);
        }
    }
    if (visible) {
        gtk_widget_show_all(GTK_WIDGET(wv->webview));
    } else {
        gtk_widget_hide(GTK_WIDGET(wv->webview));
    }
    if (wv->hasX11 && wv->isChild) {
        if (visible) {
            gtk_widget_show_all(wv->gtkWindow);
            PlaceMappedPlug(wv);
        } else {
            gtk_widget_hide(wv->gtkWindow);
        }
    }
    return true;
}

bool WebViewFocus(WebView* wv) {
    if (!wv) {
        return false;
    }

    if (wv->hasX11) {
        ::Window focus = 0;
        int revert = 0;
        XGetInputFocus(wv->xdisplay, &focus, &revert);
        if (focus == wv->parentWindow || OwnsXFocus(wv)) {
            GiveXFocus(wv, CurrentTime);
        }
    }
    gtk_widget_grab_focus(GTK_WIDGET(wv->webview));
    return true;
}

bool WebViewFocusParent(WebView* wv) {
    if (!wv) {
        return false;
    }

    if (wv->hasX11 && OwnsXFocus(wv)) {
        XTrap trap(wv->display);
        XSetInputFocus(wv->xdisplay, wv->parentWindow, RevertToParent,
                       CurrentTime);
        XFlush(wv->xdisplay);
    }
    return true;
}

bool WebViewZoom(WebView* wv, double scaleFactor) {
    if (!wv) {
        return false;
    }
    webkit_web_view_set_zoom_level(wv->webview, scaleFactor);
    return true;
}

bool WebViewSetUserZoom(WebView* wv, double zoom) {
    if (!wv || !(zoom > 0)) {
        return false;
    }
    wv->userZoom = zoom;
    return WebViewMatchPageScale(wv, wv->matchedScale);
}

bool WebViewMatchPageScale(WebView* wv, float gpuiScale) {
    if (!wv || !wv->webview) {
        return false;
    }
    int gdk = gtk_widget_get_scale_factor(GTK_WIDGET(wv->webview));
    if (gdk < 1) {
        gdk = 1;
    }
    if (gpuiScale <= 0) {
        gpuiScale = 1;
    }
    wv->matchedScale = gpuiScale;

    return WebViewZoom(wv, (double)gpuiScale / (double)gdk * wv->userZoom);
}

void WebViewSetPageClick(WebView* wv, WebViewPageClick fn, void* user) {
    if (!wv) {
        return;
    }
    wv->pageClick = fn;
    wv->pageClickUser = user;
}

bool WebViewSetBackgroundColor(WebView* wv, Rgba color) {
    if (!wv) {
        return false;
    }
    GdkRGBA c = {color.r / 255.0, color.g / 255.0, color.b / 255.0,
                 color.a / 255.0};
    webkit_web_view_set_background_color(wv->webview, &c);
    return true;
}

bool WebViewSetTheme(WebView*, Theme theme) {
    GtkSettings* settings = gtk_settings_get_default();
    if (settings) {
        g_object_set(settings, "gtk-application-prefer-dark-theme",
                     theme == Theme::Dark, nullptr);
    }
    return true;
}
bool WebViewSetMemoryUsageLevel(WebView*, MemoryUsageLevel) {
    return false;
}
bool WebViewSetTrafficLightInset(WebView*, Position) {
    return false;
}

bool WebViewReparent(WebView* wv, void* parentWindow) {
    ::Window parent = (::Window)(uintptr_t)parentWindow;
    if (!wv || !wv->hasX11 || !parent || wv->followsParent) {
        return false;
    }
    XTrap trap(wv->display);
    XReparentWindow(wv->xdisplay, wv->x11Window, parent, 0, 0);
    XFlush(wv->xdisplay);
    wv->parentWindow = parent;
    return true;
}

bool WebViewPrint(WebView* wv) {
    if (!wv) {
        return false;
    }
    WebKitPrintOperation* op = webkit_print_operation_new(wv->webview);
    webkit_print_operation_run_dialog(op, nullptr);
    g_object_unref(op);
    return true;
}

bool WebViewClearAllBrowsingData(WebView* wv) {
    if (!wv) {
        return false;
    }
    WebKitWebsiteDataManager* manager =
        webkit_web_context_get_website_data_manager(wv->context);
    if (manager) {
        webkit_website_data_manager_clear(manager, WEBKIT_WEBSITE_DATA_ALL, 0,
                                          nullptr, nullptr, nullptr);
    }
    return true;
}

static WebKitCookieManager* CookieManagerOf(WebView* wv) {
    WebKitWebsiteDataManager* data =
        wv ? webkit_web_view_get_website_data_manager(wv->webview) : nullptr;
    return data ? webkit_website_data_manager_get_cookie_manager(data)
                : nullptr;
}

static Cookie CookieFromSoup(SoupCookie* c) {
    Cookie out;
    out.name = StrDup(Str(soup_cookie_get_name(c)));
    out.value = StrDup(Str(soup_cookie_get_value(c)));
    const char* domain = soup_cookie_get_domain(c);
    const char* path = soup_cookie_get_path(c);
    out.domain = domain ? StrDup(Str(domain)) : Str();
    out.path = path ? StrDup(Str(path)) : Str();
    out.hasHttpOnly = true;
    out.httpOnly = soup_cookie_get_http_only(c);
    out.hasSecure = true;
    out.secure = soup_cookie_get_secure(c);
    out.hasSameSite = true;
    SoupSameSitePolicy policy = soup_cookie_get_same_site_policy(c);
    out.sameSite =
        policy == SOUP_SAME_SITE_POLICY_LAX
            ? CookieSameSite::Lax
            : (policy == SOUP_SAME_SITE_POLICY_STRICT ? CookieSameSite::Strict
                                                      : kSameSiteNone);
    GDateTime* expires = soup_cookie_get_expires(c);
    if (expires) {
        out.session = false;

        int64_t t = g_date_time_to_unix(expires);
        if (t >= -377705116800LL && t <= 253402300799LL) {
            out.hasExpires = true;
            out.expiresUnixSeconds = t;
        }
    }
    return out;
}

static SoupCookie* CookieToSoup(const Cookie* c) {
    SoupCookie* out = soup_cookie_new(
        CStrTemp(c->name), CStrTemp(c->value), CStrTemp(c->domain),
        CStrTemp(c->path), c->hasMaxAge ? (int)c->maxAgeSeconds : -1);
    if (c->hasExpires) {
        GDateTime* dt = g_date_time_new_from_unix_utc(c->expiresUnixSeconds);
        if (dt) {
            soup_cookie_set_expires(out, dt);
            g_date_time_unref(dt);
        }
    }
    if (c->hasHttpOnly) {
        soup_cookie_set_http_only(out, c->httpOnly);
    }
    if (c->hasSameSite) {
        soup_cookie_set_same_site_policy(
            out, c->sameSite == CookieSameSite::Lax
                     ? SOUP_SAME_SITE_POLICY_LAX
                     : (c->sameSite == CookieSameSite::Strict
                            ? SOUP_SAME_SITE_POLICY_STRICT
                            : SOUP_SAME_SITE_POLICY_NONE));
    }
    if (c->hasSecure) {
        soup_cookie_set_secure(out, c->secure);
    }
    return out;
}

struct CookieWait {
    bool done = false;
    bool ok = false;
    GList* cookies = nullptr;
    bool all = false;
};

static void OnCookies(GObject* source, GAsyncResult* result, gpointer data) {
    CookieWait* wait = (CookieWait*)data;
    GError* error = nullptr;
#if WEBKIT_CHECK_VERSION(2, 42, 0)
    if (wait->all) {
        wait->cookies = webkit_cookie_manager_get_all_cookies_finish(
            WEBKIT_COOKIE_MANAGER(source), result, &error);
    } else
#endif
    {
        wait->cookies = webkit_cookie_manager_get_cookies_finish(
            WEBKIT_COOKIE_MANAGER(source), result, &error);
    }
    wait->ok = error == nullptr;
    if (error) {
        g_error_free(error);
    }
    wait->done = true;
}

static bool CookiesInner(WebView* wv, const char* uri, Vec<Cookie>* out) {
    if (!out) {
        return false;
    }
    CookieListFree(out);
    WebKitCookieManager* manager = CookieManagerOf(wv);
    if (!manager) {
        return false;
    }
    CookieWait wait;
    if (uri) {
        webkit_cookie_manager_get_cookies(manager, uri, nullptr, OnCookies,
                                          &wait);
    } else {
#if WEBKIT_CHECK_VERSION(2, 42, 0)
        wait.all = true;
        webkit_cookie_manager_get_all_cookies(manager, nullptr, OnCookies,
                                              &wait);
#else
        logf("wry: listing every cookie needs WebKitGTK 2.42\n");
        return false;
#endif
    }
    PumpUntil(&wait.done);
    for (GList* l = wait.cookies; l; l = l->next) {
        VecAppend(*out, CookieFromSoup((SoupCookie*)l->data));
    }
    g_list_free_full(wait.cookies, (GDestroyNotify)soup_cookie_free);
    return wait.ok;
}

bool WebViewCookies(WebView* wv, Vec<Cookie>* out) {
    return CookiesInner(wv, nullptr, out);
}

bool WebViewCookiesForUrl(WebView* wv, Str url, Vec<Cookie>* out) {
    return CookiesInner(wv, CStrTemp(url), out);
}

struct CookieChange {
    bool done = false;
    bool ok = false;
    bool add = true;
};

static void OnCookieChanged(GObject* source, GAsyncResult* result,
                            gpointer data) {
    CookieChange* change = (CookieChange*)data;
    GError* error = nullptr;
    gboolean ok = change->add
                      ? webkit_cookie_manager_add_cookie_finish(
                            WEBKIT_COOKIE_MANAGER(source), result, &error)
                      : webkit_cookie_manager_delete_cookie_finish(
                            WEBKIT_COOKIE_MANAGER(source), result, &error);
    change->ok = ok && !error;
    if (error) {
        g_error_free(error);
    }
    change->done = true;
}

static bool ChangeCookie(WebView* wv, const Cookie* cookie, bool add) {
    WebKitCookieManager* manager = CookieManagerOf(wv);
    if (!manager || !cookie) {
        return false;
    }
    SoupCookie* soup = CookieToSoup(cookie);
    CookieChange change;
    change.add = add;
    if (add) {
        webkit_cookie_manager_add_cookie(manager, soup, nullptr,
                                         OnCookieChanged, &change);
    } else {
        webkit_cookie_manager_delete_cookie(manager, soup, nullptr,
                                            OnCookieChanged, &change);
    }
    PumpUntil(&change.done);
    soup_cookie_free(soup);
    return change.ok;
}

bool WebViewSetCookie(WebView* wv, const Cookie* cookie) {
    return ChangeCookie(wv, cookie, true);
}

bool WebViewDeleteCookie(WebView* wv, const Cookie* cookie) {
    return ChangeCookie(wv, cookie, false);
}

void WebViewOpenDevtools(WebView* wv) {
    WebKitWebInspector* inspector =
        wv ? webkit_web_view_get_inspector(wv->webview) : nullptr;
    if (inspector) {
        webkit_web_inspector_show(inspector);

        wv->inspectorOpen = true;
    }
}

void WebViewCloseDevtools(WebView* wv) {
    WebKitWebInspector* inspector =
        wv ? webkit_web_view_get_inspector(wv->webview) : nullptr;
    if (inspector) {
        webkit_web_inspector_close(inspector);
    }
}

bool WebViewIsDevtoolsOpen(WebView* wv) {
    return wv && wv->inspectorOpen;
}

Str WebViewVersionTemp() {
    return base::FormatTemp("%d.%d.%d", (int)webkit_get_major_version(),
                            (int)webkit_get_minor_version(),
                            (int)webkit_get_micro_version());
}

bool WebViewAvailable() {
    return EnsureGtk();
}

}

#pragma GCC diagnostic pop

#endif

#endif

#if GPUI_OS_MAC
#line 1 "src/wry/wry_mac.cpp"

#import <Cocoa/Cocoa.h>

#import <WebKit/WKDownload.h>
#import <WebKit/WKDownloadDelegate.h>
#import <WebKit/WKHTTPCookieStore.h>
#import <WebKit/WKNavigationResponse.h>
#import <WebKit/WKFrameInfo.h>
#import <WebKit/WKNavigation.h>
#import <WebKit/WKNavigationAction.h>
#import <WebKit/WKNavigationDelegate.h>
#import <WebKit/WKOpenPanelParameters.h>
#import <WebKit/WKPreferences.h>
#import <WebKit/WKScriptMessage.h>
#import <WebKit/WKScriptMessageHandler.h>
#import <WebKit/WKSecurityOrigin.h>
#import <WebKit/WKUIDelegate.h>
#import <WebKit/WKURLSchemeHandler.h>
#import <WebKit/WKURLSchemeTask.h>
#import <WebKit/WKUserContentController.h>
#import <WebKit/WKUserScript.h>
#import <WebKit/WKWebView.h>
#import <WebKit/WKWebViewConfiguration.h>
#import <WebKit/WKWebsiteDataStore.h>
#import <WebKit/WKWindowFeatures.h>

@class GpuiWryScriptHandler;
@class GpuiWryNavigationDelegate;
@class GpuiWryDownloadDelegate;
@class GpuiWryUIDelegate;
@class GpuiWryTitleObserver;
@class GpuiWrySchemeHandler;
@class GpuiWryWebView;
@interface GpuiWryCookieResult : NSObject
@property(nonatomic, assign) BOOL done;
@property(nonatomic, strong) NSArray<NSHTTPCookie*>* cookies;
@end
@implementation GpuiWryCookieResult
@end

namespace wry {

using base::logf;
using base::Str;
using base::StrDup;
using base::StrFree;
using base::Vec;

static NSString* const kIpcHandlerName = @"ipc";

static NSString* const kIpcScript =
    @"Object.defineProperty(window, 'ipc', {\n"
    @"  value: Object.freeze({postMessage: function(s) "
    @"{window.webkit.messageHandlers.ipc.postMessage(s);}})\n"
    @"});";

struct WebView : WebViewHandlers {
    Str id = {};
    WKWebView* webview = nil;
    WKUserContentController* manager = nil;
    NSView* parentView = nil;
    bool isChild = false;
    bool visible = true;

    Vec<CustomProtocol> protocols;

    Vec<Str> pendingScripts;
    bool pendingOpen = true;

    NSMutableSet* liveTasks = nil;

    GpuiWryScriptHandler* ipcDelegate = nil;
    GpuiWryNavigationDelegate* navDelegate = nil;
    GpuiWryDownloadDelegate* downloadDelegate = nil;
    GpuiWryUIDelegate* uiDelegate = nil;
    GpuiWryTitleObserver* titleObserver = nil;
    NSMutableArray* schemeHandlers = nil;
};

struct RequestResponder {

    NSMutableSet* liveTasks = nil;
    id<WKURLSchemeTask> task = nil;
    NSURL* url = nil;
    int32_t answered = 0;
};

static NSString* ToNS(Str s) {
    if (!s.s || len(s) == 0) {
        return @"";
    }
    NSString* res = [[NSString alloc] initWithBytes:s.s
                                             length:(NSUInteger)len(s)
                                           encoding:NSUTF8StringEncoding];
    return res ? res : @"";
}

static Str FromNSTemp(NSString* s) {
    return s ? base::StrDupTemp(Str([s UTF8String])) : Str();
}

static Str UrlFromWebView(WKWebView* webview) {
    NSURL* url = webview.URL;
    return url ? FromNSTemp(url.absoluteString) : Str();
}

static void FlushPendingScripts(WebView* wv);
static void HandleSchemeTask(WebView* wv, int index, id<WKURLSchemeTask> task);

}

@interface GpuiWryWebView : WKWebView
@property(nonatomic, assign) wry::WebView* wv;
@property(nonatomic, assign) BOOL childWebView;
@property(nonatomic, assign) BOOL acceptFirstMouseEnabled;
@end

@implementation GpuiWryWebView

- (BOOL)emitDrag:(id<NSDraggingInfo>)sender kind:(wry::DragDropKind)kind {
    wry::WebView* wv = self.wv;
    if (!wv || !wv->dragDropHandler) return NO;
    wry::Vec<wry::Str> paths;
    if (kind == wry::DragDropKind::Enter || kind == wry::DragDropKind::Drop) {
        NSArray* files = [sender.draggingPasteboard
            propertyListForType:NSFilenamesPboardType];
        if ([files isKindOfClass:[NSArray class]]) {
            for (id file in files) {
                if ([file isKindOfClass:[NSString class]])
                    VecAppend(paths, wry::StrDup(wry::FromNSTemp(file)));
            }
        }
    }
    wry::DragDropEvent event;
    event.kind = kind;
    event.paths = paths.els;
    event.pathCount = len(paths);
    if (kind != wry::DragDropKind::Leave) {

        NSPoint point =
            [self convertPoint:sender.draggingLocation fromView:nil];
        event.x = (int32_t)point.x;
        event.y = (int32_t)(self.isFlipped ? point.y
                                           : self.bounds.size.height - point.y);
    }
    bool handled = wv->dragDropHandler(wv->ctx, &event);
    for (int i = 0; i < len(paths); i++) wry::StrFree(paths[i]);
    return handled ? YES : NO;
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    if ([self emitDrag:sender kind:wry::DragDropKind::Enter])
        return NSDragOperationCopy;
    return [super draggingEntered:sender];
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    if ([self emitDrag:sender kind:wry::DragDropKind::Over])
        return NSDragOperationCopy;
    NSDragOperation operation = [super draggingUpdated:sender];

    return operation == NSDragOperationNone ? NSDragOperationCopy : operation;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    if ([self emitDrag:sender kind:wry::DragDropKind::Drop]) return YES;
    return [super performDragOperation:sender];
}
- (void)draggingExited:(id<NSDraggingInfo>)sender {
    if (![self emitDrag:sender kind:wry::DragDropKind::Leave])
        [super draggingExited:sender];
}

- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if (self.childWebView) {
        return NO;
    }
    return [super performKeyEquivalent:event];
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return self.acceptFirstMouseEnabled;
}

- (NSString*)syntheticMouseScript:(NSEvent*)event
                             down:(BOOL)down
                             back:(BOOL)back {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger x = p.x < 0 ? 0 : (NSUInteger)p.x;
    NSUInteger y = p.y < 0 ? 0 : (NSUInteger)p.y;
    NSUInteger buttons = [NSEvent pressedMouseButtons];
    NSEventModifierFlags mods = event.modifierFlags;
    return [NSString
        stringWithFormat:
            @"(() => { const el = document.elementFromPoint(%lu,%lu);"
             "if (!el) return; const ev = new MouseEvent('%@', {"
             "view:window,button:%d,buttons:%lu,x:%lu,y:%lu,bubbles:true,"
             "detail:%ld,cancelBubble:false,cancelable:true,clientX:%lu,"
             "clientY:%lu,composed:true,layerX:%lu,layerY:%lu,pageX:%lu,"
             "pageY:%lu,screenX:window.screenX+%lu,screenY:window.screenY+%lu,"
             "ctrlKey:%s,metaKey:%s,shiftKey:%s,altKey:%s});"
             "el.dispatchEvent(ev); if (!ev.defaultPrevented && '%@' === "
             "'mouseup') { if (ev.button === 3) history.back();"
             "if (ev.button === 4) history.forward(); } })()",
            (unsigned long)x, (unsigned long)y,
            down ? @"mousedown" : @"mouseup", back ? 3 : 4,
            (unsigned long)buttons, (unsigned long)x, (unsigned long)y,
            (long)event.clickCount, (unsigned long)x, (unsigned long)y,
            (unsigned long)x, (unsigned long)y, (unsigned long)x,
            (unsigned long)y, (unsigned long)x, (unsigned long)y,
            (mods & NSEventModifierFlagControl) ? "true" : "false",
            (mods & NSEventModifierFlagCommand) ? "true" : "false",
            (mods & NSEventModifierFlagShift) ? "true" : "false",
            (mods & NSEventModifierFlagOption) ? "true" : "false",
            down ? @"mousedown" : @"mouseup"];
}

- (void)otherMouseDown:(NSEvent*)event {
    NSInteger button = event.buttonNumber;
    if (event.type == NSEventTypeOtherMouseDown &&
        (button == 3 || button == 4)) {
        [self evaluateJavaScript:[self syntheticMouseScript:event
                                                       down:YES
                                                       back:button == 3]
               completionHandler:nil];
        return;
    }
    [self mouseDown:event];
}

- (void)otherMouseUp:(NSEvent*)event {
    NSInteger button = event.buttonNumber;
    if (event.type == NSEventTypeOtherMouseUp && (button == 3 || button == 4)) {
        [self evaluateJavaScript:[self syntheticMouseScript:event
                                                       down:NO
                                                       back:button == 3]
               completionHandler:nil];
        return;
    }
    [self mouseUp:event];
}
@end

@interface GpuiWryScriptHandler : NSObject <WKScriptMessageHandler>
@property(nonatomic, assign) wry::WebView* wv;
@end

@implementation GpuiWryScriptHandler
- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message {
    (void)controller;
    wry::WebView* wv = self.wv;
    if (!wv || !wv->ipcHandler) {
        return;
    }
    if (![message.body isKindOfClass:[NSString class]]) {

        return;
    }
    NSString* body = (NSString*)message.body;
    NSURL* url = message.frameInfo.request.URL;
    wv->ipcHandler(wv->ctx,
                   url ? wry::FromNSTemp(url.absoluteString) : wry::Str(),
                   wry::FromNSTemp(body));
}
@end

@interface GpuiWryTitleObserver : NSObject
@property(nonatomic, assign) wry::WebView* wv;
@end

@implementation GpuiWryTitleObserver
- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context {
    (void)change;
    (void)context;
    wry::WebView* wv = self.wv;
    if (!wv || !wv->documentTitleChangedHandler ||
        ![keyPath isEqualToString:@"title"]) {
        return;
    }
    NSString* title = [object title];
    wv->documentTitleChangedHandler(wv->ctx, wry::FromNSTemp(title));
}
@end

API_AVAILABLE(macos(11.3))
@interface GpuiWryDownloadDelegate : NSObject <WKDownloadDelegate>
@property(nonatomic, assign) wry::WebView* wv;
@property(nonatomic, strong) NSMutableSet* downloads;
@end

@implementation GpuiWryDownloadDelegate
- (void)download:(WKDownload*)download
    decideDestinationUsingResponse:(NSURLResponse*)response
                 suggestedFilename:(NSString*)filename
                 completionHandler:(void (^)(NSURL*))handler
    API_AVAILABLE(macos(11.3)) {
    (void)response;
    wry::WebView* wv = self.wv;
    if (!wv || !wv->downloadStartedHandler) {
        handler(nil);
        return;
    }
    NSFileManager* files = [NSFileManager defaultManager];
    NSURL* directory = [[files URLsForDirectory:NSDownloadsDirectory
                                      inDomains:NSUserDomainMask] firstObject];
    if (!directory)
        directory =
            [NSURL fileURLWithPath:files.currentDirectoryPath isDirectory:YES];
    NSString* destination =
        [directory.path stringByAppendingPathComponent:filename];
    int counter = 1;
    while ([files fileExistsAtPath:destination]) {
        destination = [directory.path
            stringByAppendingPathComponent:wry::ToNS(
                                               wry::MacDownloadFileNameTemp(
                                                   wry::FromNSTemp(filename),
                                                   counter++))];
    }
    wry::Str path = wry::FromNSTemp(destination);
    wry::Str url = wry::FromNSTemp(download.originalRequest.URL.absoluteString);
    bool allowed = wv->downloadStartedHandler(wv->ctx, url, &path);
    handler(allowed && self.wv ? [NSURL fileURLWithPath:wry::ToNS(path)] : nil);
}
- (void)complete:(WKDownload*)download
         success:(BOOL)success API_AVAILABLE(macos(11.3)) {
    [self.downloads removeObject:download];
    download.delegate = nil;
    wry::WebView* wv = self.wv;
    if (wv && wv->downloadCompletedHandler) {

        wv->downloadCompletedHandler(
            wv->ctx,
            wry::FromNSTemp(download.originalRequest.URL.absoluteString),
            nullptr, success);
    }
}
- (void)downloadDidFinish:(WKDownload*)download API_AVAILABLE(macos(11.3)) {
    [self complete:download success:YES];
}
- (void)download:(WKDownload*)download
    didFailWithError:(NSError*)error
          resumeData:(NSData*)resumeData API_AVAILABLE(macos(11.3)) {
    (void)error;
    (void)resumeData;
    [self complete:download success:NO];
}
@end

@interface GpuiWryNavigationDelegate : NSObject <WKNavigationDelegate>
@property(nonatomic, assign) wry::WebView* wv;
@end

@implementation GpuiWryNavigationDelegate
- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:
                        (void (^)(WKNavigationActionPolicy))handler {
    (void)webView;
    wry::WebView* wv = self.wv;
    if (@available(macOS 11.3, *)) {
        if (action.shouldPerformDownload) {
            handler(wv && wv->downloadStartedHandler
                        ? WKNavigationActionPolicyDownload
                        : WKNavigationActionPolicyCancel);
            return;
        }
    }
    if (!wv || !wv->navigationHandler) {
        handler(WKNavigationActionPolicyAllow);
        return;
    }
    NSURL* url = action.request.URL;
    bool allow = wv->navigationHandler(
        wv->ctx, url ? wry::FromNSTemp(url.absoluteString) : wry::Str());
    handler(allow ? WKNavigationActionPolicyAllow
                  : WKNavigationActionPolicyCancel);
}

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationResponse:(WKNavigationResponse*)response
                      decisionHandler:
                          (void (^)(WKNavigationResponsePolicy))handler {
    (void)webView;
    if (@available(macOS 11.3, *)) {
        if (!response.canShowMIMEType && self.wv &&
            self.wv->downloadStartedHandler) {
            handler(WKNavigationResponsePolicyDownload);
            return;
        }
    }
    handler(WKNavigationResponsePolicyAllow);
}

- (void)attachDownload:(WKDownload*)download API_AVAILABLE(macos(11.3)) {
    if (self.wv && self.wv->downloadDelegate) {
        [self.wv->downloadDelegate.downloads addObject:download];
        download.delegate = self.wv->downloadDelegate;
    } else {
        [download cancel:^(NSData* data) {
          (void)data;
        }];
    }
}
- (void)webView:(WKWebView*)webView
     navigationAction:(WKNavigationAction*)action
    didBecomeDownload:(WKDownload*)download API_AVAILABLE(macos(11.3)) {
    (void)webView;
    (void)action;
    [self attachDownload:download];
}
- (void)webView:(WKWebView*)webView
    navigationResponse:(WKNavigationResponse*)response
     didBecomeDownload:(WKDownload*)download API_AVAILABLE(macos(11.3)) {
    (void)webView;
    (void)response;
    [self attachDownload:download];
}

- (void)webView:(WKWebView*)webView
    didCommitNavigation:(WKNavigation*)navigation {
    (void)navigation;
    wry::WebView* wv = self.wv;
    if (!wv) {
        return;
    }
    if (wv->onPageLoadHandler) {
        wv->onPageLoadHandler(wv->ctx, wry::PageLoadEvent::Started,
                              wry::UrlFromWebView(webView));
    }
    wry::FlushPendingScripts(wv);
}

- (void)webView:(WKWebView*)webView
    didFinishNavigation:(WKNavigation*)navigation {
    (void)navigation;
    wry::WebView* wv = self.wv;
    if (wv && wv->onPageLoadHandler) {
        wv->onPageLoadHandler(wv->ctx, wry::PageLoadEvent::Finished,
                              wry::UrlFromWebView(webView));
    }
}
@end

@interface GpuiWryNewWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, weak) GpuiWryUIDelegate* owner;
@property(nonatomic, weak) NSWindow* window;
@end

@interface GpuiWryUIDelegate : NSObject <WKUIDelegate>
@property(nonatomic, assign) wry::WebView* wv;

@property(nonatomic, strong) NSMutableArray* openedWindows;
- (void)forgetWindow:(NSWindow*)window;
@end

@implementation GpuiWryNewWindowDelegate
- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    [self.owner forgetWindow:self.window];
}
@end

@implementation GpuiWryUIDelegate
- (void)forgetWindow:(NSWindow*)window {
    if (!window || !self.openedWindows) {
        return;
    }
    for (NSUInteger i = 0; i < self.openedWindows.count; i++) {
        NSArray* entry = self.openedWindows[i];
        if (entry[0] == window) {
            [self.openedWindows removeObjectAtIndex:i];
            return;
        }
    }
}

- (void)webView:(WKWebView*)webView
    runOpenPanelWithParameters:(WKOpenPanelParameters*)parameters
              initiatedByFrame:(WKFrameInfo*)frame
             completionHandler:(void (^)(NSArray<NSURL*>*))handler {
    (void)webView;
    (void)frame;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.allowsMultipleSelection = parameters.allowsMultipleSelection;
    panel.canChooseDirectories = parameters.allowsDirectories;
    if ([panel runModal] == NSModalResponseOK) {
        handler(panel.URLs);
    } else {
        handler(nil);
    }
}

- (void)webView:(WKWebView*)webView
    requestMediaCapturePermissionForOrigin:(WKSecurityOrigin*)origin
                          initiatedByFrame:(WKFrameInfo*)frame
                                      type:(WKMediaCaptureType)type
                           decisionHandler:
                               (void (^)(WKPermissionDecision))handler {
    (void)webView;
    (void)origin;
    (void)frame;
    (void)type;
    handler(WKPermissionDecisionGrant);
}

- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)action
                    windowFeatures:(WKWindowFeatures*)windowFeatures {
    wry::WebView* wv = self.wv;
    if (!wv || !wv->newWindowReqHandler) {
        return nil;
    }
    NSURL* url = action.request.URL;
    wry::NewWindowFeatures features;
    features.opener = wv;
    features.targetConfiguration = (__bridge void*)configuration;
    if (windowFeatures.x && windowFeatures.y) {
        features.hasPosition = true;
        features.x = windowFeatures.x.doubleValue;
        features.y = windowFeatures.y.doubleValue;
    }
    if (windowFeatures.width && windowFeatures.height) {
        features.hasSize = true;
        features.width = windowFeatures.width.doubleValue;
        features.height = windowFeatures.height.doubleValue;
    }

    wry::WebView* created = nullptr;
    wry::NewWindowResponse response = wv->newWindowReqHandler(
        wv->ctx, url ? wry::FromNSTemp(url.absoluteString) : wry::Str(),
        &features, &created);
    if (response == wry::NewWindowResponse::Deny) {
        return nil;
    }
    if (response == wry::NewWindowResponse::Create) {
        return created ? created->webview : nil;
    }

    NSWindow* current = webView.window;
    NSRect defaults = current ? current.frame : NSMakeRect(0, 0, 800, 600);
    NSSize size =
        NSMakeSize(features.hasSize ? features.width : defaults.size.width,
                   features.hasSize ? features.height : defaults.size.height);
    NSPoint origin = defaults.origin;
    if (features.hasPosition) {
        NSScreen* screen = current ? current.screen : [NSScreen mainScreen];
        CGFloat screenHeight = screen ? screen.frame.size.height : size.height;
        origin =
            NSMakePoint(features.x, screenHeight - features.y - size.height);
    }

    NSWindowStyleMask mask = NSWindowStyleMaskTitled |
                             NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable;
    bool resizable = windowFeatures.allowsResizing
                         ? windowFeatures.allowsResizing.boolValue
                         : true;
    if (resizable) {
        mask |= NSWindowStyleMaskResizable;
    }

    NSRect rect = NSMakeRect(origin.x, origin.y, size.width, size.height);
    NSWindow* window =
        [[NSWindow alloc] initWithContentRect:rect
                                    styleMask:mask
                                      backing:NSBackingStoreBuffered
                                        defer:NO];

    window.releasedWhenClosed = NO;

    WKWebView* child = [[WKWebView alloc] initWithFrame:window.frame
                                          configuration:configuration];
    GpuiWryNewWindowDelegate* delegate =
        [[GpuiWryNewWindowDelegate alloc] init];
    delegate.owner = self;
    delegate.window = window;
    window.delegate = delegate;
    window.contentView = child;
    [window makeKeyAndOrderFront:nil];

    if (!self.openedWindows) {
        self.openedWindows = [NSMutableArray array];
    }
    [self.openedWindows addObject:@[ window, child, delegate ]];
    return child;
}
@end

@interface GpuiWrySchemeHandler : NSObject <WKURLSchemeHandler>
@property(nonatomic, assign) wry::WebView* wv;
@property(nonatomic, assign) int index;
@end

@implementation GpuiWrySchemeHandler
- (void)webView:(WKWebView*)webView
    startURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void)webView;
    if (self.wv) {
        wry::HandleSchemeTask(self.wv, self.index, task);
    }
}

- (void)webView:(WKWebView*)webView
    stopURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void)webView;
    wry::WebView* wv = self.wv;
    if (wv && wv->liveTasks) {
        [wv->liveTasks
            removeObject:[NSValue valueWithPointer:(__bridge const void*)task]];
    }
}
@end

namespace wry {

static void AddUserScript(WebView* wv, Str js, bool forMainFrameOnly) {
    WKUserScript* script = [[WKUserScript alloc]
          initWithSource:ToNS(js)
           injectionTime:WKUserScriptInjectionTimeAtDocumentStart
        forMainFrameOnly:forMainFrameOnly ? YES : NO];
    [wv->manager addUserScript:script];
}

static void FlushPendingScripts(WebView* wv) {
    if (!wv->pendingOpen) {
        return;
    }
    for (int i = 0; i < wv->pendingScripts.len; i++) {
        [wv->webview evaluateJavaScript:ToNS(wv->pendingScripts[i])
                      completionHandler:nil];
        StrFree(wv->pendingScripts[i]);
    }
    VecReset(wv->pendingScripts);
    wv->pendingOpen = false;
}

static void HandleSchemeTask(WebView* wv, int index, id<WKURLSchemeTask> task) {
    if (index < 0 || index >= wv->protocols.len) {
        return;
    }
    NSURLRequest* request = task.request;
    NSURL* url = request.URL;
    if (!url) {
        return;
    }
    if (!wv->liveTasks) {
        wv->liveTasks = [NSMutableSet set];
    }
    [wv->liveTasks
        addObject:[NSValue valueWithPointer:(__bridge const void*)task]];

    Vec<Header> headerStore;
    NSDictionary<NSString*, NSString*>* all = request.allHTTPHeaderFields;
    for (NSString* name in all) {
        Header h;
        h.name = FromNSTemp(name);
        h.value = FromNSTemp(all[name]);
        VecAppend(headerStore, h);
    }

    Vec<uint8_t> bodyStore;
    NSData* body = request.HTTPBody;
    if (body) {
        uint8_t* dst = VecAppendBlanks(bodyStore, (int)body.length);
        if (dst) {
            memcpy(dst, body.bytes, body.length);
        }
    } else if (request.HTTPBodyStream) {
        NSInputStream* stream = request.HTTPBodyStream;
        [stream open];
        uint8_t buf[1024];
        while (stream.hasBytesAvailable) {
            NSInteger got = [stream read:buf maxLength:sizeof(buf)];
            if (got <= 0) {
                break;
            }
            uint8_t* dst = VecAppendBlanks(bodyStore, (int)got);
            if (!dst) {
                break;
            }
            memcpy(dst, buf, (size_t)got);
        }
        [stream close];
    }

    Request req;
    req.method = FromNSTemp(request.HTTPMethod ? request.HTTPMethod : @"GET");
    req.uri = FromNSTemp(url.absoluteString);
    req.headers = len(headerStore) > 0 ? &headerStore[0] : nullptr;
    req.headerCount = len(headerStore);
    req.body = len(bodyStore) > 0 ? &bodyStore[0] : nullptr;
    req.bodyLen = len(bodyStore);

    RequestResponder* responder = new RequestResponder();
    responder->liveTasks = wv->liveTasks;
    responder->task = task;
    responder->url = url;

    CustomProtocolDispatch(wv->protocols[index], wv->id, &req, responder);

    VecReset(headerStore);
    VecReset(bodyStore);
}

static void DeliverResponse(RequestResponder* responder,
                            NSHTTPURLResponse* response, NSData* data) {
    NSValue* key =
        [NSValue valueWithPointer:(__bridge const void*)responder->task];
    if (responder->liveTasks && [responder->liveTasks containsObject:key]) {
        @try {
            [responder->task didReceiveResponse:response];
            [responder->task didReceiveData:data];
            [responder->task didFinish];
        } @catch (NSException* e) {
            (void)e;
            logf(
                "wry: the custom protocol task went away before it was "
                "answered\n");
        }
        [responder->liveTasks removeObject:key];
    }
    responder->liveTasks = nil;
    responder->task = nil;
    responder->url = nil;
    delete responder;
}

void Respond(RequestResponder* responder, const Response* response) {
    if (!responder) {
        return;
    }
    if (!__sync_bool_compare_and_swap(&responder->answered, 0, 1)) {
        logf("wry: a custom protocol request was answered twice\n");
        return;
    }

    int status = response ? response->status : 500;
    int bodyLen = (response && response->body) ? response->bodyLen : 0;
    NSMutableDictionary* headers = [NSMutableDictionary dictionary];

    headers[@"Content-Length"] = [NSString stringWithFormat:@"%d", bodyLen];
    if (response) {
        for (int i = 0; i < response->headerCount; i++) {
            headers[ToNS(response->headers[i].name)] =
                ToNS(response->headers[i].value);
        }
    }
    NSData* data = bodyLen > 0 ? [NSData dataWithBytes:response->body
                                                length:(NSUInteger)bodyLen]
                               : [NSData data];
    NSHTTPURLResponse* http =
        [[NSHTTPURLResponse alloc] initWithURL:responder->url
                                    statusCode:status
                                   HTTPVersion:@"HTTP/1.1"
                                  headerFields:headers];
    if (!http) {
        logf(
            "wry: could not build the response for a custom protocol "
            "request\n");
        if (responder->liveTasks && responder->task) {
            NSValue* key = [NSValue
                valueWithPointer:(__bridge const void*)responder->task];
            [responder->liveTasks removeObject:key];
        }
        delete responder;
        return;
    }

    if ([NSThread isMainThread]) {
        DeliverResponse(responder, http, data);
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      DeliverResponse(responder, http, data);
    });
}

static NSPoint WindowPosition(NSView* view, double x, double y, double height) {
    if (view.isFlipped) {
        return NSMakePoint(x, y);
    }
    return NSMakePoint(x, view.frame.size.height - y - height);
}

static double ToLogical(double value, bool logical, double scale) {
    return logical ? value : value / scale;
}

static double ScaleFactor(NSView* view) {
    NSWindow* window = view.window;
    return window ? window.backingScaleFactor : 1.0;
}

static void SetTrafficLightInset(NSWindow* window, Position position) {
    if (!window) {
        return;
    }
    double scale = window.backingScaleFactor;
    double x = ToLogical(position.x, position.logical, scale);
    double y = ToLogical(position.y, position.logical, scale);
    NSButton* close = [window standardWindowButton:NSWindowCloseButton];
    NSButton* mini = [window standardWindowButton:NSWindowMiniaturizeButton];
    NSButton* zoom = [window standardWindowButton:NSWindowZoomButton];
    NSView* container = close.superview.superview;
    if (!close || !mini || !container) {
        return;
    }

    NSRect closeFrame = close.frame;
    CGFloat titleHeight = closeFrame.size.height + y;
    NSRect titleFrame = container.frame;
    titleFrame.size.height = titleHeight;
    titleFrame.origin.y = window.frame.size.height - titleHeight;
    container.frame = titleFrame;

    CGFloat spacing = mini.frame.origin.x - closeFrame.origin.x;
    closeFrame.origin.x = x;
    close.frame = closeFrame;
    NSRect miniFrame = mini.frame;
    miniFrame.origin.x = x + spacing;
    mini.frame = miniFrame;
    if (zoom) {
        NSRect zoomFrame = zoom.frame;
        zoomFrame.origin.x = x + 2 * spacing;
        zoom.frame = zoomFrame;
    }
}

WebView* WebViewNew(void* parentWindow, const WebViewAttributes* attrs,
                    bool asChild) {
    if (!parentWindow || !attrs) {
        return nullptr;
    }
    if (![NSThread isMainThread]) {

        logf("wry: a webview can only be made on the main thread\n");
        return nullptr;
    }
    NSView* parentView = (__bridge NSView*)parentWindow;

    bool usingExistingConfig = attrs->webviewConfiguration != nullptr;
    WKWebViewConfiguration* config =
        usingExistingConfig
            ? (__bridge WKWebViewConfiguration*)attrs->webviewConfiguration
            : [[WKWebViewConfiguration alloc] init];
    if (!usingExistingConfig) {
        if (attrs->incognito) {
            config
                .websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
        } else if (attrs->hasDataStoreIdentifier) {
            if (@available(macOS 14.0, *)) {
                NSUUID* identifier = [[NSUUID alloc]
                    initWithUUIDBytes:attrs->dataStoreIdentifier];
                config.websiteDataStore =
                    [WKWebsiteDataStore dataStoreForIdentifier:identifier];
            } else {
                config.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
            }
        } else {
            config.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
        }
    }

    WebView* wv = new WebView();
    wv->parentView = parentView;
    wv->isChild = asChild;
    wv->visible = attrs->visible;
    WebViewHandlersSet(wv, attrs);
    wv->liveTasks = [NSMutableSet set];
    wv->schemeHandlers = [NSMutableArray array];

    static int nextId = 1;
    wv->id = len(attrs->id) > 0 ? StrDup(attrs->id)
                                : StrDup(base::FormatTemp("%d", nextId++));

    for (int i = 0; i < attrs->customProtocolCount; i++) {
        NSString* scheme = ToNS(attrs->customProtocols[i].name);
        if (usingExistingConfig &&
            [config urlSchemeHandlerForURLScheme:scheme]) {
            continue;
        }
        int protocolIndex = CustomProtocolsAppendCopy(
            &wv->protocols, attrs->customProtocols[i]);

        GpuiWrySchemeHandler* handler = [[GpuiWrySchemeHandler alloc] init];
        handler.wv = wv;
        handler.index = protocolIndex;
        [wv->schemeHandlers addObject:handler];

        @try {
            [config setURLSchemeHandler:handler forURLScheme:scheme];
        } @catch (NSException* e) {
            (void)e;
            logf("wry: could not register the custom protocol '%s'\n",
                 wv->protocols[protocolIndex].name);
            WebViewFree(wv);
            return nullptr;
        }
    }

    WKPreferences* preferences = config.preferences;
    if (attrs->javascriptDisabled) {
        config.defaultWebpagePreferences.allowsContentJavaScript = NO;
    }
    if (attrs->autoplay) {
        config.mediaTypesRequiringUserActionForPlayback =
            WKAudiovisualMediaTypeNone;
    }
    if (attrs->hasBackgroundThrottling) {
        if (@available(macOS 14.0, *)) {

            int policy = 2;
            if (attrs->backgroundThrottling ==
                BackgroundThrottlingPolicy::Suspend) {
                policy = 0;
            } else if (attrs->backgroundThrottling ==
                       BackgroundThrottlingPolicy::Throttle) {
                policy = 1;
            }
            [preferences setValue:@(policy) forKey:@"inactiveSchedulingPolicy"];
        }
    }
    if (attrs->transparent) {
        [config setValue:@NO forKey:@"drawsBackground"];
    }
    [preferences setValue:@YES forKey:@"allowsPictureInPictureMediaPlayback"];
    [preferences setValue:@YES forKey:@"tabFocusesLinks"];

    double scale = ScaleFactor(parentView);
    NSRect frame;
    if (asChild && attrs->hasBounds) {
        double x = ToLogical(attrs->bounds.position.x,
                             attrs->bounds.position.logical, scale);
        double y = ToLogical(attrs->bounds.position.y,
                             attrs->bounds.position.logical, scale);
        double w = ToLogical(attrs->bounds.size.width,
                             attrs->bounds.size.logical, scale);
        double h = ToLogical(attrs->bounds.size.height,
                             attrs->bounds.size.logical, scale);
        frame.origin = WindowPosition(parentView, x, y, h);
        frame.size = NSMakeSize(w, h);
    } else {
        frame = parentView.bounds;
    }

    GpuiWryWebView* webview =
        [[GpuiWryWebView alloc] initWithFrame:frame configuration:config];
    webview.wv = wv;
    if (wv->dragDropHandler)
        [webview registerForDraggedTypes:@[ NSFilenamesPboardType ]];
    webview.childWebView = asChild ? YES : NO;
    webview.acceptFirstMouseEnabled = attrs->acceptFirstMouse ? YES : NO;
    wv->webview = webview;
    wv->manager = config.userContentController;

    if (asChild) {

        wv->webview.autoresizingMask = NSViewMinYMargin;
    } else {
        wv->webview.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    wv->webview.allowsBackForwardNavigationGestures =
        attrs->backForwardNavigationGestures ? YES : NO;
    wv->webview.allowsLinkPreview = attrs->allowLinkPreview ? YES : NO;
    if (!attrs->visible) {
        wv->webview.hidden = YES;
    }
    if (attrs->devtools) {
        if ([wv->webview respondsToSelector:@selector(setInspectable:)]) {
            wv->webview.inspectable = YES;
        }

        [preferences setValue:@YES forKey:@"developerExtrasEnabled"];
    }

    AddUserScript(wv, Str((char*)[kIpcScript UTF8String]), true);
    if (attrs->ipcHandler) {
        wv->ipcDelegate = [[GpuiWryScriptHandler alloc] init];
        wv->ipcDelegate.wv = wv;
        @try {
            [wv->manager addScriptMessageHandler:wv->ipcDelegate
                                            name:kIpcHandlerName];
        } @catch (NSException* e) {
            (void)e;
            logf("wry: could not install the ipc message handler\n");
        }
    }
    for (int i = 0; i < attrs->initializationScriptCount; i++) {
        AddUserScript(wv, attrs->initializationScripts[i].script,
                      attrs->initializationScripts[i].forMainFrameOnly);
    }

    if (attrs->documentTitleChangedHandler) {
        wv->titleObserver = [[GpuiWryTitleObserver alloc] init];
        wv->titleObserver.wv = wv;
        [wv->webview addObserver:wv->titleObserver
                      forKeyPath:@"title"
                         options:NSKeyValueObservingOptionNew
                         context:nullptr];
    }

    if (@available(macOS 11.3, *)) {
        if (wv->downloadStartedHandler || wv->downloadCompletedHandler) {
            wv->downloadDelegate = [[GpuiWryDownloadDelegate alloc] init];
            wv->downloadDelegate.wv = wv;
            wv->downloadDelegate.downloads = [NSMutableSet set];
        }
    }
    wv->navDelegate = [[GpuiWryNavigationDelegate alloc] init];
    wv->navDelegate.wv = wv;
    wv->webview.navigationDelegate = wv->navDelegate;

    wv->uiDelegate = [[GpuiWryUIDelegate alloc] init];
    wv->uiDelegate.wv = wv;
    wv->webview.UIDelegate = wv->uiDelegate;

    if (len(attrs->userAgent) > 0) {
        wv->webview.customUserAgent = ToNS(attrs->userAgent);
    }

    if (len(attrs->url) > 0) {
        NSURL* url = [NSURL URLWithString:ToNS(attrs->url)];
        if (url) {
            NSMutableURLRequest* request =
                [NSMutableURLRequest requestWithURL:url];
            for (int i = 0; i < attrs->headerCount; i++) {
                [request addValue:ToNS(attrs->headers[i].value)
                    forHTTPHeaderField:ToNS(attrs->headers[i].name)];
            }
            [wv->webview loadRequest:request];
        } else {
            logf("wry: the url could not be parsed\n");
        }
    } else if (len(attrs->html) > 0) {
        [wv->webview loadHTMLString:ToNS(attrs->html) baseURL:nil];
    }

    [parentView addSubview:wv->webview];
    if (!asChild) {
        wv->webview.frame = parentView.bounds;
        NSWindow* window = parentView.window;
        if (window) {
            [window makeFirstResponder:wv->webview];
        }
    }

    (void)attrs->focused;
    NSWindow* window = parentView.window;
    if (window &&
        [window respondsToSelector:@selector(setTitlebarSeparatorStyle:)]) {
        window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    }
    if (!asChild && attrs->hasTrafficLightInset) {
        SetTrafficLightInset(window, attrs->trafficLightInset);
    }
    if (@available(macOS 14.0, *)) {
        [NSApp activate];
    } else {
        [NSApp activateIgnoringOtherApps:YES];
    }
    return wv;
}

void WebViewFree(WebView* wv) {
    if (!wv) {
        return;
    }
    if (wv->ipcDelegate && wv->manager) {
        [wv->manager removeScriptMessageHandlerForName:kIpcHandlerName];
    }
    if (wv->titleObserver && wv->webview) {
        [wv->webview removeObserver:wv->titleObserver forKeyPath:@"title"];
    }
    if (wv->webview) {
        wv->webview.navigationDelegate = nil;
        wv->webview.UIDelegate = nil;
        [wv->webview removeFromSuperview];
    }

    ((GpuiWryWebView*)wv->webview).wv = nullptr;
    if (@available(macOS 11.3, *)) {
        wv->downloadDelegate.wv = nullptr;
        for (WKDownload* download in wv->downloadDelegate.downloads) {
            download.delegate = nil;
            [download cancel:^(NSData* data) {
              (void)data;
            }];
        }
        [wv->downloadDelegate.downloads removeAllObjects];
    }
    wv->ipcDelegate.wv = nullptr;
    wv->navDelegate.wv = nullptr;
    wv->uiDelegate.wv = nullptr;
    wv->titleObserver.wv = nullptr;
    for (GpuiWrySchemeHandler* handler in wv->schemeHandlers) {
        handler.wv = nullptr;
    }
    [wv->liveTasks removeAllObjects];
    CustomProtocolsFree(&wv->protocols);
    StrVecFree(&wv->pendingScripts);
    StrFree(wv->id);

    delete wv;
}

Str WebViewId(WebView* wv) {
    return wv ? wv->id : Str();
}

bool WebViewEval(WebView* wv, Str js) {
    if (!wv) {
        return false;
    }

    if (wv->pendingOpen) {
        VecAppend(wv->pendingScripts, StrDup(js));
        return true;
    }
    [wv->webview evaluateJavaScript:ToNS(js) completionHandler:nil];
    return true;
}

bool WebViewEvalWithCallback(WebView* wv, Str js, void* ctx,
                             void (*callback)(void* ctx, Str result)) {
    if (!wv) {
        return false;
    }
    if (!callback) {
        return WebViewEval(wv, js);
    }
    if (wv->pendingOpen) {

        VecAppend(wv->pendingScripts, StrDup(js));
        return true;
    }
    [wv->webview evaluateJavaScript:ToNS(js)
                  completionHandler:^(id result, NSError* error) {
                    (void)error;
                    if (!result) {
                        callback(ctx, Str());
                        return;
                    }

                    NSData* json = [NSJSONSerialization
                        dataWithJSONObject:result
                                   options:NSJSONWritingFragmentsAllowed
                                     error:nil];
                    if (!json) {
                        callback(ctx, Str());
                        return;
                    }
                    NSString* text =
                        [[NSString alloc] initWithData:json
                                              encoding:NSUTF8StringEncoding];
                    callback(ctx, FromNSTemp(text));
                  }];
    return true;
}

Str WebViewUrlTemp(WebView* wv) {
    return wv ? UrlFromWebView(wv->webview) : Str();
}

bool WebViewLoadUrl(WebView* wv, Str url) {
    return WebViewLoadUrlWithHeaders(wv, url, nullptr, 0);
}

bool WebViewLoadUrlWithHeaders(WebView* wv, Str url, const Header* headers,
                               int headerCount) {
    if (!wv) {
        return false;
    }
    NSURL* nsurl = [NSURL URLWithString:ToNS(url)];
    if (!nsurl) {
        return false;
    }
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:nsurl];
    for (int i = 0; i < headerCount; i++) {
        [request addValue:ToNS(headers[i].value)
            forHTTPHeaderField:ToNS(headers[i].name)];
    }
    [wv->webview loadRequest:request];
    return true;
}

bool WebViewLoadHtml(WebView* wv, Str html) {
    if (!wv) {
        return false;
    }
    [wv->webview loadHTMLString:ToNS(html) baseURL:nil];
    return true;
}

bool WebViewReload(WebView* wv) {
    if (!wv) {
        return false;
    }
    [wv->webview reload];
    return true;
}

bool WebViewBounds(WebView* wv, Rect* out) {
    if (!wv || !out) {
        return false;
    }
    NSView* parent = wv->webview.superview;
    if (!parent) {
        return false;
    }
    NSRect frame = wv->webview.frame;
    double y = parent.isFlipped ? frame.origin.y
                                : parent.frame.size.height - frame.origin.y -
                                      frame.size.height;
    out->position = LogicalPosition(frame.origin.x, y);
    out->size = LogicalSize(frame.size.width, frame.size.height);
    return true;
}

bool WebViewSetBounds(WebView* wv, Rect bounds) {
    if (!wv) {
        return false;
    }
    if (!wv->isChild) {

        return true;
    }
    NSView* parent = wv->webview.superview;
    if (!parent) {
        return false;
    }
    double scale = ScaleFactor(parent);
    double x = ToLogical(bounds.position.x, bounds.position.logical, scale);
    double y = ToLogical(bounds.position.y, bounds.position.logical, scale);
    double w = ToLogical(bounds.size.width, bounds.size.logical, scale);
    double h = ToLogical(bounds.size.height, bounds.size.logical, scale);
    NSRect frame;
    frame.origin = WindowPosition(parent, x, y, h);
    frame.size = NSMakeSize(w, h);
    wv->webview.frame = frame;
    return true;
}

bool WebViewSetVisible(WebView* wv, bool visible) {
    if (!wv) {
        return false;
    }
    wv->webview.hidden = visible ? NO : YES;
    wv->visible = visible;
    return true;
}

bool WebViewFocus(WebView* wv) {
    if (!wv) {
        return false;
    }
    NSWindow* window = wv->webview.window;
    if (!window) {
        return false;
    }
    [window makeFirstResponder:wv->webview];
    return true;
}

bool WebViewFocusParent(WebView* wv) {
    if (!wv) {
        return false;
    }
    NSWindow* window = wv->webview.window;
    if (!window) {
        return false;
    }
    [window makeFirstResponder:wv->parentView];
    return true;
}

bool WebViewZoom(WebView* wv, double scaleFactor) {
    if (!wv) {
        return false;
    }
    wv->webview.pageZoom = scaleFactor;
    return true;
}

bool WebViewSetUserZoom(WebView* wv, double zoom) {
    return WebViewZoom(wv, zoom);
}

bool WebViewMatchPageScale(WebView*, float) {
    return true;
}

void WebViewSetPageClick(WebView*, WebViewPageClick, void*) {}

bool WebViewSetBackgroundColor(WebView*, Rgba) {

    return true;
}

bool WebViewSetTheme(WebView* wv, Theme theme) {
    if (!wv || !wv->webview) {
        return false;
    }
    NSAppearanceName name = nil;
    if (theme == Theme::Dark) {
        name = NSAppearanceNameDarkAqua;
    } else if (theme == Theme::Light) {
        name = NSAppearanceNameAqua;
    }
    wv->webview.appearance = name ? [NSAppearance appearanceNamed:name] : nil;
    return true;
}

bool WebViewSetMemoryUsageLevel(WebView*, MemoryUsageLevel) {
    return false;
}

bool WebViewReparent(WebView* wv, void* parentWindow) {
    if (!wv || !parentWindow) {
        return false;
    }
    NSView* parent = (__bridge NSView*)parentWindow;
    [wv->webview removeFromSuperview];
    [parent addSubview:wv->webview];
    wv->parentView = parent;
    if (!wv->isChild) {
        wv->webview.frame = parent.bounds;
    }
    return true;
}

bool WebViewSetTrafficLightInset(WebView* wv, Position position) {
    if (!wv) {
        return false;
    }

    if (!wv->isChild) {
        SetTrafficLightInset(wv->webview.window, position);
    }
    return true;
}

bool WebViewPrint(WebView* wv) {
    if (!wv) {
        return false;
    }
    if (![wv->webview
            respondsToSelector:@selector(printOperationWithPrintInfo:)]) {
        return false;
    }
    NSWindow* window = wv->webview.window;
    if (!window) {
        return false;
    }
    NSPrintInfo* info = [NSPrintInfo sharedPrintInfo];
    NSPrintOperation* operation =
        [wv->webview printOperationWithPrintInfo:info];

    operation.canSpawnSeparateThread = YES;
    [operation runOperationModalForWindow:window
                                 delegate:nil
                           didRunSelector:nullptr
                              contextInfo:nullptr];
    return true;
}

bool WebViewClearAllBrowsingData(WebView* wv) {
    if (!wv) {
        return false;
    }
    WKWebsiteDataStore* store = wv->webview.configuration.websiteDataStore;
    NSSet* types = [WKWebsiteDataStore allWebsiteDataTypes];
    NSDate* since = [NSDate dateWithTimeIntervalSince1970:0];
    [store removeDataOfTypes:types
               modifiedSince:since
           completionHandler:^{
           }];
    return true;
}

static bool WaitForCookies(GpuiWryCookieResult* result) {
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:1.0];
    while (!result.done && deadline.timeIntervalSinceNow > 0) {
        [[NSRunLoop mainRunLoop]
               runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.002]];
    }
    if (!result.done) logf("wry: timed out waiting for cookies response\n");
    return result.done;
}

static Cookie CookieFromWK(NSHTTPCookie* source) {
    Cookie cookie;
    cookie.name = StrDup(FromNSTemp(source.name));
    cookie.value = StrDup(FromNSTemp(source.value));
    cookie.domain = StrDup(FromNSTemp(source.domain));
    cookie.path = StrDup(FromNSTemp(source.path));
    cookie.hasHttpOnly = true;
    cookie.httpOnly = source.HTTPOnly;
    cookie.hasSecure = true;
    cookie.secure = source.secure;
    cookie.hasSameSite = true;
    cookie.sameSite = CookieSameSite::None;
    if (@available(macOS 10.15, *)) {
        if ([source.sameSitePolicy isEqualToString:NSHTTPCookieSameSiteLax])
            cookie.sameSite = CookieSameSite::Lax;
        else if ([source.sameSitePolicy
                     isEqualToString:NSHTTPCookieSameSiteStrict])
            cookie.sameSite = CookieSameSite::Strict;
    }
    cookie.session = source.expiresDate == nil;
    cookie.hasExpires = !cookie.session;
    if (cookie.hasExpires)
        cookie.expiresUnixSeconds = (int64_t)source.expiresDate
                                        .timeIntervalSince1970;
    return cookie;
}

static NSHTTPCookie* CookieToWK(const Cookie* source) {
    if (!source) return nil;
    NSMutableDictionary* properties = [@{
        NSHTTPCookieName : ToNS(source->name),
        NSHTTPCookieValue : ToNS(source->value),
        NSHTTPCookieDomain : ToNS(source->domain),
        NSHTTPCookiePath : ToNS(source->path)
    } mutableCopy];
    if (source->hasMaxAge) {
        properties[NSHTTPCookieMaximumAge] =
            ToNS(base::FormatTemp("%lld", (long long)source->maxAgeSeconds));
        properties[NSHTTPCookieVersion] = @"1";
    } else if (source->hasExpires) {
        properties[NSHTTPCookieExpires] = [NSDate
            dateWithTimeIntervalSince1970:(double)source->expiresUnixSeconds];
        properties[NSHTTPCookieVersion] = @"0";
    }
    if (source->hasSecure)
        properties[NSHTTPCookieSecure] = source->secure ? @"TRUE" : @"FALSE";
    if (source->hasHttpOnly)
        properties[@"HttpOnly"] = source->httpOnly ? @"TRUE" : @"FALSE";
    if (@available(macOS 10.15, *)) {
        if (source->hasSameSite && source->sameSite != CookieSameSite::None)
            properties[NSHTTPCookieSameSitePolicy] =
                source->sameSite == CookieSameSite::Strict
                    ? NSHTTPCookieSameSiteStrict
                    : NSHTTPCookieSameSiteLax;
    }
    return [NSHTTPCookie cookieWithProperties:properties];
}

bool WebViewCookies(WebView* wv, Vec<Cookie>* out) {
    if (!out) return false;
    CookieListFree(out);
    if (!wv || !wv->webview) return false;
    GpuiWryCookieResult* result = [[GpuiWryCookieResult alloc] init];
    [wv->webview.configuration.websiteDataStore.httpCookieStore
        getAllCookies:^(NSArray<NSHTTPCookie*>* cookies) {
          result.cookies = cookies;
          result.done = YES;
        }];
    if (!WaitForCookies(result)) return false;
    for (NSHTTPCookie* source in result.cookies) {
        Cookie cookie = CookieFromWK(source);
        if (!cookie.name.s || !cookie.value.s || !cookie.domain.s ||
            !cookie.path.s || !VecAppend(*out, cookie)) {
            StrFree(cookie.name);
            StrFree(cookie.value);
            StrFree(cookie.domain);
            StrFree(cookie.path);
            CookieListFree(out);
            return false;
        }
    }
    return true;
}

bool WebViewCookiesForUrl(WebView* wv, Str url, Vec<Cookie>* out) {
    if (!out) return false;
    CookieListFree(out);
    NSURL* parsed = [NSURL URLWithString:ToNS(url)];
    if (!parsed.scheme) return false;

    NSString* host = parsed.host.lowercaseString;
    if ([host rangeOfString:@":"].location != NSNotFound ||
        [host rangeOfCharacterFromSet:[[NSCharacterSet
                                          characterSetWithCharactersInString:
                                              @"0123456789."] invertedSet]]
                .location == NSNotFound)
        host = nil;
    if (!WebViewCookies(wv, out)) return false;

    Str domain = FromNSTemp(host);
    Str scheme = FromNSTemp(parsed.scheme.lowercaseString);
    int kept = 0;
    for (int i = 0; i < len(*out); i++) {
        Cookie cookie = (*out)[i];
        if (MacCookieMatchesUrl(&cookie, scheme, domain))
            (*out)[kept++] = cookie;
        else {
            StrFree(cookie.name);
            StrFree(cookie.value);
            StrFree(cookie.domain);
            StrFree(cookie.path);
        }
    }
    out->len = kept;
    return true;
}

static bool ChangeWKCookie(WebView* wv, const Cookie* source, bool remove) {
    if (!wv || !wv->webview) return false;
    NSHTTPCookie* cookie = CookieToWK(source);
    if (!cookie) return false;
    GpuiWryCookieResult* result = [[GpuiWryCookieResult alloc] init];
    WKHTTPCookieStore* store = wv->webview.configuration.websiteDataStore
                                   .httpCookieStore;
    if (remove)
        [store deleteCookie:cookie
            completionHandler:^{
              result.done = YES;
            }];
    else
        [store setCookie:cookie
            completionHandler:^{
              result.done = YES;
            }];
    return WaitForCookies(result);
}

bool WebViewSetCookie(WebView* wv, const Cookie* source) {
    return ChangeWKCookie(wv, source, false);
}

bool WebViewDeleteCookie(WebView* wv, const Cookie* source) {
    return ChangeWKCookie(wv, source, true);
}

static id Inspector(WebView* wv) {
    if (!wv || !wv->webview) {
        return nil;
    }
    SEL sel = NSSelectorFromString(@"_inspector");
    if (![wv->webview respondsToSelector:sel]) {
        return nil;
    }
    NSMethodSignature* sig = [wv->webview methodSignatureForSelector:sel];
    if (!sig) {
        return nil;
    }
    NSInvocation* call = [NSInvocation invocationWithMethodSignature:sig];
    call.selector = sel;
    [call invokeWithTarget:wv->webview];
    void* result = nullptr;
    [call getReturnValue:&result];
    return (__bridge id)result;
}

static void CallInspector(WebView* wv, NSString* name) {
    id inspector = Inspector(wv);
    SEL sel = NSSelectorFromString(name);
    if (!inspector || ![inspector respondsToSelector:sel]) {
        return;
    }
    NSMethodSignature* sig = [inspector methodSignatureForSelector:sel];
    if (!sig) {
        return;
    }
    NSInvocation* call = [NSInvocation invocationWithMethodSignature:sig];
    call.selector = sel;
    [call invokeWithTarget:inspector];
}

void WebViewOpenDevtools(WebView* wv) {
    CallInspector(wv, @"show");
}

void WebViewCloseDevtools(WebView* wv) {
    CallInspector(wv, @"close");
}

bool WebViewIsDevtoolsOpen(WebView* wv) {
    id inspector = Inspector(wv);
    SEL sel = NSSelectorFromString(@"isVisible");
    if (!inspector || ![inspector respondsToSelector:sel]) {
        return false;
    }
    NSMethodSignature* sig = [inspector methodSignatureForSelector:sel];
    if (!sig) {
        return false;
    }
    NSInvocation* call = [NSInvocation invocationWithMethodSignature:sig];
    call.selector = sel;
    [call invokeWithTarget:inspector];
    BOOL result = NO;
    [call getReturnValue:&result];
    return result == YES;
}

Str WebViewVersionTemp() {
    NSBundle* bundle = [NSBundle bundleWithIdentifier:@"com.apple.WebKit"];
    if (!bundle) {
        return {};
    }
    NSString* version = [bundle.infoDictionary objectForKey:@"CFBundleVersion"];
    return version ? FromNSTemp(version) : Str();
}

bool WebViewAvailable() {
    return true;
}

}

#endif

#if GPUI_OS_WINDOWS
#line 1 "src/wry/wry_win.cpp"

#include <windows.h>

#include <commctrl.h>
#include <eventtoken.h>
#include <objbase.h>
#include <objidl.h>
#include <ole2.h>
#include <oleidl.h>
#include <shellapi.h>

extern "C" __declspec(dllimport) IStream* STDAPICALLTYPE
SHCreateMemStream(const BYTE* pInit, UINT cbInit);

namespace wry {

using base::AllocStrTemp;
using base::Arena;
using base::ComObject;
using base::Func0;
using base::GetTempArena;
using base::logf;
using base::MkFunc0;
using base::Str;
using base::StrDup;
using base::StrFree;
using base::ToCWstrTemp;
using base::Vec;

typedef struct COREWEBVIEW2_COLOR {
    BYTE A;
    BYTE R;
    BYTE G;
    BYTE B;
} COREWEBVIEW2_COLOR;

typedef int COREWEBVIEW2_BOUNDS_MODE;
typedef int COREWEBVIEW2_BROWSING_DATA_KINDS;
typedef int COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT;
typedef int COREWEBVIEW2_CHANNEL_SEARCH_KIND;
typedef int COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND;
typedef int COREWEBVIEW2_COOKIE_SAME_SITE_KIND;
typedef int COREWEBVIEW2_DEFAULT_DOWNLOAD_DIALOG_CORNER_ALIGNMENT;
typedef int COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON;
typedef int COREWEBVIEW2_DOWNLOAD_STATE;
typedef int COREWEBVIEW2_FAVICON_IMAGE_FORMAT;
typedef int COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND;
typedef int COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL;
typedef int COREWEBVIEW2_MOVE_FOCUS_REASON;
typedef int COREWEBVIEW2_PDF_TOOLBAR_ITEMS;
typedef int COREWEBVIEW2_PERMISSION_KIND;
typedef int COREWEBVIEW2_PERMISSION_STATE;
typedef int COREWEBVIEW2_PREFERRED_COLOR_SCHEME;
typedef int COREWEBVIEW2_PRINT_DIALOG_KIND;
typedef int COREWEBVIEW2_RELEASE_CHANNELS;
typedef int COREWEBVIEW2_SCROLLBAR_STYLE;
typedef int COREWEBVIEW2_SHARED_BUFFER_ACCESS;
typedef int COREWEBVIEW2_TRACKING_PREVENTION_LEVEL;
typedef int COREWEBVIEW2_WEB_RESOURCE_CONTEXT;
typedef int COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS;

struct ICoreWebView2AcceleratorKeyPressedEventHandler;
struct ICoreWebView2BasicAuthenticationRequestedEventHandler;
struct ICoreWebView2BrowserProcessExitedEventHandler;
struct ICoreWebView2CallDevToolsProtocolMethodCompletedHandler;
struct ICoreWebView2CapturePreviewCompletedHandler;
struct ICoreWebView2ClearServerCertificateErrorActionsCompletedHandler;
struct ICoreWebView2ClientCertificateRequestedEventHandler;
struct ICoreWebView2ContainsFullScreenElementChangedEventHandler;
struct ICoreWebView2ContentLoadingEventArgs;
struct ICoreWebView2ContextMenuItem;
struct ICoreWebView2ContextMenuRequestedEventHandler;
struct ICoreWebView2CustomSchemeRegistration;
struct ICoreWebView2Cookie;
struct ICoreWebView2CookieList;
struct ICoreWebView2CookieManager;
struct ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler;
struct ICoreWebView2DOMContentLoadedEventHandler;
struct ICoreWebView2DevToolsProtocolEventReceiver;
struct ICoreWebView2DownloadStartingEventHandler;
struct ICoreWebView2DownloadStartingEventArgs;
struct ICoreWebView2DownloadOperation;
struct ICoreWebView2BytesReceivedChangedEventHandler;
struct ICoreWebView2EstimatedEndTimeChangedEventHandler;
struct ICoreWebView2ExecuteScriptWithResultCompletedHandler;
struct ICoreWebView2FaviconChangedEventHandler;
struct ICoreWebView2FocusChangedEventHandler;
struct ICoreWebView2FrameCreatedEventHandler;
struct ICoreWebView2GetFaviconCompletedHandler;
struct ICoreWebView2GetCookiesCompletedHandler;
struct ICoreWebView2HistoryChangedEventHandler;
struct ICoreWebView2HttpResponseHeaders;
struct ICoreWebView2IsDefaultDownloadDialogOpenChangedEventHandler;
struct ICoreWebView2IsDocumentPlayingAudioChangedEventHandler;
struct ICoreWebView2IsMutedChangedEventHandler;
struct ICoreWebView2LaunchingExternalUriSchemeEventHandler;
struct ICoreWebView2MoveFocusRequestedEventHandler;
struct ICoreWebView2NavigationCompletedEventArgs;
struct ICoreWebView2NewBrowserVersionAvailableEventHandler;
struct ICoreWebView2PointerInfo;
struct ICoreWebView2PrintCompletedHandler;
struct ICoreWebView2PrintSettings;
struct ICoreWebView2PrintToPdfCompletedHandler;
struct ICoreWebView2PrintToPdfStreamCompletedHandler;
struct ICoreWebView2ProcessFailedEventArgs;
struct DECLSPEC_UUID("79e0aea4-990b-42d9-aa1d-0fcc2e5bc7f1")
    ICoreWebView2ProcessFailedEventHandler;
struct ICoreWebView2ProcessInfoCollection;
struct ICoreWebView2ProcessInfosChangedEventHandler;
struct ICoreWebView2BrowserExtension;
struct ICoreWebView2GetNonDefaultPermissionSettingsCompletedHandler;
struct ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler;
struct ICoreWebView2RasterizationScaleChangedEventHandler;
struct ICoreWebView2ScriptDialogOpeningEventHandler;
struct ICoreWebView2SetPermissionStateCompletedHandler;
struct ICoreWebView2ServerCertificateErrorDetectedEventHandler;
struct ICoreWebView2SharedBuffer;
struct ICoreWebView2SourceChangedEventHandler;
struct ICoreWebView2StatusBarTextChangedEventHandler;
struct ICoreWebView2StateChangedEventHandler;
struct ICoreWebView2TrySuspendCompletedHandler;
struct ICoreWebView2WebResourceResponseReceivedEventHandler;
struct ICoreWebView2ZoomFactorChangedEventHandler;

struct DECLSPEC_UUID("b96d755e-0319-4e92-a296-23436f46a1fc")
    ICoreWebView2Environment;
struct DECLSPEC_UUID("41f3632b-5ef4-404f-ad82-2d606c5a9a21")
    ICoreWebView2Environment2;
struct DECLSPEC_UUID("80a22ae3-be7c-4ce2-afe1-5a50056cdeeb")
    ICoreWebView2Environment3;
struct DECLSPEC_UUID("20944379-6dcf-41d6-a0a0-abc0fc50de0d")
    ICoreWebView2Environment4;
struct DECLSPEC_UUID("319e423d-e0d7-4b8d-9254-ae9475de9b17")
    ICoreWebView2Environment5;
struct DECLSPEC_UUID("e59ee362-acbd-4857-9a8e-d3644d9459a9")
    ICoreWebView2Environment6;
struct DECLSPEC_UUID("43c22296-3bbd-43a4-9c00-5c0df6dd29a2")
    ICoreWebView2Environment7;
struct DECLSPEC_UUID("d6eb91dd-c3d2-45e5-bd29-6dc2bc4de9cf")
    ICoreWebView2Environment8;
struct DECLSPEC_UUID("f06f41bf-4b5a-49d8-b9f6-fa16cd29f274")
    ICoreWebView2Environment9;
struct DECLSPEC_UUID("ee0eb9df-6f12-46ce-b53f-3f47b9c928e0")
    ICoreWebView2Environment10;
struct DECLSPEC_UUID("12aae616-8ccb-44ec-bcb3-eb1831881635")
    ICoreWebView2ControllerOptions;
struct DECLSPEC_UUID("06c991d8-9e7e-11ed-a8fc-0242ac120002")
    ICoreWebView2ControllerOptions2;
struct DECLSPEC_UUID("b32b191a-8998-57ca-b7cb-e04617e4ce4a")
    ICoreWebView2ControllerOptions3;
struct DECLSPEC_UUID("4d00c0d1-9434-4eb6-8078-8697a560334f")
    ICoreWebView2Controller;
struct DECLSPEC_UUID("c979903e-d4ca-4228-92eb-47ee3fa96eab")
    ICoreWebView2Controller2;
struct DECLSPEC_UUID("f9614724-5d2b-41dc-aef7-73d62b51543b")
    ICoreWebView2Controller3;
struct DECLSPEC_UUID("97d418d5-a426-4e49-a151-e1a10f327d9e")
    ICoreWebView2Controller4;
struct DECLSPEC_UUID("76eceacb-0462-4d94-ac83-423a6793775e") ICoreWebView2;
struct DECLSPEC_UUID("9E8F0CF8-E670-4B5E-B2BC-73E061E3184C") ICoreWebView2_2;
struct DECLSPEC_UUID("A0D6DF20-3B92-416D-AA0C-437A9C727857") ICoreWebView2_3;
struct DECLSPEC_UUID("20d02d59-6df2-42dc-bd06-f98a694b1302") ICoreWebView2_4;
struct DECLSPEC_UUID("bedb11b8-d63c-11eb-b8bc-0242ac130003") ICoreWebView2_5;
struct DECLSPEC_UUID("499aadac-d92c-4589-8a75-111bfc167795") ICoreWebView2_6;
struct DECLSPEC_UUID("79c24d83-09a3-45ae-9418-487f32a58740") ICoreWebView2_7;
struct DECLSPEC_UUID("E9632730-6E1E-43AB-B7B8-7B2C9E62E094") ICoreWebView2_8;
struct DECLSPEC_UUID("4d7b2eab-9fdc-468d-b998-a9260b5ed651") ICoreWebView2_9;
struct DECLSPEC_UUID("b1690564-6f5a-4983-8e48-31d1143fecdb") ICoreWebView2_10;
struct DECLSPEC_UUID("0be78e56-c193-4051-b943-23b460c08bdb") ICoreWebView2_11;
struct DECLSPEC_UUID("35D69927-BCFA-4566-9349-6B3E0D154CAC") ICoreWebView2_12;
struct DECLSPEC_UUID("f75f09a8-667e-4983-88d6-c8773f315e84") ICoreWebView2_13;
struct DECLSPEC_UUID("6daa4f10-4a90-4753-8898-77c5df534165") ICoreWebView2_14;
struct DECLSPEC_UUID("517B2D1D-7DAE-4A66-A4F4-10352FFB9518") ICoreWebView2_15;
struct DECLSPEC_UUID("0EB34DC9-9F91-41E1-8639-95CD5943906B") ICoreWebView2_16;
struct DECLSPEC_UUID("702e75d4-fd44-434d-9d70-1a68a6b1192a") ICoreWebView2_17;
struct DECLSPEC_UUID("7a626017-28be-49b2-b865-3ba2b3522d90") ICoreWebView2_18;
struct DECLSPEC_UUID("6921f954-79b0-437f-a997-c85811897c68") ICoreWebView2_19;
struct DECLSPEC_UUID("b4bc1926-7305-11ee-b962-0242ac120002") ICoreWebView2_20;
struct DECLSPEC_UUID("c4980dea-587b-43b9-8143-3ef3bf552d95") ICoreWebView2_21;
struct DECLSPEC_UUID("db75dfc7-a857-4632-a398-6969dde26c0a") ICoreWebView2_22;
struct DECLSPEC_UUID("e562e4f0-d7fa-43ac-8d71-c05150499f00")
    ICoreWebView2Settings;
struct DECLSPEC_UUID("ee9a0f68-f46c-4e32-ac23-ef8cac224d2a")
    ICoreWebView2Settings2;
struct DECLSPEC_UUID("fdb5ab74-af33-4854-84f0-0a631deb5eba")
    ICoreWebView2Settings3;
struct DECLSPEC_UUID("cb56846c-4168-4d53-b04f-03b6d6796ff2")
    ICoreWebView2Settings4;
struct DECLSPEC_UUID("183e7052-1d03-43a0-ab99-98e043b66b39")
    ICoreWebView2Settings5;
struct DECLSPEC_UUID("11cb3acd-9bc8-43b8-83bf-f40753714f87")
    ICoreWebView2Settings6;
struct DECLSPEC_UUID("488dc902-35ef-42d2-bc7d-94b65c4bc49c")
    ICoreWebView2Settings7;
struct DECLSPEC_UUID("9e6b0e8f-86ad-4e81-8147-a9b5edb68650")
    ICoreWebView2Settings8;
struct DECLSPEC_UUID("0528a73b-e92d-49f4-927a-e547dddaa37d")
    ICoreWebView2Settings9;
struct DECLSPEC_UUID("79110ad3-cd5d-4373-8bc3-c60658f17a5f")
    ICoreWebView2Profile;
struct DECLSPEC_UUID("fa740d4b-5eae-4344-a8ad-74be31925397")
    ICoreWebView2Profile2;
struct DECLSPEC_UUID("b188e659-5685-4e05-bdba-fc640e0f1992")
    ICoreWebView2Profile3;
struct DECLSPEC_UUID("8f4ae680-192e-4ec8-833a-21cfadaef628")
    ICoreWebView2Profile4;
struct DECLSPEC_UUID("2ee5b76e-6e80-4df2-bcd3-d4ec3340a01b")
    ICoreWebView2Profile5;
struct DECLSPEC_UUID("bd82fa6a-1d65-4c33-b2b4-0393020cc61b")
    ICoreWebView2Profile6;
struct DECLSPEC_UUID("7b4c7906-a1aa-4cb4-b723-db09f813d541")
    ICoreWebView2Profile7;
struct DECLSPEC_UUID("df1aab27-82b9-4ab6-aae8-017a49398c14")
    ICoreWebView2ProfileAddBrowserExtensionCompletedHandler;
struct DECLSPEC_UUID("0f99a40c-e962-4207-9e92-e3d542eff849")
    ICoreWebView2WebMessageReceivedEventArgs;
struct DECLSPEC_UUID("453e667f-12c7-49d4-be6d-ddbe7956f57a")
    ICoreWebView2WebResourceRequestedEventArgs;
struct DECLSPEC_UUID("97055cd4-512c-4264-8b5f-e3f446cea6a5")
    ICoreWebView2WebResourceRequest;
struct DECLSPEC_UUID("aafcc94f-fa27-48fd-97df-830ef75aaec9")
    ICoreWebView2WebResourceResponse;
struct DECLSPEC_UUID("e86cac0e-5523-465c-b536-8fb9fc8c8c60")
    ICoreWebView2HttpRequestHeaders;
struct DECLSPEC_UUID("0702fc30-f43b-47bb-ab52-a42cb552ad9f")
    ICoreWebView2HttpHeadersCollectionIterator;
struct DECLSPEC_UUID("c10e7f7b-b585-46f0-a623-8befbf3e4ee0")
    ICoreWebView2Deferral;
struct DECLSPEC_UUID("5b495469-e119-438a-9b18-7604f25f2e49")
    ICoreWebView2NavigationStartingEventArgs;
struct DECLSPEC_UUID("34acb11c-fc37-4418-9132-f9c21d1eafb9")
    ICoreWebView2NewWindowRequestedEventArgs;
struct DECLSPEC_UUID("5eaf559f-b46e-4397-8860-e422f287ff1e")
    ICoreWebView2WindowFeatures;
struct DECLSPEC_UUID("973ae2ef-ff18-4894-8fb2-3c758f046810")
    ICoreWebView2PermissionRequestedEventArgs;
struct DECLSPEC_UUID("4e8a3389-c9d8-4bd2-b6b5-124fee6cc14d")
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;
struct DECLSPEC_UUID("6c4819f3-c9b7-4260-8127-c9f5bde7f68c")
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
struct DECLSPEC_UUID("57213f19-00e6-49fa-8e07-898ea01ecbd2")
    ICoreWebView2WebMessageReceivedEventHandler;
struct DECLSPEC_UUID("ab00b74c-15f1-4646-80e8-e76341d25d71")
    ICoreWebView2WebResourceRequestedEventHandler;
struct DECLSPEC_UUID("9adbe429-f36d-432b-9ddc-f8881fbd76e3")
    ICoreWebView2NavigationStartingEventHandler;
struct DECLSPEC_UUID("d33a35bf-1c49-4f98-93ab-006e0533fe1c")
    ICoreWebView2NavigationCompletedEventHandler;
struct DECLSPEC_UUID("364471e7-f2be-4910-bdba-d72077d51c4b")
    ICoreWebView2ContentLoadingEventHandler;
struct DECLSPEC_UUID("f5f2b923-953e-4042-9f95-f3a118e1afd4")
    ICoreWebView2DocumentTitleChangedEventHandler;
struct DECLSPEC_UUID("d4c185fe-c81c-4989-97af-2d3fa7ab5651")
    ICoreWebView2NewWindowRequestedEventHandler;
struct DECLSPEC_UUID("5c19e9e0-092f-486b-affa-ca8231913039")
    ICoreWebView2WindowCloseRequestedEventHandler;
struct DECLSPEC_UUID("15e1c6a3-c72a-4df3-91d7-d097fbec6bfd")
    ICoreWebView2PermissionRequestedEventHandler;
struct DECLSPEC_UUID("b99369f3-9b11-47b5-bc6f-8e7895fcea17")
    ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler;
struct DECLSPEC_UUID("49511172-cc67-4bca-9923-137112f4c4cc")
    ICoreWebView2ExecuteScriptCompletedHandler;
struct DECLSPEC_UUID("e9710a06-1d1d-49b2-8234-226f35846ae5")
    ICoreWebView2ClearBrowsingDataCompletedHandler;
struct DECLSPEC_UUID("3d6b6cf2-afe1-44c7-a995-c65117714336")
    ICoreWebView2DownloadOperation;
struct DECLSPEC_UUID("e99bbe21-43e9-4544-a732-282764eafa60")
    ICoreWebView2DownloadStartingEventArgs;
struct DECLSPEC_UUID("efedc989-c396-41ca-83f7-07f845a55724")
    ICoreWebView2DownloadStartingEventHandler;
struct DECLSPEC_UUID("81336594-7ede-4ba9-bf71-acf0a95b58dd")
    ICoreWebView2StateChangedEventHandler;
struct DECLSPEC_UUID("ad26d6be-1486-43e6-bf87-a2034006ca21")
    ICoreWebView2Cookie;
struct DECLSPEC_UUID("f7f6f714-5d2a-43c6-9503-346ece02d186")
    ICoreWebView2CookieList;
struct DECLSPEC_UUID("177cd9e7-b6f5-451a-94a0-5d7a3a4c4141")
    ICoreWebView2CookieManager;
struct DECLSPEC_UUID("5a4f5069-5c15-47c3-8646-f4de1c116670")
    ICoreWebView2GetCookiesCompletedHandler;
struct DECLSPEC_UUID("2fde08a8-1e9a-4766-8c05-95a9ceb9d1c5")
    ICoreWebView2EnvironmentOptions;
struct DECLSPEC_UUID("ff85c98a-1ba7-4a6b-90c8-2b752c89e9e2")
    ICoreWebView2EnvironmentOptions2;
struct DECLSPEC_UUID("4a5c436e-a9e3-4a2e-89c3-910d3513f5cc")
    ICoreWebView2EnvironmentOptions3;
struct DECLSPEC_UUID("ac52d13f-0d38-475a-9dca-876580d6793e")
    ICoreWebView2EnvironmentOptions4;
struct DECLSPEC_UUID("0ae35d64-c47f-4464-814e-259c345d1501")
    ICoreWebView2EnvironmentOptions5;
struct DECLSPEC_UUID("57d29cc3-c84f-42a0-b0e2-effbd5e179de")
    ICoreWebView2EnvironmentOptions6;
struct DECLSPEC_UUID("c48d539f-e39f-441c-ae68-1f66e570bdc5")
    ICoreWebView2EnvironmentOptions7;
struct DECLSPEC_UUID("7c7ecf51-e918-5caf-853c-e9a2bcc27775")
    ICoreWebView2EnvironmentOptions8;

struct ICoreWebView2Environment : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateCoreWebView2Controller(
        HWND parentWindow,
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateWebResourceResponse(
        IStream* content, int statusCode, LPCWSTR reasonPhrase, LPCWSTR headers,
        ICoreWebView2WebResourceResponse** response) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_BrowserVersionString(LPWSTR* versionInfo) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_NewBrowserVersionAvailable(
        ICoreWebView2NewBrowserVersionAvailableEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_NewBrowserVersionAvailable(EventRegistrationToken token) = 0;
};

struct ICoreWebView2Environment2 : ICoreWebView2Environment {
    virtual HRESULT STDMETHODCALLTYPE CreateWebResourceRequest(
        LPCWSTR uri, LPCWSTR Method, IStream* postData, LPCWSTR Headers,
        ICoreWebView2WebResourceRequest** value) = 0;
};

struct ICoreWebView2Environment3 : ICoreWebView2Environment2 {
    virtual HRESULT STDMETHODCALLTYPE CreateCoreWebView2CompositionController(
        HWND ParentWindow,
        ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler*
            handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    CreateCoreWebView2PointerInfo(ICoreWebView2PointerInfo** value) = 0;
};

struct ICoreWebView2Environment4 : ICoreWebView2Environment3 {
    virtual HRESULT STDMETHODCALLTYPE
    GetAutomationProviderForWindow(HWND hwnd, IUnknown** value) = 0;
};

struct ICoreWebView2Environment5 : ICoreWebView2Environment4 {
    virtual HRESULT STDMETHODCALLTYPE add_BrowserProcessExited(
        ICoreWebView2BrowserProcessExitedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_BrowserProcessExited(EventRegistrationToken token) = 0;
};

struct ICoreWebView2Environment6 : ICoreWebView2Environment5 {
    virtual HRESULT STDMETHODCALLTYPE
    CreatePrintSettings(ICoreWebView2PrintSettings** value) = 0;
};

struct ICoreWebView2Environment7 : ICoreWebView2Environment6 {
    virtual HRESULT STDMETHODCALLTYPE get_UserDataFolder(LPWSTR* value) = 0;
};

struct ICoreWebView2Environment8 : ICoreWebView2Environment7 {
    virtual HRESULT STDMETHODCALLTYPE add_ProcessInfosChanged(
        ICoreWebView2ProcessInfosChangedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_ProcessInfosChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    GetProcessInfos(ICoreWebView2ProcessInfoCollection** value) = 0;
};

struct ICoreWebView2Environment9 : ICoreWebView2Environment8 {
    virtual HRESULT STDMETHODCALLTYPE
    CreateContextMenuItem(LPCWSTR Label, IStream* iconStream,
                          COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND Kind,
                          ICoreWebView2ContextMenuItem** value) = 0;
};

struct ICoreWebView2Environment10 : ICoreWebView2Environment9 {
    virtual HRESULT STDMETHODCALLTYPE CreateCoreWebView2ControllerOptions(
        ICoreWebView2ControllerOptions** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCoreWebView2ControllerWithOptions(
        HWND ParentWindow, ICoreWebView2ControllerOptions* options,
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    CreateCoreWebView2CompositionControllerWithOptions(
        HWND ParentWindow, ICoreWebView2ControllerOptions* options,
        ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler*
            handler) = 0;
};

struct ICoreWebView2ControllerOptions : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_ProfileName(LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ProfileName(LPCWSTR value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_IsInPrivateModeEnabled(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_IsInPrivateModeEnabled(BOOL value) = 0;
};

struct ICoreWebView2ControllerOptions2 : ICoreWebView2ControllerOptions {
    virtual HRESULT STDMETHODCALLTYPE get_ScriptLocale(LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ScriptLocale(LPCWSTR value) = 0;
};

struct ICoreWebView2ControllerOptions3 : ICoreWebView2ControllerOptions2 {
    virtual HRESULT STDMETHODCALLTYPE
    get_DefaultBackgroundColor(COREWEBVIEW2_COLOR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_DefaultBackgroundColor(COREWEBVIEW2_COLOR value) = 0;
};

struct ICoreWebView2Controller : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_IsVisible(BOOL* isVisible) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsVisible(BOOL isVisible) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Bounds(RECT* bounds) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Bounds(RECT bounds) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ZoomFactor(double* zoomFactor) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ZoomFactor(double zoomFactor) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ZoomFactorChanged(
        ICoreWebView2ZoomFactorChangedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_ZoomFactorChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    SetBoundsAndZoomFactor(RECT bounds, double zoomFactor) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON reason) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_MoveFocusRequested(
        ICoreWebView2MoveFocusRequestedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_MoveFocusRequested(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    add_GotFocus(ICoreWebView2FocusChangedEventHandler* eventHandler,
                 EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_GotFocus(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    add_LostFocus(ICoreWebView2FocusChangedEventHandler* eventHandler,
                  EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_LostFocus(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_AcceleratorKeyPressed(
        ICoreWebView2AcceleratorKeyPressedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_AcceleratorKeyPressed(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ParentWindow(HWND* parentWindow) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ParentWindow(HWND parentWindow) = 0;
    virtual HRESULT STDMETHODCALLTYPE NotifyParentWindowPositionChanged() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_CoreWebView2(ICoreWebView2** coreWebView2) = 0;
};

struct ICoreWebView2Controller2 : ICoreWebView2Controller {
    virtual HRESULT STDMETHODCALLTYPE
    get_DefaultBackgroundColor(COREWEBVIEW2_COLOR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_DefaultBackgroundColor(COREWEBVIEW2_COLOR value) = 0;
};

struct ICoreWebView2Controller3 : ICoreWebView2Controller2 {
    virtual HRESULT STDMETHODCALLTYPE get_RasterizationScale(double* scale) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_RasterizationScale(double scale) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_ShouldDetectMonitorScaleChanges(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_ShouldDetectMonitorScaleChanges(BOOL value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_RasterizationScaleChanged(
        ICoreWebView2RasterizationScaleChangedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_RasterizationScaleChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_BoundsMode(COREWEBVIEW2_BOUNDS_MODE* boundsMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE boundsMode) = 0;
};

struct ICoreWebView2Controller4 : ICoreWebView2Controller3 {
    virtual HRESULT STDMETHODCALLTYPE get_AllowExternalDrop(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AllowExternalDrop(BOOL value) = 0;
};

struct ICoreWebView2 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    get_Settings(ICoreWebView2Settings** settings) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Source(LPWSTR* uri) = 0;
    virtual HRESULT STDMETHODCALLTYPE Navigate(LPCWSTR uri) = 0;
    virtual HRESULT STDMETHODCALLTYPE NavigateToString(LPCWSTR htmlContent) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_NavigationStarting(
        ICoreWebView2NavigationStartingEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_NavigationStarting(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    add_ContentLoading(ICoreWebView2ContentLoadingEventHandler* eventHandler,
                       EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_ContentLoading(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    add_SourceChanged(ICoreWebView2SourceChangedEventHandler* eventHandler,
                      EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_SourceChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    add_HistoryChanged(ICoreWebView2HistoryChangedEventHandler* eventHandler,
                       EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_HistoryChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_NavigationCompleted(
        ICoreWebView2NavigationCompletedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_NavigationCompleted(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameNavigationStarting(
        ICoreWebView2NavigationStartingEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_FrameNavigationStarting(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameNavigationCompleted(
        ICoreWebView2NavigationCompletedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_FrameNavigationCompleted(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ScriptDialogOpening(
        ICoreWebView2ScriptDialogOpeningEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_ScriptDialogOpening(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_PermissionRequested(
        ICoreWebView2PermissionRequestedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_PermissionRequested(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    add_ProcessFailed(ICoreWebView2ProcessFailedEventHandler* eventHandler,
                      EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_ProcessFailed(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddScriptToExecuteOnDocumentCreated(
        LPCWSTR javaScript,
        ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler*
            handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    RemoveScriptToExecuteOnDocumentCreated(LPCWSTR id) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    ExecuteScript(LPCWSTR javaScript,
                  ICoreWebView2ExecuteScriptCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT imageFormat,
                   IStream* imageStream,
                   ICoreWebView2CapturePreviewCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reload() = 0;
    virtual HRESULT STDMETHODCALLTYPE
    PostWebMessageAsJson(LPCWSTR webMessageAsJson) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    PostWebMessageAsString(LPCWSTR webMessageAsString) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    add_WebMessageReceived(ICoreWebView2WebMessageReceivedEventHandler* handler,
                           EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_WebMessageReceived(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE CallDevToolsProtocolMethod(
        LPCWSTR methodName, LPCWSTR parametersAsJson,
        ICoreWebView2CallDevToolsProtocolMethodCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BrowserProcessId(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CanGoBack(BOOL* canGoBack) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CanGoForward(BOOL* canGoForward) = 0;
    virtual HRESULT STDMETHODCALLTYPE GoBack() = 0;
    virtual HRESULT STDMETHODCALLTYPE GoForward() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDevToolsProtocolEventReceiver(
        LPCWSTR eventName,
        ICoreWebView2DevToolsProtocolEventReceiver** receiver) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE add_NewWindowRequested(
        ICoreWebView2NewWindowRequestedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_NewWindowRequested(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_DocumentTitleChanged(
        ICoreWebView2DocumentTitleChangedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_DocumentTitleChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DocumentTitle(LPWSTR* title) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    AddHostObjectToScript(LPCWSTR name, VARIANT* object) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    RemoveHostObjectFromScript(LPCWSTR name) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenDevToolsWindow() = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ContainsFullScreenElementChanged(
        ICoreWebView2ContainsFullScreenElementChangedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_ContainsFullScreenElementChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_ContainsFullScreenElement(BOOL* containsFullScreenElement) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_WebResourceRequested(
        ICoreWebView2WebResourceRequestedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_WebResourceRequested(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddWebResourceRequestedFilter(
        const LPCWSTR uri,
        const COREWEBVIEW2_WEB_RESOURCE_CONTEXT resourceContext) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveWebResourceRequestedFilter(
        const LPCWSTR uri,
        const COREWEBVIEW2_WEB_RESOURCE_CONTEXT resourceContext) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_WindowCloseRequested(
        ICoreWebView2WindowCloseRequestedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_WindowCloseRequested(EventRegistrationToken token) = 0;
};

struct ICoreWebView2_2 : ICoreWebView2 {
    virtual HRESULT STDMETHODCALLTYPE add_WebResourceResponseReceived(
        ICoreWebView2WebResourceResponseReceivedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_WebResourceResponseReceived(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE NavigateWithWebResourceRequest(
        ICoreWebView2WebResourceRequest* request) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_DOMContentLoaded(
        ICoreWebView2DOMContentLoadedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_DOMContentLoaded(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_CookieManager(ICoreWebView2CookieManager** cookieManager) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_Environment(ICoreWebView2Environment** environment) = 0;
};

struct ICoreWebView2_3 : ICoreWebView2_2 {
    virtual HRESULT STDMETHODCALLTYPE
    TrySuspend(ICoreWebView2TrySuspendCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE Resume() = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsSuspended(BOOL* isSuspended) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVirtualHostNameToFolderMapping(
        LPCWSTR hostName, LPCWSTR folderPath,
        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND accessKind) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    ClearVirtualHostNameToFolderMapping(LPCWSTR hostName) = 0;
};

struct ICoreWebView2_4 : ICoreWebView2_3 {
    virtual HRESULT STDMETHODCALLTYPE
    add_FrameCreated(ICoreWebView2FrameCreatedEventHandler* eventHandler,
                     EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_FrameCreated(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_DownloadStarting(
        ICoreWebView2DownloadStartingEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_DownloadStarting(EventRegistrationToken token) = 0;
};

struct ICoreWebView2_5 : ICoreWebView2_4 {
    virtual HRESULT STDMETHODCALLTYPE add_ClientCertificateRequested(
        ICoreWebView2ClientCertificateRequestedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_ClientCertificateRequested(EventRegistrationToken token) = 0;
};

struct ICoreWebView2_6 : ICoreWebView2_5 {
    virtual HRESULT STDMETHODCALLTYPE OpenTaskManagerWindow() = 0;
};

struct ICoreWebView2_7 : ICoreWebView2_6 {
    virtual HRESULT STDMETHODCALLTYPE PrintToPdf(
        LPCWSTR ResultFilePath, ICoreWebView2PrintSettings* printSettings,
        ICoreWebView2PrintToPdfCompletedHandler* handler) = 0;
};

struct ICoreWebView2_8 : ICoreWebView2_7 {
    virtual HRESULT STDMETHODCALLTYPE
    add_IsMutedChanged(ICoreWebView2IsMutedChangedEventHandler* eventHandler,
                       EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_IsMutedChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsMuted(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsMuted(BOOL value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_IsDocumentPlayingAudioChanged(
        ICoreWebView2IsDocumentPlayingAudioChangedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_IsDocumentPlayingAudioChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_IsDocumentPlayingAudio(BOOL* value) = 0;
};

struct ICoreWebView2_9 : ICoreWebView2_8 {
    virtual HRESULT STDMETHODCALLTYPE add_IsDefaultDownloadDialogOpenChanged(
        ICoreWebView2IsDefaultDownloadDialogOpenChangedEventHandler* handler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_IsDefaultDownloadDialogOpenChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_IsDefaultDownloadDialogOpen(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenDefaultDownloadDialog() = 0;
    virtual HRESULT STDMETHODCALLTYPE CloseDefaultDownloadDialog() = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DefaultDownloadDialogCornerAlignment(
        COREWEBVIEW2_DEFAULT_DOWNLOAD_DIALOG_CORNER_ALIGNMENT* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DefaultDownloadDialogCornerAlignment(
        COREWEBVIEW2_DEFAULT_DOWNLOAD_DIALOG_CORNER_ALIGNMENT value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_DefaultDownloadDialogMargin(POINT* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_DefaultDownloadDialogMargin(POINT value) = 0;
};

struct ICoreWebView2_10 : ICoreWebView2_9 {
    virtual HRESULT STDMETHODCALLTYPE add_BasicAuthenticationRequested(
        ICoreWebView2BasicAuthenticationRequestedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_BasicAuthenticationRequested(EventRegistrationToken token) = 0;
};

struct ICoreWebView2_11 : ICoreWebView2_10 {
    virtual HRESULT STDMETHODCALLTYPE CallDevToolsProtocolMethodForSession(
        LPCWSTR sessionId, LPCWSTR methodName, LPCWSTR parametersAsJson,
        ICoreWebView2CallDevToolsProtocolMethodCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ContextMenuRequested(
        ICoreWebView2ContextMenuRequestedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_ContextMenuRequested(EventRegistrationToken token) = 0;
};

struct ICoreWebView2_12 : ICoreWebView2_11 {
    virtual HRESULT STDMETHODCALLTYPE add_StatusBarTextChanged(
        ICoreWebView2StatusBarTextChangedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_StatusBarTextChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_StatusBarText(LPWSTR* value) = 0;
};

struct ICoreWebView2_13 : ICoreWebView2_12 {
    virtual HRESULT STDMETHODCALLTYPE
    get_Profile(ICoreWebView2Profile** value) = 0;
};

struct ICoreWebView2_14 : ICoreWebView2_13 {
    virtual HRESULT STDMETHODCALLTYPE add_ServerCertificateErrorDetected(
        ICoreWebView2ServerCertificateErrorDetectedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_ServerCertificateErrorDetected(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearServerCertificateErrorActions(
        ICoreWebView2ClearServerCertificateErrorActionsCompletedHandler*
            handler) = 0;
};

struct ICoreWebView2_15 : ICoreWebView2_14 {
    virtual HRESULT STDMETHODCALLTYPE
    add_FaviconChanged(ICoreWebView2FaviconChangedEventHandler* eventHandler,
                       EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_FaviconChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FaviconUri(LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    GetFavicon(COREWEBVIEW2_FAVICON_IMAGE_FORMAT format,
               ICoreWebView2GetFaviconCompletedHandler* completedHandler) = 0;
};

struct ICoreWebView2_16 : ICoreWebView2_15 {
    virtual HRESULT STDMETHODCALLTYPE
    Print(ICoreWebView2PrintSettings* printSettings,
          ICoreWebView2PrintCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    ShowPrintUI(COREWEBVIEW2_PRINT_DIALOG_KIND printDialogKind) = 0;
    virtual HRESULT STDMETHODCALLTYPE PrintToPdfStream(
        ICoreWebView2PrintSettings* printSettings,
        ICoreWebView2PrintToPdfStreamCompletedHandler* handler) = 0;
};

struct ICoreWebView2_17 : ICoreWebView2_16 {
    virtual HRESULT STDMETHODCALLTYPE
    PostSharedBufferToScript(ICoreWebView2SharedBuffer* sharedBuffer,
                             COREWEBVIEW2_SHARED_BUFFER_ACCESS access,
                             LPCWSTR additionalDataAsJson) = 0;
};

struct ICoreWebView2_18 : ICoreWebView2_17 {
    virtual HRESULT STDMETHODCALLTYPE add_LaunchingExternalUriScheme(
        ICoreWebView2LaunchingExternalUriSchemeEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_LaunchingExternalUriScheme(EventRegistrationToken token) = 0;
};

struct ICoreWebView2_19 : ICoreWebView2_18 {
    virtual HRESULT STDMETHODCALLTYPE get_MemoryUsageTargetLevel(
        COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MemoryUsageTargetLevel(
        COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL value) = 0;
};

struct ICoreWebView2_20 : ICoreWebView2_19 {
    virtual HRESULT STDMETHODCALLTYPE get_FrameId(UINT32* value) = 0;
};

struct ICoreWebView2_21 : ICoreWebView2_20 {
    virtual HRESULT STDMETHODCALLTYPE ExecuteScriptWithResult(
        LPCWSTR javaScript,
        ICoreWebView2ExecuteScriptWithResultCompletedHandler* handler) = 0;
};

struct ICoreWebView2_22 : ICoreWebView2_21 {
    virtual HRESULT STDMETHODCALLTYPE
    AddWebResourceRequestedFilterWithRequestSourceKinds(
        LPCWSTR uri, COREWEBVIEW2_WEB_RESOURCE_CONTEXT ResourceContext,
        COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS requestSourceKinds) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    RemoveWebResourceRequestedFilterWithRequestSourceKinds(
        LPCWSTR uri, COREWEBVIEW2_WEB_RESOURCE_CONTEXT ResourceContext,
        COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS requestSourceKinds) = 0;
};

struct ICoreWebView2Settings : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    get_IsScriptEnabled(BOOL* isScriptEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_IsScriptEnabled(BOOL isScriptEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_IsWebMessageEnabled(BOOL* isWebMessageEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_IsWebMessageEnabled(BOOL isWebMessageEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AreDefaultScriptDialogsEnabled(
        BOOL* areDefaultScriptDialogsEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_AreDefaultScriptDialogsEnabled(BOOL areDefaultScriptDialogsEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_IsStatusBarEnabled(BOOL* isStatusBarEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_IsStatusBarEnabled(BOOL isStatusBarEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_AreDevToolsEnabled(BOOL* areDevToolsEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_AreDevToolsEnabled(BOOL areDevToolsEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_AreDefaultContextMenusEnabled(BOOL* enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_AreDefaultContextMenusEnabled(BOOL enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_AreHostObjectsAllowed(BOOL* allowed) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_AreHostObjectsAllowed(BOOL allowed) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_IsZoomControlEnabled(BOOL* enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_IsZoomControlEnabled(BOOL enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_IsBuiltInErrorPageEnabled(BOOL* enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_IsBuiltInErrorPageEnabled(BOOL enabled) = 0;
};

struct ICoreWebView2Settings2 : ICoreWebView2Settings {
    virtual HRESULT STDMETHODCALLTYPE get_UserAgent(LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UserAgent(LPCWSTR value) = 0;
};

struct ICoreWebView2Settings3 : ICoreWebView2Settings2 {
    virtual HRESULT STDMETHODCALLTYPE
    get_AreBrowserAcceleratorKeysEnabled(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_AreBrowserAcceleratorKeysEnabled(BOOL value) = 0;
};

#define WEBVIEW2_AUTOFILL_METHODS                                    \
    virtual HRESULT STDMETHODCALLTYPE get_IsPasswordAutosaveEnabled( \
        BOOL* value) = 0;                                            \
    virtual HRESULT STDMETHODCALLTYPE put_IsPasswordAutosaveEnabled( \
        BOOL value) = 0;                                             \
    virtual HRESULT STDMETHODCALLTYPE get_IsGeneralAutofillEnabled(  \
        BOOL* value) = 0;                                            \
    virtual HRESULT STDMETHODCALLTYPE put_IsGeneralAutofillEnabled(  \
        BOOL value) = 0;

struct ICoreWebView2Settings4 : ICoreWebView2Settings3 {
    WEBVIEW2_AUTOFILL_METHODS
};

struct ICoreWebView2Settings5 : ICoreWebView2Settings4 {
    virtual HRESULT STDMETHODCALLTYPE get_IsPinchZoomEnabled(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsPinchZoomEnabled(BOOL value) = 0;
};

struct ICoreWebView2Settings6 : ICoreWebView2Settings5 {
    virtual HRESULT STDMETHODCALLTYPE
    get_IsSwipeNavigationEnabled(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_IsSwipeNavigationEnabled(BOOL value) = 0;
};

struct ICoreWebView2Settings7 : ICoreWebView2Settings6 {
    virtual HRESULT STDMETHODCALLTYPE
    get_HiddenPdfToolbarItems(COREWEBVIEW2_PDF_TOOLBAR_ITEMS* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_HiddenPdfToolbarItems(COREWEBVIEW2_PDF_TOOLBAR_ITEMS value) = 0;
};

struct ICoreWebView2Settings8 : ICoreWebView2Settings7 {
    virtual HRESULT STDMETHODCALLTYPE
    get_IsReputationCheckingRequired(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_IsReputationCheckingRequired(BOOL value) = 0;
};

struct ICoreWebView2Settings9 : ICoreWebView2Settings8 {
    virtual HRESULT STDMETHODCALLTYPE
    get_IsNonClientRegionSupportEnabled(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_IsNonClientRegionSupportEnabled(BOOL value) = 0;
};

struct ICoreWebView2Profile : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_ProfileName(LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_IsInPrivateModeEnabled(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ProfilePath(LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_DefaultDownloadFolderPath(LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_DefaultDownloadFolderPath(LPCWSTR value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_PreferredColorScheme(COREWEBVIEW2_PREFERRED_COLOR_SCHEME* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_PreferredColorScheme(COREWEBVIEW2_PREFERRED_COLOR_SCHEME value) = 0;
};

struct ICoreWebView2Profile2 : ICoreWebView2Profile {
    virtual HRESULT STDMETHODCALLTYPE ClearBrowsingData(
        COREWEBVIEW2_BROWSING_DATA_KINDS dataKinds,
        ICoreWebView2ClearBrowsingDataCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearBrowsingDataInTimeRange(
        COREWEBVIEW2_BROWSING_DATA_KINDS dataKinds, double startTime,
        double endTime,
        ICoreWebView2ClearBrowsingDataCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearBrowsingDataAll(
        ICoreWebView2ClearBrowsingDataCompletedHandler* handler) = 0;
};

struct ICoreWebView2Profile3 : ICoreWebView2Profile2 {
    virtual HRESULT STDMETHODCALLTYPE get_PreferredTrackingPreventionLevel(
        COREWEBVIEW2_TRACKING_PREVENTION_LEVEL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PreferredTrackingPreventionLevel(
        COREWEBVIEW2_TRACKING_PREVENTION_LEVEL value) = 0;
};

struct ICoreWebView2Profile4 : ICoreWebView2Profile3 {
    virtual HRESULT STDMETHODCALLTYPE SetPermissionState(
        COREWEBVIEW2_PERMISSION_KIND permissionKind, LPCWSTR origin,
        COREWEBVIEW2_PERMISSION_STATE state,
        ICoreWebView2SetPermissionStateCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNonDefaultPermissionSettings(
        ICoreWebView2GetNonDefaultPermissionSettingsCompletedHandler*
            handler) = 0;
};

struct ICoreWebView2Profile5 : ICoreWebView2Profile4 {
    virtual HRESULT STDMETHODCALLTYPE
    get_CookieManager(ICoreWebView2CookieManager** value) = 0;
};

struct ICoreWebView2Profile6 : ICoreWebView2Profile5 {
    WEBVIEW2_AUTOFILL_METHODS
};

struct ICoreWebView2Profile7 : ICoreWebView2Profile6 {
    virtual HRESULT STDMETHODCALLTYPE AddBrowserExtension(
        LPCWSTR extensionFolderPath,
        ICoreWebView2ProfileAddBrowserExtensionCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBrowserExtensions(
        ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler* handler) = 0;
};

struct ICoreWebView2ProfileAddBrowserExtensionCompletedHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    Invoke(HRESULT errorCode, ICoreWebView2BrowserExtension* result) = 0;
};

struct ICoreWebView2DownloadOperation : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE add_BytesReceivedChanged(
        ICoreWebView2BytesReceivedChangedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_BytesReceivedChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_EstimatedEndTimeChanged(
        ICoreWebView2EstimatedEndTimeChangedEventHandler* eventHandler,
        EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_EstimatedEndTimeChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    add_StateChanged(ICoreWebView2StateChangedEventHandler* eventHandler,
                     EventRegistrationToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    remove_StateChanged(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR* uri) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_ContentDisposition(LPWSTR* contentDisposition) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MimeType(LPWSTR* mimeType) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_TotalBytesToReceive(INT64* totalBytesToReceive) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_BytesReceived(INT64* bytesReceived) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_EstimatedEndTime(LPWSTR* estimatedEndTime) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_ResultFilePath(LPWSTR* resultFilePath) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_State(COREWEBVIEW2_DOWNLOAD_STATE* downloadState) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InterruptReason(
        COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON* interruptReason) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Pause() = 0;
    virtual HRESULT STDMETHODCALLTYPE Resume() = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CanResume(BOOL* canResume) = 0;
};

struct ICoreWebView2DownloadStartingEventArgs : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_DownloadOperation(
        ICoreWebView2DownloadOperation** downloadOperation) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Cancel(BOOL* cancel) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Cancel(BOOL cancel) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_ResultFilePath(LPWSTR* resultFilePath) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_ResultFilePath(LPCWSTR resultFilePath) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Handled(BOOL* handled) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Handled(BOOL handled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    GetDeferral(ICoreWebView2Deferral** deferral) = 0;
};

struct ICoreWebView2DownloadStartingEventHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    Invoke(ICoreWebView2* sender,
           ICoreWebView2DownloadStartingEventArgs* args) = 0;
};

struct ICoreWebView2StateChangedEventHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    Invoke(ICoreWebView2DownloadOperation* sender, IUnknown* args) = 0;
};

struct ICoreWebView2Cookie : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Name(LPWSTR* name) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Value(LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Value(LPCWSTR value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Domain(LPWSTR* domain) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Path(LPWSTR* path) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Expires(double* expires) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Expires(double expires) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsHttpOnly(BOOL* isHttpOnly) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsHttpOnly(BOOL isHttpOnly) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND* sameSite) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND sameSite) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsSecure(BOOL* isSecure) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsSecure(BOOL isSecure) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsSession(BOOL* isSession) = 0;
};

struct ICoreWebView2CookieList : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Count(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    GetValueAtIndex(UINT32 index, ICoreWebView2Cookie** value) = 0;
};

struct ICoreWebView2CookieManager : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    CreateCookie(LPCWSTR name, LPCWSTR value, LPCWSTR domain, LPCWSTR path,
                 ICoreWebView2Cookie** cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyCookie(
        ICoreWebView2Cookie* cookieParam, ICoreWebView2Cookie** cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCookies(
        LPCWSTR uri, ICoreWebView2GetCookiesCompletedHandler* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    AddOrUpdateCookie(ICoreWebView2Cookie* cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    DeleteCookie(ICoreWebView2Cookie* cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE DeleteCookies(LPCWSTR name,
                                                    LPCWSTR uri) = 0;
    virtual HRESULT STDMETHODCALLTYPE DeleteCookiesWithDomainAndPath(
        LPCWSTR name, LPCWSTR domain, LPCWSTR path) = 0;
    virtual HRESULT STDMETHODCALLTYPE DeleteAllCookies() = 0;
};

struct ICoreWebView2GetCookiesCompletedHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    Invoke(HRESULT errorCode, ICoreWebView2CookieList* result) = 0;
};

struct ICoreWebView2WebMessageReceivedEventArgs : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Source(LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_WebMessageAsJson(LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    TryGetWebMessageAsString(LPWSTR* value) = 0;
};

struct ICoreWebView2WebResourceRequestedEventArgs : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    get_Request(ICoreWebView2WebResourceRequest** request) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_Response(ICoreWebView2WebResourceResponse** response) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_Response(ICoreWebView2WebResourceResponse* response) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    GetDeferral(ICoreWebView2Deferral** deferral) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_ResourceContext(COREWEBVIEW2_WEB_RESOURCE_CONTEXT* context) = 0;
};

struct ICoreWebView2WebResourceRequest : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR* uri) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Uri(LPCWSTR uri) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Method(LPWSTR* method) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Method(LPCWSTR method) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Content(IStream** content) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Content(IStream* content) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_Headers(ICoreWebView2HttpRequestHeaders** headers) = 0;
};

struct ICoreWebView2WebResourceResponse : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Content(IStream** content) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Content(IStream* content) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_Headers(ICoreWebView2HttpResponseHeaders** headers) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_StatusCode(int* statusCode) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_StatusCode(int statusCode) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_ReasonPhrase(LPWSTR* reasonPhrase) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_ReasonPhrase(LPCWSTR reasonPhrase) = 0;
};

struct ICoreWebView2HttpRequestHeaders : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetHeader(LPCWSTR name,
                                                LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetHeaders(
        LPCWSTR name, ICoreWebView2HttpHeadersCollectionIterator** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE Contains(LPCWSTR name, BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetHeader(LPCWSTR name,
                                                LPCWSTR value) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveHeader(LPCWSTR name) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    GetIterator(ICoreWebView2HttpHeadersCollectionIterator** value) = 0;
};

struct ICoreWebView2HttpHeadersCollectionIterator : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCurrentHeader(LPWSTR* name,
                                                       LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_HasCurrentHeader(BOOL* hasCurrent) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveNext(BOOL* hasNext) = 0;
};

struct ICoreWebView2Deferral : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Complete() = 0;
};

struct ICoreWebView2NavigationStartingEventArgs : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR* uri) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_IsUserInitiated(BOOL* isUserInitiated) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRedirected(BOOL* isRedirected) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_RequestHeaders(ICoreWebView2HttpRequestHeaders** requestHeaders) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Cancel(BOOL* cancel) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Cancel(BOOL cancel) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_NavigationId(UINT64* navigationId) = 0;
};

struct ICoreWebView2NewWindowRequestedEventArgs : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR* uri) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_NewWindow(ICoreWebView2* newWindow) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_NewWindow(ICoreWebView2** newWindow) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Handled(BOOL handled) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Handled(BOOL* handled) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_IsUserInitiated(BOOL* isUserInitiated) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    GetDeferral(ICoreWebView2Deferral** deferral) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_WindowFeatures(ICoreWebView2WindowFeatures** value) = 0;
};

struct ICoreWebView2WindowFeatures : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_HasPosition(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HasSize(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Left(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Top(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Height(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Width(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ShouldDisplayMenuBar(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ShouldDisplayStatus(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ShouldDisplayToolbar(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_ShouldDisplayScrollBars(BOOL* value) = 0;
};

struct ICoreWebView2PermissionRequestedEventArgs : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR* uri) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_PermissionKind(COREWEBVIEW2_PERMISSION_KIND* permissionKind) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_IsUserInitiated(BOOL* isUserInitiated) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_State(COREWEBVIEW2_PERMISSION_STATE* state) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_State(COREWEBVIEW2_PERMISSION_STATE state) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    GetDeferral(ICoreWebView2Deferral** deferral) = 0;
};

struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    Invoke(HRESULT errorCode, ICoreWebView2Environment* result) = 0;
};

struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    Invoke(HRESULT errorCode, ICoreWebView2Controller* result) = 0;
};

struct ICoreWebView2WebMessageReceivedEventHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    Invoke(ICoreWebView2* sender,
           ICoreWebView2WebMessageReceivedEventArgs* args) = 0;
};

struct ICoreWebView2WebResourceRequestedEventHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    Invoke(ICoreWebView2* sender,
           ICoreWebView2WebResourceRequestedEventArgs* args) = 0;
};

struct ICoreWebView2NavigationStartingEventHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    Invoke(ICoreWebView2* sender,
           ICoreWebView2NavigationStartingEventArgs* args) = 0;
};

struct ICoreWebView2NavigationCompletedEventHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    Invoke(ICoreWebView2* sender,
           ICoreWebView2NavigationCompletedEventArgs* args) = 0;
};

struct ICoreWebView2ContentLoadingEventHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2* sender, ICoreWebView2ContentLoadingEventArgs* args) = 0;
};

struct ICoreWebView2ProcessFailedEventArgs : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_ProcessFailedKind(int* kind) = 0;
};

struct ICoreWebView2ProcessFailedEventHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) = 0;
};

struct ICoreWebView2DocumentTitleChangedEventHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                             IUnknown* args) = 0;
};

struct ICoreWebView2NewWindowRequestedEventHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    Invoke(ICoreWebView2* sender,
           ICoreWebView2NewWindowRequestedEventArgs* args) = 0;
};

struct ICoreWebView2WindowCloseRequestedEventHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                             IUnknown* args) = 0;
};

struct ICoreWebView2PermissionRequestedEventHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    Invoke(ICoreWebView2* sender,
           ICoreWebView2PermissionRequestedEventArgs* args) = 0;
};

struct ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler
    : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode,
                                             LPCWSTR result) = 0;
};

struct ICoreWebView2ExecuteScriptCompletedHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode,
                                             LPCWSTR result) = 0;
};

struct ICoreWebView2ClearBrowsingDataCompletedHandler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode) = 0;
};

struct ICoreWebView2EnvironmentOptions : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    get_AdditionalBrowserArguments(LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_AdditionalBrowserArguments(LPCWSTR value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_TargetCompatibleBrowserVersion(LPWSTR* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_TargetCompatibleBrowserVersion(LPCWSTR value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* allow) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL allow) = 0;
};

struct ICoreWebView2EnvironmentOptions2 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    get_ExclusiveUserDataFolderAccess(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_ExclusiveUserDataFolderAccess(BOOL value) = 0;
};

struct ICoreWebView2EnvironmentOptions3 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    get_IsCustomCrashReportingEnabled(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_IsCustomCrashReportingEnabled(BOOL value) = 0;
};

struct ICoreWebView2EnvironmentOptions4 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    GetCustomSchemeRegistrations(UINT32* count, IUnknown*** values) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    SetCustomSchemeRegistrations(UINT32 count, IUnknown** values) = 0;
};

struct ICoreWebView2EnvironmentOptions5 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    get_EnableTrackingPrevention(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_EnableTrackingPrevention(BOOL value) = 0;
};

struct ICoreWebView2EnvironmentOptions6 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    get_AreBrowserExtensionsEnabled(BOOL* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_AreBrowserExtensionsEnabled(BOOL value) = 0;
};

struct ICoreWebView2EnvironmentOptions7 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    get_ChannelSearchKind(COREWEBVIEW2_CHANNEL_SEARCH_KIND* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_ChannelSearchKind(COREWEBVIEW2_CHANNEL_SEARCH_KIND value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_ReleaseChannels(COREWEBVIEW2_RELEASE_CHANNELS* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_ReleaseChannels(COREWEBVIEW2_RELEASE_CHANNELS value) = 0;
};

struct ICoreWebView2EnvironmentOptions8 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    get_ScrollBarStyle(COREWEBVIEW2_SCROLLBAR_STYLE* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    put_ScrollBarStyle(COREWEBVIEW2_SCROLLBAR_STYLE value) = 0;
};

enum : uint8_t {
    kCookieSameSiteNone = 0,
    kCookieSameSiteLax = 1,
    kCookieSameSiteStrict = 2,
    kDownloadStateInProgress = 0,
    kDownloadStateCompleted = 2,
    kMoveFocusReasonProgrammatic = 0,
    kPermissionKindClipboardRead = 6,
    kPermissionStateAllow = 1,
    kPreferredColorSchemeAuto = 0,
    kPreferredColorSchemeLight = 1,
    kPreferredColorSchemeDark = 2,
    kScrollBarStyleDefault = 0,
    kScrollBarStyleFluentOverlay = 1,
    kWebResourceContextAll = 0,
    kMemoryUsageTargetLevelNormal = 0,
    kMemoryUsageTargetLevelLow = 1,
};

static const unsigned kWebResourceRequestSourceKindsAll = 0xffffffffu;

static const UINT kParentSubclassId = WM_USER + 0x64;
static const UINT kParentDestroyMessage = WM_USER + 0x65;
static const UINT kMainThreadDispatcherSubclassId = WM_USER + 0x66;

static UINT ExecMsgId() {
    static UINT id = RegisterWindowMessageA("Wry::ExecMsg");
    return id;
}

static Str WstrToUtf8Temp(const WCHAR* ws, int wlen = -1) {
    if (!ws) {
        return {};
    }
    if (wlen < 0) {
        wlen = (int)wcslen(ws);
    }
    if (wlen == 0) {
        return StrL("");
    }
    int n =
        WideCharToMultiByte(CP_UTF8, 0, ws, wlen, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    Str res = AllocStrTemp(n + 1);
    if (!res.s) {
        return {};
    }
    WideCharToMultiByte(CP_UTF8, 0, ws, wlen, res.s, n, nullptr, nullptr);
    res.s[n] = 0;
    res.len = n;
    return res;
}

static Str TakePwstrTemp(LPWSTR p) {
    if (!p) {
        return {};
    }
    Str res = WstrToUtf8Temp(p);
    CoTaskMemFree(p);
    return res;
}

static WCHAR* CoTaskMemDupW(const WCHAR* s) {
    if (!s) {
        s = L"";
    }
    size_t n = wcslen(s) + 1;
    WCHAR* res = (WCHAR*)CoTaskMemAlloc(n * sizeof(WCHAR));
    if (res) {
        memcpy(res, s, n * sizeof(WCHAR));
    }
    return res;
}

static WCHAR* WStrDup(const WCHAR* s) {
    if (!s) {
        return nullptr;
    }
    size_t n = wcslen(s) + 1;
    WCHAR* res = (WCHAR*)malloc(n * sizeof(WCHAR));
    if (res) {
        memcpy(res, s, n * sizeof(WCHAR));
    }
    return res;
}

static WCHAR* WStrDupUtf8(Str s) {
    if (len(s) == 0) {
        return WStrDup(L"");
    }
    return WStrDup(ToCWstrTemp(s));
}

static WCHAR* EnvironmentVariableDup(const WCHAR* name) {
    DWORD cap = GetEnvironmentVariableW(name, nullptr, 0);
    if (cap == 0) {
        return nullptr;
    }
    WCHAR* value = (WCHAR*)malloc((size_t)cap * sizeof(WCHAR));
    if (!value) {
        return nullptr;
    }
    DWORD len = GetEnvironmentVariableW(name, value, cap);
    if (len == 0 || len >= cap) {
        free(value);
        return nullptr;
    }
    return value;
}

static int LoaderOverrideIds(WCHAR appId[256], WCHAR exeName[MAX_PATH],
                             const WCHAR* ids[3]) {
    appId[0] = 0;
    exeName[0] = 0;
    typedef LONG(WINAPI * GetCurrentApplicationUserModelIdFn)(UINT32*, PWSTR);
    auto getCurrentApplicationUserModelId =
        (GetCurrentApplicationUserModelIdFn)GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"),
            "GetCurrentApplicationUserModelId");
    if (getCurrentApplicationUserModelId) {
        UINT32 len = 256;
        if (getCurrentApplicationUserModelId(&len, appId) != ERROR_SUCCESS) {
            appId[0] = 0;
        }
    }
    if (appId[0] == 0) {
        typedef HRESULT(WINAPI *
                        GetCurrentProcessExplicitAppUserModelIdFn)(PWSTR*);
        HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
        auto getExplicit =
            shell32 ? (GetCurrentProcessExplicitAppUserModelIdFn)GetProcAddress(
                          shell32, "GetCurrentProcessExplicitAppUserModelID")
                    : nullptr;
        PWSTR explicitId = nullptr;
        if (getExplicit && SUCCEEDED(getExplicit(&explicitId)) && explicitId) {
            wcscpy_s(appId, 256, explicitId);
        }
        CoTaskMemFree(explicitId);
    }

    WCHAR module[MAX_PATH * 2];
    DWORD len = GetModuleFileNameW(nullptr, module,
                                   (DWORD)(sizeof(module) / sizeof(module[0])));
    if (len > 0 && len < sizeof(module) / sizeof(module[0])) {
        const WCHAR* slash = wcsrchr(module, L'\\');
        wcscpy_s(exeName, MAX_PATH, slash ? slash + 1 : module);
    }

    int count = 0;
    if (appId[0] != 0) {
        ids[count++] = appId;
    }
    if (exeName[0] != 0) {
        ids[count++] = exeName;
    }
    ids[count++] = L"*";
    return count;
}

static WCHAR* RegistryValueDup(HKEY root, const WCHAR* keyPath,
                               const WCHAR* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, keyPath, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return nullptr;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status =
        RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
    WCHAR* result = nullptr;
    if (status == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(WCHAR)) {
        result = (WCHAR*)malloc((size_t)bytes + sizeof(WCHAR));
        if (result &&
            RegQueryValueExW(key, valueName, nullptr, &type, (BYTE*)result,
                             &bytes) == ERROR_SUCCESS) {
            result[bytes / sizeof(WCHAR)] = 0;
        } else {
            free(result);
            result = nullptr;
        }
    } else if (status == ERROR_SUCCESS && type == REG_DWORD &&
               bytes == sizeof(DWORD)) {
        DWORD value = 0;
        if (RegQueryValueExW(key, valueName, nullptr, &type, (BYTE*)&value,
                             &bytes) == ERROR_SUCCESS) {
            result = (WCHAR*)malloc(16 * sizeof(WCHAR));
            if (result) {
                swprintf_s(result, 16, L"%u", value);
            }
        }
    }
    RegCloseKey(key);
    return result;
}

static WCHAR* PolicyOverrideDup(const WCHAR* property) {
    WCHAR appId[256];
    WCHAR exeName[MAX_PATH];
    const WCHAR* ids[3];
    int idCount = LoaderOverrideIds(appId, exeName, ids);
    const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};

    for (int r = 0; r < 2; r++) {
        WCHAR key[MAX_PATH];
        swprintf_s(key, L"Software\\Policies\\Microsoft\\Edge\\WebView2\\%s",
                   property);
        for (int i = 0; i < idCount; i++) {
            WCHAR* result = RegistryValueDup(roots[r], key, ids[i]);
            if (result) {
                return result;
            }
        }
    }
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < idCount; i++) {
            WCHAR key[MAX_PATH];
            swprintf_s(key,
                       L"Software\\Policies\\Microsoft\\EmbeddedBrowserWebView"
                       L"\\LoaderOverride\\%s",
                       ids[i]);
            WCHAR* result = RegistryValueDup(roots[r], key, property);
            if (result) {
                return result;
            }
        }
    }
    return nullptr;
}

static WCHAR* LoaderOverrideDup(const WCHAR* environment,
                                const WCHAR* property) {
    WCHAR* result = EnvironmentVariableDup(environment);
    if (!result) {
        result = PolicyOverrideDup(property);
    }
    if (result && result[0] == 0) {
        free(result);
        result = nullptr;
    }
    return result;
}

static const UINT kBaseDpi = 96;

static double DpiToScaleFactor(UINT dpi) {
    return (double)dpi / (double)kBaseDpi;
}

static UINT HwndDpi(HWND hwnd) {
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForWindowFn getDpiForWindow = nullptr;
    static GetDpiForMonitorFn getDpiForMonitor = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        HMODULE user32 = LoadLibraryW(L"user32.dll");
        if (user32) {
            getDpiForWindow =
                (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
        }
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) {
            getDpiForMonitor =
                (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor");
        }
    }
    if (getDpiForWindow) {
        UINT dpi = getDpiForWindow(hwnd);
        return dpi == 0 ? kBaseDpi : dpi;
    }
    if (getDpiForMonitor) {
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (!mon) {
            return kBaseDpi;
        }
        UINT dpiX = 0;
        UINT dpiY = 0;

        if (getDpiForMonitor(mon, 0, &dpiX, &dpiY) == S_OK) {
            return dpiX;
        }
    }
    return kBaseDpi;
}

static int ToPhysical(double logicalOrPhysical, bool logical, double scale) {
    double v = logical ? logicalOrPhysical * scale : logicalOrPhysical;

    return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

struct RuntimeChannel {
    const WCHAR* id;
    const WCHAR* name;
    const WCHAR* packageFamily;
    int mask;
};

static const RuntimeChannel kRuntimeChannels[] = {
    {L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}", L"",
     L"Microsoft.WebView2Runtime.Stable_8wekyb3d8bbwe", 1},
    {L"{2CD8A007-E189-409D-A2C8-9AF4EF3C72AA}", L"beta",
     L"Microsoft.WebView2Runtime.Beta_8wekyb3d8bbwe", 2},
    {L"{0D50BFEC-CD6A-4F9A-964C-C7416E3ACB10}", L"dev",
     L"Microsoft.WebView2Runtime.Dev_8wekyb3d8bbwe", 4},
    {L"{65C35B14-6C1D-4122-AC46-7148CC9D6497}", L"canary",
     L"Microsoft.WebView2Runtime.Canary_8wekyb3d8bbwe", 8},
};

static bool RegReadStr(HKEY root, const WCHAR* subKey, const WCHAR* name,
                       DWORD extraFlags, WCHAR* out, DWORD outChars) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | extraFlags, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD cb = outChars * sizeof(WCHAR);
    LSTATUS st = RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)out, &cb);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_SZ || cb < sizeof(WCHAR)) {
        return false;
    }
    out[(cb / sizeof(WCHAR)) - 1] = 0;
    return out[0] != 0;
}

struct RuntimeInfo {
    WCHAR version[64];
    WCHAR clientDll[MAX_PATH * 2];

    int runtimeType;
};

static const uint32_t kMinimumCompatibleRuntimeVersion[4] = {86, 0, 616, 0};

static bool ParseRuntimeVersion(const WCHAR* text, uint32_t version[4]) {
    if (!text) {
        return false;
    }
    for (int part = 0; part < 4; part++) {
        if (*text < L'0' || *text > L'9') {
            return false;
        }
        uint32_t value = 0;
        do {
            uint32_t digit = (uint32_t)(*text - L'0');
            if (value > (UINT32_MAX - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
            text++;
        } while (*text >= L'0' && *text <= L'9');
        version[part] = value;
        if (part < 3) {
            if (*text != L'.') {
                return false;
            }
            text++;
        }
    }
    return *text == 0;
}

static bool IsCompatibleInstalledRuntime(const WCHAR* versionText) {
    uint32_t version[4];
    if (!ParseRuntimeVersion(versionText, version)) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (version[i] != kMinimumCompatibleRuntimeVersion[i]) {
            return version[i] > kMinimumCompatibleRuntimeVersion[i];
        }
    }
    return true;
}

static const WCHAR* ArchFolder() {
#if defined(_M_ARM64)
    return L"arm64";
#elif defined(_M_X64) || defined(__x86_64__)
    return L"x64";
#else
    return L"x86";
#endif
}

static bool IsAbsoluteWindowsPath(const WCHAR* path) {
    if (!path) {
        return false;
    }

    bool drive = path[0] != 0 && path[1] == L':' &&
                 (path[2] == L'\\' || path[2] == L'/');
    bool unc = (path[0] == L'\\' || path[0] == L'/') &&
               (path[1] == L'\\' || path[1] == L'/');
    return drive || unc;
}

static WCHAR* FixedRuntimeFolderDup(const WCHAR* folder) {
    if (IsAbsoluteWindowsPath(folder)) {
        return WStrDup(folder);
    }
    WCHAR module[MAX_PATH * 2];
    DWORD len = GetModuleFileNameW(nullptr, module,
                                   (DWORD)(sizeof(module) / sizeof(module[0])));
    if (len == 0 || len >= sizeof(module) / sizeof(module[0])) {
        return nullptr;
    }
    WCHAR* slash = wcsrchr(module, L'\\');
    if (!slash) {
        return nullptr;
    }
    size_t prefixLen = (size_t)(slash - module) + 1;
    size_t folderLen = wcslen(folder);
    WCHAR* result = (WCHAR*)malloc((prefixLen + folderLen + 1) * sizeof(WCHAR));
    if (!result) {
        return nullptr;
    }
    memcpy(result, module, prefixLen * sizeof(WCHAR));
    memcpy(result + prefixLen, folder, (folderLen + 1) * sizeof(WCHAR));
    return result;
}

static bool FileVersion(const WCHAR* path, WCHAR* out, int outChars) {
    typedef DWORD(WINAPI * GetFileVersionInfoSizeWFn)(LPCWSTR, LPDWORD);
    typedef BOOL(WINAPI * GetFileVersionInfoWFn)(LPCWSTR, DWORD, DWORD, LPVOID);
    typedef BOOL(WINAPI * VerQueryValueWFn)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

    HMODULE versionDll = LoadLibraryW(L"version.dll");
    if (!versionDll) {
        return false;
    }
    auto getSize = (GetFileVersionInfoSizeWFn)GetProcAddress(
        versionDll, "GetFileVersionInfoSizeW");
    auto getInfo = (GetFileVersionInfoWFn)GetProcAddress(versionDll,
                                                         "GetFileVersionInfoW");
    auto query = (VerQueryValueWFn)GetProcAddress(versionDll, "VerQueryValueW");
    bool ok = false;
    if (getSize && getInfo && query) {
        DWORD ignored = 0;
        DWORD size = getSize(path, &ignored);
        uint8_t* data = size > 0 ? new uint8_t[size] : nullptr;
        if (data && getInfo(path, 0, size, data)) {
            VS_FIXEDFILEINFO* info = nullptr;
            UINT infoSize = 0;
            if (query(data, L"\\", (void**)&info, &infoSize) && info &&
                infoSize >= sizeof(*info) &&
                info->dwSignature == VS_FFI_SIGNATURE) {
                int n = swprintf_s(out, (size_t)outChars, L"%u.%u.%u.%u",
                                   HIWORD(info->dwProductVersionMS),
                                   LOWORD(info->dwProductVersionMS),
                                   HIWORD(info->dwProductVersionLS),
                                   LOWORD(info->dwProductVersionLS));
                ok = n > 0;
            }
        }
        delete[] data;
    }
    FreeLibrary(versionDll);
    return ok;
}

static bool RuntimeVersionAndLocation(const WCHAR* id, WCHAR* version,
                                      DWORD versionChars, WCHAR* location,
                                      DWORD locationChars) {
    struct Where {
        HKEY root;
        const WCHAR* path;
        DWORD flags;
    };
    const Where places[] = {
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\",
         KEY_WOW64_32KEY},
        {HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\", 0},
    };
    for (int i = 0; i < (int)(sizeof(places) / sizeof(places[0])); i++) {
        WCHAR key[256];
        wcscpy_s(key, places[i].path);
        wcscat_s(key, id);
        if (!RegReadStr(places[i].root, key, L"pv", places[i].flags, version,
                        versionChars)) {
            continue;
        }

        if (wcscmp(version, L"0.0.0.0") == 0) {
            continue;
        }
        if (RegReadStr(places[i].root, key, L"location", places[i].flags,
                       location, locationChars)) {
            return true;
        }
    }
    return false;
}

static bool FindPackagedRuntime(const RuntimeChannel* channel,
                                RuntimeInfo* out) {
    typedef LONG(WINAPI * TryCreatePackageDependencyFn)(
        PSID user, PCWSTR packageFamilyName, uint64_t minVersion,
        int architectures, int lifetimeKind, PCWSTR lifetimeArtifact,
        int options, PWSTR* dependencyId);
    typedef LONG(WINAPI * AddPackageDependencyFn)(PCWSTR dependencyId, int rank,
                                                  int options, void** context,
                                                  PWSTR* packageFullName);
    typedef LONG(WINAPI * GetPackagePathByFullNameFn)(
        PCWSTR packageFullName, UINT32 * pathLength, PWSTR path);

    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE apiModule = kernelBase ? kernelBase : kernel32;
    auto tryCreate = apiModule ? (TryCreatePackageDependencyFn)GetProcAddress(
                                     apiModule, "TryCreatePackageDependency")
                               : nullptr;
    auto add = apiModule ? (AddPackageDependencyFn)GetProcAddress(
                               apiModule, "AddPackageDependency")
                         : nullptr;
    auto getPath = kernel32 ? (GetPackagePathByFullNameFn)GetProcAddress(
                                  kernel32, "GetPackagePathByFullName")
                            : nullptr;
    if (!tryCreate || !add || !getPath) {
        return false;
    }

    PWSTR dependencyId = nullptr;
    LONG status = tryCreate(nullptr, channel->packageFamily, 0, 0, 0, nullptr,
                            0, &dependencyId);
    if (status != ERROR_SUCCESS || !dependencyId) {
        return false;
    }
    void* dependencyContext = nullptr;
    PWSTR packageFullName = nullptr;
    status = add(dependencyId, 0, 0, &dependencyContext, &packageFullName);
    HeapFree(GetProcessHeap(), 0, dependencyId);
    if (status != ERROR_SUCCESS || !packageFullName) {
        HeapFree(GetProcessHeap(), 0, packageFullName);
        return false;
    }

    UINT32 pathChars = 0;
    status = getPath(packageFullName, &pathChars, nullptr);
    WCHAR* packagePath = nullptr;
    if (status == ERROR_INSUFFICIENT_BUFFER && pathChars > 0) {
        packagePath = (WCHAR*)malloc((size_t)pathChars * sizeof(WCHAR));
        if (packagePath) {
            status = getPath(packageFullName, &pathChars, packagePath);
        }
    }
    HeapFree(GetProcessHeap(), 0, packageFullName);
    if (!packagePath || status != ERROR_SUCCESS) {
        free(packagePath);
        return false;
    }

    int written = swprintf_s(out->clientDll,
                             L"%s\\EBWebView\\%s\\EmbeddedBrowserWebView.dll",
                             packagePath, ArchFolder());
    free(packagePath);
    if (written < 0 ||
        GetFileAttributesW(out->clientDll) == INVALID_FILE_ATTRIBUTES ||
        !FileVersion(out->clientDll, out->version,
                     (int)(sizeof(out->version) / sizeof(out->version[0]))) ||
        !IsCompatibleInstalledRuntime(out->version)) {
        return false;
    }
    if (channel->name[0] != 0) {
        wcscat_s(out->version, L" ");
        wcscat_s(out->version, channel->name);
    }
    return true;
}

static bool FindInstalledRuntime(const RuntimeChannel* channel,
                                 RuntimeInfo* out) {
    struct Place {
        HKEY root;
        DWORD flags;
    };
    const Place places[] = {
        {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY},
        {HKEY_CURRENT_USER, KEY_WOW64_32KEY},
    };
    WCHAR key[256];
    wcscpy_s(key, L"SOFTWARE\\Microsoft\\EdgeUpdate\\ClientState\\");
    wcscat_s(key, channel->id);
    for (int i = 0; i < (int)(sizeof(places) / sizeof(places[0])); i++) {
        WCHAR folder[MAX_PATH * 2];
        if (!RegReadStr(places[i].root, key, L"EBWebView", places[i].flags,
                        folder, (DWORD)(sizeof(folder) / sizeof(folder[0])))) {
            continue;
        }
        const WCHAR* version = wcsrchr(folder, L'\\');
        version = version ? version + 1 : folder;
        if (!IsCompatibleInstalledRuntime(version)) {
            continue;
        }
        int written = swprintf_s(
            out->clientDll, L"%s\\EBWebView\\%s\\EmbeddedBrowserWebView.dll",
            folder, ArchFolder());
        if (written < 0 ||
            GetFileAttributesW(out->clientDll) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        wcscpy_s(out->version, version);
        if (out->version[0] != 0 && channel->name[0] != 0) {
            wcscat_s(out->version, L" ");
            wcscat_s(out->version, channel->name);
        }
        return true;
    }

    WCHAR location[MAX_PATH * 2];
    WCHAR version[64];
    if (!RuntimeVersionAndLocation(
            channel->id, version, (DWORD)(sizeof(version) / sizeof(version[0])),
            location, (DWORD)(sizeof(location) / sizeof(location[0])))) {
        return FindPackagedRuntime(channel, out);
    }
    if (!IsCompatibleInstalledRuntime(version)) {
        return FindPackagedRuntime(channel, out);
    }
    int written = swprintf_s(
        out->clientDll, L"%s\\%s\\EBWebView\\%s\\EmbeddedBrowserWebView.dll",
        location, version, ArchFolder());
    if (written < 0 ||
        GetFileAttributesW(out->clientDll) == INVALID_FILE_ATTRIBUTES) {
        return FindPackagedRuntime(channel, out);
    }
    wcscpy_s(out->version, version);
    if (channel->name[0] != 0) {
        wcscat_s(out->version, L" ");
        wcscat_s(out->version, channel->name);
    }
    return true;
}

static int ReleaseChannelsFromEnvironment(int fallback) {
    WCHAR* value =
        LoaderOverrideDup(L"WEBVIEW2_RELEASE_CHANNELS", L"ReleaseChannels");
    if (!value) {
        return fallback;
    }
    int result = 0;
    WCHAR* at = value;
    for (;;) {
        WCHAR* end = nullptr;
        long channel = wcstol(at, &end, 10);

        if (end == at || channel < 0 || channel > 3) {
            channel = 0;
        }
        result |= 1 << channel;
        if (!end || *end == 0) {
            break;
        }
        WCHAR* comma = wcschr(end, L',');
        if (!comma) {
            break;
        }
        at = comma + 1;
    }
    free(value);
    return result;
}

static bool LeastStableFromEnvironment(bool fallback) {
    WCHAR* legacy = LoaderOverrideDup(L"WEBVIEW2_RELEASE_CHANNEL_PREFERENCE",
                                      L"ReleaseChannelPreference");
    if (legacy) {
        fallback = wcstol(legacy, nullptr, 10) == 1;
        free(legacy);
    }
    WCHAR* value = LoaderOverrideDup(L"WEBVIEW2_CHANNEL_SEARCH_KIND",
                                     L"ChannelSearchKind");
    if (value) {
        fallback = wcstol(value, nullptr, 10) == 1;
        free(value);
    }
    return fallback;
}

static bool FindRuntime(RuntimeInfo* out, IUnknown* options = nullptr,
                        bool useOverrides = true) {
    out->version[0] = 0;
    out->clientDll[0] = 0;
    out->runtimeType = 0;

    WCHAR* folder =
        useOverrides ? LoaderOverrideDup(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",
                                         L"BrowserExecutableFolder")
                     : nullptr;
    if (folder && folder[0] != 0) {

        WCHAR* resolvedFolder = FixedRuntimeFolderDup(folder);
        free(folder);
        if (!resolvedFolder) {
            return false;
        }
        out->runtimeType = 1;
        int written = swprintf_s(
            out->clientDll, L"%s\\EBWebView\\%s\\EmbeddedBrowserWebView.dll",
            resolvedFolder, ArchFolder());
        if (written < 0 ||
            GetFileAttributesW(out->clientDll) == INVALID_FILE_ATTRIBUTES) {
            free(resolvedFolder);
            return false;
        }
        FileVersion(out->clientDll, out->version,
                    (int)(sizeof(out->version) / sizeof(out->version[0])));
        free(resolvedFolder);
        return true;
    }
    free(folder);

    int releaseChannels = 15;
    bool leastStable = false;
    ICoreWebView2EnvironmentOptions7* options7 = nullptr;
    if (options &&
        SUCCEEDED(options->QueryInterface(
            __uuidof(ICoreWebView2EnvironmentOptions7), (void**)&options7))) {
        int searchKind = 0;
        if (SUCCEEDED(options7->get_ChannelSearchKind(&searchKind))) {
            leastStable = searchKind == 1;
        }
        options7->get_ReleaseChannels(&releaseChannels);
        options7->Release();
    }
    if (releaseChannels == 0) {

        releaseChannels = 15;
    }
    if (useOverrides) {
        releaseChannels = ReleaseChannelsFromEnvironment(releaseChannels);
        leastStable = LeastStableFromEnvironment(leastStable);
    }
    int count = (int)(sizeof(kRuntimeChannels) / sizeof(kRuntimeChannels[0]));
    for (int step = 0; step < count; step++) {
        int i = leastStable ? count - step - 1 : step;
        if ((releaseChannels & kRuntimeChannels[i].mask) != 0 &&
            FindInstalledRuntime(&kRuntimeChannels[i], out)) {
            return true;
        }
    }
    return false;
}

typedef HRESULT(
    STDMETHODCALLTYPE* CreateWebViewEnvironmentWithOptionsInternalFn)(
    BOOL fromClientDll, int runtimeType, PCWSTR userDataFolder,
    IUnknown* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);

struct EnvironmentCreatedRetryHandler
    : ComObject<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
    CreateWebViewEnvironmentWithOptionsInternalFn create = nullptr;
    int runtimeType = 0;
    WCHAR* userDataFolder = nullptr;
    IUnknown* options = nullptr;
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* original =
        nullptr;
    int retries = 1;

    ~EnvironmentCreatedRetryHandler() override {
        free(userDataFolder);
        if (options) {
            options->Release();
        }
        if (original) {
            original->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE
    Invoke(HRESULT result, ICoreWebView2Environment* environment) override {
        if (SUCCEEDED(result) || retries <= 0) {
            return original->Invoke(result, environment);
        }
        retries--;
        HRESULT hr = create(TRUE, runtimeType, userDataFolder, options, this);
        if (FAILED(hr)) {
            return original->Invoke(hr, nullptr);
        }
        return S_OK;
    }
};

static void DefaultUserDataFolder(WCHAR* out, int cap) {
    out[0] = 0;
    DWORD n = GetModuleFileNameW(nullptr, out, (DWORD)cap);
    if (n == 0 || n >= (DWORD)cap - 16) {
        out[0] = 0;
        return;
    }
    wcscat_s(out, (size_t)cap, L".WebView2");
}

static HRESULT CreateEnvironmentWithOptions(
    PCWSTR userDataFolder, IUnknown* options,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler) {
    WCHAR* userDataOverride =
        LoaderOverrideDup(L"WEBVIEW2_USER_DATA_FOLDER", L"UserDataFolder");
    if (userDataOverride) {
        userDataFolder = userDataOverride;
    }
    WCHAR defaultFolder[MAX_PATH * 2];
    if (!userDataFolder || userDataFolder[0] == 0) {
        DefaultUserDataFolder(defaultFolder,
                              (int)(sizeof(defaultFolder) / sizeof(WCHAR)));
        userDataFolder = defaultFolder;
    }
    RuntimeInfo rt;
    if (!FindRuntime(&rt, options)) {
        logf("wry: no WebView2 runtime found\n");
        free(userDataOverride);
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    HMODULE client = LoadLibraryW(rt.clientDll);
    if (!client) {
        DWORD error = GetLastError();
        logf("wry: LoadLibrary of the WebView2 client dll failed, error %d\n",
             (int)error);
        HRESULT hr = HRESULT_FROM_WIN32(error);
        free(userDataOverride);
        return hr;
    }
    auto create = (CreateWebViewEnvironmentWithOptionsInternalFn)GetProcAddress(
        client, "CreateWebViewEnvironmentWithOptionsInternal");
    if (!create) {
        logf(
            "wry: the WebView2 client dll has no "
            "CreateWebViewEnvironmentWithOptionsInternal\n");
        FreeLibrary(client);
        free(userDataOverride);
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
    EnvironmentCreatedRetryHandler* retry =
        new EnvironmentCreatedRetryHandler();
    retry->create = create;
    retry->runtimeType = rt.runtimeType;
    retry->userDataFolder = WStrDup(userDataFolder);
    retry->options = options;
    retry->original = handler;
    if (options) {
        options->AddRef();
    }
    handler->AddRef();
    HRESULT hr = retry->userDataFolder
                     ? create(TRUE, rt.runtimeType, retry->userDataFolder,
                              options, retry)
                     : E_OUTOFMEMORY;

    if (FAILED(hr) && retry->userDataFolder) {
        hr =
            create(TRUE, rt.runtimeType, retry->userDataFolder, options, retry);
    }
    retry->Release();

    if (GetProcAddress(client, "DllCanUnloadNow")) {
        FreeLibrary(client);
    }
    free(userDataOverride);
    return hr;
}

Str WebViewVersionTemp() {
    RuntimeInfo rt;
    if ((!FindRuntime(&rt) || rt.version[0] == 0) &&
        (!FindRuntime(&rt, nullptr, false) || rt.version[0] == 0)) {
        return {};
    }
    return WstrToUtf8Temp(rt.version);
}

bool WebViewAvailable() {
    RuntimeInfo rt;
    return FindRuntime(&rt) || FindRuntime(&rt, nullptr, false);
}

template <typename I, typename A1, typename A2>
struct Handler2 : ComObject<I> {
    void* ctx = nullptr;
    HRESULT (*fn)(void* ctx, A1 a1, A2 a2) = nullptr;
    void (*dropCtx)(void* ctx) = nullptr;

    ~Handler2() override {
        if (dropCtx) {
            dropCtx(ctx);
        }
    }

    HRESULT STDMETHODCALLTYPE Invoke(A1 a1, A2 a2) override {
        return fn(ctx, a1, a2);
    }
};

template <typename I, typename A1>
struct Handler1 : ComObject<I> {
    void* ctx = nullptr;
    HRESULT (*fn)(void* ctx, A1 a1) = nullptr;
    void (*dropCtx)(void* ctx) = nullptr;

    ~Handler1() override {
        if (dropCtx) {
            dropCtx(ctx);
        }
    }

    HRESULT STDMETHODCALLTYPE Invoke(A1 a1) override { return fn(ctx, a1); }
};

template <typename H, typename F>
static H* MkHandler(void* ctx, F fn, void (*dropCtx)(void*) = nullptr) {
    H* h = new H();
    h->ctx = ctx;
    h->fn = fn;
    h->dropCtx = dropCtx;
    return h;
}

template <typename T>
static void ReleaseWaitState(void* ctx) {
    ((T*)ctx)->Release();
}

struct DownloadCallbackState {
    LONG refs = 1;
    LONG alive = 1;
    void* ctx = nullptr;
    DownloadCompletedHandler fn = nullptr;

    void AddRef() { InterlockedIncrement(&refs); }
    void Release() {
        if (InterlockedDecrement(&refs) == 0) {
            delete this;
        }
    }
};

struct DownloadStateHandler : ComObject<ICoreWebView2StateChangedEventHandler> {
    DownloadCallbackState* callback = nullptr;
    LONG fired = 0;

    ~DownloadStateHandler() override {
        if (callback) {
            callback->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2DownloadOperation* operation,
                                     IUnknown*) override {
        if (!operation || !callback ||
            InterlockedCompareExchange(&callback->alive, 0, 0) == 0 ||
            !callback->fn) {
            return S_OK;
        }
        COREWEBVIEW2_DOWNLOAD_STATE state = 0;
        HRESULT hr = operation->get_State(&state);
        if (FAILED(hr) || state == kDownloadStateInProgress) {
            return hr;
        }
        if (InterlockedCompareExchange(&fired, 1, 0) != 0) {
            return S_OK;
        }

        LPWSTR uriRaw = nullptr;
        hr = operation->get_Uri(&uriRaw);
        if (FAILED(hr)) {
            return hr;
        }
        Str uri = TakePwstrTemp(uriRaw);
        bool success = state == kDownloadStateCompleted;
        Str path;
        const Str* pathArg = nullptr;
        if (success) {
            LPWSTR pathRaw = nullptr;
            hr = operation->get_ResultFilePath(&pathRaw);
            if (FAILED(hr)) {
                return hr;
            }
            path = TakePwstrTemp(pathRaw);
            pathArg = &path;
        }
        if (InterlockedCompareExchange(&callback->alive, 0, 0) != 0) {
            callback->fn(callback->ctx, uri, pathArg, success);
        }
        return S_OK;
    }
};

static bool GetDropPaths(IDataObject* data, Vec<Str>* paths, HDROP* hdropOut) {
    if (!data || !paths) {
        return false;
    }
    FORMATETC format = {};
    format.cfFormat = CF_HDROP;
    format.dwAspect = DVASPECT_CONTENT;
    format.lindex = -1;
    format.tymed = TYMED_HGLOBAL;
    STGMEDIUM medium = {};
    if (FAILED(data->GetData(&format, &medium))) {
        return false;
    }

    HDROP hdrop = (HDROP)medium.hGlobal;
    UINT count = DragQueryFileW(hdrop, 0xffffffffu, nullptr, 0);
    for (UINT i = 0; i < count; i++) {
        UINT charCount = DragQueryFileW(hdrop, i, nullptr, 0);
        WCHAR* path = new WCHAR[(size_t)charCount + 1];
        if (DragQueryFileW(hdrop, i, path, charCount + 1) == charCount) {
            VecAppend(*paths, StrDup(WstrToUtf8Temp(path, (int)charCount)));
        }
        delete[] path;
    }
    if (hdropOut) {
        *hdropOut = hdrop;
    }
    return true;
}

struct DragDropTarget : ComObject<IDropTarget> {
    HWND hwnd = nullptr;
    void* ctx = nullptr;
    DragDropHandler fn = nullptr;
    DWORD cursorEffect = DROPEFFECT_NONE;
    bool enterIsValid = false;

    void Emit(DragDropKind kind, const Vec<Str>* paths, POINTL screenPoint) {
        if (!fn) {
            return;
        }
        POINT point = {screenPoint.x, screenPoint.y};
        if (kind != DragDropKind::Leave) {
            ScreenToClient(hwnd, &point);
        }
        DragDropEvent event;
        event.kind = kind;
        event.paths = paths ? paths->els : nullptr;
        event.pathCount = paths ? paths->len : 0;
        event.x = point.x;
        event.y = point.y;

        fn(ctx, &event);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL point,
                                        DWORD* effect) override {

        Vec<Str> paths;
        enterIsValid = GetDropPaths(data, &paths, nullptr);
        cursorEffect = enterIsValid ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (enterIsValid) {
            Emit(DragDropKind::Enter, &paths, point);
            StrVecFree(&paths);
        }
        if (effect) {
            *effect = cursorEffect;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point,
                                       DWORD* effect) override {
        if (enterIsValid) {
            Emit(DragDropKind::Over, nullptr, point);
        }
        if (effect) {
            *effect = cursorEffect;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (enterIsValid) {
            Emit(DragDropKind::Leave, nullptr, POINTL{});
        }
        enterIsValid = false;
        cursorEffect = DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL point,
                                   DWORD*) override {
        if (enterIsValid) {
            Vec<Str> paths;
            HDROP hdrop = nullptr;
            GetDropPaths(data, &paths, &hdrop);
            Emit(DragDropKind::Drop, &paths, point);
            StrVecFree(&paths);
            if (hdrop) {
                DragFinish(hdrop);
            }
        }
        enterIsValid = false;
        cursorEffect = DROPEFFECT_NONE;
        return S_OK;
    }
};

struct DragDropController {
    Vec<DragDropTarget*> targets;

    ~DragDropController() {
        for (int i = 0; i < len(targets); i++) {
            DragDropTarget* target = targets.els[i];
            RevokeDragDrop(target->hwnd);
            target->Release();
        }
        VecReset(targets);
    }
};

struct DragDropEnumCtx {
    DragDropController* controller = nullptr;
    void* handlerCtx = nullptr;
    DragDropHandler handler = nullptr;
};

static BOOL CALLBACK InjectDragDropTarget(HWND hwnd, LPARAM param) {

    DragDropEnumCtx* ctx = reinterpret_cast<DragDropEnumCtx*>(
        param);
    DragDropTarget* target = new DragDropTarget();
    target->hwnd = hwnd;
    target->ctx = ctx->handlerCtx;
    target->fn = ctx->handler;
    HRESULT revoked = RevokeDragDrop(hwnd);
    if (revoked != DRAGDROP_E_INVALIDHWND &&
        SUCCEEDED(RegisterDragDrop(hwnd, target))) {
        VecAppend(ctx->controller->targets, target);
    } else {
        target->Release();
    }
    return TRUE;
}

static DragDropController* NewDragDropController(HWND hwnd, void* handlerCtx,
                                                 DragDropHandler handler) {
    DragDropController* controller = new DragDropController();
    DragDropEnumCtx ctx = {controller, handlerCtx, handler};
    EnumChildWindows(hwnd, InjectDragDropTarget, (LPARAM)&ctx);
    return controller;
}

template <typename T>
static void Rel(T** p) {
    if (p && *p) {
        (*p)->Release();
        *p = nullptr;
    }
}

static void PumpUntil(const bool* done) {
    MSG msg;
    while (!*done) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) {
            logf(
                "wry: WM_QUIT arrived while waiting for WebView2; the "
                "pending call is abandoned\n");
            PostQuitMessage((int)msg.wParam);
            return;
        }
        if (got == -1) {
            logf("wry: GetMessage failed while waiting for WebView2\n");
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static thread_local bool gCreatingWebView = false;

struct CreatingWebViewGuard {
    CreatingWebViewGuard() { gCreatingWebView = true; }
    ~CreatingWebViewGuard() { gCreatingWebView = false; }
};

static bool DispatchToWindow(HWND hwnd, Func0 task) {
    Func0* posted = new Func0(task);
    if (!PostMessageW(hwnd, ExecMsgId(), (WPARAM)posted, 0)) {
        logf("wry: PostMessage failed; is the message queue full?\n");
        delete posted;
        return false;
    }
    return true;
}

static LRESULT CALLBACK MainThreadDispatcherProc(HWND hwnd, UINT msg, WPARAM wp,
                                                 LPARAM lp, UINT_PTR,
                                                 DWORD_PTR) {
    if (msg == ExecMsgId()) {
        Func0* task = (Func0*)wp;
        task->Call();
        delete task;
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INTERNALPAINT);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

struct EnvironmentOptions
    : ComObject<
          ICoreWebView2EnvironmentOptions, ICoreWebView2EnvironmentOptions2,
          ICoreWebView2EnvironmentOptions3, ICoreWebView2EnvironmentOptions4,
          ICoreWebView2EnvironmentOptions5, ICoreWebView2EnvironmentOptions6,
          ICoreWebView2EnvironmentOptions7, ICoreWebView2EnvironmentOptions8> {
    WCHAR* additionalBrowserArguments = nullptr;
    WCHAR* language = nullptr;
    WCHAR* targetCompatibleBrowserVersion = WStrDup(L"137.0.3296.44");
    BOOL allowSingleSignOn = FALSE;
    BOOL exclusiveUserDataFolderAccess = FALSE;
    BOOL customCrashReportingEnabled = FALSE;
    Vec<IUnknown*> customSchemeRegistrations;
    BOOL trackingPreventionEnabled = TRUE;
    BOOL browserExtensionsEnabled = FALSE;
    int channelSearchKind = 0;
    int releaseChannels = 1 | 2 | 4 | 8;
    int scrollBarStyle = kScrollBarStyleDefault;

    ~EnvironmentOptions() override {
        free(additionalBrowserArguments);
        free(language);
        free(targetCompatibleBrowserVersion);
        for (int i = 0; i < len(customSchemeRegistrations); i++) {
            customSchemeRegistrations[i]->Release();
        }
        VecReset(customSchemeRegistrations);
    }

    HRESULT STDMETHODCALLTYPE
    get_AdditionalBrowserArguments(LPWSTR* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = CoTaskMemDupW(additionalBrowserArguments);
        return *value ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE
    put_AdditionalBrowserArguments(LPCWSTR value) override {
        free(additionalBrowserArguments);
        additionalBrowserArguments = WStrDup(value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = CoTaskMemDupW(language);
        return *value ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR value) override {
        free(language);
        language = WStrDup(value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    get_TargetCompatibleBrowserVersion(LPWSTR* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = CoTaskMemDupW(targetCompatibleBrowserVersion);
        return *value ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE
    put_TargetCompatibleBrowserVersion(LPCWSTR value) override {
        WCHAR* copy = WStrDup(value ? value : L"");
        if (!copy) {
            return E_OUTOFMEMORY;
        }
        free(targetCompatibleBrowserVersion);
        targetCompatibleBrowserVersion = copy;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* allow) override {
        if (!allow) {
            return E_POINTER;
        }
        *allow = allowSingleSignOn;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL allow) override {
        allowSingleSignOn = allow;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    get_ExclusiveUserDataFolderAccess(BOOL* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = exclusiveUserDataFolderAccess;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    put_ExclusiveUserDataFolderAccess(BOOL value) override {
        exclusiveUserDataFolderAccess = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    get_IsCustomCrashReportingEnabled(BOOL* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = customCrashReportingEnabled;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    put_IsCustomCrashReportingEnabled(BOOL value) override {
        customCrashReportingEnabled = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    GetCustomSchemeRegistrations(UINT32* count, IUnknown*** values) override {
        if (!count || !values) {
            return E_POINTER;
        }
        *count = (UINT32)len(customSchemeRegistrations);
        *values = nullptr;
        if (len(customSchemeRegistrations) == 0) {
            return S_OK;
        }
        IUnknown** copy =
            (IUnknown**)CoTaskMemAlloc(sizeof(IUnknown*) * (size_t)*count);
        if (!copy) {
            return E_OUTOFMEMORY;
        }
        for (UINT32 i = 0; i < *count; i++) {
            copy[i] = customSchemeRegistrations[(int)i];
            copy[i]->AddRef();
        }
        *values = copy;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    SetCustomSchemeRegistrations(UINT32 count, IUnknown** values) override {
        if (count > 0 && !values) {
            return E_POINTER;
        }
        Vec<IUnknown*> copy;
        for (UINT32 i = 0; i < count; i++) {
            if (!values[i] || !VecAppend(copy, values[i])) {
                for (int j = 0; j < len(copy); j++) {
                    copy[j]->Release();
                }
                VecReset(copy);
                return values[i] ? E_OUTOFMEMORY : E_POINTER;
            }
            values[i]->AddRef();
        }
        for (int i = 0; i < len(customSchemeRegistrations); i++) {
            customSchemeRegistrations[i]->Release();
        }
        VecReset(customSchemeRegistrations);
        customSchemeRegistrations = copy;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    get_EnableTrackingPrevention(BOOL* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = trackingPreventionEnabled;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    put_EnableTrackingPrevention(BOOL value) override {
        trackingPreventionEnabled = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    get_AreBrowserExtensionsEnabled(BOOL* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = browserExtensionsEnabled;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    put_AreBrowserExtensionsEnabled(BOOL value) override {
        browserExtensionsEnabled = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    get_ChannelSearchKind(COREWEBVIEW2_CHANNEL_SEARCH_KIND* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = channelSearchKind;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    put_ChannelSearchKind(COREWEBVIEW2_CHANNEL_SEARCH_KIND value) override {
        channelSearchKind = value;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    get_ReleaseChannels(COREWEBVIEW2_RELEASE_CHANNELS* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = releaseChannels;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    put_ReleaseChannels(COREWEBVIEW2_RELEASE_CHANNELS value) override {
        releaseChannels = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    get_ScrollBarStyle(COREWEBVIEW2_SCROLLBAR_STYLE* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = scrollBarStyle;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    put_ScrollBarStyle(COREWEBVIEW2_SCROLLBAR_STYLE value) override {
        scrollBarStyle = value;
        return S_OK;
    }
};

struct WebViewEventState;

struct WebView : WebViewHandlers {
    Str id = {};
    HWND parent = nullptr;
    HWND hwnd = nullptr;
    bool isChild = false;
    bool parentSubclassAttached = false;
    DWORD mainThreadId = 0;

    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* webview = nullptr;
    ICoreWebView2Environment* env = nullptr;
    WebViewEventState* eventCallbacks = nullptr;

    DownloadCallbackState* downloadCallbacks = nullptr;
    DragDropController* dragDropController = nullptr;
    bool oleInitialized = false;

    Vec<CustomProtocol> protocols;

    const char* httpOrHttps = "http";
};

struct WebViewEventState {
    LONG refs = 1;
    LONG alive = 1;
    WebView* webview = nullptr;

    void AddRef() { InterlockedIncrement(&refs); }
    void Release() {
        if (InterlockedDecrement(&refs) == 0) {
            delete this;
        }
    }
};

static void ReleaseWebViewEventState(void* ctx) {
    ((WebViewEventState*)ctx)->Release();
}

static WebView* LiveWebView(void* ctx) {
    WebViewEventState* state = (WebViewEventState*)ctx;
    if (!state || InterlockedCompareExchange(&state->alive, 0, 0) == 0) {
        return nullptr;
    }
    return state->webview;
}

template <typename H, typename F>
static H* MkWebViewHandler(WebView* webview, F fn) {
    webview->eventCallbacks->AddRef();
    return MkHandler<H>(webview->eventCallbacks, fn, ReleaseWebViewEventState);
}

struct RequestResponder {

    ICoreWebView2Environment* env = nullptr;
    HWND hwnd = nullptr;
    DWORD mainThreadId = 0;
    ICoreWebView2WebResourceRequestedEventArgs* args = nullptr;
    ICoreWebView2Deferral* deferral = nullptr;
    LONG answered = 0;
};

static HRESULT SetThemeInner(ICoreWebView2* webview, Theme theme) {
    ICoreWebView2_13* wv13 = nullptr;
    HRESULT hr =
        webview->QueryInterface(__uuidof(ICoreWebView2_13), (void**)&wv13);
    if (FAILED(hr)) {
        return hr;
    }
    ICoreWebView2Profile* profile = nullptr;
    hr = wv13->get_Profile(&profile);
    if (SUCCEEDED(hr) && !profile) {
        hr = E_POINTER;
    }
    if (SUCCEEDED(hr)) {
        int scheme = kPreferredColorSchemeAuto;
        if (theme == Theme::Dark) {
            scheme = kPreferredColorSchemeDark;
        } else if (theme == Theme::Light) {
            scheme = kPreferredColorSchemeLight;
        }
        hr = profile->put_PreferredColorScheme(scheme);
    }
    Rel(&profile);
    Rel(&wv13);
    return hr;
}

static bool SetTheme(ICoreWebView2* webview, Theme theme) {
    return SUCCEEDED(SetThemeInner(webview, theme));
}

static bool SetBackgroundColor(ICoreWebView2Controller* controller,
                               Rgba color) {
    ICoreWebView2Controller2* c2 = nullptr;
    if (FAILED(controller->QueryInterface(__uuidof(ICoreWebView2Controller2),
                                          (void**)&c2))) {
        return false;
    }
    COREWEBVIEW2_COLOR c;
    c.R = color.r;
    c.G = color.g;
    c.B = color.b;
    c.A = color.a != 0 ? 255 : 0;
    bool ok = SUCCEEDED(c2->put_DefaultBackgroundColor(c));
    Rel(&c2);
    return ok;
}

static bool SetWebViewSettings(ICoreWebView2* webview,
                               const WebViewAttributes* attrs) {
    ICoreWebView2Settings* settings = nullptr;
    if (FAILED(webview->get_Settings(&settings)) || !settings) {
        return false;
    }
    HRESULT hr = settings->put_IsStatusBarEnabled(FALSE);
    if (SUCCEEDED(hr)) {
        hr = settings->put_AreDefaultContextMenusEnabled(
            attrs->defaultContextMenus ? TRUE : FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = settings->put_IsZoomControlEnabled(
            attrs->zoomHotkeysEnabled ? TRUE : FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = settings->put_AreDevToolsEnabled(attrs->devtools ? TRUE : FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = settings->put_IsScriptEnabled(attrs->javascriptDisabled ? FALSE
                                                                     : TRUE);
    }

    if (SUCCEEDED(hr) && attrs->userAgent.s) {
        ICoreWebView2Settings2* s2 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings2),
                                               (void**)&s2))) {
            hr = s2->put_UserAgent(ToCWstrTemp(attrs->userAgent));
            Rel(&s2);
        }
    }
    if (SUCCEEDED(hr) && !attrs->browserAcceleratorKeys) {
        ICoreWebView2Settings3* s3 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings3),
                                               (void**)&s3))) {
            hr = s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
            Rel(&s3);
        }
    }
    if (SUCCEEDED(hr)) {
        ICoreWebView2Settings5* s5 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings5),
                                               (void**)&s5))) {
            hr = s5->put_IsPinchZoomEnabled(attrs->zoomHotkeysEnabled ? TRUE
                                                                      : FALSE);
            Rel(&s5);
        }
    }
    if (SUCCEEDED(hr)) {
        ICoreWebView2Settings6* s6 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings6),
                                               (void**)&s6))) {
            hr = s6->put_IsSwipeNavigationEnabled(
                attrs->backForwardNavigationGestures ? TRUE : FALSE);
            Rel(&s6);
        }
    }
    if (SUCCEEDED(hr)) {
        ICoreWebView2Settings9* s9 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings9),
                                               (void**)&s9))) {
            hr = s9->put_IsNonClientRegionSupportEnabled(TRUE);
            Rel(&s9);
        }
    }
    Rel(&settings);
    return SUCCEEDED(hr);
}

static bool ParentBounds(HWND hwnd, int* width, int* height) {
    RECT r;
    if (!GetClientRect(hwnd, &r)) {
        return false;
    }
    *width = r.right - r.left;
    *height = r.bottom - r.top;
    return true;
}

static bool SetBoundsInner(WebView* wv, int width, int height, int x, int y) {
    RECT r;
    r.left = 0;
    r.top = 0;
    r.right = width;
    r.bottom = height;
    if (FAILED(wv->controller->put_Bounds(r))) {
        return false;
    }
    return SetWindowPos(wv->hwnd, nullptr, x, y, width, height,
                        SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER) !=
           0;
}

static bool ResizeToParent(WebView* wv) {
    int w = 0;
    int h = 0;
    if (!ParentBounds(wv->parent, &w, &h)) {
        return false;
    }
    return SetBoundsInner(wv, w, h, 0, 0);
}

static LRESULT CALLBACK ParentSubclassProc(HWND hwnd, UINT msg, WPARAM wp,
                                           LPARAM lp, UINT_PTR,
                                           DWORD_PTR refData) {
    ICoreWebView2Controller* controller = (ICoreWebView2Controller*)refData;
    switch (msg) {
        case WM_SIZE: {
            if (wp != SIZE_MINIMIZED && controller) {
                int w = 0;
                int h = 0;
                if (ParentBounds(hwnd, &w, &h)) {
                    RECT r;
                    r.left = 0;
                    r.top = 0;
                    r.right = w;
                    r.bottom = h;
                    controller->put_Bounds(r);
                    HWND child = nullptr;
                    if (SUCCEEDED(controller->get_ParentWindow(&child)) &&
                        child) {
                        SetWindowPos(
                            child, nullptr, 0, 0, w, h,
                            SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER);
                    }
                }
            }
            break;
        }
        case WM_SETFOCUS:
        case WM_ENTERSIZEMOVE: {
            if (controller) {
                controller->MoveFocus(kMoveFocusReasonProgrammatic);
            }
            break;
        }
        case WM_MOVE:
        case WM_MOVING: {
            if (controller) {
                controller->NotifyParentWindowPositionChanged();
            }
            break;
        }
        default: {
            if (msg == WM_DESTROY || msg == kParentDestroyMessage) {
                if (controller) {
                    controller->Release();

                    SetWindowSubclass(hwnd, ParentSubclassProc,
                                      kParentSubclassId, 0);
                }
            }
            break;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void AttachParentSubclass(HWND parent,
                                 ICoreWebView2Controller* controller) {
    controller->AddRef();
    SetWindowSubclass(parent, ParentSubclassProc, kParentSubclassId,
                      (DWORD_PTR)controller);
}

static void DetachParentSubclass(HWND parent) {
    SendMessageW(parent, kParentDestroyMessage, 0, 0);
    RemoveWindowSubclass(parent, ParentSubclassProc, kParentSubclassId);
}

static LRESULT CALLBACK ContainerWndProc(HWND hwnd, UINT msg, WPARAM wp,
                                         LPARAM lp) {
    if (msg == WM_SETFOCUS) {

        HWND child = GetWindow(hwnd, GW_CHILD);
        if (child) {
            SetFocus(child);
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreateContainerHwnd(HWND parent, const WebViewAttributes* attrs,
                                bool isChild) {
    static const WCHAR* kClassName = L"WRY_WEBVIEW";
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        registered = true;
        WNDCLASSEXW cls;
        memset(&cls, 0, sizeof(cls));
        cls.cbSize = sizeof(cls);
        cls.style = CS_HREDRAW | CS_VREDRAW;
        cls.lpfnWndProc = ContainerWndProc;
        cls.hInstance = inst;
        cls.lpszClassName = kClassName;
        RegisterClassExW(&cls);
    }

    DWORD style = WS_CHILD | WS_CLIPCHILDREN;
    if (attrs->visible) {
        style |= WS_VISIBLE;
    }

    double scale = DpiToScaleFactor(HwndDpi(parent));
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    if (isChild) {
        if (attrs->hasBounds) {
            x = ToPhysical(attrs->bounds.position.x,
                           attrs->bounds.position.logical, scale);
            y = ToPhysical(attrs->bounds.position.y,
                           attrs->bounds.position.logical, scale);
            w = ToPhysical(attrs->bounds.size.width, attrs->bounds.size.logical,
                           scale);
            h = ToPhysical(attrs->bounds.size.height,
                           attrs->bounds.size.logical, scale);
        } else {
            x = CW_USEDEFAULT;
            y = CW_USEDEFAULT;
            w = CW_USEDEFAULT;
            h = CW_USEDEFAULT;
        }
    } else if (!ParentBounds(parent, &w, &h)) {
        return nullptr;
    }

    HWND hwnd = CreateWindowExW(0, kClassName, nullptr, style, x, y, w, h,
                                parent, nullptr, inst, nullptr);
    if (!hwnd) {
        logf("wry: CreateWindowEx for the webview container failed, error %d\n",
             (int)GetLastError());
        return nullptr;
    }
    if (!SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                      SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOMOVE |
                          SWP_NOOWNERZORDER | SWP_NOSIZE)) {
        logf("wry: positioning the webview container failed, error %d\n",
             (int)GetLastError());
        DestroyWindow(hwnd);
        return nullptr;
    }
    return hwnd;
}

struct EnvWait {
    LONG refs = 2;
    bool done = false;
    ICoreWebView2Environment* env = nullptr;

    void Release() {
        if (InterlockedDecrement(&refs) == 0) {
            Rel(&env);
            delete this;
        }
    }
};

struct ControllerWait {
    LONG refs = 2;
    bool done = false;
    ICoreWebView2Controller* controller = nullptr;

    void Release() {
        if (InterlockedDecrement(&refs) == 0) {
            Rel(&controller);
            delete this;
        }
    }
};

static ICoreWebView2Environment* CreateEnvironment(
    const WebViewAttributes* attrs) {
    EnvironmentOptions* options = new EnvironmentOptions();

    if (attrs->additionalBrowserArgs.s) {
        options->additionalBrowserArguments =
            WStrDupUtf8(attrs->additionalBrowserArgs);
    } else {
        base::StrBuilder args;
        args.Append(StrL(
            "--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection"));
        if (attrs->autoplay) {
            args.Append(StrL(" --autoplay-policy=no-user-gesture-required"));
        }
        if (attrs->proxyConfig.kind != ProxyKind::None) {
            const char* scheme = attrs->proxyConfig.kind == ProxyKind::Http
                                     ? "http://"
                                     : "socks5://";
            args.Append(base::FormatTemp(" --proxy-server=%s%s:%s", Str(scheme),
                                         attrs->proxyConfig.host,
                                         attrs->proxyConfig.port));
        }
        options->additionalBrowserArguments = WStrDupUtf8(args.TakeStr());
    }

    WCHAR* browserArgumentsOverride =
        LoaderOverrideDup(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                          L"AdditionalBrowserArguments");
    if (browserArgumentsOverride) {
        size_t ownLen = options->additionalBrowserArguments
                            ? wcslen(options->additionalBrowserArguments)
                            : 0;
        size_t overrideLen = wcslen(browserArgumentsOverride);
        size_t total = ownLen + (ownLen > 0 ? 1 : 0) + overrideLen + 1;
        WCHAR* combined = (WCHAR*)malloc(total * sizeof(WCHAR));
        if (!combined) {
            free(browserArgumentsOverride);
            options->Release();
            return nullptr;
        }
        combined[0] = 0;
        if (ownLen > 0) {
            wcscpy_s(combined, total, options->additionalBrowserArguments);
            wcscat_s(combined, total, L" ");
        }
        wcscat_s(combined, total, browserArgumentsOverride);
        free(browserArgumentsOverride);
        free(options->additionalBrowserArguments);
        options->additionalBrowserArguments = combined;
    }

    options->browserExtensionsEnabled =
        attrs->browserExtensionsEnabled ? TRUE : FALSE;
    options->scrollBarStyle =
        attrs->scrollBarStyle == ScrollBarStyle::FluentOverlay
            ? kScrollBarStyleFluentOverlay
            : kScrollBarStyleDefault;

    WCHAR lang[LOCALE_NAME_MAX_LENGTH];
    lang[0] = 0;
    LANGID lcid = GetUserDefaultUILanguage();
    if (LCIDToLocaleName(lcid, lang, LOCALE_NAME_MAX_LENGTH,
                         LOCALE_ALLOW_NEUTRAL_NAMES) > 0) {
        options->language = WStrDup(lang);
    }

    EnvWait* wait = new EnvWait();
    auto* handler = MkHandler<
        Handler2<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                 HRESULT, ICoreWebView2Environment*>>(
        wait,
        [](void* ctx, HRESULT code, ICoreWebView2Environment* env) -> HRESULT {
            EnvWait* w = (EnvWait*)ctx;
            if (SUCCEEDED(code) && env) {
                env->AddRef();
                w->env = env;
            } else {
                logf("wry: creating the WebView2 environment failed, hr 0x%x\n",
                     (int)code);
            }
            w->done = true;
            return S_OK;
        },
        ReleaseWaitState<EnvWait>);

    PCWSTR dataDirectory =
        attrs->dataDirectory.s ? ToCWstrTemp(attrs->dataDirectory) : nullptr;

    IUnknown* optionsUnknown =
        static_cast<ICoreWebView2EnvironmentOptions*>(options);
    HRESULT hr =
        CreateEnvironmentWithOptions(dataDirectory, optionsUnknown, handler);
    handler->Release();
    options->Release();
    if (FAILED(hr)) {
        logf("wry: CreateCoreWebView2EnvironmentWithOptions failed, hr 0x%x\n",
             (int)hr);
        wait->Release();
        return nullptr;
    }
    PumpUntil(&wait->done);
    ICoreWebView2Environment* result = nullptr;
    if (wait->done) {
        result = wait->env;
        wait->env = nullptr;
    }
    wait->Release();
    return result;
}

static ICoreWebView2Controller* CreateController(HWND hwnd,
                                                 ICoreWebView2Environment* env,
                                                 bool incognito,
                                                 const Rgba* backgroundColor) {
    ControllerWait* wait = new ControllerWait();
    auto* handler = MkHandler<
        Handler2<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                 HRESULT, ICoreWebView2Controller*>>(
        wait,
        [](void* ctx, HRESULT code,
           ICoreWebView2Controller* controller) -> HRESULT {
            ControllerWait* w = (ControllerWait*)ctx;
            if (SUCCEEDED(code) && controller) {
                controller->AddRef();
                w->controller = controller;
            } else {
                logf("wry: creating the WebView2 controller failed, hr 0x%x\n",
                     (int)code);
            }
            w->done = true;
            return S_OK;
        },
        ReleaseWaitState<ControllerWait>);

    HRESULT hr = E_FAIL;
    ICoreWebView2Environment10* env10 = nullptr;
    if (SUCCEEDED(env->QueryInterface(__uuidof(ICoreWebView2Environment10),
                                      (void**)&env10))) {
        ICoreWebView2ControllerOptions* opts = nullptr;
        hr = env10->CreateCoreWebView2ControllerOptions(&opts);
        if (SUCCEEDED(hr) && !opts) {
            hr = E_POINTER;
        }
        if (SUCCEEDED(hr) && backgroundColor) {
            ICoreWebView2ControllerOptions3* opts3 = nullptr;
            if (SUCCEEDED(opts->QueryInterface(
                    __uuidof(ICoreWebView2ControllerOptions3),
                    (void**)&opts3))) {
                COREWEBVIEW2_COLOR color;
                color.R = backgroundColor->r;
                color.G = backgroundColor->g;
                color.B = backgroundColor->b;
                color.A = backgroundColor->a != 0 ? 255 : 0;
                hr = opts3->put_DefaultBackgroundColor(color);
                Rel(&opts3);
            }
        }
        if (SUCCEEDED(hr)) {
            hr = opts->put_IsInPrivateModeEnabled(incognito ? TRUE : FALSE);
        }
        if (SUCCEEDED(hr)) {
            hr = env10->CreateCoreWebView2ControllerWithOptions(hwnd, opts,
                                                                handler);
        }
        Rel(&opts);
        Rel(&env10);
    } else {

        hr = env->CreateCoreWebView2Controller(hwnd, handler);
    }
    handler->Release();
    if (FAILED(hr)) {
        logf("wry: CreateCoreWebView2Controller failed, hr 0x%x\n", (int)hr);
        wait->Release();
        return nullptr;
    }
    PumpUntil(&wait->done);
    ICoreWebView2Controller* result = nullptr;
    if (wait->done) {
        result = wait->controller;
        wait->controller = nullptr;
    }
    wait->Release();

    if (result && backgroundColor) {
        SetBackgroundColor(result, *backgroundColor);
    }
    return result;
}

struct ScriptWait {
    LONG refs = 2;
    bool done = false;
    HRESULT result = E_FAIL;

    void Release() {
        if (InterlockedDecrement(&refs) == 0) {
            delete this;
        }
    }
};

static bool AddScriptToExecuteOnDocumentCreated(ICoreWebView2* webview,
                                                Str js) {
    ScriptWait* wait = new ScriptWait();
    auto* handler = MkHandler<Handler2<
        ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
        HRESULT, LPCWSTR>>(
        wait,
        [](void* ctx, HRESULT code, LPCWSTR) -> HRESULT {
            ScriptWait* wait = (ScriptWait*)ctx;
            wait->result = code;
            wait->done = true;
            return S_OK;
        },
        ReleaseWaitState<ScriptWait>);
    HRESULT hr =
        webview->AddScriptToExecuteOnDocumentCreated(ToCWstrTemp(js), handler);
    handler->Release();
    if (FAILED(hr)) {
        logf("wry: AddScriptToExecuteOnDocumentCreated failed, hr 0x%x\n",
             (int)hr);
        wait->Release();
        return false;
    }
    PumpUntil(&wait->done);
    bool ok = wait->done && SUCCEEDED(wait->result);
    HRESULT result = wait->result;
    wait->Release();
    if (!ok) {
        logf("wry: registering a document-created script failed, hr 0x%x\n",
             (int)result);
        return false;
    }
    return true;
}

struct EvalCallback {
    void* ctx;
    void (*fn)(void* ctx, Str result);
};

static void DeleteEvalCallback(void* ctx) {
    delete (EvalCallback*)ctx;
}

static bool ExecuteScript(ICoreWebView2* webview, Str js, EvalCallback cb) {
    EvalCallback* held = new EvalCallback(cb);
    auto* handler = MkHandler<
        Handler2<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>>(
        held,
        [](void* ctx, HRESULT code, LPCWSTR result) -> HRESULT {
            EvalCallback* c = (EvalCallback*)ctx;
            if (c->fn) {
                c->fn(c->ctx, SUCCEEDED(code) ? WstrToUtf8Temp(result) : Str());
            }
            return S_OK;
        },
        DeleteEvalCallback);
    HRESULT hr = webview->ExecuteScript(ToCWstrTemp(js), handler);
    handler->Release();
    if (FAILED(hr)) {
        logf("wry: ExecuteScript failed, hr 0x%x\n", (int)hr);
        return false;
    }
    return true;
}

static LPCWSTR HttpStatusReason(int status) {
    switch (status) {
        case 100:
            return L"Continue";
        case 101:
            return L"Switching Protocols";
        case 102:
            return L"Processing";
        case 103:
            return L"Early Hints";
        case 200:
            return L"OK";
        case 201:
            return L"Created";
        case 202:
            return L"Accepted";
        case 203:
            return L"Non Authoritative Information";
        case 204:
            return L"No Content";
        case 205:
            return L"Reset Content";
        case 206:
            return L"Partial Content";
        case 207:
            return L"Multi-Status";
        case 208:
            return L"Already Reported";
        case 226:
            return L"IM Used";
        case 300:
            return L"Multiple Choices";
        case 301:
            return L"Moved Permanently";
        case 302:
            return L"Found";
        case 303:
            return L"See Other";
        case 304:
            return L"Not Modified";
        case 305:
            return L"Use Proxy";
        case 307:
            return L"Temporary Redirect";
        case 308:
            return L"Permanent Redirect";
        case 400:
            return L"Bad Request";
        case 401:
            return L"Unauthorized";
        case 402:
            return L"Payment Required";
        case 403:
            return L"Forbidden";
        case 404:
            return L"Not Found";
        case 405:
            return L"Method Not Allowed";
        case 406:
            return L"Not Acceptable";
        case 407:
            return L"Proxy Authentication Required";
        case 408:
            return L"Request Timeout";
        case 409:
            return L"Conflict";
        case 410:
            return L"Gone";
        case 411:
            return L"Length Required";
        case 412:
            return L"Precondition Failed";
        case 413:
            return L"Payload Too Large";
        case 414:
            return L"URI Too Long";
        case 415:
            return L"Unsupported Media Type";
        case 416:
            return L"Range Not Satisfiable";
        case 417:
            return L"Expectation Failed";
        case 418:
            return L"I'm a teapot";
        case 421:
            return L"Misdirected Request";
        case 422:
            return L"Unprocessable Entity";
        case 423:
            return L"Locked";
        case 424:
            return L"Failed Dependency";
        case 425:
            return L"Too Early";
        case 426:
            return L"Upgrade Required";
        case 428:
            return L"Precondition Required";
        case 429:
            return L"Too Many Requests";
        case 431:
            return L"Request Header Fields Too Large";
        case 451:
            return L"Unavailable For Legal Reasons";
        case 500:
            return L"Internal Server Error";
        case 501:
            return L"Not Implemented";
        case 502:
            return L"Bad Gateway";
        case 503:
            return L"Service Unavailable";
        case 504:
            return L"Gateway Timeout";
        case 505:
            return L"HTTP Version Not Supported";
        case 506:
            return L"Variant Also Negotiates";
        case 507:
            return L"Insufficient Storage";
        case 508:
            return L"Loop Detected";
        case 510:
            return L"Not Extended";
        case 511:
            return L"Network Authentication Required";
        default:
            return L"OK";
    }
}

static ICoreWebView2WebResourceResponse* MakeResponse(
    ICoreWebView2Environment* env, int status, Str headerBlock,
    const uint8_t* body, int bodyLen) {
    IStream* stream = nullptr;
    if (body && bodyLen > 0) {
        stream = SHCreateMemStream(body, (UINT)bodyLen);
    }
    ICoreWebView2WebResourceResponse* res = nullptr;
    HRESULT hr =
        env->CreateWebResourceResponse(stream, status, HttpStatusReason(status),
                                       ToCWstrTemp(headerBlock), &res);
    if (stream) {
        stream->Release();
    }
    if (FAILED(hr)) {
        logf("wry: CreateWebResourceResponse failed, hr 0x%x\n", (int)hr);
        return nullptr;
    }
    return res;
}

static ICoreWebView2WebResourceResponse* MakeBadRequest(
    ICoreWebView2Environment* env, HRESULT cause) {
    Str header =
        base::FormatTemp("X-Wry-Error: HRESULT 0x%08x\n", (uint32_t)cause);
    return MakeResponse(env, 400, header, nullptr, 0);
}

struct PendingResponse {
    RequestResponder* responder;
    int status;
    uint8_t* body;
    int bodyLen;

    Str headers;
};

static void DiscardResponse(PendingResponse* p) {
    RequestResponder* r = p->responder;
    if (r->deferral) {
        r->deferral->Release();
    }
    r->args->Release();
    r->env->Release();
    StrFree(p->headers);
    free(p->body);
    delete p;
    delete r;
}

static void ApplyResponse(PendingResponse* p) {
    RequestResponder* r = p->responder;
    ICoreWebView2WebResourceResponse* response =
        MakeResponse(r->env, p->status, p->headers, p->body, p->bodyLen);
    if (!response) {
        response = MakeBadRequest(r->env, E_INVALIDARG);
    }
    if (response) {
        r->args->put_Response(response);
        response->Release();
    }
    if (r->deferral) {
        r->deferral->Complete();
    }
    DiscardResponse(p);
}

void Respond(RequestResponder* responder, const Response* response) {
    if (!responder) {
        return;
    }
    if (InterlockedExchange(&responder->answered, 1) != 0) {
        logf("wry: a custom protocol request was answered twice\n");
        return;
    }
    PendingResponse* p = new PendingResponse();
    p->responder = responder;
    p->status = response ? response->status : 500;
    p->body = nullptr;
    p->bodyLen = 0;
    if (response && response->body && response->bodyLen > 0) {
        p->body = (uint8_t*)malloc((size_t)response->bodyLen);
        if (p->body) {
            memcpy(p->body, response->body, (size_t)response->bodyLen);
            p->bodyLen = response->bodyLen;
        }
    }
    base::StrBuilder headers;
    if (response) {
        for (int i = 0; i < response->headerCount; i++) {
            headers
                .Append(base::FormatTemp("%s: %s\n", response->headers[i].name,
                                         response->headers[i].value));
        }
    }
    p->headers = StrDup(headers.TakeStr());

    if (GetCurrentThreadId() == responder->mainThreadId) {
        ApplyResponse(p);
        return;
    }
    if (!DispatchToWindow(responder->hwnd, MkFunc0(ApplyResponse, p))) {
        DiscardResponse(p);
    }
}

static HRESULT PrepareRequest(WebView* wv, ICoreWebView2WebResourceRequest* req,
                              Str uri, Str protocol, Request* out,
                              Vec<Header>* headerStore,
                              Vec<uint8_t>* bodyStore) {
    LPWSTR method = nullptr;
    HRESULT hr = req->get_Method(&method);
    if (FAILED(hr)) {
        return hr;
    }
    out->method = TakePwstrTemp(method);

    ICoreWebView2HttpRequestHeaders* headers = nullptr;
    hr = req->get_Headers(&headers);
    if (FAILED(hr) || !headers) {
        Rel(&headers);
        return FAILED(hr) ? hr : E_POINTER;
    }
    ICoreWebView2HttpHeadersCollectionIterator* it = nullptr;
    hr = headers->GetIterator(&it);
    Rel(&headers);
    if (FAILED(hr) || !it) {
        Rel(&it);
        return FAILED(hr) ? hr : E_POINTER;
    }
    BOOL hasCurrent = FALSE;
    hr = it->get_HasCurrentHeader(&hasCurrent);
    while (SUCCEEDED(hr) && hasCurrent) {
        LPWSTR name = nullptr;
        LPWSTR value = nullptr;
        hr = it->GetCurrentHeader(&name, &value);
        if (SUCCEEDED(hr)) {
            Header h;
            h.name = TakePwstrTemp(name);
            h.value = TakePwstrTemp(value);
            if (!VecAppend(*headerStore, h)) {
                hr = E_OUTOFMEMORY;
            }
        } else {
            if (name) {
                CoTaskMemFree(name);
            }
            if (value) {
                CoTaskMemFree(value);
            }
        }
        if (SUCCEEDED(hr)) {
            hr = it->MoveNext(&hasCurrent);
        }
    }
    Rel(&it);
    if (FAILED(hr)) {
        return hr;
    }
    out->headers = headerStore->len > 0 ? &(*headerStore)[0] : nullptr;
    out->headerCount = headerStore->len;

    IStream* content = nullptr;
    if (SUCCEEDED(req->get_Content(&content)) && content) {
        uint8_t buf[1024];
        for (;;) {
            ULONG read = 0;
            hr = content->Read(buf, (ULONG)sizeof(buf), &read);
            if (FAILED(hr) || read == 0) {
                break;
            }
            uint8_t* dst = VecAppendBlanks(*bodyStore, (int)read);
            if (!dst) {
                hr = E_OUTOFMEMORY;
                break;
            }
            memcpy(dst, buf, read);
        }
        content->Release();
        if (FAILED(hr)) {
            return hr;
        }
    }
    out->body = bodyStore->len > 0 ? &(*bodyStore)[0] : nullptr;
    out->bodyLen = bodyStore->len;
    out->uri = RevertUriWorkAround(uri, Str(wv->httpOrHttps), protocol);
    return S_OK;
}

static HRESULT OnWebResourceRequested(
    void* ctx, ICoreWebView2*,
    ICoreWebView2WebResourceRequestedEventArgs* args) {
    WebView* wv = LiveWebView(ctx);
    if (!wv || !args) {
        return S_OK;
    }
    ICoreWebView2WebResourceRequest* req = nullptr;
    HRESULT hr = args->get_Request(&req);
    if (FAILED(hr) || !req) {
        Rel(&req);
        return FAILED(hr) ? hr : E_POINTER;
    }
    LPWSTR rawUri = nullptr;
    hr = req->get_Uri(&rawUri);
    if (FAILED(hr)) {
        Rel(&req);
        return hr;
    }
    Str uri = TakePwstrTemp(rawUri);

    CustomProtocol* found = nullptr;
    for (int i = 0; i < wv->protocols.len; i++) {
        if (IsWorkAroundUri(uri, Str(wv->httpOrHttps), wv->protocols[i].name)) {
            found = &wv->protocols[i];
            break;
        }
    }
    if (!found) {
        Rel(&req);
        return S_OK;
    }

    Vec<Header> headerStore;
    Vec<uint8_t> bodyStore;
    Request request;
    hr = PrepareRequest(wv, req, uri, found->name, &request, &headerStore,
                        &bodyStore);
    Rel(&req);
    if (FAILED(hr)) {
        ICoreWebView2WebResourceResponse* response =
            MakeBadRequest(wv->env, hr);
        HRESULT responseHr = response ? args->put_Response(response) : E_FAIL;
        Rel(&response);
        VecReset(headerStore);
        VecReset(bodyStore);
        return responseHr;
    }

    RequestResponder* responder = new RequestResponder();
    responder->env = wv->env;
    responder->env->AddRef();
    responder->hwnd = wv->hwnd;
    responder->mainThreadId = wv->mainThreadId;
    responder->args = args;
    args->AddRef();
    ICoreWebView2Deferral* deferral = nullptr;
    if (SUCCEEDED(args->GetDeferral(&deferral))) {
        responder->deferral = deferral;
    }
    CustomProtocolDispatch(*found, wv->id, &request, responder);

    VecReset(headerStore);
    VecReset(bodyStore);
    return S_OK;
}

static bool AttachCustomProtocolHandler(WebView* wv,
                                        EventRegistrationToken* token) {
    for (int i = 0; i < wv->protocols.len; i++) {
        Str filter = base::FormatTemp(
            "%s*",
            WorkAroundUriPrefix(Str(wv->httpOrHttps), wv->protocols[i].name));
        ICoreWebView2_22* wv22 = nullptr;
        if (SUCCEEDED(wv->webview->QueryInterface(__uuidof(ICoreWebView2_22),
                                                  (void**)&wv22))) {

            HRESULT hr =
                wv22->AddWebResourceRequestedFilterWithRequestSourceKinds(
                    ToCWstrTemp(filter), kWebResourceContextAll,
                    (COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS)
                        kWebResourceRequestSourceKindsAll);
            Rel(&wv22);
            if (FAILED(hr)) {
                return false;
            }
        } else {
            if (FAILED(wv->webview->AddWebResourceRequestedFilter(
                    ToCWstrTemp(filter), kWebResourceContextAll))) {
                return false;
            }
        }
    }

    auto* handler = MkWebViewHandler<
        Handler2<ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2*,
                 ICoreWebView2WebResourceRequestedEventArgs*>>(
        wv, OnWebResourceRequested);
    HRESULT hr = wv->webview->add_WebResourceRequested(handler, token);
    handler->Release();
    if (FAILED(hr)) {
        return false;
    }

    SetWindowSubclass(wv->hwnd, MainThreadDispatcherProc,
                      kMainThreadDispatcherSubclassId, 0);
    return true;
}

static HRESULT UrlFromWebViewInner(ICoreWebView2* webview, Str* out) {
    if (!webview || !out) {
        return E_POINTER;
    }
    LPWSTR uri = nullptr;
    HRESULT hr = webview->get_Source(&uri);
    if (FAILED(hr)) {
        return hr;
    }
    *out = TakePwstrTemp(uri);
    return S_OK;
}

static Str UrlFromWebView(ICoreWebView2* webview) {
    Str result;
    UrlFromWebViewInner(webview, &result);
    return result;
}

static HRESULT OnWindowCloseRequested(void* ctx, ICoreWebView2*, IUnknown*) {
    WebView* wv = LiveWebView(ctx);
    if (!wv) {
        return S_OK;
    }
    if (DestroyWindow(wv->hwnd)) {
        return S_OK;
    }
    return HRESULT_FROM_WIN32(GetLastError());
}

static HRESULT OnDocumentTitleChanged(void* ctx, ICoreWebView2* sender,
                                      IUnknown*) {
    WebView* wv = LiveWebView(ctx);
    if (!wv || !sender || !wv->documentTitleChangedHandler) {
        return S_OK;
    }
    LPWSTR title = nullptr;
    HRESULT hr = sender->get_DocumentTitle(&title);
    if (FAILED(hr)) {
        return hr;
    }
    wv->documentTitleChangedHandler(wv->ctx, TakePwstrTemp(title));
    return S_OK;
}

static HRESULT OnProcessFailed(void* ctx, ICoreWebView2*,
                               ICoreWebView2ProcessFailedEventArgs* args) {
    WebView* wv = LiveWebView(ctx);
    if (!wv || !wv->processFailedHandler) {
        return S_OK;
    }
    int kind = 0;
    if (!args || FAILED(args->get_ProcessFailedKind(&kind))) {
        kind = 0;
    }
    wv->processFailedHandler(wv->ctx, kind);
    return S_OK;
}

static HRESULT ReportPageLoad(void* ctx, ICoreWebView2* sender,
                              PageLoadEvent event) {
    WebView* wv = LiveWebView(ctx);
    if (!wv) {
        return S_OK;
    }
    if (sender && wv->onPageLoadHandler) {
        Str url;
        HRESULT hr = UrlFromWebViewInner(sender, &url);
        if (FAILED(hr)) {
            return hr;
        }
        wv->onPageLoadHandler(wv->ctx, event, url);
    }
    return S_OK;
}

static HRESULT OnContentLoading(void* ctx, ICoreWebView2* sender,
                                ICoreWebView2ContentLoadingEventArgs*) {
    return ReportPageLoad(ctx, sender, PageLoadEvent::Started);
}

static HRESULT OnNavigationCompleted(
    void* ctx, ICoreWebView2* sender,
    ICoreWebView2NavigationCompletedEventArgs*) {
    return ReportPageLoad(ctx, sender, PageLoadEvent::Finished);
}

static HRESULT OnNavigationStarting(
    void* ctx, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
    WebView* wv = LiveWebView(ctx);
    if (!wv || !args || !wv->navigationHandler) {
        return S_OK;
    }
    LPWSTR uri = nullptr;
    HRESULT hr = args->get_Uri(&uri);
    if (FAILED(hr)) {
        return hr;
    }
    bool allow = wv->navigationHandler(wv->ctx, TakePwstrTemp(uri));
    return args->put_Cancel(allow ? FALSE : TRUE);
}

static HRESULT OnNewWindowRequested(
    void* ctx, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
    WebView* wv = LiveWebView(ctx);
    if (!wv || !args) {
        return S_OK;
    }
    if (!wv->newWindowReqHandler) {
        return args->put_Handled(TRUE);
    }
    LPWSTR uri = nullptr;
    HRESULT hr = args->get_Uri(&uri);
    if (FAILED(hr)) {
        return hr;
    }
    Str url = TakePwstrTemp(uri);

    NewWindowFeatures features;
    features.opener = wv;
    ICoreWebView2WindowFeatures* f = nullptr;
    if (SUCCEEDED(args->get_WindowFeatures(&f)) && f) {
        BOOL has = FALSE;
        f->get_HasPosition(&has);
        if (has) {
            UINT32 left = 0;
            UINT32 top = 0;
            f->get_Left(&left);
            f->get_Top(&top);
            features.hasPosition = true;
            features.x = (double)left;
            features.y = (double)top;
        }
        has = FALSE;
        f->get_HasSize(&has);
        if (has) {
            UINT32 width = 0;
            UINT32 height = 0;
            f->get_Width(&width);
            f->get_Height(&height);
            features.hasSize = true;
            features.width = (double)width;
            features.height = (double)height;
        }
        Rel(&f);
    }

    WebView* created = nullptr;
    NewWindowResponse response =
        wv->newWindowReqHandler(wv->ctx, url, &features, &created);
    if (response == NewWindowResponse::Allow) {
        return args->put_Handled(FALSE);
    }
    if (response == NewWindowResponse::Create && created && created->webview) {
        hr = args->put_NewWindow(created->webview);
        if (FAILED(hr)) {
            return hr;
        }
    } else if (response == NewWindowResponse::Create) {
        logf("wry: NewWindowResponse::Create requires a target WebView\n");
    }
    return args->put_Handled(TRUE);
}

static HRESULT OnPermissionRequested(
    void*, ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) {
    if (!args) {
        return S_OK;
    }
    COREWEBVIEW2_PERMISSION_KIND kind = 0;
    HRESULT hr = args->get_PermissionKind(&kind);
    if (FAILED(hr)) {
        return hr;
    }
    if (kind == kPermissionKindClipboardRead) {
        return args->put_State(kPermissionStateAllow);
    }
    return S_OK;
}

static HRESULT OnDownloadStarting(
    void* ctx, ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) {
    WebView* wv = LiveWebView(ctx);
    if (!wv || !args) {
        return S_OK;
    }

    ICoreWebView2DownloadOperation* operation = nullptr;
    HRESULT hr = args->get_DownloadOperation(&operation);
    if (FAILED(hr) || !operation) {
        return hr;
    }

    LPWSTR uriRaw = nullptr;
    hr = operation->get_Uri(&uriRaw);
    if (FAILED(hr)) {
        Rel(&operation);
        return hr;
    }
    Str uri = TakePwstrTemp(uriRaw);

    if (wv->downloadCompletedHandler) {
        DownloadStateHandler* handler = new DownloadStateHandler();
        handler->callback = wv->downloadCallbacks;
        handler->callback->AddRef();
        EventRegistrationToken token = {};
        hr = operation->add_StateChanged(handler, &token);
        handler->Release();
        if (FAILED(hr)) {
            Rel(&operation);
            return hr;
        }
    }

    if (wv->downloadStartedHandler) {
        LPWSTR pathRaw = nullptr;
        hr = args->get_ResultFilePath(&pathRaw);
        if (FAILED(hr)) {
            Rel(&operation);
            return hr;
        }
        Str path = TakePwstrTemp(pathRaw);
        if (wv->downloadStartedHandler(wv->ctx, uri, &path)) {
            hr = args->put_ResultFilePath(ToCWstrTemp(path));
            if (SUCCEEDED(hr)) {
                hr = args->put_Handled(TRUE);
            }
        } else {
            hr = args->put_Cancel(TRUE);
        }
    }

    Rel(&operation);
    return hr;
}

static HRESULT OnWebMessageReceived(
    void* ctx, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
    WebView* wv = LiveWebView(ctx);
    if (!wv || !args || !wv->ipcHandler) {
        return S_OK;
    }
    LPWSTR source = nullptr;
    HRESULT hr = args->get_Source(&source);
    if (FAILED(hr)) {
        return hr;
    }
    Str url = TakePwstrTemp(source);
    LPWSTR message = nullptr;
    hr = args->TryGetWebMessageAsString(&message);
    if (FAILED(hr)) {
        return hr;
    }
    wv->ipcHandler(wv->ctx, url, TakePwstrTemp(message));
    return S_OK;
}

static bool AttachHandlers(WebView* wv, EventRegistrationToken* token) {
    {
        auto* h = MkWebViewHandler<
            Handler2<ICoreWebView2WindowCloseRequestedEventHandler,
                     ICoreWebView2*, IUnknown*>>(wv, OnWindowCloseRequested);
        HRESULT hr = wv->webview->add_WindowCloseRequested(h, token);
        h->Release();
        if (FAILED(hr)) {
            return false;
        }
    }
    if (wv->documentTitleChangedHandler) {
        auto* h = MkWebViewHandler<
            Handler2<ICoreWebView2DocumentTitleChangedEventHandler,
                     ICoreWebView2*, IUnknown*>>(wv, OnDocumentTitleChanged);
        HRESULT hr = wv->webview->add_DocumentTitleChanged(h, token);
        h->Release();
        if (FAILED(hr)) {
            return false;
        }
    }
    if (wv->processFailedHandler) {
        auto* h = MkWebViewHandler<
            Handler2<ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*,
                     ICoreWebView2ProcessFailedEventArgs*>>(wv,
                                                            OnProcessFailed);
        HRESULT hr = wv->webview->add_ProcessFailed(h, token);
        h->Release();
        if (FAILED(hr)) {
            return false;
        }
    }
    if (wv->onPageLoadHandler) {
        auto* started = MkWebViewHandler<
            Handler2<ICoreWebView2ContentLoadingEventHandler, ICoreWebView2*,
                     ICoreWebView2ContentLoadingEventArgs*>>(wv,
                                                             OnContentLoading);
        HRESULT hr = wv->webview->add_ContentLoading(started, token);
        started->Release();
        if (FAILED(hr)) {
            return false;
        }
        auto* finished = MkWebViewHandler<Handler2<
            ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*,
            ICoreWebView2NavigationCompletedEventArgs*>>(wv,
                                                         OnNavigationCompleted);
        hr = wv->webview->add_NavigationCompleted(finished, token);
        finished->Release();
        if (FAILED(hr)) {
            return false;
        }
    }
    if (wv->navigationHandler) {
        auto* h = MkWebViewHandler<Handler2<
            ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*,
            ICoreWebView2NavigationStartingEventArgs*>>(wv,
                                                        OnNavigationStarting);
        HRESULT hr = wv->webview->add_NavigationStarting(h, token);
        h->Release();
        if (FAILED(hr)) {
            return false;
        }
    }
    {
        auto* h = MkWebViewHandler<Handler2<
            ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*,
            ICoreWebView2NewWindowRequestedEventArgs*>>(wv,
                                                        OnNewWindowRequested);
        HRESULT hr = wv->webview->add_NewWindowRequested(h, token);
        h->Release();
        if (FAILED(hr)) {
            return false;
        }
    }
    return true;
}

static bool AttachDownloadHandlers(WebView* wv, EventRegistrationToken* token) {
    if (!wv->downloadStartedHandler && !wv->downloadCompletedHandler) {
        return true;
    }
    ICoreWebView2_4* webview4 = nullptr;
    if (FAILED(wv->webview->QueryInterface(__uuidof(ICoreWebView2_4),
                                           (void**)&webview4))) {
        logf("wry: this WebView2 runtime does not support download handlers\n");
        return false;
    }
    auto* handler = MkWebViewHandler<
        Handler2<ICoreWebView2DownloadStartingEventHandler, ICoreWebView2*,
                 ICoreWebView2DownloadStartingEventArgs*>>(wv,
                                                           OnDownloadStarting);
    HRESULT hr = webview4->add_DownloadStarting(handler, token);
    handler->Release();
    Rel(&webview4);
    if (FAILED(hr)) {
        logf("wry: adding the download handler failed, hr 0x%x\n", (int)hr);
        return false;
    }
    return true;
}

static bool AttachIpcHandler(WebView* wv, EventRegistrationToken* token) {
    if (!AddScriptToExecuteOnDocumentCreated(
            wv->webview, StrL("Object.defineProperty(window, 'ipc', { value: "
                              "Object.freeze({ postMessage: s=> "
                              "window.chrome.webview.postMessage(s) }) });"))) {
        return false;
    }
    auto* h = MkWebViewHandler<
        Handler2<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*,
                 ICoreWebView2WebMessageReceivedEventArgs*>>(
        wv, OnWebMessageReceived);
    HRESULT hr = wv->webview->add_WebMessageReceived(h, token);
    h->Release();
    return SUCCEEDED(hr);
}

static bool LoadUrlWithHeaders(WebView* wv, Str url, const Header* headers,
                               int headerCount) {
    base::StrBuilder block;
    for (int i = 0; i < headerCount; i++) {
        block.Append(
            base::FormatTemp("%s: %s\n", headers[i].name, headers[i].value));
    }
    ICoreWebView2Environment9* env9 = nullptr;
    if (FAILED(wv->env->QueryInterface(__uuidof(ICoreWebView2Environment9),
                                       (void**)&env9))) {
        return false;
    }
    ICoreWebView2WebResourceRequest* request = nullptr;
    HRESULT hr =
        env9->CreateWebResourceRequest(ToCWstrTemp(url), L"GET", nullptr,
                                       ToCWstrTemp(block.TakeStr()), &request);
    Rel(&env9);

    if (FAILED(hr) || !request) {
        Rel(&request);
        return true;
    }

    ICoreWebView2_10* wv10 = nullptr;
    bool ok = SUCCEEDED(wv->webview->QueryInterface(__uuidof(ICoreWebView2_10),
                                                    (void**)&wv10)) &&
              wv10 && SUCCEEDED(wv10->NavigateWithWebResourceRequest(request));
    Rel(&wv10);
    Rel(&request);
    return ok;
}

static bool LoadExtensions(ICoreWebView2* webview, Str extensionRoot) {

    if (len(extensionRoot) == 0) {
        logf("wry: cannot enumerate an empty browser extension path\n");
        return false;
    }
    ICoreWebView2_13* webview13 = nullptr;
    if (FAILED(webview->QueryInterface(__uuidof(ICoreWebView2_13),
                                       (void**)&webview13))) {
        logf("wry: this WebView2 runtime cannot load browser extensions\n");
        return false;
    }

    ICoreWebView2Profile* profile = nullptr;
    ICoreWebView2Profile7* profile7 = nullptr;
    bool ok = SUCCEEDED(webview13->get_Profile(&profile)) && profile &&
              SUCCEEDED(profile->QueryInterface(__uuidof(ICoreWebView2Profile7),
                                                (void**)&profile7)) &&
              profile7;
    Rel(&profile);
    Rel(&webview13);
    if (!ok) {
        logf("wry: this WebView2 profile cannot load browser extensions\n");
        Rel(&profile7);
        return false;
    }

    WCHAR* root = WStrDupUtf8(extensionRoot);
    size_t rootLen = wcslen(root);
    bool hasSeparator = rootLen > 0 && (root[rootLen - 1] == L'\\' ||
                                        root[rootLen - 1] == L'/');
    size_t prefixLen = rootLen + (hasSeparator ? 0 : 1);
    size_t pathCap = prefixLen + MAX_PATH + 1;
    WCHAR* path = new WCHAR[pathCap];
    wcscpy_s(path, pathCap, root);
    if (!hasSeparator) {
        wcscat_s(path, pathCap, L"\\");
    }
    wcscat_s(path, pathCap, L"*");

    WIN32_FIND_DATAW found = {};
    HANDLE iter = FindFirstFileW(path, &found);
    if (iter == INVALID_HANDLE_VALUE) {
        logf(
            "wry: cannot enumerate the browser extension directory, error %d\n",
            (int)GetLastError());
        ok = false;
    } else {
        do {
            if (found.cFileName[0] == L'.' &&
                (found.cFileName[1] == 0 ||
                 (found.cFileName[1] == L'.' && found.cFileName[2] == 0))) {
                continue;
            }
            wcscpy_s(path + prefixLen, pathCap - prefixLen, found.cFileName);
            auto* handler = MkHandler<Handler2<
                ICoreWebView2ProfileAddBrowserExtensionCompletedHandler,
                HRESULT, ICoreWebView2BrowserExtension*>>(
                nullptr,
                [](void*, HRESULT code,
                   ICoreWebView2BrowserExtension*) -> HRESULT {
                    if (FAILED(code)) {
                        logf(
                            "wry: loading a browser extension failed, hr "
                            "0x%x\n",
                            (int)code);
                    }
                    return S_OK;
                });
            HRESULT hr = profile7->AddBrowserExtension(path, handler);
            handler->Release();
            if (FAILED(hr)) {
                logf("wry: AddBrowserExtension failed, hr 0x%x\n", (int)hr);
                ok = false;
                break;
            }
        } while (FindNextFileW(iter, &found));
        if (ok && GetLastError() != ERROR_NO_MORE_FILES) {
            logf("wry: enumerating browser extensions failed, error %d\n",
                 (int)GetLastError());
            ok = false;
        }
        FindClose(iter);
    }

    delete[] path;
    delete[] root;
    Rel(&profile7);
    return ok;
}

WebView* WebViewNew(void* parentWindow, const WebViewAttributes* attrs,
                    bool asChild) {
    if (!parentWindow || !attrs) {
        return nullptr;
    }
    if (attrs->headerCount < 0 || (attrs->headerCount > 0 && !attrs->headers) ||
        attrs->initializationScriptCount < 0 ||
        (attrs->initializationScriptCount > 0 &&
         !attrs->initializationScripts) ||
        attrs->customProtocolCount < 0 ||
        (attrs->customProtocolCount > 0 && !attrs->customProtocols)) {
        logf("wry: invalid Windows webview attribute array\n");
        return nullptr;
    }
    for (int i = 0; i < attrs->customProtocolCount; i++) {
        for (int j = 0; j < i; j++) {
            if (base::StrEq(attrs->customProtocols[i].name,
                            attrs->customProtocols[j].name)) {
                logf("wry: duplicate custom protocol '%s'\n",
                     attrs->customProtocols[i].name);
                return nullptr;
            }
        }
    }
    if (gCreatingWebView) {
        logf(
            "wry: WebViewNew called while another webview is still being "
            "created on this thread\n");
        return nullptr;
    }
    CreatingWebViewGuard creating;
    HWND parent = (HWND)parentWindow;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HWND hwnd = CreateContainerHwnd(parent, attrs, asChild);
    if (!hwnd) {
        return nullptr;
    }

    ICoreWebView2Environment* env = (ICoreWebView2Environment*)attrs
                                        ->webviewEnvironment;
    if (env) {
        env->AddRef();
    } else {
        env = CreateEnvironment(attrs);
    }
    if (!env) {
        DestroyWindow(hwnd);
        return nullptr;
    }

    Rgba background = attrs->backgroundColor;
    bool hasBackground = attrs->hasBackgroundColor;
    if (attrs->transparent) {
        background = Rgba{0, 0, 0, 0};
        hasBackground = true;
    }

    ICoreWebView2Controller* controller = CreateController(
        hwnd, env, attrs->incognito, hasBackground ? &background : nullptr);
    if (!controller) {
        Rel(&env);
        DestroyWindow(hwnd);
        return nullptr;
    }

    ICoreWebView2* webview = nullptr;
    if (FAILED(controller->get_CoreWebView2(&webview)) || !webview) {
        controller->Close();
        Rel(&controller);
        Rel(&env);
        DestroyWindow(hwnd);
        return nullptr;
    }

    WebView* wv = new WebView();
    wv->parent = parent;
    wv->hwnd = hwnd;
    wv->isChild = asChild;
    wv->mainThreadId = GetCurrentThreadId();
    wv->controller = controller;
    wv->webview = webview;
    wv->env = env;
    wv->eventCallbacks = new WebViewEventState();
    wv->eventCallbacks->webview = wv;
    WebViewHandlersSet(wv, attrs);
    if (attrs->downloadCompletedHandler) {
        wv->downloadCallbacks = new DownloadCallbackState();
        wv->downloadCallbacks->ctx = attrs->ctx;
        wv->downloadCallbacks->fn = attrs->downloadCompletedHandler;
    }
    wv->httpOrHttps = attrs->useHttpsScheme ? "https" : "http";

    wv->id = attrs->id.s
                 ? StrDup(attrs->id)
                 : StrDup(base::FormatTemp("%lld", (int64_t)(intptr_t)hwnd));
    for (int i = 0; i < attrs->customProtocolCount; i++) {
        CustomProtocolsAppendCopy(&wv->protocols, attrs->customProtocols[i]);
    }

    if (attrs->hasTheme) {
        HRESULT hr = SetThemeInner(webview, attrs->theme);
        if (FAILED(hr) && hr != E_NOINTERFACE) {
            WebViewFree(wv);
            return nullptr;
        }
    }
    if (hasBackground && !SetBackgroundColor(controller, background)) {
        WebViewFree(wv);
        return nullptr;
    }

    EventRegistrationToken token = {};
    if (!SetWebViewSettings(webview, attrs) || !AttachHandlers(wv, &token)) {
        WebViewFree(wv);
        return nullptr;
    }
    if (!AttachDownloadHandlers(wv, &token)) {
        WebViewFree(wv);
        return nullptr;
    }
    if (!AttachIpcHandler(wv, &token) ||
        (wv->protocols.len > 0 && !AttachCustomProtocolHandler(wv, &token))) {
        WebViewFree(wv);
        return nullptr;
    }
    for (int i = 0; i < attrs->initializationScriptCount; i++) {
        if (!AddScriptToExecuteOnDocumentCreated(
                webview, attrs->initializationScripts[i].script)) {
            WebViewFree(wv);
            return nullptr;
        }
    }
    if (attrs->clipboard) {
        auto* h =
            MkHandler<Handler2<ICoreWebView2PermissionRequestedEventHandler,
                               ICoreWebView2*,
                               ICoreWebView2PermissionRequestedEventArgs*>>(
                nullptr, OnPermissionRequested);
        HRESULT hr = webview->add_PermissionRequested(h, &token);
        h->Release();
        if (FAILED(hr)) {
            WebViewFree(wv);
            return nullptr;
        }
    }

    bool navigated = true;
    if (attrs->url.s) {
        Str url = attrs->url;
        for (int i = 0; i < wv->protocols.len; i++) {

            Str prefix = base::FormatTemp("%s://", wv->protocols[i].name);
            if (base::StrStartsWith(url, prefix)) {
                url = ApplyUriWorkAround(url, Str(wv->httpOrHttps),
                                         wv->protocols[i].name);
                break;
            }
        }
        if (attrs->headers) {
            navigated =
                LoadUrlWithHeaders(wv, url, attrs->headers, attrs->headerCount);
        } else {
            navigated = SUCCEEDED(webview->Navigate(ToCWstrTemp(url)));
        }
    } else if (attrs->html.s) {
        navigated =
            SUCCEEDED(webview->NavigateToString(ToCWstrTemp(attrs->html)));
    }
    if (!navigated) {
        WebViewFree(wv);
        return nullptr;
    }

    if (!asChild) {
        AttachParentSubclass(parent, controller);
        wv->parentSubclassAttached = true;
    }
    if (FAILED(controller->put_IsVisible(attrs->visible ? TRUE : FALSE)) ||
        (attrs->focused &&
         FAILED(controller->MoveFocus(kMoveFocusReasonProgrammatic)))) {
        WebViewFree(wv);
        return nullptr;
    }

    if (attrs->browserExtensionsEnabled && attrs->extensionPath.s &&
        !LoadExtensions(webview, attrs->extensionPath)) {
        WebViewFree(wv);
        return nullptr;
    }

    if (attrs->dragDropHandler) {

        wv->oleInitialized = SUCCEEDED(OleInitialize(nullptr));
        ICoreWebView2Controller4* controller4 = nullptr;
        if (SUCCEEDED(controller->QueryInterface(
                __uuidof(ICoreWebView2Controller4), (void**)&controller4))) {
            controller4->put_AllowExternalDrop(FALSE);
            Rel(&controller4);
        }
        wv->dragDropController =
            NewDragDropController(hwnd, attrs->ctx, attrs->dragDropHandler);
    }

    if (asChild) {
        Rect bounds = attrs->hasBounds ? attrs->bounds : Rect{};
        if (!WebViewSetBounds(wv, bounds)) {
            WebViewFree(wv);
            return nullptr;
        }
    } else {
        if (!ResizeToParent(wv)) {
            WebViewFree(wv);
            return nullptr;
        }
    }
    return wv;
}

void WebViewFree(WebView* wv) {
    if (!wv) {
        return;
    }
    if (wv->eventCallbacks) {
        InterlockedExchange(&wv->eventCallbacks->alive, 0);
        wv->eventCallbacks->webview = nullptr;
    }
    if (wv->downloadCallbacks) {
        InterlockedExchange(&wv->downloadCallbacks->alive, 0);
    }
    delete wv->dragDropController;
    if (wv->oleInitialized) {
        OleUninitialize();
    }
    if (wv->controller) {
        wv->controller->Close();
    }
    if (wv->isChild && wv->hwnd) {
        DestroyWindow(wv->hwnd);
    }
    if (wv->parentSubclassAttached) {
        DetachParentSubclass(wv->parent);
    }
    if (wv->downloadCallbacks) {
        wv->downloadCallbacks->Release();
    }
    Rel(&wv->webview);
    Rel(&wv->controller);
    Rel(&wv->env);
    if (wv->eventCallbacks) {
        wv->eventCallbacks->Release();
        wv->eventCallbacks = nullptr;
    }
    CustomProtocolsFree(&wv->protocols);
    StrFree(wv->id);
    delete wv;
}

Str WebViewId(WebView* wv) {
    return wv ? wv->id : Str();
}

bool WebViewEval(WebView* wv, Str js) {
    if (!wv) {
        return false;
    }
    return ExecuteScript(wv->webview, js, EvalCallback{nullptr, nullptr});
}

bool WebViewEvalWithCallback(WebView* wv, Str js, void* ctx,
                             void (*callback)(void* ctx, Str result)) {
    if (!wv) {
        return false;
    }
    return ExecuteScript(wv->webview, js, EvalCallback{ctx, callback});
}

Str WebViewUrlTemp(WebView* wv) {
    return wv ? UrlFromWebView(wv->webview) : Str();
}

bool WebViewLoadUrl(WebView* wv, Str url) {
    if (!wv) {
        return false;
    }
    return SUCCEEDED(wv->webview->Navigate(ToCWstrTemp(url)));
}

bool WebViewLoadUrlWithHeaders(WebView* wv, Str url, const Header* headers,
                               int headerCount) {
    if (!wv) {
        return false;
    }
    return LoadUrlWithHeaders(wv, url, headers, headerCount);
}

bool WebViewLoadHtml(WebView* wv, Str html) {
    if (!wv) {
        return false;
    }
    return SUCCEEDED(wv->webview->NavigateToString(ToCWstrTemp(html)));
}

bool WebViewReload(WebView* wv) {
    if (!wv) {
        return false;
    }
    return SUCCEEDED(wv->webview->Reload());
}

bool WebViewBounds(WebView* wv, Rect* out) {
    if (!wv || !out) {
        return false;
    }
    RECT r = {};
    *out = Rect{};
    if (wv->isChild) {
        if (!GetClientRect(wv->hwnd, &r)) {
            return false;
        }
        POINT p;
        p.x = r.left;
        p.y = r.top;
        MapWindowPoints(wv->hwnd, wv->parent, &p, 1);
        out->position = PhysicalPosition((double)p.x, (double)p.y);
    } else if (FAILED(wv->controller->get_Bounds(&r))) {
        return false;
    }
    out->size =
        PhysicalSize((double)(r.right - r.left), (double)(r.bottom - r.top));
    return true;
}

bool WebViewSetBounds(WebView* wv, Rect bounds) {
    if (!wv) {
        return false;
    }
    double scale = DpiToScaleFactor(HwndDpi(wv->hwnd));
    int w = ToPhysical(bounds.size.width, bounds.size.logical, scale);
    int h = ToPhysical(bounds.size.height, bounds.size.logical, scale);
    int x = ToPhysical(bounds.position.x, bounds.position.logical, scale);
    int y = ToPhysical(bounds.position.y, bounds.position.logical, scale);
    return SetBoundsInner(wv, w, h, x, y);
}

bool WebViewSetVisible(WebView* wv, bool visible) {
    if (!wv) {
        return false;
    }
    ShowWindow(wv->hwnd, visible ? SW_SHOW : SW_HIDE);
    return SUCCEEDED(wv->controller->put_IsVisible(visible ? TRUE : FALSE));
}

bool WebViewFocus(WebView* wv) {
    if (!wv) {
        return false;
    }
    return SUCCEEDED(wv->controller->MoveFocus(kMoveFocusReasonProgrammatic));
}

bool WebViewFocusParent(WebView* wv) {
    if (!wv || !wv->parent) {
        return false;
    }

    return SetFocus(wv->parent) != nullptr;
}

bool WebViewZoom(WebView* wv, double scaleFactor) {
    if (!wv) {
        return false;
    }
    return SUCCEEDED(wv->controller->put_ZoomFactor(scaleFactor));
}

bool WebViewSetUserZoom(WebView* wv, double zoom) {
    return WebViewZoom(wv, zoom);
}

bool WebViewMatchPageScale(WebView*, float) {
    return true;
}

void WebViewSetPageClick(WebView*, WebViewPageClick, void*) {}

bool WebViewSetBackgroundColor(WebView* wv, Rgba color) {
    if (!wv) {
        return false;
    }
    return SetBackgroundColor(wv->controller, color);
}

bool WebViewSetTheme(WebView* wv, Theme theme) {
    if (!wv) {
        return false;
    }
    return SetTheme(wv->webview, theme);
}

bool WebViewSetMemoryUsageLevel(WebView* wv, MemoryUsageLevel level) {
    if (!wv) {
        return false;
    }
    ICoreWebView2_19* wv19 = nullptr;
    if (FAILED(wv->webview->QueryInterface(__uuidof(ICoreWebView2_19),
                                           (void**)&wv19))) {
        return false;
    }
    int value = level == MemoryUsageLevel::Low ? kMemoryUsageTargetLevelLow
                                               : kMemoryUsageTargetLevelNormal;
    bool ok = SUCCEEDED(wv19->put_MemoryUsageTargetLevel(value));
    Rel(&wv19);
    return ok;
}

bool WebViewReparent(WebView* wv, void* parentWindow) {
    if (!wv || !parentWindow) {
        return false;
    }
    HWND parent = (HWND)parentWindow;
    if (!SetParent(wv->hwnd, parent)) {
        return false;
    }
    if (!wv->isChild) {
        DetachParentSubclass(wv->parent);
        AttachParentSubclass(parent, wv->controller);
        wv->parentSubclassAttached = true;
        wv->parent = parent;
        return ResizeToParent(wv);
    }
    wv->parent = parent;
    return true;
}

bool WebViewSetTrafficLightInset(WebView*, Position) {
    return false;
}

bool WebViewPrint(WebView* wv) {
    return WebViewEval(wv, StrL("window.print()"));
}

static ICoreWebView2CookieManager* CookieManager(WebView* wv) {
    if (!wv) {
        return nullptr;
    }
    ICoreWebView2_2* webview2 = nullptr;
    if (FAILED(wv->webview->QueryInterface(__uuidof(ICoreWebView2_2),
                                           (void**)&webview2))) {
        return nullptr;
    }
    ICoreWebView2CookieManager* manager = nullptr;
    HRESULT hr = webview2->get_CookieManager(&manager);
    Rel(&webview2);
    if (FAILED(hr)) {
        Rel(&manager);
        return nullptr;
    }
    return manager;
}

static void FreeCookieFields(Cookie* cookie) {
    StrFree(cookie->name);
    StrFree(cookie->value);
    StrFree(cookie->domain);
    StrFree(cookie->path);
    *cookie = Cookie{};
}

static constexpr int64_t kTimeMinUnixSeconds =
    -377705116800LL;
static constexpr int64_t kTimeMaxUnixSeconds =
    253402300799LL;

static bool CookieExpiryFromDouble(double value, int64_t* result) {
    if (value != value) {
        *result = 0;
        return true;
    }

    if (value <= (double)kTimeMinUnixSeconds - 1.0 ||
        value >= (double)kTimeMaxUnixSeconds + 1.0) {
        return false;
    }
    *result = (int64_t)value;
    return true;
}

static bool CookieFromWebView2(ICoreWebView2Cookie* source, Cookie* out) {
    if (!source || !out) {
        return false;
    }
    Cookie result;
    LPWSTR raw = nullptr;
    if (FAILED(source->get_Name(&raw))) {
        return false;
    }
    result.name = StrDup(TakePwstrTemp(raw));
    raw = nullptr;
    if (FAILED(source->get_Value(&raw))) {
        FreeCookieFields(&result);
        return false;
    }
    result.value = StrDup(TakePwstrTemp(raw));
    raw = nullptr;
    if (FAILED(source->get_Domain(&raw))) {
        FreeCookieFields(&result);
        return false;
    }
    result.domain = StrDup(TakePwstrTemp(raw));
    raw = nullptr;
    if (FAILED(source->get_Path(&raw))) {
        FreeCookieFields(&result);
        return false;
    }
    result.path = StrDup(TakePwstrTemp(raw));

    BOOL flag = FALSE;
    if (FAILED(source->get_IsHttpOnly(&flag))) {
        FreeCookieFields(&result);
        return false;
    }
    result.hasHttpOnly = true;
    result.httpOnly = flag != FALSE;
    flag = FALSE;
    if (FAILED(source->get_IsSecure(&flag))) {
        FreeCookieFields(&result);
        return false;
    }
    result.hasSecure = true;
    result.secure = flag != FALSE;

    COREWEBVIEW2_COOKIE_SAME_SITE_KIND sameSite = kCookieSameSiteLax;
    if (FAILED(source->get_SameSite(&sameSite))) {
        FreeCookieFields(&result);
        return false;
    }
    result.hasSameSite = true;
    result.sameSite =
        sameSite == kCookieSameSiteStrict
            ? CookieSameSite::Strict
            : (sameSite == kCookieSameSiteLax ? CookieSameSite::Lax
                                              : CookieSameSite::None);

    BOOL isSession = FALSE;
    double expires = -1;
    if (FAILED(source->get_IsSession(&isSession)) ||
        FAILED(source->get_Expires(&expires))) {
        FreeCookieFields(&result);
        return false;
    }
    result.session = isSession != FALSE || expires == -1;
    if (!result.session) {
        result.hasExpires =
            CookieExpiryFromDouble(expires, &result.expiresUnixSeconds);
    }
    *out = result;
    return true;
}

struct CookieWait {
    LONG refs = 2;
    bool done = false;
    HRESULT result = E_FAIL;
    ICoreWebView2CookieList* cookies = nullptr;

    void Release() {
        if (InterlockedDecrement(&refs) == 0) {
            Rel(&cookies);
            delete this;
        }
    }
};

static bool CookiesInner(WebView* wv, LPCWSTR uri, Vec<Cookie>* out) {
    if (!out) {
        return false;
    }
    CookieListFree(out);
    ICoreWebView2CookieManager* manager = CookieManager(wv);
    if (!manager) {
        return false;
    }
    CookieWait* wait = new CookieWait();
    auto* handler = MkHandler<Handler2<ICoreWebView2GetCookiesCompletedHandler,
                                       HRESULT, ICoreWebView2CookieList*>>(
        wait,
        [](void* ctx, HRESULT code,
           ICoreWebView2CookieList* cookies) -> HRESULT {
            CookieWait* wait = (CookieWait*)ctx;
            wait->result = code;
            if (SUCCEEDED(code) && cookies) {
                cookies->AddRef();
                wait->cookies = cookies;
            }
            wait->done = true;
            return S_OK;
        },
        ReleaseWaitState<CookieWait>);
    HRESULT hr = manager->GetCookies(uri, handler);
    handler->Release();
    Rel(&manager);
    if (FAILED(hr)) {
        wait->Release();
        return false;
    }
    PumpUntil(&wait->done);
    if (!wait->done || FAILED(wait->result)) {
        wait->Release();
        return false;
    }
    ICoreWebView2CookieList* cookies = wait->cookies;
    wait->cookies = nullptr;
    wait->Release();
    if (!cookies) {
        return true;
    }

    UINT32 count = 0;
    if (FAILED(cookies->get_Count(&count))) {
        Rel(&cookies);
        return false;
    }
    for (UINT32 i = 0; i < count; i++) {
        ICoreWebView2Cookie* source = nullptr;
        HRESULT itemHr = cookies->GetValueAtIndex(i, &source);
        if (FAILED(itemHr) || !source) {
            Rel(&source);
            Rel(&cookies);
            CookieListFree(out);
            return false;
        }
        Cookie cookie;
        if (CookieFromWebView2(source, &cookie) && !VecAppend(*out, cookie)) {
            FreeCookieFields(&cookie);
            Rel(&source);
            Rel(&cookies);
            CookieListFree(out);
            return false;
        }
        Rel(&source);
    }
    Rel(&cookies);
    return true;
}

bool WebViewCookies(WebView* wv, Vec<Cookie>* out) {
    return CookiesInner(wv, nullptr, out);
}

bool WebViewCookiesForUrl(WebView* wv, Str url, Vec<Cookie>* out) {
    return CookiesInner(wv, ToCWstrTemp(url), out);
}

static int64_t UnixTimeNow() {
    FILETIME fileTime;
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER ticks;
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    return (int64_t)(ticks.QuadPart / 10000000ULL) - 11644473600LL;
}

static int64_t SaturatingCookieExpiry(int64_t now, int64_t duration) {
    if (duration > 0 && now > kTimeMaxUnixSeconds - duration) {
        return kTimeMaxUnixSeconds;
    }
    if (duration < 0 && now < kTimeMinUnixSeconds - duration) {
        return kTimeMinUnixSeconds;
    }
    return now + duration;
}

static ICoreWebView2Cookie* CookieToWebView2(
    ICoreWebView2CookieManager* manager, const Cookie* source) {
    if (!manager || !source) {
        return nullptr;
    }
    ICoreWebView2Cookie* cookie = nullptr;
    HRESULT hr = manager->CreateCookie(
        ToCWstrTemp(source->name), ToCWstrTemp(source->value),
        ToCWstrTemp(source->domain), ToCWstrTemp(source->path), &cookie);
    if (FAILED(hr) || !cookie) {
        return nullptr;
    }
    if (source->hasMaxAge) {
        hr = cookie->put_Expires((double)SaturatingCookieExpiry(
            UnixTimeNow(), source->maxAgeSeconds));
    } else if (source->hasExpires) {
        hr = cookie->put_Expires((double)source->expiresUnixSeconds);
    }
    if (SUCCEEDED(hr) && source->hasHttpOnly) {
        hr = cookie->put_IsHttpOnly(source->httpOnly ? TRUE : FALSE);
    }
    if (SUCCEEDED(hr) && source->hasSameSite) {
        int sameSite = source->sameSite == CookieSameSite::Strict
                           ? kCookieSameSiteStrict
                           : (source->sameSite == CookieSameSite::Lax
                                  ? kCookieSameSiteLax
                                  : kCookieSameSiteNone);
        hr = cookie->put_SameSite(sameSite);
    }
    if (SUCCEEDED(hr) && source->hasSecure) {
        hr = cookie->put_IsSecure(source->secure ? TRUE : FALSE);
    }
    if (FAILED(hr)) {
        Rel(&cookie);
    }
    return cookie;
}

bool WebViewSetCookie(WebView* wv, const Cookie* source) {
    ICoreWebView2CookieManager* manager = CookieManager(wv);
    if (!manager) {
        return false;
    }
    ICoreWebView2Cookie* cookie = CookieToWebView2(manager, source);
    bool ok = cookie && SUCCEEDED(manager->AddOrUpdateCookie(cookie));
    Rel(&cookie);
    Rel(&manager);
    return ok;
}

bool WebViewDeleteCookie(WebView* wv, const Cookie* source) {
    ICoreWebView2CookieManager* manager = CookieManager(wv);
    if (!manager) {
        return false;
    }
    ICoreWebView2Cookie* cookie = CookieToWebView2(manager, source);
    bool ok = cookie && SUCCEEDED(manager->DeleteCookie(cookie));
    Rel(&cookie);
    Rel(&manager);
    return ok;
}

bool WebViewClearAllBrowsingData(WebView* wv) {
    if (!wv) {
        return false;
    }
    ICoreWebView2_13* wv13 = nullptr;
    if (FAILED(wv->webview->QueryInterface(__uuidof(ICoreWebView2_13),
                                           (void**)&wv13))) {
        return false;
    }
    bool ok = false;
    ICoreWebView2Profile* profile = nullptr;
    if (SUCCEEDED(wv13->get_Profile(&profile)) && profile) {
        ICoreWebView2Profile2* profile2 = nullptr;
        if (SUCCEEDED(profile->QueryInterface(__uuidof(ICoreWebView2Profile2),
                                              (void**)&profile2))) {
            auto* h = MkHandler<Handler1<
                ICoreWebView2ClearBrowsingDataCompletedHandler, HRESULT>>(
                nullptr, [](void*, HRESULT) -> HRESULT { return S_OK; });
            ok = SUCCEEDED(profile2->ClearBrowsingDataAll(h));
            h->Release();
            Rel(&profile2);
        }
        Rel(&profile);
    }
    Rel(&wv13);
    return ok;
}

void WebViewOpenDevtools(WebView* wv) {
    if (wv) {
        wv->webview->OpenDevToolsWindow();
    }
}

void WebViewCloseDevtools(WebView*) {}

bool WebViewIsDevtoolsOpen(WebView*) {
    return false;
}

void* WebViewControllerRaw(WebView* wv) {
    return wv ? wv->controller : nullptr;
}

void* WebViewEnvironmentRaw(WebView* wv) {
    return wv ? wv->env : nullptr;
}

void* WebViewNativeRaw(WebView* wv) {
    return wv ? wv->webview : nullptr;
}

}

#endif
