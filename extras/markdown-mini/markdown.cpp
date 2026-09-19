#define GPUI_INCLUDE_PRIVATE_API 1
#include "markdown.h"

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

#line 1 "src/markdown-mini/markdown.cpp"

namespace markdown {

using base::Alloc;

struct MiniParser {
    Arena* a = nullptr;
    Str source = {};
    const ParseOptions* options = nullptr;
};

struct MiniLine {
    int32_t start = 0;
    int32_t end = 0;
    int32_t next = 0;
};

struct MiniListMarker {
    bool valid = false;
    bool ordered = false;
    int32_t start = 1;
    int32_t indent = 0;
    int32_t content = 0;
};

static bool MiniSpace(char c) {
    return c == ' ' || c == '\t';
}

static bool MiniDigit(char c) {
    return c >= '0' && c <= '9';
}

static bool MiniHex(char c) {
    return MiniDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool MiniPunctuation(char c) {
    switch (c) {
        case '!':
        case '"':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case '-':
        case '.':
        case '/':
        case ':':
        case ';':
        case '<':
        case '=':
        case '>':
        case '?':
        case '@':
        case '[':
        case '\\':
        case ']':
        case '^':
        case '_':
        case '`':
        case '{':
        case '|':
        case '}':
        case '~':
            return true;
        default:
            return false;
    }
}

static Str MiniSlice(Str s, int32_t start, int32_t end) {
    if (!s.s || start < 0 || end < start || end > len(s)) {
        return {};
    }
    return Str(s.s + start, end - start);
}

static Str MiniTrim(Str s) {
    int32_t start = 0;
    int32_t end = len(s);
    while (start < end && MiniSpace(s.s[start])) {
        start++;
    }
    while (end > start && MiniSpace(s.s[end - 1])) {
        end--;
    }
    return MiniSlice(s, start, end);
}

static Str MiniOwn(Arena* a, Str s) {
    char* out = (char*)Alloc(a, len(s) + 1);
    if (!out) {
        return {};
    }
    if (len(s) > 0) {
        memcpy(out, s.s, (size_t)len(s));
    }
    out[len(s)] = 0;
    return Str(out, len(s));
}

static MiniLine MiniReadLine(Str source, int32_t at) {
    MiniLine line;
    line.start = at;
    while (at < len(source) && source.s[at] != '\n' && source.s[at] != '\r') {
        at++;
    }
    line.end = at;
    if (at < len(source) && source.s[at] == '\r') {
        at++;
        if (at < len(source) && source.s[at] == '\n') {
            at++;
        }
    } else if (at < len(source)) {
        at++;
    }
    line.next = at;
    return line;
}

static Str MiniLineText(Str source, const MiniLine& line) {
    return MiniSlice(source, line.start, line.end);
}

static int32_t MiniIndent(Str line) {
    int32_t n = 0;
    while (n < len(line) && n < 4) {
        if (line.s[n] == ' ') {
            n++;
        } else if (line.s[n] == '\t') {
            return 4;
        } else {
            break;
        }
    }
    return n;
}

static bool MiniBlank(Str line) {
    for (int32_t i = 0; i < len(line); i++) {
        if (!MiniSpace(line.s[i])) {
            return false;
        }
    }
    return true;
}

static Node* MiniNode(MiniParser* p, NodeKind kind, Node* parent) {
    Node* node = NodeNew(p->a, kind);
    if (node && parent) {
        NodeAddChild(p->a, parent, node);
    }
    return node;
}

static void MiniText(MiniParser* p, Node* parent, Str value) {
    if (len(value) <= 0) {
        return;
    }
    Node* text = MiniNode(p, NodeKind::Text, parent);
    if (text) {
        NodeSetStr(p->a, text, NodeStrKind::Value, value);
    }
}

static int32_t MiniFind(Str text, int32_t at, char marker, int32_t count) {
    for (int32_t i = at; i + count <= len(text); i++) {
        if (text.s[i] == '\\') {
            i++;
            continue;
        }
        int32_t n = 0;
        while (i + n < len(text) && text.s[i + n] == marker) {
            n++;
        }
        if (n >= count && i > at && !MiniSpace(text.s[i - 1])) {
            return i;
        }
        i += n > 0 ? n - 1 : 0;
    }
    return -1;
}

static int32_t MiniBracketEnd(Str text, int32_t at) {
    int32_t depth = 1;
    for (int32_t i = at; i < len(text); i++) {
        if (text.s[i] == '\\' && i + 1 < len(text)) {
            i++;
            continue;
        }
        if (text.s[i] == '[') {
            depth++;
        } else if (text.s[i] == ']' && --depth == 0) {
            return i;
        }
    }
    return -1;
}

static int32_t MiniResourceEnd(Str text, int32_t at) {
    int32_t depth = 1;
    char quote = 0;
    for (int32_t i = at; i < len(text); i++) {
        char c = text.s[i];
        if (c == '\\' && i + 1 < len(text)) {
            i++;
            continue;
        }
        if (quote) {
            if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(') {
            depth++;
        } else if (c == ')' && --depth == 0) {
            return i;
        }
    }
    return -1;
}

static void MiniInline(MiniParser* p, Node* parent, Str text);

static bool MiniLink(MiniParser* p, Node* parent, Str text, int32_t at,
                     int32_t* after) {
    bool image = text.s[at] == '!';
    int32_t open = at + (image ? 1 : 0);
    if (open >= len(text) || text.s[open] != '[') {
        return false;
    }
    if (image && !p->options->constructs.labelStartImage) {
        return false;
    }
    if (!image && !p->options->constructs.labelStartLink) {
        return false;
    }
    int32_t close = MiniBracketEnd(text, open + 1);
    if (close < 0 || close + 1 >= len(text) || text.s[close + 1] != '(') {
        return false;
    }
    int32_t resourceEnd = MiniResourceEnd(text, close + 2);
    if (resourceEnd < 0) {
        return false;
    }

    Str body = MiniTrim(MiniSlice(text, close + 2, resourceEnd));
    Str url = {};
    Str title = {};
    if (len(body) > 1 && body.s[0] == '<') {
        int32_t end = 1;
        while (end < len(body) && body.s[end] != '>') {
            end++;
        }
        if (end >= len(body)) {
            return false;
        }
        url = MiniSlice(body, 1, end);
        body = MiniTrim(MiniSlice(body, end + 1, len(body)));
    } else {
        int32_t end = 0;
        int32_t depth = 0;
        while (end < len(body)) {
            char c = body.s[end];
            if (c == '(') {
                depth++;
            } else if (c == ')' && depth > 0) {
                depth--;
            } else if (MiniSpace(c) && depth == 0) {
                break;
            }
            end++;
        }
        url = MiniSlice(body, 0, end);
        body = MiniTrim(MiniSlice(body, end, len(body)));
    }
    if (len(body) >= 2) {
        char first = body.s[0];
        char last = body.s[len(body) - 1];
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'') ||
            (first == '(' && last == ')')) {
            title = MiniSlice(body, 1, len(body) - 1);
        }
    }

    Str label = MiniSlice(text, open + 1, close);
    Node* node = MiniNode(p, image ? NodeKind::Image : NodeKind::Link, parent);
    if (!node) {
        return false;
    }
    NodeSetStr(p->a, node, NodeStrKind::Url, url);
    NodeSetStr(p->a, node, NodeStrKind::Title, title);
    if (image) {
        NodeSetStr(p->a, node, NodeStrKind::Alt, label);
    } else {
        MiniInline(p, node, label);
    }
    *after = resourceEnd + 1;
    return true;
}

static base::TempStr MiniUtf8Temp(uint32_t cp) {
    base::TempStr out = base::AllocStrTemp(4);
    if (cp < 0x80) {
        out.s[0] = (char)cp;
        out.len = 1;
        return out;
    }
    if (cp < 0x800) {
        out.s[0] = (char)(0xc0 | (cp >> 6));
        out.s[1] = (char)(0x80 | (cp & 0x3f));
        out.len = 2;
        return out;
    }
    if (cp < 0x10000) {
        out.s[0] = (char)(0xe0 | (cp >> 12));
        out.s[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out.s[2] = (char)(0x80 | (cp & 0x3f));
        out.len = 3;
        return out;
    }
    out.s[0] = (char)(0xf0 | (cp >> 18));
    out.s[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out.s[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out.s[3] = (char)(0x80 | (cp & 0x3f));
    return out;
}

static bool MiniEntity(MiniParser* p, Str text, int32_t at, int32_t* after,
                       Str* value) {
    int32_t semi = at + 1;
    while (semi < len(text) && semi - at <= 33 && text.s[semi] != ';' &&
           !MiniSpace(text.s[semi])) {
        semi++;
    }
    if (semi >= len(text) || text.s[semi] != ';' || semi == at + 1) {
        return false;
    }
    Str name = MiniSlice(text, at + 1, semi);
    if (name.s[0] == '#') {
        int32_t start = 1;
        int radix = 10;
        if (start < len(name) &&
            (name.s[start] == 'x' || name.s[start] == 'X')) {
            start++;
            radix = 16;
        }
        if (start == len(name)) {
            return false;
        }
        for (int32_t i = start; i < len(name); i++) {
            if ((radix == 10 && !MiniDigit(name.s[i])) ||
                (radix == 16 && !MiniHex(name.s[i]))) {
                return false;
            }
        }
        *value = DecodeNumeric(p->a, MiniSlice(name, start, len(name)), radix);
    } else {
        *value = DecodeNamed(p->a, name);
    }
    if (!value->s) {
        return false;
    }
    *after = semi + 1;
    return true;
}

static void MiniInline(MiniParser* p, Node* parent, Str text) {
    int32_t plain = 0;
    int32_t at = 0;
    while (at < len(text)) {
        int32_t after = at;
        NodeKind markKind = NodeKind::Text;
        int32_t close = -1;
        char c = text.s[at];

        bool linkStart = c == '[' || (c == '!' && at + 1 < len(text) &&
                                      text.s[at + 1] == '[');
        bool linkEnabled = p->options->constructs.labelEnd &&
                           (c == '!' ? p->options->constructs.labelStartImage
                                     : p->options->constructs.labelStartLink);
        if (linkStart && linkEnabled) {
            int32_t labelOpen = at + (c == '!' ? 1 : 0);
            int32_t labelClose = MiniBracketEnd(text, labelOpen + 1);
            if (labelClose >= 0 && labelClose + 1 < len(text) &&
                text.s[labelClose + 1] == '(') {
                int32_t resourceEnd = MiniResourceEnd(text, labelClose + 2);
                if (resourceEnd >= 0) {
                    if (at > plain) {
                        MiniText(p, parent, MiniSlice(text, plain, at));
                    }
                    if (MiniLink(p, parent, text, at, &after)) {
                        at = after;
                        plain = at;
                        continue;
                    }
                }
            }
        }

        if (p->options->constructs.codeText && c == '`') {
            int32_t count = 1;
            while (at + count < len(text) && text.s[at + count] == '`') {
                count++;
            }
            close = MiniFind(text, at + count, '`', count);
            if (close >= 0) {
                if (at > plain) {
                    MiniText(p, parent, MiniSlice(text, plain, at));
                }
                Node* code = MiniNode(p, NodeKind::InlineCode, parent);
                Str value = MiniSlice(text, at + count, close);
                char* normalized = (char*)Alloc(p->a, len(value) + 1);
                if (normalized) {
                    for (int32_t i = 0; i < len(value); i++) {
                        normalized[i] = value.s[i] == '\n' || value.s[i] == '\r'
                                            ? ' '
                                            : value.s[i];
                    }
                    normalized[len(value)] = 0;
                    NodeSetStr(p->a, code, NodeStrKind::Value,
                               Str(normalized, len(value)));
                }
                at = close + count;
                plain = at;
                continue;
            }
        }

        if (p->options->constructs.attention && (c == '*' || c == '_')) {
            int32_t count = 1;
            while (count < 3 && at + count < len(text) &&
                   text.s[at + count] == c) {
                count++;
            }
            bool intraword = c == '_' && at > 0 && at + count < len(text) &&
                             !MiniSpace(text.s[at - 1]) &&
                             !MiniPunctuation(text.s[at - 1]) &&
                             !MiniSpace(text.s[at + count]) &&
                             !MiniPunctuation(text.s[at + count]);
            if (!intraword && at + count < len(text) &&
                !MiniSpace(text.s[at + count])) {
                close = MiniFind(text, at + count, c, count);
            }
            if (close >= 0) {
                if (at > plain) {
                    MiniText(p, parent, MiniSlice(text, plain, at));
                }
                if (count == 3) {
                    Node* strong = MiniNode(p, NodeKind::Strong, parent);
                    Node* emphasis = MiniNode(p, NodeKind::Emphasis, strong);
                    MiniInline(p, emphasis, MiniSlice(text, at + 3, close));
                } else {
                    markKind =
                        count == 2 ? NodeKind::Strong : NodeKind::Emphasis;
                    Node* marked = MiniNode(p, markKind, parent);
                    MiniInline(p, marked, MiniSlice(text, at + count, close));
                }
                at = close + count;
                plain = at;
                continue;
            }
        }

        if (c == '\\' && at + 1 < len(text)) {
            char next = text.s[at + 1];
            if (p->options->constructs.hardBreakEscape &&
                (next == '\n' || next == '\r')) {
                if (at > plain) {
                    MiniText(p, parent, MiniSlice(text, plain, at));
                }
                MiniNode(p, NodeKind::Break, parent);
                at +=
                    next == '\r' && at + 2 < len(text) && text.s[at + 2] == '\n'
                        ? 3
                        : 2;
                plain = at;
                continue;
            }
            if (p->options->constructs.characterEscape &&
                MiniPunctuation(next)) {
                if (at > plain) {
                    MiniText(p, parent, MiniSlice(text, plain, at));
                }
                MiniText(p, parent, MiniSlice(text, at + 1, at + 2));
                at += 2;
                plain = at;
                continue;
            }
        }

        if (p->options->constructs.characterReference && c == '&') {
            Str value = {};
            if (MiniEntity(p, text, at, &after, &value)) {
                if (at > plain) {
                    MiniText(p, parent, MiniSlice(text, plain, at));
                }
                MiniText(p, parent, value);
                at = after;
                plain = at;
                continue;
            }
        }

        if (c == '\n' || c == '\r') {
            int32_t textEnd = at;
            bool hard = false;
            if (p->options->constructs.hardBreakTrailing) {
                int32_t spaces = 0;
                while (textEnd > plain && text.s[textEnd - 1] == ' ') {
                    textEnd--;
                    spaces++;
                }
                hard = spaces >= 2;
                if (!hard) {
                    textEnd = at;
                }
            }
            if (textEnd > plain) {
                MiniText(p, parent, MiniSlice(text, plain, textEnd));
            }
            if (hard) {
                MiniNode(p, NodeKind::Break, parent);
            } else {
                MiniText(p, parent, StrL(" "));
            }
            at += c == '\r' && at + 1 < len(text) && text.s[at + 1] == '\n' ? 2
                                                                            : 1;
            plain = at;
            continue;
        }
        at++;
    }
    if (len(text) > plain) {
        MiniText(p, parent, MiniSlice(text, plain, len(text)));
    }
}

static bool MiniAtx(Str line, int32_t* level, Str* content) {
    int32_t at = MiniIndent(line);
    if (at > 3) {
        return false;
    }
    int32_t count = 0;
    while (at + count < len(line) && line.s[at + count] == '#' && count < 7) {
        count++;
    }
    if (count < 1 || count > 6 ||
        (at + count < len(line) && !MiniSpace(line.s[at + count]))) {
        return false;
    }
    int32_t start = at + count;
    while (start < len(line) && MiniSpace(line.s[start])) {
        start++;
    }
    int32_t end = len(line);
    while (end > start && MiniSpace(line.s[end - 1])) {
        end--;
    }
    int32_t hashEnd = end;
    while (end > start && line.s[end - 1] == '#') {
        end--;
    }
    if (end == hashEnd || (end > start && !MiniSpace(line.s[end - 1]))) {
        end = hashEnd;
    } else {
        while (end > start && MiniSpace(line.s[end - 1])) {
            end--;
        }
    }
    *level = count;
    *content = MiniSlice(line, start, end);
    return true;
}

static bool MiniSetext(Str line, int32_t* level) {
    Str value = MiniTrim(line);
    if (!StrStartsWithAny(value, "=-")) {
        return false;
    }
    char marker = value.s[0];
    int32_t count = 0;
    for (int32_t i = 0; i < len(value); i++) {
        if (value.s[i] == marker) {
            count++;
        } else if (!MiniSpace(value.s[i])) {
            return false;
        }
    }
    if (count == 0) {
        return false;
    }
    *level = marker == '=' ? 1 : 2;
    return true;
}

static bool MiniThematic(Str line) {
    Str value = MiniTrim(line);
    if (len(value) < 3 || !StrStartsWithAny(value, "*-_")) {
        return false;
    }
    char marker = value.s[0];
    int32_t count = 0;
    for (int32_t i = 0; i < len(value); i++) {
        if (value.s[i] == marker) {
            count++;
        } else if (!MiniSpace(value.s[i])) {
            return false;
        }
    }
    return count >= 3;
}

static bool MiniFence(Str line, char* marker, int32_t* count, Str* info) {
    int32_t at = MiniIndent(line);
    if (at > 3 || at >= len(line) || (line.s[at] != '`' && line.s[at] != '~')) {
        return false;
    }
    char c = line.s[at];
    int32_t n = 0;
    while (at + n < len(line) && line.s[at + n] == c) {
        n++;
    }
    if (n < 3) {
        return false;
    }
    *marker = c;
    *count = n;
    *info = MiniTrim(MiniSlice(line, at + n, len(line)));
    return true;
}

static bool MiniFenceClose(Str line, char marker, int32_t count) {
    int32_t at = MiniIndent(line);
    if (at > 3) {
        return false;
    }
    int32_t n = 0;
    while (at + n < len(line) && line.s[at + n] == marker) {
        n++;
    }
    if (n < count) {
        return false;
    }
    for (int32_t i = at + n; i < len(line); i++) {
        if (!MiniSpace(line.s[i])) {
            return false;
        }
    }
    return true;
}

static MiniListMarker MiniList(Str line) {
    MiniListMarker out;
    int32_t at = MiniIndent(line);
    if (at > 3 || at >= len(line)) {
        return out;
    }
    out.indent = at;
    char c = line.s[at];
    if (c == '-' || c == '+' || c == '*') {
        if (at + 1 < len(line) && !MiniSpace(line.s[at + 1])) {
            return out;
        }
        out.valid = true;
        out.content = at + 1 < len(line) ? at + 2 : len(line);
        while (out.content < len(line) && MiniSpace(line.s[out.content]) &&
               out.content < at + 5) {
            out.content++;
        }
        return out;
    }
    if (!MiniDigit(c)) {
        return out;
    }
    int32_t value = 0;
    int32_t digits = 0;
    while (at + digits < len(line) && MiniDigit(line.s[at + digits]) &&
           digits < 9) {
        value = value * 10 + line.s[at + digits] - '0';
        digits++;
    }
    int32_t mark = at + digits;
    if (digits == 0 || mark >= len(line) ||
        (line.s[mark] != '.' && line.s[mark] != ')') ||
        (mark + 1 < len(line) && !MiniSpace(line.s[mark + 1]))) {
        return out;
    }
    out.valid = true;
    out.ordered = true;
    out.start = value;
    out.content = mark + 1 < len(line) ? mark + 2 : len(line);
    while (out.content < len(line) && MiniSpace(line.s[out.content]) &&
           out.content < mark + 5) {
        out.content++;
    }
    return out;
}

static bool MiniQuote(Str line, int32_t* content) {
    int32_t at = MiniIndent(line);
    if (at > 3 || at >= len(line) || line.s[at] != '>') {
        return false;
    }
    at++;
    if (at < len(line) && line.s[at] == ' ') {
        at++;
    }
    *content = at;
    return true;
}

static Str MiniCopiedLines(MiniParser* p, int32_t start, int32_t end,
                           int32_t stripFirst, int32_t stripRest) {
    char* out = (char*)Alloc(p->a, end - start + 1);
    if (!out) {
        return {};
    }
    int32_t used = 0;
    int32_t at = start;
    bool first = true;
    while (at < end) {
        MiniLine line = MiniReadLine(p->source, at);
        if (line.end > end) {
            line.end = end;
            line.next = end;
        }
        Str value = MiniLineText(p->source, line);
        int32_t cut =
            first ? (stripFirst < len(value) ? stripFirst : len(value)) : 0;
        if (!first) {
            while (cut < len(value) && cut < stripRest &&
                   MiniSpace(value.s[cut])) {
                cut++;
            }
        }
        if (len(value) > cut) {
            memcpy(out + used, value.s + cut, (size_t)(len(value) - cut));
            used += len(value) - cut;
        }
        at = line.next;
        if (at < end) {
            out[used++] = '\n';
        }
        first = false;
    }
    out[used] = 0;
    return Str(out, used);
}

static void MiniBlocks(MiniParser* p, Node* parent, int32_t start, int32_t end);

static int32_t MiniCodeFence(MiniParser* p, Node* parent,
                             const MiniLine& opening, char marker,
                             int32_t count, Str info, int32_t end) {
    int32_t contentStart = opening.next;
    int32_t at = contentStart;
    int32_t contentEnd = end;
    int32_t after = end;
    while (at < end) {
        MiniLine line = MiniReadLine(p->source, at);
        if (MiniFenceClose(MiniLineText(p->source, line), marker, count)) {
            contentEnd = at;
            after = line.next;
            break;
        }
        at = line.next;
    }
    while (contentEnd > contentStart && (p->source.s[contentEnd - 1] == '\n' ||
                                         p->source.s[contentEnd - 1] == '\r')) {
        contentEnd--;
    }
    Node* code = MiniNode(p, NodeKind::Code, parent);
    NodeSetStr(p->a, code, NodeStrKind::Value,
               MiniSlice(p->source, contentStart, contentEnd));
    int32_t split = 0;
    while (split < len(info) && !MiniSpace(info.s[split])) {
        split++;
    }
    NodeSetStr(p->a, code, NodeStrKind::Lang, MiniSlice(info, 0, split));
    NodeSetStr(p->a, code, NodeStrKind::Meta,
               MiniTrim(MiniSlice(info, split, len(info))));
    return after;
}

static int32_t MiniBlockquote(MiniParser* p, Node* parent, int32_t start,
                              int32_t end) {
    int32_t at = start;
    int32_t cap = end - start + 1;
    char* out = (char*)Alloc(p->a, cap);
    if (!out) {
        return end;
    }
    int32_t used = 0;
    while (at < end) {
        MiniLine line = MiniReadLine(p->source, at);
        Str value = MiniLineText(p->source, line);
        int32_t content = 0;
        if (!MiniQuote(value, &content)) {
            if (!MiniBlank(value)) {
                break;
            }
            MiniLine next = MiniReadLine(p->source, line.next);
            int32_t ignored = 0;
            if (line.next >= end ||
                !MiniQuote(MiniLineText(p->source, next), &ignored)) {
                break;
            }
            content = len(value);
        }
        if (used > 0) {
            out[used++] = '\n';
        }
        if (len(value) > content) {
            memcpy(out + used, value.s + content,
                   (size_t)(len(value) - content));
            used += len(value) - content;
        }
        at = line.next;
    }
    out[used] = 0;
    Node* quote = MiniNode(p, NodeKind::Blockquote, parent);
    MiniParser nested = *p;
    nested.source = Str(out, used);
    MiniBlocks(&nested, quote, 0, used);
    return at;
}

static int32_t MiniListBlock(MiniParser* p, Node* parent, int32_t start,
                             int32_t end, MiniListMarker first) {
    Node* list = MiniNode(p, NodeKind::List, parent);
    list->Set(NodeOrdered, first.ordered);
    list->Set(NodeHasStart, first.ordered);
    if (first.ordered) {
        NodeSetPerKind(p->a, list, (uint32_t)first.start);
    }
    int32_t at = start;
    while (at < end) {
        MiniLine opening = MiniReadLine(p->source, at);
        Str openingText = MiniLineText(p->source, opening);
        MiniListMarker marker = MiniList(openingText);
        if (!marker.valid || marker.ordered != first.ordered ||
            marker.indent != first.indent) {
            break;
        }
        int32_t itemStart = at;
        int32_t scan = opening.next;
        int32_t itemEnd = scan;
        bool spread = false;
        bool sawBlank = false;
        while (scan < end) {
            MiniLine line = MiniReadLine(p->source, scan);
            Str value = MiniLineText(p->source, line);
            if (MiniBlank(value)) {
                sawBlank = true;
                itemEnd = line.next;
                scan = line.next;
                continue;
            }
            MiniListMarker next = MiniList(value);
            if (next.valid && next.ordered == first.ordered &&
                next.indent == first.indent) {
                spread = spread || sawBlank;
                break;
            }
            int32_t indent = MiniIndent(value);
            if (indent <= first.indent) {
                break;
            }
            spread = spread || sawBlank;
            itemEnd = line.next;
            scan = line.next;
        }
        Node* item = MiniNode(p, NodeKind::ListItem, list);
        item->Set(NodeSpread, spread);
        list->Set(NodeSpread, list->Has(NodeSpread) || spread);

        Str copied = MiniCopiedLines(p, itemStart, itemEnd, marker.content,
                                     marker.content);
        MiniParser nested = *p;
        nested.source = copied;
        MiniBlocks(&nested, item, 0, len(copied));
        at = scan;
    }
    return at;
}

static bool MiniBlockStart(MiniParser* p, Str line) {
    int32_t level = 0;
    Str content = {};
    char marker = 0;
    int32_t count = 0;
    Str info = {};
    int32_t quote = 0;
    return (p->options->constructs.headingAtx &&
            MiniAtx(line, &level, &content)) ||
           (p->options->constructs.codeFenced &&
            MiniFence(line, &marker, &count, &info)) ||
           (p->options->constructs.blockQuote && MiniQuote(line, &quote)) ||
           (p->options->constructs.thematicBreak && MiniThematic(line)) ||
           (p->options->constructs.listItem && MiniList(line).valid) ||
           (p->options->constructs.codeIndented && MiniIndent(line) >= 4);
}

static void MiniBlocks(MiniParser* p, Node* parent, int32_t start,
                       int32_t end) {
    int32_t at = start;
    while (at < end) {
        MiniLine line = MiniReadLine(p->source, at);
        Str text = MiniLineText(p->source, line);
        if (MiniBlank(text)) {
            at = line.next;
            continue;
        }

        int32_t level = 0;
        Str content = {};
        if (p->options->constructs.headingAtx &&
            MiniAtx(text, &level, &content)) {
            Node* heading = MiniNode(p, NodeKind::Heading, parent);
            NodeSetPerKind(p->a, heading, (uint32_t)level);
            MiniInline(p, heading, content);
            at = line.next;
            continue;
        }

        char fenceMarker = 0;
        int32_t fenceCount = 0;
        Str info = {};
        if (p->options->constructs.codeFenced &&
            MiniFence(text, &fenceMarker, &fenceCount, &info)) {
            at = MiniCodeFence(p, parent, line, fenceMarker, fenceCount, info,
                               end);
            continue;
        }

        int32_t quoteContent = 0;
        if (p->options->constructs.blockQuote &&
            MiniQuote(text, &quoteContent)) {
            at = MiniBlockquote(p, parent, at, end);
            continue;
        }

        if (p->options->constructs.thematicBreak && MiniThematic(text)) {
            MiniNode(p, NodeKind::ThematicBreak, parent);
            at = line.next;
            continue;
        }

        MiniListMarker list = MiniList(text);
        if (p->options->constructs.listItem && list.valid) {
            at = MiniListBlock(p, parent, at, end, list);
            continue;
        }

        if (p->options->constructs.codeIndented && MiniIndent(text) >= 4) {
            int32_t scan = at;
            while (scan < end) {
                MiniLine codeLine = MiniReadLine(p->source, scan);
                Str codeText = MiniLineText(p->source, codeLine);
                if (!MiniBlank(codeText) && MiniIndent(codeText) < 4) {
                    break;
                }
                scan = codeLine.next;
            }
            Str value = MiniCopiedLines(p, at, scan, 4, 4);
            while (len(value) > 0 && value.s[len(value) - 1] == '\n') {
                value.len--;
            }
            Node* code = MiniNode(p, NodeKind::Code, parent);
            NodeSetStr(p->a, code, NodeStrKind::Value, value);
            at = scan;
            continue;
        }

        MiniLine next = MiniReadLine(p->source, line.next);
        if (p->options->constructs.headingSetext && line.next < end &&
            MiniSetext(MiniLineText(p->source, next), &level)) {
            Node* heading = MiniNode(p, NodeKind::Heading, parent);
            NodeSetPerKind(p->a, heading, (uint32_t)level);
            MiniInline(p, heading, MiniTrim(text));
            at = next.next;
            continue;
        }

        int32_t paragraphEnd = line.end;
        int32_t scan = line.next;
        while (scan < end) {
            MiniLine part = MiniReadLine(p->source, scan);
            Str partText = MiniLineText(p->source, part);
            if (MiniBlank(partText) || MiniBlockStart(p, partText)) {
                break;
            }
            paragraphEnd = part.end;
            scan = part.next;
        }
        Node* paragraph = MiniNode(p, NodeKind::Paragraph, parent);
        MiniInline(p, paragraph,
                   MiniSlice(p->source, line.start, paragraphEnd));
        at = scan;
    }
}

Constructs Constructs::Gfm() {
    Constructs constructs;

    constructs.gfmAutolinkLiteral = true;
    constructs.gfmFootnoteDefinition = true;
    constructs.gfmLabelStartFootnote = true;
    constructs.gfmStrikethrough = true;
    constructs.gfmTable = true;
    constructs.gfmTaskListItem = true;
    return constructs;
}

ParseOptions ParseOptions::Gfm() {
    ParseOptions options;
    options.constructs = Constructs::Gfm();
    return options;
}

Node* ToMdast(Arena* a, Str source, const ParseOptions& options) {
    if (!a) {
        return nullptr;
    }
    MiniParser parser;
    parser.a = a;
    parser.source = source;
    parser.options = &options;
    Node* root = NodeNew(a, NodeKind::Root);
    if (root && source.s && len(source) > 0) {
        MiniBlocks(&parser, root, 0, len(source));
    }
    return root;
}

Str DecodeNamed(Arena* a, Str name) {
    const char* value = nullptr;
    if (base::StrEq(name, StrL("amp")) || base::StrEq(name, StrL("AMP"))) {
        value = "&";
    } else if (base::StrEq(name, StrL("lt")) || base::StrEq(name, StrL("LT"))) {
        value = "<";
    } else if (base::StrEq(name, StrL("gt")) || base::StrEq(name, StrL("GT"))) {
        value = ">";
    } else if (base::StrEq(name, StrL("quot")) ||
               base::StrEq(name, StrL("QUOT"))) {
        value = "\"";
    } else if (base::StrEq(name, StrL("apos"))) {
        value = "'";
    } else if (base::StrEq(name, StrL("nbsp"))) {
        value = "\xc2\xa0";
    }
    return value ? MiniOwn(a, Str((char*)value)) : Str{};
}

Str DecodeNumeric(Arena* a, Str value, int radix) {
    uint32_t cp = 0;
    bool bad = len(value) <= 0 || (radix != 10 && radix != 16);
    for (int32_t i = 0; !bad && i < len(value); i++) {
        uint8_t c = (uint8_t)value.s[i];
        uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (uint32_t)(c - 'A' + 10);
        } else {
            bad = true;
            break;
        }
        if (digit >= (uint32_t)radix ||
            cp > (0x10ffffu - digit) / (uint32_t)radix) {
            bad = true;
            break;
        }
        cp = cp * (uint32_t)radix + digit;
    }
    bad = bad || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
          cp <= 0x08 || cp == 0x0b || (cp >= 0x0e && cp <= 0x1f) ||
          (cp >= 0x7f && cp <= 0x9f);
    if (bad) {
        cp = 0xfffd;
    }
    return MiniOwn(a, MiniUtf8Temp(cp));
}

}

#line 1 "src/markdown/mdast.cpp"

namespace markdown {

using base::Alloc;

Node* NodeNew(Arena* a, NodeKind kind) {

    void* mem = a->Push(sizeof(Node), alignof(Node), false);
    if (!mem) {
        return nullptr;
    }
    Node* node = new (mem) Node();
    node->kind = kind;
    return node;
}

bool NodeHasChildren(NodeKind kind) {
    switch (kind) {
        case NodeKind::Root:
        case NodeKind::Paragraph:
        case NodeKind::Heading:
        case NodeKind::Blockquote:
        case NodeKind::List:
        case NodeKind::ListItem:
        case NodeKind::Emphasis:
        case NodeKind::Strong:
        case NodeKind::Link:
        case NodeKind::LinkReference:
        case NodeKind::FootnoteDefinition:
        case NodeKind::Table:
        case NodeKind::TableRow:
        case NodeKind::TableCell:
        case NodeKind::Delete:
            return true;
        default:
            return false;
    }
}

static bool NodeHasOwnValue(const Node* node) {
    switch (node->kind) {
        case NodeKind::Toml:
        case NodeKind::Yaml:
        case NodeKind::InlineCode:
        case NodeKind::InlineMath:
        case NodeKind::Html:
        case NodeKind::Text:
        case NodeKind::Code:
        case NodeKind::Math:
            return true;
        default:
            return false;
    }
}

static int32_t NodeToStringLen(Arena* a, const Node* node) {
    if (NodeHasChildren(node->kind)) {
        int32_t len = 0;
        for (const Node* child : NodeKids(a, node)) {
            len += NodeToStringLen(a, child);
        }
        return len;
    }
    if (!NodeHasOwnValue(node)) {
        return 0;
    }
    return NodeGetStrLen(a, node, NodeStrKind::Value);
}

static int32_t NodeToStringFill(Arena* a, const Node* node, char* out,
                                int32_t at) {
    if (NodeHasChildren(node->kind)) {
        for (const Node* child : NodeKids(a, node)) {
            at = NodeToStringFill(a, child, out, at);
        }
        return at;
    }
    Str value =
        NodeHasOwnValue(node) ? NodeGetStr(a, node, NodeStrKind::Value) : Str{};
    if (len(value) > 0) {
        memcpy(out + at, value.s, (size_t)len(value));
        at += len(value);
    }
    return at;
}

Str NodeToString(Arena* a, const Node* node) {

    int32_t len = NodeToStringLen(a, node);
    char* out = (char*)Alloc(a, len + 1);
    if (!out) {
        return {};
    }
    int32_t at = NodeToStringFill(a, node, out, 0);
    out[at] = 0;
    return Str(out, at);
}

constexpr int32_t kRecNext = 0;
constexpr int32_t kRecKind = 4;
constexpr int32_t kRecLen = 5;

static char* RecAt(Arena* a, ArenaStr off) {
    return off == kArenaStrNone ? nullptr : (char*)base::ArenaAtOffset(a, off);
}

static ArenaStr RecNext(const char* rec) {
    ArenaStr next = kArenaStrNone;
    memcpy(&next, rec + kRecNext, sizeof(next));
    return next;
}

static void RecSetNext(char* rec, ArenaStr next) {
    memcpy(rec + kRecNext, &next, sizeof(next));
}

static Str RecStr(const char* rec, int32_t* headOut = nullptr) {
    uint32_t len = 0;
    int32_t head = kRecLen + base::VarintGet(rec + kRecLen, &len);
    if (headOut) {
        *headOut = head;
    }
    return Str((char*)rec + head, (int32_t)len);
}

static char* FindRec(Arena* a, const Node* n, NodeStrKind k, char** prevOut) {
    char* prev = nullptr;
    for (ArenaStr at = n->firstStr; at != kArenaStrNone;) {
        char* rec = RecAt(a, at);
        if (!rec) {
            break;
        }
        if ((NodeStrKind)(uint8_t)rec[kRecKind] == k) {
            if (prevOut) {
                *prevOut = prev;
            }
            return rec;
        }
        prev = rec;
        at = RecNext(rec);
    }
    if (prevOut) {
        *prevOut = nullptr;
    }
    return nullptr;
}

static char* RecNew(Arena* a, Node* n, NodeStrKind k, uint32_t len,
                    int32_t* headOut) {
    int32_t head = kRecLen + base::VarintSize(len);

    char* rec = (char*)a->Push((uint64_t)head + len + 1, 1, false);
    if (!rec) {
        return nullptr;
    }
    ArenaStr at = (ArenaStr)base::ArenaOffsetOf(a, rec);
    RecSetNext(rec, n->firstStr);
    rec[kRecKind] = (char)(uint8_t)k;
    base::VarintPut(rec + kRecLen, len);
    rec[head + len] = 0;
    n->firstStr = at;
    *headOut = head;
    return rec;
}

Str NodeGetStr(Arena* a, const Node* n, NodeStrKind k) {
    char* rec = FindRec(a, n, k, nullptr);
    return rec ? RecStr(rec) : Str{};
}

int32_t NodeGetStrLen(Arena* a, const Node* n, NodeStrKind k) {
    char* rec = FindRec(a, n, k, nullptr);
    return rec ? len(RecStr(rec)) : 0;
}

bool NodeHasStr(Arena* a, const Node* n, NodeStrKind k) {
    return FindRec(a, n, k, nullptr) != nullptr;
}

void NodeClearStr(Arena* a, Node* n, NodeStrKind k) {
    char* prev = nullptr;
    char* rec = FindRec(a, n, k, &prev);
    if (!rec) {
        return;
    }

    if (prev) {
        RecSetNext(prev, RecNext(rec));
    } else {
        n->firstStr = RecNext(rec);
    }
}

void NodeSetStr(Arena* a, Node* n, NodeStrKind k, Str s) {
    if (!a || !n) {
        return;
    }
    NodeClearStr(a, n, k);
    if (!s.s || len(s) <= 0) {
        return;
    }
    int32_t head = 0;
    char* rec = RecNew(a, n, k, (uint32_t)len(s), &head);
    if (rec) {
        memcpy(rec + head, s.s, (size_t)len(s));
    }
}

void NodeGrowStr(Arena* a, Node* n, NodeStrKind k, Str more) {
    if (!a || !n || !more.s || len(more) <= 0) {
        return;
    }
    char* prev = nullptr;
    char* rec = FindRec(a, n, k, &prev);
    if (!rec) {
        NodeSetStr(a, n, k, more);
        return;
    }

    int32_t head = 0;
    Str had = RecStr(rec, &head);
    uint32_t nlen = (uint32_t)len(had) + (uint32_t)len(more);
    int32_t nhead = kRecLen + base::VarintSize(nlen);

    uint64_t used = base::ArenaUsed(a);
    uint64_t end = (uint64_t)base::ArenaOffsetOf(a, rec) + (uint64_t)head +
                   (uint64_t)len(had) + 1;

    bool newest = end == used;
    uint64_t want = newest ? (uint64_t)(nhead - head) + (uint64_t)len(more)
                           : (uint64_t)nhead + nlen + 1;
    char* dst = (char*)a->Push(want, 1, false);
    if (!dst) {
        return;
    }
    uint64_t at = base::ArenaOffsetOf(a, dst);

    if (newest && at == used) {
        if (nhead != head) {
            memmove(rec + nhead, rec + head, (size_t)len(had));
        }
        base::VarintPut(rec + kRecLen, nlen);
        memcpy(rec + nhead + len(had), more.s, (size_t)len(more));
        rec[nhead + nlen] = 0;
        return;
    }

    dst[kRecKind] = (char)(uint8_t)k;
    base::VarintPut(dst + kRecLen, nlen);
    memcpy(dst + nhead, had.s, (size_t)len(had));
    memcpy(dst + nhead + len(had), more.s, (size_t)len(more));
    dst[nhead + nlen] = 0;
    if (prev) {
        RecSetNext(prev, RecNext(rec));
    } else {
        n->firstStr = RecNext(rec);
    }
    RecSetNext(dst, n->firstStr);
    n->firstStr = (ArenaStr)at;
}

uint32_t NodePerKind(Arena* a, const Node* n) {
    char* rec = FindRec(a, n, NodeStrKind::PerKind, nullptr);
    if (!rec) {
        return 0;
    }
    uint32_t word = 0;
    Str bytes = RecStr(rec);
    base::VarintGet(bytes.s, &word);
    return word;
}

void NodeSetPerKind(Arena* a, Node* n, uint32_t word) {
    base::TempStr buf = base::AllocStrTemp(8);
    buf.len = base::VarintPut(buf.s, word);
    NodeSetStr(a, n, NodeStrKind::PerKind, buf);
}

static uint8_t* AlignAt(Arena* a, ArenaAlign al, int32_t* count) {
    *count = 0;
    if (al == kArenaAlignNone) {
        return nullptr;
    }
    char* p = (char*)base::ArenaAtOffset(a, al);
    if (!p) {
        return nullptr;
    }
    uint32_t n = 0;
    int head = base::VarintGet(p, &n);
    *count = (int32_t)n;
    return (uint8_t*)p + head;
}

ArenaAlign ArenaAlignNew(Arena* a, int32_t count) {
    if (!a || count <= 0) {
        return kArenaAlignNone;
    }
    int32_t head = base::VarintSize((uint32_t)count);
    int32_t bytes = (count + 3) / 4;

    char* mem = (char*)a->Push((uint64_t)head + (uint64_t)bytes, 1, false);
    if (!mem) {
        return kArenaAlignNone;
    }
    memset(mem, 0, (size_t)head + (size_t)bytes);
    base::VarintPut(mem, (uint32_t)count);
    return (ArenaAlign)base::ArenaOffsetOf(a, mem);
}

int32_t ArenaAlignCount(Arena* a, ArenaAlign al) {
    int32_t count = 0;
    AlignAt(a, al, &count);
    return count;
}

AlignKind ArenaAlignAt(Arena* a, ArenaAlign al, int32_t i) {
    int32_t count = 0;
    uint8_t* bits = AlignAt(a, al, &count);
    if (!bits || i < 0 || i >= count) {
        return AlignKind::None;
    }
    return (AlignKind)((bits[i / 4] >> ((i % 4) * 2)) & 3);
}

void ArenaAlignSet(Arena* a, ArenaAlign al, int32_t i, AlignKind k) {
    int32_t count = 0;
    uint8_t* bits = AlignAt(a, al, &count);
    if (!bits || i < 0 || i >= count) {
        return;
    }
    int32_t shift = (i % 4) * 2;
    uint8_t was = (uint8_t)(bits[i / 4] & ~(3 << shift));
    bits[i / 4] = (uint8_t)(was | (((uint8_t)k & 3) << shift));
}

UnistPosition GetUnistPosition(Str md, uint32_t start, uint32_t end) {
    UnistPosition out;
    int32_t line = 1;
    int32_t column = 1;
    int32_t at = 0;
    int32_t stop = (int32_t)end;
    if (!md.s) {
        return out;
    }
    if (stop > len(md)) {
        stop = len(md);
    }
    bool haveStart = false;
    while (at <= stop) {
        if (!haveStart && at == (int32_t)start) {
            out.start = UnistPoint{line, column, at};
            haveStart = true;
        }
        if (at == stop) {
            break;
        }
        uint8_t byte = (uint8_t)md.s[at];
        if (byte == '\r' && at + 1 < len(md) && md.s[at + 1] == '\n') {

            at += 1;
            continue;
        }
        if (byte == '\n' || byte == '\r') {
            line += 1;
            column = 1;
            at += 1;
            continue;
        }
        if (byte == '\t') {
            int32_t remainder = column % kTabSize;
            column += remainder == 0 ? 1 : 1 + kTabSize - remainder;
            at += 1;
            continue;
        }
        column += 1;
        at += 1;
    }
    if (!haveStart) {

        out.start = UnistPoint{line, column, at};
    }
    out.end = UnistPoint{line, column, at};
    return out;
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
