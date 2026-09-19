#define GPUI_INCLUDE_PRIVATE_API 1
#include "html5ever.h"

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

static bool base_IsDigit(char c) {
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
    while (*off < len(f) && base_IsDigit(f.s[*off])) {
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
    if (off < len(fmt.format) && base_IsDigit(fmt.format.s[off])) {
        n = parseUintAt(fmt.format, &off);
        positional = true;
    }
    while (off < len(fmt.format) && fmt.format.s[off] != '}') {
        if (!base_IsDigit(fmt.format.s[off])) {
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

#line 1 "src/html5ever/entities.cpp"

namespace html5ever {

using base::Str;
using base::StrCmp;

const char kNamedRefNames[] =
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

const char kNamedRefValues[] =
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

struct NamedRef {
    int32_t nameOff;
    int32_t valueOff;
};

const NamedRef kNamedRefs[2125] = {
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

Str NamedEntity(Str name) {
    int32_t lo = 0;
    int32_t hi = 2125 - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        const char* candidate = kNamedRefNames + kNamedRefs[mid].nameOff;
        int cmp = StrCmp(Str(candidate), name);
        if (cmp < 0) {
            lo = mid + 1;
        } else if (cmp > 0) {
            hi = mid - 1;
        } else {
            const char* value = kNamedRefValues + kNamedRefs[mid].valueOff;
            return Str(value);
        }
    }
    return {};
}

}

#line 1 "src/html5ever/html5ever.cpp"

namespace html5ever {

using namespace base;

static const char kVoidB[] = "base\0basefont\0bgsound\0br\0";
static const char kVoidI[] = "img\0input\0";
static const char kRawElements[] =
    "iframe\0noembed\0noframes\0script\0style\0xmp\0";
static const char kRcdataElements[] = "textarea\0title\0";
static const char kFormattingB[] = "b\0big\0";
static const char kFormattingS[] = "s\0small\0strike\0strong\0";
static const char kBlockA[] = "address\0article\0aside\0";
static const char kBlockD[] = "details\0dialog\0dir\0div\0dl\0";
static const char kBlockF[] = "fieldset\0figcaption\0figure\0footer\0form\0";
static const char kBlockH[] = "h1\0h2\0h3\0h4\0h5\0h6\0header\0hgroup\0hr\0";
static const char kBlockM[] = "main\0menu\0";
static const char kBlockP[] = "p\0pre\0";
static const char kBlockS[] = "search\0section\0summary\0";
static const char kHeadElements[] =
    "base\0basefont\0bgsound\0link\0meta\0noframes\0script\0style\0template\0"
    "title\0";
static const char kTableParts[] =
    "caption\0col\0colgroup\0tbody\0td\0tfoot\0th\0thead\0tr\0";

static bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool IsAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool html5ever_html5ever_IsDigit(char c) {
    return c >= '0' && c <= '9';
}

static bool IsNameChar(char c) {
    return IsAlpha(c) || html5ever_html5ever_IsDigit(c) || c == '-' || c == '_' || c == ':';
}

static bool IsVoid(Str name) {
    if (!name.s || len(name) == 0) return false;
    switch (name.s[0]) {
        case 'a':
            return StrEq(name, StrL("area"));
        case 'b':
            return SeqStrContainsI(kVoidB, name);
        case 'c':
            return StrEq(name, StrL("col"));
        case 'e':
            return StrEq(name, StrL("embed"));
        case 'f':
            return StrEq(name, StrL("frame"));
        case 'h':
            return StrEq(name, StrL("hr"));
        case 'i':
            return SeqStrContainsI(kVoidI, name);
        case 'k':
            return StrEq(name, StrL("keygen"));
        case 'l':
            return StrEq(name, StrL("link"));
        case 'm':
            return StrEq(name, StrL("meta"));
        case 'p':
            return StrEq(name, StrL("param"));
        case 's':
            return StrEq(name, StrL("source"));
        case 't':
            return StrEq(name, StrL("track"));
        case 'w':
            return StrEq(name, StrL("wbr"));
        default:
            return false;
    }
}

static bool IsFormatting(Str name) {
    if (!name.s || len(name) == 0) return false;
    switch (name.s[0]) {
        case 'a':
            return StrEq(name, StrL("a"));
        case 'b':
            return SeqStrContainsI(kFormattingB, name);
        case 'c':
            return StrEq(name, StrL("code"));
        case 'e':
            return StrEq(name, StrL("em"));
        case 'f':
            return StrEq(name, StrL("font"));
        case 'i':
            return StrEq(name, StrL("i"));
        case 'n':
            return StrEq(name, StrL("nobr"));
        case 's':
            return SeqStrContainsI(kFormattingS, name);
        case 't':
            return StrEq(name, StrL("tt"));
        case 'u':
            return StrEq(name, StrL("u"));
        default:
            return false;
    }
}

static ArenaStr LowerCopy(Arena* a, Str value) {
    ArenaStr result = ArenaStrDup(a, value);
    StrLowerAscii(ArenaStrGet(a, result).s);
    return result;
}

Str NamedEntity(Str name);

static uint32_t NumericEntity(Str value, int radix) {
    uint32_t cp = 0;
    bool any = false;
    for (int i = 0; i < len(value); i++) {
        char c = value.s[i];
        uint32_t digit = 0;
        if (html5ever_html5ever_IsDigit(c)) {
            digit = (uint32_t)(c - '0');
        } else if (radix == 16 && c >= 'a' && c <= 'f') {
            digit = (uint32_t)(c - 'a' + 10);
        } else if (radix == 16 && c >= 'A' && c <= 'F') {
            digit = (uint32_t)(c - 'A' + 10);
        } else {
            break;
        }
        any = true;
        if (cp > 0x10ffffu / (uint32_t)radix) return 0xfffd;
        cp = cp * (uint32_t)radix + digit;
    }
    if (!any || cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        return 0xfffd;
    }

    static const uint16_t controls[32] = {
        0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
        0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
        0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
        0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
    };
    if (cp >= 0x80 && cp <= 0x9f) cp = controls[cp - 0x80];
    return cp;
}

static int EncodeUtf8(char* out, uint32_t cp) {
    if (cp <= 0x7f) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7ff) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp <= 0xffff) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static void AppendCp(StrBuilder& out, uint32_t cp) {
    char bytes[4];
    int n = EncodeUtf8(bytes, cp);
    out.Append(Str(bytes, n));
}

static ArenaStr Decode(Arena* a, Str value, bool attribute) {
    bool needsDecode = false;
    for (int i = 0; i < len(value); i++) {
        if (value.s[i] == '&' || value.s[i] == '\r' || value.s[i] == 0) {
            needsDecode = true;
            break;
        }
    }
    if (!needsDecode) return ArenaStrDup(a, value);

    StrBuilder out(a);
    out.Reserve(len(value));
    for (int i = 0; i < len(value);) {
        if (value.s[i] != '&') {
            char c = value.s[i++] == '\r' ? '\n' : value.s[i - 1];
            if (c == '\n' && i < len(value) && value.s[i] == '\n' &&
                value.s[i - 1] == '\r') {
                i++;
            }
            if (c == 0) {
                AppendCp(out, 0xfffd);
            } else {
                out.AppendChar(c);
            }
            continue;
        }
        int start = i++;
        if (i < len(value) && value.s[i] == '#') {
            i++;
            int radix = 10;
            if (i < len(value) && (value.s[i] == 'x' || value.s[i] == 'X')) {
                radix = 16;
                i++;
            }
            int digits = i;
            while (
                i < len(value) &&
                (html5ever_html5ever_IsDigit(value.s[i]) ||
                 (radix == 16 && ((value.s[i] >= 'a' && value.s[i] <= 'f') ||
                                  (value.s[i] >= 'A' && value.s[i] <= 'F'))))) {
                i++;
            }
            if (digits == i) {
                out.AppendChar('&');
                i = start + 1;
                continue;
            }
            uint32_t cp =
                NumericEntity(Str(value.s + digits, i - digits), radix);
            if (i < len(value) && value.s[i] == ';') i++;
            AppendCp(out, cp);
            continue;
        }
        int end = i;
        while (end < len(value) && end - i < 31 &&
               (IsAlpha(value.s[end]) || html5ever_html5ever_IsDigit(value.s[end]))) {
            end++;
        }
        int matched = -1;
        Str decoded = {};
        for (int n = end - i; n > 0; n--) {
            decoded = NamedEntity(Str(value.s + i, n));
            if (decoded.s) {
                matched = n;
                break;
            }
        }
        bool semi = matched > 0 && i + matched < len(value) &&
                    value.s[i + matched] == ';';
        if (matched < 0 ||
            (attribute && !semi && i + matched < len(value) &&
             (html5ever_html5ever_IsDigit(value.s[i + matched]) || IsAlpha(value.s[i + matched]) ||
              value.s[i + matched] == '='))) {
            out.AppendChar('&');
            i = start + 1;
            continue;
        }
        out.Append(decoded);
        i += matched + (semi ? 1 : 0);
    }
    return ArenaStrDup(a, out.TakeStr());
}

struct Scanner {
    Arena* a = nullptr;
    Str source = {};
    int at = 0;
    int line = 1;
    TokenSink sink = nullptr;
    void* user = nullptr;
    TokenizerOptions options = {};
    Str rawName = {};
    bool rcdata = false;
    bool finishing = true;
    bool needMore = false;
    bool* paused = nullptr;
};

static void Emit(Scanner* s, const Token& token) {
    if (s->sink) s->sink(s->user, &token);
}

static bool NeedMore(Scanner* s, int tokenStart) {
    if (s->finishing) {
        return false;
    }
    s->at = tokenStart;
    s->needMore = true;
    return true;
}

static void Error(Scanner* s, const char* message) {
    if (!s->options.exactErrors) return;
    Token token;
    token.kind = TokenKind::ParseError;
    token.data = ArenaStrDup(s->a, Str((char*)message));
    token.line = s->line;
    Emit(s, token);
}

static void SkipSpace(Scanner* s) {
    while (s->at < len(s->source) && IsSpace(s->source.s[s->at])) {
        if (s->source.s[s->at++] == '\n') s->line++;
    }
}

static ArenaStr ScanName(Scanner* s) {
    int start = s->at;
    while (s->at < len(s->source) && IsNameChar(s->source.s[s->at])) s->at++;
    return LowerCopy(s->a, Str(s->source.s + start, s->at - start));
}

static Attribute* ScanAttrs(Scanner* s, bool* selfClosing) {
    Attribute* first = nullptr;
    Attribute* last = nullptr;
    *selfClosing = false;
    for (;;) {
        SkipSpace(s);
        if (s->at >= len(s->source)) return first;
        char c = s->source.s[s->at];
        if (c == '>') {
            s->at++;
            return first;
        }
        if (c == '/' && s->at + 1 < len(s->source) &&
            s->source.s[s->at + 1] == '>') {
            s->at += 2;
            *selfClosing = true;
            return first;
        }
        int nameStart = s->at;
        while (s->at < len(s->source) && !IsSpace(s->source.s[s->at]) &&
               s->source.s[s->at] != '=' && s->source.s[s->at] != '>' &&
               s->source.s[s->at] != '/') {
            s->at++;
        }
        if (nameStart == s->at) {
            Error(s, "unexpected byte in tag");
            s->at++;
            continue;
        }
        ArenaStr name =
            LowerCopy(s->a, Str(s->source.s + nameStart, s->at - nameStart));
        SkipSpace(s);
        ArenaStr value = {};
        if (s->at < len(s->source) && s->source.s[s->at] == '=') {
            s->at++;
            SkipSpace(s);
            int start = s->at;
            if (s->at < len(s->source) &&
                (s->source.s[s->at] == '\'' || s->source.s[s->at] == '"')) {
                char quote = s->source.s[s->at++];
                start = s->at;
                while (s->at < len(s->source) && s->source.s[s->at] != quote) {
                    if (s->source.s[s->at++] == '\n') s->line++;
                }
                value =
                    Decode(s->a, Str(s->source.s + start, s->at - start), true);
                if (s->at < len(s->source)) s->at++;
            } else {
                while (s->at < len(s->source) && !IsSpace(s->source.s[s->at]) &&
                       s->source.s[s->at] != '>') {
                    s->at++;
                }
                value =
                    Decode(s->a, Str(s->source.s + start, s->at - start), true);
            }
        }
        bool duplicate = false;
        for (Attribute* at = first; at; at = AttributeNext(s->a, at)) {
            if (StrEq(AttributeName(s->a, at), ArenaStrGet(s->a, name))) {
                duplicate = true;
            }
        }
        if (duplicate) {
            Error(s, "duplicate attribute");
            continue;
        }
        Attribute* attr = ArenaNew<Attribute>(s->a);
        attr->name = name;
        attr->value = value;
        if (last)
            last->next = ArenaPtrOf(s->a, attr);
        else
            first = attr;
        last = attr;
    }
}

static int FindRawClose(const Scanner* s) {
    for (int i = s->at; i + 2 + len(s->rawName) <= len(s->source); i++) {
        if (s->source.s[i] != '<' || s->source.s[i + 1] != '/') continue;
        if (!StrEqI(Str(s->source.s + i + 2, len(s->rawName)), s->rawName)) {
            continue;
        }
        int end = i + 2 + len(s->rawName);
        if (end >= len(s->source) || IsSpace(s->source.s[end]) ||
            s->source.s[end] == '>') {
            return i;
        }
    }
    return len(s->source);
}

static void TokenizeRun(Scanner* s) {
    if (s->at == 0 && s->options.discardBom && len(s->source) >= 3 &&
        (uint8_t)s->source.s[0] == 0xef && (uint8_t)s->source.s[1] == 0xbb &&
        (uint8_t)s->source.s[2] == 0xbf) {
        s->at = 3;
    }
    s->needMore = false;
    while (s->at < len(s->source)) {
        int tokenStart = s->at;
        if (s->rawName.s) {
            int end = FindRawClose(s);
            if (end >= len(s->source) && !s->finishing) {
                s->needMore = true;
                return;
            }
            if (end > s->at) {
                Token text;
                text.kind = TokenKind::Character;
                Str raw(s->source.s + s->at, end - s->at);
                text.data = s->rcdata ? Decode(s->a, raw, false)
                                      : ArenaStrDup(s->a, raw);
                text.line = s->line;
                for (int i = s->at; i < end; i++) {
                    if (s->source.s[i] == '\n') s->line++;
                }
                s->at = end;
                Emit(s, text);
                if (s->paused && *s->paused) return;
                continue;
            }
            s->rawName = {};
            s->rcdata = false;
        }
        if (s->source.s[s->at] != '<') {
            int start = s->at;
            while (s->at < len(s->source) && s->source.s[s->at] != '<') {
                if (s->source.s[s->at++] == '\n') s->line++;
            }
            if (s->at >= len(s->source) && !s->finishing &&
                start < len(s->source) &&
                s->source.s[len(s->source) - 1] == '&') {
                if (NeedMore(s, start)) return;
            }
            Token text;
            text.kind = TokenKind::Character;
            text.data =
                Decode(s->a, Str(s->source.s + start, s->at - start), false);
            text.line = s->line;
            Emit(s, text);
            if (s->paused && *s->paused) return;
            continue;
        }
        if (!s->finishing && s->at + 1 >= len(s->source)) {
            if (NeedMore(s, tokenStart)) return;
        }
        int tokenLine = s->line;
        if (s->at + 3 < len(s->source) &&
            StrEq(Str(s->source.s + s->at, 4), StrL("<!--"))) {
            s->at += 4;
            int start = s->at;
            while (s->at + 2 < len(s->source) &&
                   !(s->source.s[s->at] == '-' &&
                     s->source.s[s->at + 1] == '-' &&
                     s->source.s[s->at + 2] == '>')) {
                if (s->source.s[s->at++] == '\n') s->line++;
            }
            Token comment;
            comment.kind = TokenKind::Comment;
            comment.data =
                ArenaStrDup(s->a, Str(s->source.s + start, s->at - start));
            comment.line = tokenLine;
            if (s->at + 2 >= len(s->source) && !s->finishing) {
                if (NeedMore(s, tokenStart)) return;
            }
            if (s->at + 2 < len(s->source))
                s->at += 3;
            else
                Error(s, "eof in comment");
            Emit(s, comment);
            if (s->paused && *s->paused) return;
            continue;
        }
        if (!s->finishing && s->at + 3 >= len(s->source) &&
            s->source.s[s->at] == '<' && s->source.s[s->at + 1] == '!') {
            if (NeedMore(s, tokenStart)) return;
        }
        if (s->at + 2 < len(s->source) && s->source.s[s->at + 1] == '!') {
            int start = s->at + 2;
            s->at = start;
            while (s->at < len(s->source) && s->source.s[s->at] != '>') {
                s->at++;
            }
            Str body = StrTrimAscii(Str(s->source.s + start, s->at - start));
            if (s->at < len(s->source)) s->at++;
            Token token;
            token.kind = TokenKind::Doctype;
            token.line = tokenLine;
            if (StrStartsWithI(body, StrL("doctype"))) {
                body = StrTrimAscii(Str(body.s + 7, len(body) - 7));
                int n = 0;
                while (n < len(body) && !IsSpace(body.s[n])) n++;
                token.name = LowerCopy(s->a, Str(body.s, n));
                token.forceQuirks =
                    !StrEqI(TokenName(s->a, &token), StrL("html"));
            } else {
                token.kind = TokenKind::Comment;
                token.data = ArenaStrDup(s->a, body);
            }
            Emit(s, token);
            if (s->paused && *s->paused) return;
            continue;
        }
        if (s->at + 1 < len(s->source) && s->source.s[s->at + 1] == '/') {
            s->at += 2;
            SkipSpace(s);
            Token token;
            token.kind = TokenKind::EndTag;
            token.name = ScanName(s);
            token.line = tokenLine;
            while (s->at < len(s->source) && s->source.s[s->at] != '>') s->at++;
            if (s->at >= len(s->source) && !s->finishing) {
                if (NeedMore(s, tokenStart)) return;
            }
            if (s->at < len(s->source)) s->at++;
            Emit(s, token);
            if (s->paused && *s->paused) return;
            continue;
        }
        if (s->at + 1 < len(s->source) && IsAlpha(s->source.s[s->at + 1])) {
            s->at++;
            Token token;
            token.kind = TokenKind::StartTag;
            token.name = ScanName(s);
            token.line = tokenLine;
            token.attrs = ArenaPtrOf(s->a, ScanAttrs(s, &token.selfClosing));
            if (s->at >= len(s->source) && !s->finishing) {
                if (NeedMore(s, tokenStart)) return;
            }
            Emit(s, token);
            if (s->paused && *s->paused) return;
            Str name = TokenName(s->a, &token);
            if (!token.selfClosing &&
                (SeqStrContainsI(kRawElements, name) ||
                 SeqStrContainsI(kRcdataElements, name))) {
                s->rawName = name;
                s->rcdata = SeqStrContainsI(kRcdataElements, name);
            }
            continue;
        }
        Token text;
        text.kind = TokenKind::Character;
        text.data = ArenaStrDup(s->a, Str(s->source.s + s->at, 1));
        text.line = tokenLine;
        s->at++;
        Emit(s, text);
        if (s->paused && *s->paused) return;
    }
    if (!s->finishing) {
        return;
    }
    Token eof;
    eof.kind = TokenKind::Eof;
    eof.line = s->line;
    Emit(s, eof);
}

void Tokenize(Arena* a, Str source, TokenSink sink, void* user,
              TokenizerOptions options) {
    if (!a || !sink) return;
    Scanner scanner;
    scanner.a = a;
    scanner.source = source;
    scanner.sink = sink;
    scanner.user = user;
    scanner.options = options;
    TokenizeRun(&scanner);
}

static Node* NewNode(Arena* a, NodeKind kind, Str name = {}) {
    Node* node = ArenaNew<Node>(a);
    node->kind = kind;
    node->name = ArenaStrDup(a, name);
    return node;
}

static void Append(Arena* a, Node* parent, Node* child) {
    child->parent = ArenaPtrOf(a, parent);
    child->next = {};
    Node* last = NodeLast(a, parent);
    if (last)
        last->next = ArenaPtrOf(a, child);
    else
        parent->first = ArenaPtrOf(a, child);
    parent->last = ArenaPtrOf(a, child);
}

static void InsertBefore(Arena* a, Node* before, Node* child) {
    Node* parent = NodeParent(a, before);
    if (!parent) return;
    child->parent = ArenaPtrOf(a, parent);
    if (NodeFirst(a, parent) == before) {
        child->next = ArenaPtrOf(a, before);
        parent->first = ArenaPtrOf(a, child);
        return;
    }
    Node* prev = NodeFirst(a, parent);
    while (prev && NodeNext(a, prev) != before) prev = NodeNext(a, prev);
    if (!prev) return;
    prev->next = ArenaPtrOf(a, child);
    child->next = ArenaPtrOf(a, before);
}

static Attribute* CloneAttrs(Arena* a, const Attribute* attrs) {
    Attribute* first = nullptr;
    Attribute* last = nullptr;
    for (; attrs; attrs = AttributeNext(a, attrs)) {
        Attribute* copy = ArenaNew<Attribute>(a);
        *copy = *attrs;
        copy->next = {};
        if (last)
            last->next = ArenaPtrOf(a, copy);
        else
            first = copy;
        last = copy;
    }
    return first;
}

struct Builder {
    Arena* a = nullptr;
    ParseOptions options = {};
    Node* doc = nullptr;
    Node* html = nullptr;
    Node* head = nullptr;
    Node* body = nullptr;
    ArenaVec<Node*> open{};
    bool fragment = false;
    Str context = {};
    bool paused = false;
    bool pauseOnScript = false;
};

static Node* Current(Builder* b) {
    return b->open.len ? b->open[b->open.len - 1] : b->doc;
}

static int OpenIndex(Builder* b, Str name) {
    for (int i = b->open.len - 1; i >= 0; i--) {
        if (StrEq(NodeName(b->a, b->open[i]), name)) return i;
    }
    return -1;
}

static bool HasOpen(Builder* b, Str name) {
    return OpenIndex(b, name) >= 0;
}

static Node* Element(Builder* b, Str name, const Attribute* attrs,
                     Namespace ns = Namespace::Html) {
    Node* node = NewNode(b->a, NodeKind::Element, name);
    node->attrs = ArenaPtrOf(b->a, CloneAttrs(b->a, attrs));
    node->ns = ns;
    return node;
}

static Node* ElementFromToken(Builder* b, const Token* token,
                              Namespace ns = Namespace::Html) {
    Node* node = ArenaNew<Node>(b->a);
    node->kind = NodeKind::Element;
    node->name = token->name;
    node->attrs = token->attrs;
    node->ns = ns;
    return node;
}

static Node* EnsureWrapper(Builder* b, Node** slot, Str name, Node* parent) {
    if (*slot) return *slot;
    *slot = Element(b, name, nullptr);
    (*slot)->implicit = true;
    Append(b->a, parent, *slot);
    return *slot;
}

static Node* Body(Builder* b) {
    if (b->fragment) return b->doc;
    if (b->body) return b->body;
    EnsureWrapper(b, &b->html, StrL("html"), b->doc);
    EnsureWrapper(b, &b->head, StrL("head"), b->html);
    return EnsureWrapper(b, &b->body, StrL("body"), b->html);
}

static bool AllSpace(Str value) {
    for (int i = 0; i < len(value); i++) {
        if (!IsSpace(value.s[i])) return false;
    }
    return true;
}

static Node* TableInScope(Builder* b) {
    for (int i = b->open.len - 1; i >= 0; i--) {
        if (StrEq(NodeName(b->a, b->open[i]), StrL("table"))) {
            return b->open[i];
        }
    }
    return nullptr;
}

static bool TableAllows(Str parent, Str child) {
    if (StrEq(parent, StrL("table"))) {
        return SeqStrContainsI(kTableParts, child) ||
               StrEq(child, StrL("style")) || StrEq(child, StrL("script")) ||
               StrEq(child, StrL("template"));
    }
    if (StrEq(parent, StrL("tbody")) || StrEq(parent, StrL("thead")) ||
        StrEq(parent, StrL("tfoot"))) {
        return StrEq(child, StrL("tr"));
    }
    if (StrEq(parent, StrL("tr"))) {
        return StrEq(child, StrL("td")) || StrEq(child, StrL("th"));
    }
    return true;
}

static Node* InsertionParent(Builder* b, Str child, bool textIsSpace,
                             Node** tableOut, bool* fosterOut) {
    Node* current = Current(b);
    Node* table = TableInScope(b);
    if (tableOut) *tableOut = table;
    bool foster =
        table && !textIsSpace && !TableAllows(NodeName(b->a, current), child);
    if (fosterOut) *fosterOut = foster;
    if (foster) {
        Node* tableParent = NodeParent(b->a, table);
        return tableParent ? tableParent : current;
    }
    return current == b->doc ? Body(b) : current;
}

static void AppendText(Builder* b, ArenaStr stored) {
    Str data = ArenaStrGet(b->a, stored);
    if (len(data) <= 0) return;
    Node* table = nullptr;
    bool foster = false;
    Node* parent = InsertionParent(b, {}, AllSpace(data), &table, &foster);
    if (foster) {
        Node* text = NewNode(b->a, NodeKind::Text);
        text->data = stored;
        InsertBefore(b->a, table, text);
    } else {
        Node* last = NodeLast(b->a, parent);
        if (last && last->kind == NodeKind::Text) {
            last->data = ArenaStrAppend(b->a, last->data, data);
        } else {
            Node* text = NewNode(b->a, NodeKind::Text);
            text->data = stored;
            Append(b->a, parent, text);
        }
    }
}

static bool ClosesP(Str name) {
    if (!name.s || len(name) == 0) return false;
    char first = name.s[0];
    if (first >= 'A' && first <= 'Z') first = (char)(first + ('a' - 'A'));
    switch (first) {
        case 'a':
            return SeqStrContainsI(kBlockA, name);
        case 'b':
            return StrEq(name, StrL("blockquote"));
        case 'c':
            return StrEq(name, StrL("center"));
        case 'd':
            return SeqStrContainsI(kBlockD, name);
        case 'f':
            return SeqStrContainsI(kBlockF, name);
        case 'h':
            return SeqStrContainsI(kBlockH, name);
        case 'l':
            return StrEq(name, StrL("listing"));
        case 'm':
            return SeqStrContainsI(kBlockM, name);
        case 'n':
            return StrEq(name, StrL("nav"));
        case 'o':
            return StrEq(name, StrL("ol"));
        case 'p':
            return SeqStrContainsI(kBlockP, name);
        case 's':
            return SeqStrContainsI(kBlockS, name);
        case 't':
            return StrEq(name, StrL("table"));
        case 'u':
            return StrEq(name, StrL("ul"));
        default:
            return false;
    }
}

static void CloseNamed(Builder* b, Str name) {
    int at = OpenIndex(b, name);
    if (at >= 0) b->open.Truncate(at);
}

static void CloseImplied(Builder* b, Str name) {
    if (ClosesP(name)) CloseNamed(b, StrL("p"));
    if (StrEq(name, StrL("li"))) {
        int at = OpenIndex(b, StrL("li"));
        if (at >= 0) b->open.Truncate(at);
    }
    if (StrEq(name, StrL("dt")) || StrEq(name, StrL("dd"))) {
        int dt = OpenIndex(b, StrL("dt"));
        int dd = OpenIndex(b, StrL("dd"));
        int at = dt > dd ? dt : dd;
        if (at >= 0) b->open.Truncate(at);
    }
    if (StrEq(name, StrL("tr"))) {
        int at = OpenIndex(b, StrL("tr"));
        if (at >= 0) b->open.Truncate(at);
    }
    if (StrEq(name, StrL("td")) || StrEq(name, StrL("th"))) {
        int td = OpenIndex(b, StrL("td"));
        int th = OpenIndex(b, StrL("th"));
        int at = td > th ? td : th;
        if (at >= 0) b->open.Truncate(at);
    }
    if (len(name) == 2 && name.s[0] == 'h' && name.s[1] >= '1' &&
        name.s[1] <= '6') {
        for (int i = b->open.len - 1; i >= 0; i--) {
            Str n = NodeName(b->a, b->open[i]);
            if (len(n) == 2 && n.s[0] == 'h' && n.s[1] >= '1' &&
                n.s[1] <= '6') {
                b->open.Truncate(i);
                break;
            }
        }
    }
}

static void MergeAttrs(Arena* a, Node* node, const Attribute* attrs) {
    for (; attrs; attrs = AttributeNext(a, attrs)) {
        bool exists = false;
        for (Attribute* at = NodeAttrs(a, node); at;
             at = AttributeNext(a, at)) {
            if (StrEq(AttributeName(a, at), AttributeName(a, attrs))) {
                exists = true;
            }
        }
        if (exists) continue;
        Attribute* copy = ArenaNew<Attribute>(a);
        *copy = *attrs;
        copy->next = node->attrs;
        node->attrs = ArenaPtrOf(a, copy);
    }
}

static Node* PushElement(Builder* b, const Token* token,
                         Namespace ns = Namespace::Html) {
    Str name = TokenName(b->a, token);
    Node* table = nullptr;
    bool foster = false;
    Node* parent = InsertionParent(b, name, false, &table, &foster);
    Node* node = ElementFromToken(b, token, ns);
    if (foster) {
        InsertBefore(b->a, table, node);
    } else {
        Append(b->a, parent, node);
    }
    if (!token->selfClosing && !IsVoid(name)) {
        b->open.Append(b->a, node);
    }
    return node;
}

static void StartTag(Builder* b, const Token* token) {
    Str name = TokenName(b->a, token);
    if (!b->fragment && StrEq(name, StrL("html"))) {
        Node* html = EnsureWrapper(b, &b->html, StrL("html"), b->doc);
        html->implicit = false;
        MergeAttrs(b->a, html, TokenAttrs(b->a, token));
        if (b->open.len == 0) b->open.Append(b->a, html);
        return;
    }
    if (!b->fragment && StrEq(name, StrL("head"))) {
        EnsureWrapper(b, &b->html, StrL("html"), b->doc);
        Node* head = EnsureWrapper(b, &b->head, StrL("head"), b->html);
        head->implicit = false;
        MergeAttrs(b->a, head, TokenAttrs(b->a, token));
        if (!HasOpen(b, StrL("head"))) b->open.Append(b->a, head);
        return;
    }
    if (!b->fragment && StrEq(name, StrL("body"))) {
        Body(b)->implicit = false;
        MergeAttrs(b->a, b->body, TokenAttrs(b->a, token));
        while (b->open.len && b->open[b->open.len - 1] != b->html)
            b->open.Pop();
        if (!HasOpen(b, StrL("html"))) b->open.Append(b->a, b->html);
        b->open.Append(b->a, b->body);
        return;
    }
    if (!b->fragment && !b->body && SeqStrContainsI(kHeadElements, name)) {
        EnsureWrapper(b, &b->html, StrL("html"), b->doc);
        Node* head = EnsureWrapper(b, &b->head, StrL("head"), b->html);
        Node* node = ElementFromToken(b, token);
        Append(b->a, head, node);
        if (!token->selfClosing && !IsVoid(name)) {
            b->open.Append(b->a, node);
        }
        return;
    }
    Body(b);
    if (!b->fragment && b->open.len == 0) {
        b->open.Append(b->a, b->html);
        b->open.Append(b->a, b->body);
    } else if (!b->fragment && Current(b) == b->html) {
        b->open.Append(b->a, b->body);
    }

    CloseImplied(b, name);
    if (StrEq(name, StrL("tr")) &&
        StrEq(NodeName(b->a, Current(b)), StrL("table"))) {
        Node* tbody = Element(b, StrL("tbody"), nullptr);
        tbody->implicit = true;
        Append(b->a, Current(b), tbody);
        b->open.Append(b->a, tbody);
    } else if ((StrEq(name, StrL("td")) || StrEq(name, StrL("th"))) &&
               StrEq(NodeName(b->a, Current(b)), StrL("table"))) {
        Node* tbody = Element(b, StrL("tbody"), nullptr);
        tbody->implicit = true;
        Append(b->a, Current(b), tbody);
        b->open.Append(b->a, tbody);
        Node* tr = Element(b, StrL("tr"), nullptr);
        tr->implicit = true;
        Append(b->a, Current(b), tr);
        b->open.Append(b->a, tr);
    } else if ((StrEq(name, StrL("td")) || StrEq(name, StrL("th"))) &&
               (StrEq(NodeName(b->a, Current(b)), StrL("tbody")) ||
                StrEq(NodeName(b->a, Current(b)), StrL("thead")) ||
                StrEq(NodeName(b->a, Current(b)), StrL("tfoot")))) {
        Node* tr = Element(b, StrL("tr"), nullptr);
        tr->implicit = true;
        Append(b->a, Current(b), tr);
        b->open.Append(b->a, tr);
    }
    Namespace ns = Current(b)->ns;
    if (StrEq(name, StrL("svg")))
        ns = Namespace::Svg;
    else if (StrEq(name, StrL("math")))
        ns = Namespace::MathMl;
    PushElement(b, token, ns);
}

static void EndFormatting(Builder* b, Str name) {
    int at = OpenIndex(b, name);
    if (at < 0) return;
    ArenaVec<Node*> reopen;
    for (int i = at + 1; i < b->open.len; i++) {
        if (IsFormatting(NodeName(b->a, b->open[i]))) {
            reopen.Append(b->a, b->open[i]);
        }
    }
    Node* parent = NodeParent(b->a, b->open[at]);
    b->open.Truncate(at);
    for (int i = 0; i < len(reopen); i++) {
        Node* old = reopen[i];
        Node* node =
            Element(b, NodeName(b->a, old), NodeAttrs(b->a, old), old->ns);
        Append(b->a, parent, node);
        b->open.Append(b->a, node);
        parent = node;
    }
}

static void EndTag(Builder* b, const Token* token) {
    Str name = TokenName(b->a, token);
    if (StrEq(name, StrL("head"))) {
        CloseNamed(b, StrL("head"));
        return;
    }
    if (StrEq(name, StrL("body")) || StrEq(name, StrL("html"))) {
        while (b->open.len && b->open[b->open.len - 1] != b->html)
            b->open.Pop();
        return;
    }
    if (IsFormatting(name)) {
        EndFormatting(b, name);
        return;
    }
    int at = OpenIndex(b, name);
    if (at >= 0) b->open.Truncate(at);
    if (b->pauseOnScript && b->options.scriptingEnabled &&
        StrEq(name, StrL("script"))) {
        b->paused = true;
    }
}

static void BuildToken(void* user, const Token* token) {
    Builder* b = (Builder*)user;
    switch (token->kind) {
        case TokenKind::Character:
        case TokenKind::NullCharacter:
            AppendText(b, token->data);
            break;
        case TokenKind::Comment: {
            Node* comment = NewNode(b->a, NodeKind::Comment);
            comment->data = token->data;
            Append(b->a, Current(b), comment);
            break;
        }
        case TokenKind::Doctype:
            if (!b->options.dropDoctype && !b->fragment) {
                Node* node =
                    NewNode(b->a, NodeKind::Doctype, TokenName(b->a, token));
                node->data = token->data;
                node->systemId = token->systemId;
                Append(b->a, b->doc, node);
            }
            break;
        case TokenKind::StartTag:
            StartTag(b, token);
            break;
        case TokenKind::EndTag:
            EndTag(b, token);
            break;
        default:
            break;
    }
}

static Node* Parse(Arena* a, Str source, Str context, ParseOptions options,
                   bool fragment) {
    if (!a) return nullptr;
    Builder builder;
    builder.a = a;
    builder.options = options;
    builder.fragment = fragment;
    builder.context = context;
    builder.doc = NewNode(a, NodeKind::Document);
    if (fragment) {
        builder.doc->name =
            context.s ? LowerCopy(a, context) : ArenaStrDup(a, StrL("body"));
        builder.doc->ns = StrEqI(context, StrL("svg"))    ? Namespace::Svg
                          : StrEqI(context, StrL("math")) ? Namespace::MathMl
                                                          : Namespace::Html;
    }
    TokenizerOptions tokenizer = options.tokenizer;
    tokenizer.exactErrors = tokenizer.exactErrors || options.exactErrors;
    if (fragment && (SeqStrContainsI(kRawElements, context) ||
                     SeqStrContainsI(kRcdataElements, context))) {
        Scanner scanner;
        scanner.a = a;
        scanner.source = source;
        scanner.sink = BuildToken;
        scanner.user = &builder;
        scanner.options = tokenizer;
        scanner.rawName = context;
        scanner.rcdata = SeqStrContainsI(kRcdataElements, context);
        TokenizeRun(&scanner);
    } else {
        Tokenize(a, source, BuildToken, &builder, tokenizer);
    }
    if (!fragment) Body(&builder);
    return builder.doc;
}

Node* ParseDocument(Arena* a, Str source, ParseOptions options) {
    return Parse(a, source, {}, options, false);
}

Node* ParseFragment(Arena* a, Str source, Str context, ParseOptions options) {
    return Parse(a, source, context, options, true);
}

const Attribute* Attr(Arena* a, const Node* node, Str name) {
    if (!node) return nullptr;
    for (const Attribute* attr = NodeAttrs(a, node); attr;
         attr = AttributeNext(a, attr)) {
        if (StrEqI(AttributeName(a, attr), name)) return attr;
    }
    return nullptr;
}

Str AttrValue(Arena* a, const Node* node, Str name) {
    return AttributeValue(a, Attr(a, node, name));
}

static void WriteEscaped(StrBuilder& out, Str value, bool attribute) {
    for (int i = 0; i < len(value); i++) {
        char c = value.s[i];
        if (c == '&')
            out.Append(StrL("&amp;"));
        else if (c == '<')
            out.Append(StrL("&lt;"));
        else if (c == '>' && !attribute)
            out.Append(StrL("&gt;"));
        else if (c == '"' && attribute)
            out.Append(StrL("&quot;"));
        else
            out.AppendChar(c);
    }
}

static void WriteNode(Arena* a, StrBuilder& out, const Node* node,
                      bool include) {
    if (!node) return;
    bool element = node->kind == NodeKind::Element;
    if (include) {
        if (node->kind == NodeKind::Text) {
            const Node* parent = NodeParent(a, node);
            if (parent && SeqStrContainsI(kRawElements, NodeName(a, parent)))
                out.Append(NodeData(a, node));
            else
                WriteEscaped(out, NodeData(a, node), false);
        } else if (node->kind == NodeKind::Comment) {
            out.Append(StrL("<!--"));
            out.Append(NodeData(a, node));
            out.Append(StrL("-->"));
        } else if (node->kind == NodeKind::Doctype) {
            out.Append(StrL("<!DOCTYPE "));
            out.Append(NodeName(a, node));
            out.AppendChar('>');
        } else if (element) {
            out.AppendChar('<');
            out.Append(NodeName(a, node));
            for (const Attribute* attr = NodeAttrs(a, node); attr;
                 attr = AttributeNext(a, attr)) {
                out.AppendChar(' ');
                out.Append(AttributeName(a, attr));
                out.Append(StrL("=\""));
                WriteEscaped(out, AttributeValue(a, attr), true);
                out.AppendChar('"');
            }
            out.AppendChar('>');
        }
    }
    if (node->kind != NodeKind::Text && node->kind != NodeKind::Comment &&
        node->kind != NodeKind::Doctype) {
        for (const Node* child = NodeFirst(a, node); child;
             child = NodeNext(a, child)) {
            WriteNode(a, out, child, true);
        }
    }
    if (include && element && !IsVoid(NodeName(a, node))) {
        out.Append(StrL("</"));
        out.Append(NodeName(a, node));
        out.AppendChar('>');
    }
}

Str Serialize(Arena* a, const Node* node, SerializeOptions options) {
    if (!a || !node) return {};
    StrBuilder out(a);
    WriteNode(a, out, node, options.includeNode);
    return out.TakeStr();
}

struct ParserImpl {
    Builder builder = {};
    Scanner scanner = {};
    char* buf = nullptr;
    int bufLen = 0;
    int bufCap = 0;
    bool fragment = false;
    Str context = {};
    bool started = false;
};

static void ParserBufAppend(ParserImpl* impl, Arena* a, Str chunk) {
    int n = len(chunk);
    if (n <= 0) {
        return;
    }
    int need = impl->bufLen + n;
    if (need > impl->bufCap) {
        int cap = impl->bufCap > 0 ? impl->bufCap * 2 : 256;
        while (cap < need) {
            cap *= 2;
        }
        char* fresh = (char*)a->Push((uint64_t)cap, 1, false);
        if (impl->bufLen > 0 && impl->buf) {
            memcpy(fresh, impl->buf, (size_t)impl->bufLen);
        }
        impl->buf = fresh;
        impl->bufCap = cap;
    }
    memcpy(impl->buf + impl->bufLen, chunk.s, (size_t)n);
    impl->bufLen += n;
}

static ParserImpl* ImplOf(Parser* parser) {
    return parser ? (ParserImpl*)parser->impl : nullptr;
}

static void ParserEnsure(Parser* parser) {
    ParserImpl* impl = ImplOf(parser);
    if (!impl || impl->started) {
        return;
    }
    impl->started = true;
    impl->builder.a = parser->a;
    impl->builder.options = parser->options;
    impl->builder.fragment = impl->fragment;
    impl->builder.context = impl->context;
    impl->builder.pauseOnScript = true;
    impl->builder.doc = NewNode(parser->a, NodeKind::Document);
    if (impl->fragment) {
        impl->builder.doc->name = impl->context.s
                                      ? LowerCopy(parser->a, impl->context)
                                      : ArenaStrDup(parser->a, StrL("body"));
        impl->builder.doc
            ->ns = StrEqI(impl->context, StrL("svg"))    ? Namespace::Svg
                   : StrEqI(impl->context, StrL("math")) ? Namespace::MathMl
                                                         : Namespace::Html;
    }
    impl->scanner.a = parser->a;
    impl->scanner.sink = BuildToken;
    impl->scanner.user = &impl->builder;
    impl->scanner.options = parser->options.tokenizer;
    impl->scanner.options.exactErrors =
        impl->scanner.options.exactErrors || parser->options.exactErrors;
    impl->scanner.finishing = false;
    impl->scanner.paused = &impl->builder.paused;
    if (impl->fragment && (SeqStrContainsI(kRawElements, impl->context) ||
                           SeqStrContainsI(kRcdataElements, impl->context))) {
        impl->scanner.rawName = impl->context;
        impl->scanner.rcdata = SeqStrContainsI(kRcdataElements, impl->context);
    }
}

static void ParserPump(Parser* parser, bool finishing) {
    ParserImpl* impl = ImplOf(parser);
    if (!impl) {
        return;
    }
    ParserEnsure(parser);
    impl->scanner.source = Str(impl->buf, impl->bufLen);
    impl->scanner.finishing = finishing;
    TokenizeRun(&impl->scanner);
}

Parser* ParserNew(Arena* a, ParseOptions options) {
    if (!a) {
        return nullptr;
    }
    Parser* parser = ArenaNew<Parser>(a);
    parser->a = a;
    parser->options = options;
    ParserImpl* impl = ArenaNew<ParserImpl>(a);
    parser->impl = impl;
    return parser;
}

Parser* ParserNewFragment(Arena* a, Str context, ParseOptions options) {
    Parser* parser = ParserNew(a, options);
    if (!parser) {
        return nullptr;
    }
    ParserImpl* impl = ImplOf(parser);
    impl->fragment = true;
    impl->context = context.s ? ArenaStrGet(a, ArenaStrDup(a, context)) : Str{};
    return parser;
}

void ParserProcess(Parser* parser, Str chunk) {
    ParserImpl* impl = ImplOf(parser);
    if (!impl || !parser->a) {
        return;
    }
    ParserBufAppend(impl, parser->a, chunk);
    if (impl->builder.paused) {
        return;
    }
    ParserPump(parser, false);
}

bool ParserIsPaused(const Parser* parser) {
    ParserImpl* impl = parser ? (ParserImpl*)parser->impl : nullptr;
    return impl && impl->builder.paused;
}

void ParserResumeAfterCurrentScript(Parser* parser) {
    ParserImpl* impl = ImplOf(parser);
    if (!impl) {
        return;
    }
    impl->builder.paused = false;
    ParserPump(parser, false);
}

Node* ParserFinish(Parser* parser) {
    ParserImpl* impl = ImplOf(parser);
    if (!impl) {
        return nullptr;
    }
    impl->builder.pauseOnScript = false;
    impl->builder.paused = false;
    ParserPump(parser, true);
    if (!impl->fragment) {
        Body(&impl->builder);
    }
    return impl->builder.doc;
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
