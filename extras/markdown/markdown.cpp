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
    if (!s.s || s.len <= 0) {
        return 0;
    }
    TempStr text = StrDupTemp(s);
    return text.s ? strtof(text.s, nullptr) : 0;
}

int StrToIntUnchecked(Str s) {
    if (!s.s || s.len <= 0) {
        return 0;
    }
    int i = 0;
    while (i < s.len && s.s[i] <= ' ') {
        i++;
    }
    bool negative = false;
    Str rest = Str(s.s + i, s.len - i);
    if (StrStartsWithAny(rest, "+-")) {
        negative = rest.s[0] == '-';
        i++;
    }
    uint64_t value = 0;
    while (i < s.len && s.s[i] >= '0' && s.s[i] <= '9') {
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

static uint64_t ArenaMin(uint64_t a, uint64_t b) {
    return (a < b) ? a : b;
}

static uint64_t ArenaMax(uint64_t a, uint64_t b) {
    return (a > b) ? a : b;
}

static uint64_t ArenaClampTop(uint64_t value, uint64_t maxValue) {
    return (value < maxValue) ? value : maxValue;
}

static uint64_t ArenaClampBot(uint64_t minValue, uint64_t value) {
    return (value > minValue) ? value : minValue;
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
    if (align == 0) {
        align = 1;
    }
    Arena* current = arena->current;
    uint64_t posPost = ArenaAlignPow2(current->pos, align) + size;
    return current->reserved < posPost;
}

static void* ArenaPushLocked(Arena* arena, uint64_t size, uint64_t align,
                             bool zero) {
    if (!arena) {
        return nullptr;
    }
    if (align == 0) {
        align = 1;
    }

    Arena* current = arena->current;
    uint64_t posPre = ArenaAlignPow2(current->pos, align);
    uint64_t posPost = posPre + size;

    uint64_t sizeToZero = 0;
    if (zero && current->committed > posPre) {
        sizeToZero = ArenaMin(current->committed, posPost) - posPre;
    }

    if (current->reserved < posPost && !(arena->flags & ArenaFlagNoChain)) {

        uint64_t reserveChunkSize = arena->reserveChunkSize;
        uint64_t commitChunkSize = arena->commitChunkSize;
        if (size + kArenaHeaderSize > reserveChunkSize) {
            reserveChunkSize = ArenaAlignPow2(size + kArenaHeaderSize,
                                              ArenaMax(align, PlatPageSize()));
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
        posPre = ArenaAlignPow2(current->pos, align);
        posPost = posPre + size;
        sizeToZero = 0;
    }

    if (current->committed < posPost) {
        if (current->flags & ArenaFlagLargePages) {
            return nullptr;
        }

        uint64_t commitEnd = ArenaAlignPow2(posPost, current->commitChunkSize);
        uint64_t commitClamped = ArenaClampTop(commitEnd, current->reserved);
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

    arena->nAllocsLifetime++;
    arena->nAllocsSinceReset++;
    uint64_t used = current->basePos + posPost;
    arena->peakBytesLifetime = std::max(used, arena->peakBytesLifetime);
    arena->peakBytesSinceReset = std::max(used, arena->peakBytesSinceReset);

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
        ArenaMax(params.reserveSize, kArenaHeaderSize), pageSize);
    uint64_t commitSize =
        ArenaAlignPow2(ArenaMax(params.commitSize, kArenaHeaderSize), pageSize);
    commitSize = ArenaClampTop(commitSize, reserveSize);

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
    arena->nAllocsLifetime = 0;
    arena->peakBytesLifetime = 0;
    arena->nAllocsSinceReset = 0;
    arena->peakBytesSinceReset = 0;
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

    uint64_t bigPos = ArenaClampBot(kArenaHeaderSize, popPos);
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

static Arena* ArenaBlockAt(Arena* arena, uint64_t pos) {
    Arena* node = arena ? arena->current : nullptr;
    while (node && node->basePos > pos) {
        node = node->prev;
    }
    return node;
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
    Arena* node = ArenaBlockAt(a, s);
    if (!node) {
        return nullptr;
    }
    return (char*)node + ((uint64_t)s - node->basePos);
}

ArenaStr ArenaStrDup(Arena* a, Str src) {
    if (!a || !src.s || src.len <= 0) {
        return kArenaStrNone;
    }
    uint32_t len = (uint32_t)src.len;
    int vlen = VarintSize(len);
    a->lock.Lock();

    char* dst = (char*)ArenaPushLocked(a, (uint64_t)vlen + len + 1, 1, false);
    Arena* cur = a->current;
    uint64_t at = dst ? cur->basePos + (uint64_t)((char*)dst - (char*)cur) : 0;
    a->lock.Unlock();
    if (!dst) {
        return kArenaStrNone;
    }
    VarintPut(dst, len);
    memcpy(dst + vlen, src.s, (size_t)len);
    dst[vlen + len] = 0;
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
    if (!a || !more.s || more.len <= 0) {
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
    uint64_t at = 0;
    if (dst) {
        Arena* after = a->current;
        at = after->basePos + (uint64_t)((char*)dst - (char*)after);
    }
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
        uint64_t off = node->basePos + (uint64_t)(at - lo);
        if (off > UINT32_MAX) {

            return kArenaPtrNone;
        }
        return (uint32_t)off;
    }
    return kArenaPtrNone;
}

void* Arena::Alloc(int size) {
    if (size <= 0) {
        return nullptr;
    }
    return Push((uint64_t)size, 8, false);
}

void Arena::Reset() {
    PopTo(0);
    nAllocsSinceReset = 0;
    peakBytesSinceReset = 0;
}

void* Alloc(Arena* arena, int size) {
    if (size <= 0) {
        return nullptr;
    }
    if (!arena) {
        return malloc(size);
    }
    return arena->Alloc(size);
}

void Free(Arena* arena, void* mem) {

    if (arena) return;
    free(mem);
}

static void* Alloc(Arena* arena, size_t size) {
    if (size == 0) {
        return nullptr;
    }
    if (!arena) {
        return malloc(size);
    }
    return arena->Push((uint64_t)size, 8, false);
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

static int VecNextCap(int cap, int wanted, int elSize) {
    if (cap == 0) {
        int floorCap = elSize == 1 ? 8 : elSize <= 1024 ? 4 : 1;
        return std::max(floorCap, wanted);
    }
    return std::max(cap * 2, wanted);
}

GPUI_NOINLINE bool VecReserveNT(Arena* arena, VecNonTemplated* v, int elSize,
                                int wantedSize) {
    int cap = v->cap;
    int curCap = cap < 0 ? -cap : cap;
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
    int curCap = v->cap < 0 ? -v->cap : v->cap;
    if (newSize > curCap) {
        if (!VecReserveNT(nullptr, v, elSize, newSize)) {
            return false;
        }
        curCap = v->cap < 0 ? -v->cap : v->cap;
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
    int curCap = v->cap < 0 ? -v->cap : v->cap;
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
        int curCap = v->cap < 0 ? -v->cap : v->cap;
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

static bool StrIsNull(const Str& s) {
    return !s.s;
}

static Str WrapAllocated(char* s, int cch = -1) {
    if (!s) {
        return {};
    }
    if (cch < 0) {
        return Str(s);
    }
    return Str(s, cch);
}

Str StrDup(Arena* a, Str s) {
    if (StrIsNull(s) || s.len < 0) {
        return {};
    }
    int cch = s.len;
    return WrapAllocated(
        (char*)MemDup(a, s.s, (size_t)cch * sizeof(char), sizeof(char)), cch);
}

Str StrDup(Str s) {
    return StrDup(nullptr, s);
}

void StrDup2(Str s1, Str s2, Str& s1Out, Str& s2Out) {
    s1Out = {};
    s2Out = {};
    int n1 = (!s1.s || s1.len < 0) ? 0 : s1.len;
    int n2 = (!s2.s || s2.len < 0) ? 0 : s2.len;
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

void StrFree2(Str s) {
    StrFree(s);
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

GPUI_NOINLINE bool StrEqRest(Str s1, Str s2) {
    if (s1.s == s2.s || s1.len == 0) {
        return true;
    }
    if (!s1.s || !s2.s) {
        return false;
    }
    return memcmp(s1.s, s2.s, (size_t)s1.len) == 0;
}

bool StrEq(Str s1, const char* s2) {
    return StrEq(s1, Str(s2));
}

int StrCmp(Str s1, Str s2) {
    int common = std::min(s1.len, s2.len);
    int cmp = common > 0 ? memcmp(s1.s, s2.s, (size_t)common) : 0;
    if (cmp != 0) {
        return cmp;
    }
    return s1.len < s2.len ? -1 : s1.len > s2.len ? 1 : 0;
}

GPUI_NOINLINE bool StrEqIRest(Str s1, Str s2) {
    if (s1.s == s2.s || s1.len == 0) {
        return true;
    }
    if (StrIsNull(s1) || StrIsNull(s2)) {
        return false;
    }
    return 0 == StrCmpNI(s1.s, s2.s, s1.len);
}

bool StrEqI(Str s1, const char* s2) {
    return StrEqI(s1, Str(s2));
}

bool StrStartsWith(Str s, Str prefix) {
    if (prefix.len > s.len) {
        return false;
    }
    if (prefix.len == 0) {
        return true;
    }
    return s.s && prefix.s && StrEq(Str(s.s, prefix.len), prefix);
}

bool StrStartsWith(Str s, const char* prefix) {
    return StrStartsWith(s, Str(prefix));
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

bool StrStartsWithI(Str s, const char* prefix) {
    return StrStartsWithI(s, Str(prefix));
}

bool StrEndsWith(Str s, Str suffix) {
    if (suffix.len > s.len) {
        return false;
    }
    if (suffix.len == 0) {
        return true;
    }
    return s.s && suffix.s &&
           StrEq(Str(s.s + s.len - suffix.len, suffix.len), suffix);
}

bool StrEndsWith(Str s, const char* suffix) {
    return StrEndsWith(s, Str(suffix));
}

bool StrEndsWithI(Str s, Str suffix) {
    if (suffix.len > s.len) {
        return false;
    }
    if (suffix.len == 0) {
        return true;
    }
    return s.s && suffix.s &&
           StrEqI(Str(s.s + s.len - suffix.len, suffix.len), suffix);
}

bool StrEndsWithI(Str s, const char* suffix) {
    return StrEndsWithI(s, Str(suffix));
}

int StrFind(Str s, Str sub) {
    if (!s.s || !sub.s || sub.len <= 0 || sub.len > s.len) {
        return -1;
    }
    for (int off = 0; off + sub.len <= s.len; off++) {
        if (StrEq(Str(s.s + off, sub.len), sub)) {
            return off;
        }
    }
    return -1;
}

int StrFind(Str s, const char* sub) {
    return StrFind(s, Str(sub));
}

int StrFindI(Str s, Str sub) {
    if (!s.s || !sub.s || sub.len <= 0 || sub.len > s.len) {
        return -1;
    }
    for (int off = 0; off + sub.len <= s.len; off++) {
        if (StrEqI(Str(s.s + off, sub.len), sub)) {
            return off;
        }
    }
    return -1;
}

int StrFindI(Str s, const char* sub) {
    return StrFindI(s, Str(sub));
}

bool StrContains(Str s, Str sub) {
    return StrFind(s, sub) >= 0;
}

bool StrContainsI(Str s, Str sub) {
    return StrFindI(s, sub) >= 0;
}

static bool IsStrTrimAscii(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

Str StrTrimAscii(Str s) {
    if (!s.s || s.len <= 0) {
        return s;
    }
    int start = 0;
    int end = s.len;
    while (start < end && IsStrTrimAscii(s.s[start])) {
        start++;
    }
    while (end > start && IsStrTrimAscii(s.s[end - 1])) {
        end--;
    }
    return Str(s.s + start, end - start);
}

Str StrReplaceAll(Str value, Str from, Str to) {
    if (from.len == 0 || from.len > value.len) {
        return value;
    }
    int count = 0;
    for (int i = 0; i <= value.len - from.len;) {
        if (StrEq(Str(value.s + i, from.len), from)) {
            count++;
            i += from.len;
        } else {
            i++;
        }
    }
    if (count == 0) {
        return value;
    }

    int64_t grown = (int64_t)value.len +
                    (int64_t)count * ((int64_t)to.len - (int64_t)from.len);
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
    while (src < value.len) {
        if (src <= value.len - from.len &&
            StrEq(Str(value.s + src, from.len), from)) {
            memcpy(result.s + dst, to.s, (size_t)to.len);
            src += from.len;
            dst += to.len;
        } else {
            result.s[dst++] = value.s[src++];
        }
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
    if (s.len == 0) {
        return {};
    }
    const char* next = s.s + s.len + 1;
    return next[0] ? Str(next) : Str{};
}

static int SeqStrIndexCmp(SeqStrings strs, Str toFind, bool ignoreCase) {
    if (!strs || !toFind) return -1;
    const char* candidate = strs;
    int idx = 0;
    while (*candidate) {
        int i = 0;
        while (i < toFind.len && candidate[i]) {
            char a = candidate[i];
            char b = toFind.s[i];
            if (ignoreCase) {
                if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
                if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
            }
            if (a != b) break;
            i++;
        }
        if (i == toFind.len && !candidate[i]) return idx;
        while (*candidate) candidate++;
        candidate++;
        idx++;
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
    while (idx > 0 && s.len > 0) {
        s = SeqStrNext(s);
        idx--;
    }
    return s;
}

int SeqStrCount(SeqStrings strs) {
    int n = 0;
    for (Str s = SeqStrFirst(strs); s.len > 0; s = SeqStrNext(s)) {
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
    if (buf.s && buf.len > kPadding) {
        b.els = buf.s;
        b.cap = -(buf.len - kPadding);
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
    if (StrIsNull(src) || 0 == src.len) {
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

    while (off < fmt.format.len && fmt.format.s[off] != '}') {
        if (!IsDigit(fmt.format.s[off])) {
            fmt.isOk = false;
            return off;
        }
        n = (n * 10) + (fmt.format.s[off] - '0');
        positional = true;
        off++;
    }
    if (off >= fmt.format.len) {
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
        if (off + i >= s.len || s.s[off + i] != prefix[i]) {
            return false;
        }
        i++;
    }
    return true;
}

static int parseArgDefPerc(Fmt& fmt, int off) {
    Str f = fmt.format;
    off++;
    int fwpStart = off;
    bool leftJust = false;

    while (off < f.len &&
           (f.s[off] == '-' || f.s[off] == '+' || f.s[off] == ' ' ||
            f.s[off] == '0' || f.s[off] == '#')) {
        if (f.s[off] == '-') {
            leftJust = true;
        }
        off++;
    }

    int width = 0;
    while (off < f.len && IsDigit(f.s[off])) {
        width = (width * 10) + (f.s[off] - '0');
        off++;
    }

    int prec = -1;
    if (off < f.len && f.s[off] == '.') {
        off++;
        prec = 0;
        while (off < f.len && IsDigit(f.s[off])) {
            prec = (prec * 10) + (f.s[off] - '0');
            off++;
        }
    }
    int fwpEnd = off;

    int bits = 32;
    char lenMod = (off < f.len) ? f.s[off] : 0;
    bool is32BitLenMod =
        lenMod == 'l' || lenMod == 'h' || lenMod == 'L' || lenMod == 'w';

    bool is64BitLenMod =
        lenMod == 'z' || lenMod == 'j' || lenMod == 't' || lenMod == 'I';
    if (startsWith(f, off, "I64")) {
        bits = 64;
        off += 3;
    } else if (startsWith(f, off, "I32")) {
        off += 3;
    } else if (startsWith(f, off, "ll")) {
        bits = 64;
        off += 2;
    } else if (startsWith(f, off, "hh")) {
        off += 2;
    } else if (is32BitLenMod) {
        off++;
    } else if (is64BitLenMod) {
        bits = 64;
        off++;
    }
    char conv = (off < f.len) ? f.s[off] : 0;
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
    while (off < fmtStr.len && fmtStr.s[off]) {
        char c = fmtStr.s[off];
        if ('%' == c) {

            if (off + 1 < fmtStr.len && '%' == fmtStr.s[off + 1]) {
                addRawStr(o, start, off - start);
                start = off + 1;
                off += 2;
                continue;
            }
            addRawStr(o, start, off - start);
            if (off + 1 < fmtStr.len && '{' == fmtStr.s[off + 1]) {
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
    if (n >= 0 && n < bufS.len) {
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

static bool evalPercInst(Fmt& fmt, const Inst& inst, const FmtArg& arg) {
    if (inst.conv == 's' || inst.conv == 'S') {
        Str sv = arg.str;
        int slen = sv.len;
        if (inst.prec >= 0 && inst.prec < slen) {
            slen = inst.prec;
        }
        int pad = inst.width - slen;
        pad = std::max(pad, 0);
        if (!inst.leftJust) {
            for (int j = 0; j < pad; j++) {
                if (!fmt.res.AppendChar(' ')) {
                    return false;
                }
            }
        }
        if (!fmt.res.Append(Str(sv.s, slen))) {
            return false;
        }
        if (inst.leftJust) {
            for (int j = 0; j < pad; j++) {
                if (!fmt.res.AppendChar(' ')) {
                    return false;
                }
            }
        }
        return true;
    }

    char fbuf[64];
    int k = 0;
    fbuf[k++] = '%';
    for (int j = 0; j < inst.fwpLen && k < (int)dimof(fbuf) - 5; j++) {
        fbuf[k++] = fmt.format.s[inst.fwpOff + j];
    }
    char conv = inst.conv;
    int64_t ival = argToI64(arg);
    bool ok = true;
    switch (conv) {
        case 'd':
        case 'i':
            if (inst.intBits == 64) {
                fbuf[k++] = 'l';
                fbuf[k++] = 'l';
                fbuf[k++] = 'd';
                fbuf[k] = 0;
                ok = appendConv(fmt, fbuf, (long long)ival);
            } else {
                fbuf[k++] = 'd';
                fbuf[k] = 0;
                ok = appendConv(fmt, fbuf, (int)ival);
            }
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            if (inst.intBits == 64) {
                fbuf[k++] = 'l';
                fbuf[k++] = 'l';
                fbuf[k++] = conv;
                fbuf[k] = 0;
                ok = appendConv(fmt, fbuf, (unsigned long long)ival);
            } else {
                fbuf[k++] = conv;
                fbuf[k] = 0;
                ok = appendConv(fmt, fbuf,
                                (unsigned int)(unsigned long long)ival);
            }
            break;
        case 'c':
            fbuf[k++] = 'c';
            fbuf[k] = 0;
            ok = appendConv(fmt, fbuf, (int)ival);
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            fbuf[k++] = conv;
            fbuf[k] = 0;
            double dv = (arg.t == FmtArg::Kind::Double) ? arg.d : (double)arg.f;
            ok = appendConv(fmt, fbuf, dv);
        } break;
        case 'p': {

            const void* pv = (arg.t == FmtArg::Kind::Ptr)
                                 ? arg.ptr
                                 : (const void*)(intptr_t)ival;
            ok = appendConv(fmt, "%p", pv);
        } break;
        default:
            break;
    }
    return ok;
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
        return _vsnprintf_l(buf.s, (size_t)buf.len, fmt, loc, args);
    }
#endif
    return vsnprintf(buf.s, (size_t)buf.len, fmt, args);
}
}

#line 1 "src/markdown/constant.cpp"

namespace markdown {

extern const Str kHtmlCdataPrefix = StrL("CDATA[");

const char kHtmlBlockNames[] =
    "address\0"
    "article\0"
    "aside\0"
    "base\0"
    "basefont\0"
    "blockquote\0"
    "body\0"
    "caption\0"
    "center\0"
    "col\0"
    "colgroup\0"
    "dd\0"
    "details\0"
    "dialog\0"
    "dir\0"
    "div\0"
    "dl\0"
    "dt\0"
    "fieldset\0"
    "figcaption\0"
    "figure\0"
    "footer\0"
    "form\0"
    "frame\0"
    "frameset\0"
    "h1\0"
    "h2\0"
    "h3\0"
    "h4\0"
    "h5\0"
    "h6\0"
    "head\0"
    "header\0"
    "hr\0"
    "html\0"
    "iframe\0"
    "legend\0"
    "li\0"
    "link\0"
    "main\0"
    "menu\0"
    "menuitem\0"
    "nav\0"
    "noframes\0"
    "ol\0"
    "optgroup\0"
    "option\0"
    "p\0"
    "param\0"
    "search\0"
    "section\0"
    "summary\0"
    "table\0"
    "tbody\0"
    "td\0"
    "tfoot\0"
    "th\0"
    "thead\0"
    "title\0"
    "tr\0"
    "track\0"
    "ul\0";

const char kHtmlRawNames[] =
    "pre\0"
    "script\0"
    "style\0"
    "textarea\0";

const char kCharacterReferenceNames[] =
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

const char kCharacterReferenceValues[] =
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

const CharacterReference kCharacterReferences[2125] = {
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

}

#line 1 "src/markdown/construct_document.cpp"

namespace markdown {

enum class Phase : uint8_t {
    After,
    Prefix,
    Eof,
};

static void ExitContainers(Tokenizer* t, Phase phase);
static void DocumentResolve(Tokenizer* t);

State DocumentStart(Tokenizer* t) {
    t->tokenizeState.documentChild = TokenizerNew(t->point, t->parseState);
    TokenizerAttempt(t, StateNext(StateName::DocumentBeforeFrontmatter),
                     StateNext(StateName::DocumentBeforeFrontmatter));
    return StateRetry(StateName::BomStart);
}

State DocumentBeforeFrontmatter(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::DocumentContainerNewBefore),
                     StateNext(StateName::DocumentContainerNewBefore));
    return StateRetry(StateName::FrontmatterStart);
}

State DocumentContainerExistingBefore(Tokenizer* t) {

    if (t->tokenizeState.documentContinued <
        t->tokenizeState.documentContainerStack.len) {
        const ContainerState& container =
            t->tokenizeState
                .documentContainerStack[t->tokenizeState.documentContinued];
        StateName name = StateName::BlockQuoteContStart;
        if (container.kind == Container::GfmFootnoteDefinition) {
            name = StateName::GfmFootnoteDefinitionContStart;
        } else if (container.kind == Container::ListItem) {
            name = StateName::ListItemContStart;
        }
        TokenizerAttempt(t, StateNext(StateName::DocumentContainerExistingAfter),
                         StateNext(StateName::DocumentContainerNewBefore));
        return StateRetry(name);
    }

    return StateRetry(StateName::DocumentContainerNewBefore);
}

State DocumentContainerExistingAfter(Tokenizer* t) {
    t->tokenizeState.documentContinued += 1;
    return StateRetry(StateName::DocumentContainerExistingBefore);
}

State DocumentContainerNewBefore(Tokenizer* t) {

    if (t->tokenizeState.documentContinued ==
        t->tokenizeState.documentContainerStack.len) {
        Tokenizer* child = t->tokenizeState.documentChild;
        t->interrupt = child->interrupt;

        if (child->concrete) {
            return StateRetry(StateName::DocumentContainersAfter);
        }
    }

    int32_t tail = t->tokenizeState.documentContainerStack.len;
    ContainerState fresh;
    fresh.kind = Container::BlockQuote;
    VecAppend(t->tokenizeState.documentContainerStack, fresh);
    ContainerState swap =
        t->tokenizeState
            .documentContainerStack[t->tokenizeState.documentContinued];
    t->tokenizeState
        .documentContainerStack[t->tokenizeState.documentContinued] =
        t->tokenizeState.documentContainerStack[tail];
    t->tokenizeState.documentContainerStack[tail] = swap;

    TokenizerAttempt(
        t, StateNext(StateName::DocumentContainerNewAfter),
        StateNext(StateName::DocumentContainerNewBeforeNotBlockQuote));
    return StateRetry(StateName::BlockQuoteStart);
}

State DocumentContainerNewBeforeNotBlockQuote(Tokenizer* t) {
    ContainerState fresh;
    fresh.kind = Container::ListItem;
    t->tokenizeState
        .documentContainerStack[t->tokenizeState.documentContinued] = fresh;
    TokenizerAttempt(t, StateNext(StateName::DocumentContainerNewAfter),
                     StateNext(StateName::DocumentContainerNewBeforeNotList));
    return StateRetry(StateName::ListItemStart);
}

State DocumentContainerNewBeforeNotList(Tokenizer* t) {
    ContainerState fresh;
    fresh.kind = Container::GfmFootnoteDefinition;
    t->tokenizeState
        .documentContainerStack[t->tokenizeState.documentContinued] = fresh;
    TokenizerAttempt(
        t, StateNext(StateName::DocumentContainerNewAfter),
        StateNext(
            StateName::DocumentContainerNewBeforeNotGfmFootnoteDefinition));
    return StateRetry(StateName::GfmFootnoteDefinitionStart);
}

static ContainerState SwapRemove(Vec<ContainerState>& stack, int32_t index) {
    ContainerState out = stack[index];
    stack[index] = stack[stack.len - 1];
    stack.len -= 1;
    return out;
}

State DocumentContainerNewBeforeNotGfmFootnoteDefinition(Tokenizer* t) {
    SwapRemove(t->tokenizeState.documentContainerStack,
               t->tokenizeState.documentContinued);
    return StateRetry(StateName::DocumentContainersAfter);
}

State DocumentContainerNewAfter(Tokenizer* t) {
    ContainerState container = SwapRemove(
        t->tokenizeState.documentContainerStack,
        t->tokenizeState.documentContinued);

    if (t->tokenizeState.documentContinued !=
        t->tokenizeState.documentContainerStack.len) {
        ExitContainers(t, Phase::Prefix);
    }

    t->tokenizeState.documentChild->pierce = true;
    VecAppend(t->tokenizeState.documentContainerStack, container);
    t->tokenizeState.documentContinued += 1;
    t->interrupt = false;
    return StateRetry(StateName::DocumentContainerNewBefore);
}

State DocumentContainersAfter(Tokenizer* t) {
    Tokenizer* child = t->tokenizeState.documentChild;
    child->lazy = t->tokenizeState.documentContinued !=
                  t->tokenizeState.documentContainerStack.len;
    DefineSkip(child, t->point);

    if (t->current < 0) {
        return StateRetry(StateName::DocumentFlowEnd);
    }
    int32_t current = t->events.len;
    int32_t previous = t->tokenizeState.documentDataIndex;
    if (previous != -1) {
        t->events[previous].link.next = current;
    }
    t->tokenizeState.documentDataIndex = current;
    Link link;
    link.previous = previous;
    link.content = ContentKind::Flow;
    EnterLink(t, Name::Data, link);
    return StateRetry(StateName::DocumentFlowInside);
}

State DocumentFlowInside(Tokenizer* t) {
    if (t->current < 0) {
        Exit(t, Name::Data);
        return StateRetry(StateName::DocumentFlowEnd);
    }
    if (t->current == '\n') {
        Consume(t);
        Exit(t, Name::Data);
        return StateNext(StateName::DocumentFlowEnd);
    }
    Consume(t);
    return StateNext(StateName::DocumentFlowInside);
}

State DocumentFlowEnd(Tokenizer* t) {
    Tokenizer* child = t->tokenizeState.documentChild;
    State state = t->tokenizeState.documentChildStateSome
                      ? t->tokenizeState.documentChildState
                      : StateNext(StateName::FlowStart);
    t->tokenizeState.documentChildStateSome = false;

    ArenaVec<Event> emptyExits {};
    VecAppend(t->tokenizeState.documentExits, emptyExits);

    state = Push(child, child->point.index, child->point.vs, t->point.index,
                 t->point.vs, state);
    t->tokenizeState.documentChildState = state;
    t->tokenizeState.documentChildStateSome = true;

    bool documentLazyContinuationCurrent = false;
    int32_t stackIndex = child->stack.len;
    while (!documentLazyContinuationCurrent && stackIndex > 0) {
        stackIndex -= 1;
        Name name = child->stack[stackIndex];
        if (name == Name::Content || name == Name::GfmTableHead) {
            documentLazyContinuationCurrent = true;
        }
    }
    if (!documentLazyContinuationCurrent && child->events.len > 0) {
        Name lineEnding = Name::LineEnding;
        int32_t before =
            SkipOptBack(child->events, child->events.len - 1, &lineEnding, 1);
        Name name = child->events[before].name;
        if (name == Name::Content || name == Name::HeadingSetextUnderline) {
            documentLazyContinuationCurrent = true;
        }
    }

    child->pierce = false;

    if (child->lazy && t->tokenizeState.documentLazyAcceptingBefore &&
        documentLazyContinuationCurrent) {
        t->tokenizeState.documentContinued =
            t->tokenizeState.documentContainerStack.len;
    }

    if (t->tokenizeState.documentContinued !=
        t->tokenizeState.documentContainerStack.len) {
        ExitContainers(t, Phase::After);
    }

    if (t->current < 0) {
        t->tokenizeState.documentContinued = 0;
        ExitContainers(t, Phase::Eof);
        DocumentResolve(t);
        return StateOk();
    }

    t->tokenizeState.documentContinued = 0;
    t->tokenizeState.documentLazyAcceptingBefore =
        documentLazyContinuationCurrent;
    t->interrupt = false;
    return StateRetry(StateName::DocumentContainerExistingBefore);
}

static void ExitContainers(Tokenizer* t, Phase phase) {

    Vec<ContainerState> stackClose;
    for (int32_t i = t->tokenizeState.documentContinued;
         i < t->tokenizeState.documentContainerStack.len; i++) {
        VecAppend(stackClose, t->tokenizeState.documentContainerStack[i]);
    }
    t->tokenizeState.documentContainerStack.len =
        t->tokenizeState.documentContinued;

    Tokenizer* child = t->tokenizeState.documentChild;

    if (phase != Phase::After) {
        State state = t->tokenizeState.documentChildStateSome
                          ? t->tokenizeState.documentChildState
                          : StateNext(StateName::FlowStart);
        t->tokenizeState.documentChildStateSome = false;
        Flush(child, state, false);
    }

    if (stackClose.len > 0) {
        int32_t index = t->tokenizeState.documentExits.len -
                        (phase == Phase::After ? 2 : 1);
        ArenaVec<Event> exits {};
        while (stackClose.len > 0) {
            ContainerState container = stackClose[--stackClose.len];
            Name name = Name::BlockQuote;
            if (container.kind == Container::GfmFootnoteDefinition) {
                name = Name::GfmFootnoteDefinition;
            } else if (container.kind == Container::ListItem) {
                name = Name::ListItem;
            }
            Event event;
            event.kind = Kind::Exit;
            event.name = name;
            event.point = t->point;
            exits.Append(t->parseState->scratch, event);

            int32_t stackIndex = t->stack.len;
            while (stackIndex > 0) {
                stackIndex -= 1;
                if (t->stack[stackIndex] == name) {
                    for (int32_t i = stackIndex; i + 1 < t->stack.len; i++) {
                        t->stack[i] = t->stack[i + 1];
                    }
                    t->stack.len -= 1;
                    break;
                }
            }
        }
        t->tokenizeState.documentExits[index] = exits;
    }

    child->interrupt = false;
}

static void DocumentResolve(Tokenizer* t) {
    Tokenizer* child = t->tokenizeState.documentChild;

    int32_t childIndex = 0;
    int32_t line = 0;
    while (childIndex < child->events.len) {
        if (child->events[childIndex].kind == Kind::Exit &&
            (child->events[childIndex].name == Name::LineEnding ||
             child->events[childIndex].name == Name::BlankLineEnding)) {
            int32_t injectIndex = childIndex - 1;
            Point point = child->events[injectIndex].point;

            while (childIndex + 1 < child->events.len &&
                   child->events[childIndex + 1].kind == Kind::Exit) {
                childIndex += 1;
                point = child->events[childIndex].point;
                injectIndex = childIndex + 1;
            }
            if (line < t->tokenizeState.documentExits.len) {
                ArenaVec<Event> exits = t->tokenizeState.documentExits[line];
                if (exits.len > 0) {
                    t->tokenizeState.documentExits[line] = ArenaVec<Event>{};
                    for (Event& exit : exits) {
                        exit.point = point;
                    }
                    EditMapAdd(child->map, injectIndex, 0,
                               exits.Flatten(t->parseState->scratch),
                               exits.len);
                }
            }
            line += 1;
        }
        childIndex += 1;
    }
    EditMapConsume(child->map, child->events);

    Name data = Name::Data;
    int32_t flowIndex = SkipTo(t->events, 0, &data, 1);
    while (flowIndex < t->events.len &&
           (!t->events[flowIndex].hasLink ||
            t->events[flowIndex].link.content != ContentKind::Flow)) {
        flowIndex = SkipTo(t->events, flowIndex + 1, &data, 1);
    }
    int32_t accA = 0;
    int32_t accB = 0;
    DivideEvents(t->map, t->events, flowIndex, child->events, &accA, &accB);
    EditMapConsume(t->map, t->events);

    if (line < t->tokenizeState.documentExits.len) {
        ArenaVec<Event> exits = t->tokenizeState.documentExits[line];
        if (exits.len > 0) {
            t->tokenizeState.documentExits[line] = ArenaVec<Event>{};
            for (Event& exit : exits) {
                exit.point = t->point;
                VecAppend(t->events, exit);
            }
        }
    }

    for (int32_t i = 0; i < child->resolvers.len; i++) {
        VecAppend(t->resolvers, child->resolvers[i]);
    }
    child->resolvers.len = 0;
    for (int32_t i = 0; i < child->tokenizeState.definitions.len; i++) {
        VecAppend(t->tokenizeState.definitions, child->tokenizeState
                                                    .definitions[i]);
    }
    child->tokenizeState.definitions.len = 0;
}

State FlowStart(Tokenizer* t) {
    switch (t->current) {
        case '#':
            TokenizerAttempt(t, StateNext(StateName::FlowAfter),
                             StateNext(StateName::FlowBeforeContent));
            return StateRetry(StateName::HeadingAtxStart);

        case '$':
        case '`':
        case '~':
            TokenizerAttempt(t, StateNext(StateName::FlowAfter),
                             StateNext(StateName::FlowBeforeContent));
            return StateRetry(StateName::RawFlowStart);
        case '*':
        case '_':
            TokenizerAttempt(t, StateNext(StateName::FlowAfter),
                             StateNext(StateName::FlowBeforeContent));
            return StateRetry(StateName::ThematicBreakStart);
        case '<':

            TokenizerAttempt(t, StateNext(StateName::FlowAfter),
                             StateNext(StateName::FlowBeforeHeadingAtx));
            return StateRetry(StateName::HtmlFlowStart);
        case 'e':
        case 'i':
        case '{':

            return StateRetry(StateName::FlowBeforeContent);
        default:
            return StateRetry(StateName::FlowBlankLineBefore);
    }
}

State FlowBlankLineBefore(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::FlowBlankLineAfter),
                     StateNext(StateName::FlowBeforeCodeIndented));
    return StateRetry(StateName::BlankLineStart);
}

State FlowBeforeCodeIndented(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::FlowAfter),
                     StateNext(StateName::FlowBeforeRaw));
    return StateRetry(StateName::CodeIndentedStart);
}

State FlowBeforeRaw(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::FlowAfter),
                     StateNext(StateName::FlowBeforeHtml));
    return StateRetry(StateName::RawFlowStart);
}

State FlowBeforeHtml(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::FlowAfter),
                     StateNext(StateName::FlowBeforeHeadingAtx));
    return StateRetry(StateName::HtmlFlowStart);
}

State FlowBeforeHeadingAtx(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::FlowAfter),
                     StateNext(StateName::FlowBeforeHeadingSetext));
    return StateRetry(StateName::HeadingAtxStart);
}

State FlowBeforeHeadingSetext(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::FlowAfter),
                     StateNext(StateName::FlowBeforeThematicBreak));
    return StateRetry(StateName::HeadingSetextStart);
}

State FlowBeforeThematicBreak(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::FlowAfter),
                     StateNext(StateName::FlowBeforeGfmTable));
    return StateRetry(StateName::ThematicBreakStart);
}

State FlowBeforeGfmTable(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::FlowAfter),
                     StateNext(StateName::FlowBeforeContent));
    return StateRetry(StateName::GfmTableStart);
}

State FlowBeforeContent(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::FlowAfter), StateNok());
    return StateRetry(StateName::ContentChunkStart);
}

State FlowBlankLineAfter(Tokenizer* t) {
    if (t->current < 0) {
        return StateOk();
    }
    Enter(t, Name::BlankLineEnding);
    Consume(t);
    Exit(t, Name::BlankLineEnding);

    t->interrupt = false;
    return StateNext(StateName::FlowStart);
}

State FlowAfter(Tokenizer* t) {
    if (t->current < 0) {
        return StateOk();
    }
    Enter(t, Name::LineEnding);
    Consume(t);
    Exit(t, Name::LineEnding);
    return StateNext(StateName::FlowStart);
}

