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
            return (int64_t)(intptr_t)arg.ptr;
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
                             : (const void*)(intptr_t)argToI64(arg);
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

static bool FloatFitsHorizontally(float width, FloatDirection direction,
                                  float bfcWidth, const float floatInsets[2],
                                  const float cbInsets[2]) {
    int lead = (int)direction;
    int trail = 1 - lead;
    float xInset = F32Max(floatInsets[lead], cbInsets[lead]);
    bool fitsOpposite = floatInsets[trail] == 0.0f ||
                        xInset + width <= bfcWidth - floatInsets[trail];
    bool fitsContaining = floatInsets[lead] == 0.0f ||
                          xInset + width <= bfcWidth - cbInsets[trail];
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
    float contribution = 0.0f;
    float widest = 0.0f;

    void AddFloat(float width) {
        switch (availableWidth.kind) {
            case AvailableSpace::Kind::Definite:
            case AvailableSpace::Kind::MaxContent:
                contribution += width;
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

    float floatContentContribution = 0.0f;
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

    bool isReplaced = false;

    bool isInSameBfc = false;

    Float floatMode = Float::None;
    Clear clear = Clear::None;

    SizeFOpt size = SizeFOptNone();
    SizeFOpt minSize = SizeFOptNone();
    SizeFOpt maxSize = SizeFOptNone();

    PointOverflow overflow;
    float scrollbarWidth = 0.0f;

    Position position = Position::Relative;
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
        RectF padding = cs.padding.ResolveOrZero(nodeInnerSize, calc);
        RectF border = cs.border.ResolveOrZero(nodeInnerSize, calc);
        SizeF pbSum = (padding + border).SumAxes();
        SizeF boxSizingAdjustment =
            cs.boxSizing == BoxSizing::ContentBox ? pbSum : SizeF::Zero();

        BlockItem item;
        item.nodeId = childNodeId;
        item.order = order++;
        item.isTable = cs.itemIsTable;
        item.isReplaced = cs.IsCompressibleReplaced();
        item.floatMode = cs.floatMode;
        item.clear = cs.clear;
        item.position = cs.position;
        item.overflow = cs.overflow;
        item.scrollbarWidth = cs.scrollbarWidth;

        bool isNotFloated = cs.floatMode == Float::None;
        bool isScrollContainer = IsScrollContainer(cs.overflow.x) ||
                                 IsScrollContainer(cs.overflow.y);
        item.isInSameBfc = cs.IsBlock() && !cs.itemIsTable &&
                           cs.position != Position::Absolute && isNotFloated &&
                           !isScrollContainer;

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
        if (item.position == Position::Absolute) {
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
            childAvail.width = MaybeSub(childAvail.width, itemXMarginSum);
            width = tree->MeasureChildSize(
                item.nodeId, knownDimensions, SizeFOptNone(), childAvail,
                SizingMode::InherentSize, AbsoluteAxis::Horizontal,
                LineBool::True());
        }
        width = F32Max(width, item.paddingBorderSum.w) + itemXMarginSum;

        if (IsFloated(item.floatMode)) {
            floatContribution.AddFloat(width);
            continue;
        }
        maxChildWidth = F32Max(maxChildWidth, width);
    }

    return F32Max(maxChildWidth, floatContribution.Result());
}

struct InFlowResult {
    SizeF inflowContentSize;
    float intrinsicOuterHeight = 0.0f;
    CollapsibleMarginSet firstChildTopMarginSet;
    CollapsibleMarginSet lastChildBottomMarginSet;
    Optf firstBaseline = None();
};

InFlowResult PerformFinalLayoutOnInFlowChildren(
    TaffyTree* tree, RunMode runMode, Vec<BlockItem>* items,
    float containerOuterWidth, Optf containerPercentageResolutionHeight,
    RectF contentBoxInset, RectF resolvedContentBoxInset, RectF resolvedBorder,
    TextAlign textAlign, Direction direction,
    LineBool ownMarginsCollapseWithChildren, BlockContext* blockCtx) {
    CalcResolver calc = tree->calc;
    float containerInnerWidth = containerOuterWidth - resolvedContentBoxInset
                                                          .HorizontalAxisSum();
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
        if (item.position == Position::Absolute) {
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

            float availableWidth = containerInnerWidth - itemNonAutoXMarginSum;
            LayoutOutput itemLayout = tree->PerformChildLayout(
                item.nodeId, SizeFOptNone(), parentSize,
                {AvailableSpace::Definite(availableWidth),
                 AvailableSpace::MaxContent()},
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

            Layout layout;
            layout.order = item.order;
            layout.size = itemLayout.size;
            layout.contentSize = itemLayout.contentSize;
            layout.scrollbarSize = scrollbarSize;
            layout.location = location;
            layout.padding = item.padding;
            layout.border = item.border;
            layout.margin = itemNonAutoMargin;
            tree->SetUnroundedLayout(item.nodeId, layout);

            res.inflowContentSize = Max(
                res.inflowContentSize,
                ComputeContentSizeContribution(
                    {IsRtl(direction) ? containerOuterWidth -
                                            (location.x + itemLayout.size.w) -
                                            resolvedBorder.right
                                      : location.x - resolvedBorder.left,
                     location.y - resolvedBorder.top},
                    itemLayout.size, itemLayout.contentSize, item.overflow));
            continue;
        }

        float yMarginOffset = 0.0f;
        float stretchWidth;
        PointF floatAvoidingPosition;
        float floatAvoidingWidth;
        bool itemAvoidsFloats = false;
        bool itemPushedBelowFloat = false;

        if (item.isInSameBfc) {
            stretchWidth = containerInnerWidth - itemNonAutoXMarginSum;
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
                                 F32Max(slot.stretchWidth, minAutoWidth)),
                        item.minSize.w, item.maxSize.w);
                    if (width <= slot.borderWidth + 0.001f) {
                        break;
                    }
                    after = slot.segmentId;
                }
                itemPushedBelowFloat = slot.y > minY;
                hasActiveFloats = slot.segmentId >= 0;
                itemAvoidsFloats = true;
                stretchWidth = F32Max(slot.stretchWidth, minAutoWidth);
                floatAvoidingPosition = {slot.x, slot.y};
                floatAvoidingWidth = slot.borderWidth;
            } else {
                stretchWidth = containerInnerWidth - itemNonAutoXMarginSum;
                floatAvoidingPosition = {resolvedContentBoxInset.left, minY};
                floatAvoidingWidth = containerInnerWidth;
            }
        }

        SizeFOpt knownDimensions = SizeFOptNone();
        if (!item.isTable && !item.isReplaced) {
            SizeFOpt sized = item.size;
            sized.w = Some(MaybeClamp(UnwrapOr(sized.w, stretchWidth),
                                      item.minSize.w, item.maxSize.w));
            knownDimensions = MaybeClamp(sized, item.minSize, item.maxSize);
        }

        LayoutInput inputs;
        inputs.runMode = runMode;
        inputs.sizingMode = SizingMode::InherentSize;
        inputs.axis = RequestedAxis::Both;
        inputs.knownDimensions = knownDimensions;
        inputs.parentSize = parentSize;
        inputs.availableSpace = availableSpace;
        inputs.availableSpace.width = AvailableSpace::Definite(stretchWidth);
        inputs.verticalMarginsAreCollapsible =
            item.isInSameBfc ? LineBool::True() : LineBool::False();

        Optf clearThreshold = blockCtx->ClearedThreshold(item.clear);
        float clearPos = UnwrapOr(clearThreshold, -INFINITY);

        LayoutOutput itemLayout;
        if (item.isInSameBfc) {

            float width = UnwrapOr(knownDimensions.w, stretchWidth);

            float insetLeft = itemNonAutoMargin.left + contentBoxInset.left;
            float insetRight = containerOuterWidth - width - insetLeft;
            float insets[2] = {insetLeft, insetRight};

            BlockContext childBlockCtx = blockCtx->SubContext(
                F32Max(yOffsetForAbsolute + itemNonAutoMargin.top, clearPos),
                insets);
            itemLayout = tree->ComputeBlockChildLayout(item.nodeId, inputs,
                                                       &childBlockCtx);
            blockCtx->AddChildFloatedContentHeightContribution(
                yOffsetForAbsolute + childBlockCtx
                                         .FloatedContentHeightContribution());
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
                                topMarginSet.Resolve(),
                                bottomMarginSet.Resolve()};

        RectFOpt inset = item.inset.MaybeResolveZip(
            {Some(containerInnerWidth), Some(0.0f)}, calc);
        Optf negRight = inset.right;
        if (IsSome(negRight)) {
            negRight = -negRight;
        }
        Optf negBottom = inset.bottom;
        if (IsSome(negBottom)) {
            negBottom = -negBottom;
        }
        PointF insetOffset = {IsRtl(direction)
                                  ? UnwrapOr(Or(negRight, inset.left), 0.0f)
                                  : UnwrapOr(Or(inset.left, negRight), 0.0f),
                              UnwrapOr(Or(inset.top, negBottom), 0.0f)};

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

        float itemOuterWidth = itemLayout.size.w + resolvedMargin
                                                       .HorizontalAxisSum();
        if (itemOuterWidth < containerInnerWidth) {
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

        if (!IsSome(res.firstBaseline) && IsSome(itemLayout.firstBaselines.y)) {
            res.firstBaseline = Some(location.y + itemLayout.firstBaselines.y);
        }

        item.hasFinalLayout = true;
        item.finalLayout.order = item.order;
        item.finalLayout.size = itemLayout.size;
        item.finalLayout.contentSize = itemLayout.contentSize;
        item.finalLayout.scrollbarSize = scrollbarSize;
        item.finalLayout.location = location;
        item.finalLayout.padding = item.padding;
        item.finalLayout.border = item.border;
        item.finalLayout.margin = resolvedMargin;

        res.inflowContentSize =
            Max(res.inflowContentSize,
                ComputeContentSizeContribution(
                    {IsRtl(direction)
                         ? containerOuterWidth - (location.x + finalSize.w) -
                               resolvedBorder.right
                         : location.x - resolvedBorder.left,
                     location.y - resolvedBorder.top},
                    finalSize, itemLayout.contentSize, item.overflow));

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

SizeF PerformAbsoluteLayoutOnAbsoluteChildren(TaffyTree* tree,
                                              const Vec<BlockItem>& items,
                                              SizeF areaSize, PointF areaOffset,
                                              Direction direction) {
    CalcResolver calc = tree->calc;
    float areaWidth = areaSize.w;
    float areaHeight = areaSize.h;
    SizeF absoluteContentSize = SizeF::Zero();

    for (int i = 0; i < len(items); i++) {
        const BlockItem& item = items[i];
        if (item.position != Position::Absolute) {
            continue;
        }
        const Style& cs = tree->GetStyle(item.nodeId);
        if (cs.BoxGenMode() == BoxGenerationMode::None ||
            cs.position != Position::Absolute) {
            continue;
        }

        Optf aspectRatio = cs.aspectRatio;
        RectFOpt margin = cs.margin.MaybeResolve(Some(areaWidth), calc);
        RectF padding = cs.padding.ResolveOrZero(Some(areaWidth), calc);
        RectF border = cs.border.ResolveOrZero(Some(areaWidth), calc);
        SizeF paddingBorderSum = (padding + border).SumAxes();
        SizeF boxSizingAdjustment = cs.boxSizing == BoxSizing::ContentBox
                                        ? paddingBorderSum
                                        : SizeF::Zero();

        RectFOpt inset = cs.inset.MaybeResolveZip(AsOptional(areaSize), calc);
        Optf left = inset.left;
        Optf right = inset.right;
        Optf top = inset.top;
        Optf bottom = inset.bottom;

        SizeFOpt styleSize = MaybeAdd(
            MaybeApplyAspectRatio(
                cs.size.MaybeResolve(AsOptional(areaSize), calc), aspectRatio),
            boxSizingAdjustment);
        SizeFOpt minSize = MaybeMax(
            Or(MaybeAdd(MaybeApplyAspectRatio(
                            cs.minSize.MaybeResolve(AsOptional(areaSize), calc),
                            aspectRatio),
                        boxSizingAdjustment),
               AsOptional(paddingBorderSum)),
            paddingBorderSum);
        SizeFOpt maxSize =
            MaybeAdd(MaybeApplyAspectRatio(
                         cs.maxSize.MaybeResolve(AsOptional(areaSize), calc),
                         aspectRatio),
                     boxSizingAdjustment);
        SizeFOpt knownDimensions = MaybeClamp(styleSize, minSize, maxSize);

        if (!IsSome(knownDimensions.w) && IsSome(left) && IsSome(right)) {
            float newWidthRaw =
                MaybeSub(MaybeSub(areaWidth, margin.left), margin.right) -
                left - right;
            knownDimensions.w = Some(F32Max(newWidthRaw, 0.0f));
            knownDimensions =
                MaybeClamp(MaybeApplyAspectRatio(knownDimensions, aspectRatio),
                           minSize, maxSize);
        }
        if (!IsSome(knownDimensions.h) && IsSome(top) && IsSome(bottom)) {
            float newHeightRaw =
                MaybeSub(MaybeSub(areaHeight, margin.top), margin.bottom) -
                top - bottom;
            knownDimensions.h = Some(F32Max(newHeightRaw, 0.0f));
            knownDimensions =
                MaybeClamp(MaybeApplyAspectRatio(knownDimensions, aspectRatio),
                           minSize, maxSize);
        }

        SizeAvail childAvail = {AvailableSpace::Definite(MaybeClamp(
                                    areaWidth, minSize.w, maxSize.w)),
                                AvailableSpace::Definite(MaybeClamp(
                                    areaHeight, minSize.h, maxSize.h))};

        SizeF measuredSize = tree->MeasureChildSizeBoth(
            item.nodeId, knownDimensions, AsOptional(areaSize), childAvail,
            SizingMode::ContentSize, LineBool::False());
        SizeF finalSize = MaybeClamp(UnwrapOr(knownDimensions, measuredSize),
                                     minSize, maxSize);

        LayoutOutput layoutOutput = tree->PerformChildLayout(
            item.nodeId, AsOptional(finalSize), AsOptional(areaSize),
            childAvail, SizingMode::ContentSize, LineBool::False());

        RectF nonAutoMargin = {
            IsSome(left) ? UnwrapOr(margin.left, 0.0f) : 0.0f,
            IsSome(right) ? UnwrapOr(margin.right, 0.0f) : 0.0f,
            IsSome(top) ? UnwrapOr(margin.top, 0.0f) : 0.0f,
            IsSome(bottom) ? UnwrapOr(margin.bottom, 0.0f) : 0.0f};

        PointF absoluteAutoMarginSpace = {
            IsSome(right) ? areaSize.w - right - UnwrapOr(left, 0.0f)
                          : finalSize.w,
            IsSome(bottom) ? areaSize.h - bottom - UnwrapOr(top, 0.0f)
                           : finalSize.h};
        SizeF freeSpace = {absoluteAutoMarginSpace.x - finalSize.w -
                               nonAutoMargin.HorizontalAxisSum(),
                           absoluteAutoMarginSpace.y - finalSize.h -
                               nonAutoMargin.VerticalAxisSum()};

        int autoW =
            (IsSome(margin.left) ? 0 : 1) + (IsSome(margin.right) ? 0 : 1);
        int autoH =
            (IsSome(margin.top) ? 0 : 1) + (IsSome(margin.bottom) ? 0 : 1);
        SizeF autoMarginSize;
        if (autoW == 2 &&
            (!IsSome(styleSize.w) || styleSize.w >= freeSpace.w)) {
            autoMarginSize.w = 0.0f;
        } else if (autoW > 0) {
            autoMarginSize.w = freeSpace.w / (float)autoW;
        }
        if (autoH == 2 &&
            (!IsSome(styleSize.h) || styleSize.h >= freeSpace.h)) {
            autoMarginSize.h = 0.0f;
        } else if (autoH > 0) {
            autoMarginSize.h = freeSpace.h / (float)autoH;
        }
        RectF autoMargin = {IsSome(margin.left) ? 0.0f : autoMarginSize.w,
                            IsSome(margin.right) ? 0.0f : autoMarginSize.w,
                            IsSome(margin.top) ? 0.0f : autoMarginSize.h,
                            IsSome(margin.bottom) ? 0.0f : autoMarginSize.h};
        RectF resolvedMargin = {UnwrapOr(margin.left, autoMargin.left),
                                UnwrapOr(margin.right, autoMargin.right),
                                UnwrapOr(margin.top, autoMargin.top),
                                UnwrapOr(margin.bottom, autoMargin.bottom)};

        float xOffset;
        if (IsSome(left) && IsSome(right)) {
            xOffset = IsRtl(direction) ? areaSize.w - finalSize.w - right -
                                             resolvedMargin.right
                                       : left + resolvedMargin.left;
        } else if (IsSome(left)) {
            xOffset = left + resolvedMargin.left;
        } else if (IsSome(right)) {
            xOffset = areaSize.w - finalSize.w - right - resolvedMargin.right;
        } else {
            xOffset = IsRtl(direction) ? item.staticPosition.x - finalSize.w -
                                             resolvedMargin.right - areaOffset.x
                                       : item.staticPosition.x +
                                             resolvedMargin.left - areaOffset.x;
        }

        float yLocation;
        if (IsSome(top)) {
            yLocation = top + resolvedMargin.top + areaOffset.y;
        } else if (IsSome(bottom)) {
            yLocation = areaSize.h - finalSize.h - bottom -
                        resolvedMargin.bottom + areaOffset.y;
        } else {
            yLocation = item.staticPosition.y + resolvedMargin.top;
        }
        PointF location = {xOffset + areaOffset.x, yLocation};

        SizeF scrollbarSize = {
            item.overflow.y == Overflow::Scroll ? item.scrollbarWidth : 0.0f,
            item.overflow.x == Overflow::Scroll ? item.scrollbarWidth : 0.0f};

        Layout layout;
        layout.order = item.order;
        layout.size = finalSize;
        layout.contentSize = layoutOutput.contentSize;
        layout.scrollbarSize = scrollbarSize;
        layout.location = location;
        layout.padding = padding;
        layout.border = border;
        layout.margin = resolvedMargin;
        tree->SetUnroundedLayout(item.nodeId, layout);

        PointF relativeLocation = {location.x - areaOffset.x,
                                   location.y - areaOffset.y};
        absoluteContentSize = Max(absoluteContentSize,
                                  ComputeContentSizeContribution(
                                      relativeLocation, finalSize,
                                      layoutOutput.contentSize, item.overflow));
    }

    return absoluteContentSize;
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
    SizeFOpt containerContentBoxSize =
        MaybeSub(knownDimensions, contentBoxInset.SumAxes());

    bool isScrollContainer = IsScrollContainer(style.overflow.x) ||
                             IsScrollContainer(style.overflow.y);

    LineBool ownMarginsCollapseWithChildren = {
        verticalMarginsAreCollapsible.start && !isScrollContainer &&
            style.position == Position::Relative && padding.top == 0.0f &&
            border.top == 0.0f,
        verticalMarginsAreCollapsible.end && !isScrollContainer &&
            style.position == Position::Relative && padding.bottom == 0.0f &&
            border.bottom == 0.0f && !IsSome(size.h)};
    bool hasStylesPreventingBeingCollapsedThrough =
        !style.IsBlock() || blockCtx->IsBfcRoot() || isScrollContainer ||
        style.position == Position::Absolute || padding.top > 0.0f ||
        padding.bottom > 0.0f || border.top > 0.0f || border.bottom > 0.0f ||
        (IsSome(size.h) && size.h > 0.0f) ||
        (IsSome(minSize.h) && minSize.h > 0.0f);

    TextAlign textAlign = style.textAlign;
    OptAlignContent alignContent = style.alignContent;

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

    Optf containerPercentageResolutionHeight =
        Or(Or(knownDimensions.h, MaybeMax(size.h, minSize.h)), minSize.h);

    RectF resolvedPadding = rawPadding
                                .ResolveOrZero(Some(containerOuterWidth), calc);
    RectF resolvedBorder = rawBorder
                               .ResolveOrZero(Some(containerOuterWidth), calc);
    RectF resolvedContentBoxInset =
        resolvedPadding + resolvedBorder + scrollbarGutter;

    InFlowResult inFlow = PerformFinalLayoutOnInFlowChildren(
        tree, runMode, &items, containerOuterWidth,
        containerPercentageResolutionHeight, contentBoxInset,
        resolvedContentBoxInset, resolvedBorder, textAlign, direction,
        ownMarginsCollapseWithChildren, blockCtx);
    SizeF inflowContentSize = inFlow.inflowContentSize;
    float intrinsicOuterHeight = inFlow.intrinsicOuterHeight;

    if (blockCtx->IsBfcRoot() || isScrollContainer) {
        intrinsicOuterHeight = F32Max(
            intrinsicOuterHeight, blockCtx->FloatedContentHeightContribution());
    }

    float containerOuterHeight = MaybeMax(
        UnwrapOr(knownDimensions.h,
                 MaybeClamp(intrinsicOuterHeight, minSize.h, maxSize.h)),
        Some(paddingBorderSize.h));
    SizeF finalOuterSize = {containerOuterWidth, containerOuterHeight};

    if (alignContent.IsSome()) {
        float containerInnerHeight =
            containerOuterHeight - resolvedContentBoxInset.VerticalAxisSum();
        float inflowContentHeight =
            intrinsicOuterHeight - resolvedContentBoxInset.VerticalAxisSum();
        float freeSpace = containerInnerHeight - inflowContentHeight;
        bool anyInFlow = false;
        for (int i = 0; i < len(items); i++) {
            if (items[i].hasFinalLayout) {
                anyInFlow = true;
                break;
            }
        }
        if (anyInFlow) {
            AlignContentKeyword keyword =
                ApplyAlignmentFallback(freeSpace, 1, alignContent.val);
            float groupOffset = ComputeAlignmentOffset(freeSpace, 1, 0.0f,
                                                       keyword, false, true);
            if (IsSome(inFlow.firstBaseline)) {
                inFlow.firstBaseline += groupOffset;
            }
            for (int i = 0; i < len(items); i++) {
                if (items[i].hasFinalLayout) {
                    items[i].finalLayout.location.y += groupOffset;
                }
            }
            inflowContentSize = SizeF::Zero();
            for (int i = 0; i < len(items); i++) {
                if (!items[i].hasFinalLayout) {
                    continue;
                }
                const Layout& l = items[i].finalLayout;
                inflowContentSize = Max(
                    inflowContentSize,
                    ComputeContentSizeContribution(
                        {IsRtl(direction)
                             ? containerOuterWidth - (l.location.x + l.size.w) -
                                   resolvedBorder.right
                             : l.location.x - resolvedBorder.left,
                         l.location.y - resolvedBorder.top},
                        l.size, l.contentSize, items[i].overflow));
            }
        }
    }

    bool allInFlowChildrenCanBeCollapsedThrough = true;
    for (int i = 0; i < len(items); i++) {
        if (IsFloated(items[i].floatMode)) {
            continue;
        }
        if (items[i].position != Position::Absolute &&
            !items[i].canBeCollapsedThrough) {
            allInFlowChildrenCanBeCollapsedThrough = false;
            break;
        }
    }
    bool canBeCollapsedThrough = !hasStylesPreventingBeingCollapsedThrough &&
                                 allInFlowChildrenCanBeCollapsedThrough;

    LayoutOutput output;
    output.size = finalOuterSize;
    output.firstBaselines.y = inFlow.firstBaseline;
    output.topMargin =
        ownMarginsCollapseWithChildren.start
            ? inFlow.firstChildTopMarginSet
            : CollapsibleMarginSet::FromMargin(UnwrapOr(
                  rawMargin.MaybeResolve(parentSize.w, calc).top, 0.0f));
    output.bottomMargin =
        ownMarginsCollapseWithChildren.end
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

    RectF absolutePositionInset = resolvedBorder + scrollbarGutter;
    SizeF absolutePositionArea = finalOuterSize - absolutePositionInset
                                                      .SumAxes();
    PointF absolutePositionOffset = {absolutePositionInset.left,
                                     absolutePositionInset.top};
    SizeF absoluteContentSize = PerformAbsoluteLayoutOnAbsoluteChildren(
        tree, items, absolutePositionArea, absolutePositionOffset, direction);

    inflowContentSize
        .w += IsRtl(direction) ? resolvedPadding.left : resolvedPadding.right;
    inflowContentSize.h += resolvedPadding.bottom;
    output.contentSize = Max(inflowContentSize, absoluteContentSize);

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

    if (blockCtx && !isScrollContainer) {
        return ComputeInner(tree, nodeId, next, blockCtx);
    }
    BlockFormattingContext rootBfc;
    BlockContext rootCtx;
    rootCtx.bfc = &rootBfc;
    rootCtx.isRoot = true;
    LayoutOutput out = ComputeInner(tree, nodeId, next, &rootCtx);
    VecReset(rootBfc.floatContext.leftFloats);
    VecReset(rootBfc.floatContext.rightFloats);
    VecReset(rootBfc.floatContext.segments);
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
    SizeFOpt minSize = SizeFOptNone();
    SizeFOpt maxSize = SizeFOptNone();
    Optf aspectRatio = None();
    AlignSelf alignSelf;

    PointOverflow overflow;
    float scrollbarWidth = 0.0f;
    float flexShrink = 0.0f;
    float flexGrow = 0.0f;

    float resolvedMinimumMainSize = 0.0f;

    RectFOpt inset = RectFOptNone();
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

    float offsetMain = 0.0f;
    float offsetCross = 0.0f;

    bool IsScroll() const {
        return IsScrollContainer(overflow.x) || IsScrollContainer(overflow.y);
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

    SizeFOpt minSize = SizeFOptNone();
    SizeFOpt maxSize = SizeFOptNone();
    RectF margin;
    RectF border;

    RectF contentBoxInset;
    PointF scrollbarGutter;
    SizeF gap;
    AlignItems alignItems;
    AlignContent alignContent;
    OptJustifyContent justifyContent;

    SizeFOpt nodeOuterSize = SizeFOptNone();
    SizeFOpt nodeInnerSize = SizeFOptNone();

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

AlgoConstants ComputeConstants(TaffyTree* tree, const Style& style,
                               SizeFOpt knownDimensions, SizeFOpt parentSize) {
    CalcResolver calc = tree->calc;
    AlgoConstants c;
    c.dir = style.flexDirection;
    c.isRow = IsRow(c.dir);
    c.isColumn = IsColumn(c.dir);
    c.isWrap = style.flexWrap == FlexWrap::Wrap ||
               style.flexWrap == FlexWrap::WrapReverse;
    c.isWrapReverse = style.flexWrap == FlexWrap::WrapReverse;

    Optf aspectRatio = style.aspectRatio;
    c.margin = style.margin.ResolveOrZero(parentSize.w, calc);
    RectF padding = style.padding.ResolveOrZero(parentSize.w, calc);
    c.border = style.border.ResolveOrZero(parentSize.w, calc);
    SizeF paddingBorderSum = padding.SumAxes() + c.border.SumAxes();
    SizeF boxSizingAdjustment = style.boxSizing == BoxSizing::ContentBox
                                    ? paddingBorderSum
                                    : SizeF::Zero();

    c.alignItems = style.alignItems
                       .UnwrapOr(AlignItems{AlignItemsKeyword::Stretch});
    c.alignContent = style.alignContent
                         .UnwrapOr(AlignContent{AlignContentKeyword::Stretch});
    c.justifyContent = style.justifyContent;
    c.layoutDirection = style.direction;

    PointOverflow t = style.overflow.Transpose();
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
        if (cs.position == Position::Absolute ||
            cs.BoxGenMode() == BoxGenerationMode::None) {
            continue;
        }

        Optf aspectRatio = cs.aspectRatio;
        RectF padding = cs.padding.ResolveOrZero(c.nodeInnerSize.w, calc);
        RectF border = cs.border.ResolveOrZero(c.nodeInnerSize.w, calc);
        SizeF pbSum = (padding + border).SumAxes();
        SizeF boxSizingAdjustment =
            cs.boxSizing == BoxSizing::ContentBox ? pbSum : SizeF::Zero();

        FlexItem item;
        item.node = child;
        item.order = (uint32_t)index;
        item.size = MaybeAdd(
            MaybeApplyAspectRatio(cs.size.MaybeResolve(c.nodeInnerSize, calc),
                                  aspectRatio),
            boxSizingAdjustment);
        item.minSize = MaybeAdd(cs.minSize.MaybeResolve(c.nodeInnerSize, calc),
                                boxSizingAdjustment);
        item.maxSize = MaybeAdd(cs.maxSize.MaybeResolve(c.nodeInnerSize, calc),
                                boxSizingAdjustment);
        item.aspectRatio = aspectRatio;

        item.inset = cs.inset.MaybeResolveZip(c.nodeInnerSize, calc);
        item.margin = cs.margin.ResolveOrZero(c.nodeInnerSize.w, calc);
        item.marginIsAuto = {cs.margin.left.IsAuto(), cs.margin.right.IsAuto(),
                             cs.margin.top.IsAuto(), cs.margin.bottom.IsAuto()};
        item.padding = padding;
        item.border = border;
        item.alignSelf =
            ResolveSelfRelative(cs.alignSelf.UnwrapOr(c.alignItems),
                                cs.direction, c.layoutDirection, c.isColumn);
        item.overflow = cs.overflow;
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
        out.width = AvailableSpace::Definite(
            knownDimensions.w - c.contentBoxInset.HorizontalAxisSum());
    } else {
        out.width = MaybeSub(
            MaybeSub(outerAvailableSpace.width, c.margin.HorizontalAxisSum()),
            c.contentBoxInset.HorizontalAxisSum());
    }
    if (IsSome(knownDimensions.h)) {
        out.height = AvailableSpace::Definite(
            knownDimensions.h - c.contentBoxInset.VerticalAxisSum());
    } else {
        out.height = MaybeSub(
            MaybeSub(outerAvailableSpace.height, c.margin.VerticalAxisSum()),
            c.contentBoxInset.VerticalAxisSum());
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

        float crossAxisMarginSum = CrossAxisSum(c.margin, dir);
        SizeFOpt transferredMinSize =
            MaybeApplyAspectRatio(child.minSize, child.aspectRatio);
        SizeFOpt transferredMaxSize =
            MaybeApplyAspectRatio(child.maxSize, child.aspectRatio);
        Optf childMinCross =
            MaybeAdd(Cross(transferredMinSize, dir), crossAxisMarginSum);
        Optf childMaxCross =
            MaybeAdd(Cross(transferredMaxSize, dir), crossAxisMarginSum);

        AvailableSpace crossAxisAvailableSpace;
        AvailableSpace crossIn = availableSpace.Cross(dir);
        switch (crossIn.kind) {
            case AvailableSpace::Kind::Definite:
                crossAxisAvailableSpace = AvailableSpace::Definite(
                    MaybeClamp(UnwrapOr(crossAxisParentSize, crossIn.value),
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
                     MaybeSub(crossAxisAvailableSpace.IntoOption(),
                              CrossAxisSum(child.margin, dir)));
        }

        Optf containerWidth = Main(c.nodeInnerSize, dir);
        float boxSizingAdjustment = 0.0f;
        if (cs.boxSizing == BoxSizing::ContentBox) {
            RectF padding = cs.padding.ResolveOrZero(containerWidth, calc);
            RectF border = cs.border.ResolveOrZero(containerWidth, calc);
            boxSizingAdjustment = Main((padding + border).SumAxes(), dir);
        }
        Optf flexBasis =
            MaybeAdd(cs.flexBasis.MaybeResolve(containerWidth, calc),
                     boxSizingAdjustment);

        Optf mainSize = Main(child.size, dir);
        Optf definiteBasis = Or(flexBasis, mainSize);
        if (IsSome(definiteBasis)) {
            child.flexBasis = definiteBasis;
        } else {

            SizeAvail childAvailableSpace = SizeAvail::MaxContent();
            childAvailableSpace
                .SetMain(dir, availableSpace.Main(dir).kind ==
                                      AvailableSpace::Kind::MinContent
                                  ? AvailableSpace::MinContent()
                                  : AvailableSpace::MaxContent());
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

    if (!c.isWrap) {
        VecAppend(*lines, {items, total, 0.0f, 0.0f});
        return;
    }

    AvailableSpace mainAxisAvailableSpace;
    Optf maxMain = Main(c.maxSize, c.dir);
    if (IsSome(maxMain)) {
        mainAxisAvailableSpace = AvailableSpace::Definite(
            MaybeMax(UnwrapOr(availableSpace.Main(c.dir).IntoOption(), maxMain),
                     Main(c.minSize, c.dir)));
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
            outerMainSize =
                lines->len > 1 ? F32Max(size, mainAvail.value) : size;
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
                        MaybeMax(Some(item.flexBasis), stylePreferred);
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
                    } else {
                        Optf crossAxisParentSize = Cross(c->nodeInnerSize, dir);
                        float crossAxisMarginSum = CrossAxisSum(c->margin, dir);
                        Optf childMinCross = MaybeAdd(Cross(item.minSize, dir),
                                                      crossAxisMarginSum);
                        Optf childMaxCross = MaybeAdd(Cross(item.maxSize, dir),
                                                      crossAxisMarginSum);
                        AvailableSpace crossAxisAvailableSpace =
                            availableSpace.Cross(dir);
                        if (crossAxisAvailableSpace
                                .kind == AvailableSpace::Kind::Definite) {
                            crossAxisAvailableSpace = AvailableSpace::Definite(
                                UnwrapOr(crossAxisParentSize,
                                         crossAxisAvailableSpace.value));
                        }
                        crossAxisAvailableSpace =
                            MaybeClamp(crossAxisAvailableSpace, childMinCross,
                                       childMaxCross);

                        SizeAvail childAvailableSpace = availableSpace;
                        childAvailableSpace
                            .SetCross(dir, crossAxisAvailableSpace);

                        SizeFOpt childKnownDimensions = item.size;
                        SetMain(&childKnownDimensions, dir, None());
                        if (item.alignSelf
                                    .keyword == AlignItemsKeyword::Stretch &&
                            !IsSome(Cross(childKnownDimensions, dir))) {
                            SetCross(
                                &childKnownDimensions, dir,
                                MaybeSub(crossAxisAvailableSpace.IntoOption(),
                                         CrossAxisSum(item.margin, dir)));
                        }

                        float contentMainSize =
                            tree->MeasureChildSize(
                                item.node, childKnownDimensions,
                                c->nodeInnerSize, childAvailableSpace,
                                SizingMode::InherentSize, MainAxis(dir),
                                LineBool::False()) +
                            MainAxisSum(item.margin, dir);

                        if (c->isRow) {
                            contentContribution =
                                MaybeClamp(contentMainSize, styleMin, styleMax);
                        } else {
                            contentContribution = MaybeClamp(
                                F32Max(contentMainSize, item.flexBasis),
                                styleMin, styleMax);
                        }
                    }

                    float diff = contentContribution - item.flexBasis;
                    if (diff > 0.0f) {
                        item.contentFlexFraction =
                            diff / F32Max(1.0f, item.flexGrow);
                    } else if (diff < 0.0f) {
                        float scaledShrinkFactor =
                            F32Max(1.0f, item.flexShrink * item.innerFlexBasis);
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
                        flexContribution = scaledShrinkFactor * flexFraction;
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

void DetermineHypotheticalCrossSize(TaffyTree* tree, FlexLine* line,
                                    const AlgoConstants& c,
                                    SizeAvail availableSpace) {
    FlexDirection dir = c.dir;
    for (int i = 0; i < line->count; i++) {
        FlexItem& child = line->items[i];
        float paddingBorderSum =
            CrossAxisSum(child.padding + child.border, dir);

        AvailableSpace childKnownMain =
            AvailableSpace::Definite(Main(c.containerSize, dir));

        Optf transferredMinCross =
            Cross(MaybeApplyAspectRatio(child.minSize, child.aspectRatio), dir);
        Optf transferredMaxCross =
            Cross(MaybeApplyAspectRatio(child.maxSize, child.aspectRatio), dir);

        Optf childCross =
            MaybeMax(MaybeClamp(Cross(child.size, dir), transferredMinCross,
                                transferredMaxCross),
                     paddingBorderSum);

        AvailableSpace childAvailableCross =
            MaybeMax(MaybeClamp(availableSpace.Cross(dir), transferredMinCross,
                                transferredMaxCross),
                     paddingBorderSum);

        float childInnerCross;
        if (IsSome(childCross)) {
            childInnerCross = childCross;
        } else {
            SizeFOpt known = {c.isRow ? Some(child.targetSize.w) : childCross,
                              c.isRow ? childCross : Some(child.targetSize.h)};
            SizeAvail avail = {c.isRow ? childKnownMain : childAvailableCross,
                               c.isRow ? childAvailableCross : childKnownMain};
            float measured = tree->MeasureChildSize(
                child.node, known, c.nodeInnerSize, avail,
                SizingMode::ContentSize, CrossAxis(dir), LineBool::False());
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
            if (line.items[i]
                    .alignSelf.keyword == AlignItemsKeyword::Baseline) {
                participating++;
            }
        }
        if (participating <= 1) {
            continue;
        }

        for (int i = 0; i < line.count; i++) {
            FlexItem& child = line.items[i];
            if (child.alignSelf.keyword != AlignItemsKeyword::Baseline) {
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
            LayoutOutput out = tree->PerformChildLayout(
                child.node, known, c.nodeInnerSize, avail,
                SizingMode::ContentSize, LineBool::False());
            float baseline = UnwrapOr(out.firstBaselines.y, out.size.h);
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
            if (child.alignSelf.keyword == AlignItemsKeyword::Baseline &&
                !child.marginIsAuto.CrossStart(dir) &&
                !child.marginIsAuto.CrossEnd(dir)) {
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
            float used;
            if (child.alignSelf.keyword == AlignItemsKeyword::Stretch &&
                !child.marginIsAuto.CrossStart(dir) &&
                !child.marginIsAuto.CrossEnd(dir) &&
                cs.size.Cross(dir).IsAuto()) {

                RectF padding = cs.padding.ResolveOrZero(c.nodeInnerSize, calc);
                RectF border = cs.border.ResolveOrZero(c.nodeInnerSize, calc);
                SizeF pbSum = (padding + border).SumAxes();
                SizeF boxSizingAdjustment =
                    cs.boxSizing == BoxSizing::ContentBox ? pbSum
                                                          : SizeF::Zero();
                SizeFOpt maxSizeIgnoringAspectRatio =
                    MaybeAdd(cs.maxSize.MaybeResolve(c.nodeInnerSize, calc),
                             boxSizingAdjustment);
                used =
                    MaybeClamp(lineCrossSize - CrossAxisSum(child.margin, dir),
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
        }

        int numItems = line.count;
        bool layoutReverse = IsReverse(dir);
        float gap = Main(c.gap, dir);
        JustifyContent rawMode =
            c.justifyContent
                .UnwrapOr(AlignContent{AlignContentKeyword::FlexStart});
        AlignContentKeyword mode =
            ApplyAlignmentFallback(freeSpace, numItems, rawMode);

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
            if (line.items[i]
                    .alignSelf.keyword == AlignItemsKeyword::Baseline) {
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
                       SizeF* totalContentSize, SizeF containerSize,
                       SizeFOpt nodeInnerSize, FlexDirection direction,
                       Direction layoutDirection) {
    LayoutOutput layoutOutput = tree->PerformChildLayout(
        item->node, AsOptional(item->targetSize), nodeInnerSize,
        SizeAvail::Definite(containerSize), SizingMode::ContentSize,
        LineBool::False());
    SizeF size = layoutOutput.size;
    SizeF contentSize = layoutOutput.contentSize;

    bool isRtlRow = IsRow(direction) && IsRtl(layoutDirection);
    bool isRtlColumn = IsColumn(direction) && IsRtl(layoutDirection);

    Optf negMainStart = MainStart(item->inset, direction);
    if (IsSome(negMainStart)) {
        negMainStart = -negMainStart;
    }
    Optf negMainEnd = MainEnd(item->inset, direction);
    if (IsSome(negMainEnd)) {
        negMainEnd = -negMainEnd;
    }
    float mainRelativeInset =
        isRtlRow
            ? UnwrapOr(Or(MainEnd(item->inset, direction), negMainStart), 0.0f)
            : UnwrapOr(Or(MainStart(item->inset, direction), negMainEnd), 0.0f);

    Optf negCrossEnd = CrossEnd(item->inset, direction);
    if (IsSome(negCrossEnd)) {
        negCrossEnd = -negCrossEnd;
    }
    float crossRelativeInset =
        isRtlColumn
            ? UnwrapOr(Or(negCrossEnd, CrossStart(item->inset, direction)),
                       0.0f)
            : UnwrapOr(Or(CrossStart(item->inset, direction), negCrossEnd),
                       0.0f);

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

    float innerBaseline = UnwrapOr(layoutOutput.firstBaselines.y, size.h);
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
    layout.contentSize = contentSize;
    layout.scrollbarSize = scrollbarSize;
    layout.location = location;
    layout.padding = item->padding;
    layout.border = item->border;
    layout.margin = item->margin;
    tree->SetUnroundedLayout(item->node, layout);

    float advance = item->offsetMain + MainAxisSum(item->margin, direction) +
                    Main(size, direction);
    if (isRtlRow) {
        *totalOffsetMain -= advance;
    } else {
        *totalOffsetMain += advance;
    }

    PointF contributionLocation =
        IsRtl(layoutDirection)
            ? PointF{containerSize.w - (location.x + size.w), location.y}
            : location;
    *totalContentSize =
        Max(*totalContentSize,
            ComputeContentSizeContribution(contributionLocation, size,
                                           contentSize, item->overflow));
}

void CalculateLayoutLine(TaffyTree* tree, FlexLine* line,
                         float* totalOffsetCross, SizeF* contentSize,
                         SizeF containerSize, SizeFOpt nodeInnerSize,
                         RectF paddingBorder, FlexDirection direction,
                         Direction layoutDirection) {
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
                          lineOffsetCross, contentSize, containerSize,
                          nodeInnerSize, direction, layoutDirection);
    }

    if (!isRtlColumn) {
        *totalOffsetCross += lineOffsetCross + line->crossSize;
    }
}

SizeF FinalLayoutPass(TaffyTree* tree, Vec<FlexLine>* lines,
                      const AlgoConstants& c) {
    float totalOffsetCross =
        (c.isColumn && IsRtl(c.layoutDirection))
            ? c.containerSize.w - CrossEnd(c.contentBoxInset, c.dir)
            : CrossStart(c.contentBoxInset, c.dir);

    SizeF contentSize = SizeF::Zero();

    for (int i = 0; i < lines->len; i++) {
        FlexLine& line =
            c.isWrapReverse ? (*lines)[lines->len - 1 - i] : (*lines)[i];
        CalculateLayoutLine(tree, &line, &totalOffsetCross, &contentSize,
                            c.containerSize, c.nodeInnerSize, c.contentBoxInset,
                            c.dir, c.layoutDirection);
    }

    contentSize.w +=
        IsRtl(c.layoutDirection)
            ? c.contentBoxInset.left - c.border.left - c.scrollbarGutter.x
            : c.contentBoxInset.right - c.border.right - c.scrollbarGutter.x;
    contentSize
        .h += c.contentBoxInset.bottom - c.border.bottom - c.scrollbarGutter.y;

    return contentSize;
}

SizeF PerformAbsoluteLayoutOnAbsoluteChildren(TaffyTree* tree, NodeId node,
                                              const AlgoConstants& c) {
    CalcResolver calc = tree->calc;
    float containerWidth = c.containerSize.w;
    float containerHeight = c.containerSize.h;
    SizeF insetRelativeSize =
        c.containerSize - c.border.SumAxes() - IntoSize(c.scrollbarGutter);

    SizeF contentSize = SizeF::Zero();

    int n = tree->ChildCount(node);
    for (int order = 0; order < n; order++) {
        NodeId child = tree->GetChildId(node, order);
        const Style& cs = tree->GetStyle(child);

        if (cs.BoxGenMode() == BoxGenerationMode::None ||
            cs.position != Position::Absolute) {
            continue;
        }

        PointOverflow overflow = cs.overflow;
        float scrollbarWidth = cs.scrollbarWidth;
        Optf aspectRatio = cs.aspectRatio;
        AlignSelf alignSelf =
            ResolveSelfRelative(cs.alignSelf.UnwrapOr(c.alignItems),
                                cs.direction, c.layoutDirection, c.isColumn);
        RectFOpt margin = cs.margin
                              .MaybeResolve(Some(insetRelativeSize.w), calc);
        RectF padding = cs.padding
                            .ResolveOrZero(Some(insetRelativeSize.w), calc);
        RectF border = cs.border.ResolveOrZero(Some(insetRelativeSize.w), calc);
        SizeF paddingBorderSum = (padding + border).SumAxes();
        SizeF boxSizingAdjustment = cs.boxSizing == BoxSizing::ContentBox
                                        ? paddingBorderSum
                                        : SizeF::Zero();

        RectFOpt inset =
            cs.inset.MaybeResolveZip(AsOptional(insetRelativeSize), calc);
        Optf left = inset.left;
        Optf right = inset.right;
        Optf top = inset.top;
        Optf bottom = inset.bottom;

        SizeFOpt styleSize = MaybeAdd(
            MaybeApplyAspectRatio(
                cs.size.MaybeResolve(AsOptional(insetRelativeSize), calc),
                aspectRatio),
            boxSizingAdjustment);
        SizeFOpt minSize =
            MaybeMax(Or(MaybeAdd(MaybeApplyAspectRatio(
                                     cs.minSize.MaybeResolve(
                                         AsOptional(insetRelativeSize), calc),
                                     aspectRatio),
                                 boxSizingAdjustment),
                        AsOptional(paddingBorderSum)),
                     paddingBorderSum);
        SizeFOpt maxSize = MaybeAdd(
            MaybeApplyAspectRatio(
                cs.maxSize.MaybeResolve(AsOptional(insetRelativeSize), calc),
                aspectRatio),
            boxSizingAdjustment);
        SizeFOpt knownDimensions = MaybeClamp(styleSize, minSize, maxSize);

        if (!IsSome(knownDimensions.w) && IsSome(left) && IsSome(right)) {
            float newWidthRaw =
                MaybeSub(MaybeSub(insetRelativeSize.w, margin.left),
                         margin.right) -
                left - right;
            knownDimensions.w = Some(F32Max(newWidthRaw, 0.0f));
            knownDimensions =
                MaybeClamp(MaybeApplyAspectRatio(knownDimensions, aspectRatio),
                           minSize, maxSize);
        }
        if (!IsSome(knownDimensions.h) && IsSome(top) && IsSome(bottom)) {
            float newHeightRaw =
                MaybeSub(MaybeSub(insetRelativeSize.h, margin.top),
                         margin.bottom) -
                top - bottom;
            knownDimensions.h = Some(F32Max(newHeightRaw, 0.0f));
            knownDimensions =
                MaybeClamp(MaybeApplyAspectRatio(knownDimensions, aspectRatio),
                           minSize, maxSize);
        }

        SizeAvail childAvail = {AvailableSpace::Definite(MaybeClamp(
                                    containerWidth, minSize.w, maxSize.w)),
                                AvailableSpace::Definite(MaybeClamp(
                                    containerHeight, minSize.h, maxSize.h))};

        SizeF measuredSize = tree->MeasureChildSizeBoth(
            child, knownDimensions, c.nodeInnerSize, childAvail,
            SizingMode::InherentSize, LineBool::False());
        SizeF finalSize = MaybeClamp(UnwrapOr(knownDimensions, measuredSize),
                                     minSize, maxSize);

        LayoutOutput layoutOutput = tree->PerformChildLayout(
            child, AsOptional(finalSize), c.nodeInnerSize, childAvail,
            SizingMode::InherentSize, LineBool::False());

        RectF nonAutoMargin = {
            UnwrapOr(margin.left, 0.0f), UnwrapOr(margin.right, 0.0f),
            UnwrapOr(margin.top, 0.0f), UnwrapOr(margin.bottom, 0.0f)};

        SizeF freeSpace = Max(SizeF{c.containerSize.w - finalSize.w -
                                        nonAutoMargin.HorizontalAxisSum(),
                                    c.containerSize.h - finalSize.h -
                                        nonAutoMargin.VerticalAxisSum()},
                              SizeF::Zero());

        int autoW =
            (IsSome(margin.left) ? 0 : 1) + (IsSome(margin.right) ? 0 : 1);
        int autoH =
            (IsSome(margin.top) ? 0 : 1) + (IsSome(margin.bottom) ? 0 : 1);
        SizeF autoMarginSize = {autoW > 0 && IsSome(left) && IsSome(right)
                                    ? freeSpace.w / (float)autoW
                                    : 0.0f,
                                autoH > 0 && IsSome(top) && IsSome(bottom)
                                    ? freeSpace.h / (float)autoH
                                    : 0.0f};
        RectF resolvedMargin = {UnwrapOr(margin.left, autoMarginSize.w),
                                UnwrapOr(margin.right, autoMarginSize.w),
                                UnwrapOr(margin.top, autoMarginSize.h),
                                UnwrapOr(margin.bottom, autoMarginSize.h)};

        Optf startMain = c.isRow ? left : top;
        Optf endMain = c.isRow ? right : bottom;
        Optf startCross = c.isRow ? top : left;
        Optf endCross = c.isRow ? bottom : right;
        bool mainIsRtl = c.isRow && IsRtl(c.layoutDirection);
        bool crossIsRtl = !c.isRow && IsRtl(c.layoutDirection);
        bool mainAxisFlexStartReversed = IsReverse(c.dir) != mainIsRtl;
        bool crossAxisFlexStartReversed = c.isWrapReverse != crossIsRtl;
        float mainStartScrollbarOffset =
            mainIsRtl ? Main(c.scrollbarGutter, c.dir) : 0.0f;
        float crossStartScrollbarOffset =
            crossIsRtl ? Cross(c.scrollbarGutter, c.dir) : 0.0f;
        float mainEndScrollbarOffset =
            mainIsRtl ? 0.0f : Main(c.scrollbarGutter, c.dir);
        float crossEndScrollbarOffset =
            crossIsRtl ? 0.0f : Cross(c.scrollbarGutter, c.dir);

        float alignedToMainEnd =
            Main(c.containerSize, c.dir) - MainEnd(c.border, c.dir) -
            mainEndScrollbarOffset - Main(finalSize, c.dir) -
            UnwrapOr(endMain, 0.0f) - MainEnd(resolvedMargin, c.dir);
        float offsetMain;
        if (IsSome(startMain) || IsSome(endMain)) {
            if (IsSome(startMain) && !(mainIsRtl && IsSome(endMain))) {
                offsetMain = startMain + MainStart(c.border, c.dir) +
                             mainStartScrollbarOffset +
                             MainStart(resolvedMargin, c.dir);
            } else {
                offsetMain = alignedToMainEnd;
            }
        } else {

            float startPos = MainStart(c.contentBoxInset, c.dir) +
                             MainStart(resolvedMargin, c.dir);
            float endPos = Main(c.containerSize, c.dir) -
                           MainEnd(c.contentBoxInset, c.dir) -
                           Main(finalSize, c.dir) -
                           MainEnd(resolvedMargin, c.dir);
            AlignContentKeyword jc =
                c.justifyContent
                    .UnwrapOr(AlignContent{AlignContentKeyword::FlexStart})
                    .Keyword();
            bool rev = mainAxisFlexStartReversed;
            bool startPosition = jc == AlignContentKeyword::Start ? !mainIsRtl
                                 : jc == AlignContentKeyword::End ? mainIsRtl
                                                                  : true;
            switch (jc) {
                case AlignContentKeyword::SpaceBetween:
                case AlignContentKeyword::Stretch:
                case AlignContentKeyword::FlexStart:
                    offsetMain = rev ? endPos : startPos;
                    break;
                case AlignContentKeyword::FlexEnd:
                    offsetMain = rev ? startPos : endPos;
                    break;
                case AlignContentKeyword::Start:
                case AlignContentKeyword::End:
                    offsetMain = startPosition ? startPos : endPos;
                    break;
                default:
                    offsetMain = (Main(c.containerSize, c.dir) +
                                  MainStart(c.contentBoxInset, c.dir) -
                                  MainEnd(c.contentBoxInset, c.dir) -
                                  Main(finalSize, c.dir) +
                                  MainStart(resolvedMargin, c.dir) -
                                  MainEnd(resolvedMargin, c.dir)) /
                                 2.0f;
                    break;
            }
        }

        float alignedToCrossEnd =
            Cross(c.containerSize, c.dir) - CrossEnd(c.border, c.dir) -
            crossEndScrollbarOffset - Cross(finalSize, c.dir) -
            UnwrapOr(endCross, 0.0f) - CrossEnd(resolvedMargin, c.dir);
        float offsetCross;
        if (IsSome(startCross) || IsSome(endCross)) {
            if (IsSome(startCross) && !(crossIsRtl && IsSome(endCross))) {
                offsetCross = startCross + CrossStart(c.border, c.dir) +
                              crossStartScrollbarOffset +
                              CrossStart(resolvedMargin, c.dir);
            } else {
                offsetCross = alignedToCrossEnd;
            }
        } else {
            bool crossOverflows =
                Cross(finalSize, c.dir) + CrossAxisSum(resolvedMargin, c.dir) >
                Cross(c.containerSize, c.dir) -
                    CrossAxisSum(c.contentBoxInset, c.dir);
            AlignItemsKeyword ck =
                ResolveSelfAlignmentSafety(alignSelf, crossOverflows);
            float startPos = CrossStart(c.contentBoxInset, c.dir) +
                             CrossStart(resolvedMargin, c.dir);
            float endPos = Cross(c.containerSize, c.dir) -
                           CrossEnd(c.contentBoxInset, c.dir) -
                           Cross(finalSize, c.dir) -
                           CrossEnd(resolvedMargin, c.dir);
            bool rev = crossAxisFlexStartReversed;
            bool startPosition = ck == AlignItemsKeyword::Start ||
                                         ck == AlignItemsKeyword::Baseline
                                     ? !crossIsRtl
                                 : ck == AlignItemsKeyword::End ? crossIsRtl
                                                                : true;
            switch (ck) {
                case AlignItemsKeyword::Start:
                case AlignItemsKeyword::End:
                case AlignItemsKeyword::Baseline:
                    offsetCross = startPosition ? startPos : endPos;
                    break;
                case AlignItemsKeyword::Center:
                    offsetCross = (Cross(c.containerSize, c.dir) +
                                   CrossStart(c.contentBoxInset, c.dir) -
                                   CrossEnd(c.contentBoxInset, c.dir) -
                                   Cross(finalSize, c.dir) +
                                   CrossStart(resolvedMargin, c.dir) -
                                   CrossEnd(resolvedMargin, c.dir)) /
                                  2.0f;
                    break;
                case AlignItemsKeyword::FlexEnd:
                    offsetCross = rev ? startPos : endPos;
                    break;
                default:

                    offsetCross = rev ? endPos : startPos;
                    break;
            }
        }

        PointF location = c.isRow ? PointF{offsetMain, offsetCross}
                                  : PointF{offsetCross, offsetMain};
        SizeF scrollbarSize = {
            overflow.y == Overflow::Scroll ? scrollbarWidth : 0.0f,
            overflow.x == Overflow::Scroll ? scrollbarWidth : 0.0f};

        Layout layout;
        layout.order = (uint32_t)order;
        layout.size = finalSize;
        layout.contentSize = layoutOutput.contentSize;
        layout.scrollbarSize = scrollbarSize;
        layout.location = location;
        layout.padding = padding;
        layout.border = border;
        layout.margin = resolvedMargin;
        tree->SetUnroundedLayout(child, layout);

        SizeF sizeContribution = {
            overflow.x == Overflow::Visible
                ? F32Max(finalSize.w, layoutOutput.contentSize.w)
                : finalSize.w,
            overflow.y == Overflow::Visible
                ? F32Max(finalSize.h, layoutOutput.contentSize.h)
                : finalSize.h};
        if (HasNonZeroArea(sizeContribution)) {
            PointF absoluteAreaOffset = {
                c.border.left +
                    (IsRtl(c.layoutDirection) ? c.scrollbarGutter.x : 0.0f),
                c.border.top};
            PointF relativeLocation = {location.x - absoluteAreaOffset.x,
                                       location.y - absoluteAreaOffset.y};
            SizeF contribution;
            if (IsRtl(c.layoutDirection)) {
                float overflowExtraWidth =
                    F32Max(sizeContribution.w - finalSize.w, 0.0f);
                contribution.w =
                    F32Max(insetRelativeSize.w - relativeLocation.x, 0.0f) +
                    overflowExtraWidth;
            } else {
                contribution.w = relativeLocation.x + sizeContribution.w;
            }
            contribution.h = relativeLocation.y + sizeContribution.h;
            contentSize = Max(contentSize, contribution);
        }
    }

    return contentSize;
}

LayoutOutput ComputePreliminary(TaffyTree* tree, NodeId node,
                                const LayoutInput& inputs) {
    SizeFOpt knownDimensions = inputs.knownDimensions;
    SizeFOpt parentSize = inputs.parentSize;
    RunMode runMode = inputs.runMode;

    AlgoConstants constants = ComputeConstants(tree, tree->GetStyle(node),
                                               knownDimensions, parentSize);

    Vec<FlexItem> flexItems;

    FlexLine lineBuf[2];
    Vec<FlexLine> flexLines;
    VecUseExternalBuffer(flexLines, lineBuf);

    GenerateAnonymousFlexItems(tree, node, constants, &flexItems);

    SizeAvail availableSpace = DetermineAvailableSpace(
        knownDimensions, inputs.availableSpace, constants);

    DetermineFlexBaseSize(tree, constants, availableSpace, flexItems.els,
                          len(flexItems));

    CollectFlexLines(constants, availableSpace, &flexItems, &flexLines);

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

    SizeF inflowContentSize = FinalLayoutPass(tree, &flexLines, constants);

    SizeF absoluteContentSize =
        PerformAbsoluteLayoutOnAbsoluteChildren(tree, node, constants);

    int nChildren = tree->ChildCount(node);
    for (int order = 0; order < nChildren; order++) {
        NodeId child = tree->GetChildId(node, order);
        if (tree->GetStyle(child).BoxGenMode() == BoxGenerationMode::None) {
            tree->SetUnroundedLayout(child, Layout::WithOrder((uint32_t)order));
            tree->PerformChildLayout(
                child, SizeFOptNone(), SizeFOptNone(), SizeAvail::MaxContent(),
                SizingMode::InherentSize, LineBool::False());
        }
    }

    Optf firstVerticalBaseline = None();
    int firstLineIdx = constants.isWrapReverse ? len(flexLines) - 1 : 0;
    if (firstLineIdx >= 0 && flexLines[firstLineIdx].count > 0) {
        FlexLine& firstLine = flexLines[firstLineIdx];
        const FlexItem* chosen = nullptr;
        for (int i = 0; i < firstLine.count; i++) {
            const FlexItem& item = firstLine.items[i];
            if (constants.isColumn ||
                item.alignSelf.keyword == AlignItemsKeyword::Baseline) {
                chosen = &item;
                break;
            }
        }
        if (!chosen) {
            chosen = &firstLine.items[0];
        }
        firstVerticalBaseline = Some(chosen->baseline);
    }

    VecReset(flexItems);
    VecReset(flexLines);

    return LayoutOutput::FromSizesAndBaselines(
        constants.containerSize, Max(inflowContentSize, absoluteContentSize),
        PointFOpt{None(), firstVerticalBaseline});
}

}

LayoutOutput ComputeFlexboxLayout(TaffyTree* tree, NodeId node,
                                  const LayoutInput& inputs) {
    CalcResolver calc = tree->calc;
    SizeFOpt knownDimensions = inputs.knownDimensions;
    SizeFOpt parentSize = inputs.parentSize;
    RunMode runMode = inputs.runMode;
    const Style& style = tree->GetStyle(node);

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
    return ComputePreliminary(tree, node, next);
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
    float FitContentLimit(Optf axisAvailableGridSpace) const {
        switch (maxTrackSizingFunction.raw.Tag()) {
            case CompactLength::kFitContentPxTag:
                return maxTrackSizingFunction.raw.Value();
            case CompactLength::kFitContentPercentTag:
                return IsSome(axisAvailableGridSpace)
                           ? axisAvailableGridSpace * maxTrackSizingFunction.raw
                                                          .Value()
                           : INFINITY;
            default:
                return INFINITY;
        }
    }
    float FitContentLimitedGrowthLimit(Optf axisAvailableGridSpace) const {
        return F32Min(growthLimit, FitContentLimit(axisAvailableGridSpace));
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

struct CellOccupancyMatrix {
    Vec<uint8_t> inner;
    int nRows = 0;
    int nCols = 0;
    TrackCounts columns;
    TrackCounts rows;

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
    }
    void Free() { VecReset(inner); }

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
                                            LineOzl secondarySpan,
                                            bool reversed) const {
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
                if (reversed) {
                    while (extent > 0) {
                        int r = primaryAxis == AbsoluteAxis::Horizontal
                                    ? secondary
                                    : extent - 1;
                        int c = primaryAxis == AbsoluteAxis::Horizontal
                                    ? extent - 1
                                    : secondary;
                        if (Get(r, c) == CellOccupancyState::Unoccupied) {
                            break;
                        }
                        extent--;
                    }
                    best = !found || extent < best ? extent : best;
                } else {
                    while (extent + 1 < primaryLen) {
                        int r = primaryAxis == AbsoluteAxis::Horizontal
                                    ? secondary
                                    : extent + 1;
                        int c = primaryAxis == AbsoluteAxis::Horizontal
                                    ? extent + 1
                                    : secondary;
                        if (Get(r, c) == CellOccupancyState::Unoccupied) {
                            break;
                        }
                        extent++;
                    }
                    best = !found || extent > best ? extent : best;
                }
                found = true;
            }
        }
        if (!found) {
            return OptOriginZeroLine();
        }
        OriginZeroLine line = pc.TrackToPrevOzLine((uint16_t)best);
        int32_t next = (int32_t)line.v + (reversed ? -1 : 1);
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

    OptOriginZeroLine LastOfType(AbsoluteAxis trackType, OriginZeroLine startAt,
                                 CellOccupancyState kind) const {
        const TrackCounts& tc = Counts(OtherAxis(trackType));
        int idx = tc.OzLineToNextTrack(startAt);
        if (trackType == AbsoluteAxis::Horizontal) {
            if (idx < 0 || idx >= nRows) {
                return OptOriginZeroLine();
            }
            for (int c = nCols - 1; c >= 0; c--) {
                if (Get(idx, c) == kind) {
                    return OptOriginZeroLine(tc.TrackToPrevOzLine((uint16_t)c));
                }
            }
        } else {
            if (idx < 0 || idx >= nCols) {
                return OptOriginZeroLine();
            }
            for (int r = nRows - 1; r >= 0; r--) {
                if (Get(r, idx) == kind) {
                    return OptOriginZeroLine(tc.TrackToPrevOzLine((uint16_t)r));
                }
            }
        }
        return OptOriginZeroLine();
    }

    OptOriginZeroLine FirstOfType(AbsoluteAxis trackType,
                                  OriginZeroLine startAt,
                                  CellOccupancyState kind) const {
        const TrackCounts& tc = Counts(OtherAxis(trackType));
        int idx = tc.OzLineToNextTrack(startAt);
        if (trackType == AbsoluteAxis::Horizontal) {
            if (idx < 0 || idx >= nRows) {
                return OptOriginZeroLine();
            }
            for (int c = 0; c < nCols; c++) {
                if (Get(idx, c) == kind) {
                    return OptOriginZeroLine(tc.TrackToPrevOzLine((uint16_t)c));
                }
            }
        } else {
            if (idx < 0 || idx >= nCols) {
                return OptOriginZeroLine();
            }
            for (int r = 0; r < nRows; r++) {
                if (Get(r, idx) == kind) {
                    return OptOriginZeroLine(tc.TrackToPrevOzLine((uint16_t)r));
                }
            }
        }
        return OptOriginZeroLine();
    }
};

struct GridItem {
    NodeId node;

    uint16_t sourceOrder = 0;

    LineOzl row;
    LineOzl column;

    bool isCompressibleReplaced = false;
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

enum class NameSuffix : uint8_t {
    None,
    Start,
    End
};

struct LineNameEntry {
    Str name;
    NameSuffix suffix = NameSuffix::None;
    Vec<uint32_t> lines;
};

struct NamedLineResolver {
    Vec<LineNameEntry> rowLines;
    Vec<LineNameEntry> columnLines;
    Slice<GridTemplateArea> areas;
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
    }

    static void Upsert(Vec<LineNameEntry>* map, Str name, NameSuffix suffix,
                       uint32_t value) {
        for (int i = 0; i < map->len; i++) {
            LineNameEntry& e = (*map)[i];
            if (e.suffix == suffix && base::StrEq(e.name, name)) {
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
        e.suffix = suffix;
        VecAppend(e.lines, value);
        VecAppend(*map, e);
    }

    static const Vec<uint32_t>* Find(const Vec<LineNameEntry>& map, Str name,
                                     NameSuffix suffix) {
        for (int i = 0; i < len(map); i++) {
            const LineNameEntry& e = map[i];
            if (e.suffix == suffix && base::StrEq(e.name, name)) {
                return &e.lines;
            }
        }
        return nullptr;
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

    GridLine FindLineIndex(Str name, int32_t idx, GridAreaAxis axis,
                           GridAreaEnd end, int filterFrom, int filterTo) const;
};

void NamedLineResolver::Init(const Style& style, uint16_t columnAutoRepetitions,
                             uint16_t rowAutoRepetitions) {
    areas = style.gridTemplateAreas.areas;
    areaColumnCount = style.gridTemplateAreas.columnCount;
    areaRowCount = style.gridTemplateAreas.rowCount;
    for (int i = 0; i < areas.len; i++) {
        const GridTemplateArea& area = areas[i];
        Upsert(&columnLines, area.name, NameSuffix::Start, area.columnStart);
        Upsert(&columnLines, area.name, NameSuffix::End, area.columnEnd);
        Upsert(&rowLines, area.name, NameSuffix::Start, area.rowStart);
        Upsert(&rowLines, area.name, NameSuffix::End, area.rowEnd);
    }

    struct Axis {
        Slice<GridTemplateComponent> tracks;
        Slice<LineNameSet> names;
        uint16_t autoRepetitions;
        Vec<LineNameEntry>* map;
    };
    Axis axes[2] = {{style.gridTemplateColumns, style.gridTemplateColumnNames,
                     columnAutoRepetitions, &columnLines},
                    {style.gridTemplateRows, style.gridTemplateRowNames,
                     rowAutoRepetitions, &rowLines}};

    for (const Axis& ax : axes) {
        uint32_t currentLine = 0;
        int trackIdx = 0;
        for (int i = 0; i < ax.names.len; i++) {
            currentLine += 1;
            const LineNameSet& set = ax.names[i];
            for (int k = 0; k < set.names.len; k++) {
                Upsert(ax.map, set.names[k], NameSuffix::None, currentLine);
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
            for (uint16_t r = 0; r < repeatCount; r++) {
                for (int s = 0; s < comp.repeat.lineNames.len; s++) {
                    const LineNameSet& ls = comp.repeat.lineNames[s];
                    for (int k = 0; k < ls.names.len; k++) {
                        Upsert(ax.map, ls.names[k], NameSuffix::None,
                               currentLine);
                    }
                    currentLine += 1;
                }

                currentLine -= 1;
            }
            if (repeatCount > 0) {
                currentLine -= 1;
            }
        }
    }

}

GridLine NamedLineResolver::FindLineIndex(Str name, int32_t idx,
                                          GridAreaAxis axis, GridAreaEnd end,
                                          int filterFrom, int filterTo) const {
    int32_t explicitTrackCount =
        axis == GridAreaAxis::Row ? explicitRowCount : explicitColumnCount;
    auto gridLine = [](int64_t value) {
        value = value < INT16_MIN   ? INT16_MIN
                : value > INT16_MAX ? INT16_MAX
                                    : value;
        return GridLine{(int16_t)value};
    };

    if (idx == 0) {
        idx = 1;
    }

    const Vec<LineNameEntry>& lookup =
        axis == GridAreaAxis::Row ? rowLines : columnLines;
    const Vec<uint32_t>* lines = Find(lookup, name, NameSuffix::None);
    if (!lines) {
        lines = Find(
            lookup, name,
            end == GridAreaEnd::Start ? NameSuffix::Start : NameSuffix::End);
    }

    if (lines) {
        int from = filterFrom < 0 ? 0 : filterFrom;
        int to = filterTo < 0 || filterTo > lines->len ? lines->len : filterTo;
        int count = to - from;
        if (count < 0) {
            count = 0;
        }
        uint32_t absIdx = idx < 0 ? (uint32_t)(-(int64_t)idx) : (uint32_t)idx;
        if (absIdx <= (uint32_t)count) {
            if (idx > 0) {
                return gridLine((*lines)[from + (int)absIdx - 1]);
            }
            return gridLine((*lines)[from + count - (int)absIdx]);
        }
        int64_t remaining =
            (int64_t)(absIdx - (uint32_t)count) * (idx > 0 ? 1 : -1);
        if (idx > 0) {
            return gridLine((int64_t)explicitTrackCount + 1 + remaining);
        }
        return gridLine(-((int64_t)explicitTrackCount + 1 + remaining));
    }

    if (idx > 0) {
        return gridLine((int64_t)explicitTrackCount + 1 + idx);
    }
    return gridLine(-((int64_t)explicitTrackCount + 1 + idx));
}

int PartitionPoint(const Vec<uint32_t>* lines, uint32_t bound, bool inclusive) {
    if (!lines) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < lines->len; i++) {
        bool keep = inclusive ? ((*lines)[i] <= bound) : ((*lines)[i] < bound);
        if (!keep) {
            break;
        }
        n++;
    }
    return n;
}

LinePlain NamedLineResolver::ResolveLineNames(LinePlacement line,
                                              GridAreaAxis axis) const {
    GridPlacement start = line.start;
    GridPlacement end = line.end;

    if (start.kind == GridPlacementKind::NamedLine) {
        start = GridPlacement::FromLineIndex(
            FindLineIndex(start.name, start.line, axis, GridAreaEnd::Start, -1,
                          -1)
                .v);
    }
    if (end.kind == GridPlacementKind::NamedLine) {
        end = GridPlacement::FromLineIndex(
            FindLineIndex(end.name, end.line, axis, GridAreaEnd::End, -1, -1)
                .v);
    }

    int16_t explicitTrackCount = axis == GridAreaAxis::Row
                                     ? (int16_t)explicitRowCount
                                     : (int16_t)explicitColumnCount;
    const Vec<LineNameEntry>& lookup =
        axis == GridAreaAxis::Row ? rowLines : columnLines;

    if (start.kind == GridPlacementKind::Line &&
        end.kind == GridPlacementKind::NamedSpan) {
        int16_t normalizedStart =
            start.line > 0
                ? start.line
                : (int16_t)F32Max((float)(explicitTrackCount + 1 + start.line),
                                  0.0f);
        const Vec<uint32_t>* lines = Find(lookup, end.name, NameSuffix::None);
        int point = PartitionPoint(lines, (uint32_t)normalizedStart, true);
        GridLine endLine = FindLineIndex(end.name, (int32_t)end.span, axis,
                                         GridAreaEnd::End, point, -1);
        return {PlainPlacement::AtLine(start.line),
                PlainPlacement::AtLine(endLine.v)};
    }
    if (start.kind == GridPlacementKind::NamedSpan &&
        end.kind == GridPlacementKind::Line) {
        int16_t normalizedEnd =
            end.line > 0
                ? end.line
                : (int16_t)F32Max((float)(explicitTrackCount + 1 + end.line),
                                  0.0f);
        const Vec<uint32_t>* lines = Find(lookup, start.name, NameSuffix::None);
        int point = PartitionPoint(lines, (uint32_t)normalizedEnd, false);
        GridLine startLine = FindLineIndex(start.name, (int32_t)start.span,
                                           axis, GridAreaEnd::Start, 0, point);
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

MinMaxSpan ChildMinLineMaxLineSpan(LinePlacement line,
                                   uint16_t explicitTrackCount) {

    LinePlain oz = line.IntoOriginZeroIgnoringNamed(explicitTrackCount);
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
        out.minLine = t2;
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

struct ChildPlacementStyles {
    LinePlacement column;
    LinePlacement row;
};

void ComputeGridSizeEstimate(uint16_t explicitColCount,
                             uint16_t explicitRowCount, Direction direction,
                             const ChildPlacementStyles* children, int n,
                             TrackCounts* outCols, TrackCounts* outRows) {
    OriginZeroLine colMin{0};
    OriginZeroLine colMax{0};
    uint16_t colMaxSpan = 0;
    OriginZeroLine rowMin{0};
    OriginZeroLine rowMax{0};
    uint16_t rowMaxSpan = 0;

    for (int i = 0; i < n; i++) {
        MinMaxSpan colEst =
            ChildMinLineMaxLineSpan(children[i].column, explicitColCount);
        MinMaxSpan rowEst =
            ChildMinLineMaxLineSpan(children[i].row, explicitRowCount);

        if (IsRtl(direction) && (colEst.minLine != OriginZeroLine{0} ||
                                 colEst.maxLine != OriginZeroLine{0})) {
            int16_t endLine = (int16_t)explicitColCount;
            OriginZeroLine mirroredMin{(int16_t)(endLine - colEst.maxLine.v)};
            OriginZeroLine mirroredMax{(int16_t)(endLine - colEst.minLine.v)};
            colEst.minLine = mirroredMin;
            colEst.maxLine = mirroredMax;
        }
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
    SizeFOpt maxSize = MaybeAdd(
        MaybeApplyAspectRatio(item.maxSize.MaybeResolve(gridAreaSize, calc),
                              aspectRatio),
        boxSizingAdjustment);

    SizeFOpt gridAreaMinusItemMargins = MaybeSub(gridAreaSize, margins);

    Optf width = inherentSize.w;
    if (!IsSome(width) && !item.margin.left.IsAuto() &&
        !item.margin.right.IsAuto() &&
        item.justifySelf.keyword == AlignItemsKeyword::Stretch) {
        width = gridAreaMinusItemMargins.w;
    }
    SizeFOpt sized =
        MaybeApplyAspectRatio(SizeFOpt{width, inherentSize.h}, aspectRatio);

    Optf height = sized.h;
    if (!IsSome(height) && !item.margin.top.IsAuto() &&
        !item.margin.bottom.IsAuto() &&
        item.alignSelf.keyword == AlignItemsKeyword::Stretch) {
        height = gridAreaMinusItemMargins.h;
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

float ItemMinContentContribution(const GridItem& item, AbstractAxis axis,
                                 TaffyTree* tree, SizeFOpt gridAreaSize,
                                 SizeFOpt availableSpace) {
    SizeFOpt knownDimensions = ItemKnownDimensions(item, tree, gridAreaSize);

    SizeAvail avail = {
        IsSome(availableSpace.w) ? AvailableSpace::Definite(availableSpace.w)
                                 : AvailableSpace::MinContent(),
        IsSome(availableSpace.h) ? AvailableSpace::Definite(availableSpace.h)
                                 : AvailableSpace::MinContent()};
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
    SizeAvail avail = {
        IsSome(availableSpace.w) ? AvailableSpace::Definite(availableSpace.w)
                                 : AvailableSpace::MaxContent(),
        IsSome(availableSpace.h) ? AvailableSpace::Definite(availableSpace.h)
                                 : AvailableSpace::MaxContent()};
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
    if (!IsSome(size)) {
        size = Get(MaybeAdd(MaybeApplyAspectRatio(
                                item->minSize.MaybeResolve(gridAreaSize, calc),
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

        bool spansAutoMinTrack = false;
        bool spansAFlexibleTrack = false;
        for (int i = 0; i < axisTrackCount; i++) {
            if (axisTracks[i].minTrackSizingFunction.IsAuto()) {
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
            size = Some(minimumContribution);
        } else {
            size = Some(0.0f);
        }
    }

    Optf limit = SpannedFixedTrackLimit(*item, axis, axisTracks,
                                        Get(innerNodeSize, axis), calc);
    return MaybeMin(size, limit);
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

bool AxisIsReversed(Direction direction, AbsoluteAxis axis) {
    return IsRtl(direction) && axis == AbsoluteAxis::Horizontal;
}

OriginZeroLine AdvancePosition(OriginZeroLine position, bool reversed) {
    int32_t value = (int32_t)position.v + (reversed ? -1 : 1);
    value = value < INT16_MIN   ? INT16_MIN
            : value > INT16_MAX ? INT16_MAX
                                : value;
    return OriginZeroLine{(int16_t)value};
}

OriginZeroLine SearchStartLine(OriginZeroLine gridStartLine,
                               OriginZeroLine gridEndLine, bool reversed) {
    return reversed ? gridEndLine - (uint16_t)1 : gridStartLine;
}

LineOzl ResolveIndefiniteGridSpan(OriginZeroLine position, uint16_t span,
                                  bool reversed) {
    auto line = [](int32_t value) {
        value = value < INT16_MIN   ? INT16_MIN
                : value > INT16_MAX ? INT16_MAX
                                    : value;
        return OriginZeroLine{(int16_t)value};
    };
    if (reversed) {
        return {line((int32_t)position.v - span + 1),
                line((int32_t)position.v + 1)};
    }
    return {position, line((int32_t)position.v + span)};
}

LineOzl ClampSpanToLimitedGrid(LineOzl span, int16_t minLine, int16_t maxLine) {
    int32_t start = span.start.v;
    start = start<minLine ? minLine : start>(int32_t) maxLine - 1
                ? (int32_t)maxLine - 1
                : start;
    int32_t end = span.end.v;
    end = end < start + 1 ? start + 1 : end > maxLine ? maxLine : end;
    return {OriginZeroLine{(int16_t)start}, OriginZeroLine{(int16_t)end}};
}

LineOzl MirrorHorizontalSpan(LineOzl span, uint16_t explicitColCount) {
    int16_t endLine = (int16_t)explicitColCount;
    return {OriginZeroLine{(int16_t)(endLine - span.end.v)},
            OriginZeroLine{(int16_t)(endLine - span.start.v)}};
}

LineOzl MaybeMirrorSpan(LineOzl span, AbsoluteAxis axis, Direction direction,
                        uint16_t explicitColCount) {
    if (axis == AbsoluteAxis::Horizontal && IsRtl(direction)) {
        return MirrorHorizontalSpan(
            ClampSpanToLimitedGrid(span, kMinOzLine, kMaxOzLine),
            explicitColCount);
    }
    return span;
}

LineOzl ClampSpanForAxis(LineOzl span, AbsoluteAxis axis, Direction direction,
                         uint16_t explicitColCount) {
    if (axis == AbsoluteAxis::Horizontal && IsRtl(direction)) {
        int16_t explicitEnd = (int16_t)explicitColCount;
        return ClampSpanToLimitedGrid(span, (int16_t)(explicitEnd - kMaxOzLine),
                                      (int16_t)(explicitEnd - kMinOzLine));
    }
    return ClampSpanToLimitedGrid(span, kMinOzLine, kMaxOzLine);
}

struct PlacementChild {
    int index = 0;
    NodeId node;
    LinePlain horizontal;
    LinePlain vertical;

    LinePlain Get(AbsoluteAxis a) const {
        return a == AbsoluteAxis::Horizontal ? horizontal : vertical;
    }
};

void RecordGridPlacement(CellOccupancyMatrix* matrix, Vec<GridItem>* items,
                         TaffyTree* tree, NodeId node, int index,
                         AlignItems parentAlignItems,
                         AlignItems parentJustifyItems,
                         AbsoluteAxis primaryAxis, Direction direction,
                         uint16_t explicitColCount, LineOzl primarySpan,
                         LineOzl secondarySpan,
                         CellOccupancyState placementType) {
    primarySpan =
        ClampSpanForAxis(primarySpan, primaryAxis, direction, explicitColCount);
    secondarySpan = ClampSpanForAxis(secondarySpan, OtherAxis(primaryAxis),
                                     direction, explicitColCount);
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
                                  AbsoluteAxis axis, Direction direction,
                                  uint16_t explicitColCount) {
    return MaybeMirrorSpan(child.Get(axis).ResolveDefiniteGridLines(), axis,
                           direction, explicitColCount);
}

struct SpanPair {
    LineOzl primary;
    LineOzl secondary;
};

SpanPair PlaceDefiniteSecondaryAxisItem(const CellOccupancyMatrix& matrix,
                                        const PlacementChild& child,
                                        GridAutoFlow autoFlow,
                                        Direction direction,
                                        uint16_t explicitColCount) {
    AbsoluteAxis primaryAxis = PrimaryAxis(autoFlow);
    AbsoluteAxis secondaryAxis = OtherAxis(primaryAxis);
    bool primaryReversed = AxisIsReversed(direction, primaryAxis);
    OriginZeroLine primaryStart = matrix.Counts(primaryAxis)
                                      .ImplicitStartLine();
    OriginZeroLine primaryEnd = matrix.Counts(primaryAxis).ImplicitEndLine();

    LineOzl secondaryPlacement =
        MaybeMirrorSpan(child.Get(secondaryAxis).ResolveDefiniteGridLines(),
                        secondaryAxis, direction, explicitColCount);

    OriginZeroLine startingPosition;
    if (IsDense(autoFlow)) {
        startingPosition =
            SearchStartLine(primaryStart, primaryEnd, primaryReversed);
    } else {
        OptOriginZeroLine lookup =
            primaryReversed
                ? matrix.FirstOfType(primaryAxis, secondaryPlacement.start,
                                     CellOccupancyState::AutoPlaced)
                : matrix.LastOfType(primaryAxis, secondaryPlacement.start,
                                    CellOccupancyState::AutoPlaced);
        startingPosition =
            lookup.IsSome()
                ? lookup.val
                : SearchStartLine(primaryStart, primaryEnd, primaryReversed);
    }
    uint16_t primarySpanLen = child.Get(primaryAxis).IndefiniteSpan();

    OriginZeroLine position = startingPosition;
    while (true) {
        LineOzl primaryPlacement = ResolveIndefiniteGridSpan(
            position, primarySpanLen, primaryReversed);
        OptOriginZeroLine collision = matrix.LineAreaCollisionJump(
            primaryAxis, primaryPlacement, secondaryPlacement, primaryReversed);
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
                                         OriginZeroLine gridSecondary,
                                         Direction direction,
                                         uint16_t explicitColCount) {
    AbsoluteAxis primaryAxis = PrimaryAxis(autoFlow);
    AbsoluteAxis secondaryAxis = OtherAxis(primaryAxis);
    bool primaryReversed = AxisIsReversed(direction, primaryAxis);
    bool secondaryReversed = AxisIsReversed(direction, secondaryAxis);

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
    OriginZeroLine secondaryGridEnd = matrix.Counts(secondaryAxis)
                                          .ImplicitEndLine();
    OriginZeroLine primaryStartPosition =
        SearchStartLine(primaryGridStart, primaryGridEnd, primaryReversed);
    OriginZeroLine secondaryStartPosition = SearchStartLine(
        secondaryGridStart, secondaryGridEnd, secondaryReversed);

    OriginZeroLine primaryIdx = gridPrimary;
    OriginZeroLine secondaryIdx = gridSecondary;

    if (hasDefinitePrimary) {
        LineOzl primarySpan =
            MaybeMirrorSpan(primaryStyle.ResolveDefiniteGridLines(),
                            primaryAxis, direction, explicitColCount);
        if (IsDense(autoFlow)) {
            secondaryIdx = secondaryStartPosition;
        } else {
            bool shouldAdvance = primaryReversed
                                     ? primarySpan.start > primaryIdx
                                     : primarySpan.start < primaryIdx;
            if (shouldAdvance) {
                secondaryIdx = AdvancePosition(secondaryIdx, secondaryReversed);
            }
        }

        while (true) {
            LineOzl secondarySpan = ResolveIndefiniteGridSpan(
                secondaryIdx, secondarySpanLen, secondaryReversed);
            OptOriginZeroLine collision = matrix.LineAreaCollisionJump(
                secondaryAxis, secondarySpan, primarySpan, secondaryReversed);
            if (!collision.IsSome()) {
                return {primarySpan, secondarySpan};
            }
            secondaryIdx = collision.val;
        }
    }

    uint16_t primarySpanLen = primaryStyle.IndefiniteSpan();

    while (true) {
        LineOzl primarySpan = ResolveIndefiniteGridSpan(
            primaryIdx, primarySpanLen, primaryReversed);
        LineOzl secondarySpan = ResolveIndefiniteGridSpan(
            secondaryIdx, secondarySpanLen, secondaryReversed);

        bool primaryOutOfBounds = primaryReversed
                                      ? primarySpan.start < primaryGridStart
                                      : primarySpan.end > primaryGridEnd;
        if (primaryOutOfBounds) {
            secondaryIdx = AdvancePosition(secondaryIdx, secondaryReversed);
            primaryIdx = primaryStartPosition;
            continue;
        }
        OptOriginZeroLine collision = matrix.LineAreaCollisionJump(
            primaryAxis, primarySpan, secondarySpan, primaryReversed);
        if (collision.IsSome()) {
            primaryIdx = collision.val;
            continue;
        }
        return {primarySpan, secondarySpan};
    }
}

void PlaceGridItems(CellOccupancyMatrix* matrix, Vec<GridItem>* items,
                    TaffyTree* tree, const Vec<PlacementChild>& children,
                    Direction direction, GridAutoFlow gridAutoFlow,
                    AlignItems alignItems, AlignItems justifyItems) {
    AbsoluteAxis primaryAxis = PrimaryAxis(gridAutoFlow);
    AbsoluteAxis secondaryAxis = OtherAxis(primaryAxis);
    uint16_t explicitColCount = matrix->Counts(AbsoluteAxis::Horizontal)
                                    .explicitCount;

    for (int i = 0; i < len(children); i++) {
        const PlacementChild& c = children[i];
        if (!c.horizontal.IsDefinite() || !c.vertical.IsDefinite()) {
            continue;
        }
        LineOzl primarySpan = PlaceDefiniteGridItemAxis(
            c, primaryAxis, direction, explicitColCount);
        LineOzl secondarySpan = PlaceDefiniteGridItemAxis(
            c, secondaryAxis, direction, explicitColCount);
        RecordGridPlacement(matrix, items, tree, c.node, c.index, alignItems,
                            justifyItems, primaryAxis, direction,
                            explicitColCount, primarySpan, secondarySpan,
                            CellOccupancyState::DefinitelyPlaced);
    }

    for (int i = 0; i < len(children); i++) {
        const PlacementChild& c = children[i];
        if (!c.Get(secondaryAxis).IsDefinite() || c.Get(primaryAxis)
                                                      .IsDefinite()) {
            continue;
        }
        SpanPair spans = PlaceDefiniteSecondaryAxisItem(
            *matrix, c, gridAutoFlow, direction, explicitColCount);
        RecordGridPlacement(matrix, items, tree, c.node, c.index, alignItems,
                            justifyItems, primaryAxis, direction,
                            explicitColCount, spans.primary, spans.secondary,
                            CellOccupancyState::AutoPlaced);
    }

    bool primaryReversed = AxisIsReversed(direction, primaryAxis);
    OriginZeroLine startPrimary = SearchStartLine(
        matrix->Counts(primaryAxis).ImplicitStartLine(),
        matrix->Counts(primaryAxis).ImplicitEndLine(), primaryReversed);
    OriginZeroLine startSecondary =
        SearchStartLine(matrix->Counts(secondaryAxis).ImplicitStartLine(),
                        matrix->Counts(secondaryAxis).ImplicitEndLine(),
                        AxisIsReversed(direction, secondaryAxis));
    OriginZeroLine posPrimary = startPrimary;
    OriginZeroLine posSecondary = startSecondary;

    for (int i = 0; i < len(children); i++) {
        const PlacementChild& c = children[i];
        if (c.Get(secondaryAxis).IsDefinite()) {
            continue;
        }
        SpanPair spans = PlaceIndefinitelyPositionedItem(
            *matrix, c, gridAutoFlow, posPrimary, posSecondary, direction,
            explicitColCount);
        RecordGridPlacement(matrix, items, tree, c.node, c.index, alignItems,
                            justifyItems, primaryAxis, direction,
                            explicitColCount, spans.primary, spans.secondary,
                            CellOccupancyState::AutoPlaced);

        if (IsDense(gridAutoFlow)) {
            posPrimary = startPrimary;
            posSecondary = startSecondary;
        } else {
            posPrimary =
                primaryReversed ? spans.primary.start : spans.primary.end;
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
            if (items[i].alignSelf.keyword == AlignItemsKeyword::Baseline) {
                baselineCount++;
            }
        }
        if (baselineCount <= 1) {
            start = end;
            continue;
        }

        for (int i = start; i < end; i++) {
            GridItem& item = items[i];
            LayoutOutput out = tree->PerformChildLayout(
                item.node, SizeFOptNone(), innerNodeSize,
                SizeAvail::MinContent(), SizingMode::InherentSize,
                LineBool::False());
            RectLpa topOnly = {LengthPercentageAuto::Zero(),
                               LengthPercentageAuto::Zero(), item.margin.top,
                               LengthPercentageAuto::Zero()};
            float marginTop = topOnly.ResolveOrZero(innerNodeSize.w, tree->calc)
                                  .top;
            item.baseline =
                Some(UnwrapOr(out.firstBaselines.y, out.size.h) + marginTop);
        }

        float rowMaxBaseline = 0.0f;
        for (int i = start; i < end; i++) {
            rowMaxBaseline =
                F32Max(rowMaxBaseline, UnwrapOr(items[i].baseline, 0.0f));
        }
        for (int i = start; i < end; i++) {
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

    while (spaceToDistribute > kThreshold) {
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
    IntrinsicContributionType intrinsicContributionType,
    Optf axisInnerNodeSize) {
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
            getBaseSize, [&](const GridTrack& t) {
                return t.FitContentLimit(axisInnerNodeSize);
            });
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
    IntrinsicContributionType intrinsicContributionType,
    Optf axisInnerNodeSize) {
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
            DistributeItemSpaceToBaseSizeInner(
                space, tracks, n, filter, flexFactor, trackLimit,
                intrinsicContributionType, axisInnerNodeSize);
        } else {
            DistributeItemSpaceToBaseSizeInner(
                space, tracks, n, filter, one, trackLimit,
                intrinsicContributionType, axisInnerNodeSize);
        }
        return;
    }
    DistributeItemSpaceToBaseSizeInner(space, tracks, n, trackIsAffected, one,
                                       trackLimit, intrinsicContributionType,
                                       axisInnerNodeSize);
}

template <typename Affected>
void DistributeItemSpaceToGrowthLimit(float space, GridTrack* tracks, int n,
                                      Affected trackIsAffected,
                                      Optf axisInnerNodeSize) {
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

    auto growable = [&](const GridTrack& t) {
        return trackIsAffected(t) &&
               (t.infinitelyGrowable ||
                t.FitContentLimitedGrowthLimit(axisInnerNodeSize) == INFINITY);
    };
    int growableCount = 0;
    for (int i = 0; i < n; i++) {
        if (growable(tracks[i])) {
            growableCount++;
        }
    }
    if (growableCount > 0) {
        float increase = extraSpace / (float)growableCount;
        for (int i = 0; i < n; i++) {
            if (growable(tracks[i])) {
                tracks[i].itemIncurredIncrease = increase;
            }
        }
    } else {

        DistributeSpaceUpToLimits(
            extraSpace, tracks, n, trackIsAffected,
            [](const GridTrack&) { return 1.0f; },
            [](const GridTrack& t) {
                return t.growthLimit == INFINITY ? t.baseSize : t.growthLimit;
            },
            [&](const GridTrack& t) {
                return t.FitContentLimit(axisInnerNodeSize);
            });
    }

    for (int i = 0; i < n; i++) {
        GridTrack& t = tracks[i];
        if (t.itemIncurredIncrease > t.growthLimitPlannedIncrease) {
            t.growthLimitPlannedIncrease = t.itemIncurredIncrease;
        }
        t.itemIncurredIncrease = 0.0f;
    }
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
                switch (track.minTrackSizingFunction.raw.Tag()) {
                    case CompactLength::kMinContentTag:
                        newBaseSize = F32Max(
                            track.baseSize,
                            sizer.MinContentContribution(item, axisTracks));
                        break;
                    case CompactLength::kPercentTag:

                        if (!IsSome(axisInnerNodeSize)) {
                            newBaseSize = F32Max(
                                track.baseSize,
                                sizer.MinContentContribution(item, axisTracks));
                        }
                        break;
                    case CompactLength::kMaxContentTag:
                        newBaseSize = F32Max(
                            track.baseSize,
                            sizer.MaxContentContribution(item, axisTracks));
                        break;
                    case CompactLength::kAutoTag: {
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
                            Optf limit =
                                track.maxTrackSizingFunction
                                    .DefiniteLimit(axisInnerNodeSize, calc);
                            space = F32Max(MaybeMin(axisMinContentSize, limit),
                                           axisMinimumSize);
                        } else {
                            space = sizer.MinimumContribution(item, axisTracks,
                                                              nAxisTracks);
                        }
                        newBaseSize = F32Max(track.baseSize, space);
                        break;
                    }
                    case CompactLength::kLengthTag:

                        break;
                    default:

                        if (track.minTrackSizingFunction.raw.IsCalc() &&
                            !IsSome(axisInnerNodeSize)) {
                            newBaseSize = F32Max(
                                track.baseSize,
                                sizer.MinContentContribution(item, axisTracks));
                        }
                        break;
                }

                bool haveGrowthLimitMinContent =
                    !IsScrollContainer(axisOverflow);
                float growthLimitMinContent =
                    haveGrowthLimitMinContent
                        ? sizer.MinContentContribution(item, axisTracks)
                        : 0.0f;
                float growthLimitMaxContent =
                    sizer.MaxContentContribution(item, axisTracks);
                float growthLimitIntrinsicMinContent =
                    sizer.MinContentContribution(item, axisTracks);

                GridTrack& t = axisTracks[trackIndex];
                t.baseSize = newBaseSize;

                if (t.maxTrackSizingFunction.IsFitContent()) {

                    if (haveGrowthLimitMinContent) {
                        t.growthLimitPlannedIncrease =
                            F32Max(t.growthLimitPlannedIncrease,
                                   growthLimitMinContent);
                    }

                    float fitContentLimit =
                        t.FitContentLimit(axisInnerNodeSize);
                    float maxContentContribution =
                        F32Min(growthLimitMaxContent, fitContentLimit);
                    t.growthLimitPlannedIncrease = F32Max(
                        t.growthLimitPlannedIncrease, maxContentContribution);
                } else if (t.maxTrackSizingFunction.IsMaxContentAlike() ||
                           (t.maxTrackSizingFunction.UsesPercentage() &&
                            !IsSome(axisInnerNodeSize))) {

                    t.growthLimitPlannedIncrease = F32Max(
                        t.growthLimitPlannedIncrease, growthLimitMaxContent);
                } else if (t.maxTrackSizingFunction.IsIntrinsic()) {
                    t.growthLimitPlannedIncrease =
                        F32Max(t.growthLimitPlannedIncrease,
                               growthLimitIntrinsicMinContent);
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
                space = F32Max(MaybeMin(axisMinContentSize, limit),
                               axisMinimumSize);
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
                        [&](const GridTrack& t) {
                            return t.FitContentLimitedGrowthLimit(
                                axisInnerNodeSize);
                        },
                        IntrinsicContributionType::Minimum, axisInnerNodeSize);
                } else {
                    DistributeItemSpaceToBaseSize(
                        isFlex, useFlexFactorForDistribution, space,
                        axisTracks + from, count, hasIntrinsicMin,
                        [](const GridTrack& t) { return t.growthLimit; },
                        IntrinsicContributionType::Minimum, axisInnerNodeSize);
                }
            }
        }
        FlushPlannedBaseSizeIncreases(axisTracks, nAxisTracks);

        auto hasMinOrMaxContentMin = [](const GridTrack& t) {
            return t.minTrackSizingFunction.IsMinOrMaxContent();
        };
        for (int bi = 0; bi < batchLen; bi++) {
            GridItem* item = &batch[bi];
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
                        [&](const GridTrack& t) {
                            return t.FitContentLimitedGrowthLimit(
                                axisInnerNodeSize);
                        },
                        IntrinsicContributionType::Minimum, axisInnerNodeSize);
                } else {
                    DistributeItemSpaceToBaseSize(
                        isFlex, useFlexFactorForDistribution, space,
                        axisTracks + from, count, hasMinOrMaxContentMin,
                        [](const GridTrack& t) { return t.growthLimit; },
                        IntrinsicContributionType::Minimum, axisInnerNodeSize);
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
                float axisMaxContentSize =
                    sizer.MaxContentContribution(item, axisTracks);
                Optf limit = SpannedTrackLimit(*item, axis, axisTracks,
                                               axisInnerNodeSize, calc);
                float space = MaybeMin(axisMaxContentSize, limit);
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
                        IntrinsicContributionType::Maximum, axisInnerNodeSize);
                } else {
                    DistributeItemSpaceToBaseSize(
                        isFlex, useFlexFactorForDistribution, space,
                        axisTracks + from, count, hasAutoMin,
                        [&](const GridTrack& t) {
                            return t.FitContentLimitedGrowthLimit(
                                axisInnerNodeSize);
                        },
                        IntrinsicContributionType::Maximum, axisInnerNodeSize);
                }
            }
            FlushPlannedBaseSizeIncreases(axisTracks, nAxisTracks);
        }

        auto hasMaxContentMinFn = [](const GridTrack& t) {
            return t.minTrackSizingFunction.IsMaxContent();
        };
        for (int bi = 0; bi < batchLen; bi++) {
            GridItem* item = &batch[bi];
            float space = sizer.MaxContentContribution(item, axisTracks);
            int from = item->TrackRangeStart(axis);
            int count = item->TrackRangeEnd(axis) - from;
            if (space > 0.0f && count > 0) {
                DistributeItemSpaceToBaseSize(
                    isFlex, useFlexFactorForDistribution, space,
                    axisTracks + from, count, hasMaxContentMinFn,
                    [](const GridTrack& t) { return t.growthLimit; },
                    IntrinsicContributionType::Maximum, axisInnerNodeSize);
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
                float space = sizer.MinContentContribution(item, axisTracks);
                int from = item->TrackRangeStart(axis);
                int count = item->TrackRangeEnd(axis) - from;
                if (space > 0.0f && count > 0) {
                    DistributeItemSpaceToGrowthLimit(space, axisTracks + from,
                                                     count, hasIntrinsicMax,
                                                     axisInnerNodeSize);
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
                float space = sizer.MaxContentContribution(item, axisTracks);
                int from = item->TrackRangeStart(axis);
                int count = item->TrackRangeEnd(axis) - from;
                if (space > 0.0f && count > 0) {
                    DistributeItemSpaceToGrowthLimit(space, axisTracks + from,
                                                     count, hasMaxContentMax,
                                                     axisInnerNodeSize);
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

void MaximiseTracks(GridTrack* axisTracks, int n, Optf axisInnerNodeSize,
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
            [&](const GridTrack& t) {
                return t.FitContentLimitedGrowthLimit(axisInnerNodeSize);
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
    while (true) {
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
                          GridItem* items, int nItems, Optf axisMinSize,
                          Optf axisMaxSize,
                          AvailableSpace axisAvailableSpaceForExpansion) {
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
        for (int i = 0; i < nItems; i++) {
            GridItem* item = &items[i];
            if (!item->CrossesFlexibleTrack(axis)) {
                continue;
            }
            int from = item->TrackRangeStart(axis);
            int count = item->TrackRangeEnd(axis) - from;

            float maxContentContribution = ItemMaxContentContributionCached(
                item, axis, tree, SizeFOptNone(), SizeFOptNone());
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

    if (hasBaselineAlignedItem) {
        ResolveItemBaselines(tree, axis, items, nItems, innerNodeSize);
    }

    bool allAtLimit = true;
    for (int i = 0; i < nAxisTracks; i++) {
        if (axisTracks[i].baseSize != axisTracks[i].growthLimit) {
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

    MaximiseTracks(axisTracks, nAxisTracks, Get(innerNodeSize, axis),
                   availableGridSpace.Get(axis));

    AvailableSpace axisAvailableSpaceForExpansion;
    Optf innerAxis = Get(innerNodeSize, axis);
    if (IsSome(innerAxis)) {
        axisAvailableSpaceForExpansion = AvailableSpace::Definite(innerAxis);
    } else if (availableGridSpace.Get(axis)
                   .kind == AvailableSpace::Kind::MinContent) {
        axisAvailableSpaceForExpansion = AvailableSpace::MinContent();
    } else {
        axisAvailableSpaceForExpansion = AvailableSpace::MaxContent();
    }

    ExpandFlexibleTracks(tree, axis, axisTracks, nAxisTracks, items, nItems,
                         axisMinSize, axisMaxSize,
                         axisAvailableSpaceForExpansion);

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
    for (int i = 0; i < n; i++) {
        GridTrack& track = tracks[i];

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
    if (position == Position::Absolute) {
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
    SizeF contentSizeContribution;
    float yPosition = 0.0f;
    float height = 0.0f;
};

AlignedItem AlignAndPositionItem(TaffyTree* tree, NodeId node, uint32_t order,
                                 RectF gridArea,
                                 OptAlignItems containerJustifyItems,
                                 OptAlignItems containerAlignItems,
                                 float baselineShim, Direction direction,
                                 float containerBorderBoxWidth,
                                 RectF containerBorder) {
    CalcResolver calc = tree->calc;
    SizeF gridAreaSize = {gridArea.right - gridArea.left,
                          gridArea.bottom - gridArea.top};

    const Style& style = tree->GetStyle(node);
    PointOverflow overflow = style.overflow;
    float scrollbarWidth = style.scrollbarWidth;
    Optf aspectRatio = style.aspectRatio;
    OptAlignItems justifySelf = style.justifySelf;
    if (justifySelf.IsSome()) {
        justifySelf.val = ResolveSelfRelative(justifySelf.val, style.direction,
                                              direction, true);
    }
    OptAlignItems alignSelf = style.alignSelf;
    if (alignSelf.IsSome()) {
        alignSelf.val = ResolveSelfRelative(alignSelf.val, style.direction,
                                            direction, false);
    }
    if (containerJustifyItems.IsSome()) {
        containerJustifyItems.val = ResolveSelfRelative(
            containerJustifyItems.val, style.direction, direction, true);
    }
    if (containerAlignItems.IsSome()) {
        containerAlignItems.val = ResolveSelfRelative(
            containerAlignItems.val, style.direction, direction, false);
    }
    Position position = style.position;

    RectFOpt inset = style.inset
                         .MaybeResolveZip(AsOptional(gridAreaSize), calc);
    RectF padding = style.padding.ResolveOrZero(Some(gridAreaSize.w), calc);
    RectF border = style.border.ResolveOrZero(Some(gridAreaSize.w), calc);
    SizeF paddingBorderSize = (padding + border).SumAxes();
    SizeF boxSizingAdjustment = style.boxSizing == BoxSizing::ContentBox
                                    ? paddingBorderSize
                                    : SizeF::Zero();

    SizeFOpt gridAreaOpt = AsOptional(gridAreaSize);
    SizeFOpt inherentSize =
        MaybeAdd(MaybeApplyAspectRatio(
                     style.size.MaybeResolve(gridAreaOpt, calc), aspectRatio),
                 boxSizingAdjustment);
    SizeFOpt minSize = MaybeApplyAspectRatio(
        MaybeMax(Or(MaybeAdd(style.minSize.MaybeResolve(gridAreaOpt, calc),
                             boxSizingAdjustment),
                    AsOptional(paddingBorderSize)),
                 paddingBorderSize),
        aspectRatio);
    SizeFOpt maxSize = MaybeAdd(
        MaybeApplyAspectRatio(style.maxSize.MaybeResolve(gridAreaOpt, calc),
                              aspectRatio),
        boxSizingAdjustment);

    AlignItems horizontalAlignment =
        justifySelf.Or(containerJustifyItems)
            .UnwrapOr(IsSome(inherentSize.w)
                          ? AlignItems{AlignItemsKeyword::Start}
                          : AlignItems{AlignItemsKeyword::Stretch});
    AlignItems verticalAlignment =
        alignSelf.Or(containerAlignItems)
            .UnwrapOr((IsSome(inherentSize.h) || IsSome(aspectRatio))
                          ? AlignItems{AlignItemsKeyword::Start}
                          : AlignItems{AlignItemsKeyword::Stretch});

    RectFOpt margin = style.margin.MaybeResolve(Some(gridAreaSize.w), calc);

    SizeF gridAreaMinusItemMarginsSize = {
        MaybeSub(MaybeSub(gridAreaSize.w, margin.left), margin.right),
        MaybeSub(MaybeSub(gridAreaSize.h, margin.top), margin.bottom) -
            baselineShim};

    Optf width = inherentSize.w;
    if (!IsSome(width)) {
        if (position == Position::Absolute && IsSome(inset.left) &&
            IsSome(inset.right)) {
            width = Some(F32Max(
                gridAreaMinusItemMarginsSize.w - inset.left - inset.right,
                0.0f));
        } else if (IsSome(margin.left) && IsSome(margin.right) &&
                   horizontalAlignment.keyword == AlignItemsKeyword::Stretch &&
                   position != Position::Absolute) {
            width = Some(gridAreaMinusItemMarginsSize.w);
        }
    }
    SizeFOpt sized =
        MaybeApplyAspectRatio(SizeFOpt{width, inherentSize.h}, aspectRatio);

    Optf height = sized.h;
    if (!IsSome(height)) {
        if (position == Position::Absolute && IsSome(inset.top) &&
            IsSome(inset.bottom)) {
            height = Some(F32Max(
                gridAreaMinusItemMarginsSize.h - inset.top - inset.bottom,
                0.0f));
        } else if (IsSome(margin.top) && IsSome(margin.bottom) &&
                   verticalAlignment.keyword == AlignItemsKeyword::Stretch &&
                   position != Position::Absolute) {
            height = Some(gridAreaMinusItemMarginsSize.h);
        }
    }
    sized = MaybeApplyAspectRatio(SizeFOpt{sized.w, height}, aspectRatio);
    sized = MaybeClamp(sized, minSize, maxSize);

    SizeAvail avail = SizeAvail::Definite(gridAreaMinusItemMarginsSize);
    SizeFOpt size = sized;
    if (position == Position::Absolute &&
        (!IsSome(sized.w) || !IsSome(sized.h))) {
        SizeF measured = tree->MeasureChildSizeBoth(
            node, sized, gridAreaOpt, avail, SizingMode::InherentSize,
            LineBool::False());
        size = AsOptional(measured);
    }

    LayoutOutput layoutOutput =
        tree->PerformChildLayout(node, size, gridAreaOpt, avail,
                                 SizingMode::InherentSize, LineBool::False());

    SizeF finalSize =
        MaybeClamp(UnwrapOr(size, layoutOutput.size), minSize, maxSize);

    AlignedAxis xr = AlignItemWithinArea(
        {gridArea.left, gridArea.right},
        justifySelf.UnwrapOr(horizontalAlignment), finalSize.w, position, inset,
        false, margin, 0.0f, direction);
    AlignedAxis yr = AlignItemWithinArea({gridArea.top, gridArea.bottom},
                                         alignSelf.UnwrapOr(verticalAlignment),
                                         finalSize.h, position, inset, true,
                                         margin, baselineShim, Direction::Ltr);

    SizeF scrollbarSize = {
        overflow.y == Overflow::Scroll ? scrollbarWidth : 0.0f,
        overflow.x == Overflow::Scroll ? scrollbarWidth : 0.0f};

    Layout layout;
    layout.order = order;
    layout.location = {xr.start, yr.start};
    layout.size = finalSize;
    layout.contentSize = layoutOutput.contentSize;
    layout.scrollbarSize = scrollbarSize;
    layout.padding = padding;
    layout.border = border;
    layout.margin = {xr.margin.start, xr.margin.end, yr.margin.start,
                     yr.margin.end};
    tree->SetUnroundedLayout(node, layout);

    SizeF contribution = ComputeContentSizeContribution(
        {IsRtl(direction) ? containerBorderBoxWidth - (xr.start + finalSize.w) -
                                containerBorder.right
                          : xr.start - containerBorder.left,
         yr.start - containerBorder.top},
        finalSize, layoutOutput.contentSize, overflow);
    return {contribution, yr.start, finalSize.h};
}

void ReverseNonGutterTracks(GridTrack* tracks, int n, TrackCounts trackCounts) {

    if (trackCounts.explicitCount <= 1) {
        const int kMinTrackVecLenToReverseColumns = 5;
        if (n < kMinTrackVecLenToReverseColumns) {
            return;
        }
        int left = 1;
        int right = n - 2;
        while (left < right) {
            GridTrack tmp = tracks[left];
            tracks[left] = tracks[right];
            tracks[right] = tmp;
            left += 2;
            right = right >= 2 ? right - 2 : 0;
        }
        return;
    }

    int explicitTrackCount = (int)trackCounts.explicitCount;
    if (explicitTrackCount < 2) {
        return;
    }
    int left = (int)trackCounts.negativeImplicit;
    int right = left + explicitTrackCount - 1;
    while (left < right) {
        int li = 2 * left + 1;
        int ri = 2 * right + 1;
        GridTrack tmp = tracks[li];
        tracks[li] = tracks[ri];
        tracks[ri] = tmp;
        left += 1;
        right = right >= 1 ? right - 1 : 0;
    }
}

int RtlColumnOccupancyIndexForInitialization(int columnIndex,
                                             TrackCounts trackCounts) {
    if (trackCounts.explicitCount <= 1) {
        return trackCounts.Len() - columnIndex - 1;
    }
    int explicitStart = (int)trackCounts.negativeImplicit;
    int explicitEnd = explicitStart + (int)trackCounts.explicitCount;
    if (columnIndex >= explicitStart && columnIndex < explicitEnd) {
        return explicitStart + (explicitEnd - columnIndex - 1);
    }
    return columnIndex;
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
        style.alignContent.UnwrapOr(AlignContent{AlignContentKeyword::Stretch});
    AlignContent justifyContent =
        style.justifyContent
            .UnwrapOr(AlignContent{AlignContentKeyword::Stretch});
    OptAlignItems alignItems = style.alignItems;
    OptAlignItems justifyItems = style.justifyItems;

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
    Vec<ChildPlacementStyles> childPlacements;
    int childCount = tree->ChildCount(node);

    for (int i = 0; i < childCount; i++) {
        NodeId child = tree->GetChildId(node, i);
        const Style& cs = tree->GetStyle(child);
        if (cs.BoxGenMode() == BoxGenerationMode::None ||
            cs.position == Position::Absolute) {
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
        VecAppend(childPlacements, {cs.gridColumn, cs.gridRow});
    }

    TrackCounts estColCounts;
    TrackCounts estRowCounts;
    ComputeGridSizeEstimate(explicitColCount, explicitRowCount, direction,
                            childPlacements.els, len(childPlacements),
                            &estColCounts, &estRowCounts);

    Vec<GridItem> items;
    CellOccupancyMatrix cellOccupancyMatrix;
    cellOccupancyMatrix.Init(estColCounts, estRowCounts);
    PlaceGridItems(&cellOccupancyMatrix, &items, tree, children, direction,
                   style.gridAutoFlow,
                   alignItems.UnwrapOr(AlignItems{AlignItemsKeyword::Stretch}),
                   justifyItems
                       .UnwrapOr(AlignItems{AlignItemsKeyword::Stretch}));

    TrackCounts finalColCounts = cellOccupancyMatrix
                                     .Counts(AbsoluteAxis::Horizontal);
    TrackCounts finalRowCounts = cellOccupancyMatrix
                                     .Counts(AbsoluteAxis::Vertical);

    Vec<GridTrack> columns;
    Vec<GridTrack> rows;
    TrackCounts columnTrackCountsForInit = finalColCounts;
    if (IsRtl(direction) && finalColCounts.explicitCount <= 1) {
        columnTrackCountsForInit.negativeImplicit = finalColCounts
                                                        .positiveImplicit;
        columnTrackCountsForInit.positiveImplicit = finalColCounts
                                                        .negativeImplicit;
    }
    InitializeGridTracks(
        &columns, columnTrackCountsForInit, style, AbsoluteAxis::Horizontal,
        [&](int columnIndex) {
            int occupancyIndex = IsRtl(direction)
                                     ? RtlColumnOccupancyIndexForInitialization(
                                           columnIndex, finalColCounts)
                                     : columnIndex;
            return cellOccupancyMatrix.ColumnIsOccupied(occupancyIndex);
        });
    InitializeGridTracks(&rows, finalRowCounts, style, AbsoluteAxis::Vertical,
                         [&](int rowIndex) {
                             return cellOccupancyMatrix.RowIsOccupied(rowIndex);
                         });
    if (IsRtl(direction)) {
        ReverseNonGutterTracks(columns.els, len(columns), finalColCounts);
    }

    ResolveItemTrackIndexes(items.els, len(items), finalColCounts,
                            finalRowCounts);
    DetermineIfItemCrossesFlexibleOrIntrinsicTracks(items.els, len(items),
                                                    columns.els, rows.els);

    bool hasBaselineAlignedItem = false;
    for (int i = 0; i < len(items); i++) {
        if (items[i].alignSelf.keyword == AlignItemsKeyword::Baseline) {
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
    if (!IsSome(innerNodeSize.h)) {
        innerNodeSize.h = Some(initialRowSum);
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
        VecReset(childPlacements);
        cellOccupancyMatrix.Free();
        nameResolver.Free();
        return LayoutOutput::FromOuterSize(containerBorderBox);
    }

    if (!availableGridSpace.width.IsDefinite()) {
        for (int i = 0; i < len(columns); i++) {
            GridTrack& c = columns[i];
            Optf mn = c.minTrackSizingFunction
                          .ResolvedPercentageSize(containerContentBox.w, calc);
            Optf mx = c.maxTrackSizingFunction
                          .ResolvedPercentageSize(containerContentBox.w, calc);
            c.baseSize = MaybeClamp(c.baseSize, mn, mx);
        }
    }
    if (!availableGridSpace.height.IsDefinite()) {
        for (int i = 0; i < len(rows); i++) {
            GridTrack& r = rows[i];
            Optf mn = r.minTrackSizingFunction
                          .ResolvedPercentageSize(containerContentBox.h, calc);
            Optf mx = r.maxTrackSizingFunction
                          .ResolvedPercentageSize(containerContentBox.h, calc);
            r.baseSize = MaybeClamp(r.baseSize, mn, mx);
        }
    }

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
    bool parentWidthIndefinite = !availableSpace.width.IsDefinite();
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

    bool intrinsicRowContributionChanged = false;
    if (rerunColumnSizing) {
        TrackSizingAlgorithm(
            tree, AbstractAxis::Inline, Get(innerMinSize, AbstractAxis::Inline),
            Get(innerMaxSize, AbstractAxis::Inline), justifyContent,
            alignContent, availableGridSpace, innerNodeSize, columns.els,
            len(columns), rows.els, len(rows), items.els, len(items),
            TrackSizeEstimate::BaseSize, hasBaselineAlignedItem);

        bool parentHeightIndefinite = !availableSpace.height.IsDefinite();
        bool rerunRowSizing = parentHeightIndefinite && hasPercentageRow;

        if (!rerunRowSizing) {
            for (int i = 0; i < len(items); i++) {
                GridItem* item = &items[i];
                if (!item->crossesIntrinsicColumn) {
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
        } else {
            for (int i = 0; i < len(items); i++) {
                items[i].hasGridAreaSizeCache = false;
                items[i].minContentContributionCache.h = None();
                items[i].maxContentContributionCache.h = None();
                items[i].minimumContributionCache.h = None();
            }
        }

        if (rerunRowSizing) {
            TrackSizingAlgorithm(
                tree, AbstractAxis::Block,
                Get(innerMinSize, AbstractAxis::Block),
                Get(innerMaxSize, AbstractAxis::Block), alignContent,
                justifyContent, availableGridSpace, innerNodeSize, rows.els,
                len(rows), columns.els, len(columns), items.els, len(items),
                TrackSizeEstimate::BaseSize, false);
        }
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

    SizeF itemContentSizeContribution = SizeF::Zero();
    SizeF absoluteContentSize = SizeF::Zero();

    StableSort(items.els, len(items), [](const GridItem& a, const GridItem& b) {
        return a.sourceOrder < b.sourceOrder;
    });

    for (int index = 0; index < len(items); index++) {
        GridItem& item = items[index];
        RectF gridArea = {columns[(int)item.columnIndexes.start + 1].offset,
                          columns[(int)item.columnIndexes.end].offset,
                          rows[(int)item.rowIndexes.start + 1].offset,
                          rows[(int)item.rowIndexes.end].offset};
        AlignedItem placed =
            AlignAndPositionItem(tree, item.node, (uint32_t)index, gridArea,
                                 justifyItems, alignItems, item.baselineShim,
                                 direction, containerBorderBox.w, border);
        item.yPosition = placed.yPosition;
        item.height = placed.height;
        itemContentSizeContribution =
            Max(itemContentSizeContribution, placed.contentSizeContribution);
    }

    uint32_t order = (uint32_t)len(items);
    for (int index = 0; index < childCount; index++) {
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
        if (cs.position != Position::Absolute) {
            continue;
        }

        LineOptOzl colLines = nameResolver.ResolveColumnNames(cs.gridColumn)
                                  .IntoOriginZero(finalColCounts.explicitCount)
                                  .ResolveAbsolutelyPositionedGridTracks();
        int colStartIdx = -1;
        int colEndIdx = -1;
        if (colLines.start.IsSome()) {
            OriginZeroLine l = colLines.start.val;
            if (IsRtl(direction)) {
                l = OriginZeroLine{
                    (int16_t)((int16_t)finalColCounts.explicitCount - l.v)};
            }
            TryIntoTrackVecIndex(l, finalColCounts, &colStartIdx);
        }
        if (colLines.end.IsSome()) {
            OriginZeroLine l = colLines.end.val;
            if (IsRtl(direction)) {
                l = OriginZeroLine{
                    (int16_t)((int16_t)finalColCounts.explicitCount - l.v)};
            }
            TryIntoTrackVecIndex(l, finalColCounts, &colEndIdx);
        }
        if (IsRtl(direction)) {
            int tmp = colStartIdx;
            colStartIdx = colEndIdx;
            colEndIdx = tmp;
        }

        LineOptOzl rowLines = nameResolver.ResolveRowNames(cs.gridRow)
                                  .IntoOriginZero(finalRowCounts.explicitCount)
                                  .ResolveAbsolutelyPositionedGridTracks();
        int rowStartIdx = -1;
        int rowEndIdx = -1;
        if (rowLines.start.IsSome()) {
            TryIntoTrackVecIndex(rowLines.start.val, finalRowCounts,
                                 &rowStartIdx);
        }
        if (rowLines.end.IsSome()) {
            TryIntoTrackVecIndex(rowLines.end.val, finalRowCounts, &rowEndIdx);
        }

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

        RectF gridArea;
        gridArea.top =
            rowStartIdx >= 0 ? lineAsStartEdge(rows, rowStartIdx) : border.top;
        gridArea.bottom =
            rowEndIdx >= 0
                ? lineAsEndEdge(rows, rowEndIdx)
                : containerBorderBox.h - border.bottom - scrollbarGutter.y;
        gridArea
            .left = colStartIdx >= 0
                        ? lineAsStartEdge(columns, colStartIdx)
                        : (IsRtl(direction) ? border.left + scrollbarGutter.x
                                            : border.left);
        gridArea.right =
            colEndIdx >= 0
                ? lineAsEndEdge(columns, colEndIdx)
                : (IsRtl(direction) ? containerBorderBox.w - border.right
                                    : containerBorderBox.w - border.right -
                                          scrollbarGutter.x);

        AlignedItem placed = AlignAndPositionItem(
            tree, child, order, gridArea, justifyItems, alignItems, 0.0f,
            direction, containerBorderBox.w, border);
        absoluteContentSize =
            Max(absoluteContentSize, placed.contentSizeContribution);
        order += 1;
    }

    itemContentSizeContribution
        .w += IsRtl(direction) ? padding.left : padding.right;
    itemContentSizeContribution.h += padding.bottom;
    SizeF finalContentSize =
        Max(itemContentSizeContribution, absoluteContentSize);

    LayoutOutput out;
    if (len(items) == 0) {
        out = LayoutOutput::FromOuterSize(containerBorderBox);
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
            if (items[i].alignSelf.keyword == AlignItemsKeyword::Baseline) {
                chosen = &items[i];
                break;
            }
        }
        float gridContainerBaseline =
            chosen->yPosition + UnwrapOr(chosen->baseline, chosen->height);
        out = LayoutOutput::FromSizesAndBaselines(
            containerBorderBox, finalContentSize,
            PointFOpt{None(), Some(gridContainerBaseline)});
    }

    VecReset(items);
    VecReset(columns);
    VecReset(rows);
    VecReset(children);
    VecReset(childPlacements);
    cellOccupancyMatrix.Free();
    nameResolver.Free();
    return out;
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
    MinMaxSpan r = ChildMinLineMaxLineSpan(line, explicitTrackCount);
    *outMinLine = r.minLine.v;
    *outMaxLine = r.maxLine.v;
    *outSpan = r.span;
}

void GridSizeEstimateForTest(uint16_t explicitColCount,
                             uint16_t explicitRowCount, Direction direction,
                             const LinePlacement* columns,
                             const LinePlacement* rows, int n,
                             uint16_t* outColCounts, uint16_t* outRowCounts) {
    Vec<ChildPlacementStyles> children;
    for (int i = 0; i < n; i++) {
        VecAppend(children, {columns[i], rows[i]});
    }
    TrackCounts cols;
    TrackCounts rws;
    ComputeGridSizeEstimate(explicitColCount, explicitRowCount, direction,
                            children.els, len(children), &cols, &rws);
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
    Vec<ChildPlacementStyles> childPlacements;
    int childCount = tree->ChildCount(parent);
    for (int i = 0; i < childCount; i++) {
        NodeId child = tree->GetChildId(parent, i);
        const Style& cs = tree->GetStyle(child);
        if (cs.BoxGenMode() == BoxGenerationMode::None ||
            cs.position == Position::Absolute) {
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
        VecAppend(childPlacements, {cs.gridColumn, cs.gridRow});
    }

    TrackCounts estCols;
    TrackCounts estRows;
    ComputeGridSizeEstimate(explicitColCount, explicitRowCount, Direction::Ltr,
                            childPlacements.els, len(childPlacements), &estCols,
                            &estRows);

    Vec<GridItem> items;
    CellOccupancyMatrix matrix;
    matrix.Init(estCols, estRows);
    PlaceGridItems(&matrix, &items, tree, children, Direction::Ltr, flow,
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
    VecReset(childPlacements);
    matrix.Free();
    nameResolver.Free();
    return n;
}

}

#line 1 "src/taffy/compute.cpp"

namespace taffy {

void ComputeRootLayout(TaffyTree* tree, NodeId root, SizeAvail availableSpace) {
    SizeFOpt knownDimensions = SizeFOptNone();
    CalcResolver calc = tree->calc;

    {
        SizeFOpt parentSize = availableSpace.IntoOptions();
        const Style& style = tree->GetStyle(root);

        if (style.IsBlock()) {
            Optf aspectRatio = style.aspectRatio;
            RectF margin = style.margin.ResolveOrZero(parentSize.w, calc);
            RectF padding = style.padding.ResolveOrZero(parentSize.w, calc);
            RectF border = style.border.ResolveOrZero(parentSize.w, calc);
            SizeF paddingBorderSize = (padding + border).SumAxes();
            SizeF boxSizingAdjustment = style.boxSizing == BoxSizing::ContentBox
                                            ? paddingBorderSize
                                            : SizeF::Zero();

            SizeFOpt minSize = MaybeAdd(
                MaybeApplyAspectRatio(
                    style.minSize.MaybeResolve(parentSize, calc), aspectRatio),
                boxSizingAdjustment);
            SizeFOpt maxSize = MaybeAdd(
                MaybeApplyAspectRatio(
                    style.maxSize.MaybeResolve(parentSize, calc), aspectRatio),
                boxSizingAdjustment);
            SizeFOpt clampedStyleSize = MaybeClamp(
                MaybeAdd(
                    MaybeApplyAspectRatio(
                        style.size.MaybeResolve(parentSize, calc), aspectRatio),
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
            availableSpaceBasedSize.w = MaybeSub(
                availableSpace.width.IntoOption(), margin.HorizontalAxisSum());

            SizeFOpt known = Or(knownDimensions, minMaxDefiniteSize);
            known = Or(known, clampedStyleSize);
            known = Or(known, availableSpaceBasedSize);
            knownDimensions = MaybeMax(known, paddingBorderSize);
        }
    }

    LayoutOutput output = tree->PerformChildLayout(
        root, knownDimensions, availableSpace.IntoOptions(), availableSpace,
        SizingMode::InherentSize, LineBool::False());

    const Style& style = tree->GetStyle(root);
    Optf widthOpt = availableSpace.width.IntoOption();
    RectF padding = style.padding.ResolveOrZero(widthOpt, calc);
    RectF border = style.border.ResolveOrZero(widthOpt, calc);
    RectF margin = style.margin.ResolveOrZero(widthOpt, calc);

    SizeF scrollbarSize = {
        style.overflow.y == Overflow::Scroll ? style.scrollbarWidth : 0.0f,
        style.overflow.x == Overflow::Scroll ? style.scrollbarWidth : 0.0f};
    PointF location;
    if (IsRtl(style.direction) && IsSome(widthOpt)) {
        location.x = widthOpt - output.size.w;
    }

    Layout layout;
    layout.order = 0;
    layout.location = location;
    layout.size = output.size;
    layout.contentSize = output.contentSize;
    layout.scrollbarSize = scrollbarSize;
    layout.padding = padding;
    layout.border = border;

    layout.margin = margin;
    tree->SetUnroundedLayout(root, layout);
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

    cumulativeX += unrounded.location.x;
    cumulativeY += unrounded.location.y;

    layout.location.x = F32Round(unrounded.location.x);
    layout.location.y = F32Round(unrounded.location.y);
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
    layout.contentSize.w =
        F32Round(cumulativeX + unrounded.contentSize.w) - F32Round(cumulativeX);
    layout.contentSize.h =
        F32Round(cumulativeY + unrounded.contentSize.h) - F32Round(cumulativeY);

    tree->SetFinalLayout(nodeId, layout);

    int n = tree->ChildCount(nodeId);
    for (int i = 0; i < n; i++) {
        RoundLayoutInner(tree, tree->GetChildId(nodeId, i), cumulativeX,
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

    RectF margin = style.margin.ResolveOrZero(parentSize.w, calc);
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
        IsScrollContainer(style.overflow.y) ||
        style.position == Position::Absolute || padding.top > 0.0f ||
        padding.bottom > 0.0f || border.top > 0.0f || border.bottom > 0.0f ||
        (IsSome(nodeSize.h) && nodeSize.h > 0.0f) ||
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
    availableSpace.width = MaybeSub(availWidth, margin.HorizontalAxisSum())
                               .MaybeSet(knownDimensions.w)
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
    availableSpace.height = MaybeSub(availHeight, margin.VerticalAxisSum())
                                .MaybeSet(knownDimensions.h)
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
    out.contentSize = measuredSize + padding.SumAxes();
    out.marginsCanCollapseThrough = !hasStylesPreventingBeingCollapsedThrough &&
                                    size.h == 0.0f && measuredSize.h == 0.0f;
    return out;
}

AlignContentKeyword ApplyAlignmentFallback(float freeSpace, int numItems,
                                           AlignContent alignmentMode) {
    AlignContentKeyword keyword = alignmentMode.keyword;
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

SizeF ComputeContentSizeContribution(PointF location, SizeF size,
                                     SizeF contentSize,
                                     PointOverflow overflow) {
    SizeF contribution = {
        overflow.x == Overflow::Visible ? F32Max(size.w, contentSize.w)
                                        : size.w,
        overflow.y == Overflow::Visible ? F32Max(size.h, contentSize.h)
                                        : size.h};
    if (contribution.w > 0.0f && contribution.h > 0.0f) {
        float maxX = F32Max(location.x + contribution.w, 0.0f);
        float minX = F32Min(location.x, 0.0f);
        float maxY = F32Max(location.y + contribution.h, 0.0f);
        float minY = F32Min(location.y, 0.0f);
        return {maxX - minX, maxY - minY};
    }
    return SizeF::Zero();
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

static PlainPlacement IntoOriginZeroIgnoringNamed(const GridPlacement& p,
                                                  uint16_t explicitTrackCount) {
    switch (p.kind) {
        case GridPlacementKind::Span:
            return PlainPlacement::Spanning(p.span);
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
        return p;
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
            return DefiniteValue(parentSize, calc);
    }
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
           a.itemIsReplaced == b.itemIsReplaced && a.boxSizing == b.boxSizing &&
           a.direction == b.direction &&

           a.overflow == b.overflow &&
           SameFloatBits(a.scrollbarWidth, b.scrollbarWidth) &&

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

struct LeafMeasureCtx {
    TaffyTree* tree;
    NodeId node;
    void* nodeContext;
    const Style* style;
};

static SizeF LeafMeasureThunk(SizeFOpt knownDimensions,
                              SizeAvail availableSpace, void* ctx) {
    LeafMeasureCtx* c = (LeafMeasureCtx*)ctx;
    if (!c->tree->measureFn) {
        return SizeF::Zero();
    }
    return c->tree
        ->measureFn(knownDimensions, availableSpace, c->node, c->nodeContext,
                    c->style, c->tree->measureUserData);
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
    if (d->cache.GetWithKey(key, inputs.runMode, &cached)) {
        return cached;
    }

    Display displayMode = d->style.display;
    bool hasChildren = d->children.len > 0;

    LayoutOutput out;
    if (displayMode == Display::None) {
        out = ComputeHiddenLayout(this, node);
    } else if (!hasChildren) {
        LeafMeasureCtx ctx = {this, node, d->hasContext ? d->context : nullptr,
                              &d->style};
        out = ComputeLeafLayout(inputs, d->style, calc, LeafMeasureThunk, &ctx);
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
        "content_w: %-4g content_h: %-4g",
        Str(linesString), Str(fork), Str(displayStr), (double)layout.location.x,
        (double)layout.location.y, (double)layout.size.w, (double)layout.size.h,
        (double)layout.contentSize.w, (double)layout.contentSize.h));
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
    return key;
}

uint64_t CacheKey::XAxisParentSize() const {
    return parentSize & (kXAxisValueMask & kNonSignBitsMask);
}

static int ComputeCacheSlot(SizeFOpt knownDimensions,
                            SizeAvail availableSpace) {
    bool hasKnownWidth = IsSome(knownDimensions.w);
    bool hasKnownHeight = IsSome(knownDimensions.h);

    if (hasKnownWidth && hasKnownHeight) {
        return 0;
    }
    bool heightIsMin = availableSpace.height
                           .kind == AvailableSpace::Kind::MinContent;
    bool widthIsMin = availableSpace.width
                          .kind == AvailableSpace::Kind::MinContent;
    if (hasKnownWidth && !hasKnownHeight) {
        return 1 + (heightIsMin ? 1 : 0);
    }
    if (hasKnownHeight && !hasKnownWidth) {
        return 3 + (widthIsMin ? 1 : 0);
    }
    if (!widthIsMin) {
        return heightIsMin ? 6 : 5;
    }
    return heightIsMin ? 8 : 7;
}

bool Cache::Get(const LayoutInput& input, LayoutOutput* out) const {
    return GetWithKey(CacheKey::From(input), input.runMode, out);
}

void Cache::Store(const LayoutInput& input, const LayoutOutput& output) {
    StoreWithKey(CacheKey::From(input), input, output);
}

bool Cache::GetWithKey(CacheKey key, RunMode runMode, LayoutOutput* out) const {
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
                    e.key.XAxisParentSize() == xAxis) {
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
    switch (input.runMode) {
        case RunMode::PerformLayout:
            isEmpty = false;
            presentMask |= kFinalBit;
            finalLayoutEntry = {key, output};
            break;
        case RunMode::ComputeSize: {
            isEmpty = false;
            int slot =
                ComputeCacheSlot(input.knownDimensions, input.availableSpace);
            presentMask |= MeasureBit(slot);
            measureEntries[slot] = {key, output.size};
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
    return true;
}

bool Cache::IsEmpty() const {
    return presentMask == 0;
}

}

#if GPUI_OS_LINUX
#line 1 "src/base_linux.cpp"

#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>

namespace base {

void PlatDirNameInPlace(char* path);

void PlatGetExeDir(char* out, int cap) {
    if (!out || cap <= 0) {
        return;
    }
    out[0] = 0;
    ssize_t n = readlink("/proc/self/exe", out, (size_t)cap - 1);
    if (n <= 0) {
        return;
    }
    out[n] = 0;
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

}

#endif

#if GPUI_OS_MAC
#line 1 "src/base_mac.cpp"

#include <mach/mach.h>
#include <mach-o/dyld.h>

namespace base {

void PlatDirNameInPlace(char* path);

void PlatGetExeDir(char* out, int cap) {
    if (!out || cap <= 0) {
        return;
    }
    out[0] = 0;
    uint32_t n = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &n) != 0) {
        out[0] = 0;
        return;
    }
    out[cap - 1] = 0;
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
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace base {

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
#if GPUI_OS_MAC || GPUI_OS_IOS
        e.modified = (uint64_t)st.st_mtimespec.tv_sec * 1000000000ull +
                     (uint64_t)st.st_mtimespec.tv_nsec;
#else
        e.modified = (uint64_t)st.st_mtim.tv_sec * 1000000000ull +
                     (uint64_t)st.st_mtim.tv_nsec;
#endif
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

}

#endif

#if GPUI_OS_WASM
#line 1 "src/base_wasm.cpp"

#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
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

void PlatGetExeDir(char* out, int cap) {
    if (!out || cap <= 0) {
        return;
    }
    out[0] = 0;
    GetModuleFileNameA(nullptr, out, (DWORD)cap);
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/') {
        out[--n] = 0;
    }
    while (n > 0 && (out[n - 1] == '\\' || out[n - 1] == '/')) {
        out[--n] = 0;
    }
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

}

#endif
