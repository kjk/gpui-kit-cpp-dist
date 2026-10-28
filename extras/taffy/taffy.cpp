#define GPUI_INCLUDE_PRIVATE_API 1
#include "taffy.h"

#include <climits>
#include <cstdarg>
#include <cstring>
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
        gTodayChecked = true;
        const char* env = getenv("GPUI_TODAY");
        if (env) {
            LocalDate pinned;
            if (DateParseIso(env, &pinned)) {
                gTodayPinned = pinned;
            }
        }
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

static bool IsUnicodeWhiteSpace(uint32_t cp) {
    return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0x85 ||
           cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
           cp == 0x3000;
}

static uint32_t StrTrimDecode(Str s, int i, int* n) {
    uint8_t c = (uint8_t)s.s[i];
    int want = c < 0x80         ? 1
               : (c >> 5) == 6  ? 2
               : (c >> 4) == 14 ? 3
               : (c >> 3) == 30 ? 4
                                : 0;
    if (want <= 1 || i + want > len(s)) {
        *n = 1;
        return c;
    }
    uint32_t cp = c & (0x7F >> want);
    for (int k = 1; k < want; k++) {
        uint8_t b = (uint8_t)s.s[i + k];
        if ((b & 0xC0) != 0x80) {
            *n = 1;
            return c;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    *n = want;
    return cp;
}

Str StrTrim(Str s) {
    if (!s.s || len(s) <= 0) {
        return s;
    }
    int start = 0;
    int end = len(s);
    while (start < end) {
        int n = 1;
        if (!IsUnicodeWhiteSpace(StrTrimDecode(s, start, &n))) {
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
        int n = 1;
        uint32_t cp = StrTrimDecode(s, at, &n);
        if (at + n != end || !IsUnicodeWhiteSpace(cp)) {
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
}

#line 1 "src/taffy/compute_block.cpp"

namespace taffy {

struct ContentSlot {

    int segmentId = -1;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct BfcSlot {
    int segmentId = -1;
    float x = 0.0f;
    float y = 0.0f;
    float borderWidth = 0.0f;
    float stretchWidth = 0.0f;
};

struct PlacedFloatedBox {
    float width = 0.0f;
    float height = 0.0f;

    float xInset = 0.0f;

    float y = 0.0f;
};

static constexpr float kFitTolerance = 0.001f;

static bool FloatFitsHorizontally(float width, FloatDirection direction,
                                  float bfcWidth, const float floatInsets[2],
                                  const float cbInsets[2]) {
    int lead = (int)direction;
    int trail = 1 - lead;
    float xInset = F32Max(floatInsets[lead], cbInsets[lead]);
    bool fitsOpposite =
        floatInsets[trail] == 0.0f ||
        xInset + width <= bfcWidth - floatInsets[trail] + kFitTolerance;
    bool fitsContaining =
        floatInsets[lead] == 0.0f ||
        xInset + width <= bfcWidth - cbInsets[trail] + kFitTolerance;
    return fitsOpposite && fitsContaining;
}

struct Segment {
    float yStart = 0.0f;
    float yEnd = 0.0f;

    float insets[2] = {0.0f, 0.0f};

    bool hasFloat[2] = {false, false};

    bool FitsFloatWidth(SizeF floatedBox, FloatDirection direction,
                        float bfcWidth, const float cbInsets[2]) const {
        return FloatFitsHorizontally(floatedBox.w, direction, bfcWidth, insets,
                                     cbInsets);
    }
    bool Contains(float y) const { return y >= yStart && y < yEnd; }
};

struct FloatFitter {
    float bfcWidth = 0.0f;
    double slotHeight = 0.0;
    float floatInsets[2] = {0.0f, 0.0f};
    float cbInsets[2] = {0.0f, 0.0f};

    FloatFitter(float bfcWidth_, float slotHeight_, const float in[2])
        : bfcWidth(bfcWidth_), slotHeight((double)slotHeight_) {
        cbInsets[0] = in[0];
        cbInsets[1] = in[1];
    }

    void UnionInsets(const float other[2]) {
        floatInsets[0] = F32Max(floatInsets[0], other[0]);
        floatInsets[1] = F32Max(floatInsets[1], other[1]);
    }
    float PlacedInset(FloatDirection direction) const {
        int lead = (int)direction;
        return F32Max(floatInsets[lead], cbInsets[lead]);
    }
    bool FitsHorizontally(float width, FloatDirection direction) const {
        return FloatFitsHorizontally(width, direction, bfcWidth, floatInsets,
                                     cbInsets);
    }
    void AddHeight(float height) { slotHeight += (double)height; }
    bool FitsVertically(float height) const {
        return slotHeight >= (double)height;
    }
};

struct IndexRange {
    int start = 0;
    int end = 0;
};

struct FloatContext {

    float availableWidth = 0.0f;
    bool hasFloats = false;
    Vec<PlacedFloatedBox> leftFloats;
    Vec<PlacedFloatedBox> rightFloats;

    Vec<Segment> segments;

    IndexRange lastPlacedFloats[2];

    Optf clearBottoms[2] = {None(), None()};
    Optf floatCeiling = None();

    float LastSegmentEnd() const {
        return len(segments) > 0 ? segments[len(segments) - 1].yEnd : 0.0f;
    }

    bool HasActiveFloats(float minY) const {
        return hasFloats && LastSegmentEnd() > minY;
    }

    void SetWidth(float w) { availableWidth = w; }

    void SubdivideSegment(int idx, float divideAtY) {
        Segment newSegment;
        newSegment.insets[0] = segments[idx].insets[0];
        newSegment.insets[1] = segments[idx].insets[1];
        newSegment.hasFloat[0] = segments[idx].hasFloat[0];
        newSegment.hasFloat[1] = segments[idx].hasFloat[1];
        newSegment.yStart = divideAtY;
        newSegment.yEnd = segments[idx].yEnd;
        segments[idx].yEnd = divideAtY;
        VecInsertAt(segments, idx + 1, newSegment);
    }

    void UpdateLastPlacedFloat(FloatDirection direction, IndexRange placement) {
        int slot = (int)direction;
        IndexRange& r = lastPlacedFloats[slot];
        r.start = r.start > placement.start ? r.start : placement.start;
        r.end = r.end > placement.end ? r.end : placement.end;
    }

    PlacedFloatedBox PlaceFloatedBoxInner(SizeF floatedBox, float minY,
                                          const float containingBlockInsets[2],
                                          FloatDirection direction,
                                          Clear clear);

    PointF PlaceFloatedBox(SizeF floatedBox, float minY,
                           const float containingBlockInsets[2],
                           FloatDirection direction, Clear clear) {
        hasFloats = true;
        PlacedFloatedBox placed = PlaceFloatedBoxInner(
            floatedBox, minY, containingBlockInsets, direction, clear);
        int slot = (int)direction;
        float bottom = placed.y + placed.height;
        clearBottoms[slot] =
            Some(IsSome(clearBottoms[slot]) ? F32Max(clearBottoms[slot], bottom)
                                            : bottom);
        floatCeiling = Some(
            IsSome(floatCeiling) ? F32Max(floatCeiling, placed.y) : placed.y);
        float xInset = placed.xInset;
        float y = placed.y;
        if (direction == FloatDirection::Left) {
            VecAppend(leftFloats, placed);
            return {xInset, y};
        }
        VecAppend(rightFloats, placed);
        return {availableWidth - xInset - floatedBox.w, y};
    }

    int ClearedSegment(Clear clear) const {
        int left = lastPlacedFloats[0].end;
        int right = lastPlacedFloats[1].end;
        switch (clear) {
            case Clear::Left:
                return left > 0 ? left : -1;
            case Clear::Right:
                return right > 0 ? right : -1;
            case Clear::Both: {
                return left > 0 || right > 0 ? (left > right ? left : right)
                                             : -1;
            }
            default:
                return -1;
        }
    }

    Optf ClearedThreshold(Clear clear) const {
        switch (clear) {
            case Clear::Left:
                return clearBottoms[0];
            case Clear::Right:
                return clearBottoms[1];
            case Clear::Both:
                if (IsSome(clearBottoms[0]) && IsSome(clearBottoms[1])) {
                    return Some(F32Max(clearBottoms[0], clearBottoms[1]));
                }
                return IsSome(clearBottoms[0]) ? clearBottoms[0]
                                               : clearBottoms[1];
            default:
                return None();
        }
    }

    ContentSlot FindContentSlot(float minY,
                                const float containingBlockInsets[2],
                                Clear clear, int after) const;
    BfcSlot FindBfcSlot(float minY, const float containingBlockInsets[2],
                        const float margins[2], Direction direction,
                        Clear clear, int after) const;
};

PlacedFloatedBox FloatContext::PlaceFloatedBoxInner(
    SizeF floatedBox, float minY, const float containingBlockInsets[2],
    FloatDirection direction, Clear clear) {
    int slot = (int)direction;

    minY = F32Max(minY, UnwrapOr(floatCeiling, -INFINITY));
    minY = F32Max(minY, UnwrapOr(ClearedThreshold(clear), -INFINITY));

    int floatStart = lastPlacedFloats[0].start > lastPlacedFloats[1].start
                         ? lastPlacedFloats[0].start
                         : lastPlacedFloats[1].start;
    int hwm = 0;
    switch (clear) {
        case Clear::Left: {
            int b = lastPlacedFloats[0].end + 1;
            hwm = floatStart > b ? floatStart : b;
            break;
        }
        case Clear::Right: {
            int b = lastPlacedFloats[1].end + 1;
            hwm = floatStart > b ? floatStart : b;
            break;
        }
        case Clear::Both: {
            int l = lastPlacedFloats[0].end;
            int r = lastPlacedFloats[1].end;
            hwm = (l > r ? l : r) + 1;
            break;
        }
        default:
            hwm = floatStart;
            break;
    }

    int startIdx = segments.len;
    for (int i = hwm; i < segments.len; i++) {
        if (segments[i].yEnd > minY) {
            startIdx = i;
            break;
        }
    }
    float startY = minY;
    int endIdx = startIdx;

    bool haveStart = false;
    bool haveEnd = false;
    int foundStart = 0;
    int foundEnd = 0;
    float placedInset = containingBlockInsets[slot];

    while (true) {

        if (startIdx >= segments.len) {
            haveStart = false;
            haveEnd = false;
            placedInset = containingBlockInsets[slot];
            break;
        }

        const Segment& startSegment = segments[startIdx];
        if (!startSegment.FitsFloatWidth(floatedBox, direction, availableWidth,
                                         containingBlockInsets)) {
            startIdx++;
            if (endIdx < startIdx) {
                endIdx = startIdx;
            }
            continue;
        }

        startY = F32Max(startY, startSegment.yStart);
        float availableHeight = startSegment.yEnd - startY;
        FloatFitter fitter(availableWidth, availableHeight,
                           containingBlockInsets);
        fitter.UnionInsets(startSegment.insets);

        bool restartOuter = false;
        while (true) {
            if (endIdx >= segments.len) {
                haveStart = true;
                foundStart = startIdx;
                haveEnd = false;
                placedInset = fitter.PlacedInset(direction);
                break;
            }
            const Segment& endSegment = segments[endIdx];
            fitter.UnionInsets(endSegment.insets);
            if (!fitter.FitsHorizontally(floatedBox.w, direction)) {
                startIdx++;
                if (endIdx < startIdx) {
                    endIdx = startIdx;
                }
                restartOuter = true;
                break;
            }
            if (endIdx != startIdx) {
                fitter.AddHeight(endSegment.yEnd - endSegment.yStart);
            }
            if (!fitter.FitsVertically(floatedBox.h)) {
                endIdx++;
                continue;
            }
            haveStart = true;
            foundStart = startIdx;
            haveEnd = true;
            foundEnd = endIdx;
            placedInset = fitter.PlacedInset(direction);
            break;
        }
        if (restartOuter) {
            continue;
        }
        break;
    }

    PlacedFloatedBox out;
    out.width = floatedBox.w;
    out.height = floatedBox.h;
    out.y = startY;
    out.xInset = placedInset;

    if (floatedBox.h == 0.0f) {
        return out;
    }

    if (!haveStart) {
        float lastYEnd = LastSegmentEnd();
        if (startY > lastYEnd) {
            Segment gap;
            gap.yStart = lastYEnd;
            gap.yEnd = startY;
            VecAppend(segments, gap);
        }
        float newStartY = F32Max(lastYEnd, startY);
        Segment seg;
        seg.yStart = newStartY;
        seg.yEnd = newStartY + floatedBox.h;
        seg.insets[0] = containingBlockInsets[0];
        seg.insets[1] = containingBlockInsets[1];
        seg.insets[slot] += floatedBox.w;
        seg.hasFloat[slot] = true;
        VecAppend(segments, seg);

        int si = segments.len - 1;
        UpdateLastPlacedFloat(direction, {si, si + 1});

        out.y = newStartY;
        out.xInset = containingBlockInsets[slot];
        return out;
    }

    int si = foundStart;

    if (startY != segments[si].yStart) {
        SubdivideSegment(si, startY);
        si++;
        if (haveEnd) {
            foundEnd++;
        }
    }

    int ei;
    if (!haveEnd) {
        float lastYEnd = LastSegmentEnd();
        if (minY > lastYEnd) {
            Segment gap;
            gap.yStart = lastYEnd;
            gap.yEnd = minY;
            VecAppend(segments, gap);
        }
        ei = segments.len - 1;
    } else {
        ei = foundEnd;
        float endY = startY + floatedBox.h;
        while (ei > si && endY <= segments[ei].yStart) {
            ei--;
        }
        if (segments[ei].yStart < endY && endY < segments[ei].yEnd) {
            SubdivideSegment(ei, endY);
        }
    }

    float placedInsetPlusWidth = placedInset + floatedBox.w;
    for (int i = si; i <= ei && i < segments.len; i++) {
        segments[i].insets[slot] = placedInsetPlusWidth;
        segments[i].hasFloat[slot] = true;
    }

    UpdateLastPlacedFloat(direction, {si, ei + 1});
    return out;
}

ContentSlot FloatContext::FindContentSlot(float minY,
                                          const float containingBlockInsets[2],
                                          Clear clear, int after) const {
    ContentSlot fallback;
    fallback.segmentId = -1;
    fallback.x = containingBlockInsets[0];
    fallback.y = minY;
    fallback.width =
        availableWidth - containingBlockInsets[0] - containingBlockInsets[1];
    fallback.height = INFINITY;

    if (!HasActiveFloats(minY)) {
        return fallback;
    }

    minY = F32Max(minY, UnwrapOr(ClearedThreshold(clear), -INFINITY));

    int atLeast = after >= 0 ? after + 1 : 0;
    int cleared = ClearedSegment(clear);
    int hwm = cleared >= 0 ? cleared + 1 : 0;
    if (atLeast > hwm) {
        hwm = atLeast;
    }

    int startIdx = segments.len;
    for (int i = hwm; i < segments.len; i++) {
        if (segments[i].yEnd > minY) {
            startIdx = i;
            break;
        }
    }
    if (startIdx >= segments.len) {
        return fallback;
    }

    const Segment& segment = segments[startIdx];
    float insetLeft = F32Max(segment.insets[0], containingBlockInsets[0]);
    float insetRight = F32Max(segment.insets[1], containingBlockInsets[1]);
    ContentSlot slot;
    slot.segmentId = startIdx;
    slot.x = insetLeft;
    slot.y = F32Max(segment.yStart, minY);
    slot.width = availableWidth - insetLeft - insetRight;
    slot.height = INFINITY;
    return slot;
}

BfcSlot FloatContext::FindBfcSlot(float minY,
                                  const float containingBlockInsets[2],
                                  const float margins[2], Direction direction,
                                  Clear clear, int after) const {
    float marginInsets[2] = {containingBlockInsets[0] + margins[0],
                             containingBlockInsets[1] + margins[1]};
    float noFloatWidth = availableWidth - marginInsets[0] - marginInsets[1];
    BfcSlot fallback;
    fallback.x = marginInsets[0];
    fallback.y = minY;
    fallback.borderWidth = noFloatWidth;
    fallback.stretchWidth = noFloatWidth;
    if (!HasActiveFloats(minY)) {
        return fallback;
    }

    minY = F32Max(minY, UnwrapOr(ClearedThreshold(clear), -INFINITY));
    int atLeast = after >= 0 ? after + 1 : 0;
    int cleared = ClearedSegment(clear);
    int hwm = cleared >= 0 ? cleared + 1 : 0;
    if (atLeast > hwm) {
        hwm = atLeast;
    }
    int startIdx = segments.len;
    for (int i = hwm; i < segments.len; i++) {
        if (segments[i].yEnd > minY) {
            startIdx = i;
            break;
        }
    }
    if (startIdx >= segments.len) {
        fallback.y = F32Max(LastSegmentEnd(), minY);
        return fallback;
    }

    const Segment& segment = segments[startIdx];
    int lead = direction == Direction::Ltr ? 0 : 1;
    int trail = 1 - lead;
    bool hasLeadFloat = segment.hasFloat[lead];
    bool hasTrailFloat = segment.hasFloat[trail];
    float fitInsets[2] = {};
    float stretchInsets[2] = {};
    fitInsets[lead] = hasLeadFloat
                          ? F32Max(segment.insets[lead], marginInsets[lead])
                          : marginInsets[lead];
    stretchInsets[lead] = fitInsets[lead];
    fitInsets[trail] =
        hasTrailFloat
            ? F32Max(segment.insets[trail], containingBlockInsets[trail])
            : F32Min(marginInsets[trail], containingBlockInsets[trail]);
    stretchInsets[trail] =
        hasTrailFloat ? F32Max(segment.insets[trail], marginInsets[trail])
                      : marginInsets[trail];

    BfcSlot slot;
    slot.segmentId = startIdx;
    slot.x = fitInsets[0];
    slot.y = F32Max(segment.yStart, minY);
    slot.borderWidth = availableWidth - fitInsets[0] - fitInsets[1];
    slot.stretchWidth = availableWidth - stretchInsets[0] - stretchInsets[1];
    return slot;
}

struct FloatIntrinsicWidthCalculator {
    AvailableSpace availableWidth;

    float sideSums[2] = {};
    float contribution = 0.0f;
    float widest = 0.0f;

    void AddFloat(float width, FloatDirection direction, Clear clear) {
        switch (availableWidth.kind) {
            case AvailableSpace::Kind::Definite:
            case AvailableSpace::Kind::MaxContent:

                if (clear == Clear::Left || clear == Clear::Both) {
                    sideSums[0] = 0.0f;
                }
                if (clear == Clear::Right || clear == Clear::Both) {
                    sideSums[1] = 0.0f;
                }
                sideSums[(int)direction] += width;
                contribution = F32Max(contribution, sideSums[0] + sideSums[1]);
                break;
            case AvailableSpace::Kind::MinContent:
                contribution = F32Max(contribution, width);
                break;
        }
        widest = F32Max(widest, width);
    }
    float Result() const {
        if (availableWidth.kind == AvailableSpace::Kind::Definite) {
            return F32Max(F32Min(contribution, availableWidth.value), widest);
        }
        return contribution;
    }
};

struct BlockFormattingContext {
    FloatContext floatContext;
};

struct BlockContext {
    BlockFormattingContext* bfc = nullptr;

    float yOffset = 0.0f;

    float insets[2] = {0.0f, 0.0f};

    float contentBoxInsets[2] = {0.0f, 0.0f};

    float floatContentContribution = -INFINITY;
    bool isRoot = false;
    bool adjoiningFloats[2] = {false, false};
    bool topAdjoiningFloats[2] = {false, false};
    bool hasTopAdjoiningFloats = false;

    BlockContext SubContext(float additionalYOffset,
                            const float childInsets[2]) {
        BlockContext out;
        out.bfc = bfc;
        out.yOffset = yOffset + additionalYOffset;
        out.insets[0] = insets[0] + childInsets[0];
        out.insets[1] = insets[1] + childInsets[1];
        out.contentBoxInsets[0] = out.insets[0];
        out.contentBoxInsets[1] = out.insets[1];
        out.isRoot = false;
        out.adjoiningFloats[0] = adjoiningFloats[0];
        out.adjoiningFloats[1] = adjoiningFloats[1];
        return out;
    }

    bool IsBfcRoot() const { return isRoot; }

    void SetWidth(float availableWidth) {
        bfc->floatContext.SetWidth(availableWidth);
    }

    void ApplyContentBoxInset(const float contentBoxXInsets[2]) {
        contentBoxInsets[0] = insets[0] + contentBoxXInsets[0];
        contentBoxInsets[1] = insets[1] + contentBoxXInsets[1];
    }
    bool HasFloats() const { return bfc->floatContext.hasFloats; }
    bool HasActiveFloats(float minY) const {
        return bfc->floatContext.HasActiveFloats(minY + yOffset);
    }
    PointF PlaceFloatedBox(SizeF floatedBox, float minY,
                           FloatDirection direction, Clear clear,
                           bool adjoinsUnresolvedStrut) {
        if (adjoinsUnresolvedStrut) {
            adjoiningFloats[(int)direction] = true;
        }
        PointF pos = bfc->floatContext.PlaceFloatedBox(
            floatedBox, minY + yOffset, contentBoxInsets, direction, clear);
        pos.y -= yOffset;
        pos.x -= insets[0];
        floatContentContribution =
            F32Max(floatContentContribution, pos.y + floatedBox.h);
        return pos;
    }
    ContentSlot FindContentSlot(float minY, Clear clear, int after) const {
        ContentSlot slot = bfc->floatContext.FindContentSlot(
            minY + yOffset, contentBoxInsets, clear, after);
        slot.y -= yOffset;
        slot.x -= insets[0];
        return slot;
    }
    BfcSlot FindBfcSlot(float minY, const float margins[2], Direction direction,
                        Clear clear, int after) const {
        BfcSlot slot = bfc->floatContext.FindBfcSlot(
            minY + yOffset, contentBoxInsets, margins, direction, clear, after);
        slot.y -= yOffset;
        slot.x -= insets[0];
        return slot;
    }
    Optf ClearedThreshold(Clear clear) const {
        Optf t = bfc->floatContext.ClearedThreshold(clear);
        if (IsSome(t)) {
            return Some(t - yOffset);
        }
        return None();
    }
    bool HasAdjoiningFloat(Clear clear) const {
        switch (clear) {
            case Clear::Left:
                return adjoiningFloats[0];
            case Clear::Right:
                return adjoiningFloats[1];
            case Clear::Both:
                return adjoiningFloats[0] || adjoiningFloats[1];
            default:
                return false;
        }
    }
    void MergeAdjoiningFloats(const bool flags[2]) {
        adjoiningFloats[0] = adjoiningFloats[0] || flags[0];
        adjoiningFloats[1] = adjoiningFloats[1] || flags[1];
    }
    void CommitStrut() {
        if (!hasTopAdjoiningFloats) {
            topAdjoiningFloats[0] = adjoiningFloats[0];
            topAdjoiningFloats[1] = adjoiningFloats[1];
            hasTopAdjoiningFloats = true;
        }
        adjoiningFloats[0] = false;
        adjoiningFloats[1] = false;
    }
    void GetTopAdjoiningFloats(bool out[2]) const {
        out[0] =
            hasTopAdjoiningFloats ? topAdjoiningFloats[0] : adjoiningFloats[0];
        out[1] =
            hasTopAdjoiningFloats ? topAdjoiningFloats[1] : adjoiningFloats[1];
    }
    void AddChildFloatedContentHeightContribution(float childContribution) {
        floatContentContribution =
            F32Max(floatContentContribution, childContribution);
    }
    float FloatedContentHeightContribution() const {
        return floatContentContribution;
    }
};

namespace {

struct BlockItem {
    NodeId nodeId;

    uint32_t order = 0;

    bool isTable = false;

    bool isCompressibleReplaced = false;

    bool isInSameBfc = false;

    Float floatMode = Float::None;
    Clear clear = Clear::None;

    SizeFOpt size = SizeFOptNone();

    SizeDim sizeStyle;
    SizeFOpt minSize = SizeFOptNone();
    SizeFOpt maxSize = SizeFOptNone();

    PointOverflow overflow;
    Contain contain;
    float scrollbarWidth = 0.0f;

    Position position = Position::Static;

    Direction direction = Direction::Ltr;
    OptAlignSelf alignSelf;
    OptAlignSelf justifySelf;
    RectLpa inset;
    RectLpa margin;
    RectF padding;
    RectF border;
    SizeF paddingBorderSum;

    SizeF computedSize;

    PointF staticPosition;
    bool canBeCollapsedThrough = false;

    bool hasFinalLayout = false;
    Layout finalLayout;

    int oofStart = 0;
    int oofCount = 0;
};

void GenerateItemList(TaffyTree* tree, NodeId node, SizeFOpt nodeInnerSize,
                      Vec<BlockItem>* items) {
    CalcResolver calc = tree->calc;
    int n = tree->ChildCount(node);
    uint32_t order = 0;
    for (int i = 0; i < n; i++) {
        NodeId childNodeId = tree->GetChildId(node, i);
        const Style& cs = tree->GetStyle(childNodeId);
        if (cs.BoxGenMode() == BoxGenerationMode::None) {
            continue;
        }

        Optf aspectRatio = cs.aspectRatio;

        RectF padding = cs.padding.ResolveOrZero(nodeInnerSize.w, calc);
        RectF border = cs.border.ResolveOrZero(nodeInnerSize.w, calc);
        SizeF pbSum = (padding + border).SumAxes();
        SizeF boxSizingAdjustment =
            cs.boxSizing == BoxSizing::ContentBox ? pbSum : SizeF::Zero();

        BlockItem item;
        item.nodeId = childNodeId;
        item.order = order++;
        item.isTable = cs.itemIsTable;
        item.isCompressibleReplaced = cs.IsCompressibleReplaced();
        item.floatMode = cs.floatMode;
        item.clear = cs.clear;
        item.position = cs.position;
        item.direction = cs.direction;
        item.alignSelf = cs.alignSelf;
        item.justifySelf = cs.justifySelf;
        item.overflow = cs.overflow;
        item.contain = cs.contain;
        item.scrollbarWidth = cs.scrollbarWidth;

        bool isNotFloated = cs.floatMode == Float::None;
        bool isScrollContainer = IsScrollContainer(cs.overflow.x) ||
                                 IsScrollContainer(cs.overflow.y);

        item.isInSameBfc =
            cs.IsBlock() && !cs.itemIsTable && !IsOutOfFlow(cs.position) &&
            isNotFloated && !isScrollContainer &&
            !cs.contain.EstablishesIndependentFormattingContext() &&
            cs.alignContent.keyword == AlignContentKeyword::Normal;

        item.sizeStyle = cs.size;
        item.size = MaybeAdd(
            MaybeApplyAspectRatio(cs.size.MaybeResolve(nodeInnerSize, calc),
                                  aspectRatio),
            boxSizingAdjustment);
        item.minSize = MaybeAdd(
            MaybeApplyAspectRatio(cs.minSize.MaybeResolve(nodeInnerSize, calc),
                                  aspectRatio),
            boxSizingAdjustment);
        item.maxSize = MaybeAdd(
            MaybeApplyAspectRatio(cs.maxSize.MaybeResolve(nodeInnerSize, calc),
                                  aspectRatio),
            boxSizingAdjustment);
        item.inset = cs.inset;
        item.margin = cs.margin;
        item.padding = padding;
        item.border = border;
        item.paddingBorderSum = pbSum;
        VecAppend(*items, item);
    }
}

Optf ResolveStretchHeight(Dimension heightStyle, Optf containerInnerHeight,
                          RectF itemMargin, LineBool marginsTreatedAsZero) {
    if (!heightStyle.IsStretch() || !IsSome(containerInnerHeight)) {
        return None();
    }
    float height = containerInnerHeight;
    if (!marginsTreatedAsZero.start) {
        height -= itemMargin.top;
    }
    if (!marginsTreatedAsZero.end) {
        height -= itemMargin.bottom;
    }
    return Some(height);
}

float DetermineContentBasedContainerWidth(TaffyTree* tree,
                                          const Vec<BlockItem>& items,
                                          AvailableSpace availableWidth) {
    CalcResolver calc = tree->calc;
    SizeAvail availableSpace = {availableWidth, AvailableSpace::MinContent()};

    float maxChildWidth = 0.0f;
    FloatIntrinsicWidthCalculator floatContribution;
    floatContribution.availableWidth = availableWidth;

    for (int i = 0; i < len(items); i++) {
        const BlockItem& item = items[i];
        if (IsOutOfFlow(item.position)) {
            continue;
        }
        SizeFOpt knownDimensions =
            MaybeClamp(item.size, item.minSize, item.maxSize);
        float itemXMarginSum =
            item.margin.ResolveOrZero(availableSpace.width.IntoOption(), calc)
                .HorizontalAxisSum();
        float width;
        if (IsSome(knownDimensions.w)) {
            width = knownDimensions.w;
        } else {
            SizeAvail childAvail = availableSpace;
            SizingKeywordResolution keyword = ResolveSizingKeyword(
                item.sizeStyle.width, None(), None(), calc);
            if (keyword.isSome && keyword.kind == SizingKeywordKind::Exact) {
                childAvail.width = AvailableSpace::Definite(keyword.exact);
            } else if (keyword.isSome) {
                childAvail.width = keyword.measure;
            } else {
                childAvail.width = MaybeSub(childAvail.width, itemXMarginSum);
            }

            width = tree->MeasureChildSize(
                item.nodeId, knownDimensions, SizeFOptNone(), childAvail,
                SizingMode::InherentSize, AbsoluteAxis::Horizontal,
                item.isInSameBfc ? LineBool::True() : LineBool::False());
        }
        width = F32Max(width, item.paddingBorderSum.w) + itemXMarginSum;

        if (IsFloated(item.floatMode)) {
            floatContribution
                .AddFloat(width, FloatDir(item.floatMode).val, item.clear);
            continue;
        }
        maxChildWidth = F32Max(maxChildWidth, width);
    }

    return F32Max(maxChildWidth, floatContribution.Result());
}

struct InFlowResult {
    RectF inflowOverflowRect;
    float intrinsicOuterHeight = 0.0f;
    CollapsibleMarginSet firstChildTopMarginSet;
    CollapsibleMarginSet lastChildBottomMarginSet;
    Optf firstBaseline = None();
};

InFlowResult PerformFinalLayoutOnInFlowChildren(
    TaffyTree* tree, RunMode runMode, Vec<BlockItem>* items,
    Vec<OofCandidate>* oofPool, float containerOuterWidth,
    Optf containerPercentageResolutionHeight, RectF contentBoxInset,
    RectF resolvedContentBoxInset, RectF resolvedBorder, TextAlign textAlign,
    AlignItems justifyItems, Direction direction,
    LineBool ownMarginsCollapseWithChildren,
    LineBool childrenMarginsAdjoinOwnEdges, bool isScrollContainer,
    BlockContext* blockCtx) {
    CalcResolver calc = tree->calc;

    float containerInnerWidth =
        F32Max(0.0f, containerOuterWidth - resolvedContentBoxInset
                                               .HorizontalAxisSum());
    Optf percentageResolutionHeight =
        MaybeSub(containerPercentageResolutionHeight, resolvedContentBoxInset
                                                          .VerticalAxisSum());
    SizeFOpt parentSize = {Some(containerInnerWidth),
                           percentageResolutionHeight};

    SizeAvail availableSpace = {AvailableSpace::Definite(containerInnerWidth),
                                AvailableSpace::MaxContent()};

    if (blockCtx->IsBfcRoot()) {
        blockCtx->SetWidth(containerOuterWidth);
        float xInsets[2] = {resolvedContentBoxInset.left,
                            resolvedContentBoxInset.right};
        blockCtx->ApplyContentBoxInset(xInsets);
    }

    if (!ownMarginsCollapseWithChildren.start) {
        blockCtx->CommitStrut();
    }

    InFlowResult res;
    float committedYOffset = resolvedContentBoxInset.top;
    float yOffsetForAbsolute = resolvedContentBoxInset.top;
    CollapsibleMarginSet activeCollapsibleMarginSet;
    bool isCollapsingWithFirstMarginSet = true;
    bool activeMarginSetHasClearance = false;
    bool hasActiveFloats = blockCtx->HasActiveFloats(committedYOffset);

    for (int itemIdx = 0; itemIdx < items->len; itemIdx++) {
        BlockItem& item = (*items)[itemIdx];
        if (IsOutOfFlow(item.position)) {
            float x = direction == Direction::Ltr
                          ? resolvedContentBoxInset.left
                          : containerOuterWidth - resolvedContentBoxInset.right;
            item.staticPosition = {x, yOffsetForAbsolute};
            continue;
        }

        RectFOpt itemMargin =
            item.margin.MaybeResolve(Some(containerInnerWidth), calc);
        RectF itemNonAutoMargin = {
            UnwrapOr(itemMargin.left, 0.0f), UnwrapOr(itemMargin.right, 0.0f),
            UnwrapOr(itemMargin.top, 0.0f), UnwrapOr(itemMargin.bottom, 0.0f)};
        float itemNonAutoXMarginSum = itemNonAutoMargin.HorizontalAxisSum();

        SizeF scrollbarSize = {
            item.overflow.y == Overflow::Scroll ? item.scrollbarWidth : 0.0f,
            item.overflow.x == Overflow::Scroll ? item.scrollbarWidth : 0.0f};

        OptFloatDirection floatDirection = FloatDir(item.floatMode);
        if (floatDirection.IsSome()) {
            hasActiveFloats = true;

            float availableWidth =
                F32Max(0.0f, containerInnerWidth - itemNonAutoXMarginSum);
            Optf itemKnownWidth = None();
            AvailableSpace itemAvailableWidth =
                AvailableSpace::Definite(availableWidth);
            SizingKeywordResolution keywordWidth =
                ResolveSizingKeyword(item.sizeStyle.width, Some(availableWidth),
                                     Some(containerInnerWidth), calc);
            if (keywordWidth.isSome &&
                keywordWidth.kind == SizingKeywordKind::Measure) {
                itemAvailableWidth = keywordWidth.measure;
            } else if (keywordWidth.isSome) {
                itemKnownWidth = Some(keywordWidth.exact);
                itemAvailableWidth =
                    AvailableSpace::Definite(keywordWidth.exact);
            }
            Optf itemKnownHeight = ResolveStretchHeight(
                item.sizeStyle.height, percentageResolutionHeight,
                itemNonAutoMargin, LineBool::False());
            LayoutOutput itemLayout = tree->PerformChildLayout(
                item.nodeId, {itemKnownWidth, itemKnownHeight}, parentSize,
                {itemAvailableWidth, AvailableSpace::MaxContent()},
                SizingMode::InherentSize, LineBool::False());
            SizeF marginBox = itemLayout.size + itemNonAutoMargin.SumAxes();

            bool adjoinsUnresolvedStrut = isCollapsingWithFirstMarginSet &&
                                          ownMarginsCollapseWithChildren.start;
            float yOffsetForFloat =
                adjoinsUnresolvedStrut
                    ? committedYOffset
                    : committedYOffset + activeCollapsibleMarginSet.Resolve();
            PointF location = blockCtx->PlaceFloatedBox(
                marginBox, yOffsetForFloat, floatDirection.val, item.clear,
                adjoinsUnresolvedStrut);

            location.y += itemNonAutoMargin.top;
            location.x += itemNonAutoMargin.left;

            item.hasFinalLayout = true;
            item.finalLayout.order = item.order;
            item.finalLayout.size = itemLayout.size;
            item.finalLayout
                .scrollableOverflowRect = itemLayout.scrollableOverflowRect;
            item.finalLayout.scrollbarSize = scrollbarSize;
            item.finalLayout.location = location;
            item.finalLayout.padding = item.padding;
            item.finalLayout.border = item.border;
            item.finalLayout.margin = itemNonAutoMargin;
            OofCandRange range;
            OofAppendRange(oofPool, &range, itemLayout.oofCandidates);
            item.oofStart = range.start;
            item.oofCount = range.count;

            res.inflowOverflowRect = res.inflowOverflowRect.Union(
                ComputeScrollableOverflowContribution(
                    {IsRtl(direction) ? containerOuterWidth -
                                            (location.x + itemLayout.size.w) -
                                            resolvedBorder.right
                                      : location.x - resolvedBorder.left,
                     location.y - resolvedBorder.top},
                    itemLayout.size, itemLayout.scrollableOverflowRect,
                    item.overflow, item.contain, isScrollContainer));
            continue;
        }

        float yMarginOffset = 0.0f;
        float stretchWidth;
        PointF floatAvoidingPosition;
        float floatAvoidingWidth;
        bool itemAvoidsFloats = false;
        bool itemPushedBelowFloat = false;

        if (item.isInSameBfc) {
            stretchWidth =
                F32Max(0.0f, containerInnerWidth - itemNonAutoXMarginSum);
            floatAvoidingPosition = {0.0f, 0.0f};
            floatAvoidingWidth = 0.0f;
        } else {
            if (!isCollapsingWithFirstMarginSet ||
                !ownMarginsCollapseWithChildren.start) {
                yMarginOffset = activeCollapsibleMarginSet
                                    .CollapseWithMargin(itemNonAutoMargin.top)
                                    .Resolve();
            }
            float minY = committedYOffset + yMarginOffset;
            if (hasActiveFloats || blockCtx->HasActiveFloats(minY)) {
                float xMargins[2] = {itemNonAutoMargin.left, itemNonAutoMargin
                                                                 .right};
                float minAutoWidth = -itemNonAutoXMarginSum;
                int after = -1;
                BfcSlot slot;
                while (true) {
                    slot = blockCtx->FindBfcSlot(minY, xMargins, direction,
                                                 item.clear, after);
                    if (slot.segmentId < 0) {
                        break;
                    }
                    float width = MaybeClamp(
                        UnwrapOr(item.size.w,
                                 F32Max(F32Max(slot.stretchWidth, minAutoWidth),
                                        0.0f)),
                        item.minSize.w, item.maxSize.w);
                    if (width <= slot.borderWidth + kFitTolerance) {
                        break;
                    }
                    after = slot.segmentId;
                }
                itemPushedBelowFloat = slot.y > minY;
                hasActiveFloats = slot.segmentId >= 0;
                itemAvoidsFloats = true;
                stretchWidth =
                    F32Max(F32Max(slot.stretchWidth, minAutoWidth), 0.0f);
                floatAvoidingPosition = {slot.x, slot.y};
                floatAvoidingWidth = slot.borderWidth;
            } else {
                stretchWidth =
                    F32Max(0.0f, containerInnerWidth - itemNonAutoXMarginSum);
                floatAvoidingPosition = {resolvedContentBoxInset.left, minY};
                floatAvoidingWidth = containerInnerWidth;
            }
        }

        AlignItems justifySelf =
            ResolveSelfRelative(item.justifySelf.UnwrapOr(justifyItems),
                                item.direction, direction, true);
        bool isStretch = justifySelf.keyword == AlignItemsKeyword::Stretch;
        bool hasNonStretchJustify =
            justifySelf.keyword != AlignItemsKeyword::Normal && !isStretch;

        SizeFOpt knownDimensions = SizeFOptNone();
        if (!(item.isTable || item.isCompressibleReplaced) || isStretch) {

            Dimension widthStyle = item.sizeStyle.width;
            if (hasNonStretchJustify && widthStyle.IsAuto()) {
                widthStyle = Dimension::FitContent();
            }
            Optf keywordWidth = None();
            SizingKeywordResolution keyword =
                ResolveSizingKeyword(widthStyle, Some(stretchWidth),
                                     Some(containerInnerWidth), calc);
            if (keyword.isSome && keyword.kind == SizingKeywordKind::Exact) {
                keywordWidth = Some(keyword.exact);
            } else if (keyword.isSome) {
                keywordWidth = Some(tree->MeasureChildSize(
                    item.nodeId, SizeFOptNone(), parentSize,
                    {keyword.measure, AvailableSpace::MaxContent()},
                    SizingMode::InherentSize, AbsoluteAxis::Horizontal,
                    item.isInSameBfc ? LineBool::True() : LineBool::False()));
            }
            Optf keywordHeight = ResolveStretchHeight(
                item.sizeStyle.height, percentageResolutionHeight,
                itemNonAutoMargin, childrenMarginsAdjoinOwnEdges);
            SizeFOpt sized = item.size;
            sized.w = Some(
                MaybeClamp(UnwrapOr(Or(sized.w, keywordWidth), stretchWidth),
                           item.minSize.w, item.maxSize.w));
            sized.h = Or(sized.h, keywordHeight);
            knownDimensions = MaybeClamp(sized, item.minSize, item.maxSize);
        }

        LayoutInput inputs;
        inputs.runMode = runMode;
        inputs.sizingMode = SizingMode::InherentSize;
        inputs.axis = RequestedAxis::Both;
        inputs.knownDimensions = knownDimensions;

        inputs.knownDimensionsAreDefiniteW = true;
        inputs.knownDimensionsAreDefiniteH = true;
        inputs.parentSize = parentSize;
        inputs.availableSpace = availableSpace;
        inputs.availableSpace.width = AvailableSpace::Definite(stretchWidth);
        inputs.verticalMarginsAreCollapsible =
            item.isInSameBfc ? LineBool::True() : LineBool::False();

        Optf clearThreshold = blockCtx->ClearedThreshold(item.clear);
        float clearPos = UnwrapOr(clearThreshold, -INFINITY);

        LayoutOutput itemLayout;

        float childFloatContribution = -INFINITY;
        if (item.isInSameBfc) {

            float width = F32Max(UnwrapOr(knownDimensions.w, stretchWidth),
                                 item.paddingBorderSum.w);

            float insetLeft = itemNonAutoMargin.left + contentBoxInset.left;
            float insetRight = containerOuterWidth - width - insetLeft;
            float insets[2] = {insetLeft, insetRight};

            float estimatedY = committedYOffset +
                               activeCollapsibleMarginSet
                                   .CollapseWithMargin(itemNonAutoMargin.top)
                                   .Resolve();
            BlockContext childBlockCtx =
                blockCtx->SubContext(F32Max(estimatedY, clearPos), insets);
            itemLayout = tree->ComputeBlockChildLayout(item.nodeId, inputs,
                                                       &childBlockCtx);
            childFloatContribution = childBlockCtx
                                         .FloatedContentHeightContribution();
            bool childFlags[2];
            childBlockCtx.GetTopAdjoiningFloats(childFlags);
            blockCtx->MergeAdjoiningFloats(childFlags);
        } else {
            itemLayout = tree->ComputeChildLayout(item.nodeId, inputs);
        }
        SizeF finalSize = itemLayout.size;

        CollapsibleMarginSet topMarginSet =
            itemLayout.topMargin
                .CollapseWithMargin(UnwrapOr(itemMargin.top, 0.0f));
        CollapsibleMarginSet bottomMarginSet =
            itemLayout.bottomMargin
                .CollapseWithMargin(UnwrapOr(itemMargin.bottom, 0.0f));

        float freeXSpace = F32Max(0.0f, stretchWidth - finalSize.w);
        int autoMarginCount = (IsSome(itemMargin.left) ? 0 : 1) +
                              (IsSome(itemMargin.right) ? 0 : 1);
        float xAxisAutoMarginSize =
            autoMarginCount > 0 ? freeXSpace / (float)autoMarginCount : 0.0f;

        RectF resolvedMargin = {UnwrapOr(itemMargin.left, xAxisAutoMarginSize),
                                UnwrapOr(itemMargin.right, xAxisAutoMarginSize),
                                UnwrapOr(itemMargin.top, 0.0f),
                                UnwrapOr(itemMargin.bottom, 0.0f)};

        PointF insetOffset = {0.0f, 0.0f};
        if (item.position == Position::Relative) {
            RectFOpt inset = item.inset.MaybeResolveZip(
                {Some(containerInnerWidth), percentageResolutionHeight}, calc);
            Optf negRight = inset.right;
            if (IsSome(negRight)) {
                negRight = -negRight;
            }
            Optf negBottom = inset.bottom;
            if (IsSome(negBottom)) {
                negBottom = -negBottom;
            }
            insetOffset = {IsRtl(direction)
                               ? UnwrapOr(Or(negRight, inset.left), 0.0f)
                               : UnwrapOr(Or(inset.left, negRight), 0.0f),
                           UnwrapOr(Or(inset.top, negBottom), 0.0f)};
        }

        if (item.isInSameBfc && (!isCollapsingWithFirstMarginSet ||
                                 !ownMarginsCollapseWithChildren.start)) {
            yMarginOffset = activeCollapsibleMarginSet
                                .CollapseWithSet(topMarginSet)
                                .Resolve();
        }

        bool hasClearance = false;
        if (item.isInSameBfc && IsSome(clearThreshold)) {
            float hypotheticalY =
                committedYOffset + activeCollapsibleMarginSet
                                       .CollapseWithSet(topMarginSet)
                                       .Resolve();
            bool forcedClearance = blockCtx->HasAdjoiningFloat(item.clear);
            if (forcedClearance || hypotheticalY < clearThreshold) {
                hasClearance = true;
                float escapedMargin = isCollapsingWithFirstMarginSet &&
                                              ownMarginsCollapseWithChildren
                                                  .start
                                          ? activeCollapsibleMarginSet.Resolve()
                                          : 0.0f;
                yMarginOffset =
                    clearThreshold - committedYOffset - escapedMargin;
            }
        }

        item.computedSize = itemLayout.size;
        item.canBeCollapsedThrough =
            itemLayout.marginsCanCollapseThrough && !hasClearance;
        if (item.isInSameBfc) {
            float unclearedY = committedYOffset + activeCollapsibleMarginSet
                                                      .Resolve();
            item.staticPosition = {direction == Direction::Ltr
                                       ? resolvedContentBoxInset.left
                                       : containerOuterWidth -
                                             resolvedContentBoxInset.right -
                                             finalSize.w,
                                   F32Max(unclearedY, clearPos)};
        } else {

            item.staticPosition = {direction == Direction::Ltr
                                       ? floatAvoidingPosition.x
                                       : floatAvoidingPosition.x +
                                             floatAvoidingWidth - finalSize.w,
                                   floatAvoidingPosition.y};
        }

        PointF location;
        if (item.isInSameBfc) {
            location = {direction == Direction::Ltr
                            ? resolvedContentBoxInset.left + insetOffset.x +
                                  resolvedMargin.left
                            : containerOuterWidth -
                                  resolvedContentBoxInset.right - finalSize.w -
                                  resolvedMargin.right + insetOffset.x,
                        committedYOffset + yMarginOffset + insetOffset.y};
        } else {
            float extraLeft = itemAvoidsFloats
                                  ? resolvedMargin.left - itemNonAutoMargin.left
                                  : resolvedMargin.left;
            float extraRight = itemAvoidsFloats ? resolvedMargin.right -
                                                      itemNonAutoMargin.right
                                                : resolvedMargin.right;
            location = {
                direction == Direction::Ltr
                    ? floatAvoidingPosition.x + extraLeft + insetOffset.x
                    : floatAvoidingPosition.x + floatAvoidingWidth -
                          finalSize.w - extraRight + insetOffset.x,
                floatAvoidingPosition.y + insetOffset.y};
        }

        if (item.isInSameBfc) {
            blockCtx->AddChildFloatedContentHeightContribution(
                location.y - insetOffset.y + childFloatContribution);
        }

        float itemOuterWidth = itemLayout.size.w + resolvedMargin
                                                       .HorizontalAxisSum();
        bool hasAutoXMargin =
            !IsSome(itemMargin.left) || !IsSome(itemMargin.right);
        if (hasNonStretchJustify && !hasAutoXMargin) {

            float containerStart;
            float freeX;
            if (itemAvoidsFloats) {
                containerStart = floatAvoidingPosition.x - resolvedMargin.left;
                freeX = floatAvoidingWidth - itemLayout.size.w;
            } else {
                containerStart = resolvedContentBoxInset.left;
                freeX = containerInnerWidth - itemOuterWidth;
            }
            AlignItemsKeyword keyword = justifySelf.keyword;
            if (justifySelf.safety == AlignmentSafety::Safe && freeX < 0.0f) {
                keyword = AlignItemsKeyword::Start;
            }
            float offset;
            if (keyword == AlignItemsKeyword::Center) {
                offset = freeX / 2.0f;
            } else {
                bool alignToEnd = keyword == AlignItemsKeyword::End ||
                                  keyword == AlignItemsKeyword::FlexEnd ||
                                  keyword == AlignItemsKeyword::SelfEnd;
                offset = (alignToEnd != IsRtl(direction)) ? freeX : 0.0f;
            }
            location.x =
                containerStart + resolvedMargin.left + insetOffset.x + offset;
        } else if (!hasNonStretchJustify &&
                   itemOuterWidth < containerInnerWidth) {
            float free = containerInnerWidth - itemOuterWidth;
            switch (textAlign) {
                case TextAlign::LegacyLeft:
                    if (IsRtl(direction)) {
                        location.x -= free;
                    }
                    break;
                case TextAlign::LegacyRight:
                    if (!IsRtl(direction)) {
                        location.x += free;
                    }
                    break;
                case TextAlign::LegacyCenter:
                    location.x += IsRtl(direction) ? -free / 2.0f : free / 2.0f;
                    break;
                default:
                    break;
            }
        }

        if (!IsSome(res.firstBaseline)) {
            Optf childBaseline = itemLayout.baselines.first;
            if (IsScrollContainer(item.overflow.y)) {
                float raw = UnwrapOr(childBaseline, itemLayout.size.h);
                childBaseline =
                    Some(F32Max(0.0f, F32Min(raw, itemLayout.size.h)));
            }
            if (IsSome(childBaseline)) {
                res.firstBaseline = Some(location.y + childBaseline);
            }
        }

        item.hasFinalLayout = true;
        item.finalLayout.order = item.order;
        item.finalLayout.size = itemLayout.size;
        item.finalLayout.scrollableOverflowRect = itemLayout
                                                      .scrollableOverflowRect;
        item.finalLayout.scrollbarSize = scrollbarSize;
        item.finalLayout.location = location;
        item.finalLayout.padding = item.padding;
        item.finalLayout.border = item.border;

        if (itemAvoidsFloats) {
            if (!IsSome(itemMargin.left)) {
                resolvedMargin.left +=
                    floatAvoidingPosition.x - resolvedContentBoxInset.left;
            }
            if (!IsSome(itemMargin.right)) {
                resolvedMargin.right +=
                    containerOuterWidth - resolvedContentBoxInset.right -
                    floatAvoidingPosition.x - floatAvoidingWidth;
            }
        }
        item.finalLayout.margin = resolvedMargin;
        OofCandRange range;
        OofAppendRange(oofPool, &range, itemLayout.oofCandidates);
        item.oofStart = range.start;
        item.oofCount = range.count;

        res.inflowOverflowRect =
            res.inflowOverflowRect.Union(ComputeScrollableOverflowContribution(
                {IsRtl(direction)
                     ? containerOuterWidth - (location.x + finalSize.w) -
                           resolvedBorder.right
                     : location.x - resolvedBorder.left,
                 location.y - resolvedBorder.top},
                finalSize, itemLayout.scrollableOverflowRect, item.overflow,
                item.contain, isScrollContainer));

        if (isCollapsingWithFirstMarginSet && itemPushedBelowFloat) {
            isCollapsingWithFirstMarginSet = false;
        }
        if (isCollapsingWithFirstMarginSet && hasClearance) {
            isCollapsingWithFirstMarginSet = false;
        } else if (isCollapsingWithFirstMarginSet) {
            if (item.canBeCollapsedThrough) {
                res.firstChildTopMarginSet =
                    res.firstChildTopMarginSet.CollapseWithSet(topMarginSet)
                        .CollapseWithSet(bottomMarginSet);
            } else {
                res.firstChildTopMarginSet = res.firstChildTopMarginSet
                                                 .CollapseWithSet(topMarginSet);
                isCollapsingWithFirstMarginSet = false;
            }
        }

        if (item.canBeCollapsedThrough) {
            activeCollapsibleMarginSet = activeCollapsibleMarginSet
                                             .CollapseWithSet(topMarginSet)
                                             .CollapseWithSet(bottomMarginSet);
            yOffsetForAbsolute =
                committedYOffset + itemLayout.size.h + yMarginOffset;
        } else {
            committedYOffset = location.y - insetOffset.y + itemLayout.size.h;
            if (hasClearance && itemLayout.marginsCanCollapseThrough) {
                committedYOffset -= topMarginSet.Resolve();
                activeCollapsibleMarginSet =
                    topMarginSet.CollapseWithSet(bottomMarginSet);
                activeMarginSetHasClearance = true;
            } else {
                activeCollapsibleMarginSet = bottomMarginSet;
                activeMarginSetHasClearance = false;
            }
            yOffsetForAbsolute = committedYOffset + activeCollapsibleMarginSet
                                                        .Resolve();
            blockCtx->CommitStrut();
        }
    }

    res.lastChildBottomMarginSet = activeMarginSetHasClearance
                                       ? CollapsibleMarginSet{}
                                       : activeCollapsibleMarginSet;
    float bottomYMarginOffset = activeMarginSetHasClearance
                                    ? activeCollapsibleMarginSet.Resolve()
                                : ownMarginsCollapseWithChildren.end
                                    ? 0.0f
                                    : res.lastChildBottomMarginSet.Resolve();
    committedYOffset += resolvedContentBoxInset.bottom + bottomYMarginOffset;
    res.intrinsicOuterHeight = F32Max(0.0f, committedYOffset);
    return res;
}

LayoutOutput ComputeInner(TaffyTree* tree, NodeId nodeId,
                          const LayoutInput& inputs, BlockContext* blockCtx) {
    CalcResolver calc = tree->calc;
    SizeFOpt knownDimensionsIn = inputs.knownDimensions;
    SizeFOpt parentSize = inputs.parentSize;
    SizeAvail availableSpace = inputs.availableSpace;
    RunMode runMode = inputs.runMode;
    LineBool verticalMarginsAreCollapsible = inputs
                                                 .verticalMarginsAreCollapsible;

    const Style& style = tree->GetStyle(nodeId);
    RectLp rawPadding = style.padding;
    RectLp rawBorder = style.border;
    RectLpa rawMargin = style.margin;
    Optf aspectRatio = style.aspectRatio;
    RectF padding = rawPadding.ResolveOrZero(parentSize.w, calc);
    RectF border = rawBorder.ResolveOrZero(parentSize.w, calc);
    Direction direction = style.direction;

    PointOverflow t = style.overflow.Transpose();
    PointF offsets = {t.x == Overflow::Scroll ? style.scrollbarWidth : 0.0f,
                      t.y == Overflow::Scroll ? style.scrollbarWidth : 0.0f};
    RectF scrollbarGutter = direction == Direction::Ltr
                                ? RectF{0.0f, offsets.x, 0.0f, offsets.y}
                                : RectF{offsets.x, 0.0f, 0.0f, offsets.y};
    RectF paddingBorder = padding + border;
    SizeF paddingBorderSize = paddingBorder.SumAxes();
    RectF contentBoxInset = paddingBorder + scrollbarGutter;

    float xInsets[2] = {contentBoxInset.left, contentBoxInset.right};
    blockCtx->ApplyContentBoxInset(xInsets);

    SizeF boxSizingAdjustment = style.boxSizing == BoxSizing::ContentBox
                                    ? paddingBorderSize
                                    : SizeF::Zero();
    SizeFOpt size =
        MaybeAdd(MaybeApplyAspectRatio(
                     style.size.MaybeResolve(parentSize, calc), aspectRatio),
                 boxSizingAdjustment);
    SizeFOpt minSize =
        MaybeAdd(MaybeApplyAspectRatio(
                     style.minSize.MaybeResolve(parentSize, calc), aspectRatio),
                 boxSizingAdjustment);
    SizeFOpt maxSize =
        MaybeAdd(MaybeApplyAspectRatio(
                     style.maxSize.MaybeResolve(parentSize, calc), aspectRatio),
                 boxSizingAdjustment);

    SizeFOpt derived =
        MaybeClamp(MaybeApplyAspectRatio(knownDimensionsIn, aspectRatio),
                   minSize, maxSize);
    SizeFOpt knownDimensions = {Or(knownDimensionsIn.w, derived.w),
                                Or(knownDimensionsIn.h, derived.h)};

    SizeFOpt percentageBasisDimensions = knownDimensions;
    if (!inputs.knownDimensionsAreDefiniteH) {
        percentageBasisDimensions.h = None();
    }
    SizeFOpt containerContentBoxSize =
        MaybeSub(percentageBasisDimensions, contentBoxInset.SumAxes());

    bool isScrollContainer = IsScrollContainer(style.overflow.x) ||
                             IsScrollContainer(style.overflow.y);

    bool establishesNewBfc =
        isScrollContainer ||
        style.alignContent.keyword != AlignContentKeyword::Normal ||
        style.contain.EstablishesIndependentFormattingContext();

    LineBool childrenMarginsAdjoinOwnEdges = {
        verticalMarginsAreCollapsible.start && !establishesNewBfc &&
            !IsOutOfFlow(style.position) && padding.top == 0.0f &&
            border.top == 0.0f,
        verticalMarginsAreCollapsible.end && !establishesNewBfc &&
            !IsOutOfFlow(style.position) && padding.bottom == 0.0f &&
            border.bottom == 0.0f};
    LineBool ownMarginsCollapseWithChildren = {
        childrenMarginsAdjoinOwnEdges.start,
        childrenMarginsAdjoinOwnEdges.end && !IsSome(size.h)};
    bool hasStylesPreventingBeingCollapsedThrough =
        !style.IsBlock() || blockCtx->IsBfcRoot() || establishesNewBfc ||
        IsOutOfFlow(style.position) || padding.top > 0.0f ||
        padding.bottom > 0.0f || border.top > 0.0f || border.bottom > 0.0f;

    TextAlign textAlign = style.textAlign;
    AlignContent alignContent = style.alignContent;
    AlignItems justifyItems = style.justifyItems;

    Vec<BlockItem> items;
    GenerateItemList(tree, nodeId, containerContentBoxSize, &items);

    float containerOuterWidth;
    if (IsSome(knownDimensions.w)) {
        containerOuterWidth = knownDimensions.w;
    } else {
        AvailableSpace availableWidth =
            MaybeSub(availableSpace.width, contentBoxInset.HorizontalAxisSum());
        float intrinsicWidth =
            DetermineContentBasedContainerWidth(tree, items, availableWidth) +
            contentBoxInset.HorizontalAxisSum();
        containerOuterWidth =
            MaybeMax(MaybeClamp(intrinsicWidth, minSize.w, maxSize.w),
                     Some(paddingBorderSize.w));
    }

    if (runMode == RunMode::ComputeSize && IsSome(knownDimensions.h)) {
        return LayoutOutput::FromOuterSize(
            {containerOuterWidth, knownDimensions.h});
    }
    if (runMode == RunMode::ComputeSize &&
        inputs.axis == RequestedAxis::Horizontal) {
        return LayoutOutput::FromOuterSize({containerOuterWidth, 0.0f});
    }

    Optf ownHeightBasis = inputs.sizingMode == SizingMode::InherentSize
                              ? MaybeMax(size.h, minSize.h)
                              : None();
    Optf containerPercentageResolutionHeight =
        Or(percentageBasisDimensions.h, ownHeightBasis);

    float percentageResolutionWidth =
        UnwrapOr(parentSize.w, containerOuterWidth);
    RectF resolvedPadding =
        rawPadding.ResolveOrZero(Some(percentageResolutionWidth), calc);
    RectF resolvedBorder =
        rawBorder.ResolveOrZero(Some(percentageResolutionWidth), calc);
    RectF resolvedContentBoxInset =
        resolvedPadding + resolvedBorder + scrollbarGutter;

    Vec<OofCandidate> oofPool;
    InFlowResult inFlow = PerformFinalLayoutOnInFlowChildren(
        tree, runMode, &items, &oofPool, containerOuterWidth,
        containerPercentageResolutionHeight, contentBoxInset,
        resolvedContentBoxInset, resolvedBorder, textAlign, justifyItems,
        direction, ownMarginsCollapseWithChildren,
        childrenMarginsAdjoinOwnEdges, isScrollContainer, blockCtx);
    RectF inflowOverflowRect = inFlow.inflowOverflowRect;
    float intrinsicOuterHeight = inFlow.intrinsicOuterHeight;

    if (blockCtx->IsBfcRoot() || establishesNewBfc) {
        intrinsicOuterHeight = F32Max(
            intrinsicOuterHeight, blockCtx->FloatedContentHeightContribution());
    }

    float containerOuterHeight = MaybeMax(
        UnwrapOr(knownDimensions.h,
                 MaybeClamp(intrinsicOuterHeight, minSize.h, maxSize.h)),
        Some(paddingBorderSize.h));
    SizeF finalOuterSize = {containerOuterWidth, containerOuterHeight};

    bool heightConstrainedByMinHeight = IsSome(minSize.h) && minSize.h > 0.0f &&
                                        minSize.h >= containerOuterHeight;
    bool ownBottomMarginCollapsesWithChildren =
        ownMarginsCollapseWithChildren.end && !heightConstrainedByMinHeight;

    if (alignContent.keyword != AlignContentKeyword::Normal) {
        float containerInnerHeight =
            containerOuterHeight - resolvedContentBoxInset.VerticalAxisSum();
        float inflowContentHeight =
            intrinsicOuterHeight - resolvedContentBoxInset.VerticalAxisSum();
        float freeSpace = containerInnerHeight - inflowContentHeight;

        AlignContentKeyword keyword =
            ApplyAlignmentFallback(freeSpace, 1, alignContent);
        float groupOffset =
            ComputeAlignmentOffset(freeSpace, 1, 0.0f, keyword, false, true);
        if (IsSome(inFlow.firstBaseline)) {
            inFlow.firstBaseline += groupOffset;
        }
        bool anyInFlow = false;
        for (int i = 0; i < len(items); i++) {
            if (items[i].hasFinalLayout) {
                items[i].finalLayout.location.y += groupOffset;
                anyInFlow = true;
            }
            if (IsOutOfFlow(items[i].position)) {
                items[i].staticPosition.y += groupOffset;
            }
        }
        if (anyInFlow) {
            inflowOverflowRect = RectF::Zero();
            for (int i = 0; i < len(items); i++) {
                if (!items[i].hasFinalLayout) {
                    continue;
                }
                const Layout& l = items[i].finalLayout;
                inflowOverflowRect = inflowOverflowRect.Union(
                    ComputeScrollableOverflowContribution(
                        {IsRtl(direction)
                             ? containerOuterWidth - (l.location.x + l.size.w) -
                                   resolvedBorder.right
                             : l.location.x - resolvedBorder.left,
                         l.location.y - resolvedBorder.top},
                        l.size, l.scrollableOverflowRect, items[i].overflow,
                        items[i].contain, isScrollContainer));
            }
        }
    }

    bool allInFlowChildrenCanBeCollapsedThrough = true;
    for (int i = 0; i < len(items); i++) {
        if (IsFloated(items[i].floatMode)) {
            continue;
        }
        if (!IsOutOfFlow(items[i].position) && !items[i]
                                                    .canBeCollapsedThrough) {
            allInFlowChildrenCanBeCollapsedThrough = false;
            break;
        }
    }

    bool canBeCollapsedThrough = !hasStylesPreventingBeingCollapsedThrough &&
                                 containerOuterHeight == 0.0f &&
                                 allInFlowChildrenCanBeCollapsedThrough;

    LayoutOutput output;
    output.size = finalOuterSize;
    output.baselines = BaselinesFromFirst(inFlow.firstBaseline);
    output.topMargin =
        ownMarginsCollapseWithChildren.start
            ? inFlow.firstChildTopMarginSet
            : CollapsibleMarginSet::FromMargin(UnwrapOr(
                  rawMargin.MaybeResolve(parentSize.w, calc).top, 0.0f));
    output.bottomMargin =
        ownBottomMarginCollapsesWithChildren
            ? inFlow.lastChildBottomMarginSet
            : CollapsibleMarginSet::FromMargin(UnwrapOr(
                  rawMargin.MaybeResolve(parentSize.w, calc).bottom, 0.0f));
    output.marginsCanCollapseThrough = canBeCollapsedThrough;

    if (runMode == RunMode::ComputeSize) {
        return output;
    }

    for (int i = 0; i < len(items); i++) {
        if (items[i].hasFinalLayout) {
            tree->SetUnroundedLayout(items[i].nodeId, items[i].finalLayout);
        }
    }

    OofCandidates candidates;
    for (int i = 0; i < len(items); i++) {
        BlockItem& item = items[i];
        if (IsOutOfFlow(item.position)) {
            OofCandidate candidate;
            candidate.node = item.nodeId;
            candidate.order = item.order;
            candidate.position = item.position;

            float inlineStart = resolvedContentBoxInset.left;
            float inlineEnd = containerOuterWidth - resolvedContentBoxInset
                                                        .right;
            AlignItems itemJustify =
                ResolveSelfRelative(item.justifySelf.UnwrapOr(justifyItems),
                                    item.direction, direction, true);
            AlignItems itemAlign = ResolveSelfRelative(
                item.alignSelf.UnwrapOr(AlignItems{AlignItemsKeyword::Normal}),
                item.direction, direction, false);
            candidate.staticX = AxisStaticPosition::FromAlignment(
                itemJustify, inlineStart, inlineEnd, IsRtl(direction));
            candidate.staticY = AxisStaticPosition::FromAlignment(
                itemAlign, item.staticPosition.y, item.staticPosition.y, false);
            candidates.Push(candidate);
        } else if (item.oofCount > 0) {
            OofCandRange range = {item.oofStart, item.oofCount};
            if (item.hasFinalLayout) {
                OofTranslateRange(&oofPool, range, item.finalLayout.location);
            }
            OofAppendPoolRange(&candidates, oofPool, range);
        }
    }
    RectF absolutePositionInset = resolvedBorder + scrollbarGutter;
    output.oofCandidates = candidates;
    output.hasOofPositioningArea = true;
    output.oofPositioningArea.size = finalOuterSize - absolutePositionInset
                                                          .SumAxes();
    output.oofPositioningArea
        .offset = {absolutePositionInset.left, absolutePositionInset.top};

    if (isScrollContainer) {
        inflowOverflowRect.right +=
            IsRtl(direction) ? resolvedPadding.left : resolvedPadding.right;
        inflowOverflowRect.bottom += resolvedPadding.bottom;
    }
    output.scrollableOverflowRect = inflowOverflowRect;

    int len = tree->ChildCount(nodeId);
    for (int order = 0; order < len; order++) {
        NodeId child = tree->GetChildId(nodeId, order);
        if (tree->GetStyle(child).BoxGenMode() == BoxGenerationMode::None) {
            tree->SetUnroundedLayout(child, Layout::WithOrder((uint32_t)order));
            tree->PerformChildLayout(
                child, SizeFOptNone(), SizeFOptNone(), SizeAvail::MaxContent(),
                SizingMode::InherentSize, LineBool::False());
        }
    }

    return output;
}

}

LayoutOutput ComputeBlockLayout(TaffyTree* tree, NodeId nodeId,
                                const LayoutInput& inputs,
                                BlockContext* blockCtx) {
    CalcResolver calc = tree->calc;
    SizeFOpt knownDimensions = inputs.knownDimensions;
    SizeFOpt parentSize = inputs.parentSize;
    RunMode runMode = inputs.runMode;
    const Style& style = tree->GetStyle(nodeId);

    bool isScrollContainer = IsScrollContainer(style.overflow.x) ||
                             IsScrollContainer(style.overflow.y);

    Contain contain = style.contain;
    bool establishesNewBfc = isScrollContainer ||
                             style.alignContent
                                     .keyword != AlignContentKeyword::Normal ||
                             contain.EstablishesIndependentFormattingContext();
    Optf aspectRatio = style.aspectRatio;
    RectF padding = style.padding.ResolveOrZero(parentSize.w, calc);
    RectF border = style.border.ResolveOrZero(parentSize.w, calc);
    SizeF paddingBorderSize = (padding + border).SumAxes();
    SizeF boxSizingAdjustment = style.boxSizing == BoxSizing::ContentBox
                                    ? paddingBorderSize
                                    : SizeF::Zero();

    SizeFOpt minSize =
        MaybeAdd(MaybeApplyAspectRatio(
                     style.minSize.MaybeResolve(parentSize, calc), aspectRatio),
                 boxSizingAdjustment);
    SizeFOpt maxSize =
        MaybeAdd(MaybeApplyAspectRatio(
                     style.maxSize.MaybeResolve(parentSize, calc), aspectRatio),
                 boxSizingAdjustment);
    SizeFOpt clampedStyleSize = SizeFOptNone();
    if (inputs.sizingMode == SizingMode::InherentSize) {
        clampedStyleSize = MaybeClamp(
            MaybeAdd(
                MaybeApplyAspectRatio(style.size.MaybeResolve(parentSize, calc),
                                      aspectRatio),
                boxSizingAdjustment),
            minSize, maxSize);
    }

    SizeFOpt minMaxDefiniteSize = SizeFOptNone();
    if (IsSome(minSize.w) && IsSome(maxSize.w) && maxSize.w <= minSize.w) {
        minMaxDefiniteSize.w = minSize.w;
    }
    if (IsSome(minSize.h) && IsSome(maxSize.h) && maxSize.h <= minSize.h) {
        minMaxDefiniteSize.h = minSize.h;
    }

    SizeFOpt styledBasedKnownDimensions =
        MaybeMax(Or(Or(knownDimensions, minMaxDefiniteSize), clampedStyleSize),
                 paddingBorderSize);

    if (runMode == RunMode::ComputeSize) {
        if (BothAxisDefined(styledBasedKnownDimensions)) {
            return LayoutOutput::FromOuterSize(
                {styledBasedKnownDimensions.w, styledBasedKnownDimensions.h});
        }
        if (inputs.axis == RequestedAxis::Horizontal &&
            IsSome(styledBasedKnownDimensions.w)) {
            return LayoutOutput::FromOuterSize(
                {styledBasedKnownDimensions.w, 0.0f});
        }
    }

    LayoutInput next = inputs;
    next.knownDimensions = styledBasedKnownDimensions;

    LayoutOutput out;
    if (blockCtx && !establishesNewBfc) {
        out = ComputeInner(tree, nodeId, next, blockCtx);
    } else {
        BlockFormattingContext rootBfc;
        BlockContext rootCtx;
        rootCtx.bfc = &rootBfc;
        rootCtx.isRoot = true;
        out = ComputeInner(tree, nodeId, next, &rootCtx);
        VecReset(rootBfc.floatContext.leftFloats);
        VecReset(rootBfc.floatContext.rightFloats);
        VecReset(rootBfc.floatContext.segments);
    }

    if (contain.SuppressesBaseline()) {
        out.baselines = BaselinesFromFirst(None());
    }
    return out;
}

}

#line 1 "src/taffy/compute_flexbox.cpp"

namespace taffy {
namespace {

struct RectBool {
    bool left = false;
    bool right = false;
    bool top = false;
    bool bottom = false;

    bool MainStart(FlexDirection d) const { return IsRow(d) ? left : top; }
    bool MainEnd(FlexDirection d) const { return IsRow(d) ? right : bottom; }
    bool CrossStart(FlexDirection d) const { return IsRow(d) ? top : left; }
    bool CrossEnd(FlexDirection d) const { return IsRow(d) ? bottom : right; }
};

struct FlexItem {
    NodeId node;

    uint32_t order = 0;

    SizeFOpt size = SizeFOptNone();

    SizeDim sizeStyle;
    SizeFOpt minSize = SizeFOptNone();
    SizeFOpt maxSize = SizeFOptNone();
    Optf aspectRatio = None();
    AlignSelf alignSelf;

    PointOverflow overflow;
    Contain contain;
    float scrollbarWidth = 0.0f;
    float flexShrink = 0.0f;
    float flexGrow = 0.0f;

    bool flexBasisIsDefinite = false;

    float resolvedMinimumMainSize = 0.0f;

    SizeF relativeInset = SizeF::Zero();
    RectF margin;
    RectBool marginIsAuto;
    RectF padding;
    RectF border;

    float flexBasis = 0.0f;
    float innerFlexBasis = 0.0f;

    float violation = 0.0f;
    bool frozen = false;

    float contentFlexFraction = 0.0f;

    SizeF hypotheticalInnerSize;
    SizeF hypotheticalOuterSize;
    SizeF targetSize;
    SizeF outerTargetSize;

    float baseline = 0.0f;

    int oofStart = 0;
    int oofCount = 0;

    float offsetMain = 0.0f;
    float offsetCross = 0.0f;

    bool IsScroll() const {
        return IsScrollContainer(overflow.x) || IsScrollContainer(overflow.y);
    }

    bool ParticipatesInBaselineAlignment(FlexDirection dir) const {
        return alignSelf.keyword == AlignItemsKeyword::Baseline &&
               !marginIsAuto.CrossStart(dir) && !marginIsAuto.CrossEnd(dir);
    }
};

struct FlexLine {
    FlexItem* items = nullptr;
    int count = 0;
    float crossSize = 0.0f;
    float offsetCross = 0.0f;
};

struct AlgoConstants {
    FlexDirection dir = FlexDirection::Row;
    Direction layoutDirection = Direction::Ltr;
    bool isRow = true;
    bool isColumn = false;
    bool isWrap = false;
    bool isWrapReverse = false;
    bool isBalance = false;

    int lineCount = 0;

    float DividedCrossSpace(float crossAvailable) const {
        if (lineCount > 1) {
            float n = (float)lineCount;
            return (crossAvailable - (n - 1.0f) * Cross(gap, dir)) / n;
        }
        return crossAvailable;
    }

    SizeFOpt minSize = SizeFOptNone();
    SizeFOpt maxSize = SizeFOptNone();
    RectF border;

    RectF contentBoxInset;
    PointF scrollbarGutter;

    bool isScrollContainer = false;
    SizeF gap;
    AlignItems alignItems;
    AlignContent alignContent;
    AlignContent justifyContent;

    SizeFOpt nodeOuterSize = SizeFOptNone();
    SizeFOpt nodeInnerSize = SizeFOptNone();

    bool knownMainSizeIsDefinite = true;
    bool hasDefiniteMainSize = false;
    bool hasDefiniteCrossSize = false;

    bool crossAxisAvailableSpaceIsDefinite = false;

    SizeF containerSize;
    SizeF innerContainerSize;
};

float SumAxisGaps(float gap, int numItems) {

    if (numItems <= 1) {
        return 0.0f;
    }
    return gap * (float)(numItems - 1);
}

Optf Filter(Optf v, bool keep) {
    return keep ? v : None();
}

AlignItems ResolveNormalAlignment(AlignItems alignment) {
    if (alignment.keyword == AlignItemsKeyword::Normal) {
        return AlignItems{AlignItemsKeyword::Stretch};
    }
    return alignment;
}

AlgoConstants ComputeConstants(TaffyTree* tree, const Style& style,
                               SizeFOpt knownDimensions,
                               bool knownDimensionsAreDefiniteW,
                               bool knownDimensionsAreDefiniteH,
                               SizeFOpt parentSize, SizeAvail availableSpace) {
    CalcResolver calc = tree->calc;
    AlgoConstants c;
    c.dir = style.flexDirection;
    c.isRow = IsRow(c.dir);
    c.isColumn = IsColumn(c.dir);
    c.isWrap = FlexWrapIsMultiLine(style.flexWrap);
    c.isWrapReverse = FlexWrapIsReverse(style.flexWrap);
    c.isBalance = FlexWrapIsBalance(style.flexWrap);
    c.lineCount = 0;
    if (c.isWrap) {
        c.lineCount = style.flexLineCount < 1 ? 1 : (int)style.flexLineCount;
    }

    Optf aspectRatio = style.aspectRatio;
    RectF padding = style.padding.ResolveOrZero(parentSize.w, calc);
    c.border = style.border.ResolveOrZero(parentSize.w, calc);
    SizeF paddingBorderSum = padding.SumAxes() + c.border.SumAxes();
    SizeF boxSizingAdjustment = style.boxSizing == BoxSizing::ContentBox
                                    ? paddingBorderSum
                                    : SizeF::Zero();

    c.alignItems = ResolveNormalAlignment(style.alignItems);

    c.alignContent = style.alignContent.keyword == AlignContentKeyword::Normal
                         ? AlignContent{AlignContentKeyword::Stretch}
                         : style.alignContent;
    c.justifyContent = style.justifyContent
                                   .keyword == AlignContentKeyword::Normal
                           ? AlignContent{AlignContentKeyword::FlexStart}
                           : style.justifyContent;
    c.layoutDirection = style.direction;

    PointOverflow t = style.overflow.Transpose();
    c.isScrollContainer = IsScrollContainer(style.overflow.x) ||
                          IsScrollContainer(style.overflow.y);
    c.scrollbarGutter = {t.x == Overflow::Scroll ? style.scrollbarWidth : 0.0f,
                         t.y == Overflow::Scroll ? style.scrollbarWidth : 0.0f};
    c.contentBoxInset = padding + c.border;
    c.contentBoxInset.bottom += c.scrollbarGutter.y;
    if (c.layoutDirection == Direction::Ltr) {
        c.contentBoxInset.right += c.scrollbarGutter.x;
    } else {
        c.contentBoxInset.left += c.scrollbarGutter.x;
    }

    c.nodeOuterSize = knownDimensions;
    c.nodeInnerSize = MaybeSub(c.nodeOuterSize, c.contentBoxInset.SumAxes());
    bool knownMainDefinite =
        c.isRow ? knownDimensionsAreDefiniteW : knownDimensionsAreDefiniteH;
    bool knownCrossDefinite =
        c.isRow ? knownDimensionsAreDefiniteH : knownDimensionsAreDefiniteW;
    c.knownMainSizeIsDefinite = knownMainDefinite;
    c.hasDefiniteMainSize =
        knownMainDefinite && IsSome(Main(knownDimensions, c.dir));
    c.hasDefiniteCrossSize =
        knownCrossDefinite && IsSome(Cross(knownDimensions, c.dir));
    c.crossAxisAvailableSpaceIsDefinite =
        c.hasDefiniteCrossSize ||
        availableSpace.Cross(c.dir).kind == AvailableSpace::Kind::Definite;
    c.gap = style.gap.ResolveOrZero(Or(c.nodeInnerSize, SizeFOpt{0, 0}), calc);

    c.minSize =
        MaybeAdd(MaybeApplyAspectRatio(
                     style.minSize.MaybeResolve(parentSize, calc), aspectRatio),
                 boxSizingAdjustment);
    c.maxSize =
        MaybeAdd(MaybeApplyAspectRatio(
                     style.maxSize.MaybeResolve(parentSize, calc), aspectRatio),
                 boxSizingAdjustment);
    return c;
}

static SizeF ResolveRelativeInset(RectFOpt inset, const AlgoConstants& c) {
    FlexDirection direction = c.dir;
    bool isRtlRow = IsRow(direction) && IsRtl(c.layoutDirection);
    bool isRtlColumn = IsColumn(direction) && IsRtl(c.layoutDirection);

    Optf negMainStart = MainStart(inset, direction);
    if (IsSome(negMainStart)) {
        negMainStart = -negMainStart;
    }
    Optf negMainEnd = MainEnd(inset, direction);
    if (IsSome(negMainEnd)) {
        negMainEnd = -negMainEnd;
    }
    float main =
        isRtlRow ? UnwrapOr(Or(MainEnd(inset, direction), negMainStart), 0.0f)
                 : UnwrapOr(Or(MainStart(inset, direction), negMainEnd), 0.0f);

    Optf negCrossEnd = CrossEnd(inset, direction);
    if (IsSome(negCrossEnd)) {
        negCrossEnd = -negCrossEnd;
    }
    float cross =
        isRtlColumn
            ? UnwrapOr(Or(negCrossEnd, CrossStart(inset, direction)), 0.0f)
            : UnwrapOr(Or(CrossStart(inset, direction), negCrossEnd), 0.0f);

    SizeF out = SizeF::Zero();
    SetMain(&out, direction, main);
    SetCross(&out, direction, cross);
    return out;
}

void GenerateAnonymousFlexItems(TaffyTree* tree, NodeId node,
                                const AlgoConstants& c,
                                Vec<FlexItem>* flexItems) {
    CalcResolver calc = tree->calc;
    int n = tree->ChildCount(node);

    if (n > 0) {
        VecReserve(*flexItems, n);
    }
    for (int index = 0; index < n; index++) {
        NodeId child = tree->GetChildId(node, index);
        const Style& cs = tree->GetStyle(child);
        if (IsOutOfFlow(cs.position) ||
            cs.BoxGenMode() == BoxGenerationMode::None) {
            continue;
        }

        Optf aspectRatio = cs.aspectRatio;
        RectF padding = cs.padding.ResolveOrZero(c.nodeInnerSize.w, calc);
        RectF border = cs.border.ResolveOrZero(c.nodeInnerSize.w, calc);
        SizeF pbSum = (padding + border).SumAxes();
        SizeF boxSizingAdjustment =
            cs.boxSizing == BoxSizing::ContentBox ? pbSum : SizeF::Zero();

        SizeFOpt percentResolutionSize = c.nodeInnerSize;
        if (!c.knownMainSizeIsDefinite) {
            SetMain(&percentResolutionSize, c.dir, None());
        }

        FlexItem item;
        item.node = child;
        item.order = (uint32_t)index;
        item.sizeStyle = cs.size;
        item.size = MaybeAdd(
            MaybeApplyAspectRatio(
                cs.size.MaybeResolve(percentResolutionSize, calc), aspectRatio),
            boxSizingAdjustment);
        item.minSize =
            MaybeAdd(cs.minSize.MaybeResolve(percentResolutionSize, calc),
                     boxSizingAdjustment);
        item.maxSize =
            MaybeAdd(cs.maxSize.MaybeResolve(percentResolutionSize, calc),
                     boxSizingAdjustment);
        item.aspectRatio = aspectRatio;

        if (cs.position == Position::Relative) {
            RectFOpt inset = cs.inset.MaybeResolveZip(c.nodeInnerSize, calc);
            item.relativeInset = ResolveRelativeInset(inset, c);
        }
        item.margin = cs.margin.ResolveOrZero(c.nodeInnerSize.w, calc);
        item.marginIsAuto = {cs.margin.left.IsAuto(), cs.margin.right.IsAuto(),
                             cs.margin.top.IsAuto(), cs.margin.bottom.IsAuto()};
        item.padding = padding;
        item.border = border;
        item.alignSelf = ResolveSelfRelative(
            ResolveNormalAlignment(cs.alignSelf.UnwrapOr(c.alignItems)),
            cs.direction, c.layoutDirection, c.isColumn);
        item.overflow = cs.overflow;
        item.contain = cs.contain;
        item.scrollbarWidth = cs.scrollbarWidth;
        item.flexGrow = cs.flexGrow;
        item.flexShrink = cs.flexShrink;
        VecAppend(*flexItems, item);
    }
}

SizeAvail DetermineAvailableSpace(SizeFOpt knownDimensions,
                                  SizeAvail outerAvailableSpace,
                                  const AlgoConstants& c) {

    SizeAvail out;
    if (IsSome(knownDimensions.w)) {
        out.width = AvailableSpace::Definite(F32Max(
            0.0f, knownDimensions.w - c.contentBoxInset.HorizontalAxisSum()));
    } else {
        out.width = MaybeMax(MaybeSub(outerAvailableSpace.width,
                                      c.contentBoxInset.HorizontalAxisSum()),
                             0.0f);
    }
    if (IsSome(knownDimensions.h)) {
        out.height = AvailableSpace::Definite(F32Max(
            0.0f, knownDimensions.h - c.contentBoxInset.VerticalAxisSum()));
    } else {
        out.height = MaybeMax(MaybeSub(outerAvailableSpace.height,
                                       c.contentBoxInset.VerticalAxisSum()),
                              0.0f);
    }
    return out;
}

void DetermineFlexBaseSize(TaffyTree* tree, const AlgoConstants& c,
                           SizeAvail availableSpace, FlexItem* items,
                           int count) {
    CalcResolver calc = tree->calc;
    FlexDirection dir = c.dir;

    for (int i = 0; i < count; i++) {
        FlexItem& child = items[i];
        const Style& cs = tree->GetStyle(child.node);

        Optf crossAxisParentSize = Cross(c.nodeInnerSize, dir);
        SizeFOpt childParentSize = SizeFOptFromCross(dir, crossAxisParentSize);

        SizeFOpt transferredMinSize =
            MaybeApplyAspectRatio(child.minSize, child.aspectRatio);
        SizeFOpt transferredMaxSize =
            MaybeApplyAspectRatio(child.maxSize, child.aspectRatio);
        Optf childMinCross = Cross(transferredMinSize, dir);
        Optf childMaxCross = Cross(transferredMaxSize, dir);

        AvailableSpace crossAxisAvailableSpace;
        AvailableSpace crossIn = availableSpace.Cross(dir);
        switch (crossIn.kind) {
            case AvailableSpace::Kind::Definite:
                crossAxisAvailableSpace = AvailableSpace::Definite(MaybeClamp(
                    F32Max(0.0f, c.DividedCrossSpace(UnwrapOr(
                                     crossAxisParentSize, crossIn.value)) -
                                     CrossAxisSum(child.margin, dir)),
                    childMinCross, childMaxCross));
                break;
            case AvailableSpace::Kind::MinContent:
                crossAxisAvailableSpace =
                    IsSome(childMinCross)
                        ? AvailableSpace::Definite(childMinCross)
                        : AvailableSpace::MinContent();
                break;
            default:
                crossAxisAvailableSpace =
                    IsSome(childMaxCross)
                        ? AvailableSpace::Definite(childMaxCross)
                        : AvailableSpace::MaxContent();
                break;
        }

        bool childCrossSizeIsDefinite = IsSome(Cross(child.size, dir));
        SizeFOpt childKnownDimensions = child.size;
        SetMain(&childKnownDimensions, dir, None());
        SetCross(&childKnownDimensions, dir,
                 MaybeClamp(Cross(childKnownDimensions, dir),
                            Cross(transferredMinSize, dir),
                            Cross(transferredMaxSize, dir)));
        if (child.alignSelf.keyword == AlignItemsKeyword::Stretch &&
            !child.marginIsAuto.CrossStart(dir) &&
            !child.marginIsAuto.CrossEnd(dir) &&
            !IsSome(Cross(childKnownDimensions, dir))) {
            SetCross(&childKnownDimensions, dir,
                     crossAxisAvailableSpace.IntoOption());

            childCrossSizeIsDefinite = !c.isWrap && c.hasDefiniteCrossSize &&
                                       IsSome(crossAxisParentSize);
        }

        Optf containerWidth = c.nodeInnerSize.w;

        Optf percentResolutionMainSize =
            c.knownMainSizeIsDefinite ? Main(c.nodeInnerSize, dir) : None();
        float boxSizingAdjustment = 0.0f;
        if (cs.boxSizing == BoxSizing::ContentBox) {
            RectF padding = cs.padding.ResolveOrZero(containerWidth, calc);
            RectF border = cs.border.ResolveOrZero(containerWidth, calc);
            boxSizingAdjustment = Main((padding + border).SumAxes(), dir);
        }
        Dimension flexBasisStyle = cs.flexBasis;
        Optf flexBasis = MaybeAdd(
            flexBasisStyle.MaybeResolve(percentResolutionMainSize, calc),
            boxSizingAdjustment);

        Optf mainSize = Main(child.size, dir);
        Optf mainStretch =
            MaybeMax(MaybeSub(percentResolutionMainSize,
                              Some(MainAxisSum(child.margin, dir))),
                     0.0f);

        bool measureBasis = true;
        AvailableSpace keywordMain = AvailableSpace::MaxContent();
        bool hasKeywordMain = false;
        if (flexBasisStyle.IsContent()) {

        } else if (flexBasisStyle.IsSizingKeyword()) {
            SizingKeywordResolution keyword = ResolveSizingKeyword(
                flexBasisStyle, mainStretch, percentResolutionMainSize, calc);
            if (keyword.isSome && keyword.kind == SizingKeywordKind::Exact) {
                child.flexBasisIsDefinite = true;
                child.flexBasis = keyword.exact;
                measureBasis = false;
            } else if (keyword.isSome) {
                keywordMain = keyword.measure;
                hasKeywordMain = true;
            }
        } else if (IsSome(flexBasis) ||
                   (flexBasisStyle.IsAuto() && IsSome(mainSize))) {
            child.flexBasisIsDefinite = true;
            child.flexBasis = IsSome(flexBasis) ? flexBasis : mainSize;
            measureBasis = false;
        } else if (flexBasisStyle.IsAuto()) {
            SizingKeywordResolution keyword =
                ResolveSizingKeyword(child.sizeStyle.Main(dir), mainStretch,
                                     percentResolutionMainSize, calc);
            if (keyword.isSome && keyword.kind == SizingKeywordKind::Exact) {
                child.flexBasisIsDefinite = true;
                child.flexBasis = keyword.exact;
                measureBasis = false;
            } else if (keyword.isSome) {
                keywordMain = keyword.measure;
                hasKeywordMain = true;
            }
        }
        if (measureBasis && childCrossSizeIsDefinite) {

            Optf cross = Cross(childKnownDimensions, dir);
            if (IsSome(child.aspectRatio) && IsSome(cross)) {
                child.flexBasisIsDefinite = true;
                child.flexBasis = c.isRow ? cross * child.aspectRatio
                                          : cross / child.aspectRatio;
                measureBasis = false;
            }
        }
        if (measureBasis) {

            AvailableSpace mainAvailable =
                availableSpace.Main(dir)
                            .kind == AvailableSpace::Kind::MinContent
                    ? AvailableSpace::MinContent()
                    : AvailableSpace::MaxContent();
            if (hasKeywordMain) {
                mainAvailable = keywordMain;
            }
            SizeAvail childAvailableSpace = SizeAvail::MaxContent();
            childAvailableSpace.SetMain(dir, mainAvailable);
            childAvailableSpace.SetCross(dir, crossAxisAvailableSpace);
            child.flexBasis = tree->MeasureChildSize(
                child.node, childKnownDimensions, childParentSize,
                childAvailableSpace, SizingMode::ContentSize, MainAxis(dir),
                LineBool::False());
        }

        float paddingBorderSum =
            MainAxisSum(child.padding, dir) + MainAxisSum(child.border, dir);
        child.flexBasis = F32Max(child.flexBasis, paddingBorderSum);

        child.innerFlexBasis = child.flexBasis -
                               MainAxisSum(child.padding, dir) -
                               MainAxisSum(child.border, dir);

        SizeFOpt paddingBorderAxesSums =
            AsOptional((child.padding + child.border).SumAxes());

        SizeFOpt automaticMin = {MaybeIntoAutomaticMinSize(child.overflow.x),
                                 MaybeIntoAutomaticMinSize(child.overflow.y)};
        Optf styleMinMainSize = Main(Or(child.minSize, automaticMin), dir);

        if (IsSome(styleMinMainSize)) {
            child.resolvedMinimumMainSize = styleMinMainSize;
        } else {
            SizeAvail childAvailableSpace = SizeAvail::MinContent();
            childAvailableSpace.SetCross(dir, crossAxisAvailableSpace);
            float minContentMainSize = tree->MeasureChildSize(
                child.node, childKnownDimensions, childParentSize,
                childAvailableSpace, SizingMode::ContentSize, MainAxis(dir),
                LineBool::False());

            float clamped =
                MaybeMin(MaybeMin(minContentMainSize, Main(child.size, dir)),
                         Main(transferredMaxSize, dir));
            child.resolvedMinimumMainSize =
                MaybeMax(clamped, Main(paddingBorderAxesSums, dir));
        }

        float hypotheticalInnerMinMain =
            MaybeMax(MaybeMax(child.resolvedMinimumMainSize,
                              Main(transferredMinSize, dir)),
                     Main(paddingBorderAxesSums, dir));
        float hypotheticalInnerSize =
            MaybeClamp(child.flexBasis, Some(hypotheticalInnerMinMain),
                       Main(transferredMaxSize, dir));
        float hypotheticalOuterSize =
            hypotheticalInnerSize + MainAxisSum(child.margin, dir);

        SetMain(&child.hypotheticalInnerSize, dir, hypotheticalInnerSize);
        SetMain(&child.hypotheticalOuterSize, dir, hypotheticalOuterSize);
    }
}

void CollectFlexLines(const AlgoConstants& c, SizeAvail availableSpace,
                      Vec<FlexItem>* flexItems, Vec<FlexLine>* lines) {
    FlexItem* items = flexItems->els;
    int total = flexItems->len;

    if (!c.isWrap || !c.knownMainSizeIsDefinite) {
        VecAppend(*lines, {items, total, 0.0f, 0.0f});
        return;
    }

    AvailableSpace mainAxisAvailableSpace;
    Optf maxMain = Main(c.maxSize, c.dir);
    if (IsSome(maxMain)) {
        float available =
            UnwrapOr(availableSpace.Main(c.dir).IntoOption(), maxMain);

        if (!c.hasDefiniteMainSize) {
            available = F32Min(available, maxMain);
        }
        mainAxisAvailableSpace = AvailableSpace::Definite(
            MaybeMax(available, Main(c.minSize, c.dir)));
    } else if (!c.isRow && !c.hasDefiniteMainSize &&
               availableSpace.Main(c.dir)
                       .kind == AvailableSpace::Kind::Definite) {

        mainAxisAvailableSpace = AvailableSpace::MaxContent();
    } else {
        mainAxisAvailableSpace = availableSpace.Main(c.dir);
    }

    switch (mainAxisAvailableSpace.kind) {
        case AvailableSpace::Kind::MaxContent:

            VecAppend(*lines, {items, total, 0.0f, 0.0f});
            return;
        case AvailableSpace::Kind::MinContent:

            for (int i = 0; i < total; i++) {
                VecAppend(*lines, {items + i, 1, 0.0f, 0.0f});
            }
            return;
        default:
            break;
    }

    float limit = mainAxisAvailableSpace.value;
    float mainAxisGap = Main(c.gap, c.dir);
    int start = 0;
    while (start < total) {
        float lineLength = 0.0f;
        int index = total;
        for (int idx = start; idx < total; idx++) {

            float gapContribution = idx == start ? 0.0f : mainAxisGap;
            lineLength +=
                Main(items[idx].hypotheticalOuterSize, c.dir) + gapContribution;
            if (lineLength > limit && idx != start) {
                index = idx;
                break;
            }
        }
        VecAppend(*lines, {items + start, index - start, 0.0f, 0.0f});
        start = index;
    }
}

void CollectBalancedFlexLines(const AlgoConstants& c, SizeAvail availableSpace,
                              Vec<FlexItem>* flexItems, Vec<FlexLine>* lines) {
    FlexItem* items = flexItems->els;
    int total = flexItems->len;
    if (total == 0) {
        return;
    }

    AvailableSpace mainAxisAvailableSpace;
    if (!c.knownMainSizeIsDefinite) {
        mainAxisAvailableSpace = AvailableSpace::MaxContent();
    } else {
        Optf maxMain = Main(c.maxSize, c.dir);
        if (IsSome(maxMain)) {
            float available =
                UnwrapOr(availableSpace.Main(c.dir).IntoOption(), maxMain);
            if (!c.hasDefiniteMainSize) {
                available = F32Min(available, maxMain);
            }
            mainAxisAvailableSpace = AvailableSpace::Definite(
                MaybeMax(available, Main(c.minSize, c.dir)));
        } else if (!c.isRow && !c.hasDefiniteMainSize &&
                   availableSpace.Main(c.dir)
                           .kind == AvailableSpace::Kind::Definite) {
            mainAxisAvailableSpace = AvailableSpace::MaxContent();
        } else {
            mainAxisAvailableSpace = availableSpace.Main(c.dir);
        }
    }

    if (mainAxisAvailableSpace.kind == AvailableSpace::Kind::MinContent) {
        for (int i = 0; i < total; i++) {
            VecAppend(*lines, {items + i, 1, 0.0f, 0.0f});
        }
        return;
    }

    float lineBreak = mainAxisAvailableSpace
                                  .kind == AvailableSpace::Kind::Definite
                          ? mainAxisAvailableSpace.value
                          : INFINITY;
    Vec<float> itemSizes;
    for (int i = 0; i < total; i++) {
        VecAppend(itemSizes, Main(items[i].hypotheticalOuterSize, c.dir));
    }
    Vec<int> counts;
    int* slots = VecAppendBlanks(counts, total);
    int minLines = c.lineCount < 1 ? 1 : c.lineCount;
    int nLines = BalancedLineItemCounts(itemSizes.els, total, lineBreak,
                                        Main(c.gap, c.dir), minLines, slots);
    int start = 0;
    for (int i = 0; i < nLines; i++) {
        int count = slots[i];
        VecAppend(*lines, {items + start, count, 0.0f, 0.0f});
        start += count;
    }
}

float LineTotalTargetSize(const FlexLine& line, const AlgoConstants& c) {
    float total = 0.0f;
    for (int i = 0; i < line.count; i++) {
        const FlexItem& child = line.items[i];
        float paddingBorderSum =
            MainAxisSum(child.padding + child.border, c.dir);
        total += F32Max(MaybeMax(child.flexBasis, Main(child.minSize, c.dir)) +
                            MainAxisSum(child.margin, c.dir),
                        paddingBorderSum);
    }
    return total;
}

float LongestLineLength(Vec<FlexLine>* lines, const AlgoConstants& c) {
    float longest = 0.0f;
    for (int i = 0; i < lines->len; i++) {
        FlexLine& line = (*lines)[i];
        float lineGap = SumAxisGaps(Main(c.gap, c.dir), line.count);
        float total = LineTotalTargetSize(line, c) + lineGap;
        if (i == 0 || total > longest) {
            longest = total;
        }
    }
    return longest;
}

void DetermineContainerMainSize(TaffyTree* tree, SizeAvail availableSpace,
                                Vec<FlexLine>* lines, AlgoConstants* c) {
    FlexDirection dir = c->dir;
    float mainContentBoxInset = MainAxisSum(c->contentBoxInset, dir);

    float outerMainSize;
    Optf known = Main(c->nodeOuterSize, dir);
    if (IsSome(known)) {
        outerMainSize = known;
    } else {
        AvailableSpace mainAvail = availableSpace.Main(dir);
        if (mainAvail.kind == AvailableSpace::Kind::Definite) {
            float longest = LongestLineLength(lines, *c);
            float size = longest + mainContentBoxInset;
            if (c->isBalance) {

                int itemCount = 0;
                for (int i = 0; i < lines->len; i++) {
                    itemCount += (*lines)[i].count;
                }
                if (itemCount == 0) {
                    outerMainSize = size;
                } else {
                    Vec<float> itemLengths;
                    for (int li = 0; li < lines->len; li++) {
                        FlexLine& line = (*lines)[li];
                        for (int ii = 0; ii < line.count; ii++) {
                            const FlexItem& child = line.items[ii];
                            float paddingBorderSum =
                                MainAxisSum(child.padding + child.border, dir);
                            float itemLen =
                                F32Max(MaybeMax(child.flexBasis,
                                                Main(child.minSize, dir)) +
                                           MainAxisSum(child.margin, dir),
                                       paddingBorderSum);
                            VecAppend(itemLengths, itemLen);
                        }
                    }
                    Vec<int> counts;
                    int* slots = VecAppendBlanks(counts, itemCount);
                    int minLines = c->lineCount < 1 ? 1 : c->lineCount;
                    int nLines = BalancedLineItemCounts(
                        itemLengths.els, itemCount, INFINITY, Main(c->gap, dir),
                        minLines, slots);
                    float widest = 0.0f;
                    int index = 0;
                    for (int li = 0; li < nLines; li++) {
                        float lineLength = 0.0f;
                        for (int k = 0; k < slots[li]; k++) {
                            lineLength += itemLengths[index++];
                        }
                        lineLength += SumAxisGaps(Main(c->gap, dir), slots[li]);
                        widest = F32Max(widest, lineLength);
                    }
                    float maxContent = widest + mainContentBoxInset;
                    outerMainSize =
                        F32Max(size, F32Min(maxContent, mainAvail.value));
                }
            } else {
                outerMainSize =
                    lines->len > 1 ? F32Max(size, mainAvail.value) : size;
            }
        } else if (mainAvail.kind == AvailableSpace::Kind::MinContent &&
                   c->isWrap) {
            outerMainSize = LongestLineLength(lines, *c) + mainContentBoxInset;
        } else {

            float mainSize = 0.0f;
            for (int li = 0; li < lines->len; li++) {
                FlexLine& line = (*lines)[li];
                for (int ii = 0; ii < line.count; ii++) {
                    FlexItem& item = line.items[ii];
                    Optf styleMin = Main(item.minSize, dir);
                    Optf stylePreferred = Main(item.size, dir);
                    Optf styleMax = Main(item.maxSize, dir);

                    Optf clampingBasis =
                        c->isRow
                            ? MaybeMax(Some(item.flexBasis), stylePreferred)
                            : Some(item.flexBasis);
                    Optf flexBasisMin =
                        Filter(clampingBasis, item.flexShrink == 0.0f);
                    Optf flexBasisMax =
                        Filter(clampingBasis, item.flexGrow == 0.0f);

                    float minMainSize = F32Max(
                        UnwrapOr(
                            Or(MaybeMax(styleMin, flexBasisMin), flexBasisMin),
                            item.resolvedMinimumMainSize),
                        item.resolvedMinimumMainSize);
                    float maxMainSize = UnwrapOr(
                        Or(MaybeMin(styleMax, flexBasisMax), flexBasisMax),
                        INFINITY);

                    float contentContribution;
                    if (IsSome(stylePreferred) &&
                        (maxMainSize <= minMainSize ||
                         maxMainSize <= stylePreferred)) {

                        contentContribution =
                            F32Max(F32Min(stylePreferred, maxMainSize),
                                   minMainSize) +
                            MainAxisSum(item.margin, dir);
                    } else if (maxMainSize <= minMainSize) {
                        contentContribution =
                            minMainSize + MainAxisSum(item.margin, dir);
                    } else if (item.IsScroll()) {
                        contentContribution =
                            item.flexBasis + MainAxisSum(item.margin, dir);
                    } else if (IsSome(stylePreferred)) {

                        float itemPbMain = MainAxisSum(item.padding, dir) +
                                           MainAxisSum(item.border, dir);
                        float innerMainSize =
                            F32Max(stylePreferred, itemPbMain);
                        float marginSum = MainAxisSum(item.margin, dir);

                        if (c->isRow) {
                            contentContribution = MaybeClamp(
                                innerMainSize + marginSum, styleMin, styleMax);
                        } else {
                            contentContribution = MaybeClamp(
                                F32Max(innerMainSize, item.flexBasis) +
                                    marginSum,
                                styleMin, styleMax);
                        }
                    } else {
                        Optf crossAxisParentSize = Cross(c->nodeInnerSize, dir);

                        Optf childMinCross = Cross(item.minSize, dir);
                        Optf childMaxCross = Cross(item.maxSize, dir);
                        AvailableSpace crossAxisAvailableSpace =
                            availableSpace.Cross(dir);
                        if (crossAxisAvailableSpace
                                .kind == AvailableSpace::Kind::Definite) {
                            crossAxisAvailableSpace =
                                AvailableSpace::Definite(F32Max(
                                    0.0f, c->DividedCrossSpace(UnwrapOr(
                                              crossAxisParentSize,
                                              crossAxisAvailableSpace.value)) -
                                              CrossAxisSum(item.margin, dir)));
                        }
                        crossAxisAvailableSpace =
                            MaybeClamp(crossAxisAvailableSpace, childMinCross,
                                       childMaxCross);

                        SizeAvail childAvailableSpace = availableSpace;
                        childAvailableSpace
                            .SetCross(dir, crossAxisAvailableSpace);

                        SizeFOpt childKnownDimensions = item.size;
                        SetMain(&childKnownDimensions, dir, None());

                        SizeFOpt transferredMinSize = MaybeApplyAspectRatio(
                            item.minSize, item.aspectRatio);
                        SizeFOpt transferredMaxSize = MaybeApplyAspectRatio(
                            item.maxSize, item.aspectRatio);
                        SetCross(&childKnownDimensions, dir,
                                 MaybeClamp(Cross(childKnownDimensions, dir),
                                            Cross(transferredMinSize, dir),
                                            Cross(transferredMaxSize, dir)));
                        if (item.alignSelf
                                    .keyword == AlignItemsKeyword::Stretch &&
                            !IsSome(Cross(childKnownDimensions, dir))) {
                            SetCross(&childKnownDimensions, dir,
                                     crossAxisAvailableSpace.IntoOption());
                        }

                        float measuredMainSize = tree->MeasureChildSize(
                            item.node, childKnownDimensions, c->nodeInnerSize,
                            childAvailableSpace, SizingMode::ContentSize,
                            MainAxis(dir), LineBool::False());

                        Optf cross = Cross(childKnownDimensions, dir);
                        float innerMainSize = measuredMainSize;
                        if (IsSome(item.aspectRatio) && IsSome(cross)) {
                            float transferred = c->isRow
                                                    ? cross * item.aspectRatio
                                                    : cross / item.aspectRatio;
                            innerMainSize = F32Max(innerMainSize, transferred);
                        }
                        float marginSum = MainAxisSum(item.margin, dir);

                        if (c->isRow) {
                            contentContribution = MaybeClamp(
                                innerMainSize + marginSum, styleMin, styleMax);
                        } else {
                            contentContribution = MaybeClamp(
                                F32Max(innerMainSize, item.flexBasis) +
                                    marginSum,
                                styleMin, styleMax);
                        }
                    }

                    float diff = contentContribution - item.flexBasis;
                    if (diff > 0.0f) {
                        item.contentFlexFraction =
                            diff / F32Max(1.0f, item.flexGrow);
                    } else if (diff < 0.0f) {
                        float scaledShrinkFactor =
                            F32Max(1.0f, item.flexShrink) * item.innerFlexBasis;
                        item.contentFlexFraction = diff / scaledShrinkFactor;
                    } else {
                        item.contentFlexFraction = 0.0f;
                    }
                }

                float itemMainSizeSum = 0.0f;
                for (int ii = 0; ii < line.count; ii++) {
                    FlexItem& item = line.items[ii];
                    float flexFraction = item.contentFlexFraction;
                    float flexContribution = 0.0f;
                    if (item.contentFlexFraction > 0.0f) {
                        flexContribution =
                            F32Max(1.0f, item.flexGrow) * flexFraction;
                    } else if (item.contentFlexFraction < 0.0f) {
                        float scaledShrinkFactor =
                            F32Max(1.0f, item.flexShrink) * item.innerFlexBasis;
                        flexContribution =
                            scaledShrinkFactor == 0.0f
                                ? 0.0f
                                : scaledShrinkFactor * flexFraction;
                    }
                    float size = item.flexBasis + flexContribution;
                    SetMain(&item.outerTargetSize, dir, size);
                    SetMain(&item.targetSize, dir, size);
                    itemMainSizeSum += size;
                }

                float gapSum = SumAxisGaps(Main(c->gap, dir), line.count);
                mainSize = F32Max(mainSize, itemMainSizeSum + gapSum);
            }
            outerMainSize = mainSize + mainContentBoxInset;
        }
    }

    outerMainSize = F32Max(
        MaybeClamp(outerMainSize, Main(c->minSize, dir), Main(c->maxSize, dir)),
        mainContentBoxInset - Main(c->scrollbarGutter, dir));
    float innerMainSize = F32Max(outerMainSize - mainContentBoxInset, 0.0f);
    SetMain(&c->containerSize, dir, outerMainSize);
    SetMain(&c->innerContainerSize, dir, innerMainSize);
    SetMain(&c->nodeInnerSize, dir, Some(innerMainSize));
}

void ResolveFlexibleLengths(FlexLine* line, const AlgoConstants& c) {
    FlexDirection dir = c.dir;
    float totalMainAxisGap = SumAxisGaps(Main(c.gap, dir), line->count);

    float totalHypotheticalOuterMainSize = 0.0f;
    for (int i = 0; i < line->count; i++) {
        totalHypotheticalOuterMainSize +=
            Main(line->items[i].hypotheticalOuterSize, dir);
    }
    float usedFlexFactor = totalMainAxisGap + totalHypotheticalOuterMainSize;
    float innerMain = UnwrapOr(Main(c.nodeInnerSize, dir), 0.0f);
    bool growing = usedFlexFactor < innerMain;
    bool shrinking = usedFlexFactor > innerMain;
    bool exactlySized = !growing && !shrinking;

    for (int i = 0; i < line->count; i++) {
        FlexItem& child = line->items[i];
        float innerTargetSize = Main(child.hypotheticalInnerSize, dir);
        SetMain(&child.targetSize, dir, innerTargetSize);

        if (exactlySized ||
            (child.flexGrow == 0.0f && child.flexShrink == 0.0f) ||
            (growing && child.flexBasis > innerTargetSize) ||
            (shrinking && child.flexBasis < innerTargetSize)) {
            child.frozen = true;
            SetMain(&child.outerTargetSize, dir,
                    innerTargetSize + MainAxisSum(child.margin, dir));
        }
    }

    if (exactlySized) {
        return;
    }

    float usedSpace = totalMainAxisGap;
    for (int i = 0; i < line->count; i++) {
        FlexItem& child = line->items[i];
        usedSpace += child.frozen
                         ? Main(child.outerTargetSize, dir)
                         : child.flexBasis + MainAxisSum(child.margin, dir);
    }
    float initialFreeSpace =
        UnwrapOr(MaybeSub(Main(c.nodeInnerSize, dir), usedSpace), 0.0f);

    while (true) {

        bool allFrozen = true;
        for (int i = 0; i < line->count; i++) {
            if (!line->items[i].frozen) {
                allFrozen = false;
                break;
            }
        }
        if (allFrozen) {
            break;
        }

        usedSpace = totalMainAxisGap;
        float sumFlexGrow = 0.0f;
        float sumFlexShrink = 0.0f;
        for (int i = 0; i < line->count; i++) {
            FlexItem& child = line->items[i];
            usedSpace += child.frozen
                             ? Main(child.outerTargetSize, dir)
                             : child.flexBasis + MainAxisSum(child.margin, dir);
            if (!child.frozen) {
                sumFlexGrow += child.flexGrow;
                sumFlexShrink += child.flexShrink;
            }
        }

        Optf remaining = MaybeSub(Main(c.nodeInnerSize, dir), usedSpace);
        float freeSpace;
        if (growing && sumFlexGrow < 1.0f) {
            freeSpace = MaybeMin(
                initialFreeSpace * sumFlexGrow - totalMainAxisGap, remaining);
        } else if (shrinking && sumFlexShrink < 1.0f) {
            freeSpace = MaybeMax(
                initialFreeSpace * sumFlexShrink - totalMainAxisGap, remaining);
        } else {
            freeSpace = UnwrapOr(remaining, usedFlexFactor - usedSpace);
        }

        bool isNormal = std::isnormal(freeSpace);
        if (isNormal) {
            if (growing && sumFlexGrow > 0.0f) {
                for (int i = 0; i < line->count; i++) {
                    FlexItem& child = line->items[i];
                    if (child.frozen) {
                        continue;
                    }
                    SetMain(&child.targetSize, dir,
                            child.flexBasis +
                                freeSpace * (child.flexGrow / sumFlexGrow));
                }
            } else if (shrinking && sumFlexShrink > 0.0f) {
                float sumScaledShrinkFactor = 0.0f;
                for (int i = 0; i < line->count; i++) {
                    FlexItem& child = line->items[i];
                    if (!child.frozen) {
                        sumScaledShrinkFactor +=
                            child.innerFlexBasis * child.flexShrink;
                    }
                }
                if (sumScaledShrinkFactor > 0.0f) {
                    for (int i = 0; i < line->count; i++) {
                        FlexItem& child = line->items[i];
                        if (child.frozen) {
                            continue;
                        }
                        float scaledShrinkFactor =
                            child.innerFlexBasis * child.flexShrink;
                        SetMain(&child.targetSize, dir,
                                child.flexBasis +
                                    freeSpace * (scaledShrinkFactor /
                                                 sumScaledShrinkFactor));
                    }
                }
            }
        }

        float totalViolation = 0.0f;
        for (int i = 0; i < line->count; i++) {
            FlexItem& child = line->items[i];
            if (child.frozen) {
                continue;
            }
            Optf resolvedMinMain = Some(child.resolvedMinimumMainSize);
            Optf maxMain = Main(child.maxSize, dir);
            float clamped = F32Max(MaybeClamp(Main(child.targetSize, dir),
                                              resolvedMinMain, maxMain),
                                   0.0f);
            child.violation = clamped - Main(child.targetSize, dir);
            SetMain(&child.targetSize, dir, clamped);
            SetMain(&child.outerTargetSize, dir,
                    clamped + MainAxisSum(child.margin, dir));
            totalViolation += child.violation;
        }

        for (int i = 0; i < line->count; i++) {
            FlexItem& child = line->items[i];
            if (child.frozen) {
                continue;
            }
            if (totalViolation > 0.0f) {
                child.frozen = child.violation > 0.0f;
            } else if (totalViolation < 0.0f) {
                child.frozen = child.violation < 0.0f;
            } else {
                child.frozen = true;
            }
        }
    }
}

bool ItemCrossIsDefinite(const AlgoConstants& c, const FlexItem& item) {
    FlexDirection dir = c.dir;
    bool hasCrossAutoMargins =
        item.marginIsAuto.CrossStart(dir) || item.marginIsAuto.CrossEnd(dir);
    Dimension crossSize = item.sizeStyle.Cross(dir);
    bool isStretched =
        !hasCrossAutoMargins &&
        (crossSize.IsStretch() ||
         (item.alignSelf == AlignSelf{AlignItemsKeyword::Stretch} &&
          crossSize.IsAuto()));
    return isStretched || IsSome(Cross(item.size, dir)) ||
           (!c.isRow && c.crossAxisAvailableSpaceIsDefinite);
}

LayoutInput ItemLayoutInput(const AlgoConstants& c, const FlexItem& item,
                            RunMode runMode, RequestedAxis axis, SizeFOpt known,
                            SizeAvail available) {
    bool mainDefinite = c.hasDefiniteMainSize || item.flexBasisIsDefinite;
    bool crossDefinite = ItemCrossIsDefinite(c, item);
    LayoutInput in;
    in.runMode = runMode;
    in.sizingMode = SizingMode::ContentSize;
    in.axis = axis;
    in.knownDimensions = known;
    in.knownDimensionsAreDefiniteW = c.isRow ? mainDefinite : crossDefinite;
    in.knownDimensionsAreDefiniteH = c.isRow ? crossDefinite : mainDefinite;
    in.parentSize = c.nodeInnerSize;
    in.availableSpace = available;
    in.verticalMarginsAreCollapsible = LineBool::False();
    return in;
}

void DetermineHypotheticalCrossSize(TaffyTree* tree, FlexLine* line,
                                    const AlgoConstants& c,
                                    SizeAvail availableSpace) {
    FlexDirection dir = c.dir;
    for (int i = 0; i < line->count; i++) {
        FlexItem& child = line->items[i];
        float paddingBorderSum =
            CrossAxisSum(child.padding + child.border, dir);

        AvailableSpace childAvailableMain = AvailableSpace::Definite(F32Max(
            0.0f, Main(c.containerSize, dir) - MainAxisSum(child.margin, dir)));

        Optf transferredMinCross =
            Cross(MaybeApplyAspectRatio(child.minSize, child.aspectRatio), dir);
        Optf transferredMaxCross =
            Cross(MaybeApplyAspectRatio(child.maxSize, child.aspectRatio), dir);

        Optf childCross =
            MaybeMax(MaybeClamp(Cross(child.size, dir), transferredMinCross,
                                transferredMaxCross),
                     paddingBorderSum);

        AvailableSpace crossAvail = availableSpace.Cross(dir);
        if (crossAvail.kind == AvailableSpace::Kind::Definite) {
            crossAvail = AvailableSpace::Definite(
                F32Max(0.0f, c.DividedCrossSpace(crossAvail.value) -
                                 CrossAxisSum(child.margin, dir)));
        }
        AvailableSpace childAvailableCross = MaybeMax(
            MaybeClamp(crossAvail, transferredMinCross, transferredMaxCross),
            paddingBorderSum);

        Optf innerCross = Cross(c.nodeInnerSize, dir);
        if (IsSome(innerCross)) {
            innerCross = Some(c.DividedCrossSpace(innerCross));
        }
        Optf crossStretch = MaybeMax(
            MaybeSub(innerCross, Some(CrossAxisSum(child.margin, dir))), 0.0f);
        SizingKeywordResolution crossKeyword =
            ResolveSizingKeyword(child.sizeStyle.Cross(dir), crossStretch,
                                 Cross(c.nodeInnerSize, dir), tree->calc);
        if (crossKeyword.isSome &&
            crossKeyword.kind == SizingKeywordKind::Measure) {
            childAvailableCross = crossKeyword.measure;
        }

        float childInnerCross;
        if (IsSome(childCross)) {
            childInnerCross = childCross;
        } else {
            SizeFOpt known = {c.isRow ? Some(child.targetSize.w) : childCross,
                              c.isRow ? childCross : Some(child.targetSize.h)};
            SizeAvail avail = {
                c.isRow ? childAvailableMain : childAvailableCross,
                c.isRow ? childAvailableCross : childAvailableMain};
            LayoutOutput measuredOut = tree->ComputeChildLayout(
                child.node, ItemLayoutInput(c, child, RunMode::ComputeSize,
                                            c.isRow ? RequestedAxis::Vertical
                                                    : RequestedAxis::Horizontal,
                                            known, avail));
            float measured = GetAbs(measuredOut.size, CrossAxis(dir));
            childInnerCross = F32Max(
                MaybeClamp(measured, transferredMinCross, transferredMaxCross),
                paddingBorderSum);
        }
        float childOuterCross =
            childInnerCross + CrossAxisSum(child.margin, dir);

        SetCross(&child.hypotheticalInnerSize, dir, childInnerCross);
        SetCross(&child.hypotheticalOuterSize, dir, childOuterCross);
    }
}

void CalculateChildrenBaseLines(TaffyTree* tree, SizeFOpt nodeSize,
                                SizeAvail availableSpace, Vec<FlexLine>* lines,
                                const AlgoConstants& c) {

    if (!c.isRow) {
        return;
    }

    for (int li = 0; li < lines->len; li++) {
        FlexLine& line = (*lines)[li];

        int participating = 0;
        for (int i = 0; i < line.count; i++) {
            if (line.items[i].ParticipatesInBaselineAlignment(c.dir)) {
                participating++;
            }
        }
        if (participating <= 1) {
            continue;
        }

        for (int i = 0; i < line.count; i++) {
            FlexItem& child = line.items[i];
            if (!child.ParticipatesInBaselineAlignment(c.dir)) {
                continue;
            }
            SizeFOpt known = {c.isRow ? Some(child.targetSize.w)
                                      : Some(child.hypotheticalInnerSize.w),
                              c.isRow ? Some(child.hypotheticalInnerSize.h)
                                      : Some(child.targetSize.h)};

            SizeAvail avail = {
                c.isRow ? AvailableSpace::Definite(c.containerSize.w)
                        : availableSpace.width.MaybeSet(nodeSize.w),
                c.isRow ? availableSpace.height.MaybeSet(nodeSize.h)
                        : AvailableSpace::Definite(c.containerSize.h)};
            SizeF marginSum = child.margin.SumAxes();
            avail.width =
                MaybeMax(MaybeSub(avail.width, Some(marginSum.w)), Some(0.0f));
            avail.height =
                MaybeMax(MaybeSub(avail.height, Some(marginSum.h)), Some(0.0f));
            LayoutOutput out = tree->ComputeChildLayout(
                child.node, ItemLayoutInput(c, child, RunMode::PerformLayout,
                                            RequestedAxis::Both, known, avail));
            float baseline = UnwrapOr(out.baselines.first, out.size.h);
            if (IsScrollContainer(child.overflow.y)) {
                baseline = F32Max(0.0f, F32Min(baseline, out.size.h));
            }
            child.baseline = baseline + child.margin.top;
        }
    }
}

void CalculateCrossSize(Vec<FlexLine>* lines, SizeFOpt nodeSize,
                        const AlgoConstants& c) {
    FlexDirection dir = c.dir;
    if (lines->len == 0) {
        return;
    }

    if (!c.isWrap && IsSome(Cross(nodeSize, dir))) {
        float crossAxisPaddingBorder = CrossAxisSum(c.contentBoxInset, dir);
        (*lines)[0].crossSize = UnwrapOr(
            MaybeMax(
                MaybeSub(MaybeClamp(Cross(nodeSize, dir), Cross(c.minSize, dir),
                                    Cross(c.maxSize, dir)),
                         crossAxisPaddingBorder),
                0.0f),
            0.0f);
        return;
    }

    for (int li = 0; li < lines->len; li++) {
        FlexLine& line = (*lines)[li];
        float maxBaseline = 0.0f;
        for (int i = 0; i < line.count; i++) {
            maxBaseline = F32Max(maxBaseline, line.items[i].baseline);
        }
        float crossSize = 0.0f;
        for (int i = 0; i < line.count; i++) {
            FlexItem& child = line.items[i];
            float v;
            if (child.ParticipatesInBaselineAlignment(dir)) {
                v = maxBaseline - child.baseline +
                    Cross(child.hypotheticalOuterSize, dir);
            } else {
                v = Cross(child.hypotheticalOuterSize, dir);
            }
            crossSize = F32Max(crossSize, v);
        }
        line.crossSize = crossSize;
    }

    if (!c.isWrap) {
        float crossAxisPaddingBorder = CrossAxisSum(c.contentBoxInset, dir);
        (*lines)[0].crossSize =
            MaybeClamp((*lines)[0].crossSize,
                       MaybeSub(Cross(c.minSize, dir), crossAxisPaddingBorder),
                       MaybeSub(Cross(c.maxSize, dir), crossAxisPaddingBorder));
    }
}

void HandleAlignContentStretch(Vec<FlexLine>* lines, SizeFOpt nodeSize,
                               const AlgoConstants& c) {
    if (c.alignContent.keyword != AlignContentKeyword::Stretch ||
        lines->len == 0) {
        return;
    }
    FlexDirection dir = c.dir;
    float crossAxisPaddingBorder = CrossAxisSum(c.contentBoxInset, dir);
    Optf crossMinSize = Cross(c.minSize, dir);
    Optf crossMaxSize = Cross(c.maxSize, dir);
    float containerMinInnerCross = UnwrapOr(
        MaybeMax(MaybeSub(MaybeClamp(Or(Cross(nodeSize, dir), crossMinSize),
                                     crossMinSize, crossMaxSize),
                          crossAxisPaddingBorder),
                 0.0f),
        0.0f);

    float totalCrossAxisGap = SumAxisGaps(Cross(c.gap, dir), lines->len);
    float linesTotalCross = totalCrossAxisGap;
    for (int i = 0; i < lines->len; i++) {
        linesTotalCross += (*lines)[i].crossSize;
    }

    if (linesTotalCross < containerMinInnerCross) {
        float addition =
            (containerMinInnerCross - linesTotalCross) / (float)lines->len;
        for (int i = 0; i < lines->len; i++) {
            (*lines)[i].crossSize += addition;
        }
    }
}

void DetermineUsedCrossSize(TaffyTree* tree, Vec<FlexLine>* lines,
                            const AlgoConstants& c) {
    CalcResolver calc = tree->calc;
    FlexDirection dir = c.dir;
    for (int li = 0; li < lines->len; li++) {
        FlexLine& line = (*lines)[li];
        float lineCrossSize = line.crossSize;

        for (int i = 0; i < line.count; i++) {
            FlexItem& child = line.items[i];
            const Style& cs = tree->GetStyle(child.node);

            bool crossIsStretch = child.sizeStyle.Cross(dir).IsStretch();
            float used;
            if (!child.marginIsAuto.CrossStart(dir) &&
                !child.marginIsAuto.CrossEnd(dir) &&
                (crossIsStretch ||
                 (child.alignSelf.keyword == AlignItemsKeyword::Stretch &&
                  cs.size.Cross(dir).IsAuto()))) {

                RectF padding = cs.padding
                                    .ResolveOrZero(c.nodeInnerSize.w, calc);
                RectF border = cs.border.ResolveOrZero(c.nodeInnerSize.w, calc);
                SizeF pbSum = (padding + border).SumAxes();
                SizeF boxSizingAdjustment =
                    cs.boxSizing == BoxSizing::ContentBox ? pbSum
                                                          : SizeF::Zero();
                SizeFOpt maxSizeIgnoringAspectRatio =
                    MaybeAdd(cs.maxSize.MaybeResolve(c.nodeInnerSize, calc),
                             boxSizingAdjustment);
                used = MaybeClamp(
                    F32Max(0.0f,
                           lineCrossSize - CrossAxisSum(child.margin, dir)),
                    Cross(child.minSize, dir),
                    Cross(maxSizeIgnoringAspectRatio, dir));
            } else {
                used = Cross(child.hypotheticalInnerSize, dir);
            }
            SetCross(&child.targetSize, dir, used);
            SetCross(&child.outerTargetSize, dir,
                     used + CrossAxisSum(child.margin, dir));
        }
    }
}

void DistributeRemainingFreeSpace(Vec<FlexLine>* lines,
                                  const AlgoConstants& c) {
    FlexDirection dir = c.dir;
    for (int li = 0; li < lines->len; li++) {
        FlexLine& line = (*lines)[li];
        float totalMainAxisGap = SumAxisGaps(Main(c.gap, dir), line.count);
        float usedSpace = totalMainAxisGap;
        for (int i = 0; i < line.count; i++) {
            usedSpace += Main(line.items[i].outerTargetSize, dir);
        }
        float freeSpace = Main(c.innerContainerSize, dir) - usedSpace;

        int numAutoMargins = 0;
        for (int i = 0; i < line.count; i++) {
            FlexItem& child = line.items[i];
            if (child.marginIsAuto.MainStart(dir)) {
                numAutoMargins++;
            }
            if (child.marginIsAuto.MainEnd(dir)) {
                numAutoMargins++;
            }
        }

        if (freeSpace > 0.0f && numAutoMargins > 0) {
            float margin = freeSpace / (float)numAutoMargins;
            for (int i = 0; i < line.count; i++) {
                FlexItem& child = line.items[i];
                if (child.marginIsAuto.MainStart(dir)) {
                    if (c.isRow) {
                        child.margin.left = margin;
                    } else {
                        child.margin.top = margin;
                    }
                }
                if (child.marginIsAuto.MainEnd(dir)) {
                    if (c.isRow) {
                        child.margin.right = margin;
                    } else {
                        child.margin.bottom = margin;
                    }
                }
            }

            freeSpace = 0.0f;
        }

        int numItems = line.count;
        bool layoutReverse = IsReverse(dir);
        float gap = Main(c.gap, dir);
        AlignContentKeyword mode =
            ApplyAlignmentFallback(freeSpace, numItems, c.justifyContent);

        for (int i = 0; i < numItems; i++) {
            FlexItem& child =
                layoutReverse ? line.items[numItems - 1 - i] : line.items[i];
            child.offsetMain = ComputeAlignmentOffset(
                freeSpace, numItems, gap, mode, layoutReverse, i == 0);
        }
    }
}

float AlignFlexItemsAlongCrossAxis(const FlexItem& child, float freeSpace,
                                   float maxBaseline,
                                   float maxBaselineToBottomDistance,
                                   const AlgoConstants& c) {
    bool crossAxisShouldReverse =
        c.isColumn && c.layoutDirection == Direction::Rtl;

    AlignItemsKeyword keyword = (child.alignSelf.IsSafe() && freeSpace < 0.0f)
                                    ? AlignItemsKeyword::Start
                                    : child.alignSelf.keyword;

    switch (keyword) {
        case AlignItemsKeyword::Start:
            return crossAxisShouldReverse ? freeSpace : 0.0f;
        case AlignItemsKeyword::FlexStart:
            return (c.isWrapReverse != crossAxisShouldReverse) ? freeSpace
                                                               : 0.0f;
        case AlignItemsKeyword::End:
            return crossAxisShouldReverse ? 0.0f : freeSpace;
        case AlignItemsKeyword::FlexEnd:
            return (c.isWrapReverse != crossAxisShouldReverse) ? 0.0f
                                                               : freeSpace;
        case AlignItemsKeyword::Center:
            return freeSpace / 2.0f;
        case AlignItemsKeyword::Baseline: {
            if (c.isRow) {
                if (c.isWrapReverse) {
                    float lineCrossSize =
                        freeSpace + Cross(child.outerTargetSize, c.dir);
                    return lineCrossSize - maxBaselineToBottomDistance -
                           child.baseline;
                }
                return maxBaseline - child.baseline;
            }

            bool baselineColumnShouldReverse =
                crossAxisShouldReverse && !c.isWrap;
            return (c.isWrapReverse != baselineColumnShouldReverse) ? freeSpace
                                                                    : 0.0f;
        }
        default:
            return (c.isWrapReverse != crossAxisShouldReverse) ? freeSpace
                                                               : 0.0f;
    }
}

void ResolveCrossAxisAutoMargins(Vec<FlexLine>* lines, const AlgoConstants& c) {
    FlexDirection dir = c.dir;
    for (int li = 0; li < lines->len; li++) {
        FlexLine& line = (*lines)[li];
        float lineCrossSize = line.crossSize;
        float maxBaseline = 0.0f;
        float maxBaselineToBottomDistance = 0.0f;
        for (int i = 0; i < line.count; i++) {
            maxBaseline = F32Max(maxBaseline, line.items[i].baseline);
            if (line.items[i].ParticipatesInBaselineAlignment(c.dir)) {
                maxBaselineToBottomDistance =
                    F32Max(maxBaselineToBottomDistance,
                           Cross(line.items[i].outerTargetSize, c.dir) -
                               line.items[i].baseline);
            }
        }

        for (int i = 0; i < line.count; i++) {
            FlexItem& child = line.items[i];
            float freeSpace = lineCrossSize - Cross(child.outerTargetSize, dir);

            if (child.marginIsAuto.CrossStart(dir) && child.marginIsAuto
                                                          .CrossEnd(dir)) {
                if (c.isRow) {
                    child.margin.top = freeSpace / 2.0f;
                    child.margin.bottom = freeSpace / 2.0f;
                } else {
                    child.margin.left = freeSpace / 2.0f;
                    child.margin.right = freeSpace / 2.0f;
                }
            } else if (child.marginIsAuto.CrossStart(dir)) {
                if (c.isRow) {
                    child.margin.top = freeSpace;
                } else {
                    child.margin.left = freeSpace;
                }
            } else if (child.marginIsAuto.CrossEnd(dir)) {
                if (c.isRow) {
                    child.margin.bottom = freeSpace;
                } else {
                    child.margin.right = freeSpace;
                }
            } else {

                child.offsetCross = AlignFlexItemsAlongCrossAxis(
                    child, freeSpace, maxBaseline, maxBaselineToBottomDistance,
                    c);
            }
        }
    }
}

float DetermineContainerCrossSize(Vec<FlexLine>* lines, SizeFOpt nodeSize,
                                  AlgoConstants* c) {
    FlexDirection dir = c->dir;
    float totalCrossAxisGap = SumAxisGaps(Cross(c->gap, dir), lines->len);
    float totalLineCrossSize = 0.0f;
    for (int i = 0; i < lines->len; i++) {
        totalLineCrossSize += (*lines)[i].crossSize;
    }

    float paddingBorderSum = CrossAxisSum(c->contentBoxInset, dir);
    float crossScrollbarGutter = Cross(c->scrollbarGutter, dir);
    float outerContainerSize = F32Max(
        MaybeClamp(
            UnwrapOr(Cross(nodeSize, dir),
                     totalLineCrossSize + totalCrossAxisGap + paddingBorderSum),
            Cross(c->minSize, dir), Cross(c->maxSize, dir)),
        paddingBorderSum - crossScrollbarGutter);
    float innerContainerSize =
        F32Max(outerContainerSize - paddingBorderSum, 0.0f);

    SetCross(&c->containerSize, dir, outerContainerSize);
    SetCross(&c->innerContainerSize, dir, innerContainerSize);

    return totalLineCrossSize;
}

void AlignFlexLinesPerAlignContent(Vec<FlexLine>* lines, const AlgoConstants& c,
                                   float totalCrossSize) {
    int numLines = lines->len;
    float gap = Cross(c.gap, c.dir);
    float totalCrossAxisGap = SumAxisGaps(gap, numLines);
    float freeSpace =
        Cross(c.innerContainerSize, c.dir) - totalCrossSize - totalCrossAxisGap;

    AlignContentKeyword mode =
        ApplyAlignmentFallback(freeSpace, numLines, c.alignContent);

    for (int i = 0; i < numLines; i++) {
        FlexLine& line =
            c.isWrapReverse ? (*lines)[numLines - 1 - i] : (*lines)[i];
        line.offsetCross = ComputeAlignmentOffset(
            freeSpace, numLines, gap, mode, c.isWrapReverse, i == 0);
    }
}

void CalculateFlexItem(TaffyTree* tree, FlexItem* item, float* totalOffsetMain,
                       float totalOffsetCross, float lineOffsetCross,
                       RectF* totalOverflowRect, const AlgoConstants& c,
                       Vec<OofCandidate>* oofPool) {
    FlexDirection direction = c.dir;
    Direction layoutDirection = c.layoutDirection;
    SizeF containerSize = c.containerSize;
    bool mainDefinite = c.hasDefiniteMainSize || item->flexBasisIsDefinite;
    bool crossDefinite = ItemCrossIsDefinite(c, *item);
    LayoutInput childInput;
    childInput.runMode = RunMode::PerformLayout;
    childInput.sizingMode = SizingMode::ContentSize;
    childInput.axis = RequestedAxis::Both;
    childInput.knownDimensions = AsOptional(item->targetSize);
    childInput.knownDimensionsAreDefiniteW =
        IsRow(direction) ? mainDefinite : crossDefinite;
    childInput.knownDimensionsAreDefiniteH =
        IsRow(direction) ? crossDefinite : mainDefinite;
    childInput.parentSize = c.nodeInnerSize;

    SizeF childMarginSum = item->margin.SumAxes();
    childInput.availableSpace =
        SizeAvail::Definite({F32Max(0.0f, containerSize.w - childMarginSum.w),
                             F32Max(0.0f, containerSize.h - childMarginSum.h)});
    childInput.verticalMarginsAreCollapsible = LineBool::False();
    LayoutOutput layoutOutput =
        tree->ComputeChildLayout(item->node, childInput);
    SizeF size = layoutOutput.size;
    RectF scrollableOverflowRect = layoutOutput.scrollableOverflowRect;

    bool isRtlRow = IsRow(direction) && IsRtl(layoutDirection);
    bool isRtlColumn = IsColumn(direction) && IsRtl(layoutDirection);

    float mainRelativeInset = Main(item->relativeInset, direction);
    float crossRelativeInset = Cross(item->relativeInset, direction);

    float effectiveLineOffsetCross = isRtlColumn ? 0.0f : lineOffsetCross;

    float offsetMain =
        isRtlRow
            ? *totalOffsetMain - item->offsetMain -
                  MainEnd(item->margin, direction) - mainRelativeInset - size.w
            : *totalOffsetMain + item->offsetMain +
                  MainStart(item->margin, direction) + mainRelativeInset;

    float offsetCross =
        totalOffsetCross + item->offsetCross + effectiveLineOffsetCross +
        CrossStart(item->margin, direction) + crossRelativeInset;

    float innerBaseline = UnwrapOr(layoutOutput.baselines.first, size.h);
    if (IsRow(direction) && IsScrollContainer(item->overflow.y)) {
        innerBaseline = F32Max(0.0f, F32Min(innerBaseline, size.h));
    }
    if (IsRow(direction)) {
        float baselineOffsetCross = totalOffsetCross + item->offsetCross +
                                    effectiveLineOffsetCross +
                                    CrossStart(item->margin, direction);
        item->baseline = baselineOffsetCross + innerBaseline;
    } else {
        float baselineOffsetMain = *totalOffsetMain + item->offsetMain +
                                   MainStart(item->margin, direction);
        item->baseline = baselineOffsetMain + innerBaseline;
    }

    PointF location = IsRow(direction) ? PointF{offsetMain, offsetCross}
                                       : PointF{offsetCross, offsetMain};
    SizeF scrollbarSize = {
        item->overflow.y == Overflow::Scroll ? item->scrollbarWidth : 0.0f,
        item->overflow.x == Overflow::Scroll ? item->scrollbarWidth : 0.0f};

    Layout layout;
    layout.order = item->order;
    layout.size = size;
    layout.scrollableOverflowRect = scrollableOverflowRect;
    layout.scrollbarSize = scrollbarSize;
    layout.location = location;
    layout.padding = item->padding;
    layout.border = item->border;
    layout.margin = item->margin;
    tree->SetUnroundedLayout(item->node, layout);

    OofCandRange range;
    OofAppendRange(oofPool, &range, layoutOutput.oofCandidates);
    if (range.count > 0) {
        OofTranslateRange(oofPool, range, location);
    }
    item->oofStart = range.start;
    item->oofCount = range.count;

    float advance = item->offsetMain + MainAxisSum(item->margin, direction) +
                    Main(size, direction);
    if (isRtlRow) {
        *totalOffsetMain -= advance;
    } else {
        *totalOffsetMain += advance;
    }

    PointF contributionLocation =
        IsRtl(layoutDirection)
            ? PointF{containerSize.w - (location.x + size.w) - c.border.right,
                     location.y - c.border.top}
            : PointF{location.x - c.border.left, location.y - c.border.top};
    *totalOverflowRect =
        totalOverflowRect->Union(ComputeScrollableOverflowContribution(
            contributionLocation, size, scrollableOverflowRect, item->overflow,
            item->contain, c.isScrollContainer));
}

void CalculateLayoutLine(TaffyTree* tree, FlexLine* line,
                         float* totalOffsetCross, RectF* overflowRect,
                         const AlgoConstants& c, Vec<OofCandidate>* oofPool) {
    FlexDirection direction = c.dir;
    Direction layoutDirection = c.layoutDirection;
    SizeF containerSize = c.containerSize;
    RectF paddingBorder = c.contentBoxInset;
    float totalOffsetMain =
        (IsRtl(layoutDirection) && IsRow(direction))
            ? containerSize.w - MainEnd(paddingBorder, direction)
            : MainStart(paddingBorder, direction);
    float lineOffsetCross = line->offsetCross;

    bool isRtlColumn = IsRtl(layoutDirection) && IsColumn(direction);
    if (isRtlColumn) {
        *totalOffsetCross -= lineOffsetCross + line->crossSize;
    }

    for (int i = 0; i < line->count; i++) {
        FlexItem* item = IsReverse(direction)
                             ? &line->items[line->count - 1 - i]
                             : &line->items[i];
        CalculateFlexItem(tree, item, &totalOffsetMain, *totalOffsetCross,
                          lineOffsetCross, overflowRect, c, oofPool);
    }

    if (!isRtlColumn) {
        *totalOffsetCross += lineOffsetCross + line->crossSize;
    }
}

RectF FinalLayoutPass(TaffyTree* tree, Vec<FlexLine>* lines,
                      const AlgoConstants& c, Vec<OofCandidate>* oofPool) {
    float totalOffsetCross =
        (c.isColumn && IsRtl(c.layoutDirection))
            ? c.containerSize.w - CrossEnd(c.contentBoxInset, c.dir)
            : CrossStart(c.contentBoxInset, c.dir);

    RectF overflowRect = RectF::Zero();

    for (int i = 0; i < lines->len; i++) {
        FlexLine& line =
            c.isWrapReverse ? (*lines)[lines->len - 1 - i] : (*lines)[i];
        CalculateLayoutLine(tree, &line, &totalOffsetCross, &overflowRect, c,
                            oofPool);
    }

    if (c.isScrollContainer) {
        overflowRect.right +=
            IsRtl(c.layoutDirection)
                ? c.contentBoxInset.left - c.border.left - c.scrollbarGutter.x
                : c.contentBoxInset.right - c.border.right -
                      c.scrollbarGutter.x;
        overflowRect.bottom +=
            c.contentBoxInset.bottom - c.border.bottom - c.scrollbarGutter.y;
    }

    return overflowRect;
}

void CollectOofCandidates(TaffyTree* tree, NodeId node, const AlgoConstants& c,
                          Vec<FlexLine>* flexLines, Vec<OofCandidate>* pool,
                          OofCandidates* candidates) {
    int lineIx = 0;
    int itemIx = 0;
    auto nextWithCandidates = [&]() -> FlexItem* {
        while (lineIx < len(*flexLines)) {
            FlexLine& line = (*flexLines)[lineIx];
            while (itemIx < line.count) {
                FlexItem* item = &line.items[itemIx];
                itemIx += 1;
                if (item->oofCount > 0) {
                    return item;
                }
            }
            lineIx += 1;
            itemIx = 0;
        }
        return nullptr;
    };
    FlexItem* next = nextWithCandidates();

    FlexDirection dir = c.dir;
    bool mainIsRtl = c.isRow && IsRtl(c.layoutDirection);
    bool crossIsRtl = !c.isRow && IsRtl(c.layoutDirection);
    bool mainReversed = IsReverse(dir) != mainIsRtl;
    bool crossReversed = c.isWrapReverse != crossIsRtl;

    float mainStart = MainStart(c.contentBoxInset, dir);
    float mainEnd =
        Main(c.containerSize, dir) - MainEnd(c.contentBoxInset, dir);
    float crossStart = CrossStart(c.contentBoxInset, dir);
    float crossEnd =
        Cross(c.containerSize, dir) - CrossEnd(c.contentBoxInset, dir);

    auto crossEdge = [&](AlignItemsKeyword keyword) {
        bool startPosition = true;
        if (keyword == AlignItemsKeyword::Start ||
            keyword == AlignItemsKeyword::Baseline) {
            startPosition = !crossIsRtl;
        } else if (keyword == AlignItemsKeyword::End) {
            startPosition = crossIsRtl;
        }
        if (keyword == AlignItemsKeyword::Start ||
            keyword == AlignItemsKeyword::End ||
            keyword == AlignItemsKeyword::Baseline) {
            return startPosition ? AxisStaticEdge::Start : AxisStaticEdge::End;
        }
        if (keyword == AlignItemsKeyword::Center) {
            return AxisStaticEdge::Center;
        }
        if (keyword == AlignItemsKeyword::FlexEnd) {
            return crossReversed ? AxisStaticEdge::Start : AxisStaticEdge::End;
        }

        return crossReversed ? AxisStaticEdge::End : AxisStaticEdge::Start;
    };

    int n = tree->ChildCount(node);
    for (int order = 0; order < n; order++) {
        NodeId child = tree->GetChildId(node, order);
        const Style& cs = tree->GetStyle(child);
        if (cs.BoxGenMode() == BoxGenerationMode::None) {
            continue;
        }
        if (!IsOutOfFlow(cs.position)) {
            if (next && next->order == (uint32_t)order) {
                OofCandRange range = {next->oofStart, next->oofCount};
                OofAppendPoolRange(candidates, *pool, range);
                next = nextWithCandidates();
            }
            continue;
        }

        AlignItems alignSelf = ResolveSelfRelative(
            ResolveNormalAlignment(cs.alignSelf.UnwrapOr(c.alignItems)),
            cs.direction, c.layoutDirection, c.isColumn);

        AlignContentKeyword mainKeyword = c.justifyContent.Keyword();
        bool mainStartPosition = true;
        if (mainKeyword == AlignContentKeyword::Start) {
            mainStartPosition = !mainIsRtl;
        } else if (mainKeyword == AlignContentKeyword::End) {
            mainStartPosition = mainIsRtl;
        }
        AxisStaticEdge mainEdge = AxisStaticEdge::Center;
        switch (mainKeyword) {
            case AlignContentKeyword::SpaceBetween:
            case AlignContentKeyword::Normal:
            case AlignContentKeyword::Stretch:
            case AlignContentKeyword::FlexStart:
                mainEdge =
                    mainReversed ? AxisStaticEdge::End : AxisStaticEdge::Start;
                break;
            case AlignContentKeyword::FlexEnd:
                mainEdge =
                    mainReversed ? AxisStaticEdge::Start : AxisStaticEdge::End;
                break;
            case AlignContentKeyword::Start:
            case AlignContentKeyword::End:
                mainEdge = mainStartPosition ? AxisStaticEdge::Start
                                             : AxisStaticEdge::End;
                break;
            default:
                mainEdge = AxisStaticEdge::Center;
                break;
        }

        AxisStaticAlign crossAlign;
        crossAlign.keyword = crossEdge(alignSelf.keyword);
        crossAlign.safety = alignSelf.safety;
        crossAlign
            .fallback = crossEdge(ResolveSelfAlignmentSafety(alignSelf, true));
        AxisStaticPosition mainPos;
        mainPos.start = mainStart;
        mainPos.end = mainEnd;
        mainPos.align = AxisStaticAlign::FromKeyword(mainEdge);
        AxisStaticPosition crossPos;
        crossPos.start = crossStart;
        crossPos.end = crossEnd;
        crossPos.align = crossAlign;

        OofCandidate candidate;
        candidate.node = child;
        candidate.order = (uint32_t)order;
        candidate.position = cs.position;
        if (c.isRow) {
            candidate.staticX = mainPos;
            candidate.staticY = crossPos;
        } else {
            candidate.staticX = crossPos;
            candidate.staticY = mainPos;
        }
        candidates->Push(candidate);
    }
}

LayoutOutput ComputePreliminary(TaffyTree* tree, NodeId node,
                                const LayoutInput& inputs) {
    SizeFOpt knownDimensions = inputs.knownDimensions;
    SizeFOpt parentSize = inputs.parentSize;
    RunMode runMode = inputs.runMode;

    AlgoConstants constants = ComputeConstants(
        tree, tree->GetStyle(node), knownDimensions,
        inputs.knownDimensionsAreDefiniteW, inputs.knownDimensionsAreDefiniteH,
        parentSize, inputs.availableSpace);

    Vec<FlexItem> flexItems;

    FlexLine lineBuf[2];
    Vec<FlexLine> flexLines;
    VecUseExternalBuffer(flexLines, lineBuf);

    GenerateAnonymousFlexItems(tree, node, constants, &flexItems);

    SizeAvail availableSpace = DetermineAvailableSpace(
        knownDimensions, inputs.availableSpace, constants);

    DetermineFlexBaseSize(tree, constants, availableSpace, flexItems.els,
                          len(flexItems));

    if (constants.isBalance) {
        CollectBalancedFlexLines(constants, availableSpace, &flexItems,
                                 &flexLines);
    } else {
        CollectFlexLines(constants, availableSpace, &flexItems, &flexLines);
    }

    Optf innerMainKnown = Main(constants.nodeInnerSize, constants.dir);
    if (IsSome(innerMainKnown)) {
        float outerMainSize =
            innerMainKnown +
            MainAxisSum(constants.contentBoxInset, constants.dir);
        SetMain(&constants.innerContainerSize, constants.dir, innerMainKnown);
        SetMain(&constants.containerSize, constants.dir, outerMainSize);
    } else {
        DetermineContainerMainSize(tree, availableSpace, &flexLines,
                                   &constants);
        SetMain(&constants.nodeInnerSize, constants.dir,
                Some(Main(constants.innerContainerSize, constants.dir)));
        SetMain(&constants.nodeOuterSize, constants.dir,
                Some(Main(constants.containerSize, constants.dir)));

        const Style& style = tree->GetStyle(node);
        float innerContainerSize =
            Main(constants.innerContainerSize, constants.dir);
        SizeF resolvedGap =
            style.gap.ResolveOrZero(Some(innerContainerSize), tree->calc);
        SetMain(&constants.gap, constants.dir,
                Main(resolvedGap, constants.dir));
    }

    if (runMode == RunMode::ComputeSize) {
        bool mainIsRequested =
            (inputs.axis == RequestedAxis::Horizontal && constants.isRow) ||
            (inputs.axis == RequestedAxis::Vertical && !constants.isRow);
        if (mainIsRequested) {
            SizeF size = SizeF::Zero();
            SetMain(&size, constants.dir,
                    Main(constants.containerSize, constants.dir));
            VecReset(flexItems);
            VecReset(flexLines);
            return LayoutOutput::FromOuterSize(size);
        }
    }

    for (int i = 0; i < len(flexLines); i++) {
        ResolveFlexibleLengths(&flexLines[i], constants);
    }

    for (int i = 0; i < len(flexLines); i++) {
        DetermineHypotheticalCrossSize(tree, &flexLines[i], constants,
                                       availableSpace);
    }

    CalculateChildrenBaseLines(tree, knownDimensions, availableSpace,
                               &flexLines, constants);

    CalculateCrossSize(&flexLines, knownDimensions, constants);

    HandleAlignContentStretch(&flexLines, knownDimensions, constants);

    DetermineUsedCrossSize(tree, &flexLines, constants);

    DistributeRemainingFreeSpace(&flexLines, constants);

    ResolveCrossAxisAutoMargins(&flexLines, constants);

    float totalLineCrossSize =
        DetermineContainerCrossSize(&flexLines, knownDimensions, &constants);

    if (runMode == RunMode::ComputeSize) {
        VecReset(flexItems);
        VecReset(flexLines);
        return LayoutOutput::FromOuterSize(constants.containerSize);
    }

    AlignFlexLinesPerAlignContent(&flexLines, constants, totalLineCrossSize);

    Vec<OofCandidate> oofPool;
    RectF inflowOverflowRect =
        FinalLayoutPass(tree, &flexLines, constants, &oofPool);

    OofCandidates candidates;
    CollectOofCandidates(tree, node, constants, &flexLines, &oofPool,
                         &candidates);

    int nChildren = tree->ChildCount(node);
    for (int order = 0; order < nChildren; order++) {
        NodeId child = tree->GetChildId(node, order);
        if (tree->GetStyle(child).BoxGenMode() == BoxGenerationMode::None) {
            tree->SetUnroundedLayout(child, Layout::WithOrder((uint32_t)order));
            tree->PerformChildLayout(
                child, SizeFOptNone(), SizeFOptNone(), SizeAvail::MaxContent(),
                SizingMode::ContentSize, LineBool::False());
        }
    }

    Optf firstVerticalBaseline = None();
    int firstLineIdx = constants.isWrapReverse ? len(flexLines) - 1 : 0;
    if (firstLineIdx >= 0 && flexLines[firstLineIdx].count > 0) {
        FlexLine& firstLine = flexLines[firstLineIdx];

        const FlexItem* chosen = nullptr;
        if (constants.isColumn) {
            chosen = IsReverse(constants.dir)
                         ? &firstLine.items[firstLine.count - 1]
                         : &firstLine.items[0];
        } else {
            for (int i = 0; i < firstLine.count; i++) {
                const FlexItem& item = firstLine.items[i];
                if (item.ParticipatesInBaselineAlignment(constants.dir)) {
                    chosen = &item;
                    break;
                }
            }
            if (!chosen) {
                chosen = &firstLine.items[0];
            }
        }
        firstVerticalBaseline = Some(chosen->baseline);
    }

    RectF areaInset = constants.border;
    if (IsRtl(constants.layoutDirection)) {
        areaInset.left += constants.scrollbarGutter.x;
    } else {
        areaInset.right += constants.scrollbarGutter.x;
    }
    areaInset.bottom += constants.scrollbarGutter.y;

    VecReset(flexItems);
    VecReset(flexLines);
    VecReset(oofPool);

    LayoutOutput output = LayoutOutput::FromSizesAndBaselines(
        constants.containerSize, inflowOverflowRect,
        BaselinesFromFirst(firstVerticalBaseline));
    output.oofCandidates = candidates;
    output.hasOofPositioningArea = true;
    output.oofPositioningArea.size = constants.containerSize - areaInset
                                                                   .SumAxes();
    output.oofPositioningArea.offset = {areaInset.left, areaInset.top};
    return output;
}

}

LayoutOutput ComputeFlexboxLayout(TaffyTree* tree, NodeId node,
                                  const LayoutInput& inputs) {
    CalcResolver calc = tree->calc;
    SizeFOpt knownDimensions = inputs.knownDimensions;
    SizeFOpt parentSize = inputs.parentSize;
    RunMode runMode = inputs.runMode;
    const Style& style = tree->GetStyle(node);
    Contain contain = style.contain;

    Optf aspectRatio = style.aspectRatio;
    RectF padding = style.padding.ResolveOrZero(parentSize.w, calc);
    RectF border = style.border.ResolveOrZero(parentSize.w, calc);
    SizeF paddingBorderSum = padding.SumAxes() + border.SumAxes();
    SizeF boxSizingAdjustment = style.boxSizing == BoxSizing::ContentBox
                                    ? paddingBorderSum
                                    : SizeF::Zero();

    SizeFOpt minSize =
        MaybeAdd(MaybeApplyAspectRatio(
                     style.minSize.MaybeResolve(parentSize, calc), aspectRatio),
                 boxSizingAdjustment);
    SizeFOpt maxSize =
        MaybeAdd(MaybeApplyAspectRatio(
                     style.maxSize.MaybeResolve(parentSize, calc), aspectRatio),
                 boxSizingAdjustment);
    SizeFOpt clampedStyleSize = SizeFOptNone();
    if (inputs.sizingMode == SizingMode::InherentSize) {
        clampedStyleSize = MaybeClamp(
            MaybeAdd(
                MaybeApplyAspectRatio(style.size.MaybeResolve(parentSize, calc),
                                      aspectRatio),
                boxSizingAdjustment),
            minSize, maxSize);
    }

    SizeFOpt minMaxDefiniteSize = SizeFOptNone();
    if (IsSome(minSize.w) && IsSome(maxSize.w) && maxSize.w <= minSize.w) {
        minMaxDefiniteSize.w = minSize.w;
    }
    if (IsSome(minSize.h) && IsSome(maxSize.h) && maxSize.h <= minSize.h) {
        minMaxDefiniteSize.h = minSize.h;
    }

    SizeFOpt styledBasedKnownDimensions = Or(
        knownDimensions,
        MaybeMax(Or(minMaxDefiniteSize, clampedStyleSize), paddingBorderSum));

    if (runMode == RunMode::ComputeSize) {
        if (BothAxisDefined(styledBasedKnownDimensions)) {
            return LayoutOutput::FromOuterSize(
                {styledBasedKnownDimensions.w, styledBasedKnownDimensions.h});
        }
        if (inputs.axis == RequestedAxis::Horizontal &&
            IsSome(styledBasedKnownDimensions.w)) {
            return LayoutOutput::FromOuterSize(
                {styledBasedKnownDimensions.w, 0.0f});
        }
    }

    LayoutInput next = inputs;
    next.knownDimensions = styledBasedKnownDimensions;
    next.knownDimensionsAreDefiniteW =
        inputs.knownDimensionsAreDefiniteW || !IsSome(knownDimensions.w);
    next.knownDimensionsAreDefiniteH =
        inputs.knownDimensionsAreDefiniteH || !IsSome(knownDimensions.h);
    LayoutOutput out = ComputePreliminary(tree, node, next);

    if (contain.SuppressesBaseline()) {
        out.baselines = BaselinesFromFirst(None());
    }
    return out;
}

}

#line 1 "src/taffy/compute_grid.cpp"

namespace taffy {
namespace {

struct TrackCounts {
    uint16_t negativeImplicit = 0;
    uint16_t explicitCount = 0;
    uint16_t positiveImplicit = 0;

    static TrackCounts FromRaw(uint16_t neg, uint16_t exp, uint16_t pos) {
        return {neg, exp, pos};
    }
    int Len() const {
        return (int)negativeImplicit + (int)explicitCount +
               (int)positiveImplicit;
    }
    OriginZeroLine ImplicitStartLine() const {
        return OriginZeroLine{(int16_t)(-(int16_t)negativeImplicit)};
    }
    OriginZeroLine ImplicitEndLine() const {
        return OriginZeroLine{(int16_t)(explicitCount + positiveImplicit)};
    }

    int16_t OzLineToNextTrack(OriginZeroLine l) const {
        return (int16_t)(l.v + (int16_t)negativeImplicit);
    }

    OriginZeroLine TrackToPrevOzLine(uint16_t index) const {
        return OriginZeroLine{
            (int16_t)((int16_t)index - (int16_t)negativeImplicit)};
    }
};

int IntoTrackVecIndex(OriginZeroLine l, TrackCounts counts) {
    return 2 * (int)(l.v + (int16_t)counts.negativeImplicit);
}

bool TryIntoTrackVecIndex(OriginZeroLine l, TrackCounts counts, int* out) {
    if (l.v < -(int16_t)counts.negativeImplicit) {
        return false;
    }
    if (l.v > (int16_t)(counts.explicitCount + counts.positiveImplicit)) {
        return false;
    }
    *out = 2 * (int)(l.v + (int16_t)counts.negativeImplicit);
    return true;
}

struct LineU16 {
    uint16_t start = 0;
    uint16_t end = 0;
};

enum class GridTrackKind : uint8_t {
    Track,
    Gutter
};

struct GridTrack {
    GridTrackKind kind = GridTrackKind::Track;

    bool isCollapsed = false;
    MinTrackSizingFunction minTrackSizingFunction;
    MaxTrackSizingFunction maxTrackSizingFunction;

    float offset = 0.0f;
    float baseSize = 0.0f;

    float growthLimit = 0.0f;

    float contentAlignmentAdjustment = 0.0f;

    float itemIncurredIncrease = 0.0f;
    float baseSizePlannedIncrease = 0.0f;
    float growthLimitPlannedIncrease = 0.0f;

    bool infinitelyGrowable = false;

    float fitContentLimit = INFINITY;

    static GridTrack New(MinTrackSizingFunction mn, MaxTrackSizingFunction mx) {
        GridTrack t;
        t.kind = GridTrackKind::Track;
        t.minTrackSizingFunction = mn;
        t.maxTrackSizingFunction = mx;
        return t;
    }
    static GridTrack Gutter(LengthPercentage size) {
        GridTrack t;
        t.kind = GridTrackKind::Gutter;
        t.minTrackSizingFunction = MinTrackSizingFunction::From(size);
        t.maxTrackSizingFunction = MaxTrackSizingFunction::From(size);
        return t;
    }
    void Collapse() {
        isCollapsed = true;
        minTrackSizingFunction = MinTrackSizingFunction::Zero();
        maxTrackSizingFunction = MaxTrackSizingFunction::Zero();
        fitContentLimit = INFINITY;
    }
    bool IsFlexible() const { return maxTrackSizingFunction.IsFr(); }
    bool UsesPercentage() const {
        return minTrackSizingFunction.UsesPercentage() || maxTrackSizingFunction
                                                              .UsesPercentage();
    }
    bool HasIntrinsicSizingFunction() const {
        return minTrackSizingFunction.raw.IsIntrinsic() ||
               maxTrackSizingFunction.IsIntrinsic();
    }
    void ResolveFitContentLimit(Optf axisInnerNodeSize, CalcResolver calc) {
        fitContentLimit = UnwrapOr(
            maxTrackSizingFunction.FitContentLimit(axisInnerNodeSize, calc),
            INFINITY);
    }
    float FitContentLimit() const { return fitContentLimit; }
    float FitContentLimitedGrowthLimit() const {
        return F32Min(growthLimit, fitContentLimit);
    }
    float FlexFactor() const {
        return maxTrackSizingFunction.IsFr()
                   ? maxTrackSizingFunction.raw.Value()
                   : 0.0f;
    }
};

enum class CellOccupancyState : uint8_t {
    Unoccupied,
    DefinitelyPlaced,
    AutoPlaced
};

struct SparseCursor {
    bool set = false;
    int16_t line = 0;
};

struct CellOccupancyMatrix {
    Vec<uint8_t> inner;
    Vec<SparseCursor> rowCursor;
    Vec<SparseCursor> colCursor;
    int nRows = 0;
    int nCols = 0;
    TrackCounts columns;
    TrackCounts rows;

    static void ResetCursors(Vec<SparseCursor>& cursors, int count) {
        VecReset(cursors);
        if (count <= 0) {
            return;
        }
        SparseCursor* p = VecAppendBlanks(cursors, count);
        if (p) {

            for (int i = 0; i < count; i++) {
                p[i] = {};
            }
        }
    }

    static void ShiftCursors(Vec<SparseCursor>& cursors, int oldCount,
                             int reqNeg, int reqPos) {
        if (reqNeg == 0 && reqPos == 0) {
            return;
        }
        int newCount = oldCount + reqNeg + reqPos;
        Vec<SparseCursor> next;
        SparseCursor* p =
            newCount > 0 ? VecAppendBlanks(next, newCount) : nullptr;
        if (newCount > 0 && !p) {
            return;
        }
        if (p) {
            for (int i = 0; i < newCount; i++) {
                p[i] = {};
            }
            if (oldCount > 0 && cursors.els) {
                for (int i = 0; i < oldCount; i++) {
                    p[reqNeg + i] = cursors.els[i];
                }
            }
        }
        VecReset(cursors);
        cursors = next;
        next.els = nullptr;
        next.len = 0;
        next.cap = 0;
    }

    void Init(TrackCounts cols, TrackCounts rws) {
        columns = cols;
        rows = rws;
        nRows = rws.Len();
        nCols = cols.Len();
        VecReset(inner);
        int n = nRows * nCols;
        if (n > 0) {
            uint8_t* p = VecAppendBlanks(inner, n);
            if (p) {
                memset(p, 0, (size_t)n);
            }
        }
        ResetCursors(rowCursor, nRows);
        ResetCursors(colCursor, nCols);
    }
    void Free() {
        VecReset(inner);
        VecReset(rowCursor);
        VecReset(colCursor);
    }

    CellOccupancyState Get(int row, int col) const {
        if (row < 0 || row >= nRows || col < 0 || col >= nCols) {
            return CellOccupancyState::Unoccupied;
        }
        return (CellOccupancyState)inner[row * nCols + col];
    }
    void Set(int row, int col, CellOccupancyState v) {
        if (row < 0 || row >= nRows || col < 0 || col >= nCols) {
            return;
        }
        inner[row * nCols + col] = (uint8_t)v;
    }
    const TrackCounts& Counts(AbsoluteAxis a) const {
        return a == AbsoluteAxis::Horizontal ? columns : rows;
    }

    bool IsAreaInRange(AbsoluteAxis primaryAxis, int primaryStart,
                       int primaryEnd, int secondaryStart,
                       int secondaryEnd) const {
        if (primaryStart < 0 || primaryEnd > Counts(primaryAxis).Len()) {
            return false;
        }
        if (secondaryStart < 0 || secondaryEnd > Counts(OtherAxis(primaryAxis))
                                                     .Len()) {
            return false;
        }
        return true;
    }

    void ExpandToFitRange(int rowStart, int rowEnd, int colStart, int colEnd) {
        int reqNegRows = rowStart < 0 ? -rowStart : 0;
        int reqPosRows = rowEnd - rows.Len() > 0 ? rowEnd - rows.Len() : 0;
        int reqNegCols = colStart < 0 ? -colStart : 0;
        int reqPosCols =
            colEnd - columns.Len() > 0 ? colEnd - columns.Len() : 0;

        int oldRowCount = nRows;
        int oldColCount = nCols;
        int newRowCount = oldRowCount + reqNegRows + reqPosRows;
        int newColCount = oldColCount + reqNegCols + reqPosCols;

        Vec<uint8_t> data;
        uint8_t* p = VecAppendBlanks(data, newRowCount * newColCount);
        if (!p) {
            return;
        }
        memset(p, 0, (size_t)newRowCount * (size_t)newColCount);
        for (int row = 0; row < oldRowCount; row++) {
            for (int col = 0; col < oldColCount; col++) {
                p[(row + reqNegRows) * newColCount + (col + reqNegCols)] =
                    inner[row * nCols + col];
            }
        }
        VecReset(inner);
        inner = data;
        data.els = nullptr;
        data.len = 0;
        data.cap = 0;

        nRows = newRowCount;
        nCols = newColCount;
        rows.negativeImplicit = (uint16_t)(rows.negativeImplicit + reqNegRows);
        rows.positiveImplicit = (uint16_t)(rows.positiveImplicit + reqPosRows);
        columns.negativeImplicit =
            (uint16_t)(columns.negativeImplicit + reqNegCols);
        columns.positiveImplicit =
            (uint16_t)(columns.positiveImplicit + reqPosCols);
        ShiftCursors(rowCursor, oldRowCount, reqNegRows, reqPosRows);
        ShiftCursors(colCursor, oldColCount, reqNegCols, reqPosCols);
    }

    void AdvanceAutoPlacementCursor(AbsoluteAxis primaryAxis,
                                    OriginZeroLine secondaryStart,
                                    OriginZeroLine primaryEnd) {
        int trackIndex = Counts(OtherAxis(primaryAxis))
                             .OzLineToNextTrack(secondaryStart);
        Vec<SparseCursor>& tracks =
            primaryAxis == AbsoluteAxis::Horizontal ? rowCursor : colCursor;
        if (trackIndex < 0 || trackIndex >= len(tracks)) {
            return;
        }
        SparseCursor& cursor = tracks.els[trackIndex];
        if (!cursor.set || primaryEnd.v > cursor.line) {
            cursor.set = true;
            cursor.line = primaryEnd.v;
        }
    }

    OptOriginZeroLine AutoPlacementCursor(AbsoluteAxis trackType,
                                          OriginZeroLine startAt) const {
        int idx = Counts(OtherAxis(trackType)).OzLineToNextTrack(startAt);
        const Vec<SparseCursor>& tracks =
            trackType == AbsoluteAxis::Horizontal ? rowCursor : colCursor;
        if (idx < 0 || idx >= len(tracks) || !tracks.els[idx].set) {
            return OptOriginZeroLine();
        }
        return OptOriginZeroLine(OriginZeroLine{tracks.els[idx].line});
    }

    void MarkAreaAs(AbsoluteAxis primaryAxis, LineOzl primarySpan,
                    LineOzl secondarySpan, CellOccupancyState value) {
        LineOzl rowSpan = primaryAxis == AbsoluteAxis::Horizontal
                              ? secondarySpan
                              : primarySpan;
        LineOzl colSpan = primaryAxis == AbsoluteAxis::Horizontal
                              ? primarySpan
                              : secondarySpan;

        int colStart = columns.OzLineToNextTrack(colSpan.start);
        int colEnd = columns.OzLineToNextTrack(colSpan.end);
        int rowStart = rows.OzLineToNextTrack(rowSpan.start);
        int rowEnd = rows.OzLineToNextTrack(rowSpan.end);

        if (!IsAreaInRange(AbsoluteAxis::Horizontal, colStart, colEnd, rowStart,
                           rowEnd)) {
            ExpandToFitRange(rowStart, rowEnd, colStart, colEnd);
            colStart = columns.OzLineToNextTrack(colSpan.start);
            colEnd = columns.OzLineToNextTrack(colSpan.end);
            rowStart = rows.OzLineToNextTrack(rowSpan.start);
            rowEnd = rows.OzLineToNextTrack(rowSpan.end);
        }

        if (value == CellOccupancyState::AutoPlaced &&
            primarySpan.start < primarySpan.end &&
            secondarySpan.start < secondarySpan.end) {
            AdvanceAutoPlacementCursor(primaryAxis, secondarySpan.start,
                                       primarySpan.end);
        }

        for (int x = rowStart; x < rowEnd; x++) {
            for (int y = colStart; y < colEnd; y++) {
                Set(x, y, value);
            }
        }
    }

    bool TrackAreaIsUnoccupied(AbsoluteAxis primaryAxis, int primaryStart,
                               int primaryEnd, int secondaryStart,
                               int secondaryEnd) const {
        int rowStart = primaryAxis == AbsoluteAxis::Horizontal ? secondaryStart
                                                               : primaryStart;
        int rowEnd =
            primaryAxis == AbsoluteAxis::Horizontal ? secondaryEnd : primaryEnd;
        int colStart = primaryAxis == AbsoluteAxis::Horizontal ? primaryStart
                                                               : secondaryStart;
        int colEnd =
            primaryAxis == AbsoluteAxis::Horizontal ? primaryEnd : secondaryEnd;

        for (int x = rowStart; x < rowEnd; x++) {
            for (int y = colStart; y < colEnd; y++) {
                if (Get(x, y) != CellOccupancyState::Unoccupied) {
                    return false;
                }
            }
        }
        return true;
    }

    bool LineAreaIsUnoccupied(AbsoluteAxis primaryAxis, LineOzl primarySpan,
                              LineOzl secondarySpan) const {
        const TrackCounts& pc = Counts(primaryAxis);
        const TrackCounts& sc = Counts(OtherAxis(primaryAxis));
        return TrackAreaIsUnoccupied(primaryAxis,
                                     pc.OzLineToNextTrack(primarySpan.start),
                                     pc.OzLineToNextTrack(primarySpan.end),
                                     sc.OzLineToNextTrack(secondarySpan.start),
                                     sc.OzLineToNextTrack(secondarySpan.end));
    }

    OptOriginZeroLine LineAreaCollisionJump(AbsoluteAxis primaryAxis,
                                            LineOzl primarySpan,
                                            LineOzl secondarySpan) const {
        const TrackCounts& pc = Counts(primaryAxis);
        const TrackCounts& sc = Counts(OtherAxis(primaryAxis));
        int primaryStart = pc.OzLineToNextTrack(primarySpan.start);
        int primaryEnd = pc.OzLineToNextTrack(primarySpan.end);
        int secondaryStart = sc.OzLineToNextTrack(secondarySpan.start);
        int secondaryEnd = sc.OzLineToNextTrack(secondarySpan.end);
        int primaryLen = pc.Len();
        int secondaryLen = sc.Len();
        primaryStart = primaryStart < 0 ? 0 : primaryStart;
        primaryEnd = primaryEnd > primaryLen ? primaryLen : primaryEnd;
        secondaryStart = secondaryStart < 0 ? 0 : secondaryStart;
        secondaryEnd =
            secondaryEnd > secondaryLen ? secondaryLen : secondaryEnd;

        bool found = false;
        int best = 0;
        for (int secondary = secondaryStart; secondary < secondaryEnd;
             secondary++) {
            for (int primary = primaryStart; primary < primaryEnd; primary++) {
                int row = primaryAxis == AbsoluteAxis::Horizontal ? secondary
                                                                  : primary;
                int col = primaryAxis == AbsoluteAxis::Horizontal ? primary
                                                                  : secondary;
                if (Get(row, col) == CellOccupancyState::Unoccupied) {
                    continue;
                }

                int extent = primary;
                while (extent + 1 < primaryLen) {
                    int r = primaryAxis == AbsoluteAxis::Horizontal
                                ? secondary
                                : extent + 1;
                    int c = primaryAxis == AbsoluteAxis::Horizontal ? extent + 1
                                                                    : secondary;
                    if (Get(r, c) == CellOccupancyState::Unoccupied) {
                        break;
                    }
                    extent++;
                }
                best = !found || extent > best ? extent : best;
                found = true;
            }
        }
        if (!found) {
            return OptOriginZeroLine();
        }
        OriginZeroLine line = pc.TrackToPrevOzLine((uint16_t)best);
        int32_t next = (int32_t)line.v + 1;
        next = next < INT16_MIN   ? INT16_MIN
               : next > INT16_MAX ? INT16_MAX
                                  : next;
        return OptOriginZeroLine(OriginZeroLine{(int16_t)next});
    }

    bool RowIsOccupied(int rowIndex) const {
        if (rowIndex < 0 || rowIndex >= nRows) {
            return false;
        }
        for (int c = 0; c < nCols; c++) {
            if (Get(rowIndex, c) != CellOccupancyState::Unoccupied) {
                return true;
            }
        }
        return false;
    }
    bool ColumnIsOccupied(int columnIndex) const {
        if (columnIndex < 0 || columnIndex >= nCols) {
            return false;
        }
        for (int r = 0; r < nRows; r++) {
            if (Get(r, columnIndex) != CellOccupancyState::Unoccupied) {
                return true;
            }
        }
        return false;
    }
};

struct GridItem {
    NodeId node;

    uint16_t sourceOrder = 0;

    LineOzl row;
    LineOzl column;

    bool isCompressibleReplaced = false;

    bool isReplaced = false;
    PointOverflow overflow;
    BoxSizing boxSizing = BoxSizing::BorderBox;
    SizeDim size;
    SizeDim minSize;
    SizeDim maxSize;
    Optf aspectRatio = None();
    RectLp padding;
    RectLp border;
    RectLpa margin;
    AlignSelf alignSelf;
    AlignSelf justifySelf;
    Optf baseline = None();

    float baselineShim = 0.0f;

    bool IsStretchAligned(AlignSelf alignment) const {
        if (alignment.keyword == AlignItemsKeyword::Stretch) {
            return true;
        }
        return alignment.keyword == AlignItemsKeyword::Normal && !isReplaced;
    }

    LineU16 rowIndexes;
    LineU16 columnIndexes;

    bool crossesFlexibleRow = false;
    bool crossesFlexibleColumn = false;
    bool crossesIntrinsicRow = false;
    bool crossesIntrinsicColumn = false;

    SizeFOpt gridAreaSizeCache = SizeFOptNone();
    bool hasGridAreaSizeCache = false;
    SizeFOpt minContentContributionCache = SizeFOptNone();
    SizeFOpt minimumContributionCache = SizeFOptNone();
    SizeFOpt maxContentContributionCache = SizeFOptNone();

    float yPosition = 0.0f;
    float height = 0.0f;

    int oofStart = 0;
    int oofCount = 0;

    bool HasAutoBlockMargin() const {
        return margin.top.IsAuto() || margin.bottom.IsAuto();
    }

    bool HasCyclicBlockSizeDependency() const {
        return size.height.raw.UsesPercentage() &&
               (crossesIntrinsicRow || crossesFlexibleRow);
    }
    bool ParticipatesInBaselineAlignment() const {
        return alignSelf.keyword == AlignItemsKeyword::Baseline &&
               !HasAutoBlockMargin() && !HasCyclicBlockSizeDependency();
    }

    LineOzl Placement(AbstractAxis axis) const {
        return axis == AbstractAxis::Block ? row : column;
    }
    LineU16 PlacementIndexes(AbstractAxis axis) const {
        return axis == AbstractAxis::Block ? rowIndexes : columnIndexes;
    }

    int TrackRangeStart(AbstractAxis axis) const {
        return (int)PlacementIndexes(axis).start + 1;
    }
    int TrackRangeEnd(AbstractAxis axis) const {
        return (int)PlacementIndexes(axis).end;
    }

    template <typename Pred>
    bool SpansTrackMatching(AbstractAxis axis, const GridTrack* tracks,
                            Pred pred) const {
        int from = TrackRangeStart(axis);
        int end = TrackRangeEnd(axis);
        for (int k = from; k < end; k++) {
            if (pred(tracks[k])) {
                return true;
            }
        }
        return false;
    }
    uint16_t Span(AbstractAxis axis) const { return Placement(axis).Span(); }
    bool CrossesFlexibleTrack(AbstractAxis axis) const {
        return axis == AbstractAxis::Inline ? crossesFlexibleColumn
                                            : crossesFlexibleRow;
    }
    bool CrossesIntrinsicTrack(AbstractAxis axis) const {
        return axis == AbstractAxis::Inline ? crossesIntrinsicColumn
                                            : crossesIntrinsicRow;
    }
};

template <typename T, typename Less>
static void InsertionSortRange(T* items, int lo, int hi, Less less) {
    for (int i = lo + 1; i < hi; i++) {
        T key = items[i];
        int j = i - 1;
        while (j >= lo && less(key, items[j])) {
            items[j + 1] = items[j];
            j--;
        }
        items[j + 1] = key;
    }
}

template <typename T, typename Less>
static void MergeRuns(const T* src, T* dst, int lo, int mid, int hi,
                      Less less) {
    int i = lo;
    int j = mid;
    int k = lo;
    while (i < mid && j < hi) {
        if (!less(src[j], src[i])) {
            dst[k++] = src[i++];
        } else {
            dst[k++] = src[j++];
        }
    }
    while (i < mid) {
        dst[k++] = src[i++];
    }
    while (j < hi) {
        dst[k++] = src[j++];
    }
}

template <typename T, typename Less>
void StableSort(T* items, int n, Less less) {
    if (n < 2) {
        return;
    }

    const int kRun = 32;
    for (int lo = 0; lo < n; lo += kRun) {
        int hi = lo + kRun < n ? lo + kRun : n;
        InsertionSortRange(items, lo, hi, less);
    }
    if (n <= kRun) {
        return;
    }

    T* scratch = (T*)base::Alloc(nullptr, n * (int)sizeof(T));
    if (!scratch) {

        InsertionSortRange(items, 0, n, less);
        return;
    }

    T* src = items;
    T* dst = scratch;
    for (int width = kRun; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid = lo + width < n ? lo + width : n;
            int hi = lo + 2 * width < n ? lo + 2 * width : n;
            MergeRuns(src, dst, lo, mid, hi, less);
        }
        T* swap = src;
        src = dst;
        dst = swap;
    }
    if (src != items) {
        memcpy((void*)items, (const void*)src, (size_t)n * sizeof(T));
    }
    base::Free(nullptr, (void*)scratch);
}

struct LineNameEntry {
    Str name;
    Vec<uint32_t> lines;
};

struct AreaLines {
    Str name;
    uint32_t rowStart = 0;
    uint32_t rowEnd = 0;
    uint32_t colStart = 0;
    uint32_t colEnd = 0;

    uint32_t Edge(GridAreaAxis axis, GridAreaEnd end) const {
        if (axis == GridAreaAxis::Row) {
            return end == GridAreaEnd::Start ? rowStart : rowEnd;
        }
        return end == GridAreaEnd::Start ? colStart : colEnd;
    }
    uint32_t& EdgeMut(GridAreaAxis axis, GridAreaEnd end) {
        if (axis == GridAreaAxis::Row) {
            return end == GridAreaEnd::Start ? rowStart : rowEnd;
        }
        return end == GridAreaEnd::Start ? colStart : colEnd;
    }
};

bool SplitAreaEdgeName(Str name, Str* area, GridAreaEnd* end) {
    if (StrEndsWith(name, "-start") && len(name) > 6) {
        *area = Str(name.s, len(name) - 6);
        *end = GridAreaEnd::Start;
        return true;
    }
    if (StrEndsWith(name, "-end") && len(name) > 4) {
        *area = Str(name.s, len(name) - 4);
        *end = GridAreaEnd::End;
        return true;
    }
    return false;
}

struct NamedLineResolver {
    Vec<LineNameEntry> rowLines;
    Vec<LineNameEntry> columnLines;
    Vec<AreaLines> areaLines;
    uint16_t areaColumnCount = 0;
    uint16_t areaRowCount = 0;
    uint16_t explicitColumnCount = 0;
    uint16_t explicitRowCount = 0;

    void Free() {
        for (int i = 0; i < len(rowLines); i++) {
            VecReset(rowLines[i].lines);
        }
        for (int i = 0; i < len(columnLines); i++) {
            VecReset(columnLines[i].lines);
        }
        VecReset(rowLines);
        VecReset(columnLines);
        VecReset(areaLines);
    }

    AreaLines* FindArea(Str name) {
        for (int i = 0; i < len(areaLines); i++) {
            if (base::StrEq(areaLines[i].name, name)) {
                return &areaLines[i];
            }
        }
        return nullptr;
    }
    const AreaLines* FindArea(Str name) const {
        for (int i = 0; i < len(areaLines); i++) {
            if (base::StrEq(areaLines[i].name, name)) {
                return &areaLines[i];
            }
        }
        return nullptr;
    }
    uint32_t AreaEdge(Str name, GridAreaAxis axis, GridAreaEnd end) const {
        const AreaLines* area = FindArea(name);
        return area ? area->Edge(axis, end) : 0;
    }

    void Upsert(Vec<LineNameEntry>* map, GridAreaAxis axis, Str name,
                uint32_t value) {
        uint32_t priorEdge = 0;
        Str areaName;
        GridAreaEnd edgeEnd;
        if (SplitAreaEdgeName(name, &areaName, &edgeEnd)) {
            AreaLines* area = FindArea(areaName);
            if (!area) {
                AreaLines created;
                created.name = areaName;
                VecAppend(areaLines, created);
                area = &areaLines[len(areaLines) - 1];
            }
            uint32_t& edge = area->EdgeMut(axis, edgeEnd);
            priorEdge = edge;
            edge = priorEdge == 0 || value < priorEdge ? value : priorEdge;
        }

        for (int i = 0; i < map->len; i++) {
            LineNameEntry& e = (*map)[i];
            if (base::StrEq(e.name, name)) {
                for (int k = 0; k < e.lines.len; k++) {
                    if (e.lines[k] == value) {
                        return;
                    }
                }
                VecAppend(e.lines, value);
                return;
            }
        }
        LineNameEntry e;
        e.name = name;
        if (priorEdge != 0) {
            VecAppend(e.lines, priorEdge);
        }
        if (priorEdge != value) {
            VecAppend(e.lines, value);
        }
        VecAppend(*map, e);
    }

    static const Vec<uint32_t>* Find(const Vec<LineNameEntry>& map, Str name) {
        for (int i = 0; i < len(map); i++) {
            if (base::StrEq(map[i].name, name)) {
                return &map[i].lines;
            }
        }
        return nullptr;
    }

    static void SortUnique(Vec<uint32_t>& lines) {
        for (int i = 1; i < lines.len; i++) {
            uint32_t v = lines[i];
            int j = i;
            while (j > 0 && lines[j - 1] > v) {
                lines[j] = lines[j - 1];
                j--;
            }
            lines[j] = v;
        }
        int w = 0;
        for (int i = 0; i < lines.len; i++) {
            if (w == 0 || lines[w - 1] != lines[i]) {
                lines[w++] = lines[i];
            }
        }
        lines.len = w;
    }

    void Init(const Style& style, uint16_t columnAutoRepetitions,
              uint16_t rowAutoRepetitions);
    LinePlain ResolveLineNames(LinePlacement line, GridAreaAxis axis) const;
    LinePlain ResolveRowNames(LinePlacement line) const {
        return ResolveLineNames(line, GridAreaAxis::Row);
    }
    LinePlain ResolveColumnNames(LinePlacement line) const {
        return ResolveLineNames(line, GridAreaAxis::Column);
    }

    GridLine FindNamedLine(Str name, int16_t idx, GridAreaAxis axis,
                           GridAreaEnd end) const;
    OriginZeroLine LastExplicitLine(GridAreaAxis axis) const;

    OriginZeroLine NthLineForwards(const uint32_t* lines, int count,
                                   int32_t firstOz, int32_t n,
                                   GridAreaAxis axis) const;
    OriginZeroLine NthLineBackwards(const uint32_t* lines, int count,
                                    int32_t lastOz, int32_t n,
                                    GridAreaAxis axis) const;

    int CollectLines(Str name, GridAreaAxis axis, const uint32_t** data,
                     uint32_t* single) const;
};

static int32_t OzOfStored(uint32_t line) {
    return (int32_t)line - 1;
}

static OriginZeroLine ClampedOz(int32_t line) {
    if (line < (int32_t)kMinOzLine) {
        line = kMinOzLine;
    } else if (line > (int32_t)kMaxOzLine) {
        line = kMaxOzLine;
    }
    return OriginZeroLine{(int16_t)line};
}

static GridLine IntoGridLine(OriginZeroLine line, uint16_t explicitTrackCount) {
    int32_t explicitLineCount = (int32_t)explicitTrackCount + 1;
    int32_t value =
        line.v >= 0 ? (int32_t)line.v + 1 : (int32_t)line.v - explicitLineCount;
    return GridLine{(int16_t)value};
}

void NamedLineResolver::Init(const Style& style, uint16_t columnAutoRepetitions,
                             uint16_t rowAutoRepetitions) {
    areaColumnCount = style.gridTemplateAreas.columnCount;
    areaRowCount = style.gridTemplateAreas.rowCount;
    for (int i = 0; i < style.gridTemplateAreas.areas.len; i++) {
        const GridTemplateArea& area = style.gridTemplateAreas.areas[i];
        AreaLines lines;
        lines.name = area.name;
        lines.rowStart = area.rowStart;
        lines.rowEnd = area.rowEnd;
        lines.colStart = area.columnStart;
        lines.colEnd = area.columnEnd;
        VecAppend(areaLines, lines);
    }

    struct Axis {
        Slice<GridTemplateComponent> tracks;
        Slice<LineNameSet> names;
        uint16_t autoRepetitions;
        Vec<LineNameEntry>* map;
        GridAreaAxis axis;
    };
    Axis axes[2] = {{style.gridTemplateColumns, style.gridTemplateColumnNames,
                     columnAutoRepetitions, &columnLines, GridAreaAxis::Column},
                    {style.gridTemplateRows, style.gridTemplateRowNames,
                     rowAutoRepetitions, &rowLines, GridAreaAxis::Row}};

    for (const Axis& ax : axes) {
        uint32_t currentLine = 0;
        int trackIdx = 0;
        for (int i = 0; i < ax.names.len; i++) {
            currentLine += 1;
            const LineNameSet& set = ax.names[i];
            for (int k = 0; k < set.names.len; k++) {
                Upsert(ax.map, ax.axis, set.names[k], currentLine);
            }
            if (trackIdx >= ax.tracks.len) {
                continue;
            }
            const GridTemplateComponent& comp = ax.tracks[trackIdx];
            trackIdx++;
            if (!comp.isRepeat) {
                continue;
            }
            uint16_t repeatCount = comp.repeat.count.IsAuto()
                                       ? ax.autoRepetitions
                                       : comp.repeat.count.count;

            uint32_t lineNameSetCount = (uint32_t)comp.repeat.lineNames.len;
            uint32_t linesPerRep = comp.repeat.TrackCount();
            bool namesOk =
                lineNameSetCount == 0 || lineNameSetCount == linesPerRep + 1;
            for (uint16_t r = 0; r < repeatCount; r++) {
                if (namesOk) {
                    for (int s = 0; s < comp.repeat.lineNames.len; s++) {
                        uint32_t line = currentLine + (uint32_t)s;
                        const LineNameSet& ls = comp.repeat.lineNames[s];
                        for (int k = 0; k < ls.names.len; k++) {
                            Upsert(ax.map, ax.axis, ls.names[k], line);
                        }
                    }
                }
                currentLine += linesPerRep;

                if (currentLine > kMaxGridTracks) {
                    break;
                }
            }

            if (repeatCount > 0 && currentLine > 0) {
                currentLine -= 1;
            }
        }
    }

    for (int i = 0; i < len(columnLines); i++) {
        SortUnique(columnLines[i].lines);
    }
    for (int i = 0; i < len(rowLines); i++) {
        SortUnique(rowLines[i].lines);
    }
}

int NamedLineResolver::CollectLines(Str name, GridAreaAxis axis,
                                    const uint32_t** data,
                                    uint32_t* single) const {
    const Vec<LineNameEntry>& lookup =
        axis == GridAreaAxis::Row ? rowLines : columnLines;
    const Vec<uint32_t>* lines = Find(lookup, name);
    if (lines && lines->len > 0) {
        *data = lines->els;
        return lines->len;
    }
    Str areaName;
    GridAreaEnd edge;
    if (SplitAreaEdgeName(name, &areaName, &edge)) {
        uint32_t value = AreaEdge(areaName, axis, edge);
        if (value != 0) {
            *single = value;
            *data = single;
            return 1;
        }
    }
    *data = nullptr;
    return 0;
}

OriginZeroLine NamedLineResolver::LastExplicitLine(GridAreaAxis axis) const {
    uint16_t tracks =
        axis == GridAreaAxis::Row ? explicitRowCount : explicitColumnCount;
    return OriginZeroLine{(int16_t)tracks};
}

OriginZeroLine NamedLineResolver::NthLineForwards(const uint32_t* lines,
                                                  int count, int32_t firstOz,
                                                  int32_t n,
                                                  GridAreaAxis axis) const {

    int32_t first = firstOz > 0 ? firstOz : 0;
    int start = 0;
    while (start < count && OzOfStored(lines[start]) < first) {
        start++;
    }
    int kept = count - start;
    int32_t oz;
    if (n >= 1 && (n - 1) < kept) {
        oz = OzOfStored(lines[start + (int)(n - 1)]);
    } else {

        int32_t afterExplicit = (int32_t)LastExplicitLine(axis).v + 1;
        int32_t base = first > afterExplicit ? first : afterExplicit;
        oz = base + (n - kept) - 1;
    }
    return ClampedOz(oz);
}

OriginZeroLine NamedLineResolver::NthLineBackwards(const uint32_t* lines,
                                                   int count, int32_t lastOz,
                                                   int32_t n,
                                                   GridAreaAxis axis) const {
    int32_t lastExplicit = LastExplicitLine(axis).v;
    int32_t last = lastOz < lastExplicit ? lastOz : lastExplicit;
    int end = 0;
    while (end < count && OzOfStored(lines[end]) <= last) {
        end++;
    }
    int32_t oz;
    if (n >= 0 && (uint32_t)n <= (uint32_t)end) {
        oz = OzOfStored(lines[end - (int)n]);
    } else {

        int32_t base = last < -1 ? last : -1;
        oz = base - (n - end) + 1;
    }
    return ClampedOz(oz);
}

GridLine NamedLineResolver::FindNamedLine(Str name, int16_t idx,
                                          GridAreaAxis axis,
                                          GridAreaEnd end) const {
    uint16_t explicitCount =
        axis == GridAreaAxis::Row ? explicitRowCount : explicitColumnCount;

    if (idx == 0) {
        uint32_t edge = AreaEdge(name, axis, end);
        if (edge != 0) {
            return IntoGridLine(ClampedOz(OzOfStored(edge)), explicitCount);
        }
    }

    uint32_t single = 0;
    const uint32_t* data = nullptr;
    int count = CollectLines(name, axis, &data, &single);
    OriginZeroLine line;
    if (idx >= 0) {
        int32_t n = idx < 1 ? 1 : (int32_t)idx;
        line = NthLineForwards(data, count, 0, n, axis);
    } else {
        line = NthLineBackwards(data, count, (int32_t)LastExplicitLine(axis).v,
                                -(int32_t)idx, axis);
    }
    return IntoGridLine(line, explicitCount);
}

LinePlain NamedLineResolver::ResolveLineNames(LinePlacement line,
                                              GridAreaAxis axis) const {
    GridPlacement start = line.start;
    GridPlacement end = line.end;

    if (start.kind == GridPlacementKind::NamedLine) {
        start = GridPlacement::FromLineIndex(
            FindNamedLine(start.name, start.line, axis, GridAreaEnd::Start).v);
    }
    if (end.kind == GridPlacementKind::NamedLine) {
        end = GridPlacement::FromLineIndex(
            FindNamedLine(end.name, end.line, axis, GridAreaEnd::End).v);
    }

    uint16_t explicitTrackCount =
        axis == GridAreaAxis::Row ? explicitRowCount : explicitColumnCount;

    if (start.kind == GridPlacementKind::Line &&
        end.kind == GridPlacementKind::NamedSpan && start.line != 0) {
        int32_t after = (int32_t)IntoOriginZeroLine(GridLine{start.line},
                                                    explicitTrackCount)
                            .v +
                        1;
        uint32_t single = 0;
        const uint32_t* data = nullptr;
        int count = CollectLines(end.name, axis, &data, &single);
        int32_t n = end.span < 1 ? 1 : (int32_t)end.span;
        GridLine endLine = IntoGridLine(
            NthLineForwards(data, count, after, n, axis), explicitTrackCount);
        return {PlainPlacement::AtLine(start.line),
                PlainPlacement::AtLine(endLine.v)};
    }
    if (start.kind == GridPlacementKind::NamedSpan &&
        end.kind == GridPlacementKind::Line && end.line != 0) {
        int32_t before =
            (int32_t)IntoOriginZeroLine(GridLine{end.line}, explicitTrackCount)
                .v -
            1;
        uint32_t single = 0;
        const uint32_t* data = nullptr;
        int count = CollectLines(start.name, axis, &data, &single);
        int32_t n = start.span < 1 ? 1 : (int32_t)start.span;
        GridLine startLine = IntoGridLine(
            NthLineBackwards(data, count, before, n, axis), explicitTrackCount);
        return {PlainPlacement::AtLine(startLine.v),
                PlainPlacement::AtLine(end.line)};
    }

    auto plain = [](const GridPlacement& p) -> PlainPlacement {
        switch (p.kind) {
            case GridPlacementKind::Line:
                return PlainPlacement::AtLine(p.line);
            case GridPlacementKind::Span:
                return PlainPlacement::Spanning(p.span);
            case GridPlacementKind::NamedSpan:

                return PlainPlacement::Spanning(1);
            default:
                return PlainPlacement::Auto();
        }
    };
    return {plain(start), plain(end)};
}

struct MinMaxSpan {
    OriginZeroLine minLine;
    OriginZeroLine maxLine;
    uint16_t span = 0;
};

struct PlacementChild {
    int index = 0;
    NodeId node;
    LinePlain horizontal;
    LinePlain vertical;

    LinePlain Get(AbsoluteAxis a) const {
        return a == AbsoluteAxis::Horizontal ? horizontal : vertical;
    }
};

MinMaxSpan ChildMinLineMaxLineSpan(LinePlain oz) {

    OriginZeroLine t1 = oz.start.Ozl();
    OriginZeroLine t2 = oz.end.Ozl();

    MinMaxSpan out;
    if (oz.start.IsLine() && oz.end.IsLine()) {
        out.minLine = (t1 == t2) ? t1 : (t1 < t2 ? t1 : t2);
        out.maxLine = (t1 == t2) ? t1 + (uint16_t)1 : (t1 > t2 ? t1 : t2);
    } else if (oz.start.IsLine() && oz.end.IsAuto()) {
        out.minLine = t1;
        out.maxLine = t1 + (uint16_t)1;
    } else if (oz.start.IsLine() && oz.end.IsSpan()) {
        out.minLine = t1;
        out.maxLine = t1 + oz.end.span;
    } else if (oz.start.IsAuto() && oz.end.IsLine()) {

        out.minLine = t2 - (uint16_t)1;
        out.maxLine = t2;
    } else if (oz.start.IsSpan() && oz.end.IsLine()) {
        out.minLine = t2 - oz.start.span;
        out.maxLine = t2;
    } else {

        out.minLine = OriginZeroLine{0};
        out.maxLine = OriginZeroLine{0};
    }

    bool startIndefinite = oz.start.IsAuto() || oz.start.IsSpan();
    bool endIndefinite = oz.end.IsAuto() || oz.end.IsSpan();
    out.span = (startIndefinite && endIndefinite) ? oz.IndefiniteSpan() : 1;
    return out;
}

void ComputeGridSizeEstimate(uint16_t explicitColCount,
                             uint16_t explicitRowCount,
                             const PlacementChild* children, int n,
                             TrackCounts* outCols, TrackCounts* outRows) {
    OriginZeroLine colMin{0};
    OriginZeroLine colMax{0};
    uint16_t colMaxSpan = 0;
    OriginZeroLine rowMin{0};
    OriginZeroLine rowMax{0};
    uint16_t rowMaxSpan = 0;

    for (int i = 0; i < n; i++) {
        MinMaxSpan colEst = ChildMinLineMaxLineSpan(children[i].horizontal);
        MinMaxSpan rowEst = ChildMinLineMaxLineSpan(children[i].vertical);

        if (colEst.minLine < colMin) {
            colMin = colEst.minLine;
        }
        if (colEst.maxLine > colMax) {
            colMax = colEst.maxLine;
        }
        if (colEst.span > colMaxSpan) {
            colMaxSpan = colEst.span;
        }
        if (rowEst.minLine < rowMin) {
            rowMin = rowEst.minLine;
        }
        if (rowEst.maxLine > rowMax) {
            rowMax = rowEst.maxLine;
        }
        if (rowEst.span > rowMaxSpan) {
            rowMaxSpan = rowEst.span;
        }
    }

    uint16_t negCols = ImpliedNegativeImplicitTracks(colMin);
    uint16_t posCols = ImpliedPositiveImplicitTracks(colMax, explicitColCount);
    uint16_t negRows = ImpliedNegativeImplicitTracks(rowMin);
    uint16_t posRows = ImpliedPositiveImplicitTracks(rowMax, explicitRowCount);

    if ((uint16_t)(negCols + explicitColCount + posCols) < colMaxSpan) {
        posCols = (uint16_t)(colMaxSpan - explicitColCount - negCols);
    }
    if ((uint16_t)(negRows + explicitRowCount + posRows) < rowMaxSpan) {
        posRows = (uint16_t)(rowMaxSpan - explicitRowCount - negRows);
    }

    *outCols = TrackCounts::FromRaw(negCols, explicitColCount, posCols);
    *outRows = TrackCounts::FromRaw(negRows, explicitRowCount, posRows);
}

enum class AutoRepeatStrategy : uint8_t {

    MaxRepetitionsThatDoNotOverflow,

    MinRepetitionsThatDoOverflow
};

float TrackDefiniteValue(TrackSizingFunction fn, Optf parentSize,
                         CalcResolver calc) {
    Optf maxSize = fn.max.DefiniteValue(parentSize, calc);
    Optf minSize = fn.min.DefiniteValue(parentSize, calc);
    if (IsSome(maxSize)) {
        return MaybeMax(maxSize, minSize);
    }
    return UnwrapOr(minSize, 0.0f);
}

struct ExplicitGridSize {
    uint16_t autoRepetitionCount = 0;
    uint16_t trackCount = 0;
};

ExplicitGridSize ComputeExplicitGridSizeInAxis(
    const Style& style, Optf autoFitContainerSize,
    AutoRepeatStrategy autoFitStrategy, CalcResolver calc, AbsoluteAxis axis) {
    Slice<GridTemplateComponent> templ = axis == AbsoluteAxis::Horizontal
                                             ? style.gridTemplateColumns
                                             : style.gridTemplateRows;
    if (templ.len == 0) {
        return {};
    }

    for (int i = 0; i < templ.len; i++) {
        if (templ[i].isRepeat && templ[i].repeat.TrackCount() == 0) {
            return {};
        }
    }

    uint32_t nonAutoRepeatingTrackCount = 0;
    uint16_t autoRepetitionCount = 0;
    bool allTrackDefsHaveFixedComponent = true;
    for (int i = 0; i < templ.len; i++) {
        const GridTemplateComponent& c = templ[i];
        if (!c.isRepeat) {
            nonAutoRepeatingTrackCount =
                nonAutoRepeatingTrackCount < kMaxGridTracks
                    ? nonAutoRepeatingTrackCount + 1
                    : kMaxGridTracks;
            if (!c.single.HasFixedComponent()) {
                allTrackDefsHaveFixedComponent = false;
            }
            continue;
        }
        if (!c.repeat.count.IsAuto()) {
            uint64_t additional = (uint64_t)c.repeat.count.count *
                                  (uint64_t)c.repeat.TrackCount();
            nonAutoRepeatingTrackCount =
                (uint64_t)nonAutoRepeatingTrackCount + additional >
                        kMaxGridTracks
                    ? kMaxGridTracks
                    : nonAutoRepeatingTrackCount + (uint32_t)additional;
        } else {
            autoRepetitionCount += 1;
        }
        for (int k = 0; k < c.repeat.tracks.len; k++) {
            if (!c.repeat.tracks[k].HasFixedComponent()) {
                allTrackDefsHaveFixedComponent = false;
            }
        }
    }

    bool templateIsValid =
        autoRepetitionCount == 0 ||
        (autoRepetitionCount == 1 && allTrackDefsHaveFixedComponent);
    if (!templateIsValid) {
        return {};
    }

    if (autoRepetitionCount == 0) {
        return {0, (uint16_t)nonAutoRepeatingTrackCount};
    }

    const GridTemplateRepetition* repetition = nullptr;
    uint32_t autoRepeatInsertionPoint = 0;
    for (int i = 0; i < templ.len; i++) {
        if (templ[i].isRepeat && templ[i].repeat.count.IsAuto()) {
            repetition = &templ[i].repeat;
            break;
        }
        uint64_t additional = !templ[i].isRepeat
                                  ? 1u
                                  : (uint64_t)templ[i].repeat.count.count *
                                        templ[i].repeat.TrackCount();
        autoRepeatInsertionPoint =
            (uint64_t)autoRepeatInsertionPoint + additional > UINT32_MAX
                ? UINT32_MAX
                : autoRepeatInsertionPoint + (uint32_t)additional;
    }
    uint16_t repetitionTrackCount = repetition->TrackCount();
    if (repetitionTrackCount == 0) {
        return {};
    }

    uint32_t numRepetitions = 1;
    if (IsSome(autoFitContainerSize)) {
        float innerContainerSize = autoFitContainerSize;
        Optf parentSize = Some(innerContainerSize);

        float nonRepeatingTrackUsedSpace = 0.0f;
        for (int i = 0; i < templ.len; i++) {
            const GridTemplateComponent& c = templ[i];
            if (!c.isRepeat) {
                nonRepeatingTrackUsedSpace +=
                    TrackDefiniteValue(c.single, parentSize, calc);
                continue;
            }
            if (c.repeat.count.IsAuto()) {
                continue;
            }
            float sum = 0.0f;
            for (int k = 0; k < c.repeat.tracks.len; k++) {
                sum += TrackDefiniteValue(c.repeat.tracks[k], parentSize, calc);
            }
            nonRepeatingTrackUsedSpace += sum * (float)c.repeat.count.count;
        }

        SizeLp gapStyle = style.gap;
        LengthPercentage gapLp = gapStyle.GetAbs(axis);
        SizeLp asSize = {gapLp, gapLp};
        float gapSize = asSize.ResolveOrZero(Some(innerContainerSize), calc).w;

        float perRepetitionTrackUsedSpace = 0.0f;
        for (int k = 0; k < repetition->tracks.len; k++) {
            perRepetitionTrackUsedSpace +=
                TrackDefiniteValue(repetition->tracks[k], parentSize, calc);
        }

        int gapCount =
            (int)nonAutoRepeatingTrackCount + (int)repetitionTrackCount - 1;
        if (gapCount < 0) {
            gapCount = 0;
        }
        float firstRepetitionAndNonRepeatingTracksUsedSpace =
            nonRepeatingTrackUsedSpace + perRepetitionTrackUsedSpace +
            (float)gapCount * gapSize;

        if (firstRepetitionAndNonRepeatingTracksUsedSpace >
            innerContainerSize) {

            numRepetitions = 1;
        } else {
            float perRepetitionGapUsedSpace =
                (float)repetitionTrackCount * gapSize;
            float perRepetitionUsedSpace =
                perRepetitionTrackUsedSpace + perRepetitionGapUsedSpace;
            if (perRepetitionUsedSpace <= 0.0f) {
                numRepetitions = UINT32_MAX;
            } else {
                float numRepetitionThatFit =
                    (innerContainerSize -
                     firstRepetitionAndNonRepeatingTracksUsedSpace) /
                    perRepetitionUsedSpace;

                float rounded =
                    autoFitStrategy ==
                            AutoRepeatStrategy::MaxRepetitionsThatDoNotOverflow
                        ? floorf(numRepetitionThatFit)
                        : ceilf(numRepetitionThatFit);

                numRepetitions = !isfinite(rounded) || rounded >= 4294967040.0f
                                     ? UINT32_MAX
                                 : rounded < 0.0f ? 1u
                                                  : (uint32_t)rounded + 1u;
            }
        }
    }

    uint32_t remainingTracks = autoRepeatInsertionPoint >= kMaxGridTracks
                                   ? 0
                                   : kMaxGridTracks - autoRepeatInsertionPoint;
    if (remainingTracks == 0) {
        numRepetitions = 0;
    } else {
        uint32_t maxRepetitions =
            (remainingTracks + repetitionTrackCount - 1) / repetitionTrackCount;
        numRepetitions = numRepetitions < 1 ? 1 : numRepetitions;
        numRepetitions =
            numRepetitions > maxRepetitions ? maxRepetitions : numRepetitions;
    }
    uint32_t gridTemplateTrackCount =
        nonAutoRepeatingTrackCount +
        (uint32_t)repetitionTrackCount * numRepetitions;
    gridTemplateTrackCount = gridTemplateTrackCount > kMaxGridTracks
                                 ? kMaxGridTracks
                                 : gridTemplateTrackCount;
    return {(uint16_t)numRepetitions, (uint16_t)gridTemplateTrackCount};
}

template <typename NextTrack>
void CreateImplicitTracks(Vec<GridTrack>* tracks, uint16_t count,
                          NextTrack nextTrack, LengthPercentage gap) {
    for (uint16_t i = 0; i < count; i++) {
        TrackSizingFunction def = nextTrack();
        VecAppend(*tracks, GridTrack::New(def.MinSizingFunction(),
                                          def.MaxSizingFunction()));
        VecAppend(*tracks, GridTrack::Gutter(gap));
    }
}

template <typename TrackHasItems>
void InitializeGridTracks(Vec<GridTrack>* tracks, TrackCounts counts,
                          const Style& style, AbsoluteAxis axis,
                          TrackHasItems trackHasItems) {
    Slice<GridTemplateComponent> trackTemplate;
    Slice<TrackSizingFunction> autoTracks;
    LengthPercentage gap;
    if (axis == AbsoluteAxis::Horizontal) {
        trackTemplate = style.gridTemplateColumns;
        autoTracks = style.gridAutoColumns;
        gap = style.gap.width;
    } else {
        trackTemplate = style.gridTemplateRows;
        autoTracks = style.gridAutoRows;
        gap = style.gap.height;
    }

    tracks->len = 0;
    VecAppend(*tracks, GridTrack::Gutter(gap));

    int autoTrackCount = autoTracks.len;
    uint64_t nonAutoRepeatingTrackCount = 0;
    for (int i = 0; i < trackTemplate.len; i++) {
        const GridTemplateComponent& c = trackTemplate[i];
        if (!c.isRepeat) {
            nonAutoRepeatingTrackCount += 1;
        } else if (!c.repeat.count.IsAuto()) {
            nonAutoRepeatingTrackCount +=
                (uint64_t)c.repeat.count.count * c.repeat.TrackCount();
        }
    }

    if (counts.negativeImplicit > 0) {
        if (autoTrackCount == 0) {
            CreateImplicitTracks(
                tracks, counts.negativeImplicit,
                []() { return TrackSizingFunction::Auto(); }, gap);
        } else {
            int offset = autoTrackCount -
                         ((int)counts.negativeImplicit % autoTrackCount);
            int cursor = offset;
            CreateImplicitTracks(
                tracks, counts.negativeImplicit,
                [&]() {
                    TrackSizingFunction t = autoTracks[cursor % autoTrackCount];
                    cursor++;
                    return t;
                },
                gap);
        }
    }

    int currentTrackIndex = (int)counts.negativeImplicit;
    int explicitTrackLimit = (int)counts.negativeImplicit + (int)counts
                                                                .explicitCount;

    if (counts.explicitCount > 0) {
        for (int i = 0; i < trackTemplate.len; i++) {
            const GridTemplateComponent& c = trackTemplate[i];
            if (!c.isRepeat) {
                if (currentTrackIndex >= explicitTrackLimit) {
                    continue;
                }
                VecAppend(*tracks,
                          GridTrack::New(c.single.MinSizingFunction(),
                                         c.single.MaxSizingFunction()));
                VecAppend(*tracks, GridTrack::Gutter(gap));
                currentTrackIndex += 1;
                continue;
            }
            if (!c.repeat.count.IsAuto()) {
                int total = (int)c.repeat.TrackCount() * (int)c.repeat.count
                                                             .count;
                for (int k = 0;
                     k < total && currentTrackIndex < explicitTrackLimit; k++) {
                    TrackSizingFunction f =
                        c.repeat.tracks[k % c.repeat.tracks.len];
                    VecAppend(*tracks, GridTrack::New(f.MinSizingFunction(),
                                                      f.MaxSizingFunction()));
                    VecAppend(*tracks, GridTrack::Gutter(gap));
                    currentTrackIndex += 1;
                }
                continue;
            }
            uint64_t nonAutoCount = nonAutoRepeatingTrackCount > INT_MAX
                                        ? INT_MAX
                                        : nonAutoRepeatingTrackCount;
            int autoRepeatedTrackCount =
                (int)counts.explicitCount - (int)nonAutoCount;
            if (autoRepeatedTrackCount < 0) {
                autoRepeatedTrackCount = 0;
            }
            bool isAutoFit = c.repeat.count
                                 .kind == RepetitionCount::Kind::AutoFit;
            for (int k = 0; k < autoRepeatedTrackCount &&
                            currentTrackIndex < explicitTrackLimit;
                 k++) {
                TrackSizingFunction def = c.repeat
                                              .tracks[k % c.repeat.tracks.len];
                GridTrack track = GridTrack::New(def.MinSizingFunction(),
                                                 def.MaxSizingFunction());
                GridTrack gutter = GridTrack::Gutter(gap);

                if (isAutoFit && !trackHasItems(currentTrackIndex)) {
                    track.Collapse();
                    gutter.Collapse();
                }
                VecAppend(*tracks, track);
                VecAppend(*tracks, gutter);
                currentTrackIndex += 1;
            }

            bool isLast = currentTrackIndex == counts.Len();
            if (isAutoFit && isLast) {
                for (int t = tracks->len - 1; t >= 0; t--) {
                    GridTrack& prev = (*tracks)[t];
                    if (prev.kind == GridTrackKind::Track &&
                        !prev.isCollapsed) {
                        break;
                    }
                    prev.Collapse();
                }
            }
        }
    }

    int gridAreaTracks = (int)counts.negativeImplicit +
                         (int)counts.explicitCount - currentTrackIndex;
    if (gridAreaTracks < 0) {
        gridAreaTracks = 0;
    }
    uint16_t positive =
        (uint16_t)((int)counts.positiveImplicit + gridAreaTracks);
    if (autoTrackCount == 0) {
        CreateImplicitTracks(
            tracks, positive, []() { return TrackSizingFunction::Auto(); },
            gap);
    } else {
        int cursor = 0;
        CreateImplicitTracks(
            tracks, positive,
            [&]() {
                TrackSizingFunction t = autoTracks[cursor % autoTrackCount];
                cursor++;
                return t;
            },
            gap);
    }

    if (tracks->len > 0) {
        (*tracks)[0].Collapse();
        (*tracks)[tracks->len - 1].Collapse();
    }
}

enum class TrackSizeEstimate : uint8_t {

    MaxTrackSizingFunction,

    BaseSize
};

Optf EstimateTrackSize(const GridTrack& track, Optf basis,
                       TrackSizeEstimate kind, CalcResolver calc) {
    if (kind == TrackSizeEstimate::BaseSize) {
        return Some(track.baseSize);
    }
    return track.maxTrackSizingFunction.DefiniteValue(basis, calc);
}

SizeF MarginsAxisSumsWithBaselineShims(const GridItem& item,
                                       Optf innerNodeWidth, CalcResolver calc) {
    RectLp zeroBasis;
    (void)zeroBasis;
    RectLpa m = item.margin;
    RectF r;

    RectLpa horizontalOnly = {m.left, m.right, LengthPercentageAuto::Zero(),
                              LengthPercentageAuto::Zero()};
    RectF h = horizontalOnly.ResolveOrZero(Some(0.0f), calc);
    RectLpa verticalOnly = {LengthPercentageAuto::Zero(),
                            LengthPercentageAuto::Zero(), m.top, m.bottom};
    RectF v = verticalOnly.ResolveOrZero(innerNodeWidth, calc);
    r.left = h.left;
    r.right = h.right;
    r.top = v.top + item.baselineShim;
    r.bottom = v.bottom;
    return r.SumAxes();
}

Optf SpannedTrackLimit(const GridItem& item, AbstractAxis axis,
                       const GridTrack* axisTracks, Optf axisParentSize,
                       CalcResolver calc) {
    int from = item.TrackRangeStart(axis);
    int to = item.TrackRangeEnd(axis);
    float limit = 0.0f;
    for (int i = from; i < to; i++) {
        Optf v = axisTracks[i]
                     .maxTrackSizingFunction
                     .DefiniteLimit(axisParentSize, calc);
        if (!IsSome(v)) {
            return None();
        }
        limit += v;
    }
    return Some(limit);
}

Optf SpannedFixedTrackLimit(const GridItem& item, AbstractAxis axis,
                            const GridTrack* axisTracks, Optf axisParentSize,
                            CalcResolver calc) {
    int from = item.TrackRangeStart(axis);
    int to = item.TrackRangeEnd(axis);
    float limit = 0.0f;
    for (int i = from; i < to; i++) {
        Optf v = axisTracks[i]
                     .maxTrackSizingFunction
                     .DefiniteValue(axisParentSize, calc);
        if (!IsSome(v)) {
            return None();
        }
        limit += v;
    }
    return Some(limit);
}

SizeFOpt ItemKnownDimensions(const GridItem& item, TaffyTree* tree,
                             SizeFOpt gridAreaSize) {
    CalcResolver calc = tree->calc;
    SizeF margins =
        MarginsAxisSumsWithBaselineShims(item, gridAreaSize.w, calc);
    Optf aspectRatio = item.aspectRatio;

    RectF padding = item.padding.ResolveOrZero(gridAreaSize.w, calc);
    RectF border = item.border.ResolveOrZero(gridAreaSize.w, calc);
    SizeF paddingBorderSize = (padding + border).SumAxes();
    SizeF boxSizingAdjustment = item.boxSizing == BoxSizing::ContentBox
                                    ? paddingBorderSize
                                    : SizeF::Zero();
    SizeFOpt inherentSize =
        MaybeAdd(MaybeApplyAspectRatio(
                     item.size.MaybeResolve(gridAreaSize, calc), aspectRatio),
                 boxSizingAdjustment);

    SizeFOpt minSize = MaybeAdd(
        MaybeApplyAspectRatio(item.minSize.MaybeResolve(gridAreaSize, calc),
                              aspectRatio),
        boxSizingAdjustment);
    minSize = Or(minSize, paddingBorderSize);
    minSize = MaybeMax(minSize, paddingBorderSize);
    SizeFOpt maxSize = MaybeAdd(
        MaybeApplyAspectRatio(item.maxSize.MaybeResolve(gridAreaSize, calc),
                              aspectRatio),
        boxSizingAdjustment);

    SizeFOpt gridAreaMinusItemMargins =
        MaybeMax(MaybeSub(gridAreaSize, margins), SizeFOpt{0.0f, 0.0f});

    Optf width = inherentSize.w;
    if (!IsSome(width)) {

        if (item.size.width.IsSizingKeyword()) {
            SizingKeywordResolution keyword = ResolveSizingKeyword(
                item.size.width, gridAreaMinusItemMargins.w, gridAreaSize.w,
                calc);
            if (keyword.isSome && keyword.kind == SizingKeywordKind::Exact) {
                width = Some(keyword.exact);
            }
        } else if (!item.margin.left.IsAuto() && !item.margin.right.IsAuto() &&
                   item.IsStretchAligned(item.justifySelf)) {

            width = gridAreaMinusItemMargins.w;
        }
    }
    SizeFOpt sized =
        MaybeApplyAspectRatio(SizeFOpt{width, inherentSize.h}, aspectRatio);

    Optf height = sized.h;
    if (!IsSome(height)) {
        if (item.size.height.IsSizingKeyword()) {
            SizingKeywordResolution keyword = ResolveSizingKeyword(
                item.size.height, gridAreaMinusItemMargins.h, gridAreaSize.h,
                calc);
            if (keyword.isSome && keyword.kind == SizingKeywordKind::Exact) {
                height = Some(keyword.exact);
            }
        } else if (!item.margin.top.IsAuto() && !item.margin.bottom.IsAuto() &&
                   item.IsStretchAligned(item.alignSelf)) {
            height = gridAreaMinusItemMargins.h;
        }
    }
    sized = MaybeApplyAspectRatio(SizeFOpt{sized.w, height}, aspectRatio);
    return MaybeClamp(sized, minSize, maxSize);
}

SizeFOpt ItemGridAreaSize(const GridItem& item, AbstractAxis axis,
                          const GridTrack* axisTracks,
                          const GridTrack* otherAxisTracks,
                          SizeFOpt availableSpace, TrackSizeEstimate estimate,
                          CalcResolver calc) {
    SizeFOpt size = SizeFOptNone();

    {
        float sum = 0.0f;
        bool definite = true;
        int from = item.TrackRangeStart(axis);
        int to = item.TrackRangeEnd(axis);
        for (int i = from; i < to; i++) {
            const GridTrack& t = axisTracks[i];
            Optf mn = t.minTrackSizingFunction
                          .DefiniteValue(Get(availableSpace, axis), calc);
            Optf mx = t.maxTrackSizingFunction
                          .DefiniteValue(Get(availableSpace, axis), calc);
            if (!IsSome(mn) || !IsSome(mx) || mn != mx) {
                definite = false;
                break;
            }
            sum += t.baseSize;
        }
        Set(&size, axis, definite ? Some(sum) : None());
    }

    {
        AbstractAxis other = Other(axis);
        float sum = 0.0f;
        bool definite = true;
        int from = item.TrackRangeStart(other);
        int to = item.TrackRangeEnd(other);
        for (int i = from; i < to; i++) {
            const GridTrack& t = otherAxisTracks[i];
            Optf v = EstimateTrackSize(t, Get(availableSpace, other), estimate,
                                       calc);
            if (!IsSome(v)) {
                definite = false;
                break;
            }
            sum += v + t.contentAlignmentAdjustment;
        }
        Set(&size, other, definite ? Some(sum) : None());
    }

    return size;
}

SizeFOpt ItemGridAreaSizeCached(GridItem* item, AbstractAxis axis,
                                const GridTrack* axisTracks,
                                const GridTrack* otherAxisTracks,
                                SizeFOpt availableSpace,
                                TrackSizeEstimate estimate, CalcResolver calc) {
    if (item->hasGridAreaSizeCache) {
        return item->gridAreaSizeCache;
    }
    SizeFOpt s = ItemGridAreaSize(*item, axis, axisTracks, otherAxisTracks,
                                  availableSpace, estimate, calc);
    item->gridAreaSizeCache = s;
    item->hasGridAreaSizeCache = true;
    return s;
}

SizeAvail KeywordAdjustedAvailableSpace(const GridItem& item,
                                        SizeFOpt gridAreaSize,
                                        SizeAvail available, TaffyTree* tree) {
    if (!item.size.width.IsSizingKeyword() && !item.size.height
                                                   .IsSizingKeyword()) {
        return available;
    }
    SizeF margins =
        MarginsAxisSumsWithBaselineShims(item, gridAreaSize.w, tree->calc);
    SizeAvail adjusted = available;
    AbstractAxis axes[2] = {AbstractAxis::Inline, AbstractAxis::Block};
    for (int i = 0; i < 2; i++) {
        AbstractAxis axis = axes[i];
        Dimension sizeStyle = item.size.Get(axis);
        if (!sizeStyle.IsSizingKeyword()) {
            continue;
        }
        Optf stretch = MaybeMax(
            MaybeSub(Get(gridAreaSize, axis), Some(Get(margins, axis))), 0.0f);
        SizingKeywordResolution keyword = ResolveSizingKeyword(
            sizeStyle, stretch, Get(gridAreaSize, axis), tree->calc);
        if (keyword.isSome && keyword.kind == SizingKeywordKind::Measure) {
            adjusted.Set(axis, keyword.measure);
        }
    }
    return adjusted;
}

SizeFOpt AvailableSpaceMinusMargins(const GridItem& item, SizeFOpt gridAreaSize,
                                    SizeFOpt availableSpace, TaffyTree* tree) {
    if (!IsSome(availableSpace.w) && !IsSome(availableSpace.h)) {
        return availableSpace;
    }
    RectF margin = item.margin.ResolveOrZero(gridAreaSize.w, tree->calc);
    SizeFOpt out = availableSpace;
    if (IsSome(out.w)) {
        out.w = F32Max(out.w - margin.HorizontalAxisSum(), 0.0f);
    }
    if (IsSome(out.h)) {
        out.h =
            F32Max(out.h - margin.VerticalAxisSum() - item.baselineShim, 0.0f);
    }
    return out;
}

float ItemMinContentContribution(const GridItem& item, AbstractAxis axis,
                                 TaffyTree* tree, SizeFOpt gridAreaSize,
                                 SizeFOpt availableSpace) {
    SizeFOpt knownDimensions = ItemKnownDimensions(item, tree, gridAreaSize);

    Optf known = Get(knownDimensions, axis);
    if (IsSome(known)) {
        return known;
    }

    availableSpace =
        AvailableSpaceMinusMargins(item, gridAreaSize, availableSpace, tree);
    SizeAvail avail = {
        IsSome(availableSpace.w) ? AvailableSpace::Definite(availableSpace.w)
                                 : AvailableSpace::MinContent(),
        IsSome(availableSpace.h) ? AvailableSpace::Definite(availableSpace.h)
                                 : AvailableSpace::MinContent()};
    avail = KeywordAdjustedAvailableSpace(item, gridAreaSize, avail, tree);
    return tree->MeasureChildSize(item.node, knownDimensions, gridAreaSize,
                                  avail, SizingMode::InherentSize,
                                  AsAbsNaive(axis), LineBool::False());
}

float ItemMinContentContributionCached(GridItem* item, AbstractAxis axis,
                                       TaffyTree* tree, SizeFOpt gridAreaSize,
                                       SizeFOpt availableSpace) {
    Optf cached = Get(item->minContentContributionCache, axis);
    if (IsSome(cached)) {
        return cached;
    }
    float size = ItemMinContentContribution(*item, axis, tree, gridAreaSize,
                                            availableSpace);
    Set(&item->minContentContributionCache, axis, Some(size));
    return size;
}

float ItemMaxContentContribution(const GridItem& item, AbstractAxis axis,
                                 TaffyTree* tree, SizeFOpt gridAreaSize,
                                 SizeFOpt availableSpace) {
    SizeFOpt knownDimensions = ItemKnownDimensions(item, tree, gridAreaSize);
    Optf known = Get(knownDimensions, axis);
    if (IsSome(known)) {
        return known;
    }
    availableSpace =
        AvailableSpaceMinusMargins(item, gridAreaSize, availableSpace, tree);
    SizeAvail avail = {
        IsSome(availableSpace.w) ? AvailableSpace::Definite(availableSpace.w)
                                 : AvailableSpace::MaxContent(),
        IsSome(availableSpace.h) ? AvailableSpace::Definite(availableSpace.h)
                                 : AvailableSpace::MaxContent()};
    avail = KeywordAdjustedAvailableSpace(item, gridAreaSize, avail, tree);
    return tree->MeasureChildSize(item.node, knownDimensions, gridAreaSize,
                                  avail, SizingMode::InherentSize,
                                  AsAbsNaive(axis), LineBool::False());
}

float ItemMaxContentContributionCached(GridItem* item, AbstractAxis axis,
                                       TaffyTree* tree, SizeFOpt gridAreaSize,
                                       SizeFOpt availableSpace) {
    Optf cached = Get(item->maxContentContributionCache, axis);
    if (IsSome(cached)) {
        return cached;
    }
    float size = ItemMaxContentContribution(*item, axis, tree, gridAreaSize,
                                            availableSpace);
    Set(&item->maxContentContributionCache, axis, Some(size));
    return size;
}

float ItemMinimumContribution(GridItem* item, TaffyTree* tree,
                              AbstractAxis axis, const GridTrack* axisTracks,
                              int axisTrackCount, SizeFOpt gridAreaSize,
                              SizeFOpt innerNodeSize) {
    CalcResolver calc = tree->calc;
    RectF padding = item->padding.ResolveOrZero(gridAreaSize.w, calc);
    RectF border = item->border.ResolveOrZero(gridAreaSize.w, calc);
    SizeF paddingBorderSize = (padding + border).SumAxes();
    SizeF boxSizingAdjustment = item->boxSizing == BoxSizing::ContentBox
                                    ? paddingBorderSize
                                    : SizeF::Zero();

    Optf size = Get(MaybeAdd(MaybeApplyAspectRatio(
                                 item->size.MaybeResolve(gridAreaSize, calc),
                                 item->aspectRatio),
                             boxSizingAdjustment),
                    axis);

    if (IsSome(size) && (!item->minSize.Get(axis).IsAuto() ||
                         !item->maxSize.Get(axis).IsAuto())) {
        Optf basis =
            axis == AbstractAxis::Inline ? gridAreaSize.w : gridAreaSize.h;
        Optf maxBasis =
            item->isCompressibleReplaced ? Or(basis, Some(0.0f)) : basis;
        float adjustment = Get(boxSizingAdjustment, axis);
        Optf minResolved =
            MaybeAdd(item->minSize.Get(axis).MaybeResolve(basis, calc),
                     Some(adjustment));
        Optf maxResolved =
            MaybeAdd(item->maxSize.Get(axis).MaybeResolve(maxBasis, calc),
                     Some(adjustment));
        size = MaybeClamp(size, minResolved, maxResolved);
    }
    if (!IsSome(size)) {

        Dimension pref = item->size.Get(axis);
        if (pref.IsSizingKeyword() && !pref.IsStretch()) {
            size = Some(ItemMinContentContributionCached(
                item, axis, tree, gridAreaSize, gridAreaSize));
        }
    }
    if (!IsSome(size)) {

        SizeFOpt cyclicMinBasis = {Some(UnwrapOr(gridAreaSize.w, 0.0f)),
                                   Some(UnwrapOr(gridAreaSize.h, 0.0f))};
        size = Get(MaybeAdd(MaybeApplyAspectRatio(item->minSize.MaybeResolve(
                                                      cyclicMinBasis, calc),
                                                  item->aspectRatio),
                            boxSizingAdjustment),
                   axis);
    }
    if (!IsSome(size)) {
        Overflow o =
            axis == AbstractAxis::Inline ? item->overflow.x : item->overflow.y;
        size = MaybeIntoAutomaticMinSize(o);
    }
    if (!IsSome(size)) {

        (void)axisTrackCount;
        bool spansAutoMinTrack = false;
        bool spansAFlexibleTrack = false;
        int from = item->TrackRangeStart(axis);
        int to = item->TrackRangeEnd(axis);
        for (int i = from; i < to; i++) {
            if (axisTracks[i].minTrackSizingFunction.BehavesAsAuto(
                    Get(innerNodeSize, axis))) {
                spansAutoMinTrack = true;
            }
            if (axisTracks[i].maxTrackSizingFunction.IsFr()) {
                spansAFlexibleTrack = true;
            }
        }
        bool onlySpanOneTrack =
            (item->TrackRangeEnd(axis) - item->TrackRangeStart(axis)) == 1;
        bool useContentBasedMinimum =
            spansAutoMinTrack && (onlySpanOneTrack || !spansAFlexibleTrack);

        if (useContentBasedMinimum) {
            float minimumContribution = ItemMinContentContributionCached(
                item, axis, tree, gridAreaSize, gridAreaSize);

            if (item->isCompressibleReplaced) {
                Optf pref = item->size.Get(axis).MaybeResolve(Some(0.0f), calc);
                Optf mx = item->maxSize.Get(axis)
                              .MaybeResolve(Some(0.0f), calc);
                minimumContribution =
                    MaybeMin(MaybeMin(minimumContribution, pref), mx);
            }

            Optf limit = SpannedFixedTrackLimit(*item, axis, axisTracks,
                                                Get(innerNodeSize, axis), calc);
            if (IsSome(limit)) {
                limit = F32Max(limit, Get(paddingBorderSize, axis));
            }
            size = Some(MaybeMin(minimumContribution, limit));
        } else {
            size = Some(0.0f);
        }
    }

    return UnwrapOr(size, 0.0f);
}

float ItemMinimumContributionCached(GridItem* item, TaffyTree* tree,
                                    AbstractAxis axis,
                                    const GridTrack* axisTracks,
                                    int axisTrackCount, SizeFOpt gridAreaSize,
                                    SizeFOpt innerNodeSize) {
    Optf cached = Get(item->minimumContributionCache, axis);
    if (IsSome(cached)) {
        return cached;
    }
    float size =
        ItemMinimumContribution(item, tree, axis, axisTracks, axisTrackCount,
                                gridAreaSize, innerNodeSize);
    Set(&item->minimumContributionCache, axis, Some(size));
    return size;
}

OriginZeroLine AdvancePosition(OriginZeroLine position) {
    int32_t value = (int32_t)position.v + 1;
    value = value < INT16_MIN   ? INT16_MIN
            : value > INT16_MAX ? INT16_MAX
                                : value;
    return OriginZeroLine{(int16_t)value};
}

LineOzl ResolveIndefiniteGridSpan(OriginZeroLine position, uint16_t span) {
    auto line = [](int32_t value) {
        value = value < INT16_MIN   ? INT16_MIN
                : value > INT16_MAX ? INT16_MAX
                                    : value;
        return OriginZeroLine{(int16_t)value};
    };
    return {position, line((int32_t)position.v + span)};
}

LineOzl ClampSpanToLimitedGrid(LineOzl span) {
    int32_t start = span.start.v;
    start = start<kMinOzLine ? kMinOzLine : start>(int32_t) kMaxOzLine - 1
                ? (int32_t)kMaxOzLine - 1
                : start;
    int32_t end = span.end.v;
    end = end<start + 1 ? start + 1 : end>(int32_t) kMaxOzLine
              ? (int32_t)kMaxOzLine
              : end;
    return {OriginZeroLine{(int16_t)start}, OriginZeroLine{(int16_t)end}};
}

void RecordGridPlacement(CellOccupancyMatrix* matrix, Vec<GridItem>* items,
                         TaffyTree* tree, NodeId node, int index,
                         AlignItems parentAlignItems,
                         AlignItems parentJustifyItems,
                         AbsoluteAxis primaryAxis, LineOzl primarySpan,
                         LineOzl secondarySpan,
                         CellOccupancyState placementType) {
    primarySpan = ClampSpanToLimitedGrid(primarySpan);
    secondarySpan = ClampSpanToLimitedGrid(secondarySpan);
    matrix->MarkAreaAs(primaryAxis, primarySpan, secondarySpan, placementType);

    LineOzl colSpan =
        primaryAxis == AbsoluteAxis::Horizontal ? primarySpan : secondarySpan;
    LineOzl rowSpan =
        primaryAxis == AbsoluteAxis::Horizontal ? secondarySpan : primarySpan;

    const Style& s = tree->GetStyle(node);
    GridItem item;
    item.node = node;
    item.sourceOrder = (uint16_t)index;
    item.row = rowSpan;
    item.column = colSpan;
    item.isCompressibleReplaced = s.IsCompressibleReplaced();
    item.isReplaced = s.IsReplaced();
    item.overflow = s.overflow;
    item.boxSizing = s.boxSizing;
    item.size = s.size;
    item.minSize = s.minSize;
    item.maxSize = s.maxSize;
    item.aspectRatio = s.aspectRatio;
    item.padding = s.padding;
    item.border = s.border;
    item.margin = s.margin;
    item.alignSelf = s.alignSelf.UnwrapOr(parentAlignItems);
    item.justifySelf = s.justifySelf.UnwrapOr(parentJustifyItems);
    VecAppend(*items, item);
}

LineOzl PlaceDefiniteGridItemAxis(const PlacementChild& child,
                                  AbsoluteAxis axis) {
    return child.Get(axis).ResolveDefiniteGridLines();
}

struct SpanPair {
    LineOzl primary;
    LineOzl secondary;
};

SpanPair PlaceDefiniteSecondaryAxisItem(const CellOccupancyMatrix& matrix,
                                        const PlacementChild& child,
                                        GridAutoFlow autoFlow) {
    AbsoluteAxis primaryAxis = PrimaryAxis(autoFlow);
    AbsoluteAxis secondaryAxis = OtherAxis(primaryAxis);
    OriginZeroLine primaryStart = matrix.Counts(primaryAxis)
                                      .ImplicitStartLine();

    LineOzl secondaryPlacement = child.Get(secondaryAxis)
                                     .ResolveDefiniteGridLines();

    OriginZeroLine startingPosition = primaryStart;
    if (!IsDense(autoFlow)) {
        OptOriginZeroLine lookup =
            matrix.AutoPlacementCursor(primaryAxis, secondaryPlacement.start);
        if (lookup.IsSome()) {
            startingPosition = lookup.val;
        }
    }
    uint16_t primarySpanLen = child.Get(primaryAxis).IndefiniteSpan();

    OriginZeroLine position = startingPosition;
    while (true) {
        LineOzl primaryPlacement =
            ResolveIndefiniteGridSpan(position, primarySpanLen);
        OptOriginZeroLine collision = matrix.LineAreaCollisionJump(
            primaryAxis, primaryPlacement, secondaryPlacement);
        if (!collision.IsSome()) {
            return {primaryPlacement, secondaryPlacement};
        }
        position = collision.val;
    }
}

SpanPair PlaceIndefinitelyPositionedItem(const CellOccupancyMatrix& matrix,
                                         const PlacementChild& child,
                                         GridAutoFlow autoFlow,
                                         OriginZeroLine gridPrimary,
                                         OriginZeroLine gridSecondary) {
    AbsoluteAxis primaryAxis = PrimaryAxis(autoFlow);
    AbsoluteAxis secondaryAxis = OtherAxis(primaryAxis);

    LinePlain primaryStyle = child.Get(primaryAxis);
    LinePlain secondaryStyle = child.Get(secondaryAxis);

    uint16_t secondarySpanLen = secondaryStyle.IndefiniteSpan();
    bool hasDefinitePrimary = primaryStyle.IsDefinite();
    OriginZeroLine primaryGridStart = matrix.Counts(primaryAxis)
                                          .ImplicitStartLine();
    OriginZeroLine primaryGridEnd = matrix.Counts(primaryAxis)
                                        .ImplicitEndLine();
    OriginZeroLine secondaryGridStart = matrix.Counts(secondaryAxis)
                                            .ImplicitStartLine();

    OriginZeroLine primaryIdx = gridPrimary;
    OriginZeroLine secondaryIdx = gridSecondary;

    if (hasDefinitePrimary) {
        LineOzl primarySpan = primaryStyle.ResolveDefiniteGridLines();
        if (IsDense(autoFlow)) {
            secondaryIdx = secondaryGridStart;
        } else if (primarySpan.start < primaryIdx) {
            secondaryIdx = AdvancePosition(secondaryIdx);
        }

        while (true) {
            LineOzl secondarySpan =
                ResolveIndefiniteGridSpan(secondaryIdx, secondarySpanLen);
            OptOriginZeroLine collision = matrix.LineAreaCollisionJump(
                secondaryAxis, secondarySpan, primarySpan);
            if (!collision.IsSome()) {
                return {primarySpan, secondarySpan};
            }
            secondaryIdx = collision.val;
        }
    }

    uint16_t primarySpanLen = primaryStyle.IndefiniteSpan();

    while (true) {
        LineOzl primarySpan =
            ResolveIndefiniteGridSpan(primaryIdx, primarySpanLen);
        LineOzl secondarySpan =
            ResolveIndefiniteGridSpan(secondaryIdx, secondarySpanLen);

        if (primarySpan.end > primaryGridEnd) {

            if (primaryIdx == primaryGridStart) {
                return {primarySpan, secondarySpan};
            }
            secondaryIdx = AdvancePosition(secondaryIdx);
            primaryIdx = primaryGridStart;
            continue;
        }
        OptOriginZeroLine collision = matrix.LineAreaCollisionJump(
            primaryAxis, primarySpan, secondarySpan);
        if (collision.IsSome()) {
            primaryIdx = collision.val;
            continue;
        }
        return {primarySpan, secondarySpan};
    }
}

void PlaceGridItems(CellOccupancyMatrix* matrix, Vec<GridItem>* items,
                    TaffyTree* tree, const Vec<PlacementChild>& children,
                    GridAutoFlow gridAutoFlow, AlignItems alignItems,
                    AlignItems justifyItems) {
    AbsoluteAxis primaryAxis = PrimaryAxis(gridAutoFlow);
    AbsoluteAxis secondaryAxis = OtherAxis(primaryAxis);

    for (int i = 0; i < len(children); i++) {
        const PlacementChild& c = children[i];
        if (!c.horizontal.IsDefinite() || !c.vertical.IsDefinite()) {
            continue;
        }
        LineOzl primarySpan = PlaceDefiniteGridItemAxis(c, primaryAxis);
        LineOzl secondarySpan = PlaceDefiniteGridItemAxis(c, secondaryAxis);
        RecordGridPlacement(matrix, items, tree, c.node, c.index, alignItems,
                            justifyItems, primaryAxis, primarySpan,
                            secondarySpan,
                            CellOccupancyState::DefinitelyPlaced);
    }

    for (int i = 0; i < len(children); i++) {
        const PlacementChild& c = children[i];
        if (!c.Get(secondaryAxis).IsDefinite() || c.Get(primaryAxis)
                                                      .IsDefinite()) {
            continue;
        }
        SpanPair spans =
            PlaceDefiniteSecondaryAxisItem(*matrix, c, gridAutoFlow);
        RecordGridPlacement(matrix, items, tree, c.node, c.index, alignItems,
                            justifyItems, primaryAxis, spans.primary,
                            spans.secondary, CellOccupancyState::AutoPlaced);
    }

    OriginZeroLine startPrimary = matrix->Counts(primaryAxis)
                                      .ImplicitStartLine();
    OriginZeroLine startSecondary = matrix->Counts(secondaryAxis)
                                        .ImplicitStartLine();
    OriginZeroLine posPrimary = startPrimary;
    OriginZeroLine posSecondary = startSecondary;

    for (int i = 0; i < len(children); i++) {
        const PlacementChild& c = children[i];
        if (c.Get(secondaryAxis).IsDefinite()) {
            continue;
        }
        SpanPair spans = PlaceIndefinitelyPositionedItem(
            *matrix, c, gridAutoFlow, posPrimary, posSecondary);
        RecordGridPlacement(matrix, items, tree, c.node, c.index, alignItems,
                            justifyItems, primaryAxis, spans.primary,
                            spans.secondary, CellOccupancyState::AutoPlaced);

        if (IsDense(gridAutoFlow)) {
            posPrimary = startPrimary;
            posSecondary = startSecondary;
        } else {
            posPrimary = spans.primary.end;
            posSecondary = spans.secondary.start;
        }
    }
}

enum class IntrinsicContributionType : uint8_t {
    Minimum,
    Maximum
};

struct ItemBatcher {
    AbstractAxis axis;
    int indexOffset = 0;
    uint16_t currentSpan = 1;
    bool currentIsFlex = false;

    bool Next(GridItem* items, int n, int* outStart, int* outEnd,
              bool* outIsFlex) {
        if (currentIsFlex || indexOffset >= n) {
            return false;
        }
        const GridItem& item = items[indexOffset];
        currentSpan = item.Span(axis);
        currentIsFlex = item.CrossesFlexibleTrack(axis);

        int nextIndexOffset = n;
        if (!currentIsFlex) {
            for (int i = 0; i < n; i++) {
                if (items[i].CrossesFlexibleTrack(axis) ||
                    items[i].Span(axis) > currentSpan) {
                    nextIndexOffset = i;
                    break;
                }
            }
        }
        *outStart = indexOffset;
        *outEnd = nextIndexOffset;
        *outIsFlex = currentIsFlex;
        indexOffset = nextIndexOffset;
        return true;
    }
};

struct IntrinsicSizeMeasurer {
    TaffyTree* tree;
    const GridTrack* otherAxisTracks;
    TrackSizeEstimate estimate;
    AbstractAxis axis;
    SizeFOpt innerNodeSize = SizeFOptNone();

    CalcResolver Calc() const { return tree->calc; }

    SizeFOpt GridAreaSize(GridItem* item, const GridTrack* axisTracks) const {
        return ItemGridAreaSizeCached(item, axis, axisTracks, otherAxisTracks,
                                      innerNodeSize, estimate, tree->calc);
    }
    float MinContentContribution(GridItem* item,
                                 const GridTrack* axisTracks) const {
        SizeFOpt gridAreaSize = GridAreaSize(item, axisTracks);
        SizeFOpt availableSpace = gridAreaSize;
        Set(&availableSpace, axis, None());
        SizeF marginAxisSums = MarginsAxisSumsWithBaselineShims(
            *item, availableSpace.w, tree->calc);
        float contribution = ItemMinContentContributionCached(
            item, axis, tree, gridAreaSize, availableSpace);
        return contribution + Get(marginAxisSums, axis);
    }
    float MaxContentContribution(GridItem* item,
                                 const GridTrack* axisTracks) const {
        SizeFOpt gridAreaSize = GridAreaSize(item, axisTracks);
        SizeFOpt availableSpace = gridAreaSize;
        Set(&availableSpace, axis, None());
        SizeF marginAxisSums = MarginsAxisSumsWithBaselineShims(
            *item, availableSpace.w, tree->calc);
        float contribution = ItemMaxContentContributionCached(
            item, axis, tree, gridAreaSize, availableSpace);
        return contribution + Get(marginAxisSums, axis);
    }
    float MinimumContribution(GridItem* item, const GridTrack* axisTracks,
                              int axisTrackCount) const {
        SizeFOpt gridAreaSize = GridAreaSize(item, axisTracks);
        SizeFOpt availableSpace = gridAreaSize;
        Set(&availableSpace, axis, None());
        SizeF marginAxisSums = MarginsAxisSumsWithBaselineShims(
            *item, availableSpace.w, tree->calc);
        float contribution = ItemMinimumContributionCached(
            item, tree, axis, axisTracks, axisTrackCount, gridAreaSize,
            innerNodeSize);
        return contribution + Get(marginAxisSums, axis);
    }
};

bool CmpByCrossFlexThenSpanThenStart(const GridItem& a, const GridItem& b,
                                     AbstractAxis axis) {
    bool af = a.CrossesFlexibleTrack(axis);
    bool bf = b.CrossesFlexibleTrack(axis);
    if (af != bf) {
        return !af;
    }
    LineOzl pa = a.Placement(axis);
    LineOzl pb = b.Placement(axis);
    if (pa.Span() != pb.Span()) {
        return pa.Span() < pb.Span();
    }
    return pa.start < pb.start;
}

float ComputeAlignmentGutterAdjustment(AlignContent alignment,
                                       Optf axisInnerNodeSize,
                                       const GridTrack* tracks, int nTracks,
                                       TrackSizeEstimate estimate,
                                       CalcResolver calc) {
    if (nTracks <= 1) {
        return 0.0f;
    }

    int outerGutterWeight = 0;
    int innerGutterWeight = 0;
    switch (alignment.Keyword()) {
        case AlignContentKeyword::Start:
        case AlignContentKeyword::FlexStart:
        case AlignContentKeyword::End:
        case AlignContentKeyword::FlexEnd:
        case AlignContentKeyword::Center:
            outerGutterWeight = 1;
            innerGutterWeight = 0;
            break;
        case AlignContentKeyword::Normal:
        case AlignContentKeyword::Stretch:
            outerGutterWeight = 0;
            innerGutterWeight = 0;
            break;
        case AlignContentKeyword::SpaceBetween:
            outerGutterWeight = 0;
            innerGutterWeight = 1;
            break;
        case AlignContentKeyword::SpaceAround:
            outerGutterWeight = 1;
            innerGutterWeight = 2;
            break;
        case AlignContentKeyword::SpaceEvenly:
            outerGutterWeight = 1;
            innerGutterWeight = 1;
            break;
    }
    if (innerGutterWeight == 0) {
        return 0.0f;
    }
    if (!IsSome(axisInnerNodeSize)) {
        return 0.0f;
    }

    float trackSizeSum = 0.0f;
    bool definite = true;
    for (int i = 0; i < nTracks; i++) {
        Optf v =
            EstimateTrackSize(tracks[i], axisInnerNodeSize, estimate, calc);
        if (!IsSome(v)) {
            definite = false;
            break;
        }
        trackSizeSum += v;
    }
    float freeSpace =
        definite ? F32Max(0.0f, axisInnerNodeSize - trackSizeSum) : 0.0f;

    int weightedTrackCount =
        (((nTracks - 3) / 2) * innerGutterWeight) + (2 * outerGutterWeight);
    if (weightedTrackCount == 0) {
        return 0.0f;
    }
    return (freeSpace / (float)weightedTrackCount) * (float)innerGutterWeight;
}

void ResolveItemTrackIndexes(GridItem* items, int n, TrackCounts columnCounts,
                             TrackCounts rowCounts) {
    for (int i = 0; i < n; i++) {
        GridItem& item = items[i];
        item.columnIndexes.start =
            (uint16_t)IntoTrackVecIndex(item.column.start, columnCounts);
        item.columnIndexes
            .end = (uint16_t)IntoTrackVecIndex(item.column.end, columnCounts);
        item.rowIndexes
            .start = (uint16_t)IntoTrackVecIndex(item.row.start, rowCounts);
        item.rowIndexes
            .end = (uint16_t)IntoTrackVecIndex(item.row.end, rowCounts);
    }
}

void DetermineIfItemCrossesFlexibleOrIntrinsicTracks(GridItem* items, int n,
                                                     const GridTrack* columns,
                                                     const GridTrack* rows) {
    for (int i = 0; i < n; i++) {
        GridItem& item = items[i];
        item.crossesFlexibleColumn = false;
        item.crossesIntrinsicColumn = false;
        for (int k = item.TrackRangeStart(AbstractAxis::Inline);
             k < item.TrackRangeEnd(AbstractAxis::Inline); k++) {
            if (columns[k].IsFlexible()) {
                item.crossesFlexibleColumn = true;
            }
            if (columns[k].HasIntrinsicSizingFunction()) {
                item.crossesIntrinsicColumn = true;
            }
        }
        item.crossesFlexibleRow = false;
        item.crossesIntrinsicRow = false;
        for (int k = item.TrackRangeStart(AbstractAxis::Block);
             k < item.TrackRangeEnd(AbstractAxis::Block); k++) {
            if (rows[k].IsFlexible()) {
                item.crossesFlexibleRow = true;
            }
            if (rows[k].HasIntrinsicSizingFunction()) {
                item.crossesIntrinsicRow = true;
            }
        }
    }
}

void FlushPlannedBaseSizeIncreases(GridTrack* tracks, int n) {
    for (int i = 0; i < n; i++) {
        tracks[i].baseSize += tracks[i].baseSizePlannedIncrease;
        tracks[i].baseSizePlannedIncrease = 0.0f;
    }
}

void FlushPlannedGrowthLimitIncreases(GridTrack* tracks, int n,
                                      bool setInfinitelyGrowable) {
    for (int i = 0; i < n; i++) {
        GridTrack& t = tracks[i];
        if (t.growthLimitPlannedIncrease > 0.0f) {
            t.growthLimit = t.growthLimit == INFINITY
                                ? t.baseSize + t.growthLimitPlannedIncrease
                                : t.growthLimit + t.growthLimitPlannedIncrease;
            t.infinitelyGrowable = setInfinitelyGrowable;
        } else {
            t.infinitelyGrowable = false;
        }
        t.growthLimitPlannedIncrease = 0.0f;
    }
}

void InitializeTrackSizes(TaffyTree* tree, GridTrack* tracks, int n,
                          Optf axisInnerNodeSize) {
    CalcResolver calc = tree->calc;
    for (int i = 0; i < n; i++) {
        GridTrack& t = tracks[i];

        t.baseSize = UnwrapOr(t.minTrackSizingFunction
                                  .DefiniteValue(axisInnerNodeSize, calc),
                              0.0f);

        t.growthLimit = UnwrapOr(t.maxTrackSizingFunction
                                     .DefiniteValue(axisInnerNodeSize, calc),
                                 INFINITY);
        if (t.growthLimit < t.baseSize) {
            t.growthLimit = t.baseSize;
        }
    }
}

void ResolveItemBaselines(TaffyTree* tree, AbstractAxis axis, GridItem* items,
                          int n, SizeFOpt innerNodeSize) {
    AbstractAxis otherAxis = Other(axis);
    StableSort(items, n, [&](const GridItem& a, const GridItem& b) {
        return a.Placement(otherAxis).start < b.Placement(otherAxis).start;
    });

    int start = 0;
    while (start < n) {
        OriginZeroLine currentRow = items[start].Placement(otherAxis).start;
        int end = start;
        while (end < n && items[end].Placement(otherAxis).start == currentRow) {
            end++;
        }

        int baselineCount = 0;
        for (int i = start; i < end; i++) {
            if (items[i].ParticipatesInBaselineAlignment()) {
                baselineCount++;
            }
        }
        if (baselineCount <= 1) {
            start = end;
            continue;
        }

        for (int i = start; i < end; i++) {
            GridItem& item = items[i];
            if (!item.ParticipatesInBaselineAlignment()) {
                continue;
            }
            LayoutOutput out = tree->PerformChildLayout(
                item.node, SizeFOptNone(), innerNodeSize,
                SizeAvail::MinContent(), SizingMode::InherentSize,
                LineBool::False());
            RectLpa topOnly = {LengthPercentageAuto::Zero(),
                               LengthPercentageAuto::Zero(), item.margin.top,
                               LengthPercentageAuto::Zero()};
            float marginTop = topOnly.ResolveOrZero(innerNodeSize.w, tree->calc)
                                  .top;

            float baseline = UnwrapOr(out.baselines.first, out.size.h);
            if (IsScrollContainer(item.overflow.y)) {
                baseline = F32Max(0.0f, F32Min(baseline, out.size.h));
            }
            item.baseline = Some(baseline + marginTop);
        }

        float rowMaxBaseline = 0.0f;
        for (int i = start; i < end; i++) {
            if (!items[i].ParticipatesInBaselineAlignment()) {
                continue;
            }
            rowMaxBaseline =
                F32Max(rowMaxBaseline, UnwrapOr(items[i].baseline, 0.0f));
        }
        for (int i = start; i < end; i++) {
            if (!items[i].ParticipatesInBaselineAlignment()) {
                continue;
            }
            items[i].baselineShim =
                rowMaxBaseline - UnwrapOr(items[i].baseline, 0.0f);
        }
        start = end;
    }
}

template <typename Affected, typename Proportion, typename Property,
          typename Limit>
float DistributeSpaceUpToLimits(float spaceToDistribute, GridTrack* tracks,
                                int n, Affected trackIsAffected,
                                Proportion trackDistributionProportion,
                                Property trackAffectedProperty,
                                Limit trackLimit) {

    const float kThreshold = 0.01f;

    int maxIterations = n + 1;
    for (int iter = 0; iter < maxIterations; iter++) {
        if (spaceToDistribute <= kThreshold) {
            break;
        }
        float proportionSum = 0.0f;
        for (int i = 0; i < n; i++) {
            const GridTrack& t = tracks[i];
            if (trackAffectedProperty(t) + t.itemIncurredIncrease <
                    trackLimit(t) &&
                trackIsAffected(t)) {
                proportionSum += trackDistributionProportion(t);
            }
        }
        if (proportionSum == 0.0f) {
            break;
        }

        bool haveMin = false;
        float minIncreaseLimit = 0.0f;
        for (int i = 0; i < n; i++) {
            const GridTrack& t = tracks[i];
            if (trackAffectedProperty(t) + t.itemIncurredIncrease <
                    trackLimit(t) &&
                trackIsAffected(t)) {
                float v = (trackLimit(t) - trackAffectedProperty(t) -
                           t.itemIncurredIncrease) /
                          trackDistributionProportion(t);
                if (!haveMin || v < minIncreaseLimit) {
                    minIncreaseLimit = v;
                    haveMin = true;
                }
            }
        }
        if (!haveMin) {
            break;
        }
        float iterationIncrease =
            F32Min(minIncreaseLimit, spaceToDistribute / proportionSum);

        for (int i = 0; i < n; i++) {
            GridTrack& t = tracks[i];
            if (!trackIsAffected(t)) {
                continue;
            }
            float increase = iterationIncrease * trackDistributionProportion(t);
            if (increase > 0.0f &&
                trackAffectedProperty(t) + t.itemIncurredIncrease + increase <=
                    trackLimit(t) + kThreshold) {
                t.itemIncurredIncrease += increase;
                spaceToDistribute -= increase;
            }
        }
    }
    return spaceToDistribute;
}

template <typename Affected, typename Proportion, typename Limit>
void DistributeItemSpaceToBaseSizeInner(
    float space, GridTrack* tracks, int n, Affected trackIsAffected,
    Proportion trackDistributionProportion, Limit trackLimit,
    IntrinsicContributionType intrinsicContributionType) {
    if (space == 0.0f) {
        return;
    }
    bool anyAffected = false;
    for (int i = 0; i < n; i++) {
        if (trackIsAffected(tracks[i])) {
            anyAffected = true;
            break;
        }
    }
    if (!anyAffected) {
        return;
    }

    auto getBaseSize = [](const GridTrack& t) { return t.baseSize; };

    float trackSizes = 0.0f;
    for (int i = 0; i < n; i++) {
        trackSizes += tracks[i].baseSize;
    }
    float extraSpace = F32Max(0.0f, space - trackSizes);

    const float kThreshold = 0.000001f;
    extraSpace = DistributeSpaceUpToLimits(
        extraSpace, tracks, n, trackIsAffected, trackDistributionProportion,
        getBaseSize, trackLimit);

    if (extraSpace > kThreshold) {

        auto minimumFilter = [](const GridTrack& t) {
            return t.maxTrackSizingFunction.IsIntrinsic();
        };
        auto maximumFilter = [](const GridTrack& t) {
            return t.maxTrackSizingFunction.IsMaxOrFitContent();
        };
        int count = 0;
        for (int i = 0; i < n; i++) {
            const GridTrack& t = tracks[i];
            bool matches =
                intrinsicContributionType == IntrinsicContributionType::Minimum
                    ? minimumFilter(t)
                    : maximumFilter(t);
            if (trackIsAffected(t) && matches) {
                count++;
            }
        }

        bool useAll = count == 0;
        auto filter = [&](const GridTrack& t) {
            if (!trackIsAffected(t)) {
                return false;
            }
            if (useAll) {
                return true;
            }
            return intrinsicContributionType ==
                           IntrinsicContributionType::Minimum
                       ? minimumFilter(t)
                       : maximumFilter(t);
        };
        DistributeSpaceUpToLimits(
            extraSpace, tracks, n, filter, trackDistributionProportion,
            getBaseSize,
            [](const GridTrack& t) { return t.FitContentLimit(); });
    }

    for (int i = 0; i < n; i++) {
        GridTrack& t = tracks[i];
        if (t.itemIncurredIncrease > t.baseSizePlannedIncrease) {
            t.baseSizePlannedIncrease = t.itemIncurredIncrease;
        }
        t.itemIncurredIncrease = 0.0f;
    }
}

template <typename Affected, typename Limit>
void DistributeItemSpaceToBaseSize(
    bool isFlex, bool, float space, GridTrack* tracks, int n,
    Affected trackIsAffected, Limit trackLimit,
    IntrinsicContributionType intrinsicContributionType) {
    auto one = [](const GridTrack&) { return 1.0f; };
    if (isFlex) {
        auto filter = [&](const GridTrack& t) {
            return t.IsFlexible() && trackIsAffected(t);
        };
        float flexFactorSum = 0.0f;
        for (int i = 0; i < n; i++) {
            if (filter(tracks[i])) {
                flexFactorSum += tracks[i].FlexFactor();
            }
        }
        if (flexFactorSum > 0.0f) {
            auto flexFactor = [](const GridTrack& t) { return t.FlexFactor(); };
            DistributeItemSpaceToBaseSizeInner(space, tracks, n, filter,
                                               flexFactor, trackLimit,
                                               intrinsicContributionType);
        } else {
            DistributeItemSpaceToBaseSizeInner(space, tracks, n, filter, one,
                                               trackLimit,
                                               intrinsicContributionType);
        }
        return;
    }
    DistributeItemSpaceToBaseSizeInner(space, tracks, n, trackIsAffected, one,
                                       trackLimit, intrinsicContributionType);
}

template <typename Affected>
void DistributeItemSpaceToGrowthLimit(float space, GridTrack* tracks, int n,
                                      Affected trackIsAffected) {
    if (space == 0.0f) {
        return;
    }
    int affected = 0;
    for (int i = 0; i < n; i++) {
        if (trackIsAffected(tracks[i])) {
            affected++;
        }
    }
    if (affected == 0) {
        return;
    }

    float trackSizes = 0.0f;
    for (int i = 0; i < n; i++) {
        trackSizes += tracks[i].growthLimit == INFINITY ? tracks[i].baseSize
                                                        : tracks[i].growthLimit;
    }
    float extraSpace = F32Max(0.0f, space - trackSizes);

    auto growthLimitOrBaseSize = [](const GridTrack& t) {
        return t.growthLimit == INFINITY ? t.baseSize : t.growthLimit;
    };
    auto one = [](const GridTrack&) { return 1.0f; };
    auto fitContentLimit = [](const GridTrack& t) {
        return t.FitContentLimit();
    };
    auto growable = [&](const GridTrack& t) {
        return trackIsAffected(t) &&
               (t.infinitelyGrowable || t.growthLimit == INFINITY);
    };
    extraSpace =
        DistributeSpaceUpToLimits(extraSpace, tracks, n, growable, one,
                                  growthLimitOrBaseSize, fitContentLimit);

    if (extraSpace > 0.0f) {
        DistributeSpaceUpToLimits(extraSpace, tracks, n, trackIsAffected, one,
                                  growthLimitOrBaseSize, fitContentLimit);
    }

    for (int i = 0; i < n; i++) {
        GridTrack& t = tracks[i];
        if (t.itemIncurredIncrease > t.growthLimitPlannedIncrease) {
            t.growthLimitPlannedIncrease = t.itemIncurredIncrease;
        }
        t.itemIncurredIncrease = 0.0f;
    }
}

float CrossedFlexFactorSum(const GridTrack* tracks, int count) {
    float sum = 0.0f;
    for (int i = 0; i < count; i++) {
        if (tracks[i].maxTrackSizingFunction.IsFr()) {
            sum += tracks[i].FlexFactor();
        }
    }
    return sum;
}

float InflexibleTrackBaseSizes(const GridTrack* tracks, int count) {
    float sum = 0.0f;
    for (int i = 0; i < count; i++) {
        if (!tracks[i].maxTrackSizingFunction.IsFr()) {
            sum += tracks[i].baseSize;
        }
    }
    return sum;
}

void ResolveIntrinsicTrackSizes(TaffyTree* tree, AbstractAxis axis,
                                GridTrack* axisTracks, int nAxisTracks,
                                const GridTrack* otherAxisTracks,
                                GridItem* items, int nItems,
                                AvailableSpace axisAvailableGridSpace,
                                SizeFOpt innerNodeSize,
                                TrackSizeEstimate estimate) {

    StableSort(items, nItems, [&](const GridItem& a, const GridItem& b) {
        return CmpByCrossFlexThenSpanThenStart(a, b, axis);
    });

    Optf axisInnerNodeSize = Get(innerNodeSize, axis);
    float flexFactorSum = 0.0f;
    for (int i = 0; i < nAxisTracks; i++) {
        flexFactorSum += axisTracks[i].FlexFactor();
    }
    IntrinsicSizeMeasurer sizer = {tree, otherAxisTracks, estimate, axis,
                                   innerNodeSize};
    CalcResolver calc = tree->calc;

    ItemBatcher batcher;
    batcher.axis = axis;
    int batchStart = 0;
    int batchEnd = 0;
    bool isFlex = false;
    while (batcher.Next(items, nItems, &batchStart, &batchEnd, &isFlex)) {
        GridItem* batch = items + batchStart;
        int batchLen = batchEnd - batchStart;
        uint16_t batchSpan = batch[0].Placement(axis).Span();

        if (!isFlex && batchSpan == 1) {
            for (int bi = 0; bi < batchLen; bi++) {
                GridItem* item = &batch[bi];
                int trackIndex = (int)item->PlacementIndexes(axis).start + 1;
                const GridTrack& track = axisTracks[trackIndex];
                Overflow axisOverflow = axis == AbstractAxis::Inline
                                            ? item->overflow.x
                                            : item->overflow.y;

                float newBaseSize = track.baseSize;
                uint64_t minTag = track.minTrackSizingFunction.raw.Tag();
                if (minTag == CompactLength::kMinContentTag) {
                    newBaseSize =
                        F32Max(track.baseSize,
                               sizer.MinContentContribution(item, axisTracks));
                } else if (minTag == CompactLength::kMaxContentTag) {
                    newBaseSize =
                        F32Max(track.baseSize,
                               sizer.MaxContentContribution(item, axisTracks));
                } else if (track.minTrackSizingFunction
                               .BehavesAsAuto(axisInnerNodeSize)) {

                    float space;
                    bool minOrMaxConstraint =
                        axisAvailableGridSpace
                                .kind == AvailableSpace::Kind::MinContent ||
                        axisAvailableGridSpace
                                .kind == AvailableSpace::Kind::MaxContent;

                    if (minOrMaxConstraint &&
                        !IsScrollContainer(axisOverflow)) {
                        float axisMinimumSize = sizer.MinimumContribution(
                            item, axisTracks, nAxisTracks);
                        float axisMinContentSize =
                            sizer.MinContentContribution(item, axisTracks);
                        Optf limit = track.maxTrackSizingFunction.DefiniteLimit(
                            axisInnerNodeSize, calc);
                        space = F32Max(MaybeMin(axisMinContentSize, limit),
                                       axisMinimumSize);
                    } else {
                        space = sizer.MinimumContribution(item, axisTracks,
                                                          nAxisTracks);
                    }
                    newBaseSize = F32Max(track.baseSize, space);
                }

                GridTrack& t = axisTracks[trackIndex];
                t.baseSize = newBaseSize;

                MaxTrackSizingFunction maxFn = t.maxTrackSizingFunction;
                bool hasMinContent = t.HasIntrinsicSizingFunction();
                float minContentContribution = 0.0f;
                if (hasMinContent) {
                    minContentContribution =
                        sizer.MinContentContribution(item, axisTracks);
                }
                bool havePlanned = false;
                float planned = 0.0f;
                if (maxFn.IsFitContent()) {
                    float fitContentLimit = t.FitContentLimit();
                    float maxContentContribution =
                        F32Min(sizer.MaxContentContribution(item, axisTracks),
                               fitContentLimit);

                    if (hasMinContent && !IsScrollContainer(axisOverflow)) {
                        planned = F32Max(minContentContribution,
                                         maxContentContribution);
                    } else {
                        planned = maxContentContribution;
                    }
                    havePlanned = true;
                } else if (maxFn.IsMaxContentAlike() ||
                           (maxFn.UsesPercentage() &&
                            !IsSome(axisInnerNodeSize))) {

                    planned = sizer.MaxContentContribution(item, axisTracks);
                    havePlanned = true;
                } else if (maxFn.IsIntrinsic()) {
                    if (hasMinContent) {
                        planned = minContentContribution;
                        havePlanned = true;
                    }
                }
                if (havePlanned) {
                    t.growthLimitPlannedIncrease =
                        F32Max(t.growthLimitPlannedIncrease, planned);
                }
            }

            for (int i = 0; i < nAxisTracks; i++) {
                GridTrack& t = axisTracks[i];
                if (t.growthLimitPlannedIncrease > 0.0f) {
                    t.growthLimit = t.growthLimit == INFINITY
                                        ? t.growthLimitPlannedIncrease
                                        : F32Max(t.growthLimit,
                                                 t.growthLimitPlannedIncrease);
                }
                t.infinitelyGrowable = false;
                t.growthLimitPlannedIncrease = 0.0f;
                if (t.growthLimit < t.baseSize) {
                    t.growthLimit = t.baseSize;
                }
            }
            continue;
        }

        bool useFlexFactorForDistribution = isFlex && flexFactorSum != 0.0f;

        for (int bi = 0; bi < batchLen; bi++) {
            GridItem* item = &batch[bi];
            if (!item->CrossesIntrinsicTrack(axis)) {
                continue;
            }
            Overflow axisOverflow = axis == AbstractAxis::Inline
                                        ? item->overflow.x
                                        : item->overflow.y;
            bool minOrMaxConstraint =
                axisAvailableGridSpace
                        .kind == AvailableSpace::Kind::MinContent ||
                axisAvailableGridSpace.kind == AvailableSpace::Kind::MaxContent;
            float space;
            if (minOrMaxConstraint && !IsScrollContainer(axisOverflow)) {
                float axisMinimumSize =
                    sizer.MinimumContribution(item, axisTracks, nAxisTracks);
                float axisMinContentSize =
                    sizer.MinContentContribution(item, axisTracks);
                Optf limit = SpannedTrackLimit(*item, axis, axisTracks,
                                               axisInnerNodeSize, calc);
                float limitedMinContent = F32Max(
                    MaybeMin(axisMinContentSize, limit), axisMinimumSize);

                if (isFlex) {
                    int from = item->TrackRangeStart(axis);
                    int count = item->TrackRangeEnd(axis) - from;
                    float inflexible =
                        InflexibleTrackBaseSizes(axisTracks + from, count);
                    float scale = F32Min(
                        CrossedFlexFactorSum(axisTracks + from, count), 1.0f);
                    float excess = F32Max(limitedMinContent - inflexible, 0.0f);
                    space =
                        F32Max(axisMinimumSize, inflexible + excess * scale);
                } else {
                    space = limitedMinContent;
                }
            } else {
                space = sizer
                            .MinimumContribution(item, axisTracks, nAxisTracks);
            }
            int from = item->TrackRangeStart(axis);
            int count = item->TrackRangeEnd(axis) - from;
            if (space > 0.0f && count > 0) {
                auto hasIntrinsicMin = [&](const GridTrack& t) {
                    return !IsSome(t.minTrackSizingFunction
                                       .DefiniteValue(axisInnerNodeSize, calc));
                };
                if (IsScrollContainer(axisOverflow)) {
                    DistributeItemSpaceToBaseSize(
                        isFlex, useFlexFactorForDistribution, space,
                        axisTracks + from, count, hasIntrinsicMin,
                        [](const GridTrack& t) {
                            return t.FitContentLimitedGrowthLimit();
                        },
                        IntrinsicContributionType::Minimum);
                } else {
                    DistributeItemSpaceToBaseSize(
                        isFlex, useFlexFactorForDistribution, space,
                        axisTracks + from, count, hasIntrinsicMin,
                        [](const GridTrack& t) { return t.growthLimit; },
                        IntrinsicContributionType::Minimum);
                }
            }
        }
        FlushPlannedBaseSizeIncreases(axisTracks, nAxisTracks);

        auto hasMinOrMaxContentMin = [](const GridTrack& t) {
            return t.minTrackSizingFunction.IsMinOrMaxContent();
        };
        for (int bi = 0; bi < batchLen; bi++) {
            GridItem* item = &batch[bi];
            if (!item->SpansTrackMatching(axis, axisTracks,
                                          hasMinOrMaxContentMin)) {
                continue;
            }
            Overflow axisOverflow = axis == AbstractAxis::Inline
                                        ? item->overflow.x
                                        : item->overflow.y;
            float space = sizer.MinContentContribution(item, axisTracks);
            int from = item->TrackRangeStart(axis);
            int count = item->TrackRangeEnd(axis) - from;
            if (space > 0.0f && count > 0) {
                if (IsScrollContainer(axisOverflow)) {
                    DistributeItemSpaceToBaseSize(
                        isFlex, useFlexFactorForDistribution, space,
                        axisTracks + from, count, hasMinOrMaxContentMin,
                        [](const GridTrack& t) {
                            return t.FitContentLimitedGrowthLimit();
                        },
                        IntrinsicContributionType::Minimum);
                } else {
                    DistributeItemSpaceToBaseSize(
                        isFlex, useFlexFactorForDistribution, space,
                        axisTracks + from, count, hasMinOrMaxContentMin,
                        [](const GridTrack& t) { return t.growthLimit; },
                        IntrinsicContributionType::Minimum);
                }
            }
        }
        FlushPlannedBaseSizeIncreases(axisTracks, nAxisTracks);

        if (axisAvailableGridSpace.kind == AvailableSpace::Kind::MaxContent) {

            auto hasAutoMin = [](const GridTrack& t) {
                return t.minTrackSizingFunction.IsAuto() &&
                       !t.maxTrackSizingFunction.IsMinContent();
            };
            auto hasMaxContentMin = [](const GridTrack& t) {
                return t.minTrackSizingFunction.IsMaxContent();
            };
            for (int bi = 0; bi < batchLen; bi++) {
                GridItem* item = &batch[bi];
                if (!item->SpansTrackMatching(
                        axis, axisTracks, [&](const GridTrack& t) {
                            return hasAutoMin(t) || hasMaxContentMin(t);
                        })) {
                    continue;
                }
                float axisMaxContentSize =
                    sizer.MaxContentContribution(item, axisTracks);
                Optf limit = SpannedTrackLimit(*item, axis, axisTracks,
                                               axisInnerNodeSize, calc);
                float space = MaybeMin(axisMaxContentSize, limit);

                if (isFlex) {
                    int fromFlex = item->TrackRangeStart(axis);
                    int countFlex = item->TrackRangeEnd(axis) - fromFlex;
                    float inflexible = InflexibleTrackBaseSizes(
                        axisTracks + fromFlex, countFlex);
                    float scale = F32Min(
                        CrossedFlexFactorSum(axisTracks + fromFlex, countFlex),
                        1.0f);
                    space =
                        inflexible + F32Max(space - inflexible, 0.0f) * scale;
                }
                int from = item->TrackRangeStart(axis);
                int count = item->TrackRangeEnd(axis) - from;
                if (space <= 0.0f || count <= 0) {
                    continue;
                }
                bool anyMaxContentMin = false;
                for (int k = from; k < from + count; k++) {
                    if (hasMaxContentMin(axisTracks[k])) {
                        anyMaxContentMin = true;
                        break;
                    }
                }

                if (anyMaxContentMin) {
                    DistributeItemSpaceToBaseSize(
                        isFlex, useFlexFactorForDistribution, space,
                        axisTracks + from, count, hasMaxContentMin,
                        [](const GridTrack&) { return INFINITY; },
                        IntrinsicContributionType::Maximum);
                } else {
                    DistributeItemSpaceToBaseSize(
                        isFlex, useFlexFactorForDistribution, space,
                        axisTracks + from, count, hasAutoMin,
                        [](const GridTrack& t) {
                            return t.FitContentLimitedGrowthLimit();
                        },
                        IntrinsicContributionType::Maximum);
                }
            }
            FlushPlannedBaseSizeIncreases(axisTracks, nAxisTracks);
        }

        auto hasMaxContentMinFn = [](const GridTrack& t) {
            return t.minTrackSizingFunction.IsMaxContent();
        };
        for (int bi = 0; bi < batchLen; bi++) {
            GridItem* item = &batch[bi];
            if (!item->SpansTrackMatching(axis, axisTracks,
                                          hasMaxContentMinFn)) {
                continue;
            }
            float space = sizer.MaxContentContribution(item, axisTracks);
            int from = item->TrackRangeStart(axis);
            int count = item->TrackRangeEnd(axis) - from;
            if (space > 0.0f && count > 0) {
                DistributeItemSpaceToBaseSize(
                    isFlex, useFlexFactorForDistribution, space,
                    axisTracks + from, count, hasMaxContentMinFn,
                    [](const GridTrack& t) { return t.growthLimit; },
                    IntrinsicContributionType::Maximum);
            }
        }
        FlushPlannedBaseSizeIncreases(axisTracks, nAxisTracks);

        for (int i = 0; i < nAxisTracks; i++) {
            if (axisTracks[i].growthLimit < axisTracks[i].baseSize) {
                axisTracks[i].growthLimit = axisTracks[i].baseSize;
            }
        }

        if (!isFlex) {

            auto hasIntrinsicMax = [&](const GridTrack& t) {
                return !t.maxTrackSizingFunction
                            .HasDefiniteValue(axisInnerNodeSize);
            };
            for (int bi = 0; bi < batchLen; bi++) {
                GridItem* item = &batch[bi];
                if (!item->SpansTrackMatching(axis, axisTracks,
                                              hasIntrinsicMax)) {
                    continue;
                }
                float space = sizer.MinContentContribution(item, axisTracks);
                int from = item->TrackRangeStart(axis);
                int count = item->TrackRangeEnd(axis) - from;
                if (space > 0.0f && count > 0) {
                    DistributeItemSpaceToGrowthLimit(space, axisTracks + from,
                                                     count, hasIntrinsicMax);
                }
            }

            FlushPlannedGrowthLimitIncreases(axisTracks, nAxisTracks, true);

            auto hasMaxContentMax = [&](const GridTrack& t) {
                return t.maxTrackSizingFunction.IsMaxContentAlike() ||
                       (t.maxTrackSizingFunction.UsesPercentage() &&
                        !IsSome(axisInnerNodeSize));
            };
            for (int bi = 0; bi < batchLen; bi++) {
                GridItem* item = &batch[bi];
                if (!item->SpansTrackMatching(axis, axisTracks,
                                              hasMaxContentMax)) {
                    continue;
                }
                float space = sizer.MaxContentContribution(item, axisTracks);
                int from = item->TrackRangeStart(axis);
                int count = item->TrackRangeEnd(axis) - from;
                if (space > 0.0f && count > 0) {
                    DistributeItemSpaceToGrowthLimit(space, axisTracks + from,
                                                     count, hasMaxContentMax);
                }
            }
            FlushPlannedGrowthLimitIncreases(axisTracks, nAxisTracks, false);
        }
    }

    for (int i = 0; i < nAxisTracks; i++) {
        if (axisTracks[i].growthLimit == INFINITY) {
            axisTracks[i].growthLimit = axisTracks[i].baseSize;
        }
    }
}

void MaximiseTracks(GridTrack* axisTracks, int n,
                    AvailableSpace axisAvailableGridSpace) {
    float usedSpace = 0.0f;
    for (int i = 0; i < n; i++) {
        usedSpace += axisTracks[i].baseSize;
    }
    float freeSpace = axisAvailableGridSpace.ComputeFreeSpace(usedSpace);
    if (freeSpace == INFINITY) {
        for (int i = 0; i < n; i++) {
            axisTracks[i].baseSize = axisTracks[i].growthLimit;
        }
    } else if (freeSpace > 0.0f) {
        DistributeSpaceUpToLimits(
            freeSpace, axisTracks, n, [](const GridTrack&) { return true; },
            [](const GridTrack&) { return 1.0f; },
            [](const GridTrack& t) { return t.baseSize; },
            [](const GridTrack& t) {
                return t.FitContentLimitedGrowthLimit();
            });
        for (int i = 0; i < n; i++) {
            axisTracks[i].baseSize += axisTracks[i].itemIncurredIncrease;
            axisTracks[i].itemIncurredIncrease = 0.0f;
        }
    }
}

float FindSizeOfFr(const GridTrack* tracks, int n, float spaceToFill) {

    if (spaceToFill == 0.0f) {
        return 0.0f;
    }

    float hypotheticalFrSize = INFINITY;
    float previousIterHypotheticalFrSize;

    int maxIterations = n + 1;
    for (int iter = 0; iter < maxIterations; iter++) {
        float usedSpace = 0.0f;
        float naiveFlexFactorSum = 0.0f;
        for (int i = 0; i < n; i++) {
            const GridTrack& t = tracks[i];
            if (t.maxTrackSizingFunction.IsFr() &&
                t.maxTrackSizingFunction.raw.Value() * hypotheticalFrSize >=
                    t.baseSize) {
                naiveFlexFactorSum += t.maxTrackSizingFunction.raw.Value();
            } else {
                usedSpace += t.baseSize;
            }
        }
        float leftoverSpace = spaceToFill - usedSpace;
        float flexFactor = F32Max(naiveFlexFactorSum, 1.0f);

        previousIterHypotheticalFrSize = hypotheticalFrSize;
        hypotheticalFrSize = leftoverSpace / flexFactor;

        bool valid = true;
        for (int i = 0; i < n; i++) {
            const GridTrack& t = tracks[i];
            if (!t.maxTrackSizingFunction.IsFr()) {
                continue;
            }
            float ff = t.maxTrackSizingFunction.raw.Value();
            if (!(ff * hypotheticalFrSize >= t.baseSize ||
                  ff * previousIterHypotheticalFrSize < t.baseSize)) {
                valid = false;
                break;
            }
        }
        if (valid) {
            break;
        }
    }
    return hypotheticalFrSize;
}

void ExpandFlexibleTracks(TaffyTree* tree, AbstractAxis axis,
                          GridTrack* axisTracks, int nAxisTracks,
                          const GridTrack* otherAxisTracks, GridItem* items,
                          int nItems, Optf axisMinSize, Optf axisMaxSize,
                          AvailableSpace axisAvailableSpaceForExpansion,
                          SizeFOpt innerNodeSize, TrackSizeEstimate estimate) {
    float flexFraction = 0.0f;
    if (axisAvailableSpaceForExpansion.kind == AvailableSpace::Kind::Definite) {
        float availableSpace = axisAvailableSpaceForExpansion.value;
        float usedSpace = 0.0f;
        for (int i = 0; i < nAxisTracks; i++) {
            usedSpace += axisTracks[i].baseSize;
        }
        float freeSpace = availableSpace - usedSpace;
        flexFraction = freeSpace <= 0.0f ? 0.0f
                                         : FindSizeOfFr(axisTracks, nAxisTracks,
                                                        availableSpace);
    } else if (axisAvailableSpaceForExpansion
                   .kind == AvailableSpace::Kind::MinContent) {

        flexFraction = 0.0f;
    } else {

        float trackMax = 0.0f;
        for (int i = 0; i < nAxisTracks; i++) {
            const GridTrack& t = axisTracks[i];
            if (!t.maxTrackSizingFunction.IsFr()) {
                continue;
            }
            float ff = t.FlexFactor();
            float v = ff > 1.0f ? t.baseSize / ff : t.baseSize;
            trackMax = F32Max(trackMax, v);
        }
        float itemMax = 0.0f;
        IntrinsicSizeMeasurer sizer;
        sizer.tree = tree;
        sizer.otherAxisTracks = otherAxisTracks;
        sizer.estimate = estimate;
        sizer.axis = axis;
        sizer.innerNodeSize = innerNodeSize;
        for (int i = 0; i < nItems; i++) {
            GridItem* item = &items[i];
            if (!item->CrossesFlexibleTrack(axis)) {
                continue;
            }
            int from = item->TrackRangeStart(axis);
            int count = item->TrackRangeEnd(axis) - from;

            float maxContentContribution =
                sizer.MaxContentContribution(item, axisTracks);
            itemMax = F32Max(itemMax, FindSizeOfFr(axisTracks + from, count,
                                                   maxContentContribution));
        }
        flexFraction = F32Max(trackMax, itemMax);

        float hypotheticalGridSize = 0.0f;
        for (int i = 0; i < nAxisTracks; i++) {
            const GridTrack& t = axisTracks[i];
            if (t.maxTrackSizingFunction.IsFr()) {
                hypotheticalGridSize +=
                    F32Max(t.baseSize,
                           t.maxTrackSizingFunction.raw.Value() * flexFraction);
            } else {
                hypotheticalGridSize += t.baseSize;
            }
        }
        float minSize = UnwrapOr(axisMinSize, 0.0f);
        float maxSize = UnwrapOr(axisMaxSize, INFINITY);
        if (hypotheticalGridSize < minSize) {
            flexFraction = FindSizeOfFr(axisTracks, nAxisTracks, minSize);
        } else if (hypotheticalGridSize > maxSize) {
            flexFraction = FindSizeOfFr(axisTracks, nAxisTracks, maxSize);
        }
    }

    for (int i = 0; i < nAxisTracks; i++) {
        GridTrack& t = axisTracks[i];
        if (!t.maxTrackSizingFunction.IsFr()) {
            continue;
        }
        t.baseSize = F32Max(
            t.baseSize, t.maxTrackSizingFunction.raw.Value() * flexFraction);
    }
}

void StretchAutoTracks(GridTrack* axisTracks, int n, Optf axisMinSize,
                       AvailableSpace axisAvailableSpaceForExpansion) {
    int numAutoTracks = 0;
    for (int i = 0; i < n; i++) {
        if (axisTracks[i].maxTrackSizingFunction.IsAuto()) {
            numAutoTracks++;
        }
    }
    if (numAutoTracks == 0) {
        return;
    }
    float usedSpace = 0.0f;
    for (int i = 0; i < n; i++) {
        usedSpace += axisTracks[i].baseSize;
    }

    float freeSpace;
    if (axisAvailableSpaceForExpansion.IsDefinite()) {
        freeSpace = axisAvailableSpaceForExpansion.ComputeFreeSpace(usedSpace);
    } else {
        freeSpace = IsSome(axisMinSize) ? axisMinSize - usedSpace : 0.0f;
    }
    if (freeSpace > 0.0f) {
        float extra = freeSpace / (float)numAutoTracks;
        for (int i = 0; i < n; i++) {
            if (axisTracks[i].maxTrackSizingFunction.IsAuto()) {
                axisTracks[i].baseSize += extra;
            }
        }
    }
}

void TrackSizingAlgorithm(TaffyTree* tree, AbstractAxis axis, Optf axisMinSize,
                          Optf axisMaxSize, AlignContent axisAlignment,
                          AlignContent otherAxisAlignment,
                          SizeAvail availableGridSpace, SizeFOpt innerNodeSize,
                          GridTrack* axisTracks, int nAxisTracks,
                          GridTrack* otherAxisTracks, int nOtherAxisTracks,
                          GridItem* items, int nItems,
                          TrackSizeEstimate estimate,
                          bool hasBaselineAlignedItem) {

    Optf percentageBasis = Or(Get(innerNodeSize, axis), axisMinSize);
    InitializeTrackSizes(tree, axisTracks, nAxisTracks, percentageBasis);

    Optf axisInnerNodeSize = Get(innerNodeSize, axis);
    for (int i = 0; i < nAxisTracks; i++) {
        axisTracks[i].ResolveFitContentLimit(axisInnerNodeSize, tree->calc);
    }

    if (hasBaselineAlignedItem) {
        ResolveItemBaselines(tree, axis, items, nItems, innerNodeSize);
    }

    bool allAtLimit = true;
    for (int i = 0; i < nAxisTracks; i++) {
        const GridTrack& t = axisTracks[i];
        if (t.baseSize != t.growthLimit ||
            !IsSome(t.minTrackSizingFunction
                        .DefiniteValue(percentageBasis, tree->calc))) {
            allAtLimit = false;
            break;
        }
    }
    if (allAtLimit) {
        return;
    }

    float gutterAlignmentAdjustment = ComputeAlignmentGutterAdjustment(
        otherAxisAlignment, Get(innerNodeSize, Other(axis)), otherAxisTracks,
        nOtherAxisTracks, estimate, tree->calc);
    if (nOtherAxisTracks > 3) {
        for (int i = 2; i < nOtherAxisTracks; i += 2) {
            otherAxisTracks[i]
                .contentAlignmentAdjustment = gutterAlignmentAdjustment;
        }
    }

    ResolveIntrinsicTrackSizes(
        tree, axis, axisTracks, nAxisTracks, otherAxisTracks, items, nItems,
        availableGridSpace.Get(axis), innerNodeSize, estimate);

    MaximiseTracks(axisTracks, nAxisTracks, availableGridSpace.Get(axis));

    AvailableSpace axisAvailableSpaceForExpansion;
    Optf innerAxis = Get(innerNodeSize, axis);
    if (IsSome(innerAxis)) {
        axisAvailableSpaceForExpansion = AvailableSpace::Definite(innerAxis);
    } else if (axis == AbstractAxis::Inline &&
               availableGridSpace.Get(axis)
                       .kind == AvailableSpace::Kind::MinContent) {
        axisAvailableSpaceForExpansion = AvailableSpace::MinContent();
    } else {
        axisAvailableSpaceForExpansion = AvailableSpace::MaxContent();
    }

    ExpandFlexibleTracks(tree, axis, axisTracks, nAxisTracks, otherAxisTracks,
                         items, nItems, axisMinSize, axisMaxSize,
                         axisAvailableSpaceForExpansion, innerNodeSize,
                         estimate);

    if (axisAlignment.keyword == AlignContentKeyword::Stretch) {
        StretchAutoTracks(axisTracks, nAxisTracks, axisMinSize,
                          axisAvailableSpaceForExpansion);
    }
}

void AlignTracks(float gridContainerContentBoxSize, LineF padding, LineF border,
                 GridTrack* tracks, int n, AlignContent trackAlignmentStyle,
                 bool axisIsReversed) {
    float usedSize = 0.0f;
    for (int i = 0; i < n; i++) {
        usedSize += tracks[i].baseSize;
    }
    float freeSpace = gridContainerContentBoxSize - usedSize;
    float origin = padding.start + border.start;

    int numTracks = 0;
    for (int i = 1; i < n; i += 2) {
        if (!tracks[i].isCollapsed) {
            numTracks++;
        }
    }

    float gap = 0.0f;
    bool layoutIsReversed = false;
    AlignContentKeyword trackAlignment =
        ApplyAlignmentFallback(freeSpace, numTracks, trackAlignmentStyle);
    if (axisIsReversed) {
        trackAlignment = Reversed(trackAlignment);
    }

    float emptyGridOffset =
        numTracks == 0
            ? ComputeAlignmentOffset(freeSpace, numTracks, gap, trackAlignment,
                                     layoutIsReversed, true)
            : 0.0f;
    float totalOffset = origin + emptyGridOffset;
    bool seenNonCollapsedTrack = false;

    auto positionTrack = [&](int i, GridTrack& track) {
        bool isGutter = (i % 2) == 0;
        bool isNonCollapsedTrack = !isGutter && !track.isCollapsed;
        bool isFirst = isNonCollapsedTrack && !seenNonCollapsedTrack;

        float offset = isNonCollapsedTrack
                           ? ComputeAlignmentOffset(freeSpace, numTracks, gap,
                                                    trackAlignment,
                                                    layoutIsReversed, isFirst)
                           : 0.0f;
        track.offset = totalOffset + offset;
        totalOffset = totalOffset + offset + track.baseSize;
        if (isNonCollapsedTrack) {
            seenNonCollapsedTrack = true;
        }
    };
    if (axisIsReversed) {
        for (int i = 0; i < n; i++) {
            positionTrack(i, tracks[n - 1 - i]);
        }
    } else {
        for (int i = 0; i < n; i++) {
            positionTrack(i, tracks[i]);
        }
    }
}

struct AlignedAxis {
    float start = 0.0f;
    LineF margin;
};

AlignedAxis AlignItemWithinArea(LineF gridArea, AlignSelf alignmentStyle,
                                float resolvedSize, Position position,
                                RectFOpt insetLine, bool vertical,
                                RectFOpt marginLine, float baselineShim,
                                Direction direction) {
    Optf insetStart = vertical ? insetLine.top : insetLine.left;
    Optf insetEnd = vertical ? insetLine.bottom : insetLine.right;
    Optf marginStart = vertical ? marginLine.top : marginLine.left;
    Optf marginEnd = vertical ? marginLine.bottom : marginLine.right;

    LineF nonAutoMargin = {UnwrapOr(marginStart, 0.0f) + baselineShim,
                           UnwrapOr(marginEnd, 0.0f)};
    float gridAreaSize = F32Max(gridArea.end - gridArea.start, 0.0f);
    float freeSpace =
        F32Max(gridAreaSize - resolvedSize - nonAutoMargin.Sum(), 0.0f);

    int autoMarginCount =
        (IsSome(marginStart) ? 0 : 1) + (IsSome(marginEnd) ? 0 : 1);
    float autoMarginSize =
        autoMarginCount > 0 ? freeSpace / (float)autoMarginCount : 0.0f;
    LineF resolvedMargin = {
        UnwrapOr(marginStart, autoMarginSize) + baselineShim,
        UnwrapOr(marginEnd, autoMarginSize)};

    bool overflows = resolvedSize + nonAutoMargin.Sum() > gridAreaSize;
    AlignItemsKeyword keyword =
        ResolveSelfAlignmentSafety(alignmentStyle, overflows);

    float alignmentBasedOffset;
    switch (keyword) {
        case AlignItemsKeyword::End:
        case AlignItemsKeyword::FlexEnd:
            alignmentBasedOffset =
                IsRtl(direction)
                    ? resolvedMargin.start
                    : gridAreaSize - resolvedSize - resolvedMargin.end;
            break;
        case AlignItemsKeyword::Center:
            alignmentBasedOffset = (gridAreaSize - resolvedSize +
                                    resolvedMargin.start - resolvedMargin.end) /
                                   2.0f;
            break;
        default:

            alignmentBasedOffset =
                IsRtl(direction)
                    ? gridAreaSize - resolvedSize - resolvedMargin.end
                    : resolvedMargin.start;
            break;
    }

    float offsetWithinArea = alignmentBasedOffset;
    if (IsOutOfFlow(position)) {
        if (IsSome(insetStart) && IsSome(insetEnd)) {
            offsetWithinArea =
                IsRtl(direction)
                    ? gridAreaSize - insetEnd - resolvedSize - nonAutoMargin.end
                    : insetStart + nonAutoMargin.start;
        } else if (IsSome(insetStart)) {
            offsetWithinArea = insetStart + nonAutoMargin.start;
        } else if (IsSome(insetEnd)) {
            offsetWithinArea =
                gridAreaSize - insetEnd - resolvedSize - nonAutoMargin.end;
        }
    }

    float start = gridArea.start + offsetWithinArea;
    if (position == Position::Relative) {
        Optf negEnd = insetEnd;
        if (IsSome(negEnd)) {
            negEnd = -negEnd;
        }
        Optf relativeInset =
            IsRtl(direction) ? Or(negEnd, insetStart) : Or(insetStart, negEnd);
        start += UnwrapOr(relativeInset, 0.0f);
    }
    return {start, resolvedMargin};
}

struct AlignedItem {
    RectF overflowContribution;
    float yPosition = 0.0f;
    float height = 0.0f;
    int oofStart = 0;
    int oofCount = 0;
};

AlignedItem AlignAndPositionItem(
    TaffyTree* tree, NodeId node, uint32_t order, RectF gridArea,
    SizeF containingBlock, AlignItems containerJustifyItems,
    AlignItems containerAlignItems, float baselineShim, Direction direction,
    float containerBorderBoxWidth, RectF containerBorder,
    bool containerIsScrollContainer, Vec<OofCandidate>* oofPool) {
    CalcResolver calc = tree->calc;
    SizeF gridAreaSize = {gridArea.right - gridArea.left,
                          gridArea.bottom - gridArea.top};
    SizeFOpt containingOpt = AsOptional(containingBlock);

    const Style& style = tree->GetStyle(node);
    PointOverflow overflow = style.overflow;
    Contain contain = style.contain;
    float scrollbarWidth = style.scrollbarWidth;
    Optf aspectRatio = style.aspectRatio;
    bool isReplaced = style.IsReplaced();

    AlignItems justifySelf =
        ResolveSelfRelative(style.justifySelf.UnwrapOr(containerJustifyItems),
                            style.direction, direction, true);
    AlignItems alignSelf =
        ResolveSelfRelative(style.alignSelf.UnwrapOr(containerAlignItems),
                            style.direction, direction, false);
    Position position = style.position;

    RectFOpt inset = style.inset.MaybeResolveZip(containingOpt, calc);
    RectF padding = style.padding.ResolveOrZero(Some(containingBlock.w), calc);
    RectF border = style.border.ResolveOrZero(Some(containingBlock.w), calc);
    SizeF paddingBorderSize = (padding + border).SumAxes();
    SizeF boxSizingAdjustment = style.boxSizing == BoxSizing::ContentBox
                                    ? paddingBorderSize
                                    : SizeF::Zero();

    SizeDim sizeStyle = style.size;
    SizeFOpt inherentSize =
        MaybeAdd(MaybeApplyAspectRatio(
                     sizeStyle.MaybeResolve(containingOpt, calc), aspectRatio),
                 boxSizingAdjustment);
    SizeFOpt minSize = MaybeApplyAspectRatio(
        MaybeMax(Or(MaybeAdd(style.minSize.MaybeResolve(containingOpt, calc),
                             boxSizingAdjustment),
                    AsOptional(paddingBorderSize)),
                 paddingBorderSize),
        aspectRatio);
    SizeFOpt maxSize = MaybeAdd(
        MaybeApplyAspectRatio(style.maxSize.MaybeResolve(containingOpt, calc),
                              aspectRatio),
        boxSizingAdjustment);

    AlignItems horizontalAlignment = justifySelf;
    if (justifySelf.keyword == AlignItemsKeyword::Normal) {
        horizontalAlignment = (IsSome(inherentSize.w) ||
                               sizeStyle.width.IsSizingKeyword() || isReplaced)
                                  ? AlignItems{AlignItemsKeyword::Start}
                                  : AlignItems{AlignItemsKeyword::Stretch};
    }
    AlignItems verticalAlignment = alignSelf;
    if (alignSelf.keyword == AlignItemsKeyword::Normal) {
        verticalAlignment =
            (IsSome(inherentSize.h) || sizeStyle.height.IsSizingKeyword() ||
             IsSome(aspectRatio) || isReplaced)
                ? AlignItems{AlignItemsKeyword::Start}
                : AlignItems{AlignItemsKeyword::Stretch};
    }

    RectFOpt margin = style.margin.MaybeResolve(Some(containingBlock.w), calc);

    SizeF gridAreaMinusItemMarginsSize = {
        F32Max(MaybeSub(MaybeSub(gridAreaSize.w, margin.left), margin.right),
               0.0f),
        F32Max(MaybeSub(MaybeSub(gridAreaSize.h, margin.top), margin.bottom) -
                   baselineShim,
               0.0f)};

    SizingKeywordResolution keywordWidth;
    if (!IsSome(inherentSize.w)) {
        keywordWidth = ResolveSizingKeyword(
            sizeStyle.width, Some(gridAreaMinusItemMarginsSize.w),
            Some(containingBlock.w), calc);
    }
    SizingKeywordResolution keywordHeight;
    if (!IsSome(inherentSize.h)) {
        keywordHeight = ResolveSizingKeyword(
            sizeStyle.height, Some(gridAreaMinusItemMarginsSize.h),
            Some(containingBlock.h), calc);
    }

    SizeFOpt keywordMeasured = SizeFOptNone();
    if (!IsOutOfFlow(position) && keywordWidth.isSome && keywordHeight.isSome &&
        keywordWidth.kind == SizingKeywordKind::Measure &&
        keywordHeight.kind == SizingKeywordKind::Measure) {
        keywordMeasured = AsOptional(tree->MeasureChildSizeBoth(
            node, SizeFOptNone(), containingOpt,
            {keywordWidth.measure, keywordHeight.measure},
            SizingMode::InherentSize, LineBool::False()));
    }

    Optf width = inherentSize.w;
    if (!IsSome(width)) {
        if (IsOutOfFlow(position) && IsSome(inset.left) &&
            IsSome(inset.right)) {
            width = Some(F32Max(
                gridAreaMinusItemMarginsSize.w - inset.left - inset.right,
                0.0f));
        } else if (keywordWidth.isSome) {
            if (keywordWidth.kind == SizingKeywordKind::Exact) {
                width = Some(keywordWidth.exact);
            } else if (IsSome(keywordMeasured.w)) {
                width = keywordMeasured.w;
            } else {
                width = Some(tree->MeasureChildSize(
                    node, SizeFOptNone(), containingOpt,
                    {keywordWidth.measure,
                     AvailableSpace::Definite(gridAreaMinusItemMarginsSize.h)},
                    SizingMode::InherentSize, AbsoluteAxis::Horizontal,
                    LineBool::False()));
            }
        } else if (IsSome(margin.left) && IsSome(margin.right) &&
                   horizontalAlignment.keyword == AlignItemsKeyword::Stretch &&
                   !IsOutOfFlow(position)) {
            width = Some(gridAreaMinusItemMarginsSize.w);
        }
    }
    SizeFOpt sized =
        MaybeApplyAspectRatio(SizeFOpt{width, inherentSize.h}, aspectRatio);

    Optf height = sized.h;
    if (!IsSome(height)) {
        if (IsOutOfFlow(position) && IsSome(inset.top) &&
            IsSome(inset.bottom)) {
            height = Some(F32Max(
                gridAreaMinusItemMarginsSize.h - inset.top - inset.bottom,
                0.0f));
        } else if (keywordHeight.isSome) {
            if (keywordHeight.kind == SizingKeywordKind::Exact) {
                height = Some(keywordHeight.exact);
            } else if (IsSome(keywordMeasured.h)) {
                height = keywordMeasured.h;
            } else {
                AvailableSpace widthAvail =
                    IsSome(width) ? AvailableSpace::Definite(width)
                                  : AvailableSpace::Definite(
                                        gridAreaMinusItemMarginsSize.w);
                height = Some(tree->MeasureChildSize(
                    node, {width, None()}, containingOpt,
                    {widthAvail, keywordHeight.measure},
                    SizingMode::InherentSize, AbsoluteAxis::Vertical,
                    LineBool::False()));
            }
        } else if (IsSome(margin.top) && IsSome(margin.bottom) &&
                   verticalAlignment.keyword == AlignItemsKeyword::Stretch &&
                   !IsOutOfFlow(position)) {
            height = Some(gridAreaMinusItemMarginsSize.h);
        }
    }
    sized = MaybeApplyAspectRatio(SizeFOpt{sized.w, height}, aspectRatio);
    sized = MaybeClamp(sized, minSize, maxSize);

    SizeAvail avail = SizeAvail::Definite(gridAreaMinusItemMarginsSize);
    SizeFOpt size = sized;
    if (IsOutOfFlow(position) && (!IsSome(sized.w) || !IsSome(sized.h))) {
        SizeF measured = tree->MeasureChildSizeBoth(
            node, sized, containingOpt, avail, SizingMode::InherentSize,
            LineBool::False());
        size = AsOptional(measured);
    }

    LayoutOutput layoutOutput =
        tree->PerformChildLayout(node, size, containingOpt, avail,
                                 SizingMode::InherentSize, LineBool::False());

    SizeF finalSize =
        MaybeClamp(UnwrapOr(size, layoutOutput.size), minSize, maxSize);

    AlignedAxis xr = AlignItemWithinArea(
        {gridArea.left, gridArea.right}, horizontalAlignment, finalSize.w,
        position, inset, false, margin, 0.0f, direction);
    AlignedAxis yr = AlignItemWithinArea(
        {gridArea.top, gridArea.bottom}, verticalAlignment, finalSize.h,
        position, inset, true, margin, baselineShim, Direction::Ltr);

    SizeF scrollbarSize = {
        overflow.y == Overflow::Scroll ? scrollbarWidth : 0.0f,
        overflow.x == Overflow::Scroll ? scrollbarWidth : 0.0f};

    Layout layout;
    layout.order = order;
    layout.location = {xr.start, yr.start};
    layout.size = finalSize;
    layout.scrollableOverflowRect = layoutOutput.scrollableOverflowRect;
    layout.scrollbarSize = scrollbarSize;
    layout.padding = padding;
    layout.border = border;
    layout.margin = {xr.margin.start, xr.margin.end, yr.margin.start,
                     yr.margin.end};
    tree->SetUnroundedLayout(node, layout);

    RectF contribution = ComputeScrollableOverflowContribution(
        {IsRtl(direction) ? containerBorderBoxWidth - (xr.start + finalSize.w) -
                                containerBorder.right
                          : xr.start - containerBorder.left,
         yr.start - containerBorder.top},
        finalSize, layoutOutput.scrollableOverflowRect, overflow, contain,
        containerIsScrollContainer);
    OofCandRange range;
    OofAppendRange(oofPool, &range, layoutOutput.oofCandidates);
    if (range.count > 0) {
        OofTranslateRange(oofPool, range, {xr.start, yr.start});
    }
    return {contribution, yr.start, finalSize.h, range.start, range.count};
}

void StoreGridOofSnapshot(TaffyTree* tree, NodeId node, uint16_t colAuto,
                          uint16_t rowAuto, TrackCounts colCounts,
                          TrackCounts rowCounts, const Vec<GridTrack>& columns,
                          const Vec<GridTrack>& rows) {
    NodeData* d = tree->Get(node);
    if (!d) {
        return;
    }
    auto fill = [](GridAxisPositions* axis, uint16_t autoRep,
                   TrackCounts counts, const Vec<GridTrack>& tracks) {
        axis->negativeImplicit = counts.negativeImplicit;
        axis->explicitCount = counts.explicitCount;
        axis->positiveImplicit = counts.positiveImplicit;
        axis->autoRepetitions = autoRep;
        for (int i = 0; i < len(tracks); i++) {
            if (tracks[i].kind != GridTrackKind::Track) {
                continue;
            }
            if (tracks[i].isCollapsed) {
                int index = len(axis->spans);
                if (len(axis->collapsed) > 0 &&
                    axis->collapsed[len(axis->collapsed) - 1].end == index) {
                    axis->collapsed[len(axis->collapsed) - 1].end = index + 1;
                } else {
                    CollapsedRun run;
                    run.start = index;
                    run.end = index + 1;
                    VecAppend(axis->collapsed, run);
                }
            }
            GridTrackSpan span;
            span.start = tracks[i].offset;
            span.end = tracks[i].offset + tracks[i].baseSize;
            VecAppend(axis->spans, span);
        }
    };
    d->gridOof.Clear();
    d->gridOof.live = true;
    fill(&d->gridOof.columns, colAuto, colCounts, columns);
    fill(&d->gridOof.rows, rowAuto, rowCounts, rows);
}

}

LayoutOutput ComputeGridLayout(TaffyTree* tree, NodeId node,
                               const LayoutInput& inputs) {
    CalcResolver calc = tree->calc;
    SizeFOpt knownDimensions = inputs.knownDimensions;
    SizeFOpt parentSize = inputs.parentSize;
    SizeAvail availableSpace = inputs.availableSpace;
    RunMode runMode = inputs.runMode;

    const Style& style = tree->GetStyle(node);
    Direction direction = style.direction;
    Contain contain = style.contain;

    Optf aspectRatio = style.aspectRatio;
    RectF padding = style.padding.ResolveOrZero(parentSize.w, calc);
    RectF border = style.border.ResolveOrZero(parentSize.w, calc);
    RectF paddingBorder = padding + border;
    SizeF paddingBorderSize = paddingBorder.SumAxes();
    SizeF boxSizingAdjustment = style.boxSizing == BoxSizing::ContentBox
                                    ? paddingBorderSize
                                    : SizeF::Zero();

    SizeFOpt minSize =
        MaybeAdd(MaybeApplyAspectRatio(
                     style.minSize.MaybeResolve(parentSize, calc), aspectRatio),
                 boxSizingAdjustment);
    SizeFOpt maxSize =
        MaybeAdd(MaybeApplyAspectRatio(
                     style.maxSize.MaybeResolve(parentSize, calc), aspectRatio),
                 boxSizingAdjustment);
    SizeFOpt preferredSize = SizeFOptNone();
    if (inputs.sizingMode == SizingMode::InherentSize) {
        preferredSize = MaybeAdd(
            MaybeApplyAspectRatio(style.size.MaybeResolve(parentSize, calc),
                                  aspectRatio),
            boxSizingAdjustment);
    }

    PointOverflow t = style.overflow.Transpose();
    bool isScrollContainer = IsScrollContainer(style.overflow.x) ||
                             IsScrollContainer(style.overflow.y);
    PointF scrollbarGutter = {
        t.x == Overflow::Scroll ? style.scrollbarWidth : 0.0f,
        t.y == Overflow::Scroll ? style.scrollbarWidth : 0.0f};
    RectF contentBoxInset = paddingBorder;
    contentBoxInset.bottom += scrollbarGutter.y;
    if (direction == Direction::Ltr) {
        contentBoxInset.right += scrollbarGutter.x;
    } else {
        contentBoxInset.left += scrollbarGutter.x;
    }

    AlignContent alignContent =
        style.alignContent.keyword == AlignContentKeyword::Normal
            ? AlignContent{AlignContentKeyword::Stretch}
            : style.alignContent;
    AlignContent justifyContent =
        style.justifyContent.keyword == AlignContentKeyword::Normal
            ? AlignContent{AlignContentKeyword::Stretch}
            : style.justifyContent;
    AlignItems alignItems = style.alignItems;
    AlignItems justifyItems = style.justifyItems;

    SizeFOpt sizeOrPreferred = Or(knownDimensions, preferredSize);

    SizeAvail constrainedAvailableSpace = availableSpace;
    if (IsSome(sizeOrPreferred.w)) {
        constrainedAvailableSpace
            .width = AvailableSpace::Definite(sizeOrPreferred.w);
    }
    if (IsSome(sizeOrPreferred.h)) {
        constrainedAvailableSpace
            .height = AvailableSpace::Definite(sizeOrPreferred.h);
    }
    constrainedAvailableSpace =
        MaybeClamp(constrainedAvailableSpace, minSize, maxSize);
    constrainedAvailableSpace
        .width = MaybeMax(constrainedAvailableSpace.width, paddingBorderSize.w);
    constrainedAvailableSpace.height =
        MaybeMax(constrainedAvailableSpace.height, paddingBorderSize.h);

    SizeAvail availableGridSpace = constrainedAvailableSpace;
    if (availableGridSpace.width.IsDefinite()) {
        availableGridSpace.width =
            AvailableSpace::Definite(availableGridSpace.width.value -
                                     contentBoxInset.HorizontalAxisSum());
    }
    if (availableGridSpace.height.IsDefinite()) {
        availableGridSpace.height =
            AvailableSpace::Definite(availableGridSpace.height.value -
                                     contentBoxInset.VerticalAxisSum());
    }

    SizeFOpt outerNodeSize = MaybeMax(
        MaybeClamp(sizeOrPreferred, minSize, maxSize), paddingBorderSize);
    SizeFOpt innerNodeSize = {
        MaybeSub(outerNodeSize.w, contentBoxInset.HorizontalAxisSum()),
        MaybeSub(outerNodeSize.h, contentBoxInset.VerticalAxisSum())};
    SizeFOpt innerMinSize = MaybeSub(minSize, contentBoxInset.SumAxes());
    SizeFOpt innerMaxSize = MaybeSub(maxSize, contentBoxInset.SumAxes());

    if (runMode == RunMode::ComputeSize) {
        if (BothAxisDefined(outerNodeSize)) {
            return LayoutOutput::FromOuterSize(
                {outerNodeSize.w, outerNodeSize.h});
        }
        if (inputs.axis == RequestedAxis::Horizontal &&
            IsSome(outerNodeSize.w)) {
            return LayoutOutput::FromOuterSize({outerNodeSize.w, 0.0f});
        }
    }

    SizeFOpt autoFitContainerSize =
        MaybeSub(MaybeMax(MaybeClamp(Or(Or(outerNodeSize, maxSize), minSize),
                                     minSize, maxSize),
                          paddingBorderSize),
                 contentBoxInset.SumAxes());

    AutoRepeatStrategy colStrategy =
        (IsSome(outerNodeSize.w) || IsSome(maxSize.w))
            ? AutoRepeatStrategy::MaxRepetitionsThatDoNotOverflow
            : AutoRepeatStrategy::MinRepetitionsThatDoOverflow;
    AutoRepeatStrategy rowStrategy =
        (IsSome(outerNodeSize.h) || IsSome(maxSize.h))
            ? AutoRepeatStrategy::MaxRepetitionsThatDoNotOverflow
            : AutoRepeatStrategy::MinRepetitionsThatDoOverflow;

    ExplicitGridSize colSize = ComputeExplicitGridSizeInAxis(
        style, autoFitContainerSize.w, colStrategy, calc,
        AbsoluteAxis::Horizontal);
    ExplicitGridSize rowSize = ComputeExplicitGridSizeInAxis(
        style, autoFitContainerSize.h, rowStrategy, calc,
        AbsoluteAxis::Vertical);

    NamedLineResolver nameResolver;
    nameResolver
        .Init(style, colSize.autoRepetitionCount, rowSize.autoRepetitionCount);

    uint16_t explicitColCount = colSize.trackCount > nameResolver
                                                         .areaColumnCount
                                    ? colSize.trackCount
                                    : nameResolver.areaColumnCount;
    uint16_t explicitRowCount = rowSize.trackCount > nameResolver.areaRowCount
                                    ? rowSize.trackCount
                                    : nameResolver.areaRowCount;
    explicitColCount =
        explicitColCount > kMaxGridTracks ? kMaxGridTracks : explicitColCount;
    explicitRowCount =
        explicitRowCount > kMaxGridTracks ? kMaxGridTracks : explicitRowCount;
    nameResolver.explicitColumnCount = explicitColCount;
    nameResolver.explicitRowCount = explicitRowCount;

    Vec<PlacementChild> children;
    int childCount = tree->ChildCount(node);

    for (int i = 0; i < childCount; i++) {
        NodeId child = tree->GetChildId(node, i);
        const Style& cs = tree->GetStyle(child);
        if (cs.BoxGenMode() == BoxGenerationMode::None ||
            IsOutOfFlow(cs.position)) {
            continue;
        }
        PlacementChild pc;
        pc.index = i;
        pc.node = child;
        pc.horizontal = nameResolver.ResolveColumnNames(cs.gridColumn)
                            .IntoOriginZero(explicitColCount);
        pc.vertical = nameResolver.ResolveRowNames(cs.gridRow)
                          .IntoOriginZero(explicitRowCount);
        VecAppend(children, pc);
    }

    TrackCounts estColCounts;
    TrackCounts estRowCounts;
    ComputeGridSizeEstimate(explicitColCount, explicitRowCount, children.els,
                            len(children), &estColCounts, &estRowCounts);

    Vec<GridItem> items;
    CellOccupancyMatrix cellOccupancyMatrix;
    cellOccupancyMatrix.Init(estColCounts, estRowCounts);
    PlaceGridItems(&cellOccupancyMatrix, &items, tree, children,
                   style.gridAutoFlow, alignItems, justifyItems);

    TrackCounts finalColCounts = cellOccupancyMatrix
                                     .Counts(AbsoluteAxis::Horizontal);
    TrackCounts finalRowCounts = cellOccupancyMatrix
                                     .Counts(AbsoluteAxis::Vertical);

    Vec<GridTrack> columns;
    Vec<GridTrack> rows;
    InitializeGridTracks(&columns, finalColCounts, style,
                         AbsoluteAxis::Horizontal, [&](int columnIndex) {
                             return cellOccupancyMatrix
                                 .ColumnIsOccupied(columnIndex);
                         });
    InitializeGridTracks(&rows, finalRowCounts, style, AbsoluteAxis::Vertical,
                         [&](int rowIndex) {
                             return cellOccupancyMatrix.RowIsOccupied(rowIndex);
                         });

    ResolveItemTrackIndexes(items.els, len(items), finalColCounts,
                            finalRowCounts);
    DetermineIfItemCrossesFlexibleOrIntrinsicTracks(items.els, len(items),
                                                    columns.els, rows.els);

    bool hasBaselineAlignedItem = false;
    for (int i = 0; i < len(items); i++) {
        if (items[i].ParticipatesInBaselineAlignment()) {
            hasBaselineAlignedItem = true;
            break;
        }
    }

    TrackSizingAlgorithm(
        tree, AbstractAxis::Inline, Get(innerMinSize, AbstractAxis::Inline),
        Get(innerMaxSize, AbstractAxis::Inline), justifyContent, alignContent,
        availableGridSpace, innerNodeSize, columns.els, len(columns), rows.els,
        len(rows), items.els, len(items),
        TrackSizeEstimate::MaxTrackSizingFunction, hasBaselineAlignedItem);
    float initialColumnSum = 0.0f;
    for (int i = 0; i < len(columns); i++) {
        initialColumnSum += columns[i].baseSize;
    }
    if (!IsSome(innerNodeSize.w)) {
        innerNodeSize.w = Some(initialColumnSum);
    }

    if (runMode == RunMode::ComputeSize &&
        inputs.axis == RequestedAxis::Horizontal) {
        float width = F32Max(
            MaybeClamp(initialColumnSum + contentBoxInset.HorizontalAxisSum(),
                       minSize.w, maxSize.w),
            paddingBorderSize.w);
        VecReset(items);
        VecReset(columns);
        VecReset(rows);
        VecReset(children);
        cellOccupancyMatrix.Free();
        nameResolver.Free();
        return LayoutOutput::FromOuterSize({width, 0.0f});
    }

    for (int i = 0; i < len(items); i++) {
        items[i].hasGridAreaSizeCache = false;
    }

    TrackSizingAlgorithm(
        tree, AbstractAxis::Block, Get(innerMinSize, AbstractAxis::Block),
        Get(innerMaxSize, AbstractAxis::Block), alignContent, justifyContent,
        availableGridSpace, innerNodeSize, rows.els, len(rows), columns.els,
        len(columns), items.els, len(items), TrackSizeEstimate::BaseSize,

        false);
    float initialRowSum = 0.0f;
    for (int i = 0; i < len(rows); i++) {
        initialRowSum += rows[i].baseSize;
    }

    SizeFOpt resolvedStyleSize = Or(knownDimensions, preferredSize);
    SizeF containerBorderBox = {
        F32Max(MaybeClamp(UnwrapOr(Get(resolvedStyleSize, AbstractAxis::Inline),
                                   initialColumnSum + contentBoxInset
                                                          .HorizontalAxisSum()),
                          minSize.w, maxSize.w),
               paddingBorderSize.w),
        F32Max(MaybeClamp(
                   UnwrapOr(Get(resolvedStyleSize, AbstractAxis::Block),
                            initialRowSum + contentBoxInset.VerticalAxisSum()),
                   minSize.h, maxSize.h),
               paddingBorderSize.h)};
    SizeF containerContentBox = {
        F32Max(0.0f, containerBorderBox.w - contentBoxInset
                                                .HorizontalAxisSum()),
        F32Max(0.0f, containerBorderBox.h - contentBoxInset.VerticalAxisSum())};

    if (runMode == RunMode::ComputeSize) {
        VecReset(items);
        VecReset(columns);
        VecReset(rows);
        VecReset(children);
        cellOccupancyMatrix.Free();
        nameResolver.Free();
        return LayoutOutput::FromOuterSize(containerBorderBox);
    }

    innerNodeSize = {Some(containerContentBox.w), Some(containerContentBox.h)};

    bool hasPercentageColumn = false;
    for (int i = 0; i < len(columns); i++) {
        if (columns[i].UsesPercentage()) {
            hasPercentageColumn = true;
            break;
        }
    }
    bool hasPercentageRow = false;
    for (int i = 0; i < len(rows); i++) {
        if (rows[i].UsesPercentage()) {
            hasPercentageRow = true;
            break;
        }
    }

    bool parentWidthIndefinite = !IsSome(outerNodeSize.w);
    bool rerunColumnSizing = parentWidthIndefinite && hasPercentageColumn;
    bool intrinsicColumnContributionChanged = false;

    if (!rerunColumnSizing) {

        for (int i = 0; i < len(items); i++) {
            GridItem* item = &items[i];
            if (!item->crossesIntrinsicColumn) {
                continue;
            }
            SizeFOpt gridAreaSize = ItemGridAreaSize(
                *item, AbstractAxis::Inline, columns.els, rows.els,
                innerNodeSize, TrackSizeEstimate::BaseSize, calc);
            SizeFOpt avail = gridAreaSize;
            Set(&avail, AbstractAxis::Inline, None());
            float newMinContent = ItemMinContentContribution(
                *item, AbstractAxis::Inline, tree, gridAreaSize, avail);
            bool changed =
                !(IsSome(item->minContentContributionCache.w) &&
                  item->minContentContributionCache.w == newMinContent);
            item->gridAreaSizeCache = gridAreaSize;
            item->hasGridAreaSizeCache = true;
            item->minContentContributionCache.w = Some(newMinContent);
            item->maxContentContributionCache.w = None();
            item->minimumContributionCache.w = None();
            if (changed) {
                intrinsicColumnContributionChanged = true;
            }
        }
        rerunColumnSizing = intrinsicColumnContributionChanged;
    } else {
        for (int i = 0; i < len(items); i++) {
            items[i].hasGridAreaSizeCache = false;
            items[i].minContentContributionCache.w = None();
            items[i].maxContentContributionCache.w = None();
            items[i].minimumContributionCache.w = None();
        }
    }

    bool parentHeightIndefinite = !IsSome(outerNodeSize.h);
    bool rerunRowSizing = parentHeightIndefinite && hasPercentageRow;
    bool intrinsicRowContributionChanged = false;
    if (rerunRowSizing) {
        for (int i = 0; i < len(items); i++) {
            items[i].hasGridAreaSizeCache = false;
            items[i].minContentContributionCache.h = None();
            items[i].maxContentContributionCache.h = None();
            items[i].minimumContributionCache.h = None();
        }
    }

    if (rerunColumnSizing) {
        TrackSizingAlgorithm(
            tree, AbstractAxis::Inline, Get(innerMinSize, AbstractAxis::Inline),
            Get(innerMaxSize, AbstractAxis::Inline), justifyContent,
            alignContent, availableGridSpace, innerNodeSize, columns.els,
            len(columns), rows.els, len(rows), items.els, len(items),
            TrackSizeEstimate::BaseSize, hasBaselineAlignedItem);

        if (!rerunRowSizing) {

            for (int i = 0; i < len(items); i++) {
                GridItem* item = &items[i];
                if (!item->crossesIntrinsicRow) {
                    continue;
                }
                SizeFOpt gridAreaSize = ItemGridAreaSize(
                    *item, AbstractAxis::Block, rows.els, columns.els,
                    innerNodeSize, TrackSizeEstimate::BaseSize, calc);
                SizeFOpt avail = gridAreaSize;
                Set(&avail, AbstractAxis::Block, None());
                float newMinContent = ItemMinContentContribution(
                    *item, AbstractAxis::Block, tree, gridAreaSize, avail);
                bool changed =
                    !(IsSome(item->minContentContributionCache.h) &&
                      item->minContentContributionCache.h == newMinContent);
                item->gridAreaSizeCache = gridAreaSize;
                item->hasGridAreaSizeCache = true;
                item->minContentContributionCache.h = Some(newMinContent);
                item->maxContentContributionCache.h = None();
                item->minimumContributionCache.h = None();
                if (changed) {
                    intrinsicRowContributionChanged = true;
                }
            }
            rerunRowSizing = intrinsicRowContributionChanged;
        }
    }

    if (rerunRowSizing) {
        TrackSizingAlgorithm(
            tree, AbstractAxis::Block, Get(innerMinSize, AbstractAxis::Block),
            Get(innerMaxSize, AbstractAxis::Block), alignContent,
            justifyContent, availableGridSpace, innerNodeSize, rows.els,
            len(rows), columns.els, len(columns), items.els, len(items),
            TrackSizeEstimate::BaseSize, false);
    }

    if ((intrinsicColumnContributionChanged && !hasPercentageColumn) ||
        (intrinsicRowContributionChanged && !hasPercentageRow)) {
        float finalColumnSum = 0.0f;
        for (int i = 0; i < len(columns); i++) {
            finalColumnSum += columns[i].baseSize;
        }
        float finalRowSum = 0.0f;
        for (int i = 0; i < len(rows); i++) {
            finalRowSum += rows[i].baseSize;
        }
        if (intrinsicColumnContributionChanged && !hasPercentageColumn) {
            containerBorderBox.w = F32Max(
                MaybeClamp(
                    UnwrapOr(Get(resolvedStyleSize, AbstractAxis::Inline),
                             finalColumnSum + contentBoxInset
                                                  .HorizontalAxisSum()),
                    minSize.w, maxSize.w),
                paddingBorderSize.w);
            containerContentBox.w =
                F32Max(0.0f, containerBorderBox.w - contentBoxInset
                                                        .HorizontalAxisSum());
        }
        if (intrinsicRowContributionChanged && !hasPercentageRow) {
            containerBorderBox.h = F32Max(
                MaybeClamp(
                    UnwrapOr(Get(resolvedStyleSize, AbstractAxis::Block),
                             finalRowSum + contentBoxInset.VerticalAxisSum()),
                    minSize.h, maxSize.h),
                paddingBorderSize.h);
            containerContentBox.h = F32Max(
                0.0f, containerBorderBox.h - contentBoxInset.VerticalAxisSum());
        }
    }

    float inlineSizeWithoutScrollbar =
        F32Max(containerBorderBox.w - paddingBorderSize.w, 0.0f);
    float inlineScrollbarGutterForAlignment =
        F32Min(scrollbarGutter.x, inlineSizeWithoutScrollbar);
    AlignTracks(
        Get(containerContentBox, AbstractAxis::Inline),
        {padding.left +
             (IsRtl(direction) ? inlineScrollbarGutterForAlignment : 0.0f),
         padding.right +
             (IsRtl(direction) ? 0.0f : inlineScrollbarGutterForAlignment)},
        {border.left, border.right}, columns.els, len(columns), justifyContent,
        IsRtl(direction));
    AlignTracks(Get(containerContentBox, AbstractAxis::Block),
                {padding.top, padding.bottom}, {border.top, border.bottom},
                rows.els, len(rows), alignContent, false);

    RectF itemOverflowRect = RectF::Zero();
    Vec<OofCandidate> oofPool;
    OofCandidates oofCandidates;
    StoreGridOofSnapshot(tree, node, colSize.autoRepetitionCount,
                         rowSize.autoRepetitionCount, finalColCounts,
                         finalRowCounts, columns, rows);

    StableSort(items.els, len(items), [](const GridItem& a, const GridItem& b) {
        return a.sourceOrder < b.sourceOrder;
    });

    for (int index = 0; index < len(items); index++) {
        GridItem& item = items[index];

        float gridLeft = IsRtl(direction)
                             ? columns[(int)item.columnIndexes.end - 1].offset
                             : columns[(int)item.columnIndexes.start + 1]
                                   .offset;
        float gridRight = IsRtl(direction)
                              ? columns[(int)item.columnIndexes.start].offset
                              : columns[(int)item.columnIndexes.end].offset;
        RectF gridArea = {gridLeft, gridRight,
                          rows[(int)item.rowIndexes.start + 1].offset,
                          rows[(int)item.rowIndexes.end].offset};

        SizeF containingBlock = {gridArea.right - gridArea.left,
                                 gridArea.bottom - gridArea.top};
        AlignedItem placed = AlignAndPositionItem(
            tree, item.node, (uint32_t)index, gridArea, containingBlock,
            justifyItems, alignItems, item.baselineShim, direction,
            containerBorderBox.w, border, isScrollContainer, &oofPool);
        item.yPosition = placed.yPosition;
        item.height = placed.height;
        item.oofStart = placed.oofStart;
        item.oofCount = placed.oofCount;
        itemOverflowRect = itemOverflowRect.Union(placed.overflowContribution);
    }

    int itemCursor = 0;
    uint32_t order = (uint32_t)len(items);
    ContainingBlockClaims claims = ContainingBlockFor(style.position);
    auto lineAsStartEdge = [](const Vec<GridTrack>& tracks, int index) {
        return index + 1 < len(tracks) ? tracks[index + 1].offset
                                       : tracks[index].offset;
    };
    auto lineAsEndEdge = [](const Vec<GridTrack>& tracks, int index) {
        if (index == 0) {
            return len(tracks) > 1 ? tracks[1].offset : tracks[0].offset;
        }
        return tracks[index].offset;
    };
    auto rtlLineAsStartEdge = [](const Vec<GridTrack>& tracks, int index) {
        if (index + 1 < len(tracks)) {
            return tracks[index].offset;
        }
        if (index == 0) {
            return tracks[0].offset;
        }
        return tracks[index - 1].offset;
    };
    auto rtlLineAsEndEdge = [](const Vec<GridTrack>& tracks, int index) {
        if (index == 0) {
            return tracks[0].offset;
        }
        return tracks[index - 1].offset;
    };
    for (int index = 0; index < childCount; index++) {
        if (itemCursor < len(items) &&
            items[itemCursor].sourceOrder == (uint16_t)index) {
            OofCandRange range = {items[itemCursor].oofStart, items[itemCursor]
                                                                  .oofCount};
            OofAppendPoolRange(&oofCandidates, oofPool, range);
            itemCursor += 1;
            continue;
        }

        NodeId child = tree->GetChildId(node, index);
        const Style& cs = tree->GetStyle(child);
        if (cs.BoxGenMode() == BoxGenerationMode::None) {
            tree->SetUnroundedLayout(child, Layout::WithOrder(order));
            tree->PerformChildLayout(
                child, SizeFOptNone(), SizeFOptNone(), SizeAvail::MaxContent(),
                SizingMode::InherentSize, LineBool::False());
            order += 1;
            continue;
        }
        if (!IsOutOfFlow(cs.position)) {
            continue;
        }

        RectF area;
        if (claims.ForPosition(cs.position)) {
            LineOptOzl colLines =
                nameResolver.ResolveColumnNames(cs.gridColumn)
                    .IntoOriginZero(finalColCounts.explicitCount)
                    .ResolveAbsolutelyPositionedGridTracks();
            int colStartIdx = -1;
            int colEndIdx = -1;
            if (colLines.start.IsSome()) {
                TryIntoTrackVecIndex(colLines.start.val, finalColCounts,
                                     &colStartIdx);
            }
            if (colLines.end.IsSome()) {
                TryIntoTrackVecIndex(colLines.end.val, finalColCounts,
                                     &colEndIdx);
            }
            LineOptOzl rowLines =
                nameResolver.ResolveRowNames(cs.gridRow)
                    .IntoOriginZero(finalRowCounts.explicitCount)
                    .ResolveAbsolutelyPositionedGridTracks();
            int rowStartIdx = -1;
            int rowEndIdx = -1;
            if (rowLines.start.IsSome()) {
                TryIntoTrackVecIndex(rowLines.start.val, finalRowCounts,
                                     &rowStartIdx);
            }
            if (rowLines.end.IsSome()) {
                TryIntoTrackVecIndex(rowLines.end.val, finalRowCounts,
                                     &rowEndIdx);
            }
            area.top = rowStartIdx >= 0 ? lineAsStartEdge(rows, rowStartIdx)
                                        : border.top;
            area.bottom =
                rowEndIdx >= 0
                    ? lineAsEndEdge(rows, rowEndIdx)
                    : containerBorderBox.h - border.bottom - scrollbarGutter.y;
            if (IsRtl(direction)) {
                area.left = colEndIdx >= 0
                                ? rtlLineAsEndEdge(columns, colEndIdx)
                                : border.left + scrollbarGutter.x;
                area.right = colStartIdx >= 0
                                 ? rtlLineAsStartEdge(columns, colStartIdx)
                                 : containerBorderBox.w - border.right;
            } else {
                area.left = colStartIdx >= 0
                                ? lineAsStartEdge(columns, colStartIdx)
                                : border.left;
                area.right = colEndIdx >= 0
                                 ? lineAsEndEdge(columns, colEndIdx)
                                 : containerBorderBox.w - border.right -
                                       scrollbarGutter.x;
            }
        } else {

            area.left = border.left + padding.left +
                        (IsRtl(direction) ? scrollbarGutter.x : 0.0f);
            area.right = containerBorderBox.w - border.right - padding.right -
                         (IsRtl(direction) ? 0.0f : scrollbarGutter.x);
            area.top = border.top + padding.top;
            area.bottom = containerBorderBox.h - border.bottom -
                          padding.bottom - scrollbarGutter.y;
        }

        AlignItems justifySelf =
            ResolveSelfRelative(cs.justifySelf.UnwrapOr(justifyItems),
                                cs.direction, direction, true);
        AlignItems alignSelf = ResolveSelfRelative(
            cs.alignSelf.UnwrapOr(alignItems), cs.direction, direction, false);

        OofCandidate candidate;
        candidate.node = child;
        candidate.order = order;
        candidate.position = cs.position;
        candidate.staticX = AxisStaticPosition::FromAlignment(
            justifySelf, area.left, area.right, IsRtl(direction));
        candidate.staticY = AxisStaticPosition::FromAlignment(
            alignSelf, area.top, area.bottom, false);
        oofCandidates.Push(candidate);
        order += 1;
    }

    RectF absolutePositionInset = border;
    if (IsRtl(direction)) {
        absolutePositionInset.left += scrollbarGutter.x;
    } else {
        absolutePositionInset.right += scrollbarGutter.x;
    }
    absolutePositionInset.bottom += scrollbarGutter.y;

    auto withEndPadding = [&](RectF overflowRect) {
        if (isScrollContainer) {
            overflowRect
                .right += IsRtl(direction) ? padding.left : padding.right;
            overflowRect.bottom += padding.bottom;
        }
        return overflowRect;
    };

    LayoutOutput out;
    if (len(items) == 0) {
        out = LayoutOutput::FromSizes(containerBorderBox,
                                      withEndPadding(itemOverflowRect));
    } else {

        StableSort(items.els, len(items),
                   [](const GridItem& a, const GridItem& b) {
                       return a.row.start < b.row.start;
                   });
        OriginZeroLine firstRow = items[0].row.start;
        int rowEnd = 0;
        while (rowEnd < len(items) && items[rowEnd].row.start == firstRow) {
            rowEnd++;
        }
        const GridItem* chosen = &items[0];
        for (int i = 0; i < rowEnd; i++) {
            if (items[i].ParticipatesInBaselineAlignment()) {
                chosen = &items[i];
                break;
            }
        }

        Optf gridContainerBaseline = None();
        if (!contain.SuppressesBaseline()) {
            gridContainerBaseline = Some(
                chosen->yPosition + UnwrapOr(chosen->baseline, chosen->height));
        }
        out = LayoutOutput::FromSizesAndBaselines(
            containerBorderBox, withEndPadding(itemOverflowRect),
            BaselinesFromFirst(gridContainerBaseline));
    }

    out.oofCandidates = oofCandidates;
    out.hasOofPositioningArea = true;
    out.oofPositioningArea.size = containerBorderBox - absolutePositionInset
                                                           .SumAxes();
    out.oofPositioningArea
        .offset = {absolutePositionInset.left, absolutePositionInset.top};

    VecReset(items);
    VecReset(columns);
    VecReset(rows);
    VecReset(children);
    VecReset(oofPool);
    cellOccupancyMatrix.Free();
    nameResolver.Free();
    return out;
}

bool GridOofArea(TaffyTree* tree, NodeId grid, NodeId child, RectF paddingBox,
                 RectF* out) {
    NodeData* d = tree->Get(grid);
    if (!d || !d->gridOof.live) {
        return false;
    }
    const Style& gridStyle = tree->GetStyle(grid);
    const Style& childStyle = tree->GetStyle(child);
    const GridOofSnapshot& snap = d->gridOof;
    NamedLineResolver resolver;
    resolver.Init(gridStyle, snap.columns.autoRepetitions,
                  snap.rows.autoRepetitions);
    resolver.explicitColumnCount = snap.columns.explicitCount;
    resolver.explicitRowCount = snap.rows.explicitCount;

    auto spanIndex = [](OptOriginZeroLine line, TrackCounts counts) {
        if (!line.IsSome()) {
            return -1;
        }
        int16_t minLine = -(int16_t)counts.negativeImplicit;
        int16_t maxLine =
            (int16_t)(counts.explicitCount + counts.positiveImplicit);
        if (line.val.v < minLine || line.val.v > maxLine) {
            return -1;
        }
        int trackVec = 0;
        if (!TryIntoTrackVecIndex(line.val, counts, &trackVec)) {
            return -1;
        }
        return trackVec / 2;
    };
    auto resolveAxis = [&](LinePlacement placement,
                           const GridAxisPositions& axis, float padStart,
                           float padEnd, bool rows, bool reversed) {
        TrackCounts counts;
        counts.negativeImplicit = axis.negativeImplicit;
        counts.explicitCount = axis.explicitCount;
        counts.positiveImplicit = axis.positiveImplicit;
        LinePlain plain = rows ? resolver.ResolveRowNames(placement)
                               : resolver.ResolveColumnNames(placement);
        LineOptOzl lines = plain.IntoOriginZero(axis.explicitCount)
                               .ResolveAbsolutelyPositionedGridTracks();
        int startIdx = spanIndex(lines.start, counts);
        int endIdx = spanIndex(lines.end, counts);

        auto trackEdge = [&](int index, bool isStart) {
            int nRuns = len(axis.collapsed);
            int rangeIndex = 0;
            while (rangeIndex < nRuns && axis.collapsed[rangeIndex]
                                                 .end <= index) {
                rangeIndex++;
            }
            int use = index;
            if (rangeIndex < nRuns &&
                index >= axis.collapsed[rangeIndex].start &&
                index < axis.collapsed[rangeIndex].end) {
                const CollapsedRun& range = axis.collapsed[rangeIndex];
                int adjacent = isStart ? range.end : range.start - 1;
                if (adjacent >= 0 && adjacent < len(axis.spans)) {
                    use = adjacent;
                }
            }
            const GridTrackSpan& track = axis.spans[use];
            return (isStart != reversed) ? track.start : track.end;
        };
        float startPos;
        float endPos;
        if (startIdx >= 0 && startIdx < len(axis.spans)) {
            startPos = trackEdge(startIdx, true);
        } else if (startIdx >= 0 && len(axis.spans) > 0) {
            const GridTrackSpan& last = axis.spans[len(axis.spans) - 1];
            startPos = reversed ? last.start : last.end;
        } else {
            startPos = reversed ? padEnd : padStart;
        }
        if (endIdx > 0 && endIdx - 1 < len(axis.spans)) {
            endPos = trackEdge(endIdx - 1, false);
        } else if (endIdx >= 0 && len(axis.spans) > 0) {
            endPos = reversed ? axis.spans[0].end : axis.spans[0].start;
        } else {
            endPos = reversed ? padStart : padEnd;
        }

        float left;
        float right;
        if (reversed) {
            left = F32Min(startPos, endPos);
            right = startPos;
        } else {
            left = startPos;
            right = F32Max(startPos, endPos);
        }
        return RectF{left, right, 0.0f, 0.0f};
    };

    RectF cols =
        resolveAxis(childStyle.gridColumn, snap.columns, paddingBox.left,
                    paddingBox.right, false, IsRtl(gridStyle.direction));
    RectF row = resolveAxis(childStyle.gridRow, snap.rows, paddingBox.top,
                            paddingBox.bottom, true, false);
    resolver.Free();
    *out = {cols.left, cols.right, row.left, row.right};
    return true;
}

void GridExplicitSizeForTest(const Style& style, Optf autoFitContainerSize,
                             bool maxRepetitions, AbsoluteAxis axis,
                             CalcResolver calc, uint16_t* outAutoRepetitions,
                             uint16_t* outTrackCount) {
    ExplicitGridSize r = ComputeExplicitGridSizeInAxis(
        style, autoFitContainerSize,
        maxRepetitions ? AutoRepeatStrategy::MaxRepetitionsThatDoNotOverflow
                       : AutoRepeatStrategy::MinRepetitionsThatDoOverflow,
        calc, axis);
    *outAutoRepetitions = r.autoRepetitionCount;
    *outTrackCount = r.trackCount;
}

void GridChildMinMaxSpanForTest(LinePlacement line, uint16_t explicitTrackCount,
                                int16_t* outMinLine, int16_t* outMaxLine,
                                uint16_t* outSpan) {
    MinMaxSpan r = ChildMinLineMaxLineSpan(
        line.IntoOriginZeroIgnoringNamed(explicitTrackCount));
    *outMinLine = r.minLine.v;
    *outMaxLine = r.maxLine.v;
    *outSpan = r.span;
}

void GridSizeEstimateForTest(uint16_t explicitColCount,
                             uint16_t explicitRowCount,
                             const LinePlacement* columns,
                             const LinePlacement* rows, int n,
                             uint16_t* outColCounts, uint16_t* outRowCounts) {
    Vec<PlacementChild> children;
    for (int i = 0; i < n; i++) {
        PlacementChild pc;
        pc.horizontal = columns[i]
                            .IntoOriginZeroIgnoringNamed(explicitColCount);
        pc.vertical = rows[i].IntoOriginZeroIgnoringNamed(explicitRowCount);
        VecAppend(children, pc);
    }
    TrackCounts cols;
    TrackCounts rws;
    ComputeGridSizeEstimate(explicitColCount, explicitRowCount, children.els,
                            len(children), &cols, &rws);
    VecReset(children);
    outColCounts[0] = cols.negativeImplicit;
    outColCounts[1] = cols.explicitCount;
    outColCounts[2] = cols.positiveImplicit;
    outRowCounts[0] = rws.negativeImplicit;
    outRowCounts[1] = rws.explicitCount;
    outRowCounts[2] = rws.positiveImplicit;
}

int GridInitTracksForTest(const Style& style, AbsoluteAxis axis,
                          uint16_t negativeImplicit, uint16_t explicitCount,
                          uint16_t positiveImplicit, GridTrackForTest* out,
                          int cap) {
    Vec<GridTrack> tracks;
    TrackCounts counts =
        TrackCounts::FromRaw(negativeImplicit, explicitCount, positiveImplicit);
    InitializeGridTracks(&tracks, counts, style, axis,
                         [](int) { return false; });
    int n = len(tracks);
    for (int i = 0; i < n && i < cap; i++) {
        out[i].isGutter = tracks[i].kind == GridTrackKind::Gutter;
        out[i].isCollapsed = tracks[i].isCollapsed;
        out[i].min = tracks[i].minTrackSizingFunction.raw;
        out[i].max = tracks[i].maxTrackSizingFunction.raw;
    }
    VecReset(tracks);
    return n;
}

int GridPlaceForTest(TaffyTree* tree, NodeId parent, uint16_t explicitColCount,
                     uint16_t explicitRowCount, GridAutoFlow flow,
                     GridPlacementForTest* out, int cap, uint16_t* outColCounts,
                     uint16_t* outRowCounts) {
    NamedLineResolver nameResolver;
    nameResolver.Init(tree->GetStyle(parent), 0, 0);
    nameResolver.explicitColumnCount = explicitColCount;
    nameResolver.explicitRowCount = explicitRowCount;

    Vec<PlacementChild> children;
    int childCount = tree->ChildCount(parent);
    for (int i = 0; i < childCount; i++) {
        NodeId child = tree->GetChildId(parent, i);
        const Style& cs = tree->GetStyle(child);
        if (cs.BoxGenMode() == BoxGenerationMode::None ||
            IsOutOfFlow(cs.position)) {
            continue;
        }
        PlacementChild pc;
        pc.index = i;
        pc.node = child;
        pc.horizontal = nameResolver.ResolveColumnNames(cs.gridColumn)
                            .IntoOriginZero(explicitColCount);
        pc.vertical = nameResolver.ResolveRowNames(cs.gridRow)
                          .IntoOriginZero(explicitRowCount);
        VecAppend(children, pc);
    }

    TrackCounts estCols;
    TrackCounts estRows;
    ComputeGridSizeEstimate(explicitColCount, explicitRowCount, children.els,
                            len(children), &estCols, &estRows);

    Vec<GridItem> items;
    CellOccupancyMatrix matrix;
    matrix.Init(estCols, estRows);
    PlaceGridItems(&matrix, &items, tree, children, flow,
                   AlignItems{AlignItemsKeyword::Start},
                   AlignItems{AlignItemsKeyword::Start});

    int n = len(items);
    for (int i = 0; i < n && i < cap; i++) {
        out[i].columnStart = items[i].column.start.v;
        out[i].columnEnd = items[i].column.end.v;
        out[i].rowStart = items[i].row.start.v;
        out[i].rowEnd = items[i].row.end.v;
    }
    TrackCounts cols = matrix.Counts(AbsoluteAxis::Horizontal);
    TrackCounts rws = matrix.Counts(AbsoluteAxis::Vertical);
    outColCounts[0] = cols.negativeImplicit;
    outColCounts[1] = cols.explicitCount;
    outColCounts[2] = cols.positiveImplicit;
    outRowCounts[0] = rws.negativeImplicit;
    outRowCounts[1] = rws.explicitCount;
    outRowCounts[2] = rws.positiveImplicit;

    VecReset(items);
    VecReset(children);
    matrix.Free();
    nameResolver.Free();
    return n;
}

}

#line 1 "src/taffy/compute_oof.cpp"

namespace taffy {

static float AlignInLine(float start, float end, float size,
                         AxisStaticEdge edge) {
    if (edge == AxisStaticEdge::End) {
        return end - size;
    }
    if (edge == AxisStaticEdge::Center) {
        return (start + end - size) / 2.0f;
    }
    return start;
}

float ResolveStaticOffsetAxis(AxisStaticPosition position, float size,
                              float marginStart, float marginEnd) {
    float marginBox = size + marginStart + marginEnd;
    bool overflows = position.align.safety == AlignmentSafety::Safe &&
                     marginBox > position.end - position.start;
    AxisStaticEdge keyword =
        overflows ? position.align.fallback : position.align.keyword;
    return AlignInLine(position.start, position.end, marginBox, keyword) +
           marginStart;
}

enum class OofAlignment : uint8_t {
    Normal,
    Stretch,
    Start,
    Center,
    End
};

static OofAlignment OofAlignmentFromResolved(AlignItems alignment) {
    switch (alignment.keyword) {
        case AlignItemsKeyword::Normal:
            return OofAlignment::Normal;
        case AlignItemsKeyword::Stretch:
            return OofAlignment::Stretch;
        case AlignItemsKeyword::Center:
            return OofAlignment::Center;
        case AlignItemsKeyword::End:
        case AlignItemsKeyword::FlexEnd:
        case AlignItemsKeyword::SelfEnd:
            return OofAlignment::End;
        case AlignItemsKeyword::Start:
        case AlignItemsKeyword::FlexStart:
        case AlignItemsKeyword::SelfStart:
        case AlignItemsKeyword::Baseline:
        default:
            return OofAlignment::Start;
    }
}

static AxisStaticEdge OofPhysicalEdge(OofAlignment alignment, bool reversed) {
    if (alignment == OofAlignment::Center) {
        return AxisStaticEdge::Center;
    }
    bool isEnd = alignment == OofAlignment::End;
    return (isEnd != reversed) ? AxisStaticEdge::End : AxisStaticEdge::Start;
}

struct OofAxis {
    float cbSize = 0.0f;
    Optf insetStart = None();
    Optf insetEnd = None();
    Optf marginStart = None();
    Optf marginEnd = None();
    AxisStaticPosition staticPosition;
    OofAlignment alignment = OofAlignment::Normal;
    AlignmentSafety safety = AlignmentSafety::Default;
    bool isInline = false;
    bool reversed = false;
    bool isScrollAxis = false;

    AxisStaticEdge StartEdge() const {
        return reversed ? AxisStaticEdge::End : AxisStaticEdge::Start;
    }
    AxisStaticEdge EndEdge() const {
        return reversed ? AxisStaticEdge::Start : AxisStaticEdge::End;
    }

    void InsetModifiedContainingBlock(float* start, float* end) const {
        AxisStaticEdge weaker;
        if (IsSome(insetStart) && IsSome(insetEnd)) {
            *start = insetStart;
            *end = cbSize - insetEnd;
            weaker = EndEdge();
        } else if (IsSome(insetStart)) {
            *start = insetStart;
            *end = cbSize;
            weaker = AxisStaticEdge::End;
        } else if (IsSome(insetEnd)) {
            *start = 0.0f;
            *end = cbSize - insetEnd;
            weaker = AxisStaticEdge::Start;
        } else {
            AxisStaticEdge keyword = staticPosition.align.keyword;
            if (keyword == AxisStaticEdge::End) {
                *start = 0.0f;
                *end = staticPosition.end;
            } else if (keyword == AxisStaticEdge::Center) {
                float center =
                    (staticPosition.start + staticPosition.end) / 2.0f;
                float startDistance = center;
                float endDistance = cbSize - center;
                float startAbs =
                    startDistance < 0.0f ? -startDistance : startDistance;
                float endAbs = endDistance < 0.0f ? -endDistance : endDistance;
                if (startAbs <= endAbs) {
                    *start = 0.0f;
                    *end = 2.0f * startDistance;
                } else {
                    *start = cbSize - 2.0f * endDistance;
                    *end = cbSize;
                }
            } else {
                *start = staticPosition.start;
                *end = cbSize;
            }
            weaker = EndEdge();
        }
        if (*end < *start) {
            if (weaker == AxisStaticEdge::Start) {
                *start = *end;
            } else {
                *end = *start;
            }
        }
    }

    float InsetModifiedContainingBlockSize() const {
        float start = 0.0f;
        float end = 0.0f;
        InsetModifiedContainingBlock(&start, &end);
        return end - start;
    }

    float StretchFitSize() const {
        float margins = UnwrapOr(marginStart, 0.0f) + UnwrapOr(marginEnd, 0.0f);
        return F32Max(InsetModifiedContainingBlockSize() - margins, 0.0f);
    }

    bool IsStretchSized(bool isReplaced, bool isTable) const {
        if (!(IsSome(insetStart) && IsSome(insetEnd))) {
            return false;
        }
        if (alignment == OofAlignment::Stretch) {
            return true;
        }
        if (alignment == OofAlignment::Normal) {
            return !isReplaced && !isTable;
        }
        return false;
    }

    void ResolveMargins(float size, float* start, float* end) const {
        bool bothInsets = IsSome(insetStart) && IsSome(insetEnd);
        float imcbStart = 0.0f;
        float imcbEnd = 0.0f;
        InsetModifiedContainingBlock(&imcbStart, &imcbEnd);
        float freeSpace = imcbEnd - imcbStart - size;
        if (IsSome(marginStart) && IsSome(marginEnd)) {
            *start = marginStart;
            *end = marginEnd;
            return;
        }
        if (!bothInsets) {
            *start = UnwrapOr(marginStart, 0.0f);
            *end = UnwrapOr(marginEnd, 0.0f);
            return;
        }
        if (IsSome(marginStart)) {
            *start = marginStart;
            *end = freeSpace - marginStart;
            return;
        }
        if (IsSome(marginEnd)) {
            *end = marginEnd;
            *start = freeSpace - marginEnd;
            return;
        }

        if (freeSpace < 0.0f && isInline) {
            if (reversed) {
                *start = freeSpace;
                *end = 0.0f;
            } else {
                *start = 0.0f;
                *end = freeSpace;
            }
            return;
        }
        *start = freeSpace / 2.0f;
        *end = freeSpace / 2.0f;
    }

    float Position(float size, float marginStartResolved,
                   float marginEndResolved) const {
        float imcbStart = 0.0f;
        float imcbEnd = 0.0f;
        InsetModifiedContainingBlock(&imcbStart, &imcbEnd);
        if (!IsSome(insetStart) && !IsSome(insetEnd)) {

            float marginBox = size + marginStartResolved + marginEndResolved;
            float imcbSize = imcbEnd - imcbStart;
            bool safeOverflow = staticPosition.align
                                        .safety == AlignmentSafety::Safe &&
                                marginBox > imcbSize;
            float offset;
            if (safeOverflow) {
                offset =
                    AlignInLine(imcbStart, imcbEnd, marginBox, StartEdge());
            } else {
                offset = AlignInLine(staticPosition.start, staticPosition.end,
                                     marginBox, staticPosition.align.keyword);
            }
            return offset + marginStartResolved;
        }
        if (IsSome(insetStart) && !IsSome(insetEnd)) {
            return imcbStart + marginStartResolved;
        }
        if (!IsSome(insetStart) && IsSome(insetEnd)) {
            return imcbEnd - size - marginEndResolved;
        }
        if (!IsSome(marginStart) || !IsSome(marginEnd)) {
            return imcbStart + marginStartResolved;
        }
        float marginBox = size + marginStartResolved + marginEndResolved;
        float imcbSize = imcbEnd - imcbStart;
        AxisStaticEdge edge = OofPhysicalEdge(alignment, reversed);
        float marginBoxStart;
        if (safety == AlignmentSafety::Unsafe) {
            marginBoxStart = AlignInLine(imcbStart, imcbEnd, marginBox, edge);
        } else if (safety == AlignmentSafety::Safe) {
            AxisStaticEdge used = marginBox > imcbSize ? StartEdge() : edge;
            marginBoxStart = AlignInLine(imcbStart, imcbEnd, marginBox, used);
        } else if (marginBox <= imcbSize || alignment == OofAlignment::Normal) {
            marginBoxStart = AlignInLine(imcbStart, imcbEnd, marginBox, edge);
        } else {

            float limitStart = F32Min(imcbStart, 0.0f);
            float limitEnd = F32Max(imcbEnd, cbSize);
            if (isScrollAxis) {
                if (reversed) {
                    limitStart = -INFINITY;
                } else {
                    limitEnd = INFINITY;
                }
            }
            if (marginBox <= limitEnd - limitStart) {
                float aligned =
                    AlignInLine(imcbStart, imcbEnd, marginBox, edge);
                marginBoxStart =
                    F32Max(F32Min(aligned, limitEnd - marginBox), limitStart);
            } else {
                marginBoxStart =
                    AlignInLine(limitStart, limitEnd, marginBox, StartEdge());
            }
        }
        return marginBoxStart + marginStartResolved;
    }
};

OofBoxLayout LayoutOofBox(TaffyTree* tree, const OofCandidate& candidate,
                          SizeF areaSize, PointF areaOffset,
                          Direction direction, bool resolveGridArea,
                          NodeId gridOwner, PointOverflow containerOverflow,
                          bool* cacheHit) {
    OofBoxLayout result;
    SizeF usedArea = areaSize;
    PointF usedOffset = areaOffset;
    if (resolveGridArea) {
        RectF paddingBox = {areaOffset.x, areaOffset.x + areaSize.w,
                            areaOffset.y, areaOffset.y + areaSize.h};
        RectF gridArea;
        if (GridOofArea(tree, gridOwner, candidate.node, paddingBox,
                        &gridArea)) {
            usedArea = {gridArea.right - gridArea.left,
                        gridArea.bottom - gridArea.top};
            usedOffset = {gridArea.left, gridArea.top};
        }
    }
    float areaWidth = usedArea.w;
    float areaHeight = usedArea.h;

    CalcResolver calc = tree->calc;
    const Style& cs = tree->GetStyle(candidate.node);
    Optf aspectRatio = cs.aspectRatio;
    RectFOpt margin = cs.margin.MaybeResolve(Some(areaWidth), calc);
    RectF padding = cs.padding.ResolveOrZero(Some(areaWidth), calc);
    RectF border = cs.border.ResolveOrZero(Some(areaWidth), calc);
    SizeF paddingBorderSum = (padding + border).SumAxes();
    SizeF boxSizingAdjustment = cs.boxSizing == BoxSizing::ContentBox
                                    ? paddingBorderSum
                                    : SizeF::Zero();
    RectFOpt inset = cs.inset.MaybeResolveZip(AsOptional(usedArea), calc);
    Optf left = inset.left;
    Optf right = inset.right;
    Optf top = inset.top;
    Optf bottom = inset.bottom;
    SizeDim sizeStyle = cs.size;
    PointOverflow overflow = cs.overflow;
    Contain contain = cs.contain;
    float scrollbarWidth = cs.scrollbarWidth;

    SizeFOpt styleSize = MaybeAdd(
        MaybeApplyAspectRatio(
            sizeStyle.MaybeResolve(AsOptional(usedArea), calc), aspectRatio),
        boxSizingAdjustment);
    SizeFOpt minSize = MaybeMax(
        Or(MaybeAdd(MaybeApplyAspectRatio(
                        cs.minSize.MaybeResolve(AsOptional(usedArea), calc),
                        aspectRatio),
                    boxSizingAdjustment),
           AsOptional(paddingBorderSum)),
        paddingBorderSum);
    SizeFOpt maxSize = MaybeAdd(
        MaybeApplyAspectRatio(
            cs.maxSize.MaybeResolve(AsOptional(usedArea), calc), aspectRatio),
        boxSizingAdjustment);
    SizeFOpt knownDimensions = MaybeClamp(styleSize, minSize, maxSize);

    bool isReplaced = cs.IsReplaced();
    bool isTable = cs.itemIsTable;
    Direction itemDirection = cs.direction;

    AlignItems justifySelf = ResolveSelfRelative(
        cs.justifySelf.UnwrapOr(AlignItems{AlignItemsKeyword::Normal}),
        itemDirection, direction, true);
    AlignItems alignSelf = ResolveSelfRelative(
        cs.alignSelf.UnwrapOr(AlignItems{AlignItemsKeyword::Normal}),
        itemDirection, direction, false);

    AxisStaticPosition staticX = candidate.staticX;
    staticX.start -= usedOffset.x;
    staticX.end -= usedOffset.x;
    AxisStaticPosition staticY = candidate.staticY;
    staticY.start -= usedOffset.y;
    staticY.end -= usedOffset.y;

    OofAxis horizontal;
    horizontal.cbSize = areaWidth;
    horizontal.insetStart = left;
    horizontal.insetEnd = right;
    horizontal.marginStart = margin.left;
    horizontal.marginEnd = margin.right;
    horizontal.staticPosition = staticX;
    horizontal.alignment = OofAlignmentFromResolved(justifySelf);
    horizontal.safety = justifySelf.safety;
    horizontal.isInline = true;
    horizontal.reversed = IsRtl(direction);
    horizontal.isScrollAxis = IsScrollContainer(containerOverflow.x);

    OofAxis vertical;
    vertical.cbSize = areaHeight;
    vertical.insetStart = top;
    vertical.insetEnd = bottom;
    vertical.marginStart = margin.top;
    vertical.marginEnd = margin.bottom;
    vertical.staticPosition = staticY;
    vertical.alignment = OofAlignmentFromResolved(alignSelf);
    vertical.safety = alignSelf.safety;
    vertical.isInline = false;
    vertical.reversed = false;
    vertical.isScrollAxis = IsScrollContainer(containerOverflow.y);

    float stretchWidth = horizontal.StretchFitSize();
    float stretchHeight = vertical.StretchFitSize();

    float availWidth = stretchWidth;
    float availHeight = stretchHeight;
    if (isTable) {
        float nonAutoW =
            UnwrapOr(margin.left, 0.0f) + UnwrapOr(margin.right, 0.0f);
        float nonAutoH =
            UnwrapOr(margin.top, 0.0f) + UnwrapOr(margin.bottom, 0.0f);
        availWidth = F32Min(availWidth, F32Max(areaWidth - nonAutoW, 0.0f));
        availHeight = F32Min(availHeight, F32Max(areaHeight - nonAutoH, 0.0f));
    }
    SizeFOpt availClamped =
        MaybeClamp(SizeFOpt{availWidth, availHeight}, minSize, maxSize);
    SizeAvail childAvail =
        SizeAvail::Definite({availClamped.w, availClamped.h});

    if (sizeStyle.width.IsSizingKeyword() || sizeStyle.height
                                                 .IsSizingKeyword()) {
        ResolveAbsoluteSizingKeywords(tree, candidate.node, &knownDimensions,
                                      sizeStyle, usedArea, inset, margin,
                                      SizingMode::ContentSize);
        knownDimensions =
            MaybeClamp(MaybeApplyAspectRatio(knownDimensions, aspectRatio),
                       minSize, maxSize);
    }

    bool stretchW = !IsSome(knownDimensions.w) &&
                    horizontal.IsStretchSized(isReplaced, isTable);
    bool stretchH = !IsSome(knownDimensions.h) &&
                    vertical.IsStretchSized(isReplaced, isTable);
    bool bothExplicitStretch = horizontal.alignment == OofAlignment::Stretch &&
                               vertical.alignment == OofAlignment::Stretch;
    Optf stretchAspect =
        (stretchW && stretchH && bothExplicitStretch) ? None() : aspectRatio;
    if (stretchW) {
        if (isTable) {
            minSize.w = Some(F32Max(UnwrapOr(minSize.w, 0.0f), stretchWidth));
        } else {
            knownDimensions.w = Some(stretchWidth);
            knownDimensions = MaybeClamp(
                MaybeApplyAspectRatio(knownDimensions, stretchAspect), minSize,
                maxSize);
        }
    }
    if (!IsSome(knownDimensions.h) && stretchH) {
        if (isTable) {
            minSize.h = Some(F32Max(UnwrapOr(minSize.h, 0.0f), stretchHeight));
        } else {
            knownDimensions.h = Some(stretchHeight);
            knownDimensions = MaybeClamp(
                MaybeApplyAspectRatio(knownDimensions, stretchAspect), minSize,
                maxSize);
        }
    }

    SizeF finalSize;
    if (IsSome(knownDimensions.w) && IsSome(knownDimensions.h)) {
        finalSize = {knownDimensions.w, knownDimensions.h};
    } else {
        SizeF measured = tree->MeasureChildSizeBoth(
            candidate.node, knownDimensions, AsOptional(usedArea), childAvail,
            SizingMode::ContentSize, LineBool::False());
        finalSize = UnwrapOr(knownDimensions, measured);
    }
    finalSize = MaybeClamp(finalSize, minSize, maxSize);

    LayoutInput layoutInput;
    layoutInput.knownDimensions = AsOptional(finalSize);
    layoutInput.parentSize = AsOptional(usedArea);
    layoutInput.availableSpace = childAvail;
    layoutInput.sizingMode = SizingMode::ContentSize;
    layoutInput.axis = RequestedAxis::Both;
    layoutInput.runMode = RunMode::PerformLayout;
    layoutInput.verticalMarginsAreCollapsible = LineBool::False();
    if (cacheHit) {
        LayoutOutput probe;
        *cacheHit = tree->CacheGet(candidate.node, layoutInput, &probe);
    }
    LayoutOutput layoutOutput =
        tree->ComputeChildLayout(candidate.node, layoutInput);

    float marginLeft = 0.0f;
    float marginRight = 0.0f;
    float marginTop = 0.0f;
    float marginBottom = 0.0f;
    horizontal.ResolveMargins(finalSize.w, &marginLeft, &marginRight);
    vertical.ResolveMargins(finalSize.h, &marginTop, &marginBottom);
    RectF resolvedMargin = {marginLeft, marginRight, marginTop, marginBottom};
    PointF location = {
        horizontal.Position(finalSize.w, marginLeft, marginRight) + usedOffset
                                                                        .x,
        vertical.Position(finalSize.h, marginTop, marginBottom) + usedOffset.y};

    SizeF scrollbarSize = {
        overflow.y == Overflow::Scroll ? scrollbarWidth : 0.0f,
        overflow.x == Overflow::Scroll ? scrollbarWidth : 0.0f};

    Layout layout;
    layout.order = candidate.order;
    layout.size = finalSize;
    layout.scrollableOverflowRect = layoutOutput.scrollableOverflowRect;
    layout.scrollbarSize = scrollbarSize;
    layout.location = location;
    layout.padding = padding;
    layout.border = border;
    layout.margin = resolvedMargin;
    tree->SetUnroundedLayout(candidate.node, layout);

    if (!layoutOutput.oofCandidates.IsEmpty()) {
        result.surfaced = layoutOutput.oofCandidates;
    }

    float relX = location.x - usedOffset.x;
    PointF relativeLocation = {
        IsRtl(direction) ? usedArea.w - relX - finalSize.w : relX,
        location.y - usedOffset.y};
    result.layout = layout;
    bool isScrollContainer = IsScrollContainer(containerOverflow.x) ||
                             IsScrollContainer(containerOverflow.y);
    result.overflowContribution = ComputeScrollableOverflowContribution(
        relativeLocation, finalSize, layoutOutput.scrollableOverflowRect,
        overflow, contain, isScrollContainer);
    return result;
}

void PerformOofLayout(TaffyTree* tree, NodeId nodeId, OofCandidates candidates,
                      SizeF areaSize, PointF areaOffset, Direction direction,
                      ContainingBlockClaims claims,
                      PointOverflow containerOverflow, Vec<NodeId>* hoisted,
                      OofCandidates* unclaimed, RectF* overflowOut) {
    RectF absoluteOverflowRect = RectF::Zero();
    if (candidates.IsEmpty()) {
        *overflowOut = absoluteOverflowRect;
        return;
    }

    Vec<OofCandidate> work;
    for (int i = 0; i < len(candidates.inner); i++) {
        const OofCandidate& candidate = candidates.inner[i];
        if (claims.ForPosition(candidate.position)) {
            VecAppend(work, candidate);
        } else {
            unclaimed->Push(candidate);
        }
    }

    int index = 0;
    while (index < len(work)) {
        OofCandidate candidate = work[index];
        index += 1;
        VecAppend(*hoisted, candidate.node);
        OofBoxLayout box =
            LayoutOofBox(tree, candidate, areaSize, areaOffset, direction, true,
                         nodeId, containerOverflow, nullptr);
        if (!box.surfaced.IsEmpty()) {
            box.surfaced.Translate(box.layout.location);
        }
        for (int s = 0; s < len(box.surfaced.inner); s++) {
            const OofCandidate& surfaced = box.surfaced.inner[s];
            if (claims.ForPosition(surfaced.position)) {
                VecAppend(work, surfaced);
            } else {
                unclaimed->Push(surfaced);
            }
        }
        absoluteOverflowRect = absoluteOverflowRect
                                   .Union(box.overflowContribution);
    }

    *overflowOut = absoluteOverflowRect;
}

void ComputeOofLayout(TaffyTree* tree, NodeId node, LayoutOutput* output) {
    if (!output->hasOofPositioningArea) {
        return;
    }
    const Style& style = tree->GetStyle(node);
    Direction direction = style.direction;
    ContainingBlockClaims claims = ContainingBlockFor(style.position);
    OofCandidates candidates = output->oofCandidates;
    output->oofCandidates.Clear();

    Vec<NodeId> hoisted;
    OofCandidates unclaimed;
    RectF overflow = RectF::Zero();
    PerformOofLayout(tree, node, candidates, output->oofPositioningArea.size,
                     output->oofPositioningArea.offset, direction, claims,
                     style.overflow, &hoisted, &unclaimed, &overflow);
    tree->ClearHoistedChildren(node);
    tree->AddHoistedChildren(node, hoisted);
    output->oofCandidates = unclaimed;
    output->scrollableOverflowRect = output->scrollableOverflowRect
                                         .Union(overflow);
}

}

#line 1 "src/taffy/compute.cpp"

namespace taffy {

void ComputeRootLayout(TaffyTree* tree, NodeId root, SizeAvail availableSpace) {
    CalcResolver calc = tree->calc;
    Position rootPosition = tree->GetStyle(root).position;
    Direction rootDirection = tree->GetStyle(root).direction;
    SizeFOpt icbSize = availableSpace.IntoOptions();

    bool oofRoot =
        IsOutOfFlow(rootPosition) && IsSome(icbSize.w) && IsSome(icbSize.h);

    bool rootIsCached = false;
    Layout layout;
    OofCandidates candidates;

    if (oofRoot) {
        OofCandidate self;
        self.node = root;
        self.order = 0;
        self.position = rootPosition;
        float xAnchor = IsRtl(rootDirection) ? icbSize.w : 0.0f;
        self.staticX = AxisStaticPosition::FromEdge(
            xAnchor,
            IsRtl(rootDirection) ? AxisStaticEdge::End : AxisStaticEdge::Start);
        self.staticY =
            AxisStaticPosition::FromEdge(0.0f, AxisStaticEdge::Start);

        PointOverflow icbOverflow = {Overflow::Visible, Overflow::Visible};
        OofBoxLayout box = LayoutOofBox(tree, self, {icbSize.w, icbSize.h},
                                        {0.0f, 0.0f}, rootDirection, false,
                                        root, icbOverflow, &rootIsCached);
        layout = box.layout;
        candidates = box.surfaced;
    } else {
        SizeFOpt knownDimensions = SizeFOptNone();

        const Style& rootStyle = tree->GetStyle(root);
        RectF rootMargin = rootStyle.margin.ResolveOrZero(
            availableSpace.width.IntoOption(), calc);
        SizeF rootMarginSum = rootMargin.SumAxes();
        SizeAvail rootAvailableSpace = {
            MaybeMax(MaybeSub(availableSpace.width, Some(rootMarginSum.w)),
                     Some(0.0f)),
            MaybeMax(MaybeSub(availableSpace.height, Some(rootMarginSum.h)),
                     Some(0.0f))};

        {
            SizeFOpt parentSize = availableSpace.IntoOptions();
            const Style& style = tree->GetStyle(root);

            if (style.IsBlock()) {
                Optf aspectRatio = style.aspectRatio;
                RectF padding = style.padding.ResolveOrZero(parentSize.w, calc);
                RectF border = style.border.ResolveOrZero(parentSize.w, calc);
                SizeF paddingBorderSize = (padding + border).SumAxes();
                SizeF boxSizingAdjustment =
                    style.boxSizing == BoxSizing::ContentBox ? paddingBorderSize
                                                             : SizeF::Zero();

                SizeFOpt minSize =
                    MaybeAdd(MaybeApplyAspectRatio(
                                 style.minSize.MaybeResolve(parentSize, calc),
                                 aspectRatio),
                             boxSizingAdjustment);
                SizeFOpt maxSize =
                    MaybeAdd(MaybeApplyAspectRatio(
                                 style.maxSize.MaybeResolve(parentSize, calc),
                                 aspectRatio),
                             boxSizingAdjustment);
                SizeFOpt clampedStyleSize = MaybeClamp(
                    MaybeAdd(MaybeApplyAspectRatio(
                                 style.size.MaybeResolve(parentSize, calc),
                                 aspectRatio),
                             boxSizingAdjustment),
                    minSize, maxSize);

                SizeFOpt minMaxDefiniteSize = SizeFOptNone();
                if (IsSome(minSize.w) && IsSome(maxSize.w) &&
                    maxSize.w <= minSize.w) {
                    minMaxDefiniteSize.w = minSize.w;
                }
                if (IsSome(minSize.h) && IsSome(maxSize.h) &&
                    maxSize.h <= minSize.h) {
                    minMaxDefiniteSize.h = minSize.h;
                }

                SizeFOpt availableSpaceBasedSize = SizeFOptNone();
                availableSpaceBasedSize.w = rootAvailableSpace.width
                                                .IntoOption();

                SizeFOpt known = Or(knownDimensions, minMaxDefiniteSize);
                known = Or(known, clampedStyleSize);
                known = Or(known, availableSpaceBasedSize);
                knownDimensions = MaybeMax(known, paddingBorderSize);
            }
        }

        LayoutInput rootInput;
        rootInput.knownDimensions = knownDimensions;
        rootInput.parentSize = availableSpace.IntoOptions();
        rootInput.availableSpace = rootAvailableSpace;
        rootInput.sizingMode = SizingMode::InherentSize;
        rootInput.axis = RequestedAxis::Both;
        rootInput.runMode = RunMode::PerformLayout;
        rootInput.verticalMarginsAreCollapsible = LineBool::False();
        LayoutOutput cachedProbe;

        rootIsCached = tree->CacheGet(root, rootInput, &cachedProbe);

        LayoutOutput output = tree->PerformChildLayout(
            root, knownDimensions, availableSpace.IntoOptions(),
            rootAvailableSpace, SizingMode::InherentSize, LineBool::False());

        const Style& style = tree->GetStyle(root);
        Optf widthOpt = availableSpace.width.IntoOption();
        RectF padding = style.padding.ResolveOrZero(widthOpt, calc);
        RectF border = style.border.ResolveOrZero(widthOpt, calc);
        RectF margin = style.margin.ResolveOrZero(widthOpt, calc);

        SizeF scrollbarSize = {
            style.overflow.y == Overflow::Scroll ? style.scrollbarWidth : 0.0f,
            style.overflow.x == Overflow::Scroll ? style.scrollbarWidth : 0.0f};

        PointF location = {margin.left, margin.top};
        if (IsRtl(style.direction) && IsSome(widthOpt)) {
            location.x = widthOpt - output.size.w - margin.right;
        }

        if (style.position == Position::Relative) {
            RectFOpt inset = style.inset.MaybeResolveZip(icbSize, calc);
            Optf negRight = inset.right;
            if (IsSome(negRight)) {
                negRight = -negRight;
            }
            Optf negBottom = inset.bottom;
            if (IsSome(negBottom)) {
                negBottom = -negBottom;
            }
            Optf dx = IsRtl(style.direction) ? Or(negRight, inset.left)
                                             : Or(inset.left, negRight);
            location.x += UnwrapOr(dx, 0.0f);
            location.y += UnwrapOr(Or(inset.top, negBottom), 0.0f);
        }

        layout.order = 0;
        layout.location = location;
        layout.size = output.size;
        layout.scrollableOverflowRect = output.scrollableOverflowRect;
        layout.scrollbarSize = scrollbarSize;
        layout.padding = padding;
        layout.border = border;

        layout.margin = margin;
        tree->SetUnroundedLayout(root, layout);
        candidates = output.oofCandidates;
    }

    if (!candidates.IsEmpty()) {
        RectF areaInset = layout.border;
        areaInset.right += layout.scrollbarSize.w;
        areaInset.bottom += layout.scrollbarSize.h;
        SizeF rootPaddingBox = layout.size - areaInset.SumAxes();
        SizeF areaSize = rootPaddingBox;
        PointF areaOffset = {areaInset.left, areaInset.top};
        if (IsSome(icbSize.w)) {
            areaSize.w = F32Max(icbSize.w - layout.scrollbarSize.w, 0.0f);
            areaOffset.x = -layout.location.x;
        }
        if (IsSome(icbSize.h)) {
            areaSize.h = F32Max(icbSize.h - layout.scrollbarSize.h, 0.0f);
            areaOffset.y = -layout.location.y;
        }
        const Style& tailStyle = tree->GetStyle(root);
        Vec<NodeId> hoisted;
        OofCandidates unclaimed;
        RectF overflow = RectF::Zero();
        PerformOofLayout(tree, root, candidates, areaSize, areaOffset,
                         rootDirection, ContainingBlockClaims::All(),
                         tailStyle.overflow, &hoisted, &unclaimed, &overflow);
        if (!rootIsCached) {
            tree->AddHoistedChildren(root, hoisted);
        }
    }
}

LayoutOutput ComputeHiddenLayout(TaffyTree* tree, NodeId node) {
    tree->CacheClear(node);
    tree->SetUnroundedLayout(node, Layout::WithOrder(0));

    int n = tree->ChildCount(node);
    for (int i = 0; i < n; i++) {
        tree->ComputeChildLayout(tree->GetChildId(node, i),
                                 LayoutInput::Hidden());
    }
    return LayoutOutput::Hidden();
}

static void RoundLayoutInner(TaffyTree* tree, NodeId nodeId, float cumulativeX,
                             float cumulativeY) {
    Layout unrounded = tree->GetUnroundedLayout(nodeId);
    Layout layout = unrounded;

    float parentX = cumulativeX;
    float parentY = cumulativeY;
    cumulativeX += unrounded.location.x;
    cumulativeY += unrounded.location.y;

    layout.location.x = F32Round(cumulativeX) - F32Round(parentX);
    layout.location.y = F32Round(cumulativeY) - F32Round(parentY);
    layout.size
        .w = F32Round(cumulativeX + unrounded.size.w) - F32Round(cumulativeX);
    layout.size
        .h = F32Round(cumulativeY + unrounded.size.h) - F32Round(cumulativeY);
    layout.scrollbarSize.w = F32Round(unrounded.scrollbarSize.w);
    layout.scrollbarSize.h = F32Round(unrounded.scrollbarSize.h);
    layout.border.left =
        F32Round(cumulativeX + unrounded.border.left) - F32Round(cumulativeX);
    layout.border.right =
        F32Round(cumulativeX + unrounded.size.w) -
        F32Round(cumulativeX + unrounded.size.w - unrounded.border.right);
    layout.border.top =
        F32Round(cumulativeY + unrounded.border.top) - F32Round(cumulativeY);
    layout.border.bottom =
        F32Round(cumulativeY + unrounded.size.h) -
        F32Round(cumulativeY + unrounded.size.h - unrounded.border.bottom);
    layout.padding.left =
        F32Round(cumulativeX + unrounded.padding.left) - F32Round(cumulativeX);
    layout.padding.right =
        F32Round(cumulativeX + unrounded.size.w) -
        F32Round(cumulativeX + unrounded.size.w - unrounded.padding.right);
    layout.padding.top =
        F32Round(cumulativeY + unrounded.padding.top) - F32Round(cumulativeY);
    layout.padding.bottom =
        F32Round(cumulativeY + unrounded.size.h) -
        F32Round(cumulativeY + unrounded.size.h - unrounded.padding.bottom);
    layout.scrollableOverflowRect
        .left = F32Round(cumulativeX + unrounded.scrollableOverflowRect.left) -
                F32Round(cumulativeX);
    layout.scrollableOverflowRect.right =
        F32Round(cumulativeX + unrounded.scrollableOverflowRect.right) -
        F32Round(cumulativeX);
    layout.scrollableOverflowRect
        .top = F32Round(cumulativeY + unrounded.scrollableOverflowRect.top) -
               F32Round(cumulativeY);
    layout.scrollableOverflowRect.bottom =
        F32Round(cumulativeY + unrounded.scrollableOverflowRect.bottom) -
        F32Round(cumulativeY);

    tree->SetFinalLayout(nodeId, layout);

    int n = tree->ChildCount(nodeId);
    for (int i = 0; i < n; i++) {
        NodeId child = tree->GetChildId(nodeId, i);
        if (!IsOutOfFlow(tree->GetStyle(child).position)) {
            RoundLayoutInner(tree, child, cumulativeX, cumulativeY);
        }
    }
    int hoisted = tree->HoistedChildCount(nodeId);
    for (int i = 0; i < hoisted; i++) {
        RoundLayoutInner(tree, tree->HoistedChildAt(nodeId, i), cumulativeX,
                         cumulativeY);
    }
}

void RoundLayout(TaffyTree* tree, NodeId node) {
    RoundLayoutInner(tree, node, 0.0f, 0.0f);
}

LayoutOutput ComputeLeafLayout(const LayoutInput& inputs, const Style& style,
                               CalcResolver calc, LeafMeasureFn measure,
                               void* measureCtx) {
    SizeFOpt knownDimensions = inputs.knownDimensions;
    SizeFOpt parentSize = inputs.parentSize;
    SizeAvail availableSpaceIn = inputs.availableSpace;

    RectF padding = style.padding.ResolveOrZero(parentSize.w, calc);
    RectF border = style.border.ResolveOrZero(parentSize.w, calc);
    RectF paddingBorder = padding + border;
    SizeF pbSum = paddingBorder.SumAxes();
    SizeF boxSizingAdjustment =
        style.boxSizing == BoxSizing::ContentBox ? pbSum : SizeF::Zero();

    SizeFOpt nodeSize = SizeFOptNone();
    SizeFOpt nodeMinSize = SizeFOptNone();
    SizeFOpt nodeMaxSize = SizeFOptNone();
    Optf aspectRatio = None();
    if (inputs.sizingMode == SizingMode::ContentSize) {
        nodeSize = knownDimensions;
    } else {
        aspectRatio = style.aspectRatio;
        SizeFOpt styleSize = MaybeAdd(
            MaybeApplyAspectRatio(style.size.MaybeResolve(parentSize, calc),
                                  aspectRatio),
            boxSizingAdjustment);
        SizeFOpt styleMinSize = MaybeAdd(
            MaybeApplyAspectRatio(style.minSize.MaybeResolve(parentSize, calc),
                                  aspectRatio),
            boxSizingAdjustment);
        SizeFOpt styleMaxSize = MaybeAdd(
            style.maxSize.MaybeResolve(parentSize, calc), boxSizingAdjustment);
        nodeSize = Or(knownDimensions, styleSize);
        nodeMinSize = styleMinSize;
        nodeMaxSize = styleMaxSize;
    }

    PointOverflow t = style.overflow.Transpose();
    PointF scrollbarGutter = {
        t.x == Overflow::Scroll ? style.scrollbarWidth : 0.0f,
        t.y == Overflow::Scroll ? style.scrollbarWidth : 0.0f};

    RectF contentBoxInset = paddingBorder;
    contentBoxInset.right += scrollbarGutter.x;
    contentBoxInset.bottom += scrollbarGutter.y;

    bool hasStylesPreventingBeingCollapsedThrough =
        !style.IsBlock() || IsScrollContainer(style.overflow.x) ||
        IsScrollContainer(style.overflow.y) || IsOutOfFlow(style.position) ||
        style.contain.EstablishesIndependentFormattingContext() ||
        padding.top > 0.0f || padding.bottom > 0.0f || border.top > 0.0f ||
        border.bottom > 0.0f || (IsSome(nodeSize.h) && nodeSize.h > 0.0f) ||
        (IsSome(nodeMinSize.h) && nodeMinSize.h > 0.0f);

    if (inputs.runMode == RunMode::ComputeSize &&
        hasStylesPreventingBeingCollapsedThrough && BothAxisDefined(nodeSize)) {
        SizeF size = {nodeSize.w, nodeSize.h};
        size = MaybeMax(MaybeClamp(size, nodeMinSize, nodeMaxSize),
                        AsOptional(pbSum));
        LayoutOutput out;
        out.size = size;
        return out;
    }

    SizeAvail availableSpace;
    AvailableSpace availWidth =
        IsSome(knownDimensions.w) ? AvailableSpace::Definite(knownDimensions.w)
                                  : availableSpaceIn.width;
    availableSpace.width = availWidth.MaybeSet(knownDimensions.w)
                               .MaybeSet(nodeSize.w);
    if (availableSpace.width.IsDefinite()) {
        availableSpace.width =
            AvailableSpace::Definite(MaybeClamp(availableSpace.width.value,
                                                nodeMinSize.w, nodeMaxSize.w) -
                                     contentBoxInset.HorizontalAxisSum());
    }
    AvailableSpace availHeight =
        IsSome(knownDimensions.h) ? AvailableSpace::Definite(knownDimensions.h)
                                  : availableSpaceIn.height;
    availableSpace.height = availHeight.MaybeSet(knownDimensions.h)
                                .MaybeSet(nodeSize.h);
    if (availableSpace.height.IsDefinite()) {
        availableSpace.height =
            AvailableSpace::Definite(MaybeClamp(availableSpace.height.value,
                                                nodeMinSize.h, nodeMaxSize.h) -
                                     contentBoxInset.VerticalAxisSum());
    }

    SizeFOpt measureKnown = inputs.runMode == RunMode::ComputeSize
                                ? knownDimensions
                                : SizeFOptNone();
    SizeF measuredSize = measure
                             ? measure(measureKnown, availableSpace, measureCtx)
                             : SizeF::Zero();

    SizeF clampedSize =
        MaybeClamp(UnwrapOr(Or(knownDimensions, nodeSize),
                            measuredSize + contentBoxInset.SumAxes()),
                   nodeMinSize, nodeMaxSize);
    SizeF size = {
        clampedSize.w,
        F32Max(clampedSize.h,
               IsSome(aspectRatio) ? clampedSize.w / aspectRatio : 0.0f)};
    size = MaybeMax(size, AsOptional(pbSum));

    LayoutOutput out;
    out.size = size;

    bool isScrollContainer = IsScrollContainer(style.overflow.x) ||
                             IsScrollContainer(style.overflow.y);
    bool isRtl = IsRtl(style.direction);
    float startPadding = isRtl ? padding.right : padding.left;
    float endPadding = isRtl ? padding.left : padding.right;
    out.scrollableOverflowRect = {
        0.0f,
        startPadding + measuredSize.w + (isScrollContainer ? endPadding : 0.0f),
        0.0f,
        padding.top + measuredSize.h +
            (isScrollContainer ? padding.bottom : 0.0f)};
    out.marginsCanCollapseThrough = !hasStylesPreventingBeingCollapsedThrough &&
                                    size.h == 0.0f && measuredSize.h == 0.0f;
    return out;
}

AlignContentKeyword ApplyAlignmentFallback(float freeSpace, int numItems,
                                           AlignContent alignmentMode) {

    AlignContentKeyword keyword =
        alignmentMode.keyword == AlignContentKeyword::Normal
            ? AlignContentKeyword::Stretch
            : alignmentMode.keyword;
    bool isSafe = alignmentMode.safety == AlignmentSafety::Safe;

    if (numItems <= 1 || freeSpace <= 0.0f) {
        switch (keyword) {
            case AlignContentKeyword::Stretch:
            case AlignContentKeyword::SpaceBetween:
                keyword = AlignContentKeyword::FlexStart;
                isSafe = true;
                break;
            case AlignContentKeyword::SpaceAround:
            case AlignContentKeyword::SpaceEvenly:
                keyword = AlignContentKeyword::Center;
                isSafe = true;
                break;
            default:
                break;
        }
    }

    if (freeSpace <= 0.0f && isSafe) {
        keyword = AlignContentKeyword::Start;
    }
    return keyword;
}

float ComputeAlignmentOffset(float freeSpace, int numItems, float gap,
                             AlignContentKeyword alignmentMode,
                             bool layoutIsFlexReversed, bool isFirst) {
    if (isFirst) {
        switch (alignmentMode) {
            case AlignContentKeyword::Start:
                return 0.0f;
            case AlignContentKeyword::FlexStart:
                return layoutIsFlexReversed ? freeSpace : 0.0f;
            case AlignContentKeyword::End:
                return freeSpace;
            case AlignContentKeyword::FlexEnd:
                return layoutIsFlexReversed ? 0.0f : freeSpace;
            case AlignContentKeyword::Center:
                return freeSpace / 2.0f;
            case AlignContentKeyword::Normal:
            case AlignContentKeyword::Stretch:
            case AlignContentKeyword::SpaceBetween:
                return 0.0f;
            case AlignContentKeyword::SpaceAround:
                return freeSpace >= 0.0f
                           ? (freeSpace /
                              (float)(numItems > 0 ? numItems : 1)) /
                                 2.0f
                           : freeSpace / 2.0f;
            case AlignContentKeyword::SpaceEvenly:
                return freeSpace >= 0.0f ? freeSpace / (float)(numItems + 1)
                                         : freeSpace / 2.0f;
        }
        return 0.0f;
    }

    float free = F32Max(freeSpace, 0.0f);
    switch (alignmentMode) {
        case AlignContentKeyword::SpaceBetween:
            return gap + free / (float)(numItems - 1);
        case AlignContentKeyword::SpaceAround:
            return gap + free / (float)numItems;
        case AlignContentKeyword::SpaceEvenly:
            return gap + free / (float)(numItems + 1);
        default:
            return gap;
    }
}

RectF ComputeScrollableOverflowContribution(PointF location, SizeF size,
                                            RectF scrollableOverflowRect,
                                            PointOverflow overflow,
                                            Contain contain,
                                            bool parentIsScrollContainer) {
    bool isScrollContainer =
        IsScrollContainer(overflow.x) || IsScrollContainer(overflow.y);

    bool overflowIsContained = contain.ContainsScrollableOverflow();

    bool propagatesX = !isScrollContainer && !overflowIsContained &&
                       overflow.x == Overflow::Visible;
    bool propagatesY = !isScrollContainer && !overflowIsContained &&
                       overflow.y == Overflow::Visible;
    SizeF endExtent = {
        propagatesX ? F32Max(size.w, scrollableOverflowRect.right) : size.w,
        propagatesY ? F32Max(size.h, scrollableOverflowRect.bottom) : size.h};
    if (endExtent.w <= 0.0f || endExtent.h <= 0.0f) {
        return RectF::Zero();
    }
    PointF startExtent = {
        propagatesX ? F32Min(0.0f, scrollableOverflowRect.left) : 0.0f,
        propagatesY ? F32Min(0.0f, scrollableOverflowRect.top) : 0.0f};
    RectF contribution = {location.x + startExtent.x, location.x + endExtent.w,
                          location.y + startExtent.y, location.y + endExtent.h};
    bool whollyUnreachable =
        contribution.right <= 0.0f || contribution.bottom <= 0.0f;
    if (parentIsScrollContainer && whollyUnreachable) {
        return RectF::Zero();
    }
    return contribution;
}

}

#line 1 "src/taffy/style.cpp"

namespace taffy {

static Optf MaybeResolveRaw(CompactLength raw, Optf context,
                            CalcResolver calc) {
    switch (raw.Tag()) {
        case CompactLength::kAutoTag:
            return None();
        case CompactLength::kLengthTag:
            return Some(raw.Value());
        case CompactLength::kPercentTag:
            return IsSome(context) ? Some(context * raw.Value()) : None();

        case CompactLength::kMinContentTag:
        case CompactLength::kMaxContentTag:
        case CompactLength::kFitContentPxTag:
        case CompactLength::kFitContentPercentTag:
        case CompactLength::kFitContentKeywordTag:
        case CompactLength::kStretchTag:
        case CompactLength::kContentTag:
            return None();
        default:
            break;
    }
    if (raw.IsCalc() && IsSome(context)) {
        return Some(calc.Resolve(raw.CalcValue(), context));
    }
    return None();
}

static float ResolveOrZeroRaw(CompactLength raw, Optf context,
                              CalcResolver calc) {
    return UnwrapOr(MaybeResolveRaw(raw, context, calc), 0.0f);
}

Optf LengthPercentageAuto::ResolveToOption(float context,
                                           CalcResolver calc) const {
    return MaybeResolveRaw(raw, Some(context), calc);
}

Optf LengthPercentageAuto::MaybeResolve(Optf context, CalcResolver calc) const {
    return MaybeResolveRaw(raw, context, calc);
}

Optf Dimension::MaybeResolve(Optf context, CalcResolver calc) const {
    return MaybeResolveRaw(raw, context, calc);
}

SizeFOpt SizeDim::MaybeResolve(SizeFOpt context, CalcResolver calc) const {
    return {MaybeResolveRaw(width.raw, context.w, calc),
            MaybeResolveRaw(height.raw, context.h, calc)};
}

SizeF SizeDim::ResolveOrZero(SizeFOpt context, CalcResolver calc) const {
    return {ResolveOrZeroRaw(width.raw, context.w, calc),
            ResolveOrZeroRaw(height.raw, context.h, calc)};
}

SizeF SizeLp::ResolveOrZero(SizeFOpt context, CalcResolver calc) const {
    return {ResolveOrZeroRaw(width.raw, context.w, calc),
            ResolveOrZeroRaw(height.raw, context.h, calc)};
}

SizeF SizeLp::ResolveOrZero(Optf context, CalcResolver calc) const {
    return {ResolveOrZeroRaw(width.raw, context, calc),
            ResolveOrZeroRaw(height.raw, context, calc)};
}

RectF RectLp::ResolveOrZero(SizeFOpt context, CalcResolver calc) const {
    return {ResolveOrZeroRaw(left.raw, context.w, calc),
            ResolveOrZeroRaw(right.raw, context.w, calc),
            ResolveOrZeroRaw(top.raw, context.h, calc),
            ResolveOrZeroRaw(bottom.raw, context.h, calc)};
}

RectF RectLp::ResolveOrZero(Optf context, CalcResolver calc) const {
    return {ResolveOrZeroRaw(left.raw, context, calc),
            ResolveOrZeroRaw(right.raw, context, calc),
            ResolveOrZeroRaw(top.raw, context, calc),
            ResolveOrZeroRaw(bottom.raw, context, calc)};
}

RectF RectLpa::ResolveOrZero(SizeFOpt context, CalcResolver calc) const {
    return {ResolveOrZeroRaw(left.raw, context.w, calc),
            ResolveOrZeroRaw(right.raw, context.w, calc),
            ResolveOrZeroRaw(top.raw, context.h, calc),
            ResolveOrZeroRaw(bottom.raw, context.h, calc)};
}

RectF RectLpa::ResolveOrZero(Optf context, CalcResolver calc) const {
    return {ResolveOrZeroRaw(left.raw, context, calc),
            ResolveOrZeroRaw(right.raw, context, calc),
            ResolveOrZeroRaw(top.raw, context, calc),
            ResolveOrZeroRaw(bottom.raw, context, calc)};
}

RectFOpt RectLpa::MaybeResolve(Optf context, CalcResolver calc) const {
    return {MaybeResolveRaw(left.raw, context, calc),
            MaybeResolveRaw(right.raw, context, calc),
            MaybeResolveRaw(top.raw, context, calc),
            MaybeResolveRaw(bottom.raw, context, calc)};
}

RectFOpt RectLpa::MaybeResolveZip(SizeFOpt context, CalcResolver calc) const {
    return {MaybeResolveRaw(left.raw, context.w, calc),
            MaybeResolveRaw(right.raw, context.w, calc),
            MaybeResolveRaw(top.raw, context.h, calc),
            MaybeResolveRaw(bottom.raw, context.h, calc)};
}

OriginZeroLine IntoOriginZeroLine(GridLine line, uint16_t explicitTrackCount) {
    int32_t explicitLineCount = (int32_t)explicitTrackCount + 1;
    int32_t value = 0;
    if (line.v > 0) {
        value = (int32_t)line.v - 1;
    } else if (line.v < 0) {
        value = (int32_t)line.v + explicitLineCount;
    }
    value = value < kMinOzLine   ? kMinOzLine
            : value > kMaxOzLine ? kMaxOzLine
                                 : value;
    return OriginZeroLine{(int16_t)value};
}

static uint16_t ClampGridSpan(uint16_t span) {
    if (span < 1) {
        return 1;
    }
    return span > kMaxGridTracks ? kMaxGridTracks : span;
}

static PlainPlacement IntoOriginZeroIgnoringNamed(const GridPlacement& p,
                                                  uint16_t explicitTrackCount) {
    switch (p.kind) {
        case GridPlacementKind::Span:
            return PlainPlacement::Spanning(ClampGridSpan(p.span));
        case GridPlacementKind::Line:

            if (p.line == 0) {
                return PlainPlacement::Auto();
            }
            return PlainPlacement::AtLine(
                IntoOriginZeroLine(GridLine{p.line}, explicitTrackCount).v);
        default:

            return PlainPlacement::Auto();
    }
}

bool LinePlacement::IsDefinite() const {
    if (start.kind == GridPlacementKind::Line && start.line != 0) {
        return true;
    }
    if (end.kind == GridPlacementKind::Line && end.line != 0) {
        return true;
    }
    return start.kind == GridPlacementKind::NamedLine ||
           end.kind == GridPlacementKind::NamedLine;
}

LinePlain LinePlacement::IntoOriginZeroIgnoringNamed(
    uint16_t explicitTrackCount) const {
    return {taffy::IntoOriginZeroIgnoringNamed(start, explicitTrackCount),
            taffy::IntoOriginZeroIgnoringNamed(end, explicitTrackCount)};
}

bool LinePlain::IsDefinite() const {
    return start.IsLine() || end.IsLine();
}

bool LinePlain::IsDefiniteGridLine() const {
    return (start.IsLine() && start.line != 0) ||
           (end.IsLine() && end.line != 0);
}

static PlainPlacement PlainIntoOriginZero(PlainPlacement p,
                                          uint16_t explicitTrackCount) {
    if (p.IsSpan()) {
        return PlainPlacement::Spanning(ClampGridSpan(p.span));
    }
    if (p.IsLine()) {
        if (p.line == 0) {
            return PlainPlacement::Auto();
        }
        return PlainPlacement::AtLine(
            IntoOriginZeroLine(GridLine{p.line}, explicitTrackCount).v);
    }
    return PlainPlacement::Auto();
}

LinePlain LinePlain::IntoOriginZero(uint16_t explicitTrackCount) const {
    return {PlainIntoOriginZero(start, explicitTrackCount),
            PlainIntoOriginZero(end, explicitTrackCount)};
}

uint16_t LinePlain::IndefiniteSpan() const {
    if (start.IsSpan()) {
        return start.span < kMaxGridTracks ? start.span : kMaxGridTracks;
    }
    if (end.IsSpan()) {
        return end.span < kMaxGridTracks ? end.span : kMaxGridTracks;
    }

    return 1;
}

LineOzl LinePlain::ResolveDefiniteGridLines() const {
    OriginZeroLine s = start.Ozl();
    OriginZeroLine e = end.Ozl();
    if (start.IsLine() && end.IsLine()) {
        if (s == e) {
            return {s, s + (uint16_t)1};
        }
        return s < e ? LineOzl{s, e} : LineOzl{e, s};
    }
    if (start.IsLine() && end.IsSpan()) {
        return {s, s + end.span};
    }
    if (start.IsLine()) {
        return {s, s + (uint16_t)1};
    }
    if (start.IsSpan() && end.IsLine()) {
        return {e - start.span, e};
    }

    return {e - (uint16_t)1, e};
}

LineOptOzl LinePlain::ResolveAbsolutelyPositionedGridTracks() const {
    OriginZeroLine s = start.Ozl();
    OriginZeroLine e = end.Ozl();
    if (start.IsLine() && end.IsLine()) {
        if (s == e) {
            return {OptOriginZeroLine(s), OptOriginZeroLine(s + (uint16_t)1)};
        }
        if (s < e) {
            return {OptOriginZeroLine(s), OptOriginZeroLine(e)};
        }
        return {OptOriginZeroLine(e), OptOriginZeroLine(s)};
    }
    if (start.IsLine() && end.IsSpan()) {
        return {OptOriginZeroLine(s), OptOriginZeroLine(s + end.span)};
    }
    if (start.IsLine()) {
        return {OptOriginZeroLine(s), OptOriginZeroLine()};
    }
    if (start.IsSpan() && end.IsLine()) {
        return {OptOriginZeroLine(e - start.span), OptOriginZeroLine(e)};
    }
    if (end.IsLine()) {
        return {OptOriginZeroLine(), OptOriginZeroLine(e)};
    }
    return {};
}

LineOzl LinePlain::ResolveIndefiniteGridTracks(OriginZeroLine s) const {
    if (start.IsSpan()) {
        return {s, s + start.span};
    }
    if (end.IsSpan()) {
        return {s, s + end.span};
    }
    return {s, s + (uint16_t)1};
}

bool MaxTrackSizingFunction::HasDefiniteValue(Optf parentSize) const {
    switch (raw.Tag()) {
        case CompactLength::kLengthTag:
            return true;
        case CompactLength::kPercentTag:
            return IsSome(parentSize);
        default:
            return raw.IsCalc() && IsSome(parentSize);
    }
}

Optf MaxTrackSizingFunction::DefiniteValue(Optf parentSize,
                                           CalcResolver calc) const {
    switch (raw.Tag()) {
        case CompactLength::kLengthTag:
            return Some(raw.Value());
        case CompactLength::kPercentTag:
            return IsSome(parentSize) ? Some(raw.Value() * parentSize) : None();
        default:
            break;
    }
    if (raw.IsCalc() && IsSome(parentSize)) {
        return Some(calc.Resolve(raw.CalcValue(), parentSize));
    }
    return None();
}

Optf MaxTrackSizingFunction::DefiniteLimit(Optf parentSize,
                                           CalcResolver calc) const {
    switch (raw.Tag()) {
        case CompactLength::kFitContentPxTag:
            return Some(raw.Value());
        case CompactLength::kFitContentPercentTag:
            return IsSome(parentSize) ? Some(raw.Value() * parentSize) : None();
        default:
            break;
    }
    if (raw.IsFitContentCalc()) {
        return IsSome(parentSize)
                   ? Some(calc.Resolve(raw.CalcValue(), parentSize))
                   : None();
    }
    return DefiniteValue(parentSize, calc);
}

Optf MinTrackSizingFunction::DefiniteValue(Optf parentSize,
                                           CalcResolver calc) const {
    switch (raw.Tag()) {
        case CompactLength::kLengthTag:
            return Some(raw.Value());
        case CompactLength::kPercentTag:
            return IsSome(parentSize) ? Some(raw.Value() * parentSize) : None();
        default:
            break;
    }
    if (raw.IsCalc() && IsSome(parentSize)) {
        return Some(calc.Resolve(raw.CalcValue(), parentSize));
    }
    return None();
}

bool operator==(const Style& a, const Style& b) {
    return a.display == b.display && a.itemIsTable == b.itemIsTable &&
           a.itemIsReplaced == b.itemIsReplaced &&
           a.itemIsCompressibleReplaced == b.itemIsCompressibleReplaced &&
           a.boxSizing == b.boxSizing && a.direction == b.direction &&

           a.overflow == b.overflow &&
           SameFloatBits(a.scrollbarWidth, b.scrollbarWidth) &&
           a.contain == b.contain &&

           a.floatMode == b.floatMode && a.clear == b.clear &&

           a.position == b.position && a.inset == b.inset &&

           a.size == b.size && a.minSize == b.minSize &&
           a.maxSize == b.maxSize && SameOptf(a.aspectRatio, b.aspectRatio) &&

           a.margin == b.margin && a.padding == b.padding &&
           a.border == b.border &&

           a.alignItems == b.alignItems && a.alignSelf == b.alignSelf &&
           a.justifyItems == b.justifyItems && a.justifySelf == b.justifySelf &&
           a.alignContent == b.alignContent &&
           a.justifyContent == b.justifyContent && a.gap == b.gap &&

           a.textAlign == b.textAlign &&

           a.flexDirection == b.flexDirection && a.flexWrap == b.flexWrap &&
           a.flexLineCount == b.flexLineCount &&

           a.flexBasis == b.flexBasis &&
           SameFloatBits(a.flexGrow, b.flexGrow) &&
           SameFloatBits(a.flexShrink, b.flexShrink) &&

           SameSlice(a.gridTemplateRows, b.gridTemplateRows) &&
           SameSlice(a.gridTemplateColumns, b.gridTemplateColumns) &&
           SameSlice(a.gridAutoRows, b.gridAutoRows) &&
           SameSlice(a.gridAutoColumns, b.gridAutoColumns) &&
           a.gridAutoFlow == b.gridAutoFlow &&

           SameSlice(a.gridTemplateAreas.areas, b.gridTemplateAreas.areas) &&
           a.gridTemplateAreas.rowCount == b.gridTemplateAreas.rowCount &&
           a.gridTemplateAreas.columnCount == b.gridTemplateAreas.columnCount &&
           SameSlice(a.gridTemplateColumnNames, b.gridTemplateColumnNames) &&
           SameSlice(a.gridTemplateRowNames, b.gridTemplateRowNames) &&

           a.gridRow == b.gridRow && a.gridColumn == b.gridColumn;
}

}

#line 1 "src/taffy/taffy_tree.cpp"

namespace taffy {

using base::Str;

static NodeId MakeId(int32_t index, uint32_t generation) {
    return NodeId{((uint64_t)generation << 32) | (uint64_t)(index + 1)};
}

static int32_t IdIndex(NodeId id) {
    return (int32_t)((uint32_t)(id.raw & 0xffffffffu)) - 1;
}

static uint32_t IdGeneration(NodeId id) {
    return (uint32_t)(id.raw >> 32);
}

NodeData* TaffyTree::Get(NodeId node) const {
    int32_t idx = IdIndex(node);
    if (idx < 0 || idx >= slots.len) {
        return nullptr;
    }
    NodeData* d = slots[idx];
    if (!d || !d->alive || d->generation != IdGeneration(node)) {
        return nullptr;
    }
    return d;
}

void TaffyTree::Init(int capacity) {
    VecReset(slots);
    VecReset(freeSlots);
    liveCount = 0;
    allocs = 0;
    useRounding = true;
    if (capacity > 0) {
        base::VecReserve(slots, capacity);
    }
}

void TaffyTree::Free() {
    for (int i = 0; i < slots.len; i++) {
        NodeData* d = slots[i];
        if (!d) {
            continue;
        }
        VecReset(d->children);
        delete d;
    }
    VecReset(slots);
    VecReset(freeSlots);
    liveCount = 0;
}

static int32_t AllocSlot(TaffyTree* tree) {
    if (tree->freeSlots.len > 0) {
        int32_t idx = tree->freeSlots[tree->freeSlots.len - 1];
        tree->freeSlots.len--;
        return idx;
    }
    VecAppend(tree->slots, nullptr);
    return tree->slots.len - 1;
}

static NodeId InsertNode(TaffyTree* tree, const Style& style) {
    int32_t idx = AllocSlot(tree);
    NodeData* d = tree->slots[idx];
    uint32_t generation = 1;
    if (d) {
        generation = d->generation + 1;
    } else {
        d = new NodeData();
        tree->slots[idx] = d;
        tree->allocs++;
    }

    d->style = style;
    d->unroundedLayout = Layout{};
    d->finalLayout = Layout{};
    d->hasContext = false;
    d->context = nullptr;

    d->cache.presentMask = 0;
    d->cache.isEmpty = true;
    d->generation = generation;
    d->alive = true;
    d->children.len = 0;
    d->hoisted.len = 0;
    d->gridOof.Clear();
    d->parent = NodeId{};
    d->hasParent = false;

    tree->liveCount++;
    return MakeId(idx, generation);
}

NodeId TaffyTree::NewLeaf(const Style& style) {
    return InsertNode(this, style);
}

NodeId TaffyTree::NewLeafWithContext(const Style& style, void* context) {
    NodeId id = InsertNode(this, style);
    NodeData* d = Get(id);
    d->hasContext = true;
    d->context = context;
    return id;
}

NodeId TaffyTree::NewWithChildren(const Style& style, const NodeId* children,
                                  int n) {
    NodeId id = InsertNode(this, style);
    NodeData* d = Get(id);
    for (int i = 0; i < n; i++) {
        NodeData* c = Get(children[i]);
        if (!c) {
            continue;
        }
        c->parent = id;
        c->hasParent = true;
        VecAppend(d->children, children[i]);
    }
    return id;
}

void TaffyTree::Clear() {

    VecReset(freeSlots);
    for (int i = 0; i < slots.len; i++) {
        NodeData* d = slots[i];
        if (!d) {
            continue;
        }
        d->alive = false;
        d->children.len = 0;
        d->hasParent = false;
        d->parent = NodeId{};
        VecAppend(freeSlots, (int32_t)i);
    }
    liveCount = 0;
}

void TaffyTree::Remove(NodeId node) {
    NodeData* d = Get(node);
    if (!d) {
        return;
    }
    if (d->hasParent) {
        NodeData* p = Get(d->parent);
        if (p) {
            int w = 0;
            for (int i = 0; i < p->children.len; i++) {
                if (p->children[i] != node) {
                    p->children[w++] = p->children[i];
                }
            }
            p->children.len = w;
        }

        MarkDirty(d->parent);
    }

    for (int i = 0; i < d->children.len; i++) {
        NodeData* c = Get(d->children[i]);
        if (c) {
            c->hasParent = false;
        }
    }
    d->alive = false;
    d->children.len = 0;
    d->hasParent = false;
    liveCount--;
    VecAppend(freeSlots, IdIndex(node));
}

static void MarkReachable(const TaffyTree* tree, NodeId id, uint8_t* seen,
                          int nSlots) {
    NodeData* d = tree->Get(id);
    if (!d) {
        return;
    }
    int32_t idx = IdIndex(id);
    if (idx < 0 || idx >= nSlots || seen[idx]) {
        return;
    }
    seen[idx] = 1;
    for (int i = 0; i < d->children.len; i++) {
        MarkReachable(tree, d->children[i], seen, nSlots);
    }
}

void TaffyTree::EachUnreachable(NodeId root, void (*fn)(NodeId, void*),
                                void* user) {
    if (!fn || slots.len <= 0) {
        return;
    }
    uint8_t* seen = (uint8_t*)base::Alloc(nullptr, slots.len);
    if (!seen) {
        return;
    }
    memset(seen, 0, (size_t)slots.len);
    MarkReachable(this, root, seen, slots.len);
    Vec<NodeId> ids;
    for (int i = 0; i < slots.len; i++) {
        NodeData* d = slots[i];
        if (d && d->alive && !seen[i]) {
            VecAppend(ids, MakeId((int32_t)i, d->generation));
        }
    }
    base::Free(nullptr, seen);
    for (int i = 0; i < len(ids); i++) {
        fn(ids[i], user);
    }
    VecReset(ids);
}

void TaffyTree::SetNodeContext(NodeId node, void* context, bool hasContext) {
    NodeData* d = Get(node);
    if (!d) {
        return;
    }
    d->hasContext = hasContext;
    d->context = hasContext ? context : nullptr;
    MarkDirty(node);
}

void* TaffyTree::GetNodeContext(NodeId node) const {
    NodeData* d = Get(node);
    return d && d->hasContext ? d->context : nullptr;
}

void TaffyTree::AddChild(NodeId parent, NodeId child) {
    NodeData* p = Get(parent);
    NodeData* c = Get(child);
    if (!p || !c) {
        return;
    }
    c->parent = parent;
    c->hasParent = true;
    VecAppend(p->children, child);
    MarkDirty(parent);
}

bool TaffyTree::InsertChildAtIndex(NodeId parent, int childIndex,
                                   NodeId child) {
    NodeData* p = Get(parent);
    NodeData* c = Get(child);
    if (!p || !c) {
        return false;
    }
    if (childIndex > p->children.len || childIndex < 0) {
        return false;
    }
    c->parent = parent;
    c->hasParent = true;
    VecInsertAt(p->children, childIndex, child);
    MarkDirty(parent);
    return true;
}

void TaffyTree::SetChildren(NodeId parent, const NodeId* children, int n) {
    NodeData* p = Get(parent);
    if (!p) {
        return;
    }

    for (int i = 0; i < p->children.len; i++) {
        NodeData* c = Get(p->children[i]);
        if (c) {
            c->hasParent = false;
        }
    }
    for (int i = 0; i < n; i++) {
        NodeData* c = Get(children[i]);
        if (!c) {
            continue;
        }

        if (c->hasParent && c->parent != parent) {
            RemoveChild(c->parent, children[i]);
        }
        c->parent = parent;
        c->hasParent = true;
    }
    p->children.len = 0;
    for (int i = 0; i < n; i++) {
        VecAppend(p->children, children[i]);
    }
    MarkDirty(parent);
}

NodeId TaffyTree::RemoveChild(NodeId parent, NodeId child) {
    NodeData* p = Get(parent);
    if (!p) {
        return NodeId{};
    }
    for (int i = 0; i < p->children.len; i++) {
        if (p->children[i] == child) {
            return RemoveChildAtIndex(parent, i);
        }
    }
    return NodeId{};
}

NodeId TaffyTree::RemoveChildAtIndex(NodeId parent, int childIndex) {
    NodeData* p = Get(parent);
    if (!p || childIndex < 0 || childIndex >= p->children.len) {
        return NodeId{};
    }
    NodeId child = p->children[childIndex];
    for (int i = childIndex; i + 1 < p->children.len; i++) {
        p->children[i] = p->children[i + 1];
    }
    p->children.len--;
    NodeData* c = Get(child);
    if (c) {
        c->hasParent = false;
    }
    MarkDirty(parent);
    return child;
}

void TaffyTree::RemoveChildrenRange(NodeId parent, int start, int end) {
    NodeData* p = Get(parent);
    if (!p) {
        return;
    }
    if (start < 0) {
        start = 0;
    }
    if (end > p->children.len) {
        end = p->children.len;
    }
    if (end <= start) {
        return;
    }
    for (int i = start; i < end; i++) {
        NodeData* c = Get(p->children[i]);
        if (c) {
            c->hasParent = false;
        }
    }
    int n = end - start;
    for (int i = end; i < p->children.len; i++) {
        p->children[i - n] = p->children[i];
    }
    p->children.len -= n;
    MarkDirty(parent);
}

NodeId TaffyTree::ReplaceChildAtIndex(NodeId parent, int childIndex,
                                      NodeId newChild) {
    NodeData* p = Get(parent);
    if (!p || childIndex < 0 || childIndex >= p->children.len) {
        return NodeId{};
    }
    NodeData* nc = Get(newChild);
    if (nc) {
        nc->parent = parent;
        nc->hasParent = true;
    }
    NodeId oldChild = p->children[childIndex];
    p->children[childIndex] = newChild;
    NodeData* oc = Get(oldChild);
    if (oc) {
        oc->hasParent = false;
    }
    MarkDirty(parent);
    return oldChild;
}

NodeId TaffyTree::ChildAtIndex(NodeId parent, int childIndex) const {
    NodeData* p = Get(parent);
    if (!p || childIndex < 0 || childIndex >= p->children.len) {
        return NodeId{};
    }
    return p->children[childIndex];
}

NodeId TaffyTree::Parent(NodeId child, bool* hasParent) const {
    NodeData* c = Get(child);
    if (!c || !c->hasParent) {
        *hasParent = false;
        return NodeId{};
    }
    *hasParent = true;
    return c->parent;
}

void TaffyTree::SetStyle(NodeId node, const Style& style) {
    NodeData* d = Get(node);
    if (!d) {
        return;
    }
    d->style = style;
    MarkDirty(node);
}

static const Style& DefaultStyle() {
    static const Style kDefault;
    return kDefault;
}

const Style& TaffyTree::GetStyle(NodeId node) const {
    NodeData* d = Get(node);
    return d ? d->style : DefaultStyle();
}

static const Layout& DefaultLayout() {
    static const Layout kDefault;
    return kDefault;
}

const Layout& TaffyTree::GetLayout(NodeId node) const {
    NodeData* d = Get(node);
    if (!d) {
        return DefaultLayout();
    }
    return useRounding ? d->finalLayout : d->unroundedLayout;
}

const Layout& TaffyTree::UnroundedLayout(NodeId node) const {
    NodeData* d = Get(node);
    return d ? d->unroundedLayout : DefaultLayout();
}

void TaffyTree::MarkDirty(NodeId node) {
    NodeId cur = node;
    while (true) {
        NodeData* d = Get(cur);
        if (!d) {
            return;
        }

        if (!d->cache.Clear()) {
            return;
        }
        if (!d->hasParent) {
            return;
        }
        cur = d->parent;
    }
}

bool TaffyTree::Dirty(NodeId node) const {
    NodeData* d = Get(node);
    return d ? d->cache.IsEmpty() : true;
}

int TaffyTree::ChildCount(NodeId parent) const {
    NodeData* d = Get(parent);
    return d ? d->children.len : 0;
}

NodeId TaffyTree::GetChildId(NodeId parent, int index) const {
    NodeData* d = Get(parent);
    return d && index >= 0 && index < d->children.len ? d->children[index]
                                                      : NodeId{};
}

void TaffyTree::SetUnroundedLayout(NodeId node, const Layout& layout) {
    NodeData* d = Get(node);
    if (d) {
        d->unroundedLayout = layout;
    }
}

Layout TaffyTree::GetUnroundedLayout(NodeId node) const {
    NodeData* d = Get(node);
    return d ? d->unroundedLayout : Layout{};
}

void TaffyTree::SetFinalLayout(NodeId node, const Layout& layout) {
    NodeData* d = Get(node);
    if (d) {
        d->finalLayout = layout;
    }
}

Layout TaffyTree::GetFinalLayout(NodeId node) const {
    NodeData* d = Get(node);
    if (!d) {
        return Layout{};
    }
    return useRounding ? d->finalLayout : d->unroundedLayout;
}

const char* TaffyTree::GetDebugLabel(NodeId node) const {
    NodeData* d = Get(node);
    if (!d) {
        return "NONE";
    }
    if (d->style.display == Display::None) {
        return "NONE";
    }
    if (d->children.len == 0) {
        return "LEAF";
    }
    switch (d->style.display) {
        case Display::Block:
            return "BLOCK";
        case Display::FlowRoot:
            return "FLOW-ROOT";
        case Display::Grid:
            return "GRID";
        default:
            return IsRow(d->style.flexDirection) ? "FLEX ROW" : "FLEX COL";
    }
}

bool TaffyTree::CacheGet(NodeId node, const LayoutInput& input,
                         LayoutOutput* out) const {
    NodeData* d = Get(node);
    return d ? d->cache.Get(input, out) : false;
}

void TaffyTree::CacheStore(NodeId node, const LayoutInput& input,
                           const LayoutOutput& output) {
    NodeData* d = Get(node);
    if (d) {
        d->cache.Store(input, output);
    }
}

void TaffyTree::CacheClear(NodeId node) {
    NodeData* d = Get(node);
    if (d) {
        d->cache.Clear();
    }
}

void TaffyTree::ClearHoistedChildren(NodeId node) {
    NodeData* d = Get(node);
    if (d) {
        VecReset(d->hoisted);
    }
}

void TaffyTree::AddHoistedChildren(NodeId node, const Vec<NodeId>& ids) {
    NodeData* d = Get(node);
    if (!d) {
        return;
    }
    for (int i = 0; i < len(ids); i++) {
        VecAppend(d->hoisted, ids[i]);
    }
}

int TaffyTree::HoistedChildCount(NodeId node) const {
    NodeData* d = Get(node);
    return d ? len(d->hoisted) : 0;
}

NodeId TaffyTree::HoistedChildAt(NodeId node, int index) const {
    NodeData* d = Get(node);
    if (!d || index < 0 || index >= len(d->hoisted)) {
        return NodeId{};
    }
    return d->hoisted[index];
}

LayoutOutput TaffyTree::ComputeBlockChildLayout(NodeId node, LayoutInput inputs,
                                                BlockContext* blockCtx) {

    if (inputs.runMode == RunMode::PerformHiddenLayout) {
        return ComputeHiddenLayout(this, node);
    }

    NodeData* d = Get(node);
    if (!d) {
        return LayoutOutput::Hidden();
    }
    CacheKey key = CacheKey::From(inputs);
    LayoutOutput cached;
    if (d->cache.ModeMatches(inputs) &&
        d->cache.GetWithKey(key, inputs.runMode, &cached)) {
        return cached;
    }

    Display displayMode = d->style.display;
    bool hasChildren = d->children.len > 0;

    d->gridOof.Clear();

    LayoutOutput out;
    if (displayMode == Display::None) {
        out = ComputeHiddenLayout(this, node);
    } else if (!hasChildren) {

        if (measureFn) {
            out = measureFn(inputs, node, d->hasContext ? d->context : nullptr,
                            &d->style, measureUserData);
        } else {
            out = ComputeLeafLayout(inputs, d->style, calc, nullptr, nullptr);
        }
    } else {
        switch (displayMode) {
            case Display::Block:
                out = ComputeBlockLayout(this, node, inputs, blockCtx);
                break;
            case Display::FlowRoot:
                out = ComputeBlockLayout(this, node, inputs, nullptr);
                break;
            case Display::Grid:
                out = ComputeGridLayout(this, node, inputs);
                break;
            default:
                out = ComputeFlexboxLayout(this, node, inputs);
                break;
        }
    }

    if (inputs.runMode == RunMode::PerformLayout) {
        ComputeOofLayout(this, node, &out);
    }

    d->cache.StoreWithKey(key, inputs, out);
    return out;
}

LayoutOutput TaffyTree::ComputeChildLayout(NodeId node, LayoutInput inputs) {
    return ComputeBlockChildLayout(node, inputs, nullptr);
}

float TaffyTree::MeasureChildSize(NodeId node, SizeFOpt knownDimensions,
                                  SizeFOpt parentSize, SizeAvail availableSpace,
                                  SizingMode sizingMode, AbsoluteAxis axis,
                                  LineBool verticalMarginsAreCollapsible) {
    LayoutInput in;
    in.knownDimensions = knownDimensions;
    in.parentSize = parentSize;
    in.availableSpace = availableSpace;
    in.sizingMode = sizingMode;
    in.axis = ToRequestedAxis(axis);
    in.runMode = RunMode::ComputeSize;
    in.verticalMarginsAreCollapsible = verticalMarginsAreCollapsible;
    return GetAbs(ComputeChildLayout(node, in).size, axis);
}

SizeF TaffyTree::MeasureChildSizeBoth(NodeId node, SizeFOpt knownDimensions,
                                      SizeFOpt parentSize,
                                      SizeAvail availableSpace,
                                      SizingMode sizingMode,
                                      LineBool verticalMarginsAreCollapsible) {
    LayoutInput in;
    in.knownDimensions = knownDimensions;
    in.parentSize = parentSize;
    in.availableSpace = availableSpace;
    in.sizingMode = sizingMode;
    in.axis = RequestedAxis::Both;
    in.runMode = RunMode::ComputeSize;
    in.verticalMarginsAreCollapsible = verticalMarginsAreCollapsible;
    return ComputeChildLayout(node, in).size;
}

LayoutOutput TaffyTree::PerformChildLayout(
    NodeId node, SizeFOpt knownDimensions, SizeFOpt parentSize,
    SizeAvail availableSpace, SizingMode sizingMode,
    LineBool verticalMarginsAreCollapsible) {
    LayoutInput in;
    in.knownDimensions = knownDimensions;
    in.parentSize = parentSize;
    in.availableSpace = availableSpace;
    in.sizingMode = sizingMode;
    in.axis = RequestedAxis::Both;
    in.runMode = RunMode::PerformLayout;
    in.verticalMarginsAreCollapsible = verticalMarginsAreCollapsible;
    return ComputeChildLayout(node, in);
}

void TaffyTree::ComputeLayoutWithMeasure(NodeId node, SizeAvail availableSpace,
                                         MeasureFn measure, void* userData) {
    measureFn = measure;
    measureUserData = userData;
    ComputeRootLayout(this, node, availableSpace);
    if (useRounding) {
        RoundLayout(this, node);
    }
    measureFn = nullptr;
    measureUserData = nullptr;
}

void TaffyTree::ComputeLayout(NodeId node, SizeAvail availableSpace) {
    ComputeLayoutWithMeasure(node, availableSpace, nullptr, nullptr);
}

static void PrintNode(TaffyTree* tree, NodeId node, bool hasSibling,
                      const char* linesString, int depth) {
    Layout layout = tree->GetFinalLayout(node);
    const char* displayStr = tree->GetDebugLabel(node);
    const char* fork = hasSibling ? "├── " : "└── ";

    base::log(base::fmt(
        "%s%s%s [x: %-4g y: %-4g w: %-4g h: %-4g "
        "overflow: l:%g r:%g t:%g b:%g",
        Str(linesString), Str(fork), Str(displayStr), (double)layout.location.x,
        (double)layout.location.y, (double)layout.size.w, (double)layout.size.h,
        (double)layout.scrollableOverflowRect.left,
        (double)layout.scrollableOverflowRect.right,
        (double)layout.scrollableOverflowRect.top,
        (double)layout.scrollableOverflowRect.bottom));
    base::log(
        base::fmt(" border: l:%g r:%g t:%g b:%g, "
                  "padding: l:%g r:%g t:%g b:%g] (%llu)\n",
                  (double)layout.border.left, (double)layout.border.right,
                  (double)layout.border.top, (double)layout.border.bottom,
                  (double)layout.padding.left, (double)layout.padding.right,
                  (double)layout.padding.top, (double)layout.padding.bottom,
                  (unsigned long long)node.raw));

    constexpr int kMaxDepth = 32;
    if (depth >= kMaxDepth) {
        return;
    }
    const char* bar = hasSibling ? "│   " : "    ";
    base::TempStr lines = base::fmt("%s%s", Str(linesString), Str(bar));

    int count = tree->ChildCount(node);
    for (int i = 0; i < count; i++) {
        PrintNode(tree, tree->GetChildId(node, i), i < count - 1, lines.s,
                  depth + 1);
    }
}

void TaffyTree::PrintTree(NodeId root) {
    base::log(StrL("TREE\n"));
    PrintNode(this, root, false, "", 0);
}

}

#line 1 "src/taffy/tree.cpp"

namespace taffy {

static constexpr uint32_t kInfinityBits = 0x7f800000u;
static constexpr uint32_t kNegInfinityBits = 0xff800000u;

static constexpr uint64_t kSignBit1 = (uint64_t)1 << 63;
static constexpr uint64_t kSignBit2 = (uint64_t)1 << 31;
static constexpr uint64_t kBothSignBitsMask = kSignBit1 | kSignBit2;
static constexpr uint64_t kNonSignBitsMask = ~kBothSignBitsMask;

static constexpr uint64_t kXAxisValueMask = (uint64_t)0xffffffffu << 32;

static uint32_t ToBits(float v) {
    uint32_t out = 0;
    memcpy(&out, &v, sizeof(out));
    return out;
}

static uint32_t OptionCacheKey(Optf v) {
    return IsSome(v) ? ToBits(v) : kInfinityBits;
}

static uint64_t SizeOptionCacheKey(SizeFOpt s) {
    return ((uint64_t)OptionCacheKey(s.w) << 32) |
           (uint64_t)OptionCacheKey(s.h);
}

static uint32_t AvailableSpaceCacheKey(AvailableSpace a) {
    switch (a.kind) {
        case AvailableSpace::Kind::Definite:
            return ToBits(-a.value);
        case AvailableSpace::Kind::MinContent:
            return kNegInfinityBits;
        default:
            return kInfinityBits;
    }
}

static uint32_t MixedCacheKey(Optf kd, AvailableSpace avs) {
    return IsSome(kd) ? ToBits(kd) : AvailableSpaceCacheKey(avs);
}

static uint64_t SizeMixedCacheKey(SizeFOpt kd, SizeAvail avs) {
    return ((uint64_t)MixedCacheKey(kd.w, avs.width) << 32) |
           (uint64_t)MixedCacheKey(kd.h, avs.height);
}

CacheKey CacheKey::From(const LayoutInput& input) {
    uint64_t extraBits = 0;
    switch (input.axis) {
        case RequestedAxis::Horizontal:
            extraBits = kSignBit1;
            break;
        case RequestedAxis::Vertical:
            extraBits = kSignBit2;
            break;
        default:
            extraBits = kSignBit1 | kSignBit2;
            break;
    }
    CacheKey key;
    key.kdAvailableSpace =
        SizeMixedCacheKey(input.knownDimensions, input.availableSpace);
    key.parentSize =
        (SizeOptionCacheKey(input.parentSize) & kNonSignBitsMask) | extraBits;

    key.knownDimensionsAreDefiniteW =
        input.knownDimensionsAreDefiniteW || !IsSome(input.knownDimensions.w);
    key.knownDimensionsAreDefiniteH =
        input.knownDimensionsAreDefiniteH || !IsSome(input.knownDimensions.h);
    return key;
}

uint64_t CacheKey::XAxisParentSize() const {
    return parentSize & (kXAxisValueMask & kNonSignBitsMask);
}

bool CacheKey::SizeIsValidFor(CacheKey other) const {
    uint64_t entryAxis = parentSize & kBothSignBitsMask;
    return entryAxis == kBothSignBitsMask ||
           entryAxis == (other.parentSize & kBothSignBitsMask);
}

bool Cache::Get(const LayoutInput& input, LayoutOutput* out) {
    if (!ModeMatches(input)) {
        return false;
    }
    return GetWithKey(CacheKey::From(input), input.runMode, out);
}

void Cache::Store(const LayoutInput& input, const LayoutOutput& output) {
    StoreWithKey(CacheKey::From(input), input, output);
}

bool Cache::GetWithKey(CacheKey key, RunMode runMode, LayoutOutput* out) {
    switch (runMode) {
        case RunMode::PerformLayout:
            if ((presentMask & kFinalBit) && finalLayoutEntry.key == key) {
                *out = finalLayoutEntry.content;
                return true;
            }
            return false;
        case RunMode::ComputeSize: {
            uint64_t xAxis = key.XAxisParentSize();
            uint16_t mask = (uint16_t)(presentMask >> 1);
            for (int i = 0; mask != 0; i++, mask >>= 1) {
                if ((mask & 1) == 0) {
                    continue;
                }
                const CacheEntry<SizeF>& e = measureEntries[i];
                if (e.key.kdAvailableSpace == key.kdAvailableSpace &&
                    e.key.knownDimensionsAreDefiniteW ==
                        key.knownDimensionsAreDefiniteW &&
                    e.key.knownDimensionsAreDefiniteH ==
                        key.knownDimensionsAreDefiniteH &&
                    e.key.XAxisParentSize() == xAxis &&
                    e.key.SizeIsValidFor(key)) {
                    recentlyUsedEntries |= (uint16_t)(1u << i);
                    *out = LayoutOutput::FromOuterSize(e.content);
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

void Cache::StoreWithKey(CacheKey key, const LayoutInput& input,
                         const LayoutOutput& output) {

    if (input.runMode == RunMode::PerformHiddenLayout) {
        return;
    }
    uint8_t nextMode = CacheModeOf(input);
    if (mode != nextMode) {
        Clear();
        mode = nextMode;
    }
    switch (input.runMode) {
        case RunMode::PerformLayout:
            isEmpty = false;
            presentMask |= kFinalBit;
            finalLayoutEntry = {key, output};
            break;
        case RunMode::ComputeSize: {

            if (output.marginsCanCollapseThrough ||
                output.topMargin.positive != 0.0f ||
                output.topMargin.negative != 0.0f ||
                output.bottomMargin.positive != 0.0f ||
                output.bottomMargin.negative != 0.0f) {
                return;
            }
            isEmpty = false;
            for (int i = 0; i < kCacheSize; i++) {
                if ((presentMask & MeasureBit(i)) == 0) {
                    continue;
                }
                if (measureEntries[i].key == key) {
                    measureEntries[i].content = output.size;
                    recentlyUsedEntries |= (uint16_t)(1u << i);
                    return;
                }
            }
            while ((recentlyUsedEntries & (uint16_t)(1u << nextMeasureEntry)) !=
                   0) {
                recentlyUsedEntries &= (uint16_t)~(1u << nextMeasureEntry);
                nextMeasureEntry = (uint8_t)(nextMeasureEntry + 1);
                if (nextMeasureEntry == (uint8_t)kCacheSize) {
                    nextMeasureEntry = 0;
                }
            }
            int entryIndex = nextMeasureEntry;
            presentMask |= MeasureBit(entryIndex);
            measureEntries[entryIndex] = {key, output.size};
            recentlyUsedEntries |= (uint16_t)(1u << entryIndex);
            nextMeasureEntry = (uint8_t)(nextMeasureEntry + 1);
            if (nextMeasureEntry == (uint8_t)kCacheSize) {
                nextMeasureEntry = 0;
            }
            break;
        }
        default:
            break;
    }
}

bool Cache::Clear() {
    if (isEmpty) {
        return false;
    }
    isEmpty = true;

    presentMask = 0;
    recentlyUsedEntries = 0;
    nextMeasureEntry = 0;
    return true;
}

bool Cache::IsEmpty() const {
    return presentMask == 0;
}

}

#if GPUI_OS_ANDROID
#line 1 "src/base_android.cpp"

#include <sys/stat.h>
#include <unistd.h>

namespace base {

bool PlatGetExePath(char* out, int cap) {
    if (!out || cap <= 0) return false;
    out[0] = 0;
    ssize_t n = readlink("/proc/self/exe", out, (size_t)cap - 1);
    if (n <= 0) return false;
    out[n] = 0;
    return true;
}

uint64_t PlatStatModifiedNs(const struct stat* st) {
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ull + (uint64_t)st->st_mtim
                                                              .tv_nsec;
}

bool PlatSecondaryIsCommand() {
    return false;
}
bool PlatTitleBarCloseIsElement() {
    return false;
}
bool PlatShowsWindowControls() {
    return true;
}
float PlatCaretWidth() {
    return 1.f;
}
bool PlatScrollBounce() {
    return true;
}
bool PlatCleanOnEscape() {
    return false;
}
bool PlatScrollBounceTouch() {
    return true;
}
const char* PlatMonoFontName() {
    return "DejaVu Sans Mono";
}
const char* PlatShellDataDir() {
    return "/.local/share";
}
const char* PlatShellPlatformName() {
    return "linux";
}
bool PlatBlockSelectUsesControl() {
    return false;
}
bool PlatScrollGestureLocks() {
    return true;
}
bool PlatAsyncIo() {
    return false;
}
float PlatWindowShadowSize() {
    return 0.f;
}

}

#endif

#if GPUI_OS_IOS
#line 1 "src/base_ios.cpp"

#include <sys/stat.h>

namespace base {

bool PlatGetExePath(char* out, int cap) {
    if (out && cap > 0) out[0] = 0;
    return false;
}

uint64_t PlatStatModifiedNs(const struct stat* st) {
    return (uint64_t)st->st_mtimespec.tv_sec * 1000000000ull +
           (uint64_t)st->st_mtimespec.tv_nsec;
}

bool PlatSecondaryIsCommand() {
    return false;
}
bool PlatTitleBarCloseIsElement() {
    return false;
}
bool PlatShowsWindowControls() {
    return true;
}
float PlatCaretWidth() {
    return 1.f;
}
bool PlatScrollBounce() {
    return true;
}
bool PlatCleanOnEscape() {
    return true;
}
bool PlatScrollBounceTouch() {
    return true;
}
const char* PlatMonoFontName() {
    return "DejaVu Sans Mono";
}
const char* PlatShellDataDir() {
    return "/.local/share";
}
const char* PlatShellPlatformName() {
    return "linux";
}
bool PlatBlockSelectUsesControl() {
    return false;
}
bool PlatScrollGestureLocks() {
    return true;
}
bool PlatAsyncIo() {
    return false;
}
float PlatWindowShadowSize() {
    return 0.f;
}

}

#endif

#if GPUI_OS_LINUX
#line 1 "src/base_linux.cpp"

#include <stdio.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace base {

void PlatDirNameInPlace(char* path);

bool PlatGetExePath(char* out, int cap) {
    if (!out || cap <= 0) return false;
    out[0] = 0;
    ssize_t n = readlink("/proc/self/exe", out, (size_t)cap - 1);
    if (n <= 0) return false;
    out[n] = 0;
    return true;
}

void PlatGetExeDir(char* out, int cap) {
    if (!PlatGetExePath(out, cap)) return;
    PlatDirNameInPlace(out);
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

uint64_t PlatStatModifiedNs(const struct stat* st) {
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ull + (uint64_t)st->st_mtim
                                                              .tv_nsec;
}

bool PlatSecondaryIsCommand() {
    return false;
}
bool PlatShowsWindowControls() {
    return true;
}
bool PlatTitleBarCloseIsElement() {
    return true;
}
float PlatCaretWidth() {
    return 1.f;
}
bool PlatScrollBounce() {
    return false;
}
bool PlatCleanOnEscape() {
    return true;
}
bool PlatScrollBounceTouch() {
    return false;
}
const char* PlatMonoFontName() {
    return "DejaVu Sans Mono";
}
const char* PlatShellDataDir() {
    return "/.local/share";
}
const char* PlatShellPlatformName() {
    return "linux";
}
bool PlatBlockSelectUsesControl() {
    return true;
}
bool PlatScrollGestureLocks() {
    return true;
}
bool PlatAsyncIo() {
    return false;
}
float PlatWindowShadowSize() {
    return 20.f;
}

}

#endif

#if GPUI_OS_MAC
#line 1 "src/base_mac.cpp"

#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>

namespace base {

void PlatDirNameInPlace(char* path);

bool PlatGetExePath(char* out, int cap) {
    if (!out || cap <= 0) return false;
    out[0] = 0;
    uint32_t n = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &n) != 0) return false;
    out[cap - 1] = 0;
    return out[0] != 0;
}

void PlatGetExeDir(char* out, int cap) {
    if (!PlatGetExePath(out, cap)) return;
    PlatDirNameInPlace(out);
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

uint64_t PlatStatModifiedNs(const struct stat* st) {
    return (uint64_t)st->st_mtimespec.tv_sec * 1000000000ull +
           (uint64_t)st->st_mtimespec.tv_nsec;
}

bool PlatSecondaryIsCommand() {
    return true;
}
bool PlatTitleBarCloseIsElement() {
    return false;
}
bool PlatShowsWindowControls() {
    return false;
}
float PlatCaretWidth() {
    return 1.f;
}
bool PlatScrollBounce() {
    return true;
}
bool PlatCleanOnEscape() {
    return true;
}
bool PlatScrollBounceTouch() {
    return false;
}
const char* PlatMonoFontName() {
    return "Menlo";
}
const char* PlatShellDataDir() {
    return "/Library/Application Support";
}
const char* PlatShellPlatformName() {
    return "macos";
}
bool PlatBlockSelectUsesControl() {
    return false;
}
bool PlatScrollGestureLocks() {
    return true;
}
bool PlatAsyncIo() {
    return false;
}
float PlatWindowShadowSize() {
    return 0.f;
}

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

bool PlatMemCommit(void* base, uint64_t size, bool largePages) {
    (void)largePages;
    if (size == 0) {
        return true;
    }
    if (!base) {
        return false;
    }

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

uint64_t PlatArenaReserveSize() {
    return 64ull * 1024ull * 1024ull;
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

uint64_t PlatStatModifiedNs(const struct stat* st);

int StrCmpI(const char* a, const char* b) {
    return strcasecmp(a ? a : "", b ? b : "");
}

int StrCmpNI(const char* a, const char* b, int n) {
    if (n <= 0) {
        return 0;
    }
    return strncasecmp(a ? a : "", b ? b : "", (size_t)n);
}

void StrCopyZ(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) {
        return;
    }
    if (!src) {
        dst[0] = 0;
        return;
    }
    int n = (int)strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(dst, src, (size_t)n);
    dst[n] = 0;
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

void PlatSleepMs(int ms) {
    if (ms <= 0) {
        return;
    }
    struct timespec ts = {};
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

char PlatPathSep() {
    return '/';
}
bool PlatPathsCaseFold() {
    return false;
}
bool PlatIsWindows() {
    return false;
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

bool PlatMemCommit(void* base, uint64_t size, bool largePages) {
    (void)largePages;
    if (size == 0) {
        return true;
    }
    if (!base) {
        return false;
    }

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

uint64_t PlatArenaReserveSize() {
    return 4ull * 1024ull * 1024ull;
}

bool PlatGetExePath(char* out, int cap) {
    if (out && cap > 0) out[0] = 0;
    return false;
}

void PlatGetExeDir(char* out, int cap) {
    if (!out || cap <= 0) {
        return;
    }
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

uint64_t PlatStatModifiedNs(const struct stat* st) {
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ull + (uint64_t)st->st_mtim
                                                              .tv_nsec;
}

bool PlatSecondaryIsCommand() {
    return false;
}
bool PlatTitleBarCloseIsElement() {
    return false;
}
bool PlatShowsWindowControls() {
    return true;
}
float PlatCaretWidth() {
    return 1.f;
}
bool PlatScrollBounce() {
    return false;
}
bool PlatCleanOnEscape() {
    return true;
}
bool PlatScrollBounceTouch() {
    return false;
}
const char* PlatMonoFontName() {
    return "DejaVu Sans Mono";
}
const char* PlatShellDataDir() {
    return "/.local/share";
}
const char* PlatShellPlatformName() {
    return "emscripten";
}
bool PlatBlockSelectUsesControl() {
    return false;
}
bool PlatScrollGestureLocks() {
    return false;
}
bool PlatAsyncIo() {
    return true;
}
float PlatWindowShadowSize() {
    return 0.f;
}

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

bool PlatMemCommit(void* base, uint64_t size, bool largePages) {
    if (size == 0) {
        return true;
    }
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

uint64_t PlatArenaReserveSize() {
    return 64ull * 1024ull * 1024ull;
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

void StrCopyZ(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) {
        return;
    }
    strncpy_s(dst, (size_t)cap, src ? src : "", _TRUNCATE);
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

bool PlatGetExePath(char* out, int cap) {
    if (!out || cap <= 0) return false;
    out[0] = 0;
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
    if (!PlatGetExePath(out, cap)) return;
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] != '/') out[--n] = 0;
    while (n > 0 && out[n - 1] == '/') out[--n] = 0;
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

void PlatSleepMs(int ms) {
    Sleep((DWORD)(ms < 0 ? 0 : ms));
}

char PlatPathSep() {
    return '\\';
}
bool PlatPathsCaseFold() {
    return true;
}
bool PlatIsWindows() {
    return true;
}
bool PlatSecondaryIsCommand() {
    return false;
}
bool PlatTitleBarCloseIsElement() {
    return false;
}
bool PlatShowsWindowControls() {
    return true;
}
float PlatCaretWidth() {
    return 1.f;
}
bool PlatScrollBounce() {
    return false;
}
bool PlatCleanOnEscape() {
    return true;
}
bool PlatScrollBounceTouch() {
    return false;
}
const char* PlatMonoFontName() {
    return "Consolas";
}
const char* PlatShellDataDir() {
    return "\\AppData\\Roaming";
}
const char* PlatShellPlatformName() {
    return "windows";
}
bool PlatBlockSelectUsesControl() {
    return false;
}
bool PlatScrollGestureLocks() {
    return true;
}
bool PlatAsyncIo() {
    return false;
}
float PlatWindowShadowSize() {
    return 0.f;
}

}

#endif