State ContentChunkStart(Tokenizer* t) {
    Link link;
    link.content = ContentKind::Content;
    EnterLink(t, Name::Content, link);
    return StateRetry(StateName::ContentChunkInside);
}

State ContentChunkInside(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        Exit(t, Name::Content);
        RegisterResolverBefore(t, ResolveName::Content);

        t->interrupt = true;
        return StateOk();
    }
    Consume(t);
    return StateNext(StateName::ContentChunkInside);
}

State ContentDefinitionBefore(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::ContentDefinitionAfter),
                     StateNext(StateName::ParagraphStart));
    return StateRetry(StateName::DefinitionStart);
}

State ContentDefinitionAfter(Tokenizer* t) {
    if (t->current < 0) {
        return StateOk();
    }
    Enter(t, Name::LineEnding);
    Consume(t);
    Exit(t, Name::LineEnding);
    return StateNext(StateName::ContentDefinitionBefore);
}

bool ContentResolve(Tokenizer* t, Subresult* out) {
    int32_t index = 0;
    while (index < t->events.len) {
        const Event& event = t->events[index];
        if (event.kind == Kind::Enter && event.name == Name::Content) {
            int32_t exitIndex = index + 1;
            for (;;) {
                int32_t enterIndex = exitIndex + 1;
                if (enterIndex == t->events.len ||
                    t->events[enterIndex].name != Name::LineEnding) {
                    break;
                }

                enterIndex += 2;

                while (enterIndex < t->events.len) {
                    Name name = t->events[enterIndex].name;
                    if (name != Name::SpaceOrTab &&
                        name != Name::BlockQuotePrefix &&
                        name != Name::BlockQuoteMarker) {
                        break;
                    }
                    enterIndex += 1;
                }
                if (enterIndex == t->events.len ||
                    t->events[enterIndex].name != Name::Content) {
                    break;
                }

                t->events[exitIndex].point = t->events[exitIndex + 2].point;

                EditMapAdd(t->map, exitIndex + 1, 2, nullptr, 0);

                t->events[exitIndex - 1].link.next = enterIndex;
                t->events[enterIndex].link.previous = exitIndex - 1;
                exitIndex = enterIndex + 1;
            }
            index = exitIndex;
        }
        index += 1;
    }
    EditMapConsume(t->map, t->events);
    *out = Subtokenize(t->events, t->parseState, true, ContentKind::Content);
    return true;
}

State ParagraphStart(Tokenizer* t) {
    Enter(t, Name::Paragraph);
    return StateRetry(StateName::ParagraphLineStart);
}

State ParagraphLineStart(Tokenizer* t) {
    Link link;
    link.content = ContentKind::Text;
    EnterLink(t, Name::Data, link);
    if (t->tokenizeState.connect) {
        SubtokenizeLink(t->events, t->events.len - 1);
    } else {
        t->tokenizeState.connect = true;
    }
    return StateRetry(StateName::ParagraphInside);
}

State ParagraphInside(Tokenizer* t) {
    if (t->current < 0) {
        t->tokenizeState.connect = false;
        Exit(t, Name::Data);
        Exit(t, Name::Paragraph);
        return StateOk();
    }
    if (t->current == '\n') {
        Consume(t);
        Exit(t, Name::Data);
        return StateNext(StateName::ParagraphLineStart);
    }
    Consume(t);
    return StateNext(StateName::ParagraphInside);
}

}

#line 1 "src/markdown/construct_flow.cpp"

namespace markdown {

static int32_t IndentMax(Tokenizer* t) {
    return t->parseState->options->constructs.codeIndented ? kTabSize - 1
                                                           : kSizeMax;
}

State BlockQuoteStart(Tokenizer* t) {
    if (t->parseState->options->constructs.blockQuote) {
        Enter(t, Name::BlockQuote);
        return StateRetry(StateName::BlockQuoteContStart);
    }
    return StateNok();
}

State BlockQuoteContStart(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::BlockQuoteContBefore),
                         StateNok());
        return StateRetry(SpaceOrTabMinMax(t, 1, IndentMax(t)));
    }
    return StateRetry(StateName::BlockQuoteContBefore);
}

State BlockQuoteContBefore(Tokenizer* t) {
    if (t->current == '>') {
        Enter(t, Name::BlockQuotePrefix);
        Enter(t, Name::BlockQuoteMarker);
        Consume(t);
        Exit(t, Name::BlockQuoteMarker);
        return StateNext(StateName::BlockQuoteContAfter);
    }
    return StateNok();
}

State BlockQuoteContAfter(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        Enter(t, Name::SpaceOrTab);
        Consume(t);
        Exit(t, Name::SpaceOrTab);
    }
    Exit(t, Name::BlockQuotePrefix);
    return StateOk();
}

State CodeIndentedStart(Tokenizer* t) {

    if (!t->interrupt && t->parseState->options->constructs.codeIndented &&
        (t->current == '\t' || t->current == ' ')) {
        Enter(t, Name::CodeIndented);
        TokenizerAttempt(t, StateNext(StateName::CodeIndentedAtBreak),
                         StateNok());
        return StateRetry(SpaceOrTabMinMax(t, kTabSize, kTabSize));
    }
    return StateNok();
}

State CodeIndentedAtBreak(Tokenizer* t) {
    if (t->current < 0) {
        return StateRetry(StateName::CodeIndentedAfter);
    }
    if (t->current == '\n') {
        TokenizerAttempt(t, StateNext(StateName::CodeIndentedAtBreak),
                         StateNext(StateName::CodeIndentedAfter));
        return StateRetry(StateName::CodeIndentedFurtherStart);
    }
    Enter(t, Name::CodeFlowChunk);
    return StateRetry(StateName::CodeIndentedInside);
}

State CodeIndentedInside(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        Exit(t, Name::CodeFlowChunk);
        return StateRetry(StateName::CodeIndentedAtBreak);
    }
    Consume(t);
    return StateNext(StateName::CodeIndentedInside);
}

State CodeIndentedAfter(Tokenizer* t) {
    Exit(t, Name::CodeIndented);

    t->interrupt = false;
    return StateOk();
}

State CodeIndentedFurtherStart(Tokenizer* t) {
    if (t->lazy || t->pierce) {
        return StateNok();
    }
    if (t->current == '\n') {
        Enter(t, Name::LineEnding);
        Consume(t);
        Exit(t, Name::LineEnding);
        return StateNext(StateName::CodeIndentedFurtherStart);
    }
    TokenizerAttempt(t, StateOk(),
                     StateNext(StateName::CodeIndentedFurtherBegin));
    return StateRetry(SpaceOrTabMinMax(t, kTabSize, kTabSize));
}

State CodeIndentedFurtherBegin(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::CodeIndentedFurtherAfter),
                         StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    return StateNok();
}

State CodeIndentedFurtherAfter(Tokenizer* t) {
    if (t->current == '\n') {
        return StateRetry(StateName::CodeIndentedFurtherStart);
    }
    return StateNok();
}

State ThematicBreakStart(Tokenizer* t) {
    if (!t->parseState->options->constructs.thematicBreak) {
        return StateNok();
    }
    Enter(t, Name::ThematicBreak);
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::ThematicBreakBefore),
                         StateNok());
        return StateRetry(SpaceOrTabMinMax(t, 0, IndentMax(t)));
    }
    return StateRetry(StateName::ThematicBreakBefore);
}

State ThematicBreakBefore(Tokenizer* t) {
    if (t->current == '*' || t->current == '-' || t->current == '_') {
        t->tokenizeState.marker = (uint8_t)t->current;
        return StateRetry(StateName::ThematicBreakAtBreak);
    }
    return StateNok();
}

State ThematicBreakAtBreak(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        Enter(t, Name::ThematicBreakSequence);
        return StateRetry(StateName::ThematicBreakSequence);
    }
    if (t->tokenizeState.size >= kThematicBreakMarkerCountMin &&
        (t->current < 0 || t->current == '\n')) {
        t->tokenizeState.marker = 0;
        t->tokenizeState.size = 0;
        Exit(t, Name::ThematicBreak);

        t->interrupt = false;
        return StateOk();
    }
    t->tokenizeState.marker = 0;
    t->tokenizeState.size = 0;
    return StateNok();
}

State ThematicBreakSequence(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        Consume(t);
        t->tokenizeState.size += 1;
        return StateNext(StateName::ThematicBreakSequence);
    }
    if (t->current == '\t' || t->current == ' ') {
        Exit(t, Name::ThematicBreakSequence);
        TokenizerAttempt(t, StateNext(StateName::ThematicBreakAtBreak),
                         StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    Exit(t, Name::ThematicBreakSequence);
    return StateRetry(StateName::ThematicBreakAtBreak);
}

State HeadingAtxStart(Tokenizer* t) {
    if (!t->parseState->options->constructs.headingAtx) {
        return StateNok();
    }
    Enter(t, Name::HeadingAtx);
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::HeadingAtxBefore), StateNok());
        return StateRetry(SpaceOrTabMinMax(t, 0, IndentMax(t)));
    }
    return StateRetry(StateName::HeadingAtxBefore);
}

State HeadingAtxBefore(Tokenizer* t) {
    if (t->current == '#') {
        Enter(t, Name::HeadingAtxSequence);
        return StateRetry(StateName::HeadingAtxSequenceOpen);
    }
    return StateNok();
}

State HeadingAtxSequenceOpen(Tokenizer* t) {
    if (t->current == '#' &&
        t->tokenizeState.size < kHeadingAtxOpeningFenceSizeMax) {
        t->tokenizeState.size += 1;
        Consume(t);
        return StateNext(StateName::HeadingAtxSequenceOpen);
    }
    if (t->current < 0 || t->current == '\t' || t->current == '\n' ||
        t->current == ' ') {
        t->tokenizeState.size = 0;
        Exit(t, Name::HeadingAtxSequence);
        return StateRetry(StateName::HeadingAtxAtBreak);
    }
    t->tokenizeState.size = 0;
    return StateNok();
}

State HeadingAtxAtBreak(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        Exit(t, Name::HeadingAtx);
        RegisterResolver(t, ResolveName::HeadingAtx);

        t->interrupt = false;
        return StateOk();
    }
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::HeadingAtxAtBreak), StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    if (t->current == '#') {
        Enter(t, Name::HeadingAtxSequence);
        return StateRetry(StateName::HeadingAtxSequenceFurther);
    }
    Link link;
    link.content = ContentKind::Text;
    EnterLink(t, Name::Data, link);
    return StateRetry(StateName::HeadingAtxData);
}

State HeadingAtxSequenceFurther(Tokenizer* t) {
    if (t->current == '#') {
        Consume(t);
        return StateNext(StateName::HeadingAtxSequenceFurther);
    }
    Exit(t, Name::HeadingAtxSequence);
    return StateRetry(StateName::HeadingAtxAtBreak);
}

State HeadingAtxData(Tokenizer* t) {
    if (t->current < 0 || t->current == '\t' || t->current == '\n' ||
        t->current == ' ') {
        Exit(t, Name::Data);
        return StateRetry(StateName::HeadingAtxAtBreak);
    }
    Consume(t);
    return StateNext(StateName::HeadingAtxData);
}

bool HeadingAtxResolve(Tokenizer* t, Subresult*) {
    int32_t index = 0;
    bool headingInside = false;
    int32_t dataStart = -1;
    int32_t dataEnd = -1;
    while (index < t->events.len) {
        const Event& event = t->events[index];
        if (event.name == Name::HeadingAtx) {
            if (event.kind == Kind::Enter) {
                headingInside = true;
            } else {
                if (dataStart != -1) {
                    int32_t end = dataEnd;
                    Event add;
                    add.kind = Kind::Enter;
                    add.name = Name::HeadingAtxText;
                    add.point = t->events[dataStart].point;
                    EditMapAdd(t->map, dataStart, 0, &add, 1);
                    EditMapAdd(t->map, dataStart + 1, end - dataStart - 1,
                               nullptr, 0);
                    Event addExit;
                    addExit.kind = Kind::Exit;
                    addExit.name = Name::HeadingAtxText;
                    addExit.point = t->events[end].point;
                    EditMapAdd(t->map, end + 1, 0, &addExit, 1);
                }
                headingInside = false;
                dataStart = -1;
                dataEnd = -1;
            }
        } else if (headingInside && event.name == Name::Data) {
            if (event.kind == Kind::Enter) {
                if (dataStart == -1) {
                    dataStart = index;
                }
            } else {
                dataEnd = index;
            }
        }
        index += 1;
    }
    EditMapConsume(t->map, t->events);
    return false;
}

State HeadingSetextStart(Tokenizer* t) {
    if (!t->parseState->options->constructs.headingSetext || t->lazy ||
        t->pierce || t->events.len == 0) {
        return StateNok();
    }
    Name names[2] = {Name::LineEnding, Name::SpaceOrTab};
    int32_t before = SkipOptBack(t->events, t->events.len - 1, names, 2);
    Name name = t->events[before].name;
    if (name != Name::Content && name != Name::HeadingSetextUnderline) {
        return StateNok();
    }
    Enter(t, Name::HeadingSetextUnderline);
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::HeadingSetextBefore),
                         StateNok());
        return StateRetry(SpaceOrTabMinMax(t, 0, IndentMax(t)));
    }
    return StateRetry(StateName::HeadingSetextBefore);
}

State HeadingSetextBefore(Tokenizer* t) {
    if (t->current == '-' || t->current == '=') {
        t->tokenizeState.marker = (uint8_t)t->current;
        Enter(t, Name::HeadingSetextUnderlineSequence);
        return StateRetry(StateName::HeadingSetextInside);
    }
    return StateNok();
}

State HeadingSetextInside(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        Consume(t);
        return StateNext(StateName::HeadingSetextInside);
    }
    t->tokenizeState.marker = 0;
    Exit(t, Name::HeadingSetextUnderlineSequence);
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::HeadingSetextAfter),
                         StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    return StateRetry(StateName::HeadingSetextAfter);
}

State HeadingSetextAfter(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {

        t->interrupt = false;
        RegisterResolver(t, ResolveName::HeadingSetext);
        Exit(t, Name::HeadingSetextUnderline);
        return StateOk();
    }
    return StateNok();
}

bool HeadingSetextResolve(Tokenizer* t, Subresult*) {
    Name underline = Name::HeadingSetextUnderline;
    int32_t enter = SkipTo(t->events, 0, &underline, 1);
    while (enter < t->events.len) {
        int32_t exit = SkipTo(t->events, enter + 1, &underline, 1);

        Name names[3] = {Name::SpaceOrTab, Name::LineEnding,
                         Name::BlockQuotePrefix};
        int32_t paragraphExitBefore = SkipOptBack(t->events, enter - 1, names, 3);

        if (t->events[paragraphExitBefore].name == Name::Paragraph) {
            Name paragraph = Name::Paragraph;
            int32_t paragraphEnter =
                SkipToBack(t->events, paragraphExitBefore - 1, &paragraph, 1);

            t->events[paragraphEnter].name = Name::HeadingSetextText;
            t->events[paragraphExitBefore].name = Name::HeadingSetextText;

            Event headingEnter = t->events[paragraphEnter];
            headingEnter.name = Name::HeadingSetext;
            EditMapAdd(t->map, paragraphEnter, 0, &headingEnter, 1);
            Event headingExit = t->events[exit];
            headingExit.name = Name::HeadingSetext;
            EditMapAdd(t->map, exit + 1, 0, &headingExit, 1);
        } else if (exit + 3 < t->events.len &&
                   t->events[exit + 1].name == Name::LineEnding &&
                   t->events[exit + 3].name == Name::Paragraph) {

            t->events[enter].name = Name::Paragraph;
            t->events[exit + 1].name = Name::Data;
            t->events[exit + 2].name = Name::Data;
            t->events[exit + 1].point = t->events[enter].point;
            t->events[exit + 1].hasLink = true;
            t->events[exit + 1].link.previous = -1;
            t->events[exit + 1].link.next = exit + 4;
            t->events[exit + 1].link.content = ContentKind::Text;
            t->events[exit + 4].link.previous = exit + 1;
            EditMapAdd(t->map, enter + 1, exit - enter, nullptr, 0);
            EditMapAdd(t->map, exit + 3, 1, nullptr, 0);
        } else {

            t->events[enter].name = Name::Paragraph;
            t->events[exit].name = Name::Paragraph;
            Event add[2];
            add[0].name = Name::Data;
            add[0].kind = Kind::Enter;
            add[0].point = t->events[enter].point;
            add[0].hasLink = true;
            add[0].link.content = ContentKind::Text;
            add[1].name = Name::Data;
            add[1].kind = Kind::Exit;
            add[1].point = t->events[exit].point;
            EditMapAdd(t->map, enter + 1, exit - enter - 1, add, 2);
        }

        enter = SkipTo(t->events, exit + 1, &underline, 1);
    }
    EditMapConsume(t->map, t->events);
    return false;
}

State ListItemStart(Tokenizer* t) {
    if (!t->parseState->options->constructs.listItem) {
        return StateNok();
    }
    Enter(t, Name::ListItem);
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::ListItemBefore), StateNok());
        return StateRetry(SpaceOrTabMinMax(t, 0, IndentMax(t)));
    }
    return StateRetry(StateName::ListItemBefore);
}

State ListItemBefore(Tokenizer* t) {
    if (t->current == '*' || t->current == '-') {

        TokenizerCheck(t, StateNok(),
                       StateNext(StateName::ListItemBeforeUnordered));
        return StateRetry(StateName::ThematicBreakStart);
    }
    if (t->current == '+') {
        return StateRetry(StateName::ListItemBeforeUnordered);
    }

    if (t->current == '1' ||
        (t->current >= '0' && t->current <= '9' && !t->interrupt)) {
        return StateRetry(StateName::ListItemBeforeOrdered);
    }
    return StateNok();
}

State ListItemBeforeUnordered(Tokenizer* t) {
    Enter(t, Name::ListItemPrefix);
    return StateRetry(StateName::ListItemMarker);
}

State ListItemBeforeOrdered(Tokenizer* t) {
    Enter(t, Name::ListItemPrefix);
    Enter(t, Name::ListItemValue);
    return StateRetry(StateName::ListItemValue);
}

State ListItemValue(Tokenizer* t) {
    if ((t->current == '.' || t->current == ')') &&
        (!t->interrupt || t->tokenizeState.size < 2)) {
        Exit(t, Name::ListItemValue);
        return StateRetry(StateName::ListItemMarker);
    }
    if (t->current >= '0' && t->current <= '9' &&
        t->tokenizeState.size + 1 < kListItemValueSizeMax) {
        t->tokenizeState.size += 1;
        Consume(t);
        return StateNext(StateName::ListItemValue);
    }
    t->tokenizeState.size = 0;
    return StateNok();
}

State ListItemMarker(Tokenizer* t) {
    Enter(t, Name::ListItemMarker);
    Consume(t);
    Exit(t, Name::ListItemMarker);
    return StateNext(StateName::ListItemMarkerAfter);
}

State ListItemMarkerAfter(Tokenizer* t) {
    t->tokenizeState.size = 1;
    TokenizerCheck(t, StateNext(StateName::ListItemAfter),
                   StateNext(StateName::ListItemMarkerAfterFilled));
    return StateRetry(StateName::BlankLineStart);
}

State ListItemMarkerAfterFilled(Tokenizer* t) {
    t->tokenizeState.size = 0;

    TokenizerAttempt(t, StateNext(StateName::ListItemAfter),
                     StateNext(StateName::ListItemPrefixOther));
    return StateRetry(StateName::ListItemWhitespace);
}

State ListItemWhitespace(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::ListItemWhitespaceAfter),
                     StateNok());
    return StateRetry(SpaceOrTabMinMax(t, 1, kTabSize));
}

State ListItemWhitespaceAfter(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        return StateNok();
    }
    return StateOk();
}

State ListItemPrefixOther(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        Enter(t, Name::SpaceOrTab);
        Consume(t);
        Exit(t, Name::SpaceOrTab);
        return StateNext(StateName::ListItemAfter);
    }
    return StateNok();
}

State ListItemAfter(Tokenizer* t) {
    bool blank = t->tokenizeState.size == 1;
    t->tokenizeState.size = 0;

    if (blank && t->interrupt) {
        return StateNok();
    }

    Name listItem = Name::ListItem;
    int32_t start = SkipToBack(t->events, t->events.len - 1, &listItem, 1);
    Position position;
    position.start = t->events[start].point;
    position.end = t->point;
    int32_t prefix = SliceFromPosition(t->parseState->bytes, position).Len();
    if (blank) {
        prefix += 1;
    }

    ContainerState& container =
        t->tokenizeState
            .documentContainerStack[t->tokenizeState.documentContinued];
    container.blankInitial = blank;
    container.size = prefix;

    Exit(t, Name::ListItemPrefix);
    RegisterResolverBefore(t, ResolveName::ListItem);
    return StateOk();
}

State ListItemContStart(Tokenizer* t) {
    TokenizerCheck(t, StateNext(StateName::ListItemContBlank),
                   StateNext(StateName::ListItemContFilled));
    return StateRetry(StateName::BlankLineStart);
}

State ListItemContBlank(Tokenizer* t) {
    ContainerState& container =
        t->tokenizeState
            .documentContainerStack[t->tokenizeState.documentContinued];
    int32_t size = container.size;
    if (container.blankInitial) {
        return StateNok();
    }

    if (t->current == '\t' || t->current == ' ') {
        return StateRetry(SpaceOrTabMinMax(t, 0, size));
    }
    return StateOk();
}

State ListItemContFilled(Tokenizer* t) {
    ContainerState& container =
        t->tokenizeState
            .documentContainerStack[t->tokenizeState.documentContinued];
    int32_t size = container.size;
    container.blankInitial = false;

    if (t->current == '\t' || t->current == ' ') {
        return StateRetry(SpaceOrTabMinMax(t, size, size));
    }
    return StateNok();
}

struct ListWip {
    uint8_t marker = 0;
    int32_t balance = 0;
    int32_t start = 0;
    int32_t end = 0;
};

bool ListItemResolve(Tokenizer* t, Subresult*) {
    Vec<ListWip> listsWip;
    Vec<ListWip> lists;
    int32_t index = 0;
    int32_t balance = 0;

    while (index < t->events.len) {
        const Event& event = t->events[index];
        if (event.name == Name::ListItem) {
            if (event.kind == Kind::Enter) {
                Name listItem = Name::ListItem;
                int32_t end = SkipOpt(t->events, index, &listItem, 1) - 1;
                Name listItemMarker = Name::ListItemMarker;
                int32_t markerIndex =
                    SkipTo(t->events, index, &listItemMarker, 1);
                ListWip current;
                current.marker = (uint8_t)t->parseState->bytes
                                     .s[t->events[markerIndex].point.index];
                current.balance = balance;
                current.start = index;
                current.end = end;

                int32_t listIndex = listsWip.len;
                bool matched = false;
                while (listIndex > 0) {
                    listIndex -= 1;
                    const ListWip& previous = listsWip[listIndex];
                    Name names[4] = {Name::SpaceOrTab, Name::LineEnding,
                                     Name::BlankLineEnding,
                                     Name::BlockQuotePrefix};
                    int32_t before =
                        SkipOpt(t->events, previous.end + 1, names, 4);
                    if (previous.marker == current.marker &&
                        previous.balance == current.balance &&
                        before == current.start) {
                        listsWip[listIndex].end = current.end;
                        for (int32_t i = listIndex + 1; i < listsWip.len; i++) {
                            VecAppend(lists, listsWip[i]);
                        }
                        listsWip.len = listIndex + 1;
                        matched = true;
                        break;
                    }
                }

                if (!matched) {
                    int32_t i = listsWip.len;
                    int32_t exit = -1;
                    while (i > 0) {
                        i -= 1;
                        if (current.start > listsWip[i].end) {
                            exit = i;
                        } else {
                            break;
                        }
                    }
                    if (exit != -1) {
                        for (int32_t j = exit; j < listsWip.len; j++) {
                            VecAppend(lists, listsWip[j]);
                        }
                        listsWip.len = exit;
                    }
                    VecAppend(listsWip, current);
                }

                balance += 1;
            } else {
                balance -= 1;
            }
        }
        index += 1;
    }

    for (int32_t i = 0; i < listsWip.len; i++) {
        VecAppend(lists, listsWip[i]);
    }

    for (int32_t i = 0; i < lists.len; i++) {
        const ListWip& listItem = lists[i];
        Event listStart = t->events[listItem.start];
        Event listEnd = t->events[listItem.end];
        Name name = (listItem.marker == '.' || listItem.marker == ')')
                        ? Name::ListOrdered
                        : Name::ListUnordered;
        listStart.name = name;
        listEnd.name = name;
        EditMapAdd(t->map, listItem.start, 0, &listStart, 1);
        EditMapAdd(t->map, listItem.end + 1, 0, &listEnd, 1);
    }

    EditMapConsume(t->map, t->events);
    return false;
}

State DefinitionStart(Tokenizer* t) {
    if (!t->parseState->options->constructs.definition) {
        return StateNok();
    }
    if (t->interrupt) {

        if (t->events.len == 0) {
            return StateNok();
        }
        Name names[2] = {Name::LineEnding, Name::SpaceOrTab};
        int32_t before = SkipOptBack(t->events, t->events.len - 1, names, 2);
        if (t->events[before].name != Name::Definition) {
            return StateNok();
        }
    }
    Enter(t, Name::Definition);
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::DefinitionBefore), StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    return StateRetry(StateName::DefinitionBefore);
}

State DefinitionBefore(Tokenizer* t) {
    if (t->current == '[') {
        t->tokenizeState.token1 = Name::DefinitionLabel;
        t->tokenizeState.token2 = Name::DefinitionLabelMarker;
        t->tokenizeState.token3 = Name::DefinitionLabelString;
        TokenizerAttempt(t, StateNext(StateName::DefinitionLabelAfter),
                         StateNext(StateName::DefinitionLabelNok));
        return StateRetry(StateName::LabelStart);
    }
    return StateNok();
}

State DefinitionLabelAfter(Tokenizer* t) {
    t->tokenizeState.token1 = Name::Data;
    t->tokenizeState.token2 = Name::Data;
    t->tokenizeState.token3 = Name::Data;
    if (t->current == ':') {
        Name labelString = Name::DefinitionLabelString;
        t->tokenizeState.end =
            SkipToBack(t->events, t->events.len - 1, &labelString, 1);
        Enter(t, Name::DefinitionMarker);
        Consume(t);
        Exit(t, Name::DefinitionMarker);
        return StateNext(StateName::DefinitionMarkerAfter);
    }
    return StateNok();
}

State DefinitionLabelNok(Tokenizer* t) {
    t->tokenizeState.token1 = Name::Data;
    t->tokenizeState.token2 = Name::Data;
    t->tokenizeState.token3 = Name::Data;
    return StateNok();
}

State DefinitionMarkerAfter(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::DefinitionDestinationBefore),
                     StateNext(StateName::DefinitionDestinationBefore));
    return StateRetry(SpaceOrTabEol(t));
}

State DefinitionDestinationBefore(Tokenizer* t) {
    t->tokenizeState.token1 = Name::DefinitionDestination;
    t->tokenizeState.token2 = Name::DefinitionDestinationLiteral;
    t->tokenizeState.token3 = Name::DefinitionDestinationLiteralMarker;
    t->tokenizeState.token4 = Name::DefinitionDestinationRaw;
    t->tokenizeState.token5 = Name::DefinitionDestinationString;
    t->tokenizeState.sizeB = kSizeMax;
    TokenizerAttempt(t, StateNext(StateName::DefinitionDestinationAfter),
                     StateNext(StateName::DefinitionDestinationMissing));
    return StateRetry(StateName::DestinationStart);
}

State DefinitionDestinationAfter(Tokenizer* t) {
    t->tokenizeState.token1 = Name::Data;
    t->tokenizeState.token2 = Name::Data;
    t->tokenizeState.token3 = Name::Data;
    t->tokenizeState.token4 = Name::Data;
    t->tokenizeState.token5 = Name::Data;
    t->tokenizeState.sizeB = 0;
    TokenizerAttempt(t, StateNext(StateName::DefinitionAfter),
                     StateNext(StateName::DefinitionAfter));
    return StateRetry(StateName::DefinitionTitleBefore);
}

State DefinitionDestinationMissing(Tokenizer* t) {
    t->tokenizeState.token1 = Name::Data;
    t->tokenizeState.token2 = Name::Data;
    t->tokenizeState.token3 = Name::Data;
    t->tokenizeState.token4 = Name::Data;
    t->tokenizeState.token5 = Name::Data;
    t->tokenizeState.sizeB = 0;
    t->tokenizeState.end = 0;
    return StateNok();
}

State DefinitionAfter(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::DefinitionAfterWhitespace),
                         StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    return StateRetry(StateName::DefinitionAfterWhitespace);
}

State DefinitionAfterWhitespace(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        Exit(t, Name::Definition);
        Position position =
            PositionFromExitEvent(t->events, t->tokenizeState.end);
        Slice slice = SliceFromPosition(t->parseState->bytes, position);

        VecAppend(t->tokenizeState.definitions,
                  NormalizeIdentifier(t->parseState->scratch, slice.bytes));
        t->tokenizeState.end = 0;

        t->interrupt = true;
        return StateOk();
    }
    t->tokenizeState.end = 0;
    return StateNok();
}

State DefinitionTitleBefore(Tokenizer* t) {
    if (t->current == '\t' || t->current == '\n' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::DefinitionTitleBeforeMarker),
                         StateNok());
        return StateRetry(SpaceOrTabEol(t));
    }
    return StateNok();
}

State DefinitionTitleBeforeMarker(Tokenizer* t) {
    t->tokenizeState.token1 = Name::DefinitionTitle;
    t->tokenizeState.token2 = Name::DefinitionTitleMarker;
    t->tokenizeState.token3 = Name::DefinitionTitleString;
    TokenizerAttempt(t, StateNext(StateName::DefinitionTitleAfter), StateNok());
    return StateRetry(StateName::TitleStart);
}

State DefinitionTitleAfter(Tokenizer* t) {
    t->tokenizeState.token1 = Name::Data;
    t->tokenizeState.token2 = Name::Data;
    t->tokenizeState.token3 = Name::Data;
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(
            t, StateNext(StateName::DefinitionTitleAfterOptionalWhitespace),
            StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    return StateRetry(StateName::DefinitionTitleAfterOptionalWhitespace);
}

State DefinitionTitleAfterOptionalWhitespace(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        return StateOk();
    }
    return StateNok();
}

State FrontmatterStart(Tokenizer* t) {
    if (t->parseState->options->constructs.frontmatter &&
        (t->current == '+' || t->current == '-')) {
        t->tokenizeState.marker = (uint8_t)t->current;
        Enter(t, Name::Frontmatter);
        Enter(t, Name::FrontmatterFence);
        Enter(t, Name::FrontmatterSequence);
        return StateRetry(StateName::FrontmatterOpenSequence);
    }
    return StateNok();
}

State FrontmatterOpenSequence(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        t->tokenizeState.size += 1;
        Consume(t);
        return StateNext(StateName::FrontmatterOpenSequence);
    }
    if (t->tokenizeState.size == kFrontmatterSequenceSize) {
        t->tokenizeState.size = 0;
        Exit(t, Name::FrontmatterSequence);
        if (t->current == '\t' || t->current == ' ') {
            TokenizerAttempt(t, StateNext(StateName::FrontmatterOpenAfter),
                             StateNok());
            return StateRetry(SpaceOrTab(t));
        }
        return StateRetry(StateName::FrontmatterOpenAfter);
    }
    t->tokenizeState.marker = 0;
    t->tokenizeState.size = 0;
    return StateNok();
}

State FrontmatterOpenAfter(Tokenizer* t) {
    if (t->current == '\n') {
        Exit(t, Name::FrontmatterFence);
        Enter(t, Name::LineEnding);
        Consume(t);
        Exit(t, Name::LineEnding);
        TokenizerAttempt(t, StateNext(StateName::FrontmatterAfter),
                         StateNext(StateName::FrontmatterContentStart));
        return StateNext(StateName::FrontmatterCloseStart);
    }
    t->tokenizeState.marker = 0;
    return StateNok();
}

State FrontmatterCloseStart(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        Enter(t, Name::FrontmatterFence);
        Enter(t, Name::FrontmatterSequence);
        return StateRetry(StateName::FrontmatterCloseSequence);
    }
    return StateNok();
}

State FrontmatterCloseSequence(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        t->tokenizeState.size += 1;
        Consume(t);
        return StateNext(StateName::FrontmatterCloseSequence);
    }
    if (t->tokenizeState.size == kFrontmatterSequenceSize) {
        t->tokenizeState.size = 0;
        Exit(t, Name::FrontmatterSequence);
        if (t->current == '\t' || t->current == ' ') {
            TokenizerAttempt(t, StateNext(StateName::FrontmatterCloseAfter),
                             StateNok());
            return StateRetry(SpaceOrTab(t));
        }
        return StateRetry(StateName::FrontmatterCloseAfter);
    }
    t->tokenizeState.size = 0;
    return StateNok();
}

State FrontmatterCloseAfter(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        Exit(t, Name::FrontmatterFence);
        return StateOk();
    }
    return StateNok();
}

State FrontmatterContentStart(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        return StateRetry(StateName::FrontmatterContentEnd);
    }
    Enter(t, Name::FrontmatterChunk);
    return StateRetry(StateName::FrontmatterContentInside);
}

State FrontmatterContentInside(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        Exit(t, Name::FrontmatterChunk);
        return StateRetry(StateName::FrontmatterContentEnd);
    }
    Consume(t);
    return StateNext(StateName::FrontmatterContentInside);
}

State FrontmatterContentEnd(Tokenizer* t) {
    if (t->current < 0) {
        t->tokenizeState.marker = 0;
        return StateNok();
    }
    Enter(t, Name::LineEnding);
    Consume(t);
    Exit(t, Name::LineEnding);
    TokenizerAttempt(t, StateNext(StateName::FrontmatterAfter),
                     StateNext(StateName::FrontmatterContentStart));
    return StateNext(StateName::FrontmatterCloseStart);
}

State FrontmatterAfter(Tokenizer* t) {
    Exit(t, Name::Frontmatter);
    return StateOk();
}

}

#line 1 "src/markdown/construct_gfm.cpp"

namespace markdown {

State GfmLabelStartFootnoteStart(Tokenizer* t) {
    if (t->parseState->options->constructs.gfmLabelStartFootnote &&
        t->current == '[') {
        Enter(t, Name::GfmFootnoteCallLabel);
        Enter(t, Name::LabelMarker);
        Consume(t);
        Exit(t, Name::LabelMarker);
        return StateNext(StateName::GfmLabelStartFootnoteOpen);
    }
    return StateNok();
}

State GfmLabelStartFootnoteOpen(Tokenizer* t) {
    if (t->current == '^') {
        Enter(t, Name::GfmFootnoteCallMarker);
        Consume(t);
        Exit(t, Name::GfmFootnoteCallMarker);
        Exit(t, Name::GfmFootnoteCallLabel);
        LabelStartMark mark;
        mark.kind = LabelKind::GfmFootnote;
        mark.startA = t->events.len - 6;
        mark.startB = t->events.len - 1;
        VecAppend(t->tokenizeState.labelStarts, mark);
        RegisterResolverBefore(t, ResolveName::Label);
        return StateOk();
    }
    return StateNok();
}

State GfmTaskListItemCheckStart(Tokenizer* t) {
    if (t->parseState->options->constructs.gfmTaskListItem &&
        t->tokenizeState.documentAtFirstParagraphOfListItem &&
        t->current == '[' && t->previous < 0) {
        Enter(t, Name::GfmTaskListItemCheck);
        Enter(t, Name::GfmTaskListItemMarker);
        Consume(t);
        Exit(t, Name::GfmTaskListItemMarker);
        return StateNext(StateName::GfmTaskListItemCheckInside);
    }
    return StateNok();
}

State GfmTaskListItemCheckInside(Tokenizer* t) {
    if (t->current == '\t' || t->current == '\n' || t->current == ' ') {
        Enter(t, Name::GfmTaskListItemValueUnchecked);
        Consume(t);
        Exit(t, Name::GfmTaskListItemValueUnchecked);
        return StateNext(StateName::GfmTaskListItemCheckClose);
    }
    if (t->current == 'X' || t->current == 'x') {
        Enter(t, Name::GfmTaskListItemValueChecked);
        Consume(t);
        Exit(t, Name::GfmTaskListItemValueChecked);
        return StateNext(StateName::GfmTaskListItemCheckClose);
    }
    return StateNok();
}

State GfmTaskListItemCheckClose(Tokenizer* t) {
    if (t->current == ']') {
        Enter(t, Name::GfmTaskListItemMarker);
        Consume(t);
        Exit(t, Name::GfmTaskListItemMarker);
        Exit(t, Name::GfmTaskListItemCheck);
        return StateNext(StateName::GfmTaskListItemCheckAfter);
    }
    return StateNok();
}

State GfmTaskListItemCheckAfter(Tokenizer* t) {
    if (t->current == '\n') {
        return StateOk();
    }
    if (t->current == '\t' || t->current == ' ') {

        TokenizerCheck(t, StateOk(), StateNok());
        TokenizerAttempt(
            t, StateNext(StateName::GfmTaskListItemCheckAfterSpaceOrTab),
            StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    return StateNok();
}

State GfmTaskListItemCheckAfterSpaceOrTab(Tokenizer* t) {

    if (t->current < 0) {
        return StateNok();
    }
    return StateOk();
}

State GfmFootnoteDefinitionStart(Tokenizer* t) {
    if (!t->parseState->options->constructs.gfmFootnoteDefinition) {
        return StateNok();
    }
    Enter(t, Name::GfmFootnoteDefinition);
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(
            t, StateNext(StateName::GfmFootnoteDefinitionLabelBefore),
            StateNok());
        int32_t max = t->parseState->options->constructs.codeIndented
                          ? kTabSize - 1
                          : kSizeMax;
        return StateRetry(SpaceOrTabMinMax(t, 1, max));
    }
    return StateRetry(StateName::GfmFootnoteDefinitionLabelBefore);
}

State GfmFootnoteDefinitionLabelBefore(Tokenizer* t) {
    if (t->current == '[') {
        Enter(t, Name::GfmFootnoteDefinitionPrefix);
        Enter(t, Name::GfmFootnoteDefinitionLabel);
        Enter(t, Name::GfmFootnoteDefinitionLabelMarker);
        Consume(t);
        Exit(t, Name::GfmFootnoteDefinitionLabelMarker);
        return StateNext(StateName::GfmFootnoteDefinitionLabelAtMarker);
    }
    return StateNok();
}

State GfmFootnoteDefinitionLabelAtMarker(Tokenizer* t) {
    if (t->current == '^') {
        Enter(t, Name::GfmFootnoteDefinitionMarker);
        Consume(t);
        Exit(t, Name::GfmFootnoteDefinitionMarker);
        Enter(t, Name::GfmFootnoteDefinitionLabelString);
        Link link;
        link.content = ContentKind::String;
        EnterLink(t, Name::Data, link);
        return StateNext(StateName::GfmFootnoteDefinitionLabelInside);
    }
    return StateNok();
}

State GfmFootnoteDefinitionLabelInside(Tokenizer* t) {
    if (t->tokenizeState.size > kLinkReferenceSizeMax || t->current < 0 ||
        t->current == '\t' || t->current == '\n' || t->current == ' ' ||
        t->current == '[' || (t->current == ']' && t->tokenizeState.size == 0)) {
        t->tokenizeState.size = 0;
        return StateNok();
    }
    if (t->current == ']') {
        t->tokenizeState.size = 0;
        Exit(t, Name::Data);
        Exit(t, Name::GfmFootnoteDefinitionLabelString);
        Enter(t, Name::GfmFootnoteDefinitionLabelMarker);
        Consume(t);
        Exit(t, Name::GfmFootnoteDefinitionLabelMarker);
        Exit(t, Name::GfmFootnoteDefinitionLabel);
        return StateNext(StateName::GfmFootnoteDefinitionLabelAfter);
    }
    StateName next = t->current == '\\'
                         ? StateName::GfmFootnoteDefinitionLabelEscape
                         : StateName::GfmFootnoteDefinitionLabelInside;
    Consume(t);
    t->tokenizeState.size += 1;
    return StateNext(next);
}

State GfmFootnoteDefinitionLabelEscape(Tokenizer* t) {
    if (t->current == '[' || t->current == '\\' || t->current == ']') {
        t->tokenizeState.size += 1;
        Consume(t);
        return StateNext(StateName::GfmFootnoteDefinitionLabelInside);
    }
    return StateRetry(StateName::GfmFootnoteDefinitionLabelInside);
}

State GfmFootnoteDefinitionLabelAfter(Tokenizer* t) {
    if (t->current == ':') {
        Name labelString = Name::GfmFootnoteDefinitionLabelString;
        int32_t end = SkipToBack(t->events, t->events.len - 1, &labelString, 1);
        Position position = PositionFromExitEvent(t->events, end);
        Slice slice = SliceFromPosition(t->parseState->bytes, position);
        VecAppend(t->tokenizeState.gfmFootnoteDefinitions,
                  NormalizeIdentifier(t->parseState->scratch, slice.bytes));
        Enter(t, Name::DefinitionMarker);
        Consume(t);
        Exit(t, Name::DefinitionMarker);
        TokenizerAttempt(
            t, StateNext(StateName::GfmFootnoteDefinitionWhitespaceAfter),
            StateNok());
        return StateNext(SpaceOrTabMinMax(t, 0, kSizeMax));
    }
    return StateNok();
}

State GfmFootnoteDefinitionWhitespaceAfter(Tokenizer* t) {
    Exit(t, Name::GfmFootnoteDefinitionPrefix);
    return StateOk();
}

State GfmFootnoteDefinitionContStart(Tokenizer* t) {
    TokenizerCheck(t, StateNext(StateName::GfmFootnoteDefinitionContBlank),
                   StateNext(StateName::GfmFootnoteDefinitionContFilled));
    return StateRetry(StateName::BlankLineStart);
}

State GfmFootnoteDefinitionContBlank(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        return StateRetry(SpaceOrTabMinMax(t, 0, kTabSize));
    }
    return StateOk();
}

State GfmFootnoteDefinitionContFilled(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        return StateRetry(SpaceOrTabMinMax(t, kTabSize, kTabSize));
    }
    return StateNok();
}

State GfmTableStart(Tokenizer* t) {
    if (!t->parseState->options->constructs.gfmTable) {
        return StateNok();
    }
    if (!t->pierce && t->events.len > 0) {
        Name names[2] = {Name::LineEnding, Name::SpaceOrTab};
        int32_t at = SkipOptBack(t->events, t->events.len - 1, names, 2);
        Name name = t->events[at].name;
        if (name == Name::GfmTableHead || name == Name::GfmTableRow) {
            return StateRetry(StateName::GfmTableBodyRowStart);
        }
    }
    return StateRetry(StateName::GfmTableHeadRowBefore);
}

State GfmTableHeadRowBefore(Tokenizer* t) {
    Enter(t, Name::GfmTableHead);
    Enter(t, Name::GfmTableRow);
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::GfmTableHeadRowStart),
                         StateNok());
        int32_t max = t->parseState->options->constructs.codeIndented
                          ? kTabSize - 1
                          : kSizeMax;
        return StateRetry(SpaceOrTabMinMax(t, 0, max));
    }
    return StateRetry(StateName::GfmTableHeadRowStart);
}

State GfmTableHeadRowStart(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        return StateNok();
    }
    if (t->current == '|') {
        return StateRetry(StateName::GfmTableHeadRowBreak);
    }
    t->tokenizeState.seen = true;
    t->tokenizeState.sizeB += 1;
    return StateRetry(StateName::GfmTableHeadRowBreak);
}

State GfmTableHeadRowBreak(Tokenizer* t) {
    if (t->current < 0) {
        t->tokenizeState.seen = false;
        t->tokenizeState.size = 0;
        t->tokenizeState.sizeB = 0;
        return StateNok();
    }
    if (t->current == '\n') {
        if (t->tokenizeState.sizeB > 1) {
            t->tokenizeState.sizeB = 0;

            t->interrupt = true;
            Exit(t, Name::GfmTableRow);
            Enter(t, Name::LineEnding);
            Consume(t);
            Exit(t, Name::LineEnding);
            return StateNext(StateName::GfmTableHeadDelimiterStart);
        }
        t->tokenizeState.seen = false;
        t->tokenizeState.size = 0;
        t->tokenizeState.sizeB = 0;
        return StateNok();
    }
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::GfmTableHeadRowBreak),
                         StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    t->tokenizeState.sizeB += 1;
    if (t->tokenizeState.seen) {
        t->tokenizeState.seen = false;
        t->tokenizeState.size += 1;
    }
    if (t->current == '|') {
        Enter(t, Name::GfmTableCellDivider);
        Consume(t);
        Exit(t, Name::GfmTableCellDivider);
        t->tokenizeState.seen = true;
        return StateNext(StateName::GfmTableHeadRowBreak);
    }
    Enter(t, Name::Data);
    return StateRetry(StateName::GfmTableHeadRowData);
}

State GfmTableHeadRowData(Tokenizer* t) {
    if (t->current < 0 || t->current == '\t' || t->current == '\n' ||
        t->current == ' ' || t->current == '|') {
        Exit(t, Name::Data);
        return StateRetry(StateName::GfmTableHeadRowBreak);
    }
    StateName name = t->current == '\\' ? StateName::GfmTableHeadRowEscape
                                        : StateName::GfmTableHeadRowData;
    Consume(t);
    return StateNext(name);
}

State GfmTableHeadRowEscape(Tokenizer* t) {
    if (t->current == '\\' || t->current == '|') {
        Consume(t);
        return StateNext(StateName::GfmTableHeadRowData);
    }
    return StateRetry(StateName::GfmTableHeadRowData);
}

State GfmTableHeadDelimiterStart(Tokenizer* t) {

    t->interrupt = false;
    if (t->lazy || t->pierce) {
        t->tokenizeState.size = 0;
        return StateNok();
    }
    Enter(t, Name::GfmTableDelimiterRow);
    t->tokenizeState.seen = false;
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::GfmTableHeadDelimiterBefore),
                         StateNext(StateName::GfmTableHeadDelimiterNok));
        int32_t max = t->parseState->options->constructs.codeIndented
                          ? kTabSize - 1
                          : kSizeMax;
        return StateRetry(SpaceOrTabMinMax(t, 0, max));
    }
    return StateRetry(StateName::GfmTableHeadDelimiterBefore);
}

State GfmTableHeadDelimiterBefore(Tokenizer* t) {
    if (t->current == '-' || t->current == ':') {
        return StateRetry(StateName::GfmTableHeadDelimiterValueBefore);
    }
    if (t->current == '|') {
        t->tokenizeState.seen = true;
        Enter(t, Name::GfmTableCellDivider);
        Consume(t);
        Exit(t, Name::GfmTableCellDivider);
        return StateNext(StateName::GfmTableHeadDelimiterCellBefore);
    }
    return StateRetry(StateName::GfmTableHeadDelimiterNok);
}

State GfmTableHeadDelimiterCellBefore(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t,
                         StateNext(StateName::GfmTableHeadDelimiterValueBefore),
                         StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    return StateRetry(StateName::GfmTableHeadDelimiterValueBefore);
}

State GfmTableHeadDelimiterValueBefore(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        return StateRetry(StateName::GfmTableHeadDelimiterCellAfter);
    }
    if (t->current == ':') {
        t->tokenizeState.sizeB += 1;
        t->tokenizeState.seen = true;
        Enter(t, Name::GfmTableDelimiterMarker);
        Consume(t);
        Exit(t, Name::GfmTableDelimiterMarker);
        return StateNext(StateName::GfmTableHeadDelimiterLeftAlignmentAfter);
    }
    if (t->current == '-') {
        t->tokenizeState.sizeB += 1;
        return StateRetry(StateName::GfmTableHeadDelimiterLeftAlignmentAfter);
    }
    return StateRetry(StateName::GfmTableHeadDelimiterNok);
}

State GfmTableHeadDelimiterLeftAlignmentAfter(Tokenizer* t) {
    if (t->current == '-') {
        Enter(t, Name::GfmTableDelimiterFiller);
        return StateRetry(StateName::GfmTableHeadDelimiterFiller);
    }
    return StateRetry(StateName::GfmTableHeadDelimiterNok);
}

State GfmTableHeadDelimiterFiller(Tokenizer* t) {
    if (t->current == '-') {
        Consume(t);
        return StateNext(StateName::GfmTableHeadDelimiterFiller);
    }
    if (t->current == ':') {
        t->tokenizeState.seen = true;
        Exit(t, Name::GfmTableDelimiterFiller);
        Enter(t, Name::GfmTableDelimiterMarker);
        Consume(t);
        Exit(t, Name::GfmTableDelimiterMarker);
        return StateNext(StateName::GfmTableHeadDelimiterRightAlignmentAfter);
    }
    Exit(t, Name::GfmTableDelimiterFiller);
    return StateRetry(StateName::GfmTableHeadDelimiterRightAlignmentAfter);
}

State GfmTableHeadDelimiterRightAlignmentAfter(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t,
                         StateNext(StateName::GfmTableHeadDelimiterCellAfter),
                         StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    return StateRetry(StateName::GfmTableHeadDelimiterCellAfter);
}

State GfmTableHeadDelimiterCellAfter(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {

        if (!t->tokenizeState.seen ||
            t->tokenizeState.size != t->tokenizeState.sizeB) {
            return StateRetry(StateName::GfmTableHeadDelimiterNok);
        }
        t->tokenizeState.seen = false;
        t->tokenizeState.size = 0;
        t->tokenizeState.sizeB = 0;
        Exit(t, Name::GfmTableDelimiterRow);
        Exit(t, Name::GfmTableHead);
        RegisterResolver(t, ResolveName::GfmTable);
        return StateOk();
    }
    if (t->current == '|') {
        return StateRetry(StateName::GfmTableHeadDelimiterBefore);
    }
    return StateRetry(StateName::GfmTableHeadDelimiterNok);
}

State GfmTableHeadDelimiterNok(Tokenizer* t) {
    t->tokenizeState.seen = false;
    t->tokenizeState.size = 0;
    t->tokenizeState.sizeB = 0;
    return StateNok();
}

State GfmTableBodyRowStart(Tokenizer* t) {
    if (t->lazy) {
        return StateNok();
    }
    Enter(t, Name::GfmTableRow);
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::GfmTableBodyRowBreak),
                         StateNok());
        return StateRetry(SpaceOrTabMinMax(t, 0, kSizeMax));
    }
    return StateRetry(StateName::GfmTableBodyRowBreak);
}

State GfmTableBodyRowBreak(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        Exit(t, Name::GfmTableRow);
        return StateOk();
    }
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::GfmTableBodyRowBreak),
                         StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    if (t->current == '|') {
        Enter(t, Name::GfmTableCellDivider);
        Consume(t);
        Exit(t, Name::GfmTableCellDivider);
        return StateNext(StateName::GfmTableBodyRowBreak);
    }
    Enter(t, Name::Data);
    return StateRetry(StateName::GfmTableBodyRowData);
}

State GfmTableBodyRowData(Tokenizer* t) {
    if (t->current < 0 || t->current == '\t' || t->current == '\n' ||
        t->current == ' ' || t->current == '|') {
        Exit(t, Name::Data);
        return StateRetry(StateName::GfmTableBodyRowBreak);
    }
    StateName name = t->current == '\\' ? StateName::GfmTableBodyRowEscape
                                        : StateName::GfmTableBodyRowData;
    Consume(t);
    return StateNext(name);
}

State GfmTableBodyRowEscape(Tokenizer* t) {
    if (t->current == '\\' || t->current == '|') {
        Consume(t);
        return StateNext(StateName::GfmTableBodyRowData);
    }
    return StateRetry(StateName::GfmTableBodyRowData);
}

struct CellRange {
    int32_t previousEnd = 0;
    int32_t start = 0;
    int32_t valueStart = 0;
    int32_t valueEnd = 0;
};

static void FlushCell(Tokenizer* t, const CellRange& range,
                      bool inDelimiterRow, int32_t rowEnd) {
    Name groupName = inDelimiterRow ? Name::GfmTableDelimiterCell
                                    : Name::GfmTableCell;
    Name valueName = inDelimiterRow ? Name::GfmTableDelimiterCellValue
                                    : Name::GfmTableCellText;

    if (range.previousEnd != 0) {
        Event exit;
        exit.kind = Kind::Exit;
        exit.name = groupName;
        exit.point = t->events[range.previousEnd].point;
        EditMapAdd(t->map, range.previousEnd, 0, &exit, 1);
    }

    Event enter;
    enter.kind = Kind::Enter;
    enter.name = groupName;
    enter.point = t->events[range.start].point;
    EditMapAdd(t->map, range.start, 0, &enter, 1);

    if (range.valueStart != 0) {
        Event valueEnter;
        valueEnter.kind = Kind::Enter;
        valueEnter.name = valueName;
        valueEnter.point = t->events[range.valueStart].point;
        EditMapAdd(t->map, range.valueStart, 0, &valueEnter, 1);

        if (!inDelimiterRow) {
            t->events[range.valueStart].hasLink = true;
            t->events[range.valueStart].link.previous = -1;
            t->events[range.valueStart].link.next = -1;
            t->events[range.valueStart].link.content = ContentKind::Text;
            if (range.valueEnd > range.valueStart + 1) {
                int32_t a = range.valueStart + 1;
                int32_t b = range.valueEnd - range.valueStart - 1;
                EditMapAdd(t->map, a, b, nullptr, 0);
            }
        }

        Event valueExit;
        valueExit.kind = Kind::Exit;
        valueExit.name = valueName;
        valueExit.point = t->events[range.valueEnd].point;
        EditMapAdd(t->map, range.valueEnd + 1, 0, &valueExit, 1);
    }

    if (rowEnd != -1) {
        Event exit;
        exit.kind = Kind::Exit;
        exit.name = groupName;
        exit.point = t->events[rowEnd].point;
        EditMapAdd(t->map, rowEnd, 0, &exit, 1);
    }
}

static void FlushTableEnd(Tokenizer* t, int32_t index, bool body) {
    Event exits[2];
    int32_t len = 0;
    if (body) {
        exits[len].kind = Kind::Exit;
        exits[len].name = Name::GfmTableBody;
        exits[len].point = t->events[index].point;
        len++;
    }
    exits[len].kind = Kind::Exit;
    exits[len].name = Name::GfmTable;
    exits[len].point = t->events[index].point;
    len++;
    EditMapAdd(t->map, index + 1, 0, exits, len);
}

static bool IsCellValueName(Name name) {
    return name == Name::Data || name == Name::GfmTableDelimiterMarker ||
           name == Name::GfmTableDelimiterFiller;
}

bool GfmTableResolve(Tokenizer* t, Subresult*) {
    int32_t index = 0;
    bool inFirstCellAwaitingPipe = true;
    bool inRow = false;
    bool inDelimiterRow = false;
    CellRange lastCell = {};
    CellRange cell = {};
    bool afterHeadAwaitingFirstBodyRow = false;
    int32_t lastTableEnd = 0;
    bool lastTableHasBody = false;

    while (index < t->events.len) {
        const Event& event = t->events[index];
        if (event.kind == Kind::Enter) {
            if (event.name == Name::GfmTableHead) {
                afterHeadAwaitingFirstBodyRow = false;

                if (lastTableEnd != 0) {
                    FlushTableEnd(t, lastTableEnd, lastTableHasBody);
                    lastTableHasBody = false;
                    lastTableEnd = 0;
                }
                Event enter;
                enter.kind = Kind::Enter;
                enter.name = Name::GfmTable;
                enter.point = t->events[index].point;
                EditMapAdd(t->map, index, 0, &enter, 1);
            } else if (event.name == Name::GfmTableRow ||
                       event.name == Name::GfmTableDelimiterRow) {
                inDelimiterRow = event.name == Name::GfmTableDelimiterRow;
                inRow = true;
                inFirstCellAwaitingPipe = true;
                lastCell = CellRange{};
                cell = CellRange{};
                cell.start = index + 1;

                if (afterHeadAwaitingFirstBodyRow) {
                    afterHeadAwaitingFirstBodyRow = false;
                    lastTableHasBody = true;
                    Event enter;
                    enter.kind = Kind::Enter;
                    enter.name = Name::GfmTableBody;
                    enter.point = t->events[index].point;
                    EditMapAdd(t->map, index, 0, &enter, 1);
                }
            } else if (inRow && IsCellValueName(event.name)) {
                inFirstCellAwaitingPipe = false;
                if (cell.valueStart == 0) {
                    if (lastCell.start != 0) {
                        cell.previousEnd = cell.start;
                        FlushCell(t, lastCell, inDelimiterRow, -1);
                        lastCell = CellRange{};
                    }
                    cell.valueStart = index;
                }
            } else if (event.name == Name::GfmTableCellDivider) {
                if (inFirstCellAwaitingPipe) {
                    inFirstCellAwaitingPipe = false;
                } else {
                    if (lastCell.start != 0) {
                        cell.previousEnd = cell.start;
                        FlushCell(t, lastCell, inDelimiterRow, -1);
                    }
                    lastCell = cell;
                    cell = CellRange{};
                    cell.previousEnd = lastCell.start;
                    cell.start = index;
                }
            }
        } else if (event.name == Name::GfmTableHead) {
            afterHeadAwaitingFirstBodyRow = true;
            lastTableEnd = index;
        } else if (event.name == Name::GfmTableRow ||
                   event.name == Name::GfmTableDelimiterRow) {
            inRow = false;
            lastTableEnd = index;
            if (lastCell.start != 0) {
                cell.previousEnd = cell.start;
                FlushCell(t, lastCell, inDelimiterRow, index);
            } else if (cell.start != 0) {
                FlushCell(t, cell, inDelimiterRow, index);
            }
        } else if (inRow && IsCellValueName(event.name)) {
            cell.valueEnd = index;
        }
        index += 1;
    }

    if (lastTableEnd != 0) {
        FlushTableEnd(t, lastTableEnd, lastTableHasBody);
    }

    EditMapConsume(t->map, t->events);
    return false;
}

State GfmAutolinkLiteralProtocolStart(Tokenizer* t) {
    bool alphaBefore =
        t->previous >= 0 && IsAsciiAlpha((uint8_t)t->previous);
    if (t->parseState->options->constructs.gfmAutolinkLiteral &&
        (t->current == 'H' || t->current == 'h') && !alphaBefore) {
        Enter(t, Name::GfmAutolinkLiteralProtocol);
        TokenizerAttempt(t,
                         StateNext(StateName::GfmAutolinkLiteralProtocolAfter),
                         StateNok());
        TokenizerAttempt(t,
                         StateNext(StateName::GfmAutolinkLiteralDomainInside),
                         StateNok());
        t->tokenizeState.start = t->point.index;
        return StateRetry(StateName::GfmAutolinkLiteralProtocolPrefixInside);
    }
    return StateNok();
}

State GfmAutolinkLiteralProtocolAfter(Tokenizer* t) {
    Exit(t, Name::GfmAutolinkLiteralProtocol);
    return StateOk();
}

State GfmAutolinkLiteralProtocolPrefixInside(Tokenizer* t) {
    if (t->current >= 0 && IsAsciiAlpha((uint8_t)t->current) &&
        t->point.index - t->tokenizeState.start < 5) {
        Consume(t);
        return StateNext(StateName::GfmAutolinkLiteralProtocolPrefixInside);
    }
    if (t->current == ':') {
        Slice slice = SliceFromIndices(t->parseState->bytes,
                                       t->tokenizeState.start, t->point.index);
        t->tokenizeState.start = 0;
        if (base::StrEqI(slice.bytes, "http") ||
            base::StrEqI(slice.bytes, "https")) {
            Consume(t);
            return StateNext(
                StateName::GfmAutolinkLiteralProtocolSlashesInside);
        }
        return StateNok();
    }
    t->tokenizeState.start = 0;
    return StateNok();
}

State GfmAutolinkLiteralProtocolSlashesInside(Tokenizer* t) {
    if (t->current == '/') {
        Consume(t);
        if (t->tokenizeState.size == 0) {
            t->tokenizeState.size += 1;
            return StateNext(
                StateName::GfmAutolinkLiteralProtocolSlashesInside);
        }
        t->tokenizeState.size = 0;
        return StateOk();
    }
    t->tokenizeState.size = 0;
    return StateNok();
}

State GfmAutolinkLiteralWwwStart(Tokenizer* t) {
    int32_t p = t->previous;
    bool okBefore = p < 0 || p == '\t' || p == '\n' || p == ' ' || p == '(' ||
                    p == '*' || p == '_' || p == '[' || p == ']' || p == '~';
    if (t->parseState->options->constructs.gfmAutolinkLiteral &&
        (t->current == 'W' || t->current == 'w') && okBefore) {
        Enter(t, Name::GfmAutolinkLiteralWww);
        TokenizerAttempt(t, StateNext(StateName::GfmAutolinkLiteralWwwAfter),
                         StateNok());

        TokenizerCheck(t, StateNext(StateName::GfmAutolinkLiteralDomainInside),
                       StateNok());
        return StateRetry(StateName::GfmAutolinkLiteralWwwPrefixInside);
    }
    return StateNok();
}

State GfmAutolinkLiteralWwwAfter(Tokenizer* t) {
    Exit(t, Name::GfmAutolinkLiteralWww);
    return StateOk();
}

State GfmAutolinkLiteralWwwPrefixInside(Tokenizer* t) {
    if (t->current == '.' && t->tokenizeState.size == 3) {
        t->tokenizeState.size = 0;
        Consume(t);
        return StateNext(StateName::GfmAutolinkLiteralWwwPrefixAfter);
    }
    if ((t->current == 'W' || t->current == 'w') && t->tokenizeState.size < 3) {
        t->tokenizeState.size += 1;
        Consume(t);
        return StateNext(StateName::GfmAutolinkLiteralWwwPrefixInside);
    }
    t->tokenizeState.size = 0;
    return StateNok();
}

State GfmAutolinkLiteralWwwPrefixAfter(Tokenizer* t) {

    if (t->current < 0) {
        return StateNok();
    }
    return StateOk();
}

State GfmAutolinkLiteralDomainInside(Tokenizer* t) {
    if (t->current == '.' || t->current == '_') {
        TokenizerCheck(t, StateNext(StateName::GfmAutolinkLiteralDomainAfter),
                       StateNext(
                           StateName::GfmAutolinkLiteralDomainAtPunctuation));
        return StateRetry(StateName::GfmAutolinkLiteralTrail);
    }

    if (t->current == '-' || (t->current >= 0x80 && t->current <= 0xbf)) {
        Consume(t);
        return StateNext(StateName::GfmAutolinkLiteralDomainInside);
    }
    if (KindAfterIndex(t->parseState->bytes, t->point.index) ==
        CharKind::Other) {
        t->tokenizeState.seen = true;
        Consume(t);
        return StateNext(StateName::GfmAutolinkLiteralDomainInside);
    }
    return StateRetry(StateName::GfmAutolinkLiteralDomainAfter);
}

State GfmAutolinkLiteralDomainAtPunctuation(Tokenizer* t) {

    if (t->current == '_') {
        t->tokenizeState.marker = '_';
    } else {

        t->tokenizeState.markerB = t->tokenizeState.marker;
        t->tokenizeState.marker = 0;
    }
    Consume(t);
    return StateNext(StateName::GfmAutolinkLiteralDomainInside);
}

State GfmAutolinkLiteralDomainAfter(Tokenizer* t) {
    State result;
    if (t->tokenizeState.markerB == '_' || t->tokenizeState.marker == '_' ||
        !t->tokenizeState.seen) {
        result = StateNok();
    } else {
        result = StateRetry(StateName::GfmAutolinkLiteralPathInside);
    }
    t->tokenizeState.seen = false;
    t->tokenizeState.marker = 0;
    t->tokenizeState.markerB = 0;
    return result;
}

State GfmAutolinkLiteralPathInside(Tokenizer* t) {
    if (t->current >= 0x80 && t->current <= 0xbf) {
        Consume(t);
        return StateNext(StateName::GfmAutolinkLiteralPathInside);
    }
    if (t->current == '(') {
        t->tokenizeState.size += 1;
        Consume(t);
        return StateNext(StateName::GfmAutolinkLiteralPathInside);
    }
    int32_t c = t->current;
    bool trailing = c == '!' || c == '"' || c == '&' || c == '\'' ||
                    c == ')' || c == '*' || c == ',' || c == '.' ||
                    c == ':' || c == ';' || c == '<' || c == '?' ||
                    c == ']' || c == '_' || c == '~';
    if (trailing) {
        StateName next = StateName::GfmAutolinkLiteralPathAfter;
        if (c == ')' && t->tokenizeState.sizeB < t->tokenizeState.size) {
            next = StateName::GfmAutolinkLiteralPathAtPunctuation;
        }
        TokenizerCheck(
            t, StateNext(next),
            StateNext(StateName::GfmAutolinkLiteralPathAtPunctuation));
        return StateRetry(StateName::GfmAutolinkLiteralTrail);
    }
    if (t->current < 0 ||
        KindAfterIndex(t->parseState->bytes, t->point.index) ==
            CharKind::Whitespace) {
        return StateRetry(StateName::GfmAutolinkLiteralPathAfter);
    }
    Consume(t);
    return StateNext(StateName::GfmAutolinkLiteralPathInside);
}

State GfmAutolinkLiteralPathAtPunctuation(Tokenizer* t) {
    if (t->current == ')') {
        t->tokenizeState.sizeB += 1;
    }
    Consume(t);
    return StateNext(StateName::GfmAutolinkLiteralPathInside);
}

State GfmAutolinkLiteralPathAfter(Tokenizer* t) {
    t->tokenizeState.size = 0;
    t->tokenizeState.sizeB = 0;
    return StateOk();
}

State GfmAutolinkLiteralTrail(Tokenizer* t) {
    int32_t c = t->current;
    bool trailing = c == '!' || c == '"' || c == '\'' || c == ')' ||
                    c == '*' || c == ',' || c == '.' || c == ':' ||
                    c == ';' || c == '?' || c == '_' || c == '~';
    if (trailing) {
        Consume(t);
        return StateNext(StateName::GfmAutolinkLiteralTrail);
    }
    if (c == '&') {
        Consume(t);
        return StateNext(StateName::GfmAutolinkLiteralTrailCharRefStart);
    }
    if (c == '<') {
        return StateOk();
    }
    if (c == ']') {
        Consume(t);
        return StateNext(StateName::GfmAutolinkLiteralTrailBracketAfter);
    }
    if (KindAfterIndex(t->parseState->bytes, t->point.index) ==
        CharKind::Whitespace) {
        return StateOk();
    }
    return StateNok();
}

State GfmAutolinkLiteralTrailBracketAfter(Tokenizer* t) {
    int32_t c = t->current;
    if (c < 0 || c == '\t' || c == '\n' || c == ' ' || c == '(' || c == '[') {
        return StateOk();
    }
    return StateRetry(StateName::GfmAutolinkLiteralTrail);
}

State GfmAutolinkLiteralTrailCharRefStart(Tokenizer* t) {
    if (t->current >= 0 && IsAsciiAlpha((uint8_t)t->current)) {
        return StateRetry(StateName::GfmAutolinkLiteralTrailCharRefInside);
    }
    return StateNok();
}

State GfmAutolinkLiteralTrailCharRefInside(Tokenizer* t) {
    if (t->current >= 0 && IsAsciiAlpha((uint8_t)t->current)) {
        Consume(t);
        return StateNext(StateName::GfmAutolinkLiteralTrailCharRefInside);
    }
    if (t->current == ';') {
        Consume(t);
        return StateNext(StateName::GfmAutolinkLiteralTrail);
    }
    return StateNok();
}

static int32_t PeekBytesAtext(Str bytes, int32_t min, int32_t end) {
    int32_t index = end;
    while (index > min) {
        uint8_t byte = (uint8_t)bytes.s[index - 1];
        bool atext = byte == '+' || byte == '-' || byte == '.' ||
                     byte == '_' || IsAsciiAlphanumeric(byte);
        if (!atext) {
            break;
        }
        index -= 1;
    }
    if (index == end || (index > min && bytes.s[index - 1] == '/')) {
        return -1;
    }
    return index;
}

static int32_t PeekProtocol(Str bytes, int32_t min, int32_t end, Name* name) {
    *name = Name::GfmAutolinkLiteralEmail;
    int32_t index = end;
    if (index > min && bytes.s[index - 1] == ':') {
        index -= 1;
        while (index > min &&
               IsAsciiAlphanumeric((uint8_t)bytes.s[index - 1])) {
            index -= 1;
        }
        Slice slice = SliceFromIndices(bytes, index, end - 1);
        if (base::StrEqI(slice.bytes, "xmpp")) {
            *name = Name::GfmAutolinkLiteralXmpp;
            return index;
        }
        if (base::StrEqI(slice.bytes, "mailto")) {
            *name = Name::GfmAutolinkLiteralMailto;
            return index;
        }
    }
    return end;
}

static int32_t PeekBytesEmailDomain(Str bytes, int32_t start, bool xmpp) {
    int32_t index = start;
    bool dot = false;
    while (index < bytes.len) {
        uint8_t byte = (uint8_t)bytes.s[index];
        if (byte == '-' || byte == '_' || IsAsciiAlphanumeric(byte) ||
            (byte == '/' && xmpp)) {

        } else if (byte == '.' && index + 1 < bytes.len &&
                   IsAsciiAlphanumeric((uint8_t)bytes.s[index + 1])) {
            dot = true;
        } else {
            break;
        }
        index += 1;
    }
    if (index > start && dot) {
        uint8_t last = (uint8_t)bytes.s[index - 1];
        if (last == '.' || IsAsciiAlpha(last)) {
            return index;
        }
    }
    return -1;
}

void GfmAutolinkLiteralResolve(Tokenizer* t) {
    EditMapConsume(t->map, t->events);
    Arena* a = t->parseState->scratch;

    int32_t index = 0;
    int32_t links = 0;
    while (index < t->events.len) {
        const Event& event = t->events[index];
        if (event.kind == Kind::Enter) {
            if (event.name == Name::Link) {
                links += 1;
            }
        } else {
            if (event.name == Name::Data && links == 0) {
                Position position = PositionFromExitEvent(t->events, index);
                Slice slice =
                    SliceFromPosition(t->parseState->bytes, position);
                Str bytes = slice.bytes;
                int32_t byteIndex = 0;
                ArenaVec<Event> replace {};
                Point point = t->events[index - 1].point;
                int32_t startIndex = point.index;
                int32_t min = 0;

                while (byteIndex < bytes.len) {
                    if (bytes.s[byteIndex] == '@') {
                        int32_t rangeStart = 0;
                        int32_t rangeEnd = 0;
                        Name rangeName = Name::GfmAutolinkLiteralEmail;
                        int32_t start = PeekBytesAtext(bytes, min, byteIndex);
                        if (start != -1) {
                            Name kind;
                            start = PeekProtocol(bytes, min, start, &kind);
                            int32_t end = PeekBytesEmailDomain(
                                bytes, byteIndex + 1,
                                kind == Name::GfmAutolinkLiteralXmpp);
                            if (end != -1) {
                                rangeStart = start;
                                rangeEnd = end;
                                rangeName = kind;
                            }
                        }

                        if (rangeEnd != 0) {
                            byteIndex = rangeEnd;

                            if (min != rangeStart) {
                                Event enter;
                                enter.kind = Kind::Enter;
                                enter.name = Name::Data;
                                enter.point = point;
                                replace.Append(a, enter);
                                point = PointShiftTo(point,
                                                     t->parseState->bytes,
                                                     startIndex + rangeStart);
                                Event exit;
                                exit.kind = Kind::Exit;
                                exit.name = Name::Data;
                                exit.point = point;
                                replace.Append(a, exit);
                            }
                            Event enter;
                            enter.kind = Kind::Enter;
                            enter.name = rangeName;
                            enter.point = point;
                            replace.Append(a, enter);
                            point = PointShiftTo(point, t->parseState->bytes,
                                                 startIndex + rangeEnd);
                            Event exit;
                            exit.kind = Kind::Exit;
                            exit.name = rangeName;
                            exit.point = point;
                            replace.Append(a, exit);
                            min = rangeEnd;
                        }
                    }
                    byteIndex += 1;
                }

                if (min != 0 && min < bytes.len) {
                    Event enter;
                    enter.kind = Kind::Enter;
                    enter.name = Name::Data;
                    enter.point = point;
                    replace.Append(a, enter);
                    Event exit;
                    exit.kind = Kind::Exit;
                    exit.name = Name::Data;
                    exit.point = t->events[index].point;
                    replace.Append(a, exit);
                }

                if (replace.len > 0) {
                    EditMapAdd(t->map, index - 1, 2, replace.Flatten(a),
                               replace.len);
                }
            }
            if (event.name == Name::Link) {
                links -= 1;
            }
        }
        index += 1;
    }
}

}

#line 1 "src/markdown/construct_html.cpp"

namespace markdown {

static const uint8_t kHtmlRaw = 1;
static const uint8_t kHtmlComment = 2;
static const uint8_t kHtmlInstruction = 3;
static const uint8_t kHtmlDeclaration = 4;
static const uint8_t kHtmlCdata = 5;
static const uint8_t kHtmlBasic = 6;
static const uint8_t kHtmlComplete = 7;

static bool NamesContainI(SeqStrings names, Str name) {
    for (Str item = SeqStrFirst(names); item.len > 0; item = SeqStrNext(item)) {
        if (base::StrEqI(item, name)) {
            return true;
        }
    }
    return false;
}

State HtmlFlowStart(Tokenizer* t) {
    if (!t->parseState->options->constructs.htmlFlow) {
        return StateNok();
    }
    Enter(t, Name::HtmlFlow);
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::HtmlFlowBefore), StateNok());
        SpaceOrTabOptions options;
        options.kind = Name::HtmlFlowData;
        options.min = 0;
        options.max = t->parseState->options->constructs.codeIndented
                          ? kTabSize - 1
                          : kSizeMax;
        return StateRetry(SpaceOrTabWithOptions(t, options));
    }
    return StateRetry(StateName::HtmlFlowBefore);
}

State HtmlFlowBefore(Tokenizer* t) {
    if (t->current == '<') {
        Enter(t, Name::HtmlFlowData);
        Consume(t);
        return StateNext(StateName::HtmlFlowOpen);
    }
    return StateNok();
}

State HtmlFlowOpen(Tokenizer* t) {
    if (t->current == '!') {
        Consume(t);
        return StateNext(StateName::HtmlFlowDeclarationOpen);
    }
    if (t->current == '/') {
        Consume(t);
        t->tokenizeState.seen = true;
        t->tokenizeState.start = t->point.index;
        return StateNext(StateName::HtmlFlowTagCloseStart);
    }
    if (t->current == '?') {
        Consume(t);
        t->tokenizeState.marker = kHtmlInstruction;

        t->concrete = true;
        return StateNext(StateName::HtmlFlowContinuationDeclarationInside);
    }
    if (t->current >= 0 && IsAsciiAlpha((uint8_t)t->current)) {
        t->tokenizeState.start = t->point.index;
        return StateRetry(StateName::HtmlFlowTagName);
    }
    return StateNok();
}

State HtmlFlowDeclarationOpen(Tokenizer* t) {
    if (t->current == '-') {
        Consume(t);
        t->tokenizeState.marker = kHtmlComment;
        return StateNext(StateName::HtmlFlowCommentOpenInside);
    }
    if (t->current >= 0 && IsAsciiAlpha((uint8_t)t->current)) {
        Consume(t);
        t->tokenizeState.marker = kHtmlDeclaration;
        t->concrete = true;
        return StateNext(StateName::HtmlFlowContinuationDeclarationInside);
    }
    if (t->current == '[') {
        Consume(t);
        t->tokenizeState.marker = kHtmlCdata;
        return StateNext(StateName::HtmlFlowCdataOpenInside);
    }
    return StateNok();
}

State HtmlFlowCommentOpenInside(Tokenizer* t) {
    if (t->current == '-') {
        Consume(t);
        t->concrete = true;
        return StateNext(StateName::HtmlFlowContinuationDeclarationInside);
    }
    t->tokenizeState.marker = 0;
    return StateNok();
}

State HtmlFlowCdataOpenInside(Tokenizer* t) {
    if (t->current == (int32_t)(uint8_t)kHtmlCdataPrefix.s[t->tokenizeState.size]) {
        Consume(t);
        t->tokenizeState.size += 1;
        if (t->tokenizeState.size == kHtmlCdataPrefix.len) {
            t->tokenizeState.size = 0;
            t->concrete = true;
            return StateNext(StateName::HtmlFlowContinuation);
        }
        return StateNext(StateName::HtmlFlowCdataOpenInside);
    }
    t->tokenizeState.marker = 0;
    t->tokenizeState.size = 0;
    return StateNok();
}

State HtmlFlowTagCloseStart(Tokenizer* t) {
    if (t->current >= 0 && IsAsciiAlpha((uint8_t)t->current)) {
        Consume(t);
        return StateNext(StateName::HtmlFlowTagName);
    }
    t->tokenizeState.seen = false;
    t->tokenizeState.start = 0;
    return StateNok();
}

State HtmlFlowTagName(Tokenizer* t) {
    if (t->current < 0 || t->current == '\t' || t->current == '\n' ||
        t->current == ' ' || t->current == '/' || t->current == '>') {
        bool closingTag = t->tokenizeState.seen;
        bool slash = t->current == '/';
        Slice slice = SliceFromIndices(t->parseState->bytes,
                                       t->tokenizeState.start, t->point.index);
        Str name = base::StrTrimAscii(slice.bytes);
        t->tokenizeState.seen = false;
        t->tokenizeState.start = 0;

        if (!slash && !closingTag && NamesContainI(kHtmlRawNames, name)) {
            t->tokenizeState.marker = kHtmlRaw;
            t->concrete = true;
            return StateRetry(StateName::HtmlFlowContinuation);
        }
        if (NamesContainI(kHtmlBlockNames, name)) {
            t->tokenizeState.marker = kHtmlBasic;
            if (slash) {
                Consume(t);
                return StateNext(StateName::HtmlFlowBasicSelfClosing);
            }
            t->concrete = true;
            return StateRetry(StateName::HtmlFlowContinuation);
        }
        t->tokenizeState.marker = kHtmlComplete;

        if (t->interrupt && !t->lazy) {
            t->tokenizeState.marker = 0;
            return StateNok();
        }
        if (closingTag) {
            return StateRetry(StateName::HtmlFlowCompleteClosingTagAfter);
        }
        return StateRetry(StateName::HtmlFlowCompleteAttributeNameBefore);
    }
    if (t->current == '-' ||
        (t->current >= 0 && IsAsciiAlphanumeric((uint8_t)t->current))) {
        Consume(t);
        return StateNext(StateName::HtmlFlowTagName);
    }
    t->tokenizeState.seen = false;
    return StateNok();
}

State HtmlFlowBasicSelfClosing(Tokenizer* t) {
    if (t->current == '>') {
        Consume(t);
        t->concrete = true;
        return StateNext(StateName::HtmlFlowContinuation);
    }
    t->tokenizeState.marker = 0;
    return StateNok();
}

State HtmlFlowCompleteClosingTagAfter(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        Consume(t);
        return StateNext(StateName::HtmlFlowCompleteClosingTagAfter);
    }
    return StateRetry(StateName::HtmlFlowCompleteEnd);
}

State HtmlFlowCompleteAttributeNameBefore(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        Consume(t);
        return StateNext(StateName::HtmlFlowCompleteAttributeNameBefore);
    }
    if (t->current == '/') {
        Consume(t);
        return StateNext(StateName::HtmlFlowCompleteEnd);
    }
    if (t->current == ':' || t->current == '_' ||
        (t->current >= 0 && (IsAsciiDigit((uint8_t)t->current) ||
                             IsAsciiAlpha((uint8_t)t->current)))) {
        Consume(t);
        return StateNext(StateName::HtmlFlowCompleteAttributeName);
    }
    return StateRetry(StateName::HtmlFlowCompleteEnd);
}

State HtmlFlowCompleteAttributeName(Tokenizer* t) {
    if (t->current == '-' || t->current == '.' || t->current == ':' ||
        t->current == '_' ||
        (t->current >= 0 && IsAsciiAlphanumeric((uint8_t)t->current))) {
        Consume(t);
        return StateNext(StateName::HtmlFlowCompleteAttributeName);
    }
    return StateRetry(StateName::HtmlFlowCompleteAttributeNameAfter);
}

State HtmlFlowCompleteAttributeNameAfter(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        Consume(t);
        return StateNext(StateName::HtmlFlowCompleteAttributeNameAfter);
    }
    if (t->current == '=') {
        Consume(t);
        return StateNext(StateName::HtmlFlowCompleteAttributeValueBefore);
    }
    return StateRetry(StateName::HtmlFlowCompleteAttributeNameBefore);
}

State HtmlFlowCompleteAttributeValueBefore(Tokenizer* t) {
    if (t->current < 0 || t->current == '<' || t->current == '=' ||
        t->current == '>' || t->current == '`') {
        t->tokenizeState.marker = 0;
        return StateNok();
    }
    if (t->current == '\t' || t->current == ' ') {
        Consume(t);
        return StateNext(StateName::HtmlFlowCompleteAttributeValueBefore);
    }
    if (t->current == '"' || t->current == '\'') {
        t->tokenizeState.markerB = (uint8_t)t->current;
        Consume(t);
        return StateNext(StateName::HtmlFlowCompleteAttributeValueQuoted);
    }
    return StateRetry(StateName::HtmlFlowCompleteAttributeValueUnquoted);
}

State HtmlFlowCompleteAttributeValueQuoted(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.markerB) {
        Consume(t);
        t->tokenizeState.markerB = 0;
        return StateNext(StateName::HtmlFlowCompleteAttributeValueQuotedAfter);
    }
    if (t->current < 0 || t->current == '\n') {
        t->tokenizeState.marker = 0;
        t->tokenizeState.markerB = 0;
        return StateNok();
    }
    Consume(t);
    return StateNext(StateName::HtmlFlowCompleteAttributeValueQuoted);
}

State HtmlFlowCompleteAttributeValueUnquoted(Tokenizer* t) {
    if (t->current < 0 || t->current == '\t' || t->current == '\n' ||
        t->current == ' ' || t->current == '"' || t->current == '\'' ||
        t->current == '/' || t->current == '<' || t->current == '=' ||
        t->current == '>' || t->current == '`') {
        return StateRetry(StateName::HtmlFlowCompleteAttributeNameAfter);
    }
    Consume(t);
    return StateNext(StateName::HtmlFlowCompleteAttributeValueUnquoted);
}

State HtmlFlowCompleteAttributeValueQuotedAfter(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ' || t->current == '/' ||
        t->current == '>') {
        return StateRetry(StateName::HtmlFlowCompleteAttributeNameBefore);
    }
    t->tokenizeState.marker = 0;
    return StateNok();
}

State HtmlFlowCompleteEnd(Tokenizer* t) {
    if (t->current == '>') {
        Consume(t);
        return StateNext(StateName::HtmlFlowCompleteAfter);
    }
    t->tokenizeState.marker = 0;
    return StateNok();
}

State HtmlFlowCompleteAfter(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {

        t->concrete = true;
        return StateRetry(StateName::HtmlFlowContinuation);
    }
    if (t->current == '\t' || t->current == ' ') {
        Consume(t);
        return StateNext(StateName::HtmlFlowCompleteAfter);
    }
    t->tokenizeState.marker = 0;
    return StateNok();
}

State HtmlFlowContinuation(Tokenizer* t) {
    uint8_t marker = t->tokenizeState.marker;
    if (marker == kHtmlComment && t->current == '-') {
        Consume(t);
        return StateNext(StateName::HtmlFlowContinuationCommentInside);
    }
    if (marker == kHtmlRaw && t->current == '<') {
        Consume(t);
        return StateNext(StateName::HtmlFlowContinuationRawTagOpen);
    }
    if (marker == kHtmlDeclaration && t->current == '>') {
        Consume(t);
        return StateNext(StateName::HtmlFlowContinuationClose);
    }
    if (marker == kHtmlInstruction && t->current == '?') {
        Consume(t);
        return StateNext(StateName::HtmlFlowContinuationDeclarationInside);
    }
    if (marker == kHtmlCdata && t->current == ']') {
        Consume(t);
        return StateNext(StateName::HtmlFlowContinuationCdataInside);
    }
    if ((marker == kHtmlBasic || marker == kHtmlComplete) &&
        t->current == '\n') {
        Exit(t, Name::HtmlFlowData);
        TokenizerCheck(t, StateNext(StateName::HtmlFlowContinuationAfter),
                       StateNext(StateName::HtmlFlowContinuationStart));
        return StateRetry(StateName::HtmlFlowBlankLineBefore);
    }
    if (t->current < 0 || t->current == '\n') {
        Exit(t, Name::HtmlFlowData);
        return StateRetry(StateName::HtmlFlowContinuationStart);
    }
    Consume(t);
    return StateNext(StateName::HtmlFlowContinuation);
}

State HtmlFlowContinuationStart(Tokenizer* t) {
    TokenizerCheck(t, StateNext(StateName::HtmlFlowContinuationStartNonLazy),
                   StateNext(StateName::HtmlFlowContinuationAfter));
    return StateRetry(StateName::NonLazyContinuationStart);
}

State HtmlFlowContinuationStartNonLazy(Tokenizer* t) {
    Enter(t, Name::LineEnding);
    Consume(t);
    Exit(t, Name::LineEnding);
    return StateNext(StateName::HtmlFlowContinuationBefore);
}

State HtmlFlowContinuationBefore(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        return StateRetry(StateName::HtmlFlowContinuationStart);
    }
    Enter(t, Name::HtmlFlowData);
    return StateRetry(StateName::HtmlFlowContinuation);
}

State HtmlFlowContinuationCommentInside(Tokenizer* t) {
    if (t->current == '-') {
        Consume(t);
        return StateNext(StateName::HtmlFlowContinuationDeclarationInside);
    }
    return StateRetry(StateName::HtmlFlowContinuation);
}

State HtmlFlowContinuationRawTagOpen(Tokenizer* t) {
    if (t->current == '/') {
        Consume(t);
        t->tokenizeState.start = t->point.index;
        return StateNext(StateName::HtmlFlowContinuationRawEndTag);
    }
    return StateRetry(StateName::HtmlFlowContinuation);
}

State HtmlFlowContinuationRawEndTag(Tokenizer* t) {
    if (t->current == '>') {
        Slice slice = SliceFromIndices(t->parseState->bytes,
                                       t->tokenizeState.start, t->point.index);
        t->tokenizeState.start = 0;
        if (NamesContainI(kHtmlRawNames, slice.bytes)) {
            Consume(t);
            return StateNext(StateName::HtmlFlowContinuationClose);
        }
        return StateRetry(StateName::HtmlFlowContinuation);
    }
    if (t->current >= 0 && IsAsciiAlpha((uint8_t)t->current) &&
        t->point.index - t->tokenizeState.start < kHtmlRawSizeMax) {
        Consume(t);
        return StateNext(StateName::HtmlFlowContinuationRawEndTag);
    }
    t->tokenizeState.start = 0;
    return StateRetry(StateName::HtmlFlowContinuation);
}

State HtmlFlowContinuationCdataInside(Tokenizer* t) {
    if (t->current == ']') {
        Consume(t);
        return StateNext(StateName::HtmlFlowContinuationDeclarationInside);
    }
    return StateRetry(StateName::HtmlFlowContinuation);
}

State HtmlFlowContinuationDeclarationInside(Tokenizer* t) {
    if (t->tokenizeState.marker == kHtmlComment && t->current == '-') {
        Consume(t);
        return StateNext(StateName::HtmlFlowContinuationDeclarationInside);
    }
    if (t->current == '>') {
        Consume(t);
        return StateNext(StateName::HtmlFlowContinuationClose);
    }
    return StateRetry(StateName::HtmlFlowContinuation);
}

State HtmlFlowContinuationClose(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        Exit(t, Name::HtmlFlowData);
        return StateRetry(StateName::HtmlFlowContinuationAfter);
    }
    Consume(t);
    return StateNext(StateName::HtmlFlowContinuationClose);
}

State HtmlFlowContinuationAfter(Tokenizer* t) {
    Exit(t, Name::HtmlFlow);
    t->tokenizeState.marker = 0;
    t->interrupt = false;

    t->concrete = false;
    return StateOk();
}

State HtmlFlowBlankLineBefore(Tokenizer* t) {
    Enter(t, Name::LineEnding);
    Consume(t);
    Exit(t, Name::LineEnding);
    return StateNext(StateName::BlankLineStart);
}

State HtmlTextStart(Tokenizer* t) {
    if (t->current == '<' && t->parseState->options->constructs.htmlText) {
        Enter(t, Name::HtmlText);
        Enter(t, Name::HtmlTextData);
        Consume(t);
        return StateNext(StateName::HtmlTextOpen);
    }
    return StateNok();
}

State HtmlTextOpen(Tokenizer* t) {
    if (t->current == '!') {
        Consume(t);
        return StateNext(StateName::HtmlTextDeclarationOpen);
    }
    if (t->current == '/') {
        Consume(t);
        return StateNext(StateName::HtmlTextTagCloseStart);
    }
    if (t->current == '?') {
        Consume(t);
        return StateNext(StateName::HtmlTextInstruction);
    }
    if (t->current >= 0 && IsAsciiAlpha((uint8_t)t->current)) {
        Consume(t);
        return StateNext(StateName::HtmlTextTagOpen);
    }
    return StateNok();
}

State HtmlTextDeclarationOpen(Tokenizer* t) {
    if (t->current == '-') {
        Consume(t);
        return StateNext(StateName::HtmlTextCommentOpenInside);
    }
    if (t->current >= 0 && IsAsciiAlpha((uint8_t)t->current)) {
        Consume(t);
        return StateNext(StateName::HtmlTextDeclaration);
    }
    if (t->current == '[') {
        Consume(t);
        return StateNext(StateName::HtmlTextCdataOpenInside);
    }
    return StateNok();
}

State HtmlTextCommentOpenInside(Tokenizer* t) {
    if (t->current == '-') {
        Consume(t);
        return StateNext(StateName::HtmlTextCommentEnd);
    }
    return StateNok();
}

State HtmlTextComment(Tokenizer* t) {
    if (t->current < 0) {
        return StateNok();
    }
    if (t->current == '\n') {
        TokenizerAttempt(t, StateNext(StateName::HtmlTextComment), StateNok());
        return StateRetry(StateName::HtmlTextLineEndingBefore);
    }
    if (t->current == '-') {
        Consume(t);
        return StateNext(StateName::HtmlTextCommentClose);
    }
    Consume(t);
    return StateNext(StateName::HtmlTextComment);
}

State HtmlTextCommentClose(Tokenizer* t) {
    if (t->current == '-') {
        Consume(t);
        return StateNext(StateName::HtmlTextCommentEnd);
    }
    return StateRetry(StateName::HtmlTextComment);
}

State HtmlTextCommentEnd(Tokenizer* t) {
    if (t->current == '>') {
        return StateRetry(StateName::HtmlTextEnd);
    }
    if (t->current == '-') {
        return StateRetry(StateName::HtmlTextCommentClose);
    }
    return StateRetry(StateName::HtmlTextComment);
}

State HtmlTextCdataOpenInside(Tokenizer* t) {
    if (t->current == (int32_t)(uint8_t)kHtmlCdataPrefix.s[t->tokenizeState.size]) {
        t->tokenizeState.size += 1;
        Consume(t);
        if (t->tokenizeState.size == kHtmlCdataPrefix.len) {
            t->tokenizeState.size = 0;
            return StateNext(StateName::HtmlTextCdata);
        }
        return StateNext(StateName::HtmlTextCdataOpenInside);
    }
    return StateNok();
}

State HtmlTextCdata(Tokenizer* t) {
    if (t->current < 0) {
        return StateNok();
    }
    if (t->current == '\n') {
        TokenizerAttempt(t, StateNext(StateName::HtmlTextCdata), StateNok());
        return StateRetry(StateName::HtmlTextLineEndingBefore);
    }
    if (t->current == ']') {
        Consume(t);
        return StateNext(StateName::HtmlTextCdataClose);
    }
    Consume(t);
    return StateNext(StateName::HtmlTextCdata);
}

State HtmlTextCdataClose(Tokenizer* t) {
    if (t->current == ']') {
        Consume(t);
        return StateNext(StateName::HtmlTextCdataEnd);
    }
    return StateRetry(StateName::HtmlTextCdata);
}

State HtmlTextCdataEnd(Tokenizer* t) {
    if (t->current == '>') {
        return StateRetry(StateName::HtmlTextEnd);
    }
    if (t->current == ']') {
        return StateRetry(StateName::HtmlTextCdataClose);
    }
    return StateRetry(StateName::HtmlTextCdata);
}

State HtmlTextDeclaration(Tokenizer* t) {
    if (t->current < 0 || t->current == '>') {
        return StateRetry(StateName::HtmlTextEnd);
    }
    if (t->current == '\n') {
        TokenizerAttempt(t, StateNext(StateName::HtmlTextDeclaration),
                         StateNok());
        return StateRetry(StateName::HtmlTextLineEndingBefore);
    }
    Consume(t);
    return StateNext(StateName::HtmlTextDeclaration);
}

State HtmlTextInstruction(Tokenizer* t) {
    if (t->current < 0) {
        return StateNok();
    }
    if (t->current == '\n') {
        TokenizerAttempt(t, StateNext(StateName::HtmlTextInstruction),
                         StateNok());
        return StateRetry(StateName::HtmlTextLineEndingBefore);
    }
    if (t->current == '?') {
        Consume(t);
        return StateNext(StateName::HtmlTextInstructionClose);
    }
    Consume(t);
    return StateNext(StateName::HtmlTextInstruction);
}

State HtmlTextInstructionClose(Tokenizer* t) {
    if (t->current == '>') {
        return StateRetry(StateName::HtmlTextEnd);
    }
    return StateRetry(StateName::HtmlTextInstruction);
}

State HtmlTextTagCloseStart(Tokenizer* t) {
    if (t->current >= 0 && IsAsciiAlpha((uint8_t)t->current)) {
        Consume(t);
        return StateNext(StateName::HtmlTextTagClose);
    }
    return StateNok();
}

State HtmlTextTagClose(Tokenizer* t) {
    if (t->current == '-' ||
        (t->current >= 0 && IsAsciiAlphanumeric((uint8_t)t->current))) {
        Consume(t);
        return StateNext(StateName::HtmlTextTagClose);
    }
    return StateRetry(StateName::HtmlTextTagCloseBetween);
}

State HtmlTextTagCloseBetween(Tokenizer* t) {
    if (t->current == '\n') {
        TokenizerAttempt(t, StateNext(StateName::HtmlTextTagCloseBetween),
                         StateNok());
        return StateRetry(StateName::HtmlTextLineEndingBefore);
    }
    if (t->current == '\t' || t->current == ' ') {
        Consume(t);
        return StateNext(StateName::HtmlTextTagCloseBetween);
    }
    return StateRetry(StateName::HtmlTextEnd);
}

State HtmlTextTagOpen(Tokenizer* t) {
    if (t->current == '-' ||
        (t->current >= 0 && IsAsciiAlphanumeric((uint8_t)t->current))) {
        Consume(t);
        return StateNext(StateName::HtmlTextTagOpen);
    }
    if (t->current == '\t' || t->current == '\n' || t->current == ' ' ||
        t->current == '/' || t->current == '>') {
        return StateRetry(StateName::HtmlTextTagOpenBetween);
    }
    return StateNok();
}

State HtmlTextTagOpenBetween(Tokenizer* t) {
    if (t->current == '\n') {
        TokenizerAttempt(t, StateNext(StateName::HtmlTextTagOpenBetween),
                         StateNok());
        return StateRetry(StateName::HtmlTextLineEndingBefore);
    }
    if (t->current == '\t' || t->current == ' ') {
        Consume(t);
        return StateNext(StateName::HtmlTextTagOpenBetween);
    }
    if (t->current == '/') {
        Consume(t);
        return StateNext(StateName::HtmlTextEnd);
    }
    if (t->current == ':' || t->current == '_' ||
        (t->current >= 0 && IsAsciiAlpha((uint8_t)t->current))) {
        Consume(t);
        return StateNext(StateName::HtmlTextTagOpenAttributeName);
    }
    return StateRetry(StateName::HtmlTextEnd);
}

State HtmlTextTagOpenAttributeName(Tokenizer* t) {
    if (t->current == '-' || t->current == '.' || t->current == ':' ||
        t->current == '_' ||
        (t->current >= 0 && IsAsciiAlphanumeric((uint8_t)t->current))) {
        Consume(t);
        return StateNext(StateName::HtmlTextTagOpenAttributeName);
    }
    return StateRetry(StateName::HtmlTextTagOpenAttributeNameAfter);
}

State HtmlTextTagOpenAttributeNameAfter(Tokenizer* t) {
    if (t->current == '\n') {
        TokenizerAttempt(
            t, StateNext(StateName::HtmlTextTagOpenAttributeNameAfter),
            StateNok());
        return StateRetry(StateName::HtmlTextLineEndingBefore);
    }
    if (t->current == '\t' || t->current == ' ') {
        Consume(t);
        return StateNext(StateName::HtmlTextTagOpenAttributeNameAfter);
    }
    if (t->current == '=') {
        Consume(t);
        return StateNext(StateName::HtmlTextTagOpenAttributeValueBefore);
    }
    return StateRetry(StateName::HtmlTextTagOpenBetween);
}

State HtmlTextTagOpenAttributeValueBefore(Tokenizer* t) {
    if (t->current < 0 || t->current == '<' || t->current == '=' ||
        t->current == '>' || t->current == '`') {
        return StateNok();
    }
    if (t->current == '\n') {
        TokenizerAttempt(
            t, StateNext(StateName::HtmlTextTagOpenAttributeValueBefore),
            StateNok());
        return StateRetry(StateName::HtmlTextLineEndingBefore);
    }
    if (t->current == '\t' || t->current == ' ') {
        Consume(t);
        return StateNext(StateName::HtmlTextTagOpenAttributeValueBefore);
    }
    if (t->current == '"' || t->current == '\'') {
        t->tokenizeState.marker = (uint8_t)t->current;
        Consume(t);
        return StateNext(StateName::HtmlTextTagOpenAttributeValueQuoted);
    }
    Consume(t);
    return StateNext(StateName::HtmlTextTagOpenAttributeValueUnquoted);
}

State HtmlTextTagOpenAttributeValueQuoted(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        t->tokenizeState.marker = 0;
        Consume(t);
        return StateNext(StateName::HtmlTextTagOpenAttributeValueQuotedAfter);
    }
    if (t->current < 0) {
        t->tokenizeState.marker = 0;
        return StateNok();
    }
    if (t->current == '\n') {
        TokenizerAttempt(
            t, StateNext(StateName::HtmlTextTagOpenAttributeValueQuoted),
            StateNok());
        return StateRetry(StateName::HtmlTextLineEndingBefore);
    }
    Consume(t);
    return StateNext(StateName::HtmlTextTagOpenAttributeValueQuoted);
}

State HtmlTextTagOpenAttributeValueUnquoted(Tokenizer* t) {
    if (t->current < 0 || t->current == '"' || t->current == '\'' ||
        t->current == '<' || t->current == '=' || t->current == '`') {
        return StateNok();
    }
    if (t->current == '\t' || t->current == '\n' || t->current == ' ' ||
        t->current == '/' || t->current == '>') {
        return StateRetry(StateName::HtmlTextTagOpenBetween);
    }
    Consume(t);
    return StateNext(StateName::HtmlTextTagOpenAttributeValueUnquoted);
}

State HtmlTextTagOpenAttributeValueQuotedAfter(Tokenizer* t) {
    if (t->current == '\t' || t->current == '\n' || t->current == ' ' ||
        t->current == '/' || t->current == '>') {
        return StateRetry(StateName::HtmlTextTagOpenBetween);
    }
    return StateNok();
}

State HtmlTextEnd(Tokenizer* t) {
    if (t->current == '>') {
        Consume(t);
        Exit(t, Name::HtmlTextData);
        Exit(t, Name::HtmlText);
        return StateOk();
    }
    return StateNok();
}

State HtmlTextLineEndingBefore(Tokenizer* t) {
    Exit(t, Name::HtmlTextData);
    Enter(t, Name::LineEnding);
    Consume(t);
    Exit(t, Name::LineEnding);
    return StateNext(StateName::HtmlTextLineEndingAfter);
}

State HtmlTextLineEndingAfter(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t,
                         StateNext(StateName::HtmlTextLineEndingAfterPrefix),
                         StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    return StateRetry(StateName::HtmlTextLineEndingAfterPrefix);
}

State HtmlTextLineEndingAfterPrefix(Tokenizer* t) {
    Enter(t, Name::HtmlTextData);
    return StateOk();
}

}

#line 1 "src/markdown/construct_label.cpp"

namespace markdown {

static bool DefinitionsContain(const Vec<Str>& definitions, Str id) {
    for (int32_t i = 0; i < definitions.len; i++) {
        if (base::StrEq(definitions[i], id)) {
            return true;
        }
    }
    return false;
}

State LabelEndStart(Tokenizer* t) {
    if (t->current == ']' && t->parseState->options->constructs.labelEnd &&
        t->tokenizeState.labelStarts.len > 0) {
        const LabelStartMark& labelStart =
            t->tokenizeState.labelStarts[t->tokenizeState.labelStarts.len - 1];
        t->tokenizeState.end = t->events.len;

        if (labelStart.inactive) {
            return StateRetry(StateName::LabelEndNok);
        }
        Enter(t, Name::LabelEnd);
        Enter(t, Name::LabelMarker);
        Consume(t);
        Exit(t, Name::LabelMarker);
        Exit(t, Name::LabelEnd);
        return StateNext(StateName::LabelEndAfter);
    }
    return StateNok();
}

State LabelEndAfter(Tokenizer* t) {
    int32_t startIndex = t->tokenizeState.labelStarts.len - 1;
    const LabelStartMark& start = t->tokenizeState.labelStarts[startIndex];
    int32_t from = t->events[start.startB].point.index;
    int32_t to = t->events[t->tokenizeState.end].point.index;
    Arena* a = t->parseState->scratch;
    Slice slice = SliceFromIndices(t->parseState->bytes, from, to);
    Str id = NormalizeIdentifier(a, slice.bytes);

    if (start.kind == LabelKind::GfmFootnote) {
        if (DefinitionsContain(t->parseState->gfmFootnoteDefinitions, id)) {
            return StateRetry(StateName::LabelEndOk);
        }

        t->tokenizeState.labelStarts[startIndex].kind =
            LabelKind::GfmUndefinedFootnote;
        char* caret = (char*)base::Alloc(a, id.len + 2);
        caret[0] = '^';
        if (id.len > 0) {
            memcpy(caret + 1, id.s, (size_t)id.len);
        }
        caret[id.len + 1] = 0;
        id = Str(caret, id.len + 1);
    }

    bool defined = DefinitionsContain(t->parseState->definitions, id);

    if (t->current == '(') {
        TokenizerAttempt(t, StateNext(StateName::LabelEndOk),
                         StateNext(defined ? StateName::LabelEndOk
                                           : StateName::LabelEndNok));
        return StateRetry(StateName::LabelEndResourceStart);
    }
    if (t->current == '[') {
        TokenizerAttempt(
            t, StateNext(StateName::LabelEndOk),
            StateNext(defined ? StateName::LabelEndReferenceNotFull
                              : StateName::LabelEndNok));
        return StateRetry(StateName::LabelEndReferenceFull);
    }
    return StateRetry(defined ? StateName::LabelEndOk : StateName::LabelEndNok);
}

State LabelEndReferenceNotFull(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::LabelEndOk),
                     StateNext(StateName::LabelEndNok));
    return StateRetry(StateName::LabelEndReferenceCollapsed);
}

State LabelEndOk(Tokenizer* t) {

    LabelStartMark labelStart =
        t->tokenizeState.labelStarts[--t->tokenizeState.labelStarts.len];

    if (labelStart.kind != LabelKind::Image) {
        for (int32_t index = 0; index < t->tokenizeState.labelStarts.len;
             index++) {
            if (t->tokenizeState.labelStarts[index].kind != LabelKind::Image) {
                t->tokenizeState.labelStarts[index].inactive = true;
            }
        }
    }

    Label label;
    label.kind = labelStart.kind;
    label.startA = labelStart.startA;
    label.startB = labelStart.startB;
    label.endA = t->tokenizeState.end;
    label.endB = t->events.len - 1;
    VecAppend(t->tokenizeState.labels, label);
    t->tokenizeState.end = 0;
    RegisterResolverBefore(t, ResolveName::Label);
    return StateOk();
}

State LabelEndNok(Tokenizer* t) {
    LabelStartMark start =
        t->tokenizeState.labelStarts[--t->tokenizeState.labelStarts.len];
    VecAppend(t->tokenizeState.labelStartsLoose, start);
    t->tokenizeState.end = 0;
    return StateNok();
}

State LabelEndResourceStart(Tokenizer* t) {
    Enter(t, Name::Resource);
    Enter(t, Name::ResourceMarker);
    Consume(t);
    Exit(t, Name::ResourceMarker);
    return StateNext(StateName::LabelEndResourceBefore);
}

State LabelEndResourceBefore(Tokenizer* t) {
    if (t->current == '\t' || t->current == '\n' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::LabelEndResourceOpen),
                         StateNext(StateName::LabelEndResourceOpen));
        return StateRetry(SpaceOrTabEol(t));
    }
    return StateRetry(StateName::LabelEndResourceOpen);
}

State LabelEndResourceOpen(Tokenizer* t) {
    if (t->current == ')') {
        return StateRetry(StateName::LabelEndResourceEnd);
    }
    t->tokenizeState.token1 = Name::ResourceDestination;
    t->tokenizeState.token2 = Name::ResourceDestinationLiteral;
    t->tokenizeState.token3 = Name::ResourceDestinationLiteralMarker;
    t->tokenizeState.token4 = Name::ResourceDestinationRaw;
    t->tokenizeState.token5 = Name::ResourceDestinationString;
    t->tokenizeState.sizeB = kResourceDestinationBalanceMax;
    TokenizerAttempt(
        t, StateNext(StateName::LabelEndResourceDestinationAfter),
        StateNext(StateName::LabelEndResourceDestinationMissing));
    return StateRetry(StateName::DestinationStart);
}

State LabelEndResourceDestinationAfter(Tokenizer* t) {
    t->tokenizeState.token1 = Name::Data;
    t->tokenizeState.token2 = Name::Data;
    t->tokenizeState.token3 = Name::Data;
    t->tokenizeState.token4 = Name::Data;
    t->tokenizeState.token5 = Name::Data;
    t->tokenizeState.sizeB = 0;
    if (t->current == '\t' || t->current == '\n' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::LabelEndResourceBetween),
                         StateNext(StateName::LabelEndResourceEnd));
        return StateRetry(SpaceOrTabEol(t));
    }
    return StateRetry(StateName::LabelEndResourceEnd);
}

State LabelEndResourceDestinationMissing(Tokenizer* t) {
    t->tokenizeState.token1 = Name::Data;
    t->tokenizeState.token2 = Name::Data;
    t->tokenizeState.token3 = Name::Data;
    t->tokenizeState.token4 = Name::Data;
    t->tokenizeState.token5 = Name::Data;
    t->tokenizeState.sizeB = 0;
    return StateNok();
}

State LabelEndResourceBetween(Tokenizer* t) {
    if (t->current == '"' || t->current == '\'' || t->current == '(') {
        t->tokenizeState.token1 = Name::ResourceTitle;
        t->tokenizeState.token2 = Name::ResourceTitleMarker;
        t->tokenizeState.token3 = Name::ResourceTitleString;
        TokenizerAttempt(t, StateNext(StateName::LabelEndResourceTitleAfter),
                         StateNok());
        return StateRetry(StateName::TitleStart);
    }
    return StateRetry(StateName::LabelEndResourceEnd);
}

State LabelEndResourceTitleAfter(Tokenizer* t) {
    t->tokenizeState.token1 = Name::Data;
    t->tokenizeState.token2 = Name::Data;
    t->tokenizeState.token3 = Name::Data;
    if (t->current == '\t' || t->current == '\n' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::LabelEndResourceEnd),
                         StateNext(StateName::LabelEndResourceEnd));
        return StateRetry(SpaceOrTabEol(t));
    }
    return StateRetry(StateName::LabelEndResourceEnd);
}

State LabelEndResourceEnd(Tokenizer* t) {
    if (t->current == ')') {
        Enter(t, Name::ResourceMarker);
        Consume(t);
        Exit(t, Name::ResourceMarker);
        Exit(t, Name::Resource);
        return StateOk();
    }
    return StateNok();
}

State LabelEndReferenceFull(Tokenizer* t) {
    t->tokenizeState.token1 = Name::Reference;
    t->tokenizeState.token2 = Name::ReferenceMarker;
    t->tokenizeState.token3 = Name::ReferenceString;
    TokenizerAttempt(t, StateNext(StateName::LabelEndReferenceFullAfter),
                     StateNext(StateName::LabelEndReferenceFullMissing));
    return StateRetry(StateName::LabelStart);
}

State LabelEndReferenceFullAfter(Tokenizer* t) {
    t->tokenizeState.token1 = Name::Data;
    t->tokenizeState.token2 = Name::Data;
    t->tokenizeState.token3 = Name::Data;
    Name referenceString = Name::ReferenceString;
    int32_t at = SkipToBack(t->events, t->events.len - 1, &referenceString, 1);
    Position position = PositionFromExitEvent(t->events, at);
    Slice slice = SliceFromPosition(t->parseState->bytes, position);
    Str id = NormalizeIdentifier(t->parseState->scratch, slice.bytes);
    if (DefinitionsContain(t->parseState->definitions, id)) {
        return StateOk();
    }
    return StateNok();
}

State LabelEndReferenceFullMissing(Tokenizer* t) {
    t->tokenizeState.token1 = Name::Data;
    t->tokenizeState.token2 = Name::Data;
    t->tokenizeState.token3 = Name::Data;
    return StateNok();
}

State LabelEndReferenceCollapsed(Tokenizer* t) {
    Enter(t, Name::Reference);
    Enter(t, Name::ReferenceMarker);
    Consume(t);
    Exit(t, Name::ReferenceMarker);
    return StateNext(StateName::LabelEndReferenceCollapsedOpen);
}

State LabelEndReferenceCollapsedOpen(Tokenizer* t) {
    if (t->current == ']') {
        Enter(t, Name::ReferenceMarker);
        Consume(t);
        Exit(t, Name::ReferenceMarker);
        Exit(t, Name::Reference);
        return StateOk();
    }
    return StateNok();
}

static void InjectLabels(Tokenizer* t, const Vec<Label>& labels) {
    for (int32_t index = 0; index < labels.len; index++) {
        const Label& label = labels[index];
        Name groupName = Name::Link;
        if (label.kind == LabelKind::GfmFootnote) {
            groupName = Name::GfmFootnoteCall;
        } else if (label.kind == LabelKind::Image) {
            groupName = Name::Image;
        }

        Event caret[2];
        int32_t caretLen = 0;
        if (label.kind == LabelKind::GfmUndefinedFootnote) {
            caret[0].kind = Kind::Enter;
            caret[0].name = Name::Data;
            caret[0].point = t->events[label.startB - 2].point;
            caret[1].kind = Kind::Exit;
            caret[1].name = Name::Data;
            caret[1].point = t->events[label.startB - 1].point;
            caretLen = 2;
            t->events[label.startA].name = Name::LabelLink;
            t->events[label.startB].name = Name::LabelLink;
            t->events[label.startB].point = caret[0].point;
            EditMapAdd(t->map, label.startB - 2, 2, nullptr, 0);
        }

        Event open[2];
        open[0].kind = Kind::Enter;
        open[0].name = groupName;
        open[0].point = t->events[label.startA].point;
        open[1].kind = Kind::Enter;
        open[1].name = Name::Label;
        open[1].point = t->events[label.startA].point;
        EditMapAdd(t->map, label.startA, 0, open, 2);

        if (label.startB != label.endA || caretLen > 0) {
            Event textEnter;
            textEnter.kind = Kind::Enter;
            textEnter.name = Name::LabelText;
            textEnter.point = t->events[label.startB].point;
            EditMapAddBefore(t->map, label.startB + 1, 0, &textEnter, 1);
            Event textExit;
            textExit.kind = Kind::Exit;
            textExit.name = Name::LabelText;
            textExit.point = t->events[label.endA].point;
            EditMapAdd(t->map, label.endA, 0, &textExit, 1);
        }

        if (caretLen > 0) {
            EditMapAdd(t->map, label.startB + 1, 0, caret, caretLen);
        }

        Event labelExit;
        labelExit.kind = Kind::Exit;
        labelExit.name = Name::Label;
        labelExit.point = t->events[label.endA + 3].point;
        EditMapAdd(t->map, label.endA + 4, 0, &labelExit, 1);

        Event groupExit;
        groupExit.kind = Kind::Exit;
        groupExit.name = groupName;
        groupExit.point = t->events[label.endB].point;
        EditMapAdd(t->map, label.endB + 1, 0, &groupExit, 1);
    }
}

static void MarkAsData(Tokenizer* t, const Vec<LabelStartMark>& events) {
    for (int32_t index = 0; index < events.len; index++) {
        int32_t dataEnterIndex = events[index].startA;
        int32_t dataExitIndex = events[index].startB;
        Event add[2];
        add[0].kind = Kind::Enter;
        add[0].name = Name::Data;
        add[0].point = t->events[dataEnterIndex].point;
        add[1].kind = Kind::Exit;
        add[1].name = Name::Data;
        add[1].point = t->events[dataExitIndex].point;
        EditMapAdd(t->map, dataEnterIndex,
                   dataExitIndex - dataEnterIndex + 1, add, 2);
    }
}

bool LabelEndResolve(Tokenizer* t, Subresult*) {

    Vec<Label> labels;
    for (int32_t i = 0; i < t->tokenizeState.labels.len; i++) {
        VecAppend(labels, t->tokenizeState.labels[i]);
    }
    t->tokenizeState.labels.len = 0;
    InjectLabels(t, labels);

    Vec<LabelStartMark> starts;
    for (int32_t i = 0; i < t->tokenizeState.labelStarts.len; i++) {
        VecAppend(starts, t->tokenizeState.labelStarts[i]);
    }
    t->tokenizeState.labelStarts.len = 0;
    MarkAsData(t, starts);

    starts.len = 0;
    for (int32_t i = 0; i < t->tokenizeState.labelStartsLoose.len; i++) {
        VecAppend(starts, t->tokenizeState.labelStartsLoose[i]);
    }
    t->tokenizeState.labelStartsLoose.len = 0;
    MarkAsData(t, starts);

    EditMapConsume(t->map, t->events);
    return false;
}

}

#line 1 "src/markdown/construct_partial.cpp"

namespace markdown {

StateName SpaceOrTabWithOptions(Tokenizer* t,
                                const SpaceOrTabOptions& options) {
    t->tokenizeState.spaceOrTabConnect = options.connect;
    t->tokenizeState.spaceOrTabContent = options.content;
    t->tokenizeState.spaceOrTabContentSome = options.contentSome;
    t->tokenizeState.spaceOrTabMin = options.min;
    t->tokenizeState.spaceOrTabMax = options.max;
    t->tokenizeState.spaceOrTabToken = options.kind;
    return StateName::SpaceOrTabStart;
}

StateName SpaceOrTabMinMax(Tokenizer* t, int32_t min, int32_t max) {
    SpaceOrTabOptions options;
    options.kind = Name::SpaceOrTab;
    options.min = min;
    options.max = max;
    return SpaceOrTabWithOptions(t, options);
}

StateName SpaceOrTab(Tokenizer* t) {
    return SpaceOrTabMinMax(t, 1, kSizeMax);
}

State SpaceOrTabStart(Tokenizer* t) {
    if (t->tokenizeState.spaceOrTabMax > 0 &&
        (t->current == '\t' || t->current == ' ')) {
        if (t->tokenizeState.spaceOrTabContentSome) {
            Link link;
            link.content = t->tokenizeState.spaceOrTabContent;
            EnterLink(t, t->tokenizeState.spaceOrTabToken, link);
        } else {
            Enter(t, t->tokenizeState.spaceOrTabToken);
        }
        if (t->tokenizeState.spaceOrTabConnect) {
            SubtokenizeLink(t->events, t->events.len - 1);
        } else {
            t->tokenizeState.spaceOrTabConnect = true;
        }
        return StateRetry(StateName::SpaceOrTabInside);
    }
    return StateRetry(StateName::SpaceOrTabAfter);
}

State SpaceOrTabInside(Tokenizer* t) {
    if ((t->current == '\t' || t->current == ' ') &&
        t->tokenizeState.spaceOrTabSize < t->tokenizeState.spaceOrTabMax) {
        Consume(t);
        t->tokenizeState.spaceOrTabSize += 1;
        return StateNext(StateName::SpaceOrTabInside);
    }
    Exit(t, t->tokenizeState.spaceOrTabToken);
    return StateRetry(StateName::SpaceOrTabAfter);
}

State SpaceOrTabAfter(Tokenizer* t) {
    State state = t->tokenizeState.spaceOrTabSize >= t->tokenizeState.spaceOrTabMin
                      ? StateOk()
                      : StateNok();
    t->tokenizeState.spaceOrTabConnect = false;
    t->tokenizeState.spaceOrTabContentSome = false;
    t->tokenizeState.spaceOrTabSize = 0;
    t->tokenizeState.spaceOrTabMax = 0;
    t->tokenizeState.spaceOrTabMin = 0;
    t->tokenizeState.spaceOrTabToken = Name::SpaceOrTab;
    return state;
}

StateName SpaceOrTabEolWithOptions(Tokenizer* t,
                                   const SpaceOrTabEolOptions& options) {
    t->tokenizeState.spaceOrTabEolContent = options.content;
    t->tokenizeState.spaceOrTabEolContentSome = options.contentSome;
    t->tokenizeState.spaceOrTabEolConnect = options.connect;
    return StateName::SpaceOrTabEolStart;
}

StateName SpaceOrTabEol(Tokenizer* t) {
    SpaceOrTabEolOptions options;
    return SpaceOrTabEolWithOptions(t, options);
}

State SpaceOrTabEolStart(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::SpaceOrTabEolAfterFirst),
                 StateNext(StateName::SpaceOrTabEolAtEol));
        SpaceOrTabOptions options;
        options.kind = Name::SpaceOrTab;
        options.min = 1;
        options.max = kSizeMax;
        options.content = t->tokenizeState.spaceOrTabEolContent;
        options.contentSome = t->tokenizeState.spaceOrTabEolContentSome;
        options.connect = t->tokenizeState.spaceOrTabEolConnect;
        return StateRetry(SpaceOrTabWithOptions(t, options));
    }
    return StateRetry(StateName::SpaceOrTabEolAtEol);
}

State SpaceOrTabEolAfterFirst(Tokenizer* t) {
    t->tokenizeState.spaceOrTabEolOk = true;
    return StateRetry(StateName::SpaceOrTabEolAtEol);
}

State SpaceOrTabEolAtEol(Tokenizer* t) {
    if (t->current == '\n') {
        if (t->tokenizeState.spaceOrTabEolContentSome) {
            Link link;
            link.content = t->tokenizeState.spaceOrTabEolContent;
            EnterLink(t, Name::LineEnding, link);
        } else {
            Enter(t, Name::LineEnding);
        }
        if (t->tokenizeState.spaceOrTabEolConnect) {
            SubtokenizeLink(t->events, t->events.len - 1);
        } else if (t->tokenizeState.spaceOrTabEolContentSome) {
            t->tokenizeState.spaceOrTabEolConnect = true;
        }
        Consume(t);
        Exit(t, Name::LineEnding);
        return StateNext(StateName::SpaceOrTabEolAfterEol);
    }
    bool ok = t->tokenizeState.spaceOrTabEolOk;
    t->tokenizeState.spaceOrTabEolContentSome = false;
    t->tokenizeState.spaceOrTabEolConnect = false;
    t->tokenizeState.spaceOrTabEolOk = false;
    return ok ? StateOk() : StateNok();
}

State SpaceOrTabEolAfterEol(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::SpaceOrTabEolAfterMore), StateNok());
        SpaceOrTabOptions options;
        options.kind = Name::SpaceOrTab;
        options.min = 1;
        options.max = kSizeMax;
        options.content = t->tokenizeState.spaceOrTabEolContent;
        options.contentSome = t->tokenizeState.spaceOrTabEolContentSome;
        options.connect = t->tokenizeState.spaceOrTabEolConnect;
        return StateRetry(SpaceOrTabWithOptions(t, options));
    }
    return StateRetry(StateName::SpaceOrTabEolAfterMore);
}

State SpaceOrTabEolAfterMore(Tokenizer* t) {
    t->tokenizeState.spaceOrTabEolContentSome = false;
    t->tokenizeState.spaceOrTabEolConnect = false;
    t->tokenizeState.spaceOrTabEolOk = false;
    return StateOk();
}

static bool MarkersContain(Tokenizer* t, int32_t byte) {
    if (byte < 0) {
        return false;
    }
    for (int32_t i = 0; i < t->tokenizeState.markersLen; i++) {
        if (t->tokenizeState.markers[i] == (uint8_t)byte) {
            return true;
        }
    }
    return false;
}

State DataStart(Tokenizer* t) {
    if (t->current >= 0 && MarkersContain(t, t->current)) {
        Enter(t, Name::Data);
        Consume(t);
        return StateNext(StateName::DataInside);
    }
    return StateRetry(StateName::DataAtBreak);
}

State DataAtBreak(Tokenizer* t) {
    if (t->current >= 0 && !MarkersContain(t, t->current)) {
        if (t->current == '\n') {
            Enter(t, Name::LineEnding);
            Consume(t);
            Exit(t, Name::LineEnding);
            return StateNext(StateName::DataAtBreak);
        }
        Enter(t, Name::Data);
        return StateRetry(StateName::DataInside);
    }
    return StateOk();
}

State DataInside(Tokenizer* t) {
    if (t->current >= 0 && t->current != '\n' && !MarkersContain(t, t->current)) {
        Consume(t);
        return StateNext(StateName::DataInside);
    }
    Exit(t, Name::Data);
    return StateRetry(StateName::DataAtBreak);
}

bool DataResolve(Tokenizer* t, Subresult*) {
    int32_t index = 0;
    while (index < t->events.len) {
        const Event& event = t->events[index];
        if (event.kind == Kind::Enter && event.name == Name::Data) {
            index += 1;
            int32_t exitIndex = index;
            while (exitIndex + 1 < t->events.len &&
                   t->events[exitIndex + 1].name == Name::Data) {
                exitIndex += 2;
            }
            if (exitIndex > index) {
                EditMapAdd(t->map, index, exitIndex - index, nullptr, 0);
                t->events[index].point = t->events[exitIndex].point;
                index = exitIndex;
            }
        }
        index += 1;
    }
    EditMapConsume(t->map, t->events);
    return false;
}

State DestinationStart(Tokenizer* t) {
    if (t->current == '<') {
        Enter(t, t->tokenizeState.token1);
        Enter(t, t->tokenizeState.token2);
        Enter(t, t->tokenizeState.token3);
        Consume(t);
        Exit(t, t->tokenizeState.token3);
        return StateNext(StateName::DestinationEnclosedBefore);
    }
    if (t->current < 0 || (t->current >= 0x01 && t->current <= 0x1f) ||
        t->current == ' ' || t->current == ')' || t->current == 0x7f) {
        return StateNok();
    }
    Enter(t, t->tokenizeState.token1);
    Enter(t, t->tokenizeState.token4);
    Enter(t, t->tokenizeState.token5);
    Link link;
    link.content = ContentKind::String;
    EnterLink(t, Name::Data, link);
    return StateRetry(StateName::DestinationRaw);
}

State DestinationEnclosedBefore(Tokenizer* t) {
    if (t->current == '>') {
        Enter(t, t->tokenizeState.token3);
        Consume(t);
        Exit(t, t->tokenizeState.token3);
        Exit(t, t->tokenizeState.token2);
        Exit(t, t->tokenizeState.token1);
        return StateOk();
    }
    Enter(t, t->tokenizeState.token5);
    Link link;
    link.content = ContentKind::String;
    EnterLink(t, Name::Data, link);
    return StateRetry(StateName::DestinationEnclosed);
}

State DestinationEnclosed(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n' || t->current == '<') {
        return StateNok();
    }
    if (t->current == '>') {
        Exit(t, Name::Data);
        Exit(t, t->tokenizeState.token5);
        return StateRetry(StateName::DestinationEnclosedBefore);
    }
    if (t->current == '\\') {
        Consume(t);
        return StateNext(StateName::DestinationEnclosedEscape);
    }
    Consume(t);
    return StateNext(StateName::DestinationEnclosed);
}

State DestinationEnclosedEscape(Tokenizer* t) {
    if (t->current == '<' || t->current == '>' || t->current == '\\') {
        Consume(t);
        return StateNext(StateName::DestinationEnclosed);
    }
    return StateRetry(StateName::DestinationEnclosed);
}

State DestinationRaw(Tokenizer* t) {
    if (t->tokenizeState.size == 0 &&
        (t->current < 0 || t->current == '\t' || t->current == '\n' ||
         t->current == ' ' || t->current == ')')) {
        Exit(t, Name::Data);
        Exit(t, t->tokenizeState.token5);
        Exit(t, t->tokenizeState.token4);
        Exit(t, t->tokenizeState.token1);
        t->tokenizeState.size = 0;
        return StateOk();
    }
    if (t->tokenizeState.size < t->tokenizeState.sizeB && t->current == '(') {
        Consume(t);
        t->tokenizeState.size += 1;
        return StateNext(StateName::DestinationRaw);
    }
    if (t->current == ')') {
        Consume(t);
        t->tokenizeState.size -= 1;
        return StateNext(StateName::DestinationRaw);
    }

    if (t->current < 0 || (t->current >= 0x01 && t->current <= 0x1f) ||
        t->current == ' ' || t->current == '(' || t->current == 0x7f) {
        t->tokenizeState.size = 0;
        return StateNok();
    }
    if (t->current == '\\') {
        Consume(t);
        return StateNext(StateName::DestinationRawEscape);
    }
    Consume(t);
    return StateNext(StateName::DestinationRaw);
}

State DestinationRawEscape(Tokenizer* t) {
    if (t->current == '(' || t->current == ')' || t->current == '\\') {
        Consume(t);
        return StateNext(StateName::DestinationRaw);
    }
    return StateRetry(StateName::DestinationRaw);
}

State LabelStart(Tokenizer* t) {
    Enter(t, t->tokenizeState.token1);
    Enter(t, t->tokenizeState.token2);
    Consume(t);
    Exit(t, t->tokenizeState.token2);
    Enter(t, t->tokenizeState.token3);
    return StateNext(StateName::LabelAtBreak);
}

State LabelAtBreak(Tokenizer* t) {
    if (t->tokenizeState.size > kLinkReferenceSizeMax || t->current < 0 ||
        t->current == '[' || (t->current == ']' && !t->tokenizeState.seen)) {
        return StateRetry(StateName::LabelNok);
    }
    if (t->current == '\n') {
        TokenizerAttempt(t, StateNext(StateName::LabelEolAfter),
                 StateNext(StateName::LabelNok));
        SpaceOrTabEolOptions options;
        options.content = ContentKind::String;
        options.contentSome = true;
        options.connect = t->tokenizeState.connect;
        return StateRetry(SpaceOrTabEolWithOptions(t, options));
    }
    if (t->current == ']') {
        Exit(t, t->tokenizeState.token3);
        Enter(t, t->tokenizeState.token2);
        Consume(t);
        Exit(t, t->tokenizeState.token2);
        Exit(t, t->tokenizeState.token1);
        t->tokenizeState.connect = false;
        t->tokenizeState.seen = false;
        t->tokenizeState.size = 0;
        return StateOk();
    }
    Link link;
    link.content = ContentKind::String;
    EnterLink(t, Name::Data, link);
    if (t->tokenizeState.connect) {
        SubtokenizeLink(t->events, t->events.len - 1);
    } else {
        t->tokenizeState.connect = true;
    }
    return StateRetry(StateName::LabelInside);
}

State LabelEolAfter(Tokenizer* t) {
    t->tokenizeState.connect = true;
    return StateRetry(StateName::LabelAtBreak);
}

State LabelNok(Tokenizer* t) {
    t->tokenizeState.connect = false;
    t->tokenizeState.seen = false;
    t->tokenizeState.size = 0;
    return StateNok();
}

State LabelInside(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n' || t->current == '[' ||
        t->current == ']') {
        Exit(t, Name::Data);
        return StateRetry(StateName::LabelAtBreak);
    }
    if (t->tokenizeState.size > kLinkReferenceSizeMax) {
        Exit(t, Name::Data);
        return StateRetry(StateName::LabelAtBreak);
    }
    int32_t byte = t->current;
    Consume(t);
    t->tokenizeState.size += 1;
    if (!t->tokenizeState.seen && byte != '\t' && byte != ' ') {
        t->tokenizeState.seen = true;
    }
    return StateNext(byte == '\\' ? StateName::LabelEscape
                                  : StateName::LabelInside);
}

State LabelEscape(Tokenizer* t) {
    if (t->current == '[' || t->current == '\\' || t->current == ']') {
        Consume(t);
        t->tokenizeState.size += 1;
        return StateNext(StateName::LabelInside);
    }
    return StateRetry(StateName::LabelInside);
}

State TitleStart(Tokenizer* t) {
    if (t->current == '"' || t->current == '\'' || t->current == '(') {
        uint8_t marker = (uint8_t)t->current;
        t->tokenizeState.marker = marker == '(' ? (uint8_t)')' : marker;
        Enter(t, t->tokenizeState.token1);
        Enter(t, t->tokenizeState.token2);
        Consume(t);
        Exit(t, t->tokenizeState.token2);
        return StateNext(StateName::TitleBegin);
    }
    return StateNok();
}

State TitleBegin(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        Enter(t, t->tokenizeState.token2);
        Consume(t);
        Exit(t, t->tokenizeState.token2);
        Exit(t, t->tokenizeState.token1);
        t->tokenizeState.marker = 0;
        t->tokenizeState.connect = false;
        return StateOk();
    }
    Enter(t, t->tokenizeState.token3);
    return StateRetry(StateName::TitleAtBreak);
}

State TitleAtBreak(Tokenizer* t) {
    if (t->current < 0) {
        return StateRetry(StateName::TitleNok);
    }
    if (t->current == (int32_t)t->tokenizeState.marker) {
        Exit(t, t->tokenizeState.token3);
        return StateRetry(StateName::TitleBegin);
    }
    if (t->current == '\n') {
        TokenizerAttempt(t, StateNext(StateName::TitleAfterEol),
                 StateNext(StateName::TitleNok));
        SpaceOrTabEolOptions options;
        options.content = ContentKind::String;
        options.contentSome = true;
        options.connect = t->tokenizeState.connect;
        return StateRetry(SpaceOrTabEolWithOptions(t, options));
    }
    Link link;
    link.content = ContentKind::String;
    EnterLink(t, Name::Data, link);
    if (t->tokenizeState.connect) {
        SubtokenizeLink(t->events, t->events.len - 1);
    } else {
        t->tokenizeState.connect = true;
    }
    return StateRetry(StateName::TitleInside);
}

State TitleAfterEol(Tokenizer* t) {
    t->tokenizeState.connect = true;
    return StateRetry(StateName::TitleAtBreak);
}

State TitleNok(Tokenizer* t) {
    t->tokenizeState.marker = 0;
    t->tokenizeState.connect = false;
    return StateNok();
}

State TitleInside(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker || t->current < 0 ||
        t->current == '\n') {
        Exit(t, Name::Data);
        return StateRetry(StateName::TitleAtBreak);
    }
    StateName name = t->current == '\\' ? StateName::TitleEscape
                                        : StateName::TitleInside;
    Consume(t);
    return StateNext(name);
}

State TitleEscape(Tokenizer* t) {
    if (t->current == '"' || t->current == '\'' || t->current == ')' ||
        t->current == '\\') {
        Consume(t);
        return StateNext(StateName::TitleInside);
    }
    return StateRetry(StateName::TitleInside);
}

static void TrimData(Tokenizer* t, int32_t exitIndex, bool trimStart,
                     bool trimEnd, bool hardBreak) {
    Position position = PositionFromExitEvent(t->events, exitIndex);
    Slice slice = SliceFromPosition(t->parseState->bytes, position);

    if (trimEnd) {
        int32_t index = slice.bytes.len;
        bool spacesOnly = slice.after == 0;
        while (index > 0) {
            char byte = slice.bytes.s[index - 1];
            if (byte == ' ') {

            } else if (byte == '\t') {
                spacesOnly = false;
            } else {
                break;
            }
            index -= 1;
        }
        int32_t diff = slice.bytes.len - index;
        Name name = (hardBreak && spacesOnly &&
                     diff >= kHardBreakPrefixSizeMin &&
                     exitIndex + 1 < t->events.len)
                        ? Name::HardBreakTrailing
                        : Name::SpaceOrTab;
        if (index == 0) {
            t->events[exitIndex - 1].name = name;
            t->events[exitIndex].name = name;
            return;
        }
        if (diff > 0 || slice.after > 0) {
            Point exitPoint = t->events[exitIndex].point;
            Point enterPoint = exitPoint;
            enterPoint.index -= diff;
            enterPoint.column -= diff;
            enterPoint.vs = 0;
            Event add[2];
            add[0].kind = Kind::Enter;
            add[0].name = name;
            add[0].point = enterPoint;
            add[1].kind = Kind::Exit;
            add[1].name = name;
            add[1].point = exitPoint;
            EditMapAdd(t->map, exitIndex + 1, 0, add, 2);
            t->events[exitIndex].point = enterPoint;
            slice.bytes.len = index;
        }
    }

    if (trimStart) {
        int32_t index = 0;
        while (index < slice.bytes.len) {
            char byte = slice.bytes.s[index];
            if (byte == ' ' || byte == '\t') {
                index += 1;
            } else {
                break;
            }
        }
        if (index == slice.bytes.len) {
            t->events[exitIndex - 1].name = Name::SpaceOrTab;
            t->events[exitIndex].name = Name::SpaceOrTab;
            return;
        }
        if (index > 0 || slice.before > 0) {
            Point enterPoint = t->events[exitIndex - 1].point;
            Point exitPoint = enterPoint;
            exitPoint.index += index;
            exitPoint.column += index;
            exitPoint.vs = 0;
            Event add[2];
            add[0].kind = Kind::Enter;
            add[0].name = Name::SpaceOrTab;
            add[0].point = enterPoint;
            add[1].kind = Kind::Exit;
            add[1].name = Name::SpaceOrTab;
            add[1].point = exitPoint;
            EditMapAdd(t->map, exitIndex - 1, 0, add, 2);
            t->events[exitIndex - 1].point = exitPoint;
        }
    }
}

void ResolveWhitespace(Tokenizer* t, bool hardBreak, bool trimWhole) {
    int32_t index = 0;
    while (index < t->events.len) {
        const Event& event = t->events[index];
        if (event.kind == Kind::Exit && event.name == Name::Data) {
            bool trimStart =
                (trimWhole && index == 1) ||
                (index > 1 && t->events[index - 2].name == Name::LineEnding);
            bool trimEnd = (trimWhole && index == t->events.len - 1) ||
                           (index + 1 < t->events.len &&
                            t->events[index + 1].name == Name::LineEnding);
            TrimData(t, index, trimStart, trimEnd, hardBreak);
        }
        index += 1;
    }
    EditMapConsume(t->map, t->events);
}

static const uint8_t kBom[3] = {0xef, 0xbb, 0xbf};

State BomStart(Tokenizer* t) {
    if (t->current == (int32_t)kBom[0]) {
        Enter(t, Name::ByteOrderMark);
        return StateRetry(StateName::BomInside);
    }
    return StateNok();
}

State BomInside(Tokenizer* t) {
    if (t->current == (int32_t)kBom[t->tokenizeState.size]) {
        t->tokenizeState.size += 1;
        Consume(t);
        if (t->tokenizeState.size == 3) {
            Exit(t, Name::ByteOrderMark);
            t->tokenizeState.size = 0;
            return StateOk();
        }
        return StateNext(StateName::BomInside);
    }
    t->tokenizeState.size = 0;
    return StateNok();
}

State NonLazyContinuationStart(Tokenizer* t) {
    if (t->current == '\n') {
        Enter(t, Name::LineEnding);
        Consume(t);
        Exit(t, Name::LineEnding);
        return StateNext(StateName::NonLazyContinuationAfter);
    }
    return StateNok();
}

State NonLazyContinuationAfter(Tokenizer* t) {
    return t->lazy ? StateNok() : StateOk();
}

State BlankLineStart(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::BlankLineAfter), StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    return StateRetry(StateName::BlankLineAfter);
}

State BlankLineAfter(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        return StateOk();
    }
    return StateNok();
}

}

#line 1 "src/markdown/construct_raw.cpp"

namespace markdown {

static void RawFlowClear(Tokenizer* t) {
    t->tokenizeState.marker = 0;
    t->tokenizeState.sizeC = 0;
    t->tokenizeState.size = 0;
    t->tokenizeState.token1 = Name::Data;
    t->tokenizeState.token2 = Name::Data;
    t->tokenizeState.token3 = Name::Data;
    t->tokenizeState.token4 = Name::Data;
    t->tokenizeState.token5 = Name::Data;
    t->tokenizeState.token6 = Name::Data;
}

State RawFlowStart(Tokenizer* t) {
    if (t->parseState->options->constructs.codeFenced ||
        t->parseState->options->constructs.mathFlow) {
        if (t->current == '\t' || t->current == ' ') {
            TokenizerAttempt(t, StateNext(StateName::RawFlowBeforeSequenceOpen),
                             StateNok());
            int32_t max = t->parseState->options->constructs.codeIndented
                              ? kTabSize - 1
                              : kSizeMax;
            return StateRetry(SpaceOrTabMinMax(t, 0, max));
        }
        if (t->current == '$' || t->current == '`' || t->current == '~') {
            return StateRetry(StateName::RawFlowBeforeSequenceOpen);
        }
    }
    return StateNok();
}

State RawFlowBeforeSequenceOpen(Tokenizer* t) {
    int32_t prefix = 0;
    if (t->events.len > 0 &&
        t->events[t->events.len - 1].name == Name::SpaceOrTab) {
        Position position = PositionFromExitEvent(t->events, t->events.len - 1);
        prefix = SliceFromPosition(t->parseState->bytes, position).Len();
    }

    bool codeFence = t->parseState->options->constructs.codeFenced &&
                     (t->current == '`' || t->current == '~');
    bool mathFence =
        t->parseState->options->constructs.mathFlow && t->current == '$';
    if (!codeFence && !mathFence) {
        return StateNok();
    }

    t->tokenizeState.marker = (uint8_t)t->current;
    t->tokenizeState.sizeC = prefix;
    if (t->tokenizeState.marker == '$') {
        t->tokenizeState.token1 = Name::MathFlow;
        t->tokenizeState.token2 = Name::MathFlowFence;
        t->tokenizeState.token3 = Name::MathFlowFenceSequence;
        t->tokenizeState.token5 = Name::MathFlowFenceMeta;
        t->tokenizeState.token6 = Name::MathFlowChunk;
    } else {
        t->tokenizeState.token1 = Name::CodeFenced;
        t->tokenizeState.token2 = Name::CodeFencedFence;
        t->tokenizeState.token3 = Name::CodeFencedFenceSequence;
        t->tokenizeState.token4 = Name::CodeFencedFenceInfo;
        t->tokenizeState.token5 = Name::CodeFencedFenceMeta;
        t->tokenizeState.token6 = Name::CodeFlowChunk;
    }
    Enter(t, t->tokenizeState.token1);
    Enter(t, t->tokenizeState.token2);
    Enter(t, t->tokenizeState.token3);
    return StateRetry(StateName::RawFlowSequenceOpen);
}

State RawFlowSequenceOpen(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        t->tokenizeState.size += 1;
        Consume(t);
        return StateNext(StateName::RawFlowSequenceOpen);
    }
    int32_t min = t->tokenizeState.marker == '$' ? kMathFlowSequenceSizeMin
                                                 : kCodeFencedSequenceSizeMin;
    if (t->tokenizeState.size < min) {
        RawFlowClear(t);
        return StateNok();
    }
    StateName next = t->tokenizeState.marker == '$'
                         ? StateName::RawFlowMetaBefore
                         : StateName::RawFlowInfoBefore;
    if (t->current == '\t' || t->current == ' ') {
        Exit(t, t->tokenizeState.token3);
        TokenizerAttempt(t, StateNext(next), StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    Exit(t, t->tokenizeState.token3);
    return StateRetry(next);
}

State RawFlowInfoBefore(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        Exit(t, t->tokenizeState.token2);

        t->concrete = true;
        TokenizerCheck(t, StateNext(StateName::RawFlowAtNonLazyBreak),
                       StateNext(StateName::RawFlowAfter));
        return StateRetry(StateName::NonLazyContinuationStart);
    }
    Enter(t, t->tokenizeState.token4);
    Link link;
    link.content = ContentKind::String;
    EnterLink(t, Name::Data, link);
    return StateRetry(StateName::RawFlowInfo);
}

State RawFlowInfo(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        Exit(t, Name::Data);
        Exit(t, t->tokenizeState.token4);
        return StateRetry(StateName::RawFlowInfoBefore);
    }
    if (t->current == '\t' || t->current == ' ') {
        Exit(t, Name::Data);
        Exit(t, t->tokenizeState.token4);
        TokenizerAttempt(t, StateNext(StateName::RawFlowMetaBefore), StateNok());
        return StateRetry(SpaceOrTab(t));
    }
    if (t->current == (int32_t)t->tokenizeState.marker &&
        (t->current == '$' || t->current == '`')) {
        t->concrete = false;
        RawFlowClear(t);
        return StateNok();
    }
    Consume(t);
    return StateNext(StateName::RawFlowInfo);
}

State RawFlowMetaBefore(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        return StateRetry(StateName::RawFlowInfoBefore);
    }
    Enter(t, t->tokenizeState.token5);
    Link link;
    link.content = ContentKind::String;
    EnterLink(t, Name::Data, link);
    return StateRetry(StateName::RawFlowMeta);
}

State RawFlowMeta(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        Exit(t, Name::Data);
        Exit(t, t->tokenizeState.token5);
        return StateRetry(StateName::RawFlowInfoBefore);
    }
    if (t->current == (int32_t)t->tokenizeState.marker &&
        (t->current == '$' || t->current == '`')) {
        t->concrete = false;
        RawFlowClear(t);
        return StateNok();
    }
    Consume(t);
    return StateNext(StateName::RawFlowMeta);
}

State RawFlowAtNonLazyBreak(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::RawFlowAfter),
                     StateNext(StateName::RawFlowContentBefore));
    Enter(t, Name::LineEnding);
    Consume(t);
    Exit(t, Name::LineEnding);
    return StateNext(StateName::RawFlowCloseStart);
}

State RawFlowCloseStart(Tokenizer* t) {
    Enter(t, t->tokenizeState.token2);
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::RawFlowBeforeSequenceClose),
                         StateNok());
        int32_t max = t->parseState->options->constructs.codeIndented
                          ? kTabSize - 1
                          : kSizeMax;
        return StateRetry(SpaceOrTabMinMax(t, 0, max));
    }
    return StateRetry(StateName::RawFlowBeforeSequenceClose);
}

State RawFlowBeforeSequenceClose(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        Enter(t, t->tokenizeState.token3);
        return StateRetry(StateName::RawFlowSequenceClose);
    }
    return StateNok();
}

State RawFlowSequenceClose(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        t->tokenizeState.sizeB += 1;
        Consume(t);
        return StateNext(StateName::RawFlowSequenceClose);
    }
    if (t->tokenizeState.sizeB >= t->tokenizeState.size) {
        t->tokenizeState.sizeB = 0;
        Exit(t, t->tokenizeState.token3);
        if (t->current == '\t' || t->current == ' ') {
            TokenizerAttempt(t,
                             StateNext(StateName::RawFlowAfterSequenceClose),
                             StateNok());
            return StateRetry(SpaceOrTab(t));
        }
        return StateRetry(StateName::RawFlowAfterSequenceClose);
    }
    t->tokenizeState.sizeB = 0;
    return StateNok();
}

State RawFlowAfterSequenceClose(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        Exit(t, t->tokenizeState.token2);
        return StateOk();
    }
    return StateNok();
}

State RawFlowContentBefore(Tokenizer* t) {
    Enter(t, Name::LineEnding);
    Consume(t);
    Exit(t, Name::LineEnding);
    return StateNext(StateName::RawFlowContentStart);
}

State RawFlowContentStart(Tokenizer* t) {
    if (t->current == '\t' || t->current == ' ') {
        TokenizerAttempt(t, StateNext(StateName::RawFlowBeforeContentChunk),
                         StateNok());
        return StateRetry(SpaceOrTabMinMax(t, 0, t->tokenizeState.sizeC));
    }
    return StateRetry(StateName::RawFlowBeforeContentChunk);
}

State RawFlowBeforeContentChunk(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        TokenizerCheck(t, StateNext(StateName::RawFlowAtNonLazyBreak),
                       StateNext(StateName::RawFlowAfter));
        return StateRetry(StateName::NonLazyContinuationStart);
    }
    Enter(t, t->tokenizeState.token6);
    return StateRetry(StateName::RawFlowContentChunk);
}

State RawFlowContentChunk(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n') {
        Exit(t, t->tokenizeState.token6);
        return StateRetry(StateName::RawFlowBeforeContentChunk);
    }
    Consume(t);
    return StateNext(StateName::RawFlowContentChunk);
}

State RawFlowAfter(Tokenizer* t) {
    Exit(t, t->tokenizeState.token1);
    RawFlowClear(t);
    t->interrupt = false;

    t->concrete = false;
    return StateOk();
}

static void RawTextClear(Tokenizer* t) {
    t->tokenizeState.marker = 0;
    t->tokenizeState.size = 0;
    t->tokenizeState.token1 = Name::Data;
    t->tokenizeState.token2 = Name::Data;
    t->tokenizeState.token3 = Name::Data;
}

State RawTextStart(Tokenizer* t) {
    bool code =
        t->parseState->options->constructs.codeText && t->current == '`';
    bool math =
        t->parseState->options->constructs.mathText && t->current == '$';
    bool afterEscape =
        t->events.len > 0 &&
        t->events[t->events.len - 1].name == Name::CharacterEscape;
    if ((code || math) && (t->previous != t->current || afterEscape)) {
        uint8_t marker = (uint8_t)t->current;
        if (marker == '`') {
            t->tokenizeState.token1 = Name::CodeText;
            t->tokenizeState.token2 = Name::CodeTextSequence;
            t->tokenizeState.token3 = Name::CodeTextData;
        } else {
            t->tokenizeState.token1 = Name::MathText;
            t->tokenizeState.token2 = Name::MathTextSequence;
            t->tokenizeState.token3 = Name::MathTextData;
        }
        t->tokenizeState.marker = marker;
        Enter(t, t->tokenizeState.token1);
        Enter(t, t->tokenizeState.token2);
        return StateRetry(StateName::RawTextSequenceOpen);
    }
    return StateNok();
}

State RawTextSequenceOpen(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        t->tokenizeState.size += 1;
        Consume(t);
        return StateNext(StateName::RawTextSequenceOpen);
    }
    if (t->tokenizeState.marker == '$' && t->tokenizeState.size == 1 &&
        !t->parseState->options->mathTextSingleDollar) {
        RawTextClear(t);
        return StateNok();
    }
    Exit(t, t->tokenizeState.token2);
    return StateRetry(StateName::RawTextBetween);
}

State RawTextBetween(Tokenizer* t) {
    if (t->current < 0) {
        RawTextClear(t);
        return StateNok();
    }
    if (t->current == '\n') {
        Enter(t, Name::LineEnding);
        Consume(t);
        Exit(t, Name::LineEnding);
        return StateNext(StateName::RawTextBetween);
    }
    if (t->current == (int32_t)t->tokenizeState.marker) {
        Enter(t, t->tokenizeState.token2);
        return StateRetry(StateName::RawTextSequenceClose);
    }
    Enter(t, t->tokenizeState.token3);
    return StateRetry(StateName::RawTextData);
}

State RawTextData(Tokenizer* t) {
    if (t->current < 0 || t->current == '\n' ||
        t->current == (int32_t)t->tokenizeState.marker) {
        Exit(t, t->tokenizeState.token3);
        return StateRetry(StateName::RawTextBetween);
    }
    Consume(t);
    return StateNext(StateName::RawTextData);
}

State RawTextSequenceClose(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        t->tokenizeState.sizeB += 1;
        Consume(t);
        return StateNext(StateName::RawTextSequenceClose);
    }
    Exit(t, t->tokenizeState.token2);
    if (t->tokenizeState.size == t->tokenizeState.sizeB) {
        Exit(t, t->tokenizeState.token1);
        t->tokenizeState.sizeB = 0;
        RawTextClear(t);
        return StateOk();
    }

    int32_t len = t->events.len;
    t->events[len - 2].name = t->tokenizeState.token3;
    t->events[len - 1].name = t->tokenizeState.token3;
    t->tokenizeState.sizeB = 0;
    return StateRetry(StateName::RawTextBetween);
}

}

#line 1 "src/markdown/construct_text.cpp"

namespace markdown {

static const uint8_t kTextMarkers[16] = {
    '!',
    '$',
    '&',
    '*',
    '<',
    'H',
    'W',
    '[',
    '\\',
    ']',
    '_',
    '`',
    'h',
    'w',
    '{',
    '~',
};

State TextStart(Tokenizer* t) {
    t->tokenizeState.markers = kTextMarkers;
    t->tokenizeState.markersLen = 16;
    TokenizerAttempt(t, StateNext(StateName::TextBefore),
                     StateNext(StateName::TextBefore));
    return StateRetry(StateName::GfmTaskListItemCheckStart);
}

State TextBefore(Tokenizer* t) {
    switch (t->current) {
        case -1:
            RegisterResolver(t, ResolveName::Data);
            RegisterResolver(t, ResolveName::Text);
            return StateOk();
        case '!':
            TokenizerAttempt(t, StateNext(StateName::TextBefore),
                             StateNext(StateName::TextBeforeData));
            return StateRetry(StateName::LabelStartImageStart);
        case '$':
        case '`':
            TokenizerAttempt(t, StateNext(StateName::TextBefore),
                             StateNext(StateName::TextBeforeData));
            return StateRetry(StateName::RawTextStart);
        case '&':
            TokenizerAttempt(t, StateNext(StateName::TextBefore),
                             StateNext(StateName::TextBeforeData));
            return StateRetry(StateName::CharacterReferenceStart);
        case '*':
        case '_':
        case '~':
            TokenizerAttempt(t, StateNext(StateName::TextBefore),
                             StateNext(StateName::TextBeforeData));
            return StateRetry(StateName::AttentionStart);
        case '<':
            TokenizerAttempt(t, StateNext(StateName::TextBefore),
                             StateNext(StateName::TextBeforeHtml));
            return StateRetry(StateName::AutolinkStart);
        case 'H':
        case 'h':
            TokenizerAttempt(t, StateNext(StateName::TextBefore),
                             StateNext(StateName::TextBeforeData));
            return StateRetry(StateName::GfmAutolinkLiteralProtocolStart);
        case 'W':
        case 'w':
            TokenizerAttempt(t, StateNext(StateName::TextBefore),
                             StateNext(StateName::TextBeforeData));
            return StateRetry(StateName::GfmAutolinkLiteralWwwStart);
        case '[':
            TokenizerAttempt(t, StateNext(StateName::TextBefore),
                             StateNext(StateName::TextBeforeLabelStartLink));
            return StateRetry(StateName::GfmLabelStartFootnoteStart);
        case '\\':
            TokenizerAttempt(t, StateNext(StateName::TextBefore),
                             StateNext(StateName::TextBeforeHardBreakEscape));
            return StateRetry(StateName::CharacterEscapeStart);
        case ']':
            TokenizerAttempt(t, StateNext(StateName::TextBefore),
                             StateNext(StateName::TextBeforeData));
            return StateRetry(StateName::LabelEndStart);
        default:

            return StateRetry(StateName::TextBeforeData);
    }
}

State TextBeforeHtml(Tokenizer* t) {

    TokenizerAttempt(t, StateNext(StateName::TextBefore),
                     StateNext(StateName::TextBeforeData));
    return StateRetry(StateName::HtmlTextStart);
}

State TextBeforeHardBreakEscape(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::TextBefore),
                     StateNext(StateName::TextBeforeData));
    return StateRetry(StateName::HardBreakEscapeStart);
}

State TextBeforeLabelStartLink(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::TextBefore),
                     StateNext(StateName::TextBeforeData));
    return StateRetry(StateName::LabelStartLinkStart);
}

State TextBeforeData(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::TextBefore), StateNok());
    return StateRetry(StateName::DataStart);
}

bool TextResolve(Tokenizer* t, Subresult*) {
    ResolveWhitespace(
        t, t->parseState->options->constructs.hardBreakTrailing, true);
    if (t->parseState->options->constructs.gfmAutolinkLiteral) {
        GfmAutolinkLiteralResolve(t);
    }
    EditMapConsume(t->map, t->events);
    return false;
}

static const uint8_t kStringMarkers[2] = {'&', '\\'};

State StringStart(Tokenizer* t) {
    t->tokenizeState.markers = kStringMarkers;
    t->tokenizeState.markersLen = 2;
    return StateRetry(StateName::StringBefore);
}

State StringBefore(Tokenizer* t) {
    if (t->current < 0) {
        RegisterResolver(t, ResolveName::Data);
        RegisterResolver(t, ResolveName::String);
        return StateOk();
    }
    if (t->current == '&') {
        TokenizerAttempt(t, StateNext(StateName::StringBefore),
                         StateNext(StateName::StringBeforeData));
        return StateRetry(StateName::CharacterReferenceStart);
    }
    if (t->current == '\\') {
        TokenizerAttempt(t, StateNext(StateName::StringBefore),
                         StateNext(StateName::StringBeforeData));
        return StateRetry(StateName::CharacterEscapeStart);
    }
    return StateRetry(StateName::StringBeforeData);
}

State StringBeforeData(Tokenizer* t) {
    TokenizerAttempt(t, StateNext(StateName::StringBefore), StateNok());
    return StateRetry(StateName::DataStart);
}

bool StringResolve(Tokenizer* t, Subresult*) {
    ResolveWhitespace(t, false, false);
    return false;
}

State CharacterEscapeStart(Tokenizer* t) {
    if (t->parseState->options->constructs.characterEscape &&
        t->current == '\\') {
        Enter(t, Name::CharacterEscape);
        Enter(t, Name::CharacterEscapeMarker);
        Consume(t);
        Exit(t, Name::CharacterEscapeMarker);
        return StateNext(StateName::CharacterEscapeInside);
    }
    return StateNok();
}

State CharacterEscapeInside(Tokenizer* t) {
    if (t->current >= 0 && IsAsciiPunctuation((uint8_t)t->current)) {
        Enter(t, Name::CharacterEscapeValue);
        Consume(t);
        Exit(t, Name::CharacterEscapeValue);
        Exit(t, Name::CharacterEscape);
        return StateOk();
    }
    return StateNok();
}

State CharacterReferenceStart(Tokenizer* t) {
    if (t->parseState->options->constructs.characterReference &&
        t->current == '&') {
        Enter(t, Name::CharacterReference);
        Enter(t, Name::CharacterReferenceMarker);
        Consume(t);
        Exit(t, Name::CharacterReferenceMarker);
        return StateNext(StateName::CharacterReferenceOpen);
    }
    return StateNok();
}

State CharacterReferenceOpen(Tokenizer* t) {
    if (t->current == '#') {
        Enter(t, Name::CharacterReferenceMarkerNumeric);
        Consume(t);
        Exit(t, Name::CharacterReferenceMarkerNumeric);
        return StateNext(StateName::CharacterReferenceNumeric);
    }
    t->tokenizeState.marker = '&';
    Enter(t, Name::CharacterReferenceValue);
    return StateRetry(StateName::CharacterReferenceValue);
}

State CharacterReferenceNumeric(Tokenizer* t) {
    if (t->current == 'x' || t->current == 'X') {
        Enter(t, Name::CharacterReferenceMarkerHexadecimal);
        Consume(t);
        Exit(t, Name::CharacterReferenceMarkerHexadecimal);
        Enter(t, Name::CharacterReferenceValue);
        t->tokenizeState.marker = 'x';
        return StateNext(StateName::CharacterReferenceValue);
    }
    Enter(t, Name::CharacterReferenceValue);
    t->tokenizeState.marker = '#';
    return StateRetry(StateName::CharacterReferenceValue);
}

State CharacterReferenceValue(Tokenizer* t) {
    if (t->current == ';' && t->tokenizeState.size > 0) {
        if (t->tokenizeState.marker == '&') {
            Slice slice = SliceFromIndices(
                t->parseState->bytes,
                t->point.index - t->tokenizeState.size, t->point.index);
            if (!DecodeNamed(t->parseState->scratch, slice.bytes).s) {
                t->tokenizeState.marker = 0;
                t->tokenizeState.size = 0;
                return StateNok();
            }
        }
        Exit(t, Name::CharacterReferenceValue);
        Enter(t, Name::CharacterReferenceMarkerSemi);
        Consume(t);
        Exit(t, Name::CharacterReferenceMarkerSemi);
        Exit(t, Name::CharacterReference);
        t->tokenizeState.marker = 0;
        t->tokenizeState.size = 0;
        return StateOk();
    }
    if (t->current >= 0 &&
        t->tokenizeState.size <
            CharacterReferenceValueMax(t->tokenizeState.marker) &&
        CharacterReferenceValueTest(t->tokenizeState.marker,
                                    (uint8_t)t->current)) {
        t->tokenizeState.size += 1;
        Consume(t);
        return StateNext(StateName::CharacterReferenceValue);
    }
    t->tokenizeState.marker = 0;
    t->tokenizeState.size = 0;
    return StateNok();
}

State HardBreakEscapeStart(Tokenizer* t) {
    if (t->parseState->options->constructs.hardBreakEscape &&
        t->current == '\\') {
        Enter(t, Name::HardBreakEscape);
        Consume(t);
        return StateNext(StateName::HardBreakEscapeAfter);
    }
    return StateNok();
}

State HardBreakEscapeAfter(Tokenizer* t) {
    if (t->current == '\n') {
        Exit(t, Name::HardBreakEscape);
        return StateOk();
    }
    return StateNok();
}

State LabelStartImageStart(Tokenizer* t) {
    if (t->parseState->options->constructs.labelStartImage &&
        t->current == '!') {
        Enter(t, Name::LabelImage);
        Enter(t, Name::LabelImageMarker);
        Consume(t);
        Exit(t, Name::LabelImageMarker);
        return StateNext(StateName::LabelStartImageOpen);
    }
    return StateNok();
}

State LabelStartImageOpen(Tokenizer* t) {
    if (t->current == '[') {
        Enter(t, Name::LabelMarker);
        Consume(t);
        Exit(t, Name::LabelMarker);
        Exit(t, Name::LabelImage);
        return StateNext(StateName::LabelStartImageAfter);
    }
    return StateNok();
}

State LabelStartImageAfter(Tokenizer* t) {

    if (t->parseState->options->constructs.gfmLabelStartFootnote &&
        t->current == '^') {
        return StateNok();
    }
    LabelStartMark start;
    start.kind = LabelKind::Image;
    start.startA = t->events.len - 6;
    start.startB = t->events.len - 1;
    VecAppend(t->tokenizeState.labelStarts, start);
    RegisterResolverBefore(t, ResolveName::Label);
    return StateOk();
}

State LabelStartLinkStart(Tokenizer* t) {
    if (t->parseState->options->constructs.labelStartLink &&
        t->current == '[') {
        int32_t start = t->events.len;
        Enter(t, Name::LabelLink);
        Enter(t, Name::LabelMarker);
        Consume(t);
        Exit(t, Name::LabelMarker);
        Exit(t, Name::LabelLink);
        LabelStartMark mark;
        mark.kind = LabelKind::Link;
        mark.startA = start;
        mark.startB = t->events.len - 1;
        VecAppend(t->tokenizeState.labelStarts, mark);
        RegisterResolverBefore(t, ResolveName::Label);
        return StateOk();
    }
    return StateNok();
}

State AutolinkStart(Tokenizer* t) {
    if (t->parseState->options->constructs.autolink && t->current == '<') {
        Enter(t, Name::Autolink);
        Enter(t, Name::AutolinkMarker);
        Consume(t);
        Exit(t, Name::AutolinkMarker);
        Enter(t, Name::AutolinkProtocol);
        return StateNext(StateName::AutolinkOpen);
    }
    return StateNok();
}

State AutolinkOpen(Tokenizer* t) {
    if (t->current >= 0 && IsAsciiAlpha((uint8_t)t->current)) {
        Consume(t);
        return StateNext(StateName::AutolinkSchemeOrEmailAtext);
    }
    if (t->current == '@') {
        return StateNok();
    }
    return StateRetry(StateName::AutolinkEmailAtext);
}

static bool IsSchemeByte(int32_t byte) {
    return byte == '+' || byte == '-' || byte == '.' ||
           (byte >= 0 && IsAsciiAlphanumeric((uint8_t)byte));
}

State AutolinkSchemeOrEmailAtext(Tokenizer* t) {
    if (IsSchemeByte(t->current)) {

        t->tokenizeState.size = 1;
        return StateRetry(StateName::AutolinkSchemeInsideOrEmailAtext);
    }
    return StateRetry(StateName::AutolinkEmailAtext);
}

State AutolinkSchemeInsideOrEmailAtext(Tokenizer* t) {
    if (t->current == ':') {
        Consume(t);
        t->tokenizeState.size = 0;
        return StateNext(StateName::AutolinkUrlInside);
    }
    if (IsSchemeByte(t->current) &&
        t->tokenizeState.size < kAutolinkSchemeSizeMax) {
        Consume(t);
        t->tokenizeState.size += 1;
        return StateNext(StateName::AutolinkSchemeInsideOrEmailAtext);
    }
    t->tokenizeState.size = 0;
    return StateRetry(StateName::AutolinkEmailAtext);
}

State AutolinkUrlInside(Tokenizer* t) {
    if (t->current == '>') {
        Exit(t, Name::AutolinkProtocol);
        Enter(t, Name::AutolinkMarker);
        Consume(t);
        Exit(t, Name::AutolinkMarker);
        Exit(t, Name::Autolink);
        return StateOk();
    }
    if (t->current < 0 || t->current <= 0x1f || t->current == ' ' ||
        t->current == '<' || t->current == 0x7f) {
        return StateNok();
    }
    Consume(t);
    return StateNext(StateName::AutolinkUrlInside);
}

State AutolinkEmailAtext(Tokenizer* t) {
    if (t->current == '@') {
        Consume(t);
        return StateNext(StateName::AutolinkEmailAtSignOrDot);
    }

    int32_t c = t->current;
    bool atext = (c >= '#' && c <= '\'') || c == '*' || c == '+' ||
                 (c >= '-' && c <= '9') || c == '=' || c == '?' ||
                 (c >= 'A' && c <= 'Z') || (c >= '^' && c <= '~');
    if (atext) {
        Consume(t);
        return StateNext(StateName::AutolinkEmailAtext);
    }
    return StateNok();
}

State AutolinkEmailAtSignOrDot(Tokenizer* t) {
    if (t->current >= 0 && IsAsciiAlphanumeric((uint8_t)t->current)) {
        return StateRetry(StateName::AutolinkEmailValue);
    }
    return StateNok();
}

State AutolinkEmailLabel(Tokenizer* t) {
    if (t->current == '.') {
        Consume(t);
        t->tokenizeState.size = 0;
        return StateNext(StateName::AutolinkEmailAtSignOrDot);
    }
    if (t->current == '>') {
        int32_t index = t->events.len;
        Exit(t, Name::AutolinkProtocol);

        t->events[index - 1].name = Name::AutolinkEmail;
        t->events[index].name = Name::AutolinkEmail;
        Enter(t, Name::AutolinkMarker);
        Consume(t);
        Exit(t, Name::AutolinkMarker);
        Exit(t, Name::Autolink);
        t->tokenizeState.size = 0;
        return StateOk();
    }
    return StateRetry(StateName::AutolinkEmailValue);
}

State AutolinkEmailValue(Tokenizer* t) {
    bool value = t->current == '-' ||
                 (t->current >= 0 && IsAsciiAlphanumeric((uint8_t)t->current));
    if (value && t->tokenizeState.size < kAutolinkDomainSizeMax) {
        StateName name = t->current == '-' ? StateName::AutolinkEmailValue
                                           : StateName::AutolinkEmailLabel;
        t->tokenizeState.size += 1;
        Consume(t);
        return StateNext(name);
    }
    t->tokenizeState.size = 0;
    return StateNok();
}

struct Sequence {
    uint8_t marker = 0;
    ArenaVec<int32_t> stack {};
    int32_t index = 0;
    Point startPoint = {};
    Point endPoint = {};
    int32_t size = 0;
    bool open = false;
    bool close = false;
};

State AttentionStart(Tokenizer* t) {
    bool emphasis = t->parseState->options->constructs.attention &&
                    (t->current == '*' || t->current == '_');
    bool strikethrough =
        t->parseState->options->constructs.gfmStrikethrough && t->current == '~';
    if (emphasis || strikethrough) {
        t->tokenizeState.marker = (uint8_t)t->current;
        Enter(t, Name::AttentionSequence);
        return StateRetry(StateName::AttentionInside);
    }
    return StateNok();
}

State AttentionInside(Tokenizer* t) {
    if (t->current == (int32_t)t->tokenizeState.marker) {
        Consume(t);
        return StateNext(StateName::AttentionInside);
    }
    Exit(t, Name::AttentionSequence);
    RegisterResolver(t, ResolveName::Attention);
    t->tokenizeState.marker = 0;
    return StateOk();
}

static bool StackEq(const ArenaVec<int32_t>& a, const ArenaVec<int32_t>& b) {
    if (a.len != b.len) {
        return false;
    }

    ArenaVec<int32_t>::Iter ia = a.begin();
    ArenaVec<int32_t>::Iter ib = b.begin();
    for (; ia != a.end(); ++ia, ++ib) {
        if (*ia != *ib) {
            return false;
        }
    }
    return true;
}

static void GetSequences(Tokenizer* t, Vec<Sequence>& sequences) {
    Arena* a = t->parseState->scratch;
    int32_t index = 0;
    ArenaVec<int32_t> stack {};
    while (index < t->events.len) {
        const Event& enter = t->events[index];
        if (enter.name == Name::AttentionSequence) {
            if (enter.kind == Kind::Enter) {
                const Event& exit = t->events[index + 1];
                uint8_t marker =
                    (uint8_t)t->parseState->bytes.s[enter.point.index];
                int32_t beforeChar =
                    CharBeforeIndex(t->parseState->bytes, enter.point.index);
                CharKind before = Classify(beforeChar);
                int32_t afterChar =
                    CharAfterIndex(t->parseState->bytes, exit.point.index);
                CharKind after = Classify(afterChar);
                bool gfm = t->parseState->options->constructs.gfmStrikethrough;
                bool open =
                    after == CharKind::Other ||
                    (after == CharKind::Punctuation && before != CharKind::Other) ||
                    (marker != '~' && (afterChar == '*' || afterChar == '_')) ||
                    (marker != '~' && gfm && afterChar == '~');
                bool close =
                    before == CharKind::Other ||
                    (before == CharKind::Punctuation && after != CharKind::Other) ||
                    (marker != '~' && (beforeChar == '*' || beforeChar == '_')) ||
                    (marker != '~' && gfm && beforeChar == '~');

                Sequence sequence;
                sequence.index = index;

                for (int32_t eventIndex : stack) {
                    sequence.stack.Append(a, eventIndex);
                }
                sequence.startPoint = enter.point;
                sequence.endPoint = exit.point;
                sequence.size = exit.point.index - enter.point.index;
                sequence.open = marker == '_'
                                    ? (open && (before != CharKind::Other || !close))
                                    : open;
                sequence.close = marker == '_'
                                     ? (close && (after != CharKind::Other || !open))
                                     : close;
                sequence.marker = marker;
                VecAppend(sequences, sequence);
            }
        } else if (enter.kind == Kind::Enter) {
            stack.Append(a, index);
        } else if (stack.len > 0) {
            stack.Pop();
        }
        index += 1;
    }
}

static void SequencesRemove(Vec<Sequence>& sequences, int32_t index) {
    for (int32_t i = index; i + 1 < sequences.len; i++) {
        sequences[i] = sequences[i + 1];
    }
    sequences.len -= 1;
}

static int32_t MatchSequences(Tokenizer* t, Vec<Sequence>& sequences,
                              int32_t open, int32_t close) {
    int32_t next = close;

    int32_t take =
        (sequences[open].size > 1 && sequences[close].size > 1) ? 2 : 1;

    for (int32_t between = open + 1; between < close; between++) {
        sequences[between].open = false;
    }

    Name groupName = Name::Emphasis;
    Name seqName = Name::EmphasisSequence;
    Name textName = Name::EmphasisText;
    if (sequences[open].marker == '~') {
        groupName = Name::GfmStrikethrough;
        seqName = Name::GfmStrikethroughSequence;
        textName = Name::GfmStrikethroughText;
    } else if (take != 1) {
        groupName = Name::Strong;
        seqName = Name::StrongSequence;
        textName = Name::StrongText;
    }

    int32_t openIndex = sequences[open].index;
    int32_t closeIndex = sequences[close].index;
    Point openExit = sequences[open].endPoint;
    Point closeEnter = sequences[close].startPoint;

    sequences[open].size -= take;
    sequences[close].size -= take;
    sequences[open].endPoint.column -= take;
    sequences[open].endPoint.index -= take;
    sequences[close].startPoint.column += take;
    sequences[close].startPoint.index += take;

    Event before[4];
    before[0].kind = Kind::Enter;
    before[0].name = groupName;
    before[0].point = sequences[open].endPoint;
    before[1].kind = Kind::Enter;
    before[1].name = seqName;
    before[1].point = sequences[open].endPoint;
    before[2].kind = Kind::Exit;
    before[2].name = seqName;
    before[2].point = openExit;
    before[3].kind = Kind::Enter;
    before[3].name = textName;
    before[3].point = openExit;
    EditMapAddBefore(t->map, openIndex + 2, 0, before, 4);

    Event after[4];
    after[0].kind = Kind::Exit;
    after[0].name = textName;
    after[0].point = closeEnter;
    after[1].kind = Kind::Enter;
    after[1].name = seqName;
    after[1].point = closeEnter;
    after[2].kind = Kind::Exit;
    after[2].name = seqName;
    after[2].point = sequences[close].startPoint;
    after[3].kind = Kind::Exit;
    after[3].name = groupName;
    after[3].point = sequences[close].startPoint;
    EditMapAdd(t->map, closeIndex, 0, after, 4);

    if (sequences[close].size == 0) {
        SequencesRemove(sequences, close);
        EditMapAdd(t->map, closeIndex, 2, nullptr, 0);
    } else {
        t->events[closeIndex].point = sequences[close].startPoint;
    }

    if (sequences[open].size == 0) {
        SequencesRemove(sequences, open);
        EditMapAdd(t->map, openIndex, 2, nullptr, 0);
        next -= 1;
    } else {
        t->events[openIndex + 1].point = sequences[open].endPoint;
    }

    return next;
}

bool AttentionResolve(Tokenizer* t, Subresult*) {
    Vec<Sequence> sequences;
    GetSequences(t, sequences);

    int32_t close = 0;
    while (close < sequences.len) {
        int32_t nextIndex = close + 1;
        if (sequences[close].close) {
            int32_t open = close;
            while (open > 0) {
                open -= 1;
                if (!sequences[open].open ||
                    sequences[close].marker != sequences[open].marker ||
                    !StackEq(sequences[close].stack, sequences[open].stack)) {
                    continue;
                }

                if ((sequences[open].close || sequences[close].open) &&
                    sequences[close].size % 3 != 0 &&
                    (sequences[open].size + sequences[close].size) % 3 == 0) {
                    continue;
                }

                if (sequences[close].marker == '~' &&
                    (sequences[close].size != sequences[open].size ||
                     sequences[close].size > 2 ||
                     (sequences[close].size == 1 &&
                      !t->parseState->options->gfmStrikethroughSingleTilde))) {
                    continue;
                }
                nextIndex = MatchSequences(t, sequences, open, close);
                break;
            }
        }
        close = nextIndex;
    }

    for (int32_t index = 0; index < sequences.len; index++) {
        t->events[sequences[index].index].name = Name::Data;
        t->events[sequences[index].index + 1].name = Name::Data;
    }

    EditMapConsume(t->map, t->events);
    return false;
}

}

#line 1 "src/markdown/markdown.cpp"

namespace markdown {

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
    ParseState parseState;
    parseState.a = a;

    parseState.scratch = base::ArenaNew();
    parseState.options = &options;
    parseState.bytes = source;

    Vec<Event> events = Parse(&parseState);
    Node* tree = ToMdastCompile(events, &parseState);

    base::ArenaDelete(parseState.scratch);
    return tree;
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
    if (value.len > 0) {
        memcpy(out + at, value.s, (size_t)value.len);
        at += value.len;
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
    return rec ? RecStr(rec).len : 0;
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
    if (!s.s || s.len <= 0) {
        return;
    }
    int32_t head = 0;
    char* rec = RecNew(a, n, k, (uint32_t)s.len, &head);
    if (rec) {
        memcpy(rec + head, s.s, (size_t)s.len);
    }
}

void NodeGrowStr(Arena* a, Node* n, NodeStrKind k, Str more) {
    if (!a || !n || !more.s || more.len <= 0) {
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
    uint32_t nlen = (uint32_t)had.len + (uint32_t)more.len;
    int32_t nhead = kRecLen + base::VarintSize(nlen);

    uint64_t used = base::ArenaUsed(a);
    uint64_t end = (uint64_t)base::ArenaOffsetOf(a, rec) + (uint64_t)head +
                   (uint64_t)had.len + 1;

    bool newest = end == used;
    uint64_t want = newest ? (uint64_t)(nhead - head) + (uint64_t)more.len
                           : (uint64_t)nhead + nlen + 1;
    char* dst = (char*)a->Push(want, 1, false);
    if (!dst) {
        return;
    }
    uint64_t at = base::ArenaOffsetOf(a, dst);

    if (newest && at == used) {
        if (nhead != head) {
            memmove(rec + nhead, rec + head, (size_t)had.len);
        }
        base::VarintPut(rec + kRecLen, nlen);
        memcpy(rec + nhead + had.len, more.s, (size_t)more.len);
        rec[nhead + nlen] = 0;
        return;
    }

    dst[kRecKind] = (char)(uint8_t)k;
    base::VarintPut(dst + kRecLen, nlen);
    memcpy(dst + nhead, had.s, (size_t)had.len);
    memcpy(dst + nhead + had.len, more.s, (size_t)more.len);
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
    if (stop > md.len) {
        stop = md.len;
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
        if (byte == '\r' && at + 1 < md.len && md.s[at + 1] == '\n') {

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

#line 1 "src/markdown/parser.cpp"

namespace markdown {

bool ResolveCall(Tokenizer* t, ResolveName name, Subresult* out) {
    switch (name) {
        case ResolveName::Label:
            return LabelEndResolve(t, out);
        case ResolveName::Attention:
            return AttentionResolve(t, out);
        case ResolveName::GfmTable:
            return GfmTableResolve(t, out);
        case ResolveName::HeadingAtx:
            return HeadingAtxResolve(t, out);
        case ResolveName::HeadingSetext:
            return HeadingSetextResolve(t, out);
        case ResolveName::ListItem:
            return ListItemResolve(t, out);
        case ResolveName::Content:
            return ContentResolve(t, out);
        case ResolveName::Data:
            return DataResolve(t, out);
        case ResolveName::String:
            return StringResolve(t, out);
        case ResolveName::Text:
            return TextResolve(t, out);
    }
    return false;
}

void SubtokenizeLinkTo(Vec<Event>& events, int32_t previous, int32_t next) {
    events[previous].link.next = next;
    events[next].link.previous = previous;
}

void SubtokenizeLink(Vec<Event>& events, int32_t index) {
    SubtokenizeLinkTo(events, index - 2, index);
}

struct DivideSlice {
    int32_t linkIndex;
    int32_t sliceStart;
};

void DivideEvents(EditMap& map, const Vec<Event>& events, int32_t linkIndex,
                  Vec<Event>& childEvents, int32_t* accA, int32_t* accB) {
    int32_t childIndex = 0;
    Vec<DivideSlice> slices;
    int32_t sliceStart = 0;
    int32_t oldPrev = -1;
    int32_t len = childEvents.len;

    while (childIndex < len) {
        const Point& current = childEvents[childIndex].point;
        const Point& end = events[linkIndex + 1].point;

        if (current.index > end.index ||
            (current.index == end.index && current.vs > end.vs)) {
            DivideSlice slice = {linkIndex, sliceStart};
            VecAppend(slices, slice);
            sliceStart = childIndex;
            linkIndex = events[linkIndex].link.next;
        }

        if (childEvents[childIndex].hasLink &&
            childEvents[childIndex].link.previous != -1) {
            Event& prevEvent = childEvents[oldPrev];
            int32_t newLink = slices.len == 0
                                  ? oldPrev + linkIndex + 2
                                  : oldPrev + linkIndex - (slices.len - 1) * 2;
            prevEvent.link.next = newLink + *accB - *accA;
        }

        if (childEvents[childIndex].hasLink &&
            childEvents[childIndex].link.next != -1) {
            int32_t next = childEvents[childIndex].link.next;
            oldPrev = childEvents[next].link.previous;
            if (childEvents[next].link.previous != -1) {
                childEvents[next].link.previous =
                    childEvents[next].link.previous + linkIndex -
                    (slices.len * 2) + *accB - *accA;
            }
        }

        childIndex += 1;
    }

    if (childEvents.len > 0) {
        DivideSlice slice = {linkIndex, sliceStart};
        VecAppend(slices, slice);
    }

    int32_t index = slices.len;
    while (index > 0) {
        index -= 1;
        int32_t from = slices[index].sliceStart;
        EditMapAdd(map, slices[index].linkIndex, 2, childEvents.els + from,
                   childEvents.len - from);
        childEvents.len = from;
    }

    *accA = *accA + slices.len * 2;
    *accB = *accB + len;
}

Subresult Subtokenize(Vec<Event>& events, ParseState* parseState,
                      bool hasFilter, ContentKind filter) {
    EditMap map;
    map.a = parseState->scratch;
    int32_t index = 0;
    Subresult value;
    value.done = true;
    int32_t accA = 0;
    int32_t accB = 0;

    while (index < events.len) {
        if (events[index].hasLink && events[index].link.previous == -1 &&
            (!hasFilter || events[index].link.content == filter)) {
            const Link& link = events[index].link;
            int32_t linkIndex = index;
            Tokenizer* tokenizer = TokenizerNew(events[index].point, parseState);

            StateName startName = StateName::TextStart;
            if (link.content == ContentKind::Content) {
                startName = StateName::ContentDefinitionBefore;
            } else if (link.content == ContentKind::String) {
                startName = StateName::StringStart;
            }
            State state = StateNext(startName);

            if (parseState->options->constructs.gfmTaskListItem && index > 2 &&
                events[index - 1].kind == Kind::Enter &&
                events[index - 1].name == Name::Paragraph) {
                Name names[4] = {Name::BlankLineEnding, Name::Definition,
                                 Name::LineEnding, Name::SpaceOrTab};
                int32_t before = SkipOptBack(events, index - 2, names, 4);
                if (events[before].kind == Kind::Exit &&
                    events[before].name == Name::ListItemPrefix) {
                    tokenizer->tokenizeState
                        .documentAtFirstParagraphOfListItem = true;
                }
            }

            while (linkIndex != -1) {
                const Event& enter = events[linkIndex];
                const Link& linkCurr = enter.link;
                if (linkCurr.previous != -1) {
                    DefineSkip(tokenizer, enter.point);
                }
                const Point& end = events[linkIndex + 1].point;
                state = Push(tokenizer, enter.point.index, enter.point.vs,
                             end.index, end.vs, state);
                linkIndex = linkCurr.next;
            }

            Subresult result = Flush(tokenizer, state, true);
            SubresultAppend(value, result);
            value.done = false;
            DivideEvents(map, events, index, tokenizer->events, &accA, &accB);
            TokenizerFree(tokenizer);
        }
        index += 1;
    }

    EditMapConsume(map, events);
    return value;
}

Vec<Event> Parse(ParseState* parseState) {
    Point start;
    start.line = 1;
    start.column = 1;
    start.index = 0;
    start.vs = 0;

    Tokenizer* tokenizer = TokenizerNew(start, parseState);

    VecReserve(tokenizer->events, parseState->bytes.len / 2);
    State state = Push(tokenizer, 0, 0, parseState->bytes.len, 0,
                       StateNext(StateName::DocumentStart));
    Subresult result = Flush(tokenizer, state, true);

    Vec<Event> events;
    events.els = tokenizer->events.els;
    events.len = tokenizer->events.len;
    events.cap = tokenizer->events.cap;
    tokenizer->events.els = nullptr;
    tokenizer->events.len = 0;
    tokenizer->events.cap = 0;
    TokenizerFree(tokenizer);

    for (;;) {
        for (int32_t i = 0; i < result.gfmFootnoteDefinitions.len; i++) {
            VecAppend(parseState->gfmFootnoteDefinitions,
                      result.gfmFootnoteDefinitions[i]);
        }
        for (int32_t i = 0; i < result.definitions.len; i++) {
            VecAppend(parseState->definitions, result.definitions[i]);
        }
        result.gfmFootnoteDefinitions.len = 0;
        result.definitions.len = 0;
        if (result.done) {
            return events;
        }
        result = Subtokenize(events, parseState, false, ContentKind::Flow);
    }
}

}

#line 1 "src/markdown/state.cpp"

namespace markdown {

using StateFn = State (*)(Tokenizer*);

static StateFn const kStateFns[] = {
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
};
static_assert(sizeof(kStateFns) / sizeof(kStateFns[0]) ==
              (uint16_t)StateName::Count);

State Call(Tokenizer* t, StateName name) {
    return kStateFns[(uint16_t)name](t);
}

}

#line 1 "src/markdown/to_mdast.cpp"

namespace markdown {

using base::Alloc;

struct Reference {
    ReferenceKind kind = ReferenceKind::Shortcut;
    bool kindSome = true;
    Str identifier = {};
    Str label = {};
};

struct TreeFrame {
    Node* tree = nullptr;

    ArenaVec<Node*> stack{};
    ArenaVec<int32_t> eventStack{};
};

struct CompileContext {
    Arena* a = nullptr;
    const Vec<Event>* events = nullptr;
    Str bytes = {};
    uint8_t characterReferenceMarker = 0;
    bool gfmTableInside = false;
    bool hardBreakAfter = false;
    bool headingSetextTextAfter = false;
    Vec<Reference> mediaReferenceStack;
    bool rawFlowFenceSeen = false;
    Vec<TreeFrame> trees;
    int32_t index = 0;
};

static Str IdentifierFrom(Arena* a, Str value) {
    Str id = NormalizeIdentifier(a, value);
    for (int32_t i = 0; i < id.len; i++) {
        if (id.s[i] >= 'A' && id.s[i] <= 'Z') {
            id.s[i] = (char)(id.s[i] + 32);
        }
    }
    return id;
}

static Str TrimEol(Str value, bool atStart, bool atEnd) {
    int32_t start = 0;
    int32_t end = value.len;
    if (atStart && value.len > 0) {
        if (value.s[0] == '\n') {
            start += 1;
        } else if (value.s[0] == '\r') {
            start += 1;
            if (value.len > 1 && value.s[1] == '\n') {
                start += 1;
            }
        }
    }
    if (atEnd && end > start) {
        if (value.s[end - 1] == '\n') {
            end -= 1;
            if (end > start && value.s[end - 1] == '\r') {
                end -= 1;
            }
        } else if (value.s[end - 1] == '\r') {
            end -= 1;
        }
    }
    return Str(value.s + start, end - start);
}

static TreeFrame& TreeTail(CompileContext* c) {
    return c->trees[c->trees.len - 1];
}

static void Keep(CompileContext* c, Node* n, NodeStrKind k, Str s) {
    NodeSetStr(c->a, n, k, s);
}

static Str Get(CompileContext* c, const Node* n, NodeStrKind k) {
    return NodeGetStr(c->a, n, k);
}

static void Grow(CompileContext* c, Node* n, NodeStrKind k, Str more) {
    NodeGrowStr(c->a, n, k, more);
}

static Node* TailMut(CompileContext* c) {
    TreeFrame& frame = TreeTail(c);
    return frame.stack.len > 0 ? frame.stack[frame.stack.len - 1] : frame.tree;
}

static Node* TailPenultimateMut(CompileContext* c) {
    TreeFrame& frame = TreeTail(c);
    return frame.stack.len > 1 ? frame.stack[frame.stack.len - 2] : frame.tree;
}

static void Buffer(CompileContext* c) {
    TreeFrame frame;
    frame.tree = NodeNew(c->a, NodeKind::Paragraph);
    VecAppend(c->trees, frame);
}

static Node* Resume(CompileContext* c) {
    TreeFrame frame = c->trees[--c->trees.len];
    return frame.tree;
}

static void TailPush(CompileContext* c, Node* child) {
    Node* node = TailMut(c);
    NodeAddChild(c->a, node, child);
    TreeFrame& frame = TreeTail(c);
    frame.stack.Append(c->a, child);
    frame.eventStack.Append(c->a, c->index);
}

static void TailPushAgain(CompileContext* c, Node* child) {
    TreeFrame& frame = TreeTail(c);
    frame.stack.Append(c->a, child);
    frame.eventStack.Append(c->a, c->index);
}

static void TailPop(CompileContext* c) {
    TreeFrame& frame = TreeTail(c);
    frame.stack.Pop();
    frame.eventStack.Pop();
}

static void OnEnterBuffer(CompileContext* c) {
    Buffer(c);
}

static void OnEnterData(CompileContext* c) {
    Node* parent = TailMut(c);
    Node* last = NodeLastChild(c->a, parent);
    if (last && last->kind == NodeKind::Text) {
        TailPushAgain(c, last);
    } else {
        TailPush(c, NodeNew(c->a, NodeKind::Text));
    }
}

static void OnEnterAutolink(CompileContext* c) {
    TailPush(c, NodeNew(c->a, NodeKind::Link));
}

static void OnEnterCodeFenced(CompileContext* c) {
    TailPush(c, NodeNew(c->a, NodeKind::Code));
}

static void OnEnterGfmAutolinkLiteral(CompileContext* c) {
    OnEnterAutolink(c);
    OnEnterData(c);
}

static void OnEnterList(CompileContext* c) {
    Node* node = NodeNew(c->a, NodeKind::List);
    node->Set(NodeOrdered, (*c->events)[c->index].name == Name::ListOrdered);
    node->Set(NodeSpread, ListLoose(*c->events, c->index, false));
    TailPush(c, node);
}

static void OnEnterListItem(CompileContext* c) {
    Node* node = NodeNew(c->a, NodeKind::ListItem);
    node->Set(NodeSpread, ListItemLoose(*c->events, c->index));
    TailPush(c, node);
}

static void OnEnterMedia(CompileContext* c, NodeKind kind) {
    TailPush(c, NodeNew(c->a, kind));
    Reference reference;
    VecAppend(c->mediaReferenceStack, reference);
}

static void Enter(CompileContext* c) {
    switch ((*c->events)[c->index].name) {
        case Name::AutolinkEmail:
        case Name::AutolinkProtocol:
        case Name::CharacterEscapeValue:
        case Name::CharacterReference:
        case Name::CodeFlowChunk:
        case Name::CodeTextData:
        case Name::Data:
        case Name::FrontmatterChunk:
        case Name::HtmlFlowData:
        case Name::HtmlTextData:
        case Name::MathFlowChunk:
        case Name::MathTextData:
            OnEnterData(c);
            break;

        case Name::CodeFencedFenceInfo:
        case Name::CodeFencedFenceMeta:
        case Name::DefinitionDestinationString:
        case Name::DefinitionLabelString:
        case Name::DefinitionTitleString:
        case Name::GfmFootnoteDefinitionLabelString:
        case Name::LabelText:
        case Name::MathFlowFenceMeta:
        case Name::ReferenceString:
        case Name::ResourceDestinationString:
        case Name::ResourceTitleString:
            OnEnterBuffer(c);
            break;

        case Name::Autolink:
            OnEnterAutolink(c);
            break;
        case Name::BlockQuote:
            TailPush(c, NodeNew(c->a, NodeKind::Blockquote));
            break;
        case Name::CodeFenced:
            OnEnterCodeFenced(c);
            break;
        case Name::CodeIndented:
            OnEnterCodeFenced(c);
            OnEnterBuffer(c);
            break;
        case Name::CodeText:
            TailPush(c, NodeNew(c->a, NodeKind::InlineCode));
            Buffer(c);
            break;
        case Name::MathText:
            TailPush(c, NodeNew(c->a, NodeKind::InlineMath));
            Buffer(c);
            break;
        case Name::Definition:
            TailPush(c, NodeNew(c->a, NodeKind::Definition));
            break;
        case Name::Emphasis:
            TailPush(c, NodeNew(c->a, NodeKind::Emphasis));
            break;
        case Name::Frontmatter: {
            int32_t index = (*c->events)[c->index].point.index;
            TailPush(c,
                     NodeNew(c->a, c->bytes.s[index] == '+' ? NodeKind::Toml
                                                            : NodeKind::Yaml));
            Buffer(c);
            break;
        }
        case Name::GfmAutolinkLiteralEmail:
        case Name::GfmAutolinkLiteralMailto:
        case Name::GfmAutolinkLiteralProtocol:
        case Name::GfmAutolinkLiteralWww:
        case Name::GfmAutolinkLiteralXmpp:
            OnEnterGfmAutolinkLiteral(c);
            break;
        case Name::GfmFootnoteCall:
            OnEnterMedia(c, NodeKind::FootnoteReference);
            break;
        case Name::GfmFootnoteDefinition:
            TailPush(c, NodeNew(c->a, NodeKind::FootnoteDefinition));
            break;
        case Name::GfmStrikethrough:
            TailPush(c, NodeNew(c->a, NodeKind::Delete));
            break;
        case Name::GfmTable: {
            Node* node = NodeNew(c->a, NodeKind::Table);
            NodeSetPerKind(c->a, node,
                           GfmTableAlign(*c->events, c->index, c->a));
            TailPush(c, node);
            c->gfmTableInside = true;
            break;
        }
        case Name::GfmTableRow:
            TailPush(c, NodeNew(c->a, NodeKind::TableRow));
            break;
        case Name::GfmTableCell:
            TailPush(c, NodeNew(c->a, NodeKind::TableCell));
            break;
        case Name::HardBreakEscape:
        case Name::HardBreakTrailing:
            TailPush(c, NodeNew(c->a, NodeKind::Break));
            break;
        case Name::HeadingAtx:
        case Name::HeadingSetext:

            TailPush(c, NodeNew(c->a, NodeKind::Heading));
            break;
        case Name::HtmlFlow:
        case Name::HtmlText:
            TailPush(c, NodeNew(c->a, NodeKind::Html));
            Buffer(c);
            break;
        case Name::Image:
            OnEnterMedia(c, NodeKind::Image);
            break;
        case Name::Link:
            OnEnterMedia(c, NodeKind::Link);
            break;
        case Name::ListItem:
            OnEnterListItem(c);
            break;
        case Name::ListOrdered:
        case Name::ListUnordered:
            OnEnterList(c);
            break;
        case Name::MathFlow:
            TailPush(c, NodeNew(c->a, NodeKind::Math));
            break;
        case Name::Paragraph:
            TailPush(c, NodeNew(c->a, NodeKind::Paragraph));
            break;
        case Name::Reference:
            c->mediaReferenceStack[c->mediaReferenceStack.len - 1]
                .kind = ReferenceKind::Collapsed;
            c->mediaReferenceStack[c->mediaReferenceStack.len - 1]
                .kindSome = true;
            break;
        case Name::Resource:
            c->mediaReferenceStack[c->mediaReferenceStack.len - 1]
                .kindSome = false;
            break;
        case Name::Strong:
            TailPush(c, NodeNew(c->a, NodeKind::Strong));
            break;
        case Name::ThematicBreak:
            TailPush(c, NodeNew(c->a, NodeKind::ThematicBreak));
            break;
        default:
            break;
    }
}

static Slice ExitSlice(CompileContext* c) {
    Position position = PositionFromExitEvent(*c->events, c->index);
    return SliceFromPosition(c->bytes, position);
}

static void OnExit(CompileContext* c) {
    TailPop(c);
}

static void OnExitData(CompileContext* c) {
    Str value = ExitSlice(c).bytes;
    Node* node = TailMut(c);
    Grow(c, node, NodeStrKind::Value, value);
    OnExit(c);
}

static void OnExitAutolinkProtocol(CompileContext* c) {
    OnExitData(c);
    Str value = ExitSlice(c).bytes;
    Node* link = TailMut(c);
    Grow(c, link, NodeStrKind::Url, value);
}

static void OnExitAutolinkEmail(CompileContext* c) {
    OnExitData(c);
    Str value = ExitSlice(c).bytes;
    Node* link = TailMut(c);
    Grow(c, link, NodeStrKind::Url, StrL("mailto:"));
    Grow(c, link, NodeStrKind::Url, value);
}

static void OnExitCharacterReferenceValue(CompileContext* c) {

    base::TempStr value = CharacterReferenceDecodeTemp(
        ExitSlice(c).bytes, c->characterReferenceMarker);
    Node* node = TailMut(c);
    Grow(c, node, NodeStrKind::Value, value);
    c->characterReferenceMarker = 0;
}

static void OnExitRawFlowFence(CompileContext* c) {
    if (!c->rawFlowFenceSeen) {
        Buffer(c);
        c->rawFlowFenceSeen = true;
    }
}

static void OnExitRawFlow(CompileContext* c) {
    Str value = TrimEol(NodeToString(c->a, Resume(c)), true, true);
    Keep(c, TailMut(c), NodeStrKind::Value, value);
    OnExit(c);
    c->rawFlowFenceSeen = false;
}

static void OnExitCodeIndented(CompileContext* c) {
    Str value = TrimEol(NodeToString(c->a, Resume(c)), false, true);
    Keep(c, TailMut(c), NodeStrKind::Value, value);
    OnExit(c);
    c->rawFlowFenceSeen = false;
}

static void OnExitRawText(CompileContext* c) {
    Str value = NodeToString(c->a, Resume(c));

    if (c->gfmTableInside) {
        int32_t index = 0;
        int32_t len = value.len;
        bool replace = false;
        char* bytes = value.s;
        while (index < len) {
            if (index + 1 < len && bytes[index] == '\\' &&
                bytes[index + 1] == '|') {
                replace = true;
                for (int32_t i = index; i + 1 < len; i++) {
                    bytes[i] = bytes[i + 1];
                }
                len -= 1;
            }
            index += 1;
        }
        if (replace) {
            value.len = len;
            value.s[len] = 0;
        }
    }

    if (value.len > 2 && value.s[0] == ' ' && value.s[value.len - 1] == ' ') {
        bool allSpaces = true;
        for (int32_t i = 0; i < value.len; i++) {
            if (value.s[i] != ' ') {
                allSpaces = false;
                break;
            }
        }
        if (!allSpaces) {
            value = Str(value.s + 1, value.len - 2);
        }
    }

    Keep(c, TailMut(c), NodeStrKind::Value, value);
    OnExit(c);
}

static void OnExitDefinitionId(CompileContext* c) {
    Str label = NodeToString(c->a, Resume(c));
    Str identifier = IdentifierFrom(c->a, ExitSlice(c).bytes);
    Node* node = TailMut(c);
    Keep(c, node, NodeStrKind::Label, label);
    Keep(c, node, NodeStrKind::Identifier, identifier);
}

static void OnExitGfmAutolinkLiteral(CompileContext* c) {
    OnExitData(c);
    Str value = ExitSlice(c).bytes;
    Name name = (*c->events)[c->index].name;
    Node* link = TailMut(c);
    if (name == Name::GfmAutolinkLiteralEmail) {
        Grow(c, link, NodeStrKind::Url, StrL("mailto:"));
    } else if (name == Name::GfmAutolinkLiteralWww) {
        Grow(c, link, NodeStrKind::Url, StrL("http://"));
    }
    Grow(c, link, NodeStrKind::Url, value);
    OnExit(c);
}

static void OnExitGfmTaskListItemValue(CompileContext* c) {
    bool checked = (*c->events)[c->index]
                       .name == Name::GfmTaskListItemValueChecked;
    Node* ancestor = TailPenultimateMut(c);
    ancestor->Set(NodeChecked, checked);
    ancestor->Set(NodeHasChecked, true);
}

static void OnExitHeadingAtxSequence(CompileContext* c) {
    Node* node = TailMut(c);
    if (NodePerKind(c->a, node) == 0) {
        NodeSetPerKind(c->a, node, (uint32_t)ExitSlice(c).Len());
    }
}

static void OnExitHeadingSetextUnderlineSequence(CompileContext* c) {
    Position position = PositionFromExitEvent(*c->events, c->index);
    uint8_t head = (uint8_t)c->bytes.s[position.start.index];
    NodeSetPerKind(c->a, TailMut(c), head == '-' ? 2 : 1);
}

static void OnExitLabelText(CompileContext* c) {
    Node* fragment = Resume(c);
    Str label = NodeToString(c->a, fragment);
    Str identifier = IdentifierFrom(c->a, ExitSlice(c).bytes);

    Reference& reference =
        c->mediaReferenceStack[c->mediaReferenceStack.len - 1];
    reference.label = label;
    reference.identifier = identifier;

    Node* node = TailMut(c);
    if (node->kind == NodeKind::Link) {

        node->lastKid = fragment->lastKid;
        fragment->lastKid = ArenaNode{};
    } else if (node->kind == NodeKind::Image) {
        Keep(c, node, NodeStrKind::Alt, label);
    }
}

static void OnExitLineEnding(CompileContext* c) {
    if (c->headingSetextTextAfter) {

        return;
    }
    if (c->hardBreakAfter) {
        c->hardBreakAfter = false;
        return;
    }
    NodeKind kind = TailMut(c)->kind;
    if (kind == NodeKind::Emphasis || kind == NodeKind::Heading ||
        kind == NodeKind::Paragraph || kind == NodeKind::Strong ||
        kind == NodeKind::Delete) {
        c->index -= 1;
        OnEnterData(c);
        c->index += 1;
        OnExitData(c);
    }
}

static void OnExitHtml(CompileContext* c) {
    Str value = NodeToString(c->a, Resume(c));
    Keep(c, TailMut(c), NodeStrKind::Value, value);
    OnExit(c);
}

static void OnExitMedia(CompileContext* c) {
    Reference reference = c->mediaReferenceStack[--c->mediaReferenceStack.len];
    OnExit(c);
    if (!reference.kindSome) {
        return;
    }
    Node* parent = TailMut(c);
    Node* node = NodeLastChild(c->a, parent);
    if (node->kind == NodeKind::FootnoteReference) {
        Keep(c, node, NodeStrKind::Identifier, reference.identifier);
        Keep(c, node, NodeStrKind::Label, reference.label);
    } else if (node->kind == NodeKind::Image) {
        node->kind = NodeKind::ImageReference;
        NodeSetRefKind(node, reference.kind);
        Keep(c, node, NodeStrKind::Identifier, reference.identifier);
        Keep(c, node, NodeStrKind::Label, reference.label);
        NodeClearStr(c->a, node, NodeStrKind::Url);
        NodeClearStr(c->a, node, NodeStrKind::Title);
    } else if (node->kind == NodeKind::Link) {
        node->kind = NodeKind::LinkReference;
        NodeSetRefKind(node, reference.kind);
        Keep(c, node, NodeStrKind::Identifier, reference.identifier);
        Keep(c, node, NodeStrKind::Label, reference.label);
        NodeClearStr(c->a, node, NodeStrKind::Url);
        NodeClearStr(c->a, node, NodeStrKind::Title);
    }
}

static void OnExitListItem(CompileContext* c) {
    Node* item = TailMut(c);
    Node* first = NodeFirstChild(c->a, item);
    if (item->Has(NodeHasChecked) && first &&
        first->kind == NodeKind::Paragraph) {
        Node* paragraph = first;
        Node* firstInParagraph = NodeFirstChild(c->a, paragraph);
        if (firstInParagraph && firstInParagraph->kind == NodeKind::Text) {
            Node* text = firstInParagraph;
            Str value = Get(c, text, NodeStrKind::Value);
            int32_t start = 0;
            if (StrStartsWithAny(value, "\t ")) {
                start += 1;
            } else if (StrStartsWithAny(value, "\r\n")) {
                start += 1;
                if (value.len > 1 && value.s[0] == '\r' && value.s[1] == '\n') {
                    start += 1;
                }
            }
            if (start == value.len) {

                Node* last = NodeLastChild(c->a, paragraph);
                if (last == text) {
                    paragraph->lastKid = ArenaNode{};
                } else {
                    last->sibling = text->sibling;
                }
            } else {
                Keep(c, text, NodeStrKind::Value,
                     Str(value.s + start, value.len - start));
            }
        }
    }
    OnExit(c);
}

static void OnExitListItemValue(CompileContext* c) {
    Str value = ExitSlice(c).bytes;
    uint32_t start = 0;
    for (int32_t i = 0; i < value.len; i++) {
        start = start * 10 + (uint32_t)(value.s[i] - '0');
    }
    Node* node = TailPenultimateMut(c);
    if (!node->Has(NodeHasStart)) {
        NodeSetPerKind(c->a, node, start);
        node->Set(NodeHasStart, true);
    }
}

static void OnExitReferenceString(CompileContext* c) {
    Str label = NodeToString(c->a, Resume(c));
    Str identifier = IdentifierFrom(c->a, ExitSlice(c).bytes);
    Reference& reference =
        c->mediaReferenceStack[c->mediaReferenceStack.len - 1];
    reference.kind = ReferenceKind::Full;
    reference.kindSome = true;
    reference.label = label;
    reference.identifier = identifier;
}

static void Exit(CompileContext* c) {
    switch ((*c->events)[c->index].name) {
        case Name::Autolink:
        case Name::BlockQuote:
        case Name::CharacterReference:
        case Name::Definition:
        case Name::Emphasis:
        case Name::GfmFootnoteDefinition:
        case Name::GfmStrikethrough:
        case Name::GfmTableRow:
        case Name::GfmTableCell:
        case Name::HeadingAtx:
        case Name::ListOrdered:
        case Name::ListUnordered:
        case Name::Paragraph:
        case Name::Strong:
        case Name::ThematicBreak:
            OnExit(c);
            break;

        case Name::CharacterEscapeValue:
        case Name::CodeFlowChunk:
        case Name::CodeTextData:
        case Name::Data:
        case Name::FrontmatterChunk:
        case Name::HtmlFlowData:
        case Name::HtmlTextData:
        case Name::MathFlowChunk:
        case Name::MathTextData:
            OnExitData(c);
            break;

        case Name::AutolinkProtocol:
            OnExitAutolinkProtocol(c);
            break;
        case Name::AutolinkEmail:
            OnExitAutolinkEmail(c);
            break;
        case Name::CharacterReferenceMarker:
            c->characterReferenceMarker = '&';
            break;
        case Name::CharacterReferenceMarkerNumeric:
            c->characterReferenceMarker = '#';
            break;
        case Name::CharacterReferenceMarkerHexadecimal:
            c->characterReferenceMarker = 'x';
            break;
        case Name::CharacterReferenceValue:
            OnExitCharacterReferenceValue(c);
            break;
        case Name::CodeFencedFenceInfo: {

            Str s = NodeToString(c->a, Resume(c));
            Keep(c, TailMut(c), NodeStrKind::Lang, s);
        } break;
        case Name::CodeFencedFenceMeta:
        case Name::MathFlowFenceMeta: {

            Str s = NodeToString(c->a, Resume(c));
            Keep(c, TailMut(c), NodeStrKind::Meta, s);
        } break;
        case Name::CodeFencedFence:
        case Name::MathFlowFence:
            OnExitRawFlowFence(c);
            break;
        case Name::CodeFenced:
        case Name::MathFlow:
            OnExitRawFlow(c);
            break;
        case Name::CodeIndented:
            OnExitCodeIndented(c);
            break;
        case Name::CodeText:
        case Name::MathText:
            OnExitRawText(c);
            break;
        case Name::DefinitionDestinationString: {

            Str s = NodeToString(c->a, Resume(c));
            Keep(c, TailMut(c), NodeStrKind::Url, s);
        } break;
        case Name::DefinitionLabelString:
        case Name::GfmFootnoteDefinitionLabelString:
            OnExitDefinitionId(c);
            break;
        case Name::DefinitionTitleString: {

            Str s = NodeToString(c->a, Resume(c));
            Keep(c, TailMut(c), NodeStrKind::Title, s);
        } break;
        case Name::Frontmatter: {

            Str s = TrimEol(NodeToString(c->a, Resume(c)), true, true);
            Keep(c, TailMut(c), NodeStrKind::Value, s);
            OnExit(c);
            break;
        }
        case Name::GfmAutolinkLiteralEmail:
        case Name::GfmAutolinkLiteralMailto:
        case Name::GfmAutolinkLiteralProtocol:
        case Name::GfmAutolinkLiteralWww:
        case Name::GfmAutolinkLiteralXmpp:
            OnExitGfmAutolinkLiteral(c);
            break;
        case Name::GfmFootnoteCall:
        case Name::Image:
        case Name::Link:
            OnExitMedia(c);
            break;
        case Name::GfmTable:
            OnExit(c);
            c->gfmTableInside = false;
            break;
        case Name::GfmTaskListItemValueUnchecked:
        case Name::GfmTaskListItemValueChecked:
            OnExitGfmTaskListItemValue(c);
            break;
        case Name::HardBreakEscape:
        case Name::HardBreakTrailing:
            OnExit(c);
            c->hardBreakAfter = true;
            break;
        case Name::HeadingAtxSequence:
            OnExitHeadingAtxSequence(c);
            break;
        case Name::HeadingSetext:
            c->headingSetextTextAfter = false;
            OnExit(c);
            break;
        case Name::HeadingSetextUnderlineSequence:
            OnExitHeadingSetextUnderlineSequence(c);
            break;
        case Name::HeadingSetextText:
            c->headingSetextTextAfter = true;
            break;
        case Name::HtmlFlow:
        case Name::HtmlText:
            OnExitHtml(c);
            break;
        case Name::LabelText:
            OnExitLabelText(c);
            break;
        case Name::LineEnding:
            OnExitLineEnding(c);
            break;
        case Name::ListItem:
            OnExitListItem(c);
            break;
        case Name::ListItemValue:
            OnExitListItemValue(c);
            break;
        case Name::ReferenceString:
            OnExitReferenceString(c);
            break;
        case Name::ResourceDestinationString: {

            Str s = NodeToString(c->a, Resume(c));
            Keep(c, TailMut(c), NodeStrKind::Url, s);
        } break;
        case Name::ResourceTitleString: {

            Str s = NodeToString(c->a, Resume(c));
            Keep(c, TailMut(c), NodeStrKind::Title, s);
        } break;
        default:
            break;
    }
}

Node* ToMdastCompile(const Vec<Event>& events, ParseState* parseState) {
    CompileContext context;
    context.a = parseState->a;
    context.events = &events;
    context.bytes = parseState->bytes;

    TreeFrame frame;
    frame.tree = NodeNew(context.a, NodeKind::Root);
    VecAppend(context.trees, frame);

    int32_t index = 0;
    while (index < events.len) {
        context.index = index;
        if (events[index].kind == Kind::Enter) {
            Enter(&context);
        } else {
            Exit(&context);
        }
        index += 1;
    }

    return context.trees[0].tree;
}

}

#line 1 "src/markdown/tokenizer.cpp"

namespace markdown {

enum class ByteActionKind : uint8_t {
    Normal,
    Ignore,
    Insert,
};

struct ByteAction {
    ByteActionKind kind = ByteActionKind::Normal;
    uint8_t byte = 0;
};

static ByteAction ByteActionAt(Str bytes, const Point& point) {
    uint8_t byte = (uint8_t)bytes.s[point.index];
    if (byte == '\r') {

        if (point.index < bytes.len - 1 && bytes.s[point.index + 1] == '\n') {
            return ByteAction{ByteActionKind::Ignore, 0};
        }
        return ByteAction{ByteActionKind::Normal, '\n'};
    }
    if (byte == '\t') {
        int32_t remainder = point.column % kTabSize;
        int32_t vs = remainder == 0 ? 0 : kTabSize - remainder;
        if (point.vs == 0) {
            if (vs == 0) {
                return ByteAction{ByteActionKind::Normal, byte};
            }
            return ByteAction{ByteActionKind::Insert, byte};
        }
        if (vs == 0) {
            return ByteAction{ByteActionKind::Normal, ' '};
        }
        return ByteAction{ByteActionKind::Insert, ' '};
    }
    return ByteAction{ByteActionKind::Normal, byte};
}

Point PointShiftTo(const Point& from, Str bytes, int32_t index) {
    Point next = from;
    while (next.index < index) {
        if (bytes.s[next.index] == '\t') {
            int32_t remainder = next.column % kTabSize;
            int32_t vs = remainder == 0 ? 0 : kTabSize - remainder;
            next.index += 1;
            next.column += 1 + vs;
        } else {
            next.index += 1;
            next.column += 1;
        }
    }
    return next;
}

bool IsVoidEvent(Name name) {
    switch (name) {
        case Name::AttentionSequence:
        case Name::AutolinkEmail:
        case Name::AutolinkMarker:
        case Name::AutolinkProtocol:
        case Name::BlankLineEnding:
        case Name::BlockQuoteMarker:
        case Name::ByteOrderMark:
        case Name::CharacterEscapeMarker:
        case Name::CharacterEscapeValue:
        case Name::CharacterReferenceMarker:
        case Name::CharacterReferenceMarkerHexadecimal:
        case Name::CharacterReferenceMarkerNumeric:
        case Name::CharacterReferenceMarkerSemi:
        case Name::CharacterReferenceValue:
        case Name::CodeFencedFenceSequence:
        case Name::CodeFlowChunk:
        case Name::CodeTextData:
        case Name::CodeTextSequence:
        case Name::Data:
        case Name::DefinitionDestinationLiteralMarker:
        case Name::DefinitionLabelMarker:
        case Name::DefinitionMarker:
        case Name::DefinitionTitleMarker:
        case Name::EmphasisSequence:
        case Name::FrontmatterChunk:
        case Name::GfmAutolinkLiteralEmail:
        case Name::GfmAutolinkLiteralProtocol:
        case Name::GfmAutolinkLiteralWww:
        case Name::GfmFootnoteCallMarker:
        case Name::GfmFootnoteDefinitionLabelMarker:
        case Name::GfmFootnoteDefinitionMarker:
        case Name::GfmStrikethroughSequence:
        case Name::GfmTableCellDivider:
        case Name::GfmTableDelimiterMarker:
        case Name::GfmTableDelimiterFiller:
        case Name::GfmTaskListItemMarker:
        case Name::GfmTaskListItemValueChecked:
        case Name::GfmTaskListItemValueUnchecked:
        case Name::FrontmatterSequence:
        case Name::HardBreakEscape:
        case Name::HardBreakTrailing:
        case Name::HeadingAtxSequence:
        case Name::HeadingSetextUnderlineSequence:
        case Name::HtmlFlowData:
        case Name::HtmlTextData:
        case Name::LabelImageMarker:
        case Name::LabelMarker:
        case Name::LineEnding:
        case Name::ListItemMarker:
        case Name::ListItemValue:
        case Name::MathFlowFenceSequence:
        case Name::MathFlowChunk:
        case Name::MathTextData:
        case Name::MathTextSequence:
        case Name::ReferenceMarker:
        case Name::ResourceMarker:
        case Name::ResourceTitleMarker:
        case Name::SpaceOrTab:
        case Name::StrongSequence:
        case Name::ThematicBreakSequence:
            return true;
        default:
            return false;
    }
}

void SubresultAppend(Subresult& dst, Subresult& src) {
    for (int32_t i = 0; i < src.gfmFootnoteDefinitions.len; i++) {
        VecAppend(dst.gfmFootnoteDefinitions, src.gfmFootnoteDefinitions[i]);
    }
    for (int32_t i = 0; i < src.definitions.len; i++) {
        VecAppend(dst.definitions, src.definitions[i]);
    }
    src.gfmFootnoteDefinitions.len = 0;
    src.definitions.len = 0;
}

Tokenizer* TokenizerNew(Point point, ParseState* parseState) {
    Tokenizer* t = new Tokenizer();
    t->firstLine = point.line;
    t->lineStart = point;
    t->point = point;
    t->parseState = parseState;
    t->map.a = parseState->scratch;
    return t;
}

void TokenizerFree(Tokenizer* t) {
    if (!t) {
        return;
    }
    TokenizerFree(t->tokenizeState.documentChild);
    delete t;
}

void RegisterResolver(Tokenizer* t, ResolveName name) {
    for (int32_t i = 0; i < t->resolvers.len; i++) {
        if (t->resolvers[i] == name) {
            return;
        }
    }
    VecAppend(t->resolvers, name);
}

void RegisterResolverBefore(Tokenizer* t, ResolveName name) {
    for (int32_t i = 0; i < t->resolvers.len; i++) {
        if (t->resolvers[i] == name) {
            return;
        }
    }
    VecInsertAt(t->resolvers, 0, name);
}

static void MoveOne(Tokenizer* t);

static void MoveTo(Tokenizer* t, int32_t toIndex, int32_t toVs) {
    while (t->point.index < toIndex ||
           (t->point.index == toIndex && t->point.vs < toVs)) {
        MoveOne(t);
    }
}

static void AccountForPotentialSkip(Tokenizer* t) {
    int32_t at = t->point.line - t->firstLine;
    if (t->point.column == 1 && at != t->columnStart.len) {
        MoveTo(t, t->columnStart[at].index, t->columnStart[at].vs);
    }
}

static void MovePointBack(Tokenizer* t, Point* point) {
    while (point->index > 0) {
        point->index -= 1;
        ByteAction action = ByteActionAt(t->parseState->bytes, *point);
        if (action.kind != ByteActionKind::Ignore) {
            point->index += 1;
            break;
        }
    }
}

void DefineSkip(Tokenizer* t, Point point) {
    MovePointBack(t, &point);
    IndexVs info = {point.index, point.vs};
    int32_t at = point.line - t->firstLine;
    if (at >= t->columnStart.len) {
        VecAppend(t->columnStart, info);
    } else {
        t->columnStart[at] = info;
    }
    AccountForPotentialSkip(t);
}

static void MoveOne(Tokenizer* t) {
    ByteAction action = ByteActionAt(t->parseState->bytes, t->point);
    if (action.kind == ByteActionKind::Ignore) {
        t->point.index += 1;
        return;
    }
    if (action.kind == ByteActionKind::Insert) {
        t->previous = action.byte;
        t->point.column += 1;
        t->point.vs += 1;
        return;
    }
    t->previous = action.byte;
    t->point.vs = 0;
    t->point.index += 1;
    if (action.byte == '\n') {
        t->point.line += 1;
        t->point.column = 1;
        if (t->point.line - t->firstLine + 1 > t->columnStart.len) {
            IndexVs info = {t->point.index, t->point.vs};
            VecAppend(t->columnStart, info);
        }
        t->lineStart = t->point;
        AccountForPotentialSkip(t);
    } else {
        t->point.column += 1;
    }
}

static void Expect(Tokenizer* t, int32_t byte) {
    t->consumed = false;
    t->current = byte;
}

void Consume(Tokenizer* t) {
    MoveOne(t);
    t->previous = t->current;
    t->current = -1;
    t->consumed = true;
}

static void EnterImpl(Tokenizer* t, Name name, bool hasLink, Link link) {
    Point point = t->point;
    MovePointBack(t, &point);
    VecAppend(t->stack, name);
    Event event;
    event.kind = Kind::Enter;
    event.name = name;
    event.point = point;
    event.hasLink = hasLink;
    event.link = link;
    VecAppend(t->events, event);
}

void Enter(Tokenizer* t, Name name) {
    EnterImpl(t, name, false, Link{});
}

void EnterLink(Tokenizer* t, Name name, Link link) {
    EnterImpl(t, name, true, link);
}

void Exit(Tokenizer* t, Name name) {
    t->stack.len -= 1;
    Point point = t->point;
    if (t->previous == '\n') {
        point = t->lineStart;
    } else {
        MovePointBack(t, &point);
    }
    Event event;
    event.kind = Kind::Exit;
    event.name = name;
    event.point = point;
    VecAppend(t->events, event);
}

static Progress Capture(Tokenizer* t) {
    Progress p;
    p.previous = t->previous;
    p.current = t->current;
    p.point = t->point;
    p.eventsLen = t->events.len;
    p.stackLen = t->stack.len;
    return p;
}

static void FreeProgress(Tokenizer* t, const Progress& previous) {
    t->previous = previous.previous;
    t->current = previous.current;
    t->point = previous.point;
    t->events.len = previous.eventsLen;
    t->stack.len = previous.stackLen;
}

void TokenizerCheck(Tokenizer* t, State ok, State nok) {
    Attempt attempt;
    attempt.check = true;
    attempt.hasProgress = true;
    attempt.progress = Capture(t);
    attempt.ok = ok;
    attempt.nok = nok;
    VecAppend(t->attempts, attempt);
}

void TokenizerAttempt(Tokenizer* t, State ok, State nok) {
    Attempt attempt;
    attempt.check = false;
    attempt.hasProgress = nok != StateNok();
    if (attempt.hasProgress) {
        attempt.progress = Capture(t);
    }
    attempt.ok = ok;
    attempt.nok = nok;
    VecAppend(t->attempts, attempt);
}

static State PushImpl(Tokenizer* t, int32_t fromIndex, int32_t fromVs,
                      int32_t toIndex, int32_t toVs, State state, bool flush) {
    MoveTo(t, fromIndex, fromVs);
    for (;;) {
        if (state.kind == State::Kind::Ok || state.kind == State::Kind::Nok) {
            if (t->attempts.len == 0) {
                break;
            }
            Attempt attempt = t->attempts[--t->attempts.len];
            if ((attempt.check || state.kind == State::Kind::Nok) &&
                attempt.hasProgress) {
                FreeProgress(t, attempt.progress);
            }
            t->consumed = true;
            state = state.kind == State::Kind::Ok ? attempt.ok : attempt.nok;
            continue;
        }
        if (state.kind == State::Kind::Next) {
            bool haveAction = false;
            ByteAction action = {};
            if (t->point.index < toIndex ||
                (t->point.index == toIndex && t->point.vs < toVs)) {
                action = ByteActionAt(t->parseState->bytes, t->point);
                haveAction = true;
            } else if (!flush) {
                break;
            }
            if (haveAction && action.kind == ByteActionKind::Ignore) {
                MoveOne(t);
                continue;
            }
            int32_t byte = haveAction ? (int32_t)action.byte : -1;
            StateName name = state.name;
            Expect(t, byte);
            state = Call(t, name);
            continue;
        }

        state = Call(t, state.name);
    }
    t->consumed = true;
    return state;
}

State Push(Tokenizer* t, int32_t fromIndex, int32_t fromVs, int32_t toIndex,
           int32_t toVs, State state) {
    return PushImpl(t, fromIndex, fromVs, toIndex, toVs, state, false);
}

Subresult Flush(Tokenizer* t, State state, bool resolve) {
    int32_t toIndex = t->point.index;
    int32_t toVs = t->point.vs;
    PushImpl(t, toIndex, toVs, toIndex, toVs, state, true);

    Subresult value;
    value.done = false;
    for (int32_t i = 0; i < t->tokenizeState.gfmFootnoteDefinitions.len; i++) {
        VecAppend(value.gfmFootnoteDefinitions, t->tokenizeState
                                                    .gfmFootnoteDefinitions[i]);
    }
    for (int32_t i = 0; i < t->tokenizeState.definitions.len; i++) {
        VecAppend(value.definitions, t->tokenizeState.definitions[i]);
    }
    t->tokenizeState.gfmFootnoteDefinitions.len = 0;
    t->tokenizeState.definitions.len = 0;

    if (resolve) {
        Vec<ResolveName> resolvers;
        for (int32_t i = 0; i < t->resolvers.len; i++) {
            VecAppend(resolvers, t->resolvers[i]);
        }
        t->resolvers.len = 0;
        for (int32_t index = 0; index < resolvers.len; index++) {
            Subresult result;
            if (ResolveCall(t, resolvers[index], &result)) {
                SubresultAppend(value, result);
            }
        }
        EditMapConsume(t->map, t->events);
    }
    return value;
}

}

#line 1 "src/markdown/unicode.cpp"

namespace markdown {

namespace {

struct CodePointRange {
    uint32_t first;
    uint32_t last;
};

const CodePointRange kPunctuation[349] = {
    {0x0021, 0x002F}, {0x003A, 0x0040}, {0x005B, 0x0060}, {0x007B, 0x007E},
    {0x00A1, 0x00A9}, {0x00AB, 0x00AC}, {0x00AE, 0x00B1}, {0x00B4, 0x00B4},
    {0x00B6, 0x00B8}, {0x00BB, 0x00BB}, {0x00BF, 0x00BF}, {0x00D7, 0x00D7},
    {0x00F7, 0x00F7}, {0x02C2, 0x02C5}, {0x02D2, 0x02DF}, {0x02E5, 0x02EB},
    {0x02ED, 0x02ED}, {0x02EF, 0x02FF}, {0x0375, 0x0375}, {0x037E, 0x037E},
    {0x0384, 0x0385}, {0x0387, 0x0387}, {0x03F6, 0x03F6}, {0x0482, 0x0482},
    {0x055A, 0x055F}, {0x0589, 0x058A}, {0x058D, 0x058F}, {0x05BE, 0x05BE},
    {0x05C0, 0x05C0}, {0x05C3, 0x05C3}, {0x05C6, 0x05C6}, {0x05F3, 0x05F4},
    {0x0606, 0x060F}, {0x061B, 0x061B}, {0x061D, 0x061F}, {0x066A, 0x066D},
    {0x06D4, 0x06D4}, {0x06DE, 0x06DE}, {0x06E9, 0x06E9}, {0x06FD, 0x06FE},
    {0x0700, 0x070D}, {0x07F6, 0x07F9}, {0x07FE, 0x07FF}, {0x0830, 0x083E},
    {0x085E, 0x085E}, {0x0888, 0x0888}, {0x0964, 0x0965}, {0x0970, 0x0970},
    {0x09F2, 0x09F3}, {0x09FA, 0x09FB}, {0x09FD, 0x09FD}, {0x0A76, 0x0A76},
    {0x0AF0, 0x0AF1}, {0x0B70, 0x0B70}, {0x0BF3, 0x0BFA}, {0x0C77, 0x0C77},
    {0x0C7F, 0x0C7F}, {0x0C84, 0x0C84}, {0x0D4F, 0x0D4F}, {0x0D79, 0x0D79},
    {0x0DF4, 0x0DF4}, {0x0E3F, 0x0E3F}, {0x0E4F, 0x0E4F}, {0x0E5A, 0x0E5B},
    {0x0F01, 0x0F17}, {0x0F1A, 0x0F1F}, {0x0F34, 0x0F34}, {0x0F36, 0x0F36},
    {0x0F38, 0x0F38}, {0x0F3A, 0x0F3D}, {0x0F85, 0x0F85}, {0x0FBE, 0x0FC5},
    {0x0FC7, 0x0FCC}, {0x0FCE, 0x0FDA}, {0x104A, 0x104F}, {0x109E, 0x109F},
    {0x10FB, 0x10FB}, {0x1360, 0x1368}, {0x1390, 0x1399}, {0x1400, 0x1400},
    {0x166D, 0x166E}, {0x169B, 0x169C}, {0x16EB, 0x16ED}, {0x1735, 0x1736},
    {0x17D4, 0x17D6}, {0x17D8, 0x17DB}, {0x1800, 0x180A}, {0x1940, 0x1940},
    {0x1944, 0x1945}, {0x19DE, 0x19FF}, {0x1A1E, 0x1A1F}, {0x1AA0, 0x1AA6},
    {0x1AA8, 0x1AAD}, {0x1B4E, 0x1B4F}, {0x1B5A, 0x1B6A}, {0x1B74, 0x1B7F},
    {0x1BFC, 0x1BFF}, {0x1C3B, 0x1C3F}, {0x1C7E, 0x1C7F}, {0x1CC0, 0x1CC7},
    {0x1CD3, 0x1CD3}, {0x1FBD, 0x1FBD}, {0x1FBF, 0x1FC1}, {0x1FCD, 0x1FCF},
    {0x1FDD, 0x1FDF}, {0x1FED, 0x1FEF}, {0x1FFD, 0x1FFE}, {0x2010, 0x2027},
    {0x2030, 0x205E}, {0x207A, 0x207E}, {0x208A, 0x208E}, {0x20A0, 0x20C0},
    {0x2100, 0x2101}, {0x2103, 0x2106}, {0x2108, 0x2109}, {0x2114, 0x2114},
    {0x2116, 0x2118}, {0x211E, 0x2123}, {0x2125, 0x2125}, {0x2127, 0x2127},
    {0x2129, 0x2129}, {0x212E, 0x212E}, {0x213A, 0x213B}, {0x2140, 0x2144},
    {0x214A, 0x214D}, {0x214F, 0x214F}, {0x218A, 0x218B}, {0x2190, 0x2429},
    {0x2440, 0x244A}, {0x249C, 0x24E9}, {0x2500, 0x2775}, {0x2794, 0x2B73},
    {0x2B76, 0x2B95}, {0x2B97, 0x2BFF}, {0x2CE5, 0x2CEA}, {0x2CF9, 0x2CFC},
    {0x2CFE, 0x2CFF}, {0x2D70, 0x2D70}, {0x2E00, 0x2E2E}, {0x2E30, 0x2E5D},
    {0x2E80, 0x2E99}, {0x2E9B, 0x2EF3}, {0x2F00, 0x2FD5}, {0x2FF0, 0x2FFF},
    {0x3001, 0x3004}, {0x3008, 0x3020}, {0x3030, 0x3030}, {0x3036, 0x3037},
    {0x303D, 0x303F}, {0x309B, 0x309C}, {0x30A0, 0x30A0}, {0x30FB, 0x30FB},
    {0x3190, 0x3191}, {0x3196, 0x319F}, {0x31C0, 0x31E5}, {0x31EF, 0x31EF},
    {0x3200, 0x321E}, {0x322A, 0x3247}, {0x3250, 0x3250}, {0x3260, 0x327F},
    {0x328A, 0x32B0}, {0x32C0, 0x33FF}, {0x4DC0, 0x4DFF}, {0xA490, 0xA4C6},
    {0xA4FE, 0xA4FF}, {0xA60D, 0xA60F}, {0xA673, 0xA673}, {0xA67E, 0xA67E},
    {0xA6F2, 0xA6F7}, {0xA700, 0xA716}, {0xA720, 0xA721}, {0xA789, 0xA78A},
    {0xA828, 0xA82B}, {0xA836, 0xA839}, {0xA874, 0xA877}, {0xA8CE, 0xA8CF},
    {0xA8F8, 0xA8FA}, {0xA8FC, 0xA8FC}, {0xA92E, 0xA92F}, {0xA95F, 0xA95F},
    {0xA9C1, 0xA9CD}, {0xA9DE, 0xA9DF}, {0xAA5C, 0xAA5F}, {0xAA77, 0xAA79},
    {0xAADE, 0xAADF}, {0xAAF0, 0xAAF1}, {0xAB5B, 0xAB5B}, {0xAB6A, 0xAB6B},
    {0xABEB, 0xABEB}, {0xFB29, 0xFB29}, {0xFBB2, 0xFBC2}, {0xFD3E, 0xFD4F},
    {0xFDCF, 0xFDCF}, {0xFDFC, 0xFDFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE52},
    {0xFE54, 0xFE66}, {0xFE68, 0xFE6B}, {0xFF01, 0xFF0F}, {0xFF1A, 0xFF20},
    {0xFF3B, 0xFF40}, {0xFF5B, 0xFF65}, {0xFFE0, 0xFFE6}, {0xFFE8, 0xFFEE},
    {0xFFFC, 0xFFFD}, {0x10100, 0x10102}, {0x10137, 0x1013F}, {0x10179, 0x10189},
    {0x1018C, 0x1018E}, {0x10190, 0x1019C}, {0x101A0, 0x101A0}, {0x101D0, 0x101FC},
    {0x1039F, 0x1039F}, {0x103D0, 0x103D0}, {0x1056F, 0x1056F}, {0x10857, 0x10857},
    {0x10877, 0x10878}, {0x1091F, 0x1091F}, {0x1093F, 0x1093F}, {0x10A50, 0x10A58},
    {0x10A7F, 0x10A7F}, {0x10AC8, 0x10AC8}, {0x10AF0, 0x10AF6}, {0x10B39, 0x10B3F},
    {0x10B99, 0x10B9C}, {0x10D6E, 0x10D6E}, {0x10D8E, 0x10D8F}, {0x10EAD, 0x10EAD},
    {0x10F55, 0x10F59}, {0x10F86, 0x10F89}, {0x11047, 0x1104D}, {0x110BB, 0x110BC},
    {0x110BE, 0x110C1}, {0x11140, 0x11143}, {0x11174, 0x11175}, {0x111C5, 0x111C8},
    {0x111CD, 0x111CD}, {0x111DB, 0x111DB}, {0x111DD, 0x111DF}, {0x11238, 0x1123D},
    {0x112A9, 0x112A9}, {0x113D4, 0x113D5}, {0x113D7, 0x113D8}, {0x1144B, 0x1144F},
    {0x1145A, 0x1145B}, {0x1145D, 0x1145D}, {0x114C6, 0x114C6}, {0x115C1, 0x115D7},
    {0x11641, 0x11643}, {0x11660, 0x1166C}, {0x116B9, 0x116B9}, {0x1173C, 0x1173F},
    {0x1183B, 0x1183B}, {0x11944, 0x11946}, {0x119E2, 0x119E2}, {0x11A3F, 0x11A46},
    {0x11A9A, 0x11A9C}, {0x11A9E, 0x11AA2}, {0x11B00, 0x11B09}, {0x11BE1, 0x11BE1},
    {0x11C41, 0x11C45}, {0x11C70, 0x11C71}, {0x11EF7, 0x11EF8}, {0x11F43, 0x11F4F},
    {0x11FD5, 0x11FF1}, {0x11FFF, 0x11FFF}, {0x12470, 0x12474}, {0x12FF1, 0x12FF2},
    {0x16A6E, 0x16A6F}, {0x16AF5, 0x16AF5}, {0x16B37, 0x16B3F}, {0x16B44, 0x16B45},
    {0x16D6D, 0x16D6F}, {0x16E97, 0x16E9A}, {0x16FE2, 0x16FE2}, {0x1BC9C, 0x1BC9C},
    {0x1BC9F, 0x1BC9F}, {0x1CC00, 0x1CCEF}, {0x1CD00, 0x1CEB3}, {0x1CF50, 0x1CFC3},
    {0x1D000, 0x1D0F5}, {0x1D100, 0x1D126}, {0x1D129, 0x1D164}, {0x1D16A, 0x1D16C},
    {0x1D183, 0x1D184}, {0x1D18C, 0x1D1A9}, {0x1D1AE, 0x1D1EA}, {0x1D200, 0x1D241},
    {0x1D245, 0x1D245}, {0x1D300, 0x1D356}, {0x1D6C1, 0x1D6C1}, {0x1D6DB, 0x1D6DB},
    {0x1D6FB, 0x1D6FB}, {0x1D715, 0x1D715}, {0x1D735, 0x1D735}, {0x1D74F, 0x1D74F},
    {0x1D76F, 0x1D76F}, {0x1D789, 0x1D789}, {0x1D7A9, 0x1D7A9}, {0x1D7C3, 0x1D7C3},
    {0x1D800, 0x1D9FF}, {0x1DA37, 0x1DA3A}, {0x1DA6D, 0x1DA74}, {0x1DA76, 0x1DA83},
    {0x1DA85, 0x1DA8B}, {0x1E14F, 0x1E14F}, {0x1E2FF, 0x1E2FF}, {0x1E5FF, 0x1E5FF},
    {0x1E95E, 0x1E95F}, {0x1ECAC, 0x1ECAC}, {0x1ECB0, 0x1ECB0}, {0x1ED2E, 0x1ED2E},
    {0x1EEF0, 0x1EEF1}, {0x1F000, 0x1F02B}, {0x1F030, 0x1F093}, {0x1F0A0, 0x1F0AE},
    {0x1F0B1, 0x1F0BF}, {0x1F0C1, 0x1F0CF}, {0x1F0D1, 0x1F0F5}, {0x1F10D, 0x1F1AD},
    {0x1F1E6, 0x1F202}, {0x1F210, 0x1F23B}, {0x1F240, 0x1F248}, {0x1F250, 0x1F251},
    {0x1F260, 0x1F265}, {0x1F300, 0x1F6D7}, {0x1F6DC, 0x1F6EC}, {0x1F6F0, 0x1F6FC},
    {0x1F700, 0x1F776}, {0x1F77B, 0x1F7D9}, {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0},
    {0x1F800, 0x1F80B}, {0x1F810, 0x1F847}, {0x1F850, 0x1F859}, {0x1F860, 0x1F887},
    {0x1F890, 0x1F8AD}, {0x1F8B0, 0x1F8BB}, {0x1F8C0, 0x1F8C1}, {0x1F900, 0x1FA53},
    {0x1FA60, 0x1FA6D}, {0x1FA70, 0x1FA7C}, {0x1FA80, 0x1FA89}, {0x1FA8F, 0x1FAC6},
    {0x1FACE, 0x1FADC}, {0x1FADF, 0x1FAE9}, {0x1FAF0, 0x1FAF8}, {0x1FB00, 0x1FB92},
    {0x1FB94, 0x1FBEF},
};

}

bool IsUnicodePunctuation(uint32_t cp) {
    int lo = 0;
    int hi = (int)(sizeof(kPunctuation) / sizeof(kPunctuation[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < kPunctuation[mid].first) {
            hi = mid - 1;
        } else if (cp > kPunctuation[mid].last) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

}

#line 1 "src/markdown/util.cpp"

namespace markdown {

using base::Alloc;

Str StrOwn(Arena* a, const char* s, int32_t len) {
    char* out = (char*)Alloc(a, len + 1);
    if (!out) {
        return {};
    }
    if (len > 0) {
        memcpy(out, s, (size_t)len);
    }
    out[len] = 0;
    return Str(out, len);
}

Str StrOwn(Arena* a, Str s) {
    return StrOwn(a, s.s, s.len);
}

int32_t Utf8Encode(char* out, uint32_t cp) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
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

static int32_t Utf8Len(uint8_t b) {
    if (b < 0x80) {
        return 1;
    }
    if ((b & 0xe0) == 0xc0) {
        return 2;
    }
    if ((b & 0xf0) == 0xe0) {
        return 3;
    }
    if ((b & 0xf8) == 0xf0) {
        return 4;
    }
    return 1;
}

static int32_t Utf8Decode(Str bytes, int32_t index) {
    uint8_t b = (uint8_t)bytes.s[index];
    int32_t len = Utf8Len(b);
    if (len == 1) {
        return b < 0x80 ? (int32_t)b : 0xfffd;
    }
    if (index + len > bytes.len) {
        return 0xfffd;
    }
    uint32_t cp = (uint32_t)(b & (0xff >> (len + 1)));
    for (int32_t i = 1; i < len; i++) {
        uint8_t c = (uint8_t)bytes.s[index + i];
        if ((c & 0xc0) != 0x80) {
            return 0xfffd;
        }
        cp = (cp << 6) | (uint32_t)(c & 0x3f);
    }
    return (int32_t)cp;
}

int32_t CharAfterIndex(Str bytes, int32_t index) {
    if (index >= bytes.len) {
        return -1;
    }
    return Utf8Decode(bytes, index);
}

int32_t CharBeforeIndex(Str bytes, int32_t index) {
    if (index <= 0) {
        return -1;
    }

    int32_t start = index - 1;
    int32_t limit = index < 4 ? 0 : index - 4;
    while (start > limit && ((uint8_t)bytes.s[start] & 0xc0) == 0x80) {
        start--;
    }
    return Utf8Decode(bytes, start);
}

static bool IsUnicodeWhitespace(uint32_t cp) {
    if (cp <= 0x20) {
        return cp == 0x20 || (cp >= 0x09 && cp <= 0x0d);
    }
    if (cp == 0x85 || cp == 0xa0 || cp == 0x1680) {
        return true;
    }
    if (cp >= 0x2000 && cp <= 0x200a) {
        return true;
    }
    return cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f ||
           cp == 0x3000;
}

CharKind Classify(int32_t cp) {
    if (cp < 0) {
        return CharKind::Whitespace;
    }
    if (IsUnicodeWhitespace((uint32_t)cp)) {
        return CharKind::Whitespace;
    }
    if ((cp < 0x80 && IsAsciiPunctuation((uint8_t)cp)) ||
        IsUnicodePunctuation((uint32_t)cp)) {
        return CharKind::Punctuation;
    }
    return CharKind::Other;
}

CharKind KindAfterIndex(Str bytes, int32_t index) {
    if (index == bytes.len) {
        return CharKind::Whitespace;
    }
    uint8_t byte = (uint8_t)bytes.s[index];
    if (IsAsciiWhitespace(byte)) {
        return CharKind::Whitespace;
    }
    if (IsAsciiPunctuation(byte)) {
        return CharKind::Punctuation;
    }
    if (IsAsciiAlphanumeric(byte)) {
        return CharKind::Other;
    }
    return Classify(CharAfterIndex(bytes, index));
}

Position PositionFromExitEvent(const Vec<Event>& events, int32_t index) {
    Position pos;
    pos.end = events[index].point;
    Name name = events[index].name;
    int32_t i = index - 1;
    while (!(events[i].kind == Kind::Enter && events[i].name == name)) {
        i--;
    }
    pos.start = events[i].point;
    return pos;
}

Slice SliceFromPosition(Str bytes, const Position& position) {
    int32_t before = position.start.vs;
    int32_t after = position.end.vs;
    int32_t start = position.start.index;
    int32_t end = position.end.index;
    if (before > 0) {
        before = kTabSize - before;
        start += 1;
    }
    if (after > 0) {
        after -= 1;
        end += 1;
    }
    Slice slice;
    slice.bytes = Str(bytes.s + start, end - start);
    slice.before = before;
    slice.after = after;
    return slice;
}

Slice SliceFromIndices(Str bytes, int32_t start, int32_t end) {
    Slice slice;
    slice.bytes = Str(bytes.s + start, end - start);
    return slice;
}

Str SliceSerialize(Arena* a, const Slice& slice) {
    int32_t len = slice.Len();
    char* out = (char*)Alloc(a, len + 1);
    if (!out) {
        return {};
    }
    int32_t at = 0;
    for (int32_t i = 0; i < slice.before; i++) {
        out[at++] = ' ';
    }
    if (slice.bytes.len > 0) {
        memcpy(out + at, slice.bytes.s, (size_t)slice.bytes.len);
        at += slice.bytes.len;
    }
    for (int32_t i = 0; i < slice.after; i++) {
        out[at++] = ' ';
    }
    out[at] = 0;
    return Str(out, at);
}

struct Jump {
    int32_t at;
    int32_t remove;
    int32_t add;
};

static void ShiftLinks(Vec<Event>& events, const Vec<Jump>& jumps) {
    int32_t jumpIndex = 0;
    int32_t index = 0;
    int32_t add = 0;
    int32_t rm = 0;
    while (index < events.len) {
        int32_t rmCurr = rm;
        while (jumpIndex < jumps.len && jumps[jumpIndex].at <= index) {
            add = jumps[jumpIndex].add;
            rm = jumps[jumpIndex].remove;
            jumpIndex++;
        }
        if (rm > rmCurr) {
            index += rm - rmCurr;
            continue;
        }
        if (events[index].hasLink && events[index].link.next != -1) {
            int32_t next = events[index].link.next;
            events[next].link.previous = index + add - rm;
            while (jumpIndex < jumps.len && jumps[jumpIndex].at <= next) {
                add = jumps[jumpIndex].add;
                rm = jumps[jumpIndex].remove;
                jumpIndex++;
            }
            events[index].link.next = next + add - rm;
            index = next;
            continue;
        }
        index++;
    }
}

static int32_t BucketFor(const Vec<int32_t>& buckets,
                         const Vec<EditMap::Entry>& entries, int32_t at) {
    uint32_t h = (uint32_t)at * 2654435761u;
    h ^= h >> 15;
    int32_t mask = buckets.len - 1;
    int32_t i = (int32_t)h & mask;
    while (buckets[i] != 0 && entries[buckets[i] - 1].at != at) {
        i = (i + 1) & mask;
    }
    return i;
}

static void RehashBuckets(EditMap& map, int32_t wanted) {
    VecReset(map.buckets);
    VecReserve(map.buckets, wanted);
    map.buckets.len = wanted;
    memset((void*)map.buckets.els, 0, (size_t)wanted * sizeof(int32_t));
    for (int32_t i = 0; i < map.map.len; i++) {
        map.buckets[BucketFor(map.buckets, map.map, map.map[i].at)] = i + 1;
    }
}

static void AddImpl(EditMap& map, int32_t at, int32_t remove, const Event* add,
                    int32_t addLen, bool before) {
    if (remove == 0 && addLen == 0) {
        return;
    }

    if ((map.map.len + 1) * 4 >= map.buckets.len * 3) {
        RehashBuckets(map, map.buckets.len > 0 ? map.buckets.len * 2 : 16);
    }
    int32_t bucket = BucketFor(map.buckets, map.map, at);
    if (map.buckets[bucket] != 0) {
        EditMap::Entry& e = map.map[map.buckets[bucket] - 1];
        e.remove += remove;
        if (before) {
            ArenaVec<Event> merged{};
            merged.Reserve(map.a, addLen + e.add.len);
            merged.AppendMany(map.a, add, addLen);
            for (const Event& ev : e.add) {
                merged.Append(map.a, ev);
            }
            e.add = merged;
        } else {
            e.add.AppendMany(map.a, add, addLen);
        }
        return;
    }
    EditMap::Entry e;
    e.at = at;
    e.remove = remove;
    e.add.AppendMany(map.a, add, addLen);
    VecAppend(map.map, e);
    map.buckets[bucket] = map.map.len;
}

void EditMapAdd(EditMap& map, int32_t index, int32_t remove, const Event* add,
                int32_t addLen) {
    AddImpl(map, index, remove, add, addLen, false);
}

void EditMapAddBefore(EditMap& map, int32_t index, int32_t remove,
                      const Event* add, int32_t addLen) {
    AddImpl(map, index, remove, add, addLen, true);
}

static void SortEntries(Vec<EditMap::Entry>& entries) {
    int32_t n = entries.len;
    if (n < 2) {
        return;
    }
    Vec<EditMap::Entry> scratch;
    VecReserve(scratch, n);
    scratch.len = n;
    EditMap::Entry* src = entries.els;
    EditMap::Entry* dst = scratch.els;
    for (int32_t width = 1; width < n; width *= 2) {
        for (int32_t lo = 0; lo < n; lo += width * 2) {
            int32_t mid = lo + width < n ? lo + width : n;
            int32_t hi = lo + width * 2 < n ? lo + width * 2 : n;
            int32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                dst[k++] = src[j].at < src[i].at ? src[j++] : src[i++];
            }
            while (i < mid) {
                dst[k++] = src[i++];
            }
            while (j < hi) {
                dst[k++] = src[j++];
            }
        }
        EditMap::Entry* t = src;
        src = dst;
        dst = t;
    }
    if (src != entries.els) {
        memcpy((void*)entries.els, (const void*)src,
               (size_t)n * sizeof(EditMap::Entry));
    }
}

void EditMapConsume(EditMap& map, Vec<Event>& events) {
    SortEntries(map.map);
    if (map.map.len == 0) {
        return;
    }

    Vec<Jump> jumps;
    VecReserve(jumps, map.map.len);
    int32_t addAcc = 0;
    int32_t removeAcc = 0;
    for (int32_t index = 0; index < map.map.len; index++) {
        removeAcc += map.map[index].remove;
        addAcc += map.map[index].add.len;
        Jump j = {map.map[index].at, removeAcc, addAcc};
        VecAppend(jumps, j);
    }

    ShiftLinks(events, jumps);

    Vec<Event> out;
    VecReserve(out, events.len + addAcc - removeAcc);
    int32_t index = 0;
    for (int32_t i = 0; i < map.map.len; i++) {
        const EditMap::Entry& e = map.map[i];
        for (int32_t j = index; j < e.at; j++) {
            VecAppend(out, events[j]);
        }
        for (const Event& ev : e.add) {
            VecAppend(out, ev);
        }
        index = e.at + e.remove;
    }
    for (int32_t j = index; j < events.len; j++) {
        VecAppend(out, events[j]);
    }

    VecReset(events);
    events.els = out.els;
    events.len = out.len;
    events.cap = out.cap;
    out.els = nullptr;
    out.len = 0;
    out.cap = 0;
    map.map.len = 0;

    if (map.buckets.len > 0) {
        memset((void*)map.buckets.els, 0,
               (size_t)map.buckets.len * sizeof(int32_t));
    }
}

static bool NamesContain(const Name* names, int32_t namesLen, Name name) {
    for (int32_t i = 0; i < namesLen; i++) {
        if (names[i] == name) {
            return true;
        }
    }
    return false;
}

static int32_t SkipToImpl(const Vec<Event>& events, int32_t index,
                          const Name* names, int32_t namesLen, bool forward) {
    while (index < events.len) {
        if (NamesContain(names, namesLen, events[index].name)) {
            break;
        }
        index = forward ? index + 1 : index - 1;
    }
    return index;
}

static int32_t SkipOptImpl(const Vec<Event>& events, int32_t index,
                           const Name* names, int32_t namesLen, bool forward) {
    int32_t balance = 0;
    Kind open = forward ? Kind::Enter : Kind::Exit;
    while (index < events.len) {
        Name current = events[index].name;
        if (!NamesContain(names, namesLen, current) || events[index]
                                                               .kind != open) {
            break;
        }
        index = forward ? index + 1 : index - 1;
        balance += 1;
        for (;;) {
            balance = events[index].kind == open ? balance + 1 : balance - 1;
            int32_t next =
                forward ? index + 1 : (index > 0 ? index - 1 : index);
            if (events[index].name == current && balance == 0) {
                index = next;
                break;
            }
            index = next;
        }
    }
    return index;
}

int32_t SkipOpt(const Vec<Event>& events, int32_t index, const Name* names,
                int32_t namesLen) {
    return SkipOptImpl(events, index, names, namesLen, true);
}

int32_t SkipOptBack(const Vec<Event>& events, int32_t index, const Name* names,
                    int32_t namesLen) {
    return SkipOptImpl(events, index, names, namesLen, false);
}

int32_t SkipTo(const Vec<Event>& events, int32_t index, const Name* names,
               int32_t namesLen) {
    return SkipToImpl(events, index, names, namesLen, true);
}

int32_t SkipToBack(const Vec<Event>& events, int32_t index, const Name* names,
                   int32_t namesLen) {
    return SkipToImpl(events, index, names, namesLen, false);
}

Str NormalizeIdentifier(Arena* a, Str value) {
    char* out = (char*)Alloc(a, value.len + 1);
    if (!out) {
        return {};
    }
    int32_t at = 0;
    bool inWhitespace = true;
    int32_t index = 0;
    int32_t start = 0;
    while (index < value.len) {
        char c = value.s[index];
        if (c == '\t' || c == '\n' || c == '\r' || c == ' ') {
            if (!inWhitespace) {
                memcpy(out + at, value.s + start, (size_t)(index - start));
                at += index - start;
                inWhitespace = true;
            }
        } else if (inWhitespace) {
            if (start != 0) {
                out[at++] = ' ';
            }
            start = index;
            inWhitespace = false;
        }
        index++;
    }
    if (!inWhitespace) {
        memcpy(out + at, value.s + start, (size_t)(value.len - start));
        at += value.len - start;
    }

    for (int32_t i = 0; i < at; i++) {
        if (out[i] >= 'a' && out[i] <= 'z') {
            out[i] = (char)(out[i] - 32);
        }
    }
    out[at] = 0;
    return Str(out, at);
}

bool ListLoose(const Vec<Event>& events, int32_t index, bool includeItems) {
    int32_t balance = 0;
    Name name = events[index].name;
    while (index < events.len) {
        const Event& event = events[index];
        if (event.kind == Kind::Enter) {
            balance += 1;
            if (includeItems && balance == 2 && event.name == Name::ListItem &&
                ListItemLoose(events, index)) {
                return true;
            }
        } else {
            balance -= 1;
            if (balance == 1 && event.name == Name::BlankLineEnding) {
                bool atEmptyListItem = false;
                bool atEmptyBlockQuote = false;
                int32_t before = index - 2;
                if (events[before].name == Name::ListItem) {
                    before -= 1;
                    if (events[before].name == Name::SpaceOrTab) {
                        before -= 2;
                    }
                    if (events[before].name == Name::BlockQuote &&
                        events[before - 1].name == Name::BlockQuotePrefix) {
                        atEmptyBlockQuote = true;
                    } else if (events[before].name == Name::ListItemPrefix) {
                        atEmptyListItem = true;
                    }
                }
                if (!atEmptyListItem && !atEmptyBlockQuote) {
                    return true;
                }
            }
            if (balance == 0 && event.name == name) {
                break;
            }
        }
        index++;
    }
    return false;
}

bool ListItemLoose(const Vec<Event>& events, int32_t index) {
    int32_t balance = 0;
    while (index < events.len) {
        const Event& event = events[index];
        if (event.kind == Kind::Enter) {
            balance += 1;
        } else {
            balance -= 1;
            if (balance == 1 && event.name == Name::BlankLineEnding) {
                bool atPrefix = false;
                int32_t before = index - 2;
                if (events[before].name == Name::SpaceOrTab) {
                    before -= 2;
                }
                if (events[before].name == Name::ListItemPrefix) {
                    atPrefix = true;
                }
                if (!atPrefix) {
                    return true;
                }
            }
            if (balance == 0 && event.name == Name::ListItem) {
                break;
            }
        }
        index++;
    }
    return false;
}

static int32_t ScanTableAlign(const Vec<Event>& events, int32_t index, Arena* a,
                              ArenaAlign out) {
    bool inDelimiterRow = false;
    int32_t count = 0;
    while (index < events.len) {
        const Event& event = events[index];
        if (inDelimiterRow) {
            if (event.kind == Kind::Enter) {
                if (event.name == Name::GfmTableDelimiterCellValue) {

                    AlignKind kind =
                        events[index + 1].name == Name::GfmTableDelimiterMarker
                            ? AlignKind::Left
                            : AlignKind::None;
                    if (out != kArenaAlignNone) {
                        ArenaAlignSet(a, out, count, kind);
                    }
                    count++;
                }
            } else if (event.name == Name::GfmTableDelimiterCellValue) {

                if (count > 0 &&
                    events[index - 1].name == Name::GfmTableDelimiterMarker) {
                    if (out != kArenaAlignNone) {
                        AlignKind was = ArenaAlignAt(a, out, count - 1);
                        ArenaAlignSet(a, out, count - 1,
                                      was == AlignKind::Left
                                          ? AlignKind::Center
                                          : AlignKind::Right);
                    }
                }
            } else if (event.name == Name::GfmTableDelimiterRow) {
                break;
            }
        } else if (event.kind == Kind::Enter &&
                   event.name == Name::GfmTableDelimiterRow) {
            inDelimiterRow = true;
        }
        index++;
    }
    return count;
}

ArenaAlign GfmTableAlign(const Vec<Event>& events, int32_t index, Arena* a) {
    int32_t count = ScanTableAlign(events, index, a, kArenaAlignNone);
    if (count <= 0) {
        return kArenaAlignNone;
    }
    ArenaAlign out = ArenaAlignNew(a, count);
    ScanTableAlign(events, index, a, out);
    return out;
}

int32_t CharacterReferenceValueMax(uint8_t marker) {
    if (marker == '&') {
        return kCharacterReferenceNamedSizeMax;
    }
    if (marker == 'x') {
        return kCharacterReferenceHexadecimalSizeMax;
    }
    return kCharacterReferenceDecimalSizeMax;
}

bool CharacterReferenceValueTest(uint8_t marker, uint8_t byte) {
    if (marker == '&') {
        return IsAsciiAlphanumeric(byte);
    }
    if (marker == 'x') {
        return IsAsciiHexDigit(byte);
    }
    return IsAsciiDigit(byte);
}

static const char* NamedValue(Str name) {

    int32_t lo = 0;
    int32_t hi = 2125 - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        const char* candidate =
            kCharacterReferenceNames + kCharacterReferences[mid].nameOff;
        int cmp = StrCmp(Str(candidate), name);
        if (cmp < 0) {
            lo = mid + 1;
        } else if (cmp > 0) {
            hi = mid - 1;
        } else {
            return kCharacterReferenceValues + kCharacterReferences[mid]
                                                   .valueOff;
        }
    }
    return nullptr;
}

Str DecodeNamed(Arena* a, Str name) {
    const char* value = NamedValue(name);
    return value ? StrOwn(a, value, (int32_t)strlen(value)) : Str{};
}

static uint32_t DecodeNumericCp(Str value, int radix) {
    uint32_t cp = 0;
    bool overflow = false;
    for (int32_t i = 0; i < value.len; i++) {
        uint8_t c = (uint8_t)value.s[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (uint32_t)(c - 'a' + 10);
        } else {
            digit = (uint32_t)(c - 'A' + 10);
        }
        if (cp > 0x10ffff) {
            overflow = true;
            break;
        }
        cp = cp * (uint32_t)radix + digit;
    }

    bool bad = overflow || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
               cp <= 0x08 || cp == 0x0b || (cp >= 0x0e && cp <= 0x1f) ||
               (cp >= 0x7f && cp <= 0x9f);
    return bad ? 0xfffd : cp;
}

Str DecodeNumeric(Arena* a, Str value, int radix) {
    char* out = (char*)a->Push(4, 1, false);
    int32_t n = Utf8Encode(out, DecodeNumericCp(value, radix));
    return Str(out, n);
}

base::TempStr CharacterReferenceDecodeTemp(Str value, uint8_t marker) {
    if (marker == '#' || marker == 'x') {
        base::TempStr out = base::AllocStrTemp(4);
        uint32_t cp = DecodeNumericCp(value, marker == '#' ? 10 : 16);
        out.len = Utf8Encode(out.s, cp);
        return out;
    }
    const char* found = NamedValue(value);
    return found ? Str((char*)found, (int32_t)strlen(found)) : Str{};
}

Str CharacterReferenceDecode(Arena* a, Str value, uint8_t marker) {
    if (marker == '#') {
        return DecodeNumeric(a, value, 10);
    }
    if (marker == 'x') {
        return DecodeNumeric(a, value, 16);
    }
    return DecodeNamed(a, value);
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
        if (full.len >= kMaxPath || lstat(full.s, &st) != 0) {
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
    if (s.s && s.len > 0) {
        n = MultiByteToWideChar(CP_UTF8, 0, s.s, s.len, nullptr, 0);
        if (n < 0) {
            n = 0;
        }
    }
    auto res = (WCHAR*)arena->Push((uint64_t)(n + 1) * sizeof(WCHAR),
                                   alignof(WCHAR), false);
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s.s, s.len, res, n);
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
