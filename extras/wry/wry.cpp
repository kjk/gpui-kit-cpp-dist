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

#line 1 "src/wry/wry.cpp"

namespace wry {

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

#if GPUI_OS_LINUX
#line 1 "src/wry/wry_linux.cpp"

namespace wry {

using base::logf;
using base::Str;

static void Unsupported() {
    static bool said = false;
    if (!said) {
        said = true;
        logf("wry: no webview backend on this platform\n");
    }
}

WebView* WebViewNew(void*, const WebViewAttributes*, bool) {
    Unsupported();
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

#if GPUI_OS_MAC
#line 1 "src/wry/wry_mac.cpp"

#import <Cocoa/Cocoa.h>

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
@class GpuiWryUIDelegate;
@class GpuiWryTitleObserver;
@class GpuiWrySchemeHandler;
@class GpuiWryWebView;

namespace wry {

using base::AllocStrTemp;
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

struct ProtocolCopy {
    Str name;
    void* ctx;
    void (*handler)(void* ctx, Str id, const Request* request, RequestResponder* responder);
};

struct WebView {
    Str id = {};
    WKWebView* webview = nil;
    WKUserContentController* manager = nil;
    NSView* parentView = nil;
    bool isChild = false;
    bool visible = true;

    void* ctx = nullptr;
    void (*ipcHandler)(void* ctx, Str url, Str body) = nullptr;
    bool (*navigationHandler)(void* ctx, Str url) = nullptr;
    void (*documentTitleChangedHandler)(void* ctx, Str title) = nullptr;
    void (*onPageLoadHandler)(void* ctx, PageLoadEvent event, Str url) = nullptr;
    NewWindowResponse (*newWindowReqHandler)(void* ctx, Str url,
                                             const NewWindowFeatures* features,
                                             WebView** createdWebView) = nullptr;

    Vec<ProtocolCopy> protocols;

    Vec<Str> pendingScripts;
    bool pendingOpen = true;

    NSMutableSet* liveTasks = nil;

    GpuiWryScriptHandler* ipcDelegate = nil;
    GpuiWryNavigationDelegate* navDelegate = nil;
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
    if (!s.s || s.len == 0) {
        return @"";
    }
    NSString* res = [[NSString alloc] initWithBytes:s.s
                                             length:(NSUInteger)s.len
                                           encoding:NSUTF8StringEncoding];
    return res ? res : @"";
}

static Str FromNSTemp(NSString* s) {
    if (!s) {
        return {};
    }
    const char* utf8 = [s UTF8String];
    if (!utf8) {
        return {};
    }
    int n = (int)strlen(utf8);
    Str res = AllocStrTemp(n + 1);
    if (!res.s) {
        return {};
    }
    memcpy(res.s, utf8, (size_t)n);
    res.s[n] = 0;
    res.len = n;
    return res;
}

static Str UrlFromWebView(WKWebView* webview) {
    NSURL* url = webview.URL;
    return url ? FromNSTemp(url.absoluteString) : Str();
}

static void FlushPendingScripts(WebView* wv);
static void HandleSchemeTask(WebView* wv, int index, id<WKURLSchemeTask> task);

}

@interface GpuiWryWebView : WKWebView
@property(nonatomic, assign) BOOL childWebView;
@property(nonatomic, assign) BOOL acceptFirstMouseEnabled;
@end

@implementation GpuiWryWebView
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

- (NSString*)syntheticMouseScript:(NSEvent*)event down:(BOOL)down back:(BOOL)back {
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
            (unsigned long)x, (unsigned long)y, down ? @"mousedown" : @"mouseup",
            back ? 3 : 4, (unsigned long)buttons, (unsigned long)x, (unsigned long)y,
            (long)event.clickCount, (unsigned long)x, (unsigned long)y,
            (unsigned long)x, (unsigned long)y, (unsigned long)x, (unsigned long)y,
            (unsigned long)x, (unsigned long)y,
            (mods & NSEventModifierFlagControl) ? "true" : "false",
            (mods & NSEventModifierFlagCommand) ? "true" : "false",
            (mods & NSEventModifierFlagShift) ? "true" : "false",
            (mods & NSEventModifierFlagOption) ? "true" : "false",
            down ? @"mousedown" : @"mouseup"];
}

- (void)otherMouseDown:(NSEvent*)event {
    NSInteger button = event.buttonNumber;
    if (event.type == NSEventTypeOtherMouseDown && (button == 3 || button == 4)) {
        [self evaluateJavaScript:[self syntheticMouseScript:event down:YES back:button == 3]
              completionHandler:nil];
        return;
    }
    [self mouseDown:event];
}

- (void)otherMouseUp:(NSEvent*)event {
    NSInteger button = event.buttonNumber;
    if (event.type == NSEventTypeOtherMouseUp && (button == 3 || button == 4)) {
        [self evaluateJavaScript:[self syntheticMouseScript:event down:NO back:button == 3]
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
    wv->ipcHandler(wv->ctx, url ? wry::FromNSTemp(url.absoluteString) : wry::Str(),
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
    if (!wv || !wv->documentTitleChangedHandler || ![keyPath isEqualToString:@"title"]) {
        return;
    }
    NSString* title = [object title];
    wv->documentTitleChangedHandler(wv->ctx, wry::FromNSTemp(title));
}
@end

@interface GpuiWryNavigationDelegate : NSObject <WKNavigationDelegate>
@property(nonatomic, assign) wry::WebView* wv;
@end

@implementation GpuiWryNavigationDelegate
- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
    (void)webView;
    wry::WebView* wv = self.wv;

    if ([action respondsToSelector:@selector(shouldPerformDownload)] &&
        action.shouldPerformDownload) {
        handler(WKNavigationActionPolicyCancel);
        return;
    }
    if (!wv || !wv->navigationHandler) {
        handler(WKNavigationActionPolicyAllow);
        return;
    }
    NSURL* url = action.request.URL;
    bool allow =
        wv->navigationHandler(wv->ctx, url ? wry::FromNSTemp(url.absoluteString) : wry::Str());
    handler(allow ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}

- (void)webView:(WKWebView*)webView didCommitNavigation:(WKNavigation*)navigation {
    (void)navigation;
    wry::WebView* wv = self.wv;
    if (!wv) {
        return;
    }
    if (wv->onPageLoadHandler) {
        wv->onPageLoadHandler(wv->ctx, wry::PageLoadEvent::Started, wry::UrlFromWebView(webView));
    }
    wry::FlushPendingScripts(wv);
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    (void)navigation;
    wry::WebView* wv = self.wv;
    if (wv && wv->onPageLoadHandler) {
        wv->onPageLoadHandler(wv->ctx, wry::PageLoadEvent::Finished, wry::UrlFromWebView(webView));
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
                           decisionHandler:(void (^)(WKPermissionDecision))handler {
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
        wv->ctx, url ? wry::FromNSTemp(url.absoluteString) : wry::Str(), &features, &created);
    if (response == wry::NewWindowResponse::Deny) {
        return nil;
    }
    if (response == wry::NewWindowResponse::Create) {
        return created ? created->webview : nil;
    }

    NSWindow* current = webView.window;
    NSRect defaults = current ? current.frame : NSMakeRect(0, 0, 800, 600);
    NSSize size = NSMakeSize(features.hasSize ? features.width : defaults.size.width,
                             features.hasSize ? features.height : defaults.size.height);
    NSPoint origin = defaults.origin;
    if (features.hasPosition) {
        NSScreen* screen = current ? current.screen : [NSScreen mainScreen];
        CGFloat screenHeight = screen ? screen.frame.size.height : size.height;
        origin = NSMakePoint(features.x, screenHeight - features.y - size.height);
    }

    NSWindowStyleMask mask =
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    bool resizable = windowFeatures.allowsResizing ? windowFeatures.allowsResizing.boolValue : true;
    if (resizable) {
        mask |= NSWindowStyleMaskResizable;
    }

    NSRect rect = NSMakeRect(origin.x, origin.y, size.width, size.height);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:rect
                                                   styleMask:mask
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];

    window.releasedWhenClosed = NO;

    WKWebView* child = [[WKWebView alloc] initWithFrame:window.frame configuration:configuration];
    GpuiWryNewWindowDelegate* delegate = [[GpuiWryNewWindowDelegate alloc] init];
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
- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void)webView;
    if (self.wv) {
        wry::HandleSchemeTask(self.wv, self.index, task);
    }
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void)webView;
    wry::WebView* wv = self.wv;
    if (wv && wv->liveTasks) {
        [wv->liveTasks removeObject:[NSValue valueWithPointer:(__bridge const void*)task]];
    }
}
@end

namespace wry {

static void AddUserScript(WebView* wv, Str js, bool forMainFrameOnly) {
    WKUserScript* script =
        [[WKUserScript alloc] initWithSource:ToNS(js)
                               injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                            forMainFrameOnly:forMainFrameOnly ? YES : NO];
    [wv->manager addUserScript:script];
}

static void FlushPendingScripts(WebView* wv) {
    if (!wv->pendingOpen) {
        return;
    }
    for (int i = 0; i < wv->pendingScripts.len; i++) {
        [wv->webview evaluateJavaScript:ToNS(wv->pendingScripts[i]) completionHandler:nil];
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
    [wv->liveTasks addObject:[NSValue valueWithPointer:(__bridge const void*)task]];

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
    req.headers = headerStore.len > 0 ? &headerStore[0] : nullptr;
    req.headerCount = headerStore.len;
    req.body = bodyStore.len > 0 ? &bodyStore[0] : nullptr;
    req.bodyLen = bodyStore.len;

    RequestResponder* responder = new RequestResponder();
    responder->liveTasks = wv->liveTasks;
    responder->task = task;
    responder->url = url;

    ProtocolCopy& p = wv->protocols[index];
    if (p.handler) {
        p.handler(p.ctx, wv->id, &req, responder);
    } else {
        Response response;
        response.status = 500;
        Respond(responder, &response);
    }

    VecReset(headerStore);
    VecReset(bodyStore);
}

static void DeliverResponse(RequestResponder* responder, NSHTTPURLResponse* response,
                            NSData* data) {
    NSValue* key = [NSValue valueWithPointer:(__bridge const void*)responder->task];
    if (responder->liveTasks && [responder->liveTasks containsObject:key]) {
        @try {
            [responder->task didReceiveResponse:response];
            [responder->task didReceiveData:data];
            [responder->task didFinish];
        } @catch (NSException* e) {
            (void)e;
            logf("wry: the custom protocol task went away before it was answered\n");
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
            headers[ToNS(response->headers[i].name)] = ToNS(response->headers[i].value);
        }
    }
    NSData* data = bodyLen > 0
                       ? [NSData dataWithBytes:response->body length:(NSUInteger)bodyLen]
                       : [NSData data];
    NSHTTPURLResponse* http = [[NSHTTPURLResponse alloc] initWithURL:responder->url
                                                          statusCode:status
                                                         HTTPVersion:@"HTTP/1.1"
                                                        headerFields:headers];
    if (!http) {
        logf("wry: could not build the response for a custom protocol request\n");
        if (responder->liveTasks && responder->task) {
            NSValue* key =
                [NSValue valueWithPointer:(__bridge const void*)responder->task];
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

WebView* WebViewNew(void* parentWindow, const WebViewAttributes* attrs, bool asChild) {
    if (!parentWindow || !attrs) {
        return nullptr;
    }
    if (![NSThread isMainThread]) {

        logf("wry: a webview can only be made on the main thread\n");
        return nullptr;
    }
    NSView* parentView = (__bridge NSView*)parentWindow;

    bool usingExistingConfig = attrs->webviewConfiguration != nullptr;
    WKWebViewConfiguration* config = usingExistingConfig
                                         ? (__bridge WKWebViewConfiguration*)attrs->webviewConfiguration
                                         : [[WKWebViewConfiguration alloc] init];
    if (!usingExistingConfig) {
        if (attrs->incognito) {
            config.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
        } else if (attrs->hasDataStoreIdentifier) {
            if (@available(macOS 14.0, *)) {
                NSUUID* identifier =
                    [[NSUUID alloc] initWithUUIDBytes:attrs->dataStoreIdentifier];
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
    wv->ctx = attrs->ctx;
    wv->ipcHandler = attrs->ipcHandler;
    wv->navigationHandler = attrs->navigationHandler;
    wv->documentTitleChangedHandler = attrs->documentTitleChangedHandler;
    wv->onPageLoadHandler = attrs->onPageLoadHandler;
    wv->newWindowReqHandler = attrs->newWindowReqHandler;
    wv->liveTasks = [NSMutableSet set];
    wv->schemeHandlers = [NSMutableArray array];

    static int nextId = 1;
    wv->id = attrs->id.len > 0 ? StrDup(attrs->id) : StrDup(base::FormatTemp("%d", nextId++));

    for (int i = 0; i < attrs->customProtocolCount; i++) {
        NSString* scheme = ToNS(attrs->customProtocols[i].name);
        if (usingExistingConfig && [config urlSchemeHandlerForURLScheme:scheme]) {
            continue;
        }
        ProtocolCopy p;
        p.name = StrDup(attrs->customProtocols[i].name);
        p.ctx = attrs->customProtocols[i].ctx;
        p.handler = attrs->customProtocols[i].handler;
        int protocolIndex = wv->protocols.len;
        VecAppend(wv->protocols, p);

        GpuiWrySchemeHandler* handler = [[GpuiWrySchemeHandler alloc] init];
        handler.wv = wv;
        handler.index = protocolIndex;
        [wv->schemeHandlers addObject:handler];

        @try {
            [config setURLSchemeHandler:handler forURLScheme:scheme];
        } @catch (NSException* e) {
            (void)e;
            logf("wry: could not register the custom protocol '%s'\n", p.name);
            WebViewFree(wv);
            return nullptr;
        }
    }

    WKPreferences* preferences = config.preferences;
    if (attrs->javascriptDisabled) {
        config.defaultWebpagePreferences.allowsContentJavaScript = NO;
    }
    if (attrs->autoplay) {
        config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
    }
    if (attrs->hasBackgroundThrottling) {
        if (@available(macOS 14.0, *)) {

            int policy = 2;
            if (attrs->backgroundThrottling == BackgroundThrottlingPolicy::Suspend) {
                policy = 0;
            } else if (attrs->backgroundThrottling == BackgroundThrottlingPolicy::Throttle) {
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
        double x = ToLogical(attrs->bounds.position.x, attrs->bounds.position.logical, scale);
        double y = ToLogical(attrs->bounds.position.y, attrs->bounds.position.logical, scale);
        double w = ToLogical(attrs->bounds.size.width, attrs->bounds.size.logical, scale);
        double h = ToLogical(attrs->bounds.size.height, attrs->bounds.size.logical, scale);
        frame.origin = WindowPosition(parentView, x, y, h);
        frame.size = NSMakeSize(w, h);
    } else {
        frame = parentView.bounds;
    }

    GpuiWryWebView* webview =
        [[GpuiWryWebView alloc] initWithFrame:frame configuration:config];
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
            [wv->manager addScriptMessageHandler:wv->ipcDelegate name:kIpcHandlerName];
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

    wv->navDelegate = [[GpuiWryNavigationDelegate alloc] init];
    wv->navDelegate.wv = wv;
    wv->webview.navigationDelegate = wv->navDelegate;

    wv->uiDelegate = [[GpuiWryUIDelegate alloc] init];
    wv->uiDelegate.wv = wv;
    wv->webview.UIDelegate = wv->uiDelegate;

    if (attrs->userAgent.len > 0) {
        wv->webview.customUserAgent = ToNS(attrs->userAgent);
    }

    if (attrs->url.len > 0) {
        NSURL* url = [NSURL URLWithString:ToNS(attrs->url)];
        if (url) {
            NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
            for (int i = 0; i < attrs->headerCount; i++) {
                [request addValue:ToNS(attrs->headers[i].value)
                    forHTTPHeaderField:ToNS(attrs->headers[i].name)];
            }
            [wv->webview loadRequest:request];
        } else {
            logf("wry: the url could not be parsed\n");
        }
    } else if (attrs->html.len > 0) {
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
    if (window && [window respondsToSelector:@selector(setTitlebarSeparatorStyle:)]) {
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

    wv->ipcDelegate.wv = nullptr;
    wv->navDelegate.wv = nullptr;
    wv->uiDelegate.wv = nullptr;
    wv->titleObserver.wv = nullptr;
    for (GpuiWrySchemeHandler* handler in wv->schemeHandlers) {
        handler.wv = nullptr;
    }
    [wv->liveTasks removeAllObjects];
    for (int i = 0; i < wv->protocols.len; i++) {
        StrFree(wv->protocols[i].name);
    }
    VecReset(wv->protocols);
    for (int i = 0; i < wv->pendingScripts.len; i++) {
        StrFree(wv->pendingScripts[i]);
    }
    VecReset(wv->pendingScripts);
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

                    NSData* json =
                        [NSJSONSerialization dataWithJSONObject:result
                                                        options:NSJSONWritingFragmentsAllowed
                                                          error:nil];
                    if (!json) {
                        callback(ctx, Str());
                        return;
                    }
                    NSString* text = [[NSString alloc] initWithData:json
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

bool WebViewLoadUrlWithHeaders(WebView* wv, Str url, const Header* headers, int headerCount) {
    if (!wv) {
        return false;
    }
    NSURL* nsurl = [NSURL URLWithString:ToNS(url)];
    if (!nsurl) {
        return false;
    }
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:nsurl];
    for (int i = 0; i < headerCount; i++) {
        [request addValue:ToNS(headers[i].value) forHTTPHeaderField:ToNS(headers[i].name)];
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
                                : parent.frame.size.height - frame.origin.y - frame.size.height;
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

bool WebViewSetBackgroundColor(WebView*, Rgba) {

    return true;
}

bool WebViewSetTheme(WebView*, Theme) {
    return false;
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
    if (![wv->webview respondsToSelector:@selector(printOperationWithPrintInfo:)]) {
        return false;
    }
    NSWindow* window = wv->webview.window;
    if (!window) {
        return false;
    }
    NSPrintInfo* info = [NSPrintInfo sharedPrintInfo];
    NSPrintOperation* operation = [wv->webview printOperationWithPrintInfo:info];

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

#if GPUI_OS_WASM
#line 1 "src/wry/wry_wasm.cpp"

namespace wry {

using base::logf;
using base::Str;

static void Unsupported() {
    static bool said = false;
    if (!said) {
        said = true;
        logf("wry: no webview backend on this platform\n");
    }
}

WebView* WebViewNew(void*, const WebViewAttributes*, bool) {
    Unsupported();
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
struct ICoreWebView2ProcessFailedEventHandler;
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

struct DECLSPEC_UUID("b96d755e-0319-4e92-a296-23436f46a1fc") ICoreWebView2Environment;
struct DECLSPEC_UUID("41f3632b-5ef4-404f-ad82-2d606c5a9a21") ICoreWebView2Environment2;
struct DECLSPEC_UUID("80a22ae3-be7c-4ce2-afe1-5a50056cdeeb") ICoreWebView2Environment3;
struct DECLSPEC_UUID("20944379-6dcf-41d6-a0a0-abc0fc50de0d") ICoreWebView2Environment4;
struct DECLSPEC_UUID("319e423d-e0d7-4b8d-9254-ae9475de9b17") ICoreWebView2Environment5;
struct DECLSPEC_UUID("e59ee362-acbd-4857-9a8e-d3644d9459a9") ICoreWebView2Environment6;
struct DECLSPEC_UUID("43c22296-3bbd-43a4-9c00-5c0df6dd29a2") ICoreWebView2Environment7;
struct DECLSPEC_UUID("d6eb91dd-c3d2-45e5-bd29-6dc2bc4de9cf") ICoreWebView2Environment8;
struct DECLSPEC_UUID("f06f41bf-4b5a-49d8-b9f6-fa16cd29f274") ICoreWebView2Environment9;
struct DECLSPEC_UUID("ee0eb9df-6f12-46ce-b53f-3f47b9c928e0") ICoreWebView2Environment10;
struct DECLSPEC_UUID("12aae616-8ccb-44ec-bcb3-eb1831881635") ICoreWebView2ControllerOptions;
struct DECLSPEC_UUID("06c991d8-9e7e-11ed-a8fc-0242ac120002") ICoreWebView2ControllerOptions2;
struct DECLSPEC_UUID("b32b191a-8998-57ca-b7cb-e04617e4ce4a") ICoreWebView2ControllerOptions3;
struct DECLSPEC_UUID("4d00c0d1-9434-4eb6-8078-8697a560334f") ICoreWebView2Controller;
struct DECLSPEC_UUID("c979903e-d4ca-4228-92eb-47ee3fa96eab") ICoreWebView2Controller2;
struct DECLSPEC_UUID("f9614724-5d2b-41dc-aef7-73d62b51543b") ICoreWebView2Controller3;
struct DECLSPEC_UUID("97d418d5-a426-4e49-a151-e1a10f327d9e") ICoreWebView2Controller4;
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
struct DECLSPEC_UUID("e562e4f0-d7fa-43ac-8d71-c05150499f00") ICoreWebView2Settings;
struct DECLSPEC_UUID("ee9a0f68-f46c-4e32-ac23-ef8cac224d2a") ICoreWebView2Settings2;
struct DECLSPEC_UUID("fdb5ab74-af33-4854-84f0-0a631deb5eba") ICoreWebView2Settings3;
struct DECLSPEC_UUID("cb56846c-4168-4d53-b04f-03b6d6796ff2") ICoreWebView2Settings4;
struct DECLSPEC_UUID("183e7052-1d03-43a0-ab99-98e043b66b39") ICoreWebView2Settings5;
struct DECLSPEC_UUID("11cb3acd-9bc8-43b8-83bf-f40753714f87") ICoreWebView2Settings6;
struct DECLSPEC_UUID("488dc902-35ef-42d2-bc7d-94b65c4bc49c") ICoreWebView2Settings7;
struct DECLSPEC_UUID("9e6b0e8f-86ad-4e81-8147-a9b5edb68650") ICoreWebView2Settings8;
struct DECLSPEC_UUID("0528a73b-e92d-49f4-927a-e547dddaa37d") ICoreWebView2Settings9;
struct DECLSPEC_UUID("79110ad3-cd5d-4373-8bc3-c60658f17a5f") ICoreWebView2Profile;
struct DECLSPEC_UUID("fa740d4b-5eae-4344-a8ad-74be31925397") ICoreWebView2Profile2;
struct DECLSPEC_UUID("b188e659-5685-4e05-bdba-fc640e0f1992") ICoreWebView2Profile3;
struct DECLSPEC_UUID("8f4ae680-192e-4ec8-833a-21cfadaef628") ICoreWebView2Profile4;
struct DECLSPEC_UUID("2ee5b76e-6e80-4df2-bcd3-d4ec3340a01b") ICoreWebView2Profile5;
struct DECLSPEC_UUID("bd82fa6a-1d65-4c33-b2b4-0393020cc61b") ICoreWebView2Profile6;
struct DECLSPEC_UUID("7b4c7906-a1aa-4cb4-b723-db09f813d541") ICoreWebView2Profile7;
struct DECLSPEC_UUID("df1aab27-82b9-4ab6-aae8-017a49398c14")
    ICoreWebView2ProfileAddBrowserExtensionCompletedHandler;
struct DECLSPEC_UUID("0f99a40c-e962-4207-9e92-e3d542eff849") ICoreWebView2WebMessageReceivedEventArgs;
struct DECLSPEC_UUID("453e667f-12c7-49d4-be6d-ddbe7956f57a") ICoreWebView2WebResourceRequestedEventArgs;
struct DECLSPEC_UUID("97055cd4-512c-4264-8b5f-e3f446cea6a5") ICoreWebView2WebResourceRequest;
struct DECLSPEC_UUID("aafcc94f-fa27-48fd-97df-830ef75aaec9") ICoreWebView2WebResourceResponse;
struct DECLSPEC_UUID("e86cac0e-5523-465c-b536-8fb9fc8c8c60") ICoreWebView2HttpRequestHeaders;
struct DECLSPEC_UUID("0702fc30-f43b-47bb-ab52-a42cb552ad9f") ICoreWebView2HttpHeadersCollectionIterator;
struct DECLSPEC_UUID("c10e7f7b-b585-46f0-a623-8befbf3e4ee0") ICoreWebView2Deferral;
struct DECLSPEC_UUID("5b495469-e119-438a-9b18-7604f25f2e49") ICoreWebView2NavigationStartingEventArgs;
struct DECLSPEC_UUID("34acb11c-fc37-4418-9132-f9c21d1eafb9") ICoreWebView2NewWindowRequestedEventArgs;
struct DECLSPEC_UUID("5eaf559f-b46e-4397-8860-e422f287ff1e") ICoreWebView2WindowFeatures;
struct DECLSPEC_UUID("973ae2ef-ff18-4894-8fb2-3c758f046810") ICoreWebView2PermissionRequestedEventArgs;
struct DECLSPEC_UUID("4e8a3389-c9d8-4bd2-b6b5-124fee6cc14d") ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;
struct DECLSPEC_UUID("6c4819f3-c9b7-4260-8127-c9f5bde7f68c") ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
struct DECLSPEC_UUID("57213f19-00e6-49fa-8e07-898ea01ecbd2") ICoreWebView2WebMessageReceivedEventHandler;
struct DECLSPEC_UUID("ab00b74c-15f1-4646-80e8-e76341d25d71") ICoreWebView2WebResourceRequestedEventHandler;
struct DECLSPEC_UUID("9adbe429-f36d-432b-9ddc-f8881fbd76e3") ICoreWebView2NavigationStartingEventHandler;
struct DECLSPEC_UUID("d33a35bf-1c49-4f98-93ab-006e0533fe1c") ICoreWebView2NavigationCompletedEventHandler;
struct DECLSPEC_UUID("364471e7-f2be-4910-bdba-d72077d51c4b") ICoreWebView2ContentLoadingEventHandler;
struct DECLSPEC_UUID("f5f2b923-953e-4042-9f95-f3a118e1afd4") ICoreWebView2DocumentTitleChangedEventHandler;
struct DECLSPEC_UUID("d4c185fe-c81c-4989-97af-2d3fa7ab5651") ICoreWebView2NewWindowRequestedEventHandler;
struct DECLSPEC_UUID("5c19e9e0-092f-486b-affa-ca8231913039") ICoreWebView2WindowCloseRequestedEventHandler;
struct DECLSPEC_UUID("15e1c6a3-c72a-4df3-91d7-d097fbec6bfd") ICoreWebView2PermissionRequestedEventHandler;
struct DECLSPEC_UUID("b99369f3-9b11-47b5-bc6f-8e7895fcea17") ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler;
struct DECLSPEC_UUID("49511172-cc67-4bca-9923-137112f4c4cc") ICoreWebView2ExecuteScriptCompletedHandler;
struct DECLSPEC_UUID("e9710a06-1d1d-49b2-8234-226f35846ae5") ICoreWebView2ClearBrowsingDataCompletedHandler;
struct DECLSPEC_UUID("3d6b6cf2-afe1-44c7-a995-c65117714336") ICoreWebView2DownloadOperation;
struct DECLSPEC_UUID("e99bbe21-43e9-4544-a732-282764eafa60") ICoreWebView2DownloadStartingEventArgs;
struct DECLSPEC_UUID("efedc989-c396-41ca-83f7-07f845a55724") ICoreWebView2DownloadStartingEventHandler;
struct DECLSPEC_UUID("81336594-7ede-4ba9-bf71-acf0a95b58dd") ICoreWebView2StateChangedEventHandler;
struct DECLSPEC_UUID("ad26d6be-1486-43e6-bf87-a2034006ca21") ICoreWebView2Cookie;
struct DECLSPEC_UUID("f7f6f714-5d2a-43c6-9503-346ece02d186") ICoreWebView2CookieList;
struct DECLSPEC_UUID("177cd9e7-b6f5-451a-94a0-5d7a3a4c4141") ICoreWebView2CookieManager;
struct DECLSPEC_UUID("5a4f5069-5c15-47c3-8646-f4de1c116670") ICoreWebView2GetCookiesCompletedHandler;
struct DECLSPEC_UUID("2fde08a8-1e9a-4766-8c05-95a9ceb9d1c5") ICoreWebView2EnvironmentOptions;
struct DECLSPEC_UUID("ff85c98a-1ba7-4a6b-90c8-2b752c89e9e2") ICoreWebView2EnvironmentOptions2;
struct DECLSPEC_UUID("4a5c436e-a9e3-4a2e-89c3-910d3513f5cc") ICoreWebView2EnvironmentOptions3;
struct DECLSPEC_UUID("ac52d13f-0d38-475a-9dca-876580d6793e") ICoreWebView2EnvironmentOptions4;
struct DECLSPEC_UUID("0ae35d64-c47f-4464-814e-259c345d1501") ICoreWebView2EnvironmentOptions5;
struct DECLSPEC_UUID("57d29cc3-c84f-42a0-b0e2-effbd5e179de") ICoreWebView2EnvironmentOptions6;
struct DECLSPEC_UUID("c48d539f-e39f-441c-ae68-1f66e570bdc5") ICoreWebView2EnvironmentOptions7;
struct DECLSPEC_UUID("7c7ecf51-e918-5caf-853c-e9a2bcc27775") ICoreWebView2EnvironmentOptions8;

struct ICoreWebView2Environment : IUnknown {
virtual HRESULT STDMETHODCALLTYPE CreateCoreWebView2Controller( HWND parentWindow, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE CreateWebResourceResponse( IStream *content, int statusCode, LPCWSTR reasonPhrase, LPCWSTR headers, ICoreWebView2WebResourceResponse **response) = 0;
virtual HRESULT STDMETHODCALLTYPE get_BrowserVersionString( LPWSTR *versionInfo) = 0;
virtual HRESULT STDMETHODCALLTYPE add_NewBrowserVersionAvailable( ICoreWebView2NewBrowserVersionAvailableEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_NewBrowserVersionAvailable( EventRegistrationToken token) = 0;
};

struct ICoreWebView2Environment2 : ICoreWebView2Environment {
virtual HRESULT STDMETHODCALLTYPE CreateWebResourceRequest( LPCWSTR uri, LPCWSTR Method, IStream *postData, LPCWSTR Headers, ICoreWebView2WebResourceRequest **value) = 0;
};

struct ICoreWebView2Environment3 : ICoreWebView2Environment2 {
virtual HRESULT STDMETHODCALLTYPE CreateCoreWebView2CompositionController( HWND ParentWindow, ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE CreateCoreWebView2PointerInfo( ICoreWebView2PointerInfo **value) = 0;
};

struct ICoreWebView2Environment4 : ICoreWebView2Environment3 {
virtual HRESULT STDMETHODCALLTYPE GetAutomationProviderForWindow( HWND hwnd, IUnknown **value) = 0;
};

struct ICoreWebView2Environment5 : ICoreWebView2Environment4 {
virtual HRESULT STDMETHODCALLTYPE add_BrowserProcessExited( ICoreWebView2BrowserProcessExitedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_BrowserProcessExited( EventRegistrationToken token) = 0;
};

struct ICoreWebView2Environment6 : ICoreWebView2Environment5 {
virtual HRESULT STDMETHODCALLTYPE CreatePrintSettings( ICoreWebView2PrintSettings **value) = 0;
};

struct ICoreWebView2Environment7 : ICoreWebView2Environment6 {
virtual HRESULT STDMETHODCALLTYPE get_UserDataFolder( LPWSTR *value) = 0;
};

struct ICoreWebView2Environment8 : ICoreWebView2Environment7 {
virtual HRESULT STDMETHODCALLTYPE add_ProcessInfosChanged( ICoreWebView2ProcessInfosChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_ProcessInfosChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE GetProcessInfos( ICoreWebView2ProcessInfoCollection **value) = 0;
};

struct ICoreWebView2Environment9 : ICoreWebView2Environment8 {
virtual HRESULT STDMETHODCALLTYPE CreateContextMenuItem( LPCWSTR Label, IStream *iconStream, COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND Kind, ICoreWebView2ContextMenuItem **value) = 0;
};

struct ICoreWebView2Environment10 : ICoreWebView2Environment9 {
virtual HRESULT STDMETHODCALLTYPE CreateCoreWebView2ControllerOptions( ICoreWebView2ControllerOptions **value) = 0;
virtual HRESULT STDMETHODCALLTYPE CreateCoreWebView2ControllerWithOptions( HWND ParentWindow, ICoreWebView2ControllerOptions *options, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE CreateCoreWebView2CompositionControllerWithOptions( HWND ParentWindow, ICoreWebView2ControllerOptions *options, ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler *handler) = 0;
};

struct ICoreWebView2ControllerOptions : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_ProfileName( LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_ProfileName( LPCWSTR value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsInPrivateModeEnabled( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsInPrivateModeEnabled( BOOL value) = 0;
};

struct ICoreWebView2ControllerOptions2 : ICoreWebView2ControllerOptions {
virtual HRESULT STDMETHODCALLTYPE get_ScriptLocale( LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_ScriptLocale( LPCWSTR value) = 0;
};

struct ICoreWebView2ControllerOptions3 : ICoreWebView2ControllerOptions2 {
virtual HRESULT STDMETHODCALLTYPE get_DefaultBackgroundColor( COREWEBVIEW2_COLOR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_DefaultBackgroundColor( COREWEBVIEW2_COLOR value) = 0;
};

struct ICoreWebView2Controller : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_IsVisible( BOOL *isVisible) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsVisible( BOOL isVisible) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Bounds( RECT *bounds) = 0;
virtual HRESULT STDMETHODCALLTYPE put_Bounds( RECT bounds) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ZoomFactor( double *zoomFactor) = 0;
virtual HRESULT STDMETHODCALLTYPE put_ZoomFactor( double zoomFactor) = 0;
virtual HRESULT STDMETHODCALLTYPE add_ZoomFactorChanged( ICoreWebView2ZoomFactorChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_ZoomFactorChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE SetBoundsAndZoomFactor( RECT bounds, double zoomFactor) = 0;
virtual HRESULT STDMETHODCALLTYPE MoveFocus( COREWEBVIEW2_MOVE_FOCUS_REASON reason) = 0;
virtual HRESULT STDMETHODCALLTYPE add_MoveFocusRequested( ICoreWebView2MoveFocusRequestedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_MoveFocusRequested( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_GotFocus( ICoreWebView2FocusChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_GotFocus( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_LostFocus( ICoreWebView2FocusChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_LostFocus( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_AcceleratorKeyPressed( ICoreWebView2AcceleratorKeyPressedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_AcceleratorKeyPressed( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ParentWindow( HWND *parentWindow) = 0;
virtual HRESULT STDMETHODCALLTYPE put_ParentWindow( HWND parentWindow) = 0;
virtual HRESULT STDMETHODCALLTYPE NotifyParentWindowPositionChanged() = 0;
virtual HRESULT STDMETHODCALLTYPE Close() = 0;
virtual HRESULT STDMETHODCALLTYPE get_CoreWebView2( ICoreWebView2 **coreWebView2) = 0;
};

struct ICoreWebView2Controller2 : ICoreWebView2Controller {
virtual HRESULT STDMETHODCALLTYPE get_DefaultBackgroundColor( COREWEBVIEW2_COLOR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_DefaultBackgroundColor( COREWEBVIEW2_COLOR value) = 0;
};

struct ICoreWebView2Controller3 : ICoreWebView2Controller2 {
virtual HRESULT STDMETHODCALLTYPE get_RasterizationScale( double *scale) = 0;
virtual HRESULT STDMETHODCALLTYPE put_RasterizationScale( double scale) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ShouldDetectMonitorScaleChanges( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_ShouldDetectMonitorScaleChanges( BOOL value) = 0;
virtual HRESULT STDMETHODCALLTYPE add_RasterizationScaleChanged( ICoreWebView2RasterizationScaleChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_RasterizationScaleChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE get_BoundsMode( COREWEBVIEW2_BOUNDS_MODE *boundsMode) = 0;
virtual HRESULT STDMETHODCALLTYPE put_BoundsMode( COREWEBVIEW2_BOUNDS_MODE boundsMode) = 0;
};

struct ICoreWebView2Controller4 : ICoreWebView2Controller3 {
virtual HRESULT STDMETHODCALLTYPE get_AllowExternalDrop( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_AllowExternalDrop( BOOL value) = 0;
};

struct ICoreWebView2 : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_Settings( ICoreWebView2Settings **settings) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Source( LPWSTR *uri) = 0;
virtual HRESULT STDMETHODCALLTYPE Navigate( LPCWSTR uri) = 0;
virtual HRESULT STDMETHODCALLTYPE NavigateToString( LPCWSTR htmlContent) = 0;
virtual HRESULT STDMETHODCALLTYPE add_NavigationStarting( ICoreWebView2NavigationStartingEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_NavigationStarting( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_ContentLoading( ICoreWebView2ContentLoadingEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_ContentLoading( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_SourceChanged( ICoreWebView2SourceChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_SourceChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_HistoryChanged( ICoreWebView2HistoryChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_HistoryChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_NavigationCompleted( ICoreWebView2NavigationCompletedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_NavigationCompleted( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_FrameNavigationStarting( ICoreWebView2NavigationStartingEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_FrameNavigationStarting( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_FrameNavigationCompleted( ICoreWebView2NavigationCompletedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_FrameNavigationCompleted( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_ScriptDialogOpening( ICoreWebView2ScriptDialogOpeningEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_ScriptDialogOpening( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_PermissionRequested( ICoreWebView2PermissionRequestedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_PermissionRequested( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_ProcessFailed( ICoreWebView2ProcessFailedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_ProcessFailed( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE AddScriptToExecuteOnDocumentCreated( LPCWSTR javaScript, ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE RemoveScriptToExecuteOnDocumentCreated( LPCWSTR id) = 0;
virtual HRESULT STDMETHODCALLTYPE ExecuteScript( LPCWSTR javaScript, ICoreWebView2ExecuteScriptCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE CapturePreview( COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT imageFormat, IStream *imageStream, ICoreWebView2CapturePreviewCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE Reload() = 0;
virtual HRESULT STDMETHODCALLTYPE PostWebMessageAsJson( LPCWSTR webMessageAsJson) = 0;
virtual HRESULT STDMETHODCALLTYPE PostWebMessageAsString( LPCWSTR webMessageAsString) = 0;
virtual HRESULT STDMETHODCALLTYPE add_WebMessageReceived( ICoreWebView2WebMessageReceivedEventHandler *handler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_WebMessageReceived( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE CallDevToolsProtocolMethod( LPCWSTR methodName, LPCWSTR parametersAsJson, ICoreWebView2CallDevToolsProtocolMethodCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE get_BrowserProcessId( UINT32 *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_CanGoBack( BOOL *canGoBack) = 0;
virtual HRESULT STDMETHODCALLTYPE get_CanGoForward( BOOL *canGoForward) = 0;
virtual HRESULT STDMETHODCALLTYPE GoBack() = 0;
virtual HRESULT STDMETHODCALLTYPE GoForward() = 0;
virtual HRESULT STDMETHODCALLTYPE GetDevToolsProtocolEventReceiver( LPCWSTR eventName, ICoreWebView2DevToolsProtocolEventReceiver **receiver) = 0;
virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
virtual HRESULT STDMETHODCALLTYPE add_NewWindowRequested( ICoreWebView2NewWindowRequestedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_NewWindowRequested( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_DocumentTitleChanged( ICoreWebView2DocumentTitleChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_DocumentTitleChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE get_DocumentTitle( LPWSTR *title) = 0;
virtual HRESULT STDMETHODCALLTYPE AddHostObjectToScript( LPCWSTR name, VARIANT *object) = 0;
virtual HRESULT STDMETHODCALLTYPE RemoveHostObjectFromScript( LPCWSTR name) = 0;
virtual HRESULT STDMETHODCALLTYPE OpenDevToolsWindow() = 0;
virtual HRESULT STDMETHODCALLTYPE add_ContainsFullScreenElementChanged( ICoreWebView2ContainsFullScreenElementChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_ContainsFullScreenElementChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ContainsFullScreenElement( BOOL *containsFullScreenElement) = 0;
virtual HRESULT STDMETHODCALLTYPE add_WebResourceRequested( ICoreWebView2WebResourceRequestedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_WebResourceRequested( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE AddWebResourceRequestedFilter( const LPCWSTR uri, const COREWEBVIEW2_WEB_RESOURCE_CONTEXT resourceContext) = 0;
virtual HRESULT STDMETHODCALLTYPE RemoveWebResourceRequestedFilter( const LPCWSTR uri, const COREWEBVIEW2_WEB_RESOURCE_CONTEXT resourceContext) = 0;
virtual HRESULT STDMETHODCALLTYPE add_WindowCloseRequested( ICoreWebView2WindowCloseRequestedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_WindowCloseRequested( EventRegistrationToken token) = 0;
};

struct ICoreWebView2_2 : ICoreWebView2 {
virtual HRESULT STDMETHODCALLTYPE add_WebResourceResponseReceived( ICoreWebView2WebResourceResponseReceivedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_WebResourceResponseReceived( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE NavigateWithWebResourceRequest( ICoreWebView2WebResourceRequest *request) = 0;
virtual HRESULT STDMETHODCALLTYPE add_DOMContentLoaded( ICoreWebView2DOMContentLoadedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_DOMContentLoaded( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE get_CookieManager( ICoreWebView2CookieManager **cookieManager) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Environment( ICoreWebView2Environment **environment) = 0;
};

struct ICoreWebView2_3 : ICoreWebView2_2 {
virtual HRESULT STDMETHODCALLTYPE TrySuspend( ICoreWebView2TrySuspendCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE Resume() = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsSuspended( BOOL *isSuspended) = 0;
virtual HRESULT STDMETHODCALLTYPE SetVirtualHostNameToFolderMapping( LPCWSTR hostName, LPCWSTR folderPath, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND accessKind) = 0;
virtual HRESULT STDMETHODCALLTYPE ClearVirtualHostNameToFolderMapping( LPCWSTR hostName) = 0;
};

struct ICoreWebView2_4 : ICoreWebView2_3 {
virtual HRESULT STDMETHODCALLTYPE add_FrameCreated( ICoreWebView2FrameCreatedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_FrameCreated( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_DownloadStarting( ICoreWebView2DownloadStartingEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_DownloadStarting( EventRegistrationToken token) = 0;
};

struct ICoreWebView2_5 : ICoreWebView2_4 {
virtual HRESULT STDMETHODCALLTYPE add_ClientCertificateRequested( ICoreWebView2ClientCertificateRequestedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_ClientCertificateRequested( EventRegistrationToken token) = 0;
};

struct ICoreWebView2_6 : ICoreWebView2_5 {
virtual HRESULT STDMETHODCALLTYPE OpenTaskManagerWindow() = 0;
};

struct ICoreWebView2_7 : ICoreWebView2_6 {
virtual HRESULT STDMETHODCALLTYPE PrintToPdf( LPCWSTR ResultFilePath, ICoreWebView2PrintSettings *printSettings, ICoreWebView2PrintToPdfCompletedHandler *handler) = 0;
};

struct ICoreWebView2_8 : ICoreWebView2_7 {
virtual HRESULT STDMETHODCALLTYPE add_IsMutedChanged( ICoreWebView2IsMutedChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_IsMutedChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsMuted( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsMuted( BOOL value) = 0;
virtual HRESULT STDMETHODCALLTYPE add_IsDocumentPlayingAudioChanged( ICoreWebView2IsDocumentPlayingAudioChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_IsDocumentPlayingAudioChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsDocumentPlayingAudio( BOOL *value) = 0;
};

struct ICoreWebView2_9 : ICoreWebView2_8 {
virtual HRESULT STDMETHODCALLTYPE add_IsDefaultDownloadDialogOpenChanged( ICoreWebView2IsDefaultDownloadDialogOpenChangedEventHandler *handler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_IsDefaultDownloadDialogOpenChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsDefaultDownloadDialogOpen( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE OpenDefaultDownloadDialog() = 0;
virtual HRESULT STDMETHODCALLTYPE CloseDefaultDownloadDialog() = 0;
virtual HRESULT STDMETHODCALLTYPE get_DefaultDownloadDialogCornerAlignment( COREWEBVIEW2_DEFAULT_DOWNLOAD_DIALOG_CORNER_ALIGNMENT *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_DefaultDownloadDialogCornerAlignment( COREWEBVIEW2_DEFAULT_DOWNLOAD_DIALOG_CORNER_ALIGNMENT value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_DefaultDownloadDialogMargin( POINT *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_DefaultDownloadDialogMargin( POINT value) = 0;
};

struct ICoreWebView2_10 : ICoreWebView2_9 {
virtual HRESULT STDMETHODCALLTYPE add_BasicAuthenticationRequested( ICoreWebView2BasicAuthenticationRequestedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_BasicAuthenticationRequested( EventRegistrationToken token) = 0;
};

struct ICoreWebView2_11 : ICoreWebView2_10 {
virtual HRESULT STDMETHODCALLTYPE CallDevToolsProtocolMethodForSession( LPCWSTR sessionId, LPCWSTR methodName, LPCWSTR parametersAsJson, ICoreWebView2CallDevToolsProtocolMethodCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE add_ContextMenuRequested( ICoreWebView2ContextMenuRequestedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_ContextMenuRequested( EventRegistrationToken token) = 0;
};

struct ICoreWebView2_12 : ICoreWebView2_11 {
virtual HRESULT STDMETHODCALLTYPE add_StatusBarTextChanged( ICoreWebView2StatusBarTextChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_StatusBarTextChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE get_StatusBarText( LPWSTR *value) = 0;
};

struct ICoreWebView2_13 : ICoreWebView2_12 {
virtual HRESULT STDMETHODCALLTYPE get_Profile( ICoreWebView2Profile **value) = 0;
};

struct ICoreWebView2_14 : ICoreWebView2_13 {
virtual HRESULT STDMETHODCALLTYPE add_ServerCertificateErrorDetected( ICoreWebView2ServerCertificateErrorDetectedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_ServerCertificateErrorDetected( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE ClearServerCertificateErrorActions( ICoreWebView2ClearServerCertificateErrorActionsCompletedHandler *handler) = 0;
};

struct ICoreWebView2_15 : ICoreWebView2_14 {
virtual HRESULT STDMETHODCALLTYPE add_FaviconChanged( ICoreWebView2FaviconChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_FaviconChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE get_FaviconUri( LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE GetFavicon( COREWEBVIEW2_FAVICON_IMAGE_FORMAT format, ICoreWebView2GetFaviconCompletedHandler *completedHandler) = 0;
};

struct ICoreWebView2_16 : ICoreWebView2_15 {
virtual HRESULT STDMETHODCALLTYPE Print( ICoreWebView2PrintSettings *printSettings, ICoreWebView2PrintCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE ShowPrintUI( COREWEBVIEW2_PRINT_DIALOG_KIND printDialogKind) = 0;
virtual HRESULT STDMETHODCALLTYPE PrintToPdfStream( ICoreWebView2PrintSettings *printSettings, ICoreWebView2PrintToPdfStreamCompletedHandler *handler) = 0;
};

struct ICoreWebView2_17 : ICoreWebView2_16 {
virtual HRESULT STDMETHODCALLTYPE PostSharedBufferToScript( ICoreWebView2SharedBuffer *sharedBuffer, COREWEBVIEW2_SHARED_BUFFER_ACCESS access, LPCWSTR additionalDataAsJson) = 0;
};

struct ICoreWebView2_18 : ICoreWebView2_17 {
virtual HRESULT STDMETHODCALLTYPE add_LaunchingExternalUriScheme( ICoreWebView2LaunchingExternalUriSchemeEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_LaunchingExternalUriScheme( EventRegistrationToken token) = 0;
};

struct ICoreWebView2_19 : ICoreWebView2_18 {
virtual HRESULT STDMETHODCALLTYPE get_MemoryUsageTargetLevel( COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_MemoryUsageTargetLevel( COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL value) = 0;
};

struct ICoreWebView2_20 : ICoreWebView2_19 {
virtual HRESULT STDMETHODCALLTYPE get_FrameId( UINT32 *value) = 0;
};

struct ICoreWebView2_21 : ICoreWebView2_20 {
virtual HRESULT STDMETHODCALLTYPE ExecuteScriptWithResult( LPCWSTR javaScript, ICoreWebView2ExecuteScriptWithResultCompletedHandler *handler) = 0;
};

struct ICoreWebView2_22 : ICoreWebView2_21 {
virtual HRESULT STDMETHODCALLTYPE AddWebResourceRequestedFilterWithRequestSourceKinds( LPCWSTR uri, COREWEBVIEW2_WEB_RESOURCE_CONTEXT ResourceContext, COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS requestSourceKinds) = 0;
virtual HRESULT STDMETHODCALLTYPE RemoveWebResourceRequestedFilterWithRequestSourceKinds( LPCWSTR uri, COREWEBVIEW2_WEB_RESOURCE_CONTEXT ResourceContext, COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS requestSourceKinds) = 0;
};

struct ICoreWebView2Settings : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_IsScriptEnabled( BOOL *isScriptEnabled) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsScriptEnabled( BOOL isScriptEnabled) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsWebMessageEnabled( BOOL *isWebMessageEnabled) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsWebMessageEnabled( BOOL isWebMessageEnabled) = 0;
virtual HRESULT STDMETHODCALLTYPE get_AreDefaultScriptDialogsEnabled( BOOL *areDefaultScriptDialogsEnabled) = 0;
virtual HRESULT STDMETHODCALLTYPE put_AreDefaultScriptDialogsEnabled( BOOL areDefaultScriptDialogsEnabled) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsStatusBarEnabled( BOOL *isStatusBarEnabled) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsStatusBarEnabled( BOOL isStatusBarEnabled) = 0;
virtual HRESULT STDMETHODCALLTYPE get_AreDevToolsEnabled( BOOL *areDevToolsEnabled) = 0;
virtual HRESULT STDMETHODCALLTYPE put_AreDevToolsEnabled( BOOL areDevToolsEnabled) = 0;
virtual HRESULT STDMETHODCALLTYPE get_AreDefaultContextMenusEnabled( BOOL *enabled) = 0;
virtual HRESULT STDMETHODCALLTYPE put_AreDefaultContextMenusEnabled( BOOL enabled) = 0;
virtual HRESULT STDMETHODCALLTYPE get_AreHostObjectsAllowed( BOOL *allowed) = 0;
virtual HRESULT STDMETHODCALLTYPE put_AreHostObjectsAllowed( BOOL allowed) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsZoomControlEnabled( BOOL *enabled) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsZoomControlEnabled( BOOL enabled) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsBuiltInErrorPageEnabled( BOOL *enabled) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsBuiltInErrorPageEnabled( BOOL enabled) = 0;
};

struct ICoreWebView2Settings2 : ICoreWebView2Settings {
virtual HRESULT STDMETHODCALLTYPE get_UserAgent( LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_UserAgent( LPCWSTR value) = 0;
};

struct ICoreWebView2Settings3 : ICoreWebView2Settings2 {
virtual HRESULT STDMETHODCALLTYPE get_AreBrowserAcceleratorKeysEnabled( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_AreBrowserAcceleratorKeysEnabled( BOOL value) = 0;
};

struct ICoreWebView2Settings4 : ICoreWebView2Settings3 {
virtual HRESULT STDMETHODCALLTYPE get_IsPasswordAutosaveEnabled( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsPasswordAutosaveEnabled( BOOL value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsGeneralAutofillEnabled( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsGeneralAutofillEnabled( BOOL value) = 0;
};

struct ICoreWebView2Settings5 : ICoreWebView2Settings4 {
virtual HRESULT STDMETHODCALLTYPE get_IsPinchZoomEnabled( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsPinchZoomEnabled( BOOL value) = 0;
};

struct ICoreWebView2Settings6 : ICoreWebView2Settings5 {
virtual HRESULT STDMETHODCALLTYPE get_IsSwipeNavigationEnabled( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsSwipeNavigationEnabled( BOOL value) = 0;
};

struct ICoreWebView2Settings7 : ICoreWebView2Settings6 {
virtual HRESULT STDMETHODCALLTYPE get_HiddenPdfToolbarItems( COREWEBVIEW2_PDF_TOOLBAR_ITEMS *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_HiddenPdfToolbarItems( COREWEBVIEW2_PDF_TOOLBAR_ITEMS value) = 0;
};

struct ICoreWebView2Settings8 : ICoreWebView2Settings7 {
virtual HRESULT STDMETHODCALLTYPE get_IsReputationCheckingRequired( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsReputationCheckingRequired( BOOL value) = 0;
};

struct ICoreWebView2Settings9 : ICoreWebView2Settings8 {
virtual HRESULT STDMETHODCALLTYPE get_IsNonClientRegionSupportEnabled( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsNonClientRegionSupportEnabled( BOOL value) = 0;
};

struct ICoreWebView2Profile : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_ProfileName( LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsInPrivateModeEnabled( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ProfilePath( LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_DefaultDownloadFolderPath( LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_DefaultDownloadFolderPath( LPCWSTR value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_PreferredColorScheme( COREWEBVIEW2_PREFERRED_COLOR_SCHEME *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_PreferredColorScheme( COREWEBVIEW2_PREFERRED_COLOR_SCHEME value) = 0;
};

struct ICoreWebView2Profile2 : ICoreWebView2Profile {
virtual HRESULT STDMETHODCALLTYPE ClearBrowsingData( COREWEBVIEW2_BROWSING_DATA_KINDS dataKinds, ICoreWebView2ClearBrowsingDataCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE ClearBrowsingDataInTimeRange( COREWEBVIEW2_BROWSING_DATA_KINDS dataKinds, double startTime, double endTime, ICoreWebView2ClearBrowsingDataCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE ClearBrowsingDataAll( ICoreWebView2ClearBrowsingDataCompletedHandler *handler) = 0;
};

struct ICoreWebView2Profile3 : ICoreWebView2Profile2 {
virtual HRESULT STDMETHODCALLTYPE get_PreferredTrackingPreventionLevel( COREWEBVIEW2_TRACKING_PREVENTION_LEVEL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_PreferredTrackingPreventionLevel( COREWEBVIEW2_TRACKING_PREVENTION_LEVEL value) = 0;
};

struct ICoreWebView2Profile4 : ICoreWebView2Profile3 {
virtual HRESULT STDMETHODCALLTYPE SetPermissionState( COREWEBVIEW2_PERMISSION_KIND permissionKind, LPCWSTR origin, COREWEBVIEW2_PERMISSION_STATE state, ICoreWebView2SetPermissionStateCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE GetNonDefaultPermissionSettings( ICoreWebView2GetNonDefaultPermissionSettingsCompletedHandler *handler) = 0;
};

struct ICoreWebView2Profile5 : ICoreWebView2Profile4 {
virtual HRESULT STDMETHODCALLTYPE get_CookieManager( ICoreWebView2CookieManager **value) = 0;
};

struct ICoreWebView2Profile6 : ICoreWebView2Profile5 {
virtual HRESULT STDMETHODCALLTYPE get_IsPasswordAutosaveEnabled( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsPasswordAutosaveEnabled( BOOL value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsGeneralAutofillEnabled( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsGeneralAutofillEnabled( BOOL value) = 0;
};

struct ICoreWebView2Profile7 : ICoreWebView2Profile6 {
virtual HRESULT STDMETHODCALLTYPE AddBrowserExtension( LPCWSTR extensionFolderPath, ICoreWebView2ProfileAddBrowserExtensionCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE GetBrowserExtensions( ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler *handler) = 0;
};

struct ICoreWebView2ProfileAddBrowserExtensionCompletedHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( HRESULT errorCode, ICoreWebView2BrowserExtension *result) = 0;
};

struct ICoreWebView2DownloadOperation : IUnknown {
virtual HRESULT STDMETHODCALLTYPE add_BytesReceivedChanged( ICoreWebView2BytesReceivedChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_BytesReceivedChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_EstimatedEndTimeChanged( ICoreWebView2EstimatedEndTimeChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_EstimatedEndTimeChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE add_StateChanged( ICoreWebView2StateChangedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
virtual HRESULT STDMETHODCALLTYPE remove_StateChanged( EventRegistrationToken token) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Uri( LPWSTR *uri) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ContentDisposition( LPWSTR *contentDisposition) = 0;
virtual HRESULT STDMETHODCALLTYPE get_MimeType( LPWSTR *mimeType) = 0;
virtual HRESULT STDMETHODCALLTYPE get_TotalBytesToReceive( INT64 *totalBytesToReceive) = 0;
virtual HRESULT STDMETHODCALLTYPE get_BytesReceived( INT64 *bytesReceived) = 0;
virtual HRESULT STDMETHODCALLTYPE get_EstimatedEndTime( LPWSTR *estimatedEndTime) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ResultFilePath( LPWSTR *resultFilePath) = 0;
virtual HRESULT STDMETHODCALLTYPE get_State( COREWEBVIEW2_DOWNLOAD_STATE *downloadState) = 0;
virtual HRESULT STDMETHODCALLTYPE get_InterruptReason( COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON *interruptReason) = 0;
virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
virtual HRESULT STDMETHODCALLTYPE Pause() = 0;
virtual HRESULT STDMETHODCALLTYPE Resume() = 0;
virtual HRESULT STDMETHODCALLTYPE get_CanResume( BOOL *canResume) = 0;
};

struct ICoreWebView2DownloadStartingEventArgs : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_DownloadOperation( ICoreWebView2DownloadOperation **downloadOperation) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Cancel( BOOL *cancel) = 0;
virtual HRESULT STDMETHODCALLTYPE put_Cancel( BOOL cancel) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ResultFilePath( LPWSTR *resultFilePath) = 0;
virtual HRESULT STDMETHODCALLTYPE put_ResultFilePath( LPCWSTR resultFilePath) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Handled( BOOL *handled) = 0;
virtual HRESULT STDMETHODCALLTYPE put_Handled( BOOL handled) = 0;
virtual HRESULT STDMETHODCALLTYPE GetDeferral( ICoreWebView2Deferral **deferral) = 0;
};

struct ICoreWebView2DownloadStartingEventHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( ICoreWebView2 *sender, ICoreWebView2DownloadStartingEventArgs *args) = 0;
};

struct ICoreWebView2StateChangedEventHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( ICoreWebView2DownloadOperation *sender, IUnknown *args) = 0;
};

struct ICoreWebView2Cookie : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_Name( LPWSTR *name) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Value( LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_Value( LPCWSTR value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Domain( LPWSTR *domain) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Path( LPWSTR *path) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Expires( double *expires) = 0;
virtual HRESULT STDMETHODCALLTYPE put_Expires( double expires) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsHttpOnly( BOOL *isHttpOnly) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsHttpOnly( BOOL isHttpOnly) = 0;
virtual HRESULT STDMETHODCALLTYPE get_SameSite( COREWEBVIEW2_COOKIE_SAME_SITE_KIND *sameSite) = 0;
virtual HRESULT STDMETHODCALLTYPE put_SameSite( COREWEBVIEW2_COOKIE_SAME_SITE_KIND sameSite) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsSecure( BOOL *isSecure) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsSecure( BOOL isSecure) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsSession( BOOL *isSession) = 0;
};

struct ICoreWebView2CookieList : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_Count( UINT32 *value) = 0;
virtual HRESULT STDMETHODCALLTYPE GetValueAtIndex( UINT32 index, ICoreWebView2Cookie **value) = 0;
};

struct ICoreWebView2CookieManager : IUnknown {
virtual HRESULT STDMETHODCALLTYPE CreateCookie( LPCWSTR name, LPCWSTR value, LPCWSTR domain, LPCWSTR path, ICoreWebView2Cookie **cookie) = 0;
virtual HRESULT STDMETHODCALLTYPE CopyCookie( ICoreWebView2Cookie *cookieParam, ICoreWebView2Cookie **cookie) = 0;
virtual HRESULT STDMETHODCALLTYPE GetCookies( LPCWSTR uri, ICoreWebView2GetCookiesCompletedHandler *handler) = 0;
virtual HRESULT STDMETHODCALLTYPE AddOrUpdateCookie( ICoreWebView2Cookie *cookie) = 0;
virtual HRESULT STDMETHODCALLTYPE DeleteCookie( ICoreWebView2Cookie *cookie) = 0;
virtual HRESULT STDMETHODCALLTYPE DeleteCookies( LPCWSTR name, LPCWSTR uri) = 0;
virtual HRESULT STDMETHODCALLTYPE DeleteCookiesWithDomainAndPath( LPCWSTR name, LPCWSTR domain, LPCWSTR path) = 0;
virtual HRESULT STDMETHODCALLTYPE DeleteAllCookies() = 0;
};

struct ICoreWebView2GetCookiesCompletedHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( HRESULT errorCode, ICoreWebView2CookieList *result) = 0;
};

struct ICoreWebView2WebMessageReceivedEventArgs : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_Source( LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_WebMessageAsJson( LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE TryGetWebMessageAsString( LPWSTR *value) = 0;
};

struct ICoreWebView2WebResourceRequestedEventArgs : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_Request( ICoreWebView2WebResourceRequest **request) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Response( ICoreWebView2WebResourceResponse **response) = 0;
virtual HRESULT STDMETHODCALLTYPE put_Response( ICoreWebView2WebResourceResponse *response) = 0;
virtual HRESULT STDMETHODCALLTYPE GetDeferral( ICoreWebView2Deferral **deferral) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ResourceContext( COREWEBVIEW2_WEB_RESOURCE_CONTEXT *context) = 0;
};

struct ICoreWebView2WebResourceRequest : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_Uri( LPWSTR *uri) = 0;
virtual HRESULT STDMETHODCALLTYPE put_Uri( LPCWSTR uri) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Method( LPWSTR *method) = 0;
virtual HRESULT STDMETHODCALLTYPE put_Method( LPCWSTR method) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Content( IStream **content) = 0;
virtual HRESULT STDMETHODCALLTYPE put_Content( IStream *content) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Headers( ICoreWebView2HttpRequestHeaders **headers) = 0;
};

struct ICoreWebView2WebResourceResponse : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_Content( IStream **content) = 0;
virtual HRESULT STDMETHODCALLTYPE put_Content( IStream *content) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Headers( ICoreWebView2HttpResponseHeaders **headers) = 0;
virtual HRESULT STDMETHODCALLTYPE get_StatusCode( int *statusCode) = 0;
virtual HRESULT STDMETHODCALLTYPE put_StatusCode( int statusCode) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ReasonPhrase( LPWSTR *reasonPhrase) = 0;
virtual HRESULT STDMETHODCALLTYPE put_ReasonPhrase( LPCWSTR reasonPhrase) = 0;
};

struct ICoreWebView2HttpRequestHeaders : IUnknown {
virtual HRESULT STDMETHODCALLTYPE GetHeader( LPCWSTR name, LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE GetHeaders( LPCWSTR name, ICoreWebView2HttpHeadersCollectionIterator **value) = 0;
virtual HRESULT STDMETHODCALLTYPE Contains( LPCWSTR name, BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE SetHeader( LPCWSTR name, LPCWSTR value) = 0;
virtual HRESULT STDMETHODCALLTYPE RemoveHeader( LPCWSTR name) = 0;
virtual HRESULT STDMETHODCALLTYPE GetIterator( ICoreWebView2HttpHeadersCollectionIterator **value) = 0;
};

struct ICoreWebView2HttpHeadersCollectionIterator : IUnknown {
virtual HRESULT STDMETHODCALLTYPE GetCurrentHeader( LPWSTR *name, LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_HasCurrentHeader( BOOL *hasCurrent) = 0;
virtual HRESULT STDMETHODCALLTYPE MoveNext( BOOL *hasNext) = 0;
};

struct ICoreWebView2Deferral : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Complete() = 0;
};

struct ICoreWebView2NavigationStartingEventArgs : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_Uri( LPWSTR *uri) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsUserInitiated( BOOL *isUserInitiated) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsRedirected( BOOL *isRedirected) = 0;
virtual HRESULT STDMETHODCALLTYPE get_RequestHeaders( ICoreWebView2HttpRequestHeaders **requestHeaders) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Cancel( BOOL *cancel) = 0;
virtual HRESULT STDMETHODCALLTYPE put_Cancel( BOOL cancel) = 0;
virtual HRESULT STDMETHODCALLTYPE get_NavigationId( UINT64 *navigationId) = 0;
};

struct ICoreWebView2NewWindowRequestedEventArgs : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_Uri( LPWSTR *uri) = 0;
virtual HRESULT STDMETHODCALLTYPE put_NewWindow( ICoreWebView2 *newWindow) = 0;
virtual HRESULT STDMETHODCALLTYPE get_NewWindow( ICoreWebView2 **newWindow) = 0;
virtual HRESULT STDMETHODCALLTYPE put_Handled( BOOL handled) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Handled( BOOL *handled) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsUserInitiated( BOOL *isUserInitiated) = 0;
virtual HRESULT STDMETHODCALLTYPE GetDeferral( ICoreWebView2Deferral **deferral) = 0;
virtual HRESULT STDMETHODCALLTYPE get_WindowFeatures( ICoreWebView2WindowFeatures **value) = 0;
};

struct ICoreWebView2WindowFeatures : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_HasPosition( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_HasSize( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Left( UINT32 *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Top( UINT32 *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Height( UINT32 *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Width( UINT32 *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ShouldDisplayMenuBar( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ShouldDisplayStatus( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ShouldDisplayToolbar( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ShouldDisplayScrollBars( BOOL *value) = 0;
};

struct ICoreWebView2PermissionRequestedEventArgs : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_Uri( LPWSTR *uri) = 0;
virtual HRESULT STDMETHODCALLTYPE get_PermissionKind( COREWEBVIEW2_PERMISSION_KIND *permissionKind) = 0;
virtual HRESULT STDMETHODCALLTYPE get_IsUserInitiated( BOOL *isUserInitiated) = 0;
virtual HRESULT STDMETHODCALLTYPE get_State( COREWEBVIEW2_PERMISSION_STATE *state) = 0;
virtual HRESULT STDMETHODCALLTYPE put_State( COREWEBVIEW2_PERMISSION_STATE state) = 0;
virtual HRESULT STDMETHODCALLTYPE GetDeferral( ICoreWebView2Deferral **deferral) = 0;
};

struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( HRESULT errorCode, ICoreWebView2Environment *result) = 0;
};

struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( HRESULT errorCode, ICoreWebView2Controller *result) = 0;
};

struct ICoreWebView2WebMessageReceivedEventHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args) = 0;
};

struct ICoreWebView2WebResourceRequestedEventHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( ICoreWebView2 *sender, ICoreWebView2WebResourceRequestedEventArgs *args) = 0;
};

struct ICoreWebView2NavigationStartingEventHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( ICoreWebView2 *sender, ICoreWebView2NavigationStartingEventArgs *args) = 0;
};

struct ICoreWebView2NavigationCompletedEventHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) = 0;
};

struct ICoreWebView2ContentLoadingEventHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( ICoreWebView2 *sender, ICoreWebView2ContentLoadingEventArgs *args) = 0;
};

struct ICoreWebView2DocumentTitleChangedEventHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( ICoreWebView2 *sender, IUnknown *args) = 0;
};

struct ICoreWebView2NewWindowRequestedEventHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( ICoreWebView2 *sender, ICoreWebView2NewWindowRequestedEventArgs *args) = 0;
};

struct ICoreWebView2WindowCloseRequestedEventHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( ICoreWebView2 *sender, IUnknown *args) = 0;
};

struct ICoreWebView2PermissionRequestedEventHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( ICoreWebView2 *sender, ICoreWebView2PermissionRequestedEventArgs *args) = 0;
};

struct ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( HRESULT errorCode, LPCWSTR result) = 0;
};

struct ICoreWebView2ExecuteScriptCompletedHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( HRESULT errorCode, LPCWSTR result) = 0;
};

struct ICoreWebView2ClearBrowsingDataCompletedHandler : IUnknown {
virtual HRESULT STDMETHODCALLTYPE Invoke( HRESULT errorCode) = 0;
};

struct ICoreWebView2EnvironmentOptions : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments( LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments( LPCWSTR value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_Language( LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_Language( LPCWSTR value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion( LPWSTR *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion( LPCWSTR value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount( BOOL *allow) = 0;
virtual HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount( BOOL allow) = 0;
};

struct ICoreWebView2EnvironmentOptions2 : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_ExclusiveUserDataFolderAccess( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_ExclusiveUserDataFolderAccess( BOOL value) = 0;
};

struct ICoreWebView2EnvironmentOptions3 : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_IsCustomCrashReportingEnabled( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_IsCustomCrashReportingEnabled( BOOL value) = 0;
};

struct ICoreWebView2EnvironmentOptions4 : IUnknown {
virtual HRESULT STDMETHODCALLTYPE GetCustomSchemeRegistrations( UINT32 *count, IUnknown ***values) = 0;
virtual HRESULT STDMETHODCALLTYPE SetCustomSchemeRegistrations( UINT32 count, IUnknown **values) = 0;
};

struct ICoreWebView2EnvironmentOptions5 : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_EnableTrackingPrevention( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_EnableTrackingPrevention( BOOL value) = 0;
};

struct ICoreWebView2EnvironmentOptions6 : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_AreBrowserExtensionsEnabled( BOOL *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_AreBrowserExtensionsEnabled( BOOL value) = 0;
};

struct ICoreWebView2EnvironmentOptions7 : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_ChannelSearchKind( COREWEBVIEW2_CHANNEL_SEARCH_KIND *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_ChannelSearchKind( COREWEBVIEW2_CHANNEL_SEARCH_KIND value) = 0;
virtual HRESULT STDMETHODCALLTYPE get_ReleaseChannels( COREWEBVIEW2_RELEASE_CHANNELS *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_ReleaseChannels( COREWEBVIEW2_RELEASE_CHANNELS value) = 0;
};

struct ICoreWebView2EnvironmentOptions8 : IUnknown {
virtual HRESULT STDMETHODCALLTYPE get_ScrollBarStyle( COREWEBVIEW2_SCROLLBAR_STYLE *value) = 0;
virtual HRESULT STDMETHODCALLTYPE put_ScrollBarStyle( COREWEBVIEW2_SCROLLBAR_STYLE value) = 0;
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
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, wlen, nullptr, 0, nullptr, nullptr);
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
    if (s.len == 0) {
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
    auto getCurrentApplicationUserModelId = (GetCurrentApplicationUserModelIdFn)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "GetCurrentApplicationUserModelId");
    if (getCurrentApplicationUserModelId) {
        UINT32 len = 256;
        if (getCurrentApplicationUserModelId(&len, appId) != ERROR_SUCCESS) {
            appId[0] = 0;
        }
    }
    if (appId[0] == 0) {
        typedef HRESULT(WINAPI * GetCurrentProcessExplicitAppUserModelIdFn)(PWSTR*);
        HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
        auto getExplicit = shell32 ? (GetCurrentProcessExplicitAppUserModelIdFn)GetProcAddress(
                                         shell32, "GetCurrentProcessExplicitAppUserModelID")
                                   : nullptr;
        PWSTR explicitId = nullptr;
        if (getExplicit && SUCCEEDED(getExplicit(&explicitId)) && explicitId) {
            wcscpy_s(appId, 256, explicitId);
        }
        CoTaskMemFree(explicitId);
    }

    WCHAR module[MAX_PATH * 2];
    DWORD len = GetModuleFileNameW(nullptr, module, (DWORD)(sizeof(module) / sizeof(module[0])));
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

static WCHAR* RegistryValueDup(HKEY root, const WCHAR* keyPath, const WCHAR* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, keyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return nullptr;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS status = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
    WCHAR* result = nullptr;
    if (status == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(WCHAR)) {
        result = (WCHAR*)malloc((size_t)bytes + sizeof(WCHAR));
        if (result && RegQueryValueExW(key, valueName, nullptr, &type, (BYTE*)result, &bytes) ==
                          ERROR_SUCCESS) {
            result[bytes / sizeof(WCHAR)] = 0;
        } else {
            free(result);
            result = nullptr;
        }
    } else if (status == ERROR_SUCCESS && type == REG_DWORD && bytes == sizeof(DWORD)) {
        DWORD value = 0;
        if (RegQueryValueExW(key, valueName, nullptr, &type, (BYTE*)&value, &bytes) ==
            ERROR_SUCCESS) {
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
        swprintf_s(key, L"Software\\Policies\\Microsoft\\Edge\\WebView2\\%s", property);
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
                       L"Software\\Policies\\Microsoft\\EmbeddedBrowserWebView\\LoaderOverride\\%s",
                       ids[i]);
            WCHAR* result = RegistryValueDup(roots[r], key, property);
            if (result) {
                return result;
            }
        }
    }
    return nullptr;
}

static WCHAR* LoaderOverrideDup(const WCHAR* environment, const WCHAR* property) {
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
            getDpiForWindow = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
        }
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) {
            getDpiForMonitor = (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor");
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

static bool RegReadStr(HKEY root, const WCHAR* subKey, const WCHAR* name, DWORD extraFlags,
                       WCHAR* out, DWORD outChars) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | extraFlags, &key) != ERROR_SUCCESS) {
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
    auto getSize =
        (GetFileVersionInfoSizeWFn)GetProcAddress(versionDll, "GetFileVersionInfoSizeW");
    auto getInfo = (GetFileVersionInfoWFn)GetProcAddress(versionDll, "GetFileVersionInfoW");
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
                infoSize >= sizeof(*info) && info->dwSignature == VS_FFI_SIGNATURE) {
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

static bool RuntimeVersionAndLocation(const WCHAR* id, WCHAR* version, DWORD versionChars,
                                       WCHAR* location, DWORD locationChars) {
    struct Where {
        HKEY root;
        const WCHAR* path;
        DWORD flags;
    };
    const Where places[] = {
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\", KEY_WOW64_32KEY},
        {HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\", 0},
    };
    for (int i = 0; i < (int)(sizeof(places) / sizeof(places[0])); i++) {
        WCHAR key[256];
        wcscpy_s(key, places[i].path);
        wcscat_s(key, id);
        if (!RegReadStr(places[i].root, key, L"pv", places[i].flags, version, versionChars)) {
            continue;
        }

        if (wcscmp(version, L"0.0.0.0") == 0) {
            continue;
        }
        if (RegReadStr(places[i].root, key, L"location", places[i].flags, location,
                       locationChars)) {
            return true;
        }
    }
    return false;
}

static bool FindPackagedRuntime(const RuntimeChannel* channel, RuntimeInfo* out) {
    typedef LONG(WINAPI * TryCreatePackageDependencyFn)(
        PSID user, PCWSTR packageFamilyName, uint64_t minVersion, int architectures,
        int lifetimeKind, PCWSTR lifetimeArtifact, int options, PWSTR* dependencyId);
    typedef LONG(WINAPI * AddPackageDependencyFn)(PCWSTR dependencyId, int rank, int options,
                                                   void** context, PWSTR* packageFullName);
    typedef LONG(WINAPI * GetPackagePathByFullNameFn)(PCWSTR packageFullName,
                                                       UINT32* pathLength, PWSTR path);

    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE apiModule = kernelBase ? kernelBase : kernel32;
    auto tryCreate = apiModule ? (TryCreatePackageDependencyFn)GetProcAddress(
                                     apiModule, "TryCreatePackageDependency")
                               : nullptr;
    auto add = apiModule
                   ? (AddPackageDependencyFn)GetProcAddress(apiModule, "AddPackageDependency")
                   : nullptr;
    auto getPath = kernel32 ? (GetPackagePathByFullNameFn)GetProcAddress(
                                  kernel32, "GetPackagePathByFullName")
                            : nullptr;
    if (!tryCreate || !add || !getPath) {
        return false;
    }

    PWSTR dependencyId = nullptr;
    LONG status = tryCreate(nullptr, channel->packageFamily, 0, 0, 0, nullptr, 0,
                            &dependencyId);
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
    if (written < 0 || GetFileAttributesW(out->clientDll) == INVALID_FILE_ATTRIBUTES ||
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

static bool FindInstalledRuntime(const RuntimeChannel* channel, RuntimeInfo* out) {
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
        if (!RegReadStr(places[i].root, key, L"EBWebView", places[i].flags, folder,
                         (DWORD)(sizeof(folder) / sizeof(folder[0])))) {
            continue;
        }
        const WCHAR* version = wcsrchr(folder, L'\\');
        version = version ? version + 1 : folder;
        if (!IsCompatibleInstalledRuntime(version)) {
            continue;
        }
        int written = swprintf_s(out->clientDll,
                                 L"%s\\EBWebView\\%s\\EmbeddedBrowserWebView.dll", folder,
                                 ArchFolder());
        if (written < 0 || GetFileAttributesW(out->clientDll) == INVALID_FILE_ATTRIBUTES) {
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
    if (!RuntimeVersionAndLocation(channel->id, version,
                                    (DWORD)(sizeof(version) / sizeof(version[0])), location,
                                    (DWORD)(sizeof(location) / sizeof(location[0])))) {
        return FindPackagedRuntime(channel, out);
    }
    if (!IsCompatibleInstalledRuntime(version)) {
        return FindPackagedRuntime(channel, out);
    }
    int written = swprintf_s(out->clientDll,
                             L"%s\\%s\\EBWebView\\%s\\EmbeddedBrowserWebView.dll", location,
                             version, ArchFolder());
    if (written < 0 || GetFileAttributesW(out->clientDll) == INVALID_FILE_ATTRIBUTES) {
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
    WCHAR* value =
        LoaderOverrideDup(L"WEBVIEW2_CHANNEL_SEARCH_KIND", L"ChannelSearchKind");
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

    WCHAR* folder = useOverrides
                        ? LoaderOverrideDup(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",
                                            L"BrowserExecutableFolder")
                        : nullptr;
    if (folder && folder[0] != 0) {

        WCHAR* resolvedFolder = FixedRuntimeFolderDup(folder);
        free(folder);
        if (!resolvedFolder) {
            return false;
        }
        out->runtimeType = 1;
        int written = swprintf_s(out->clientDll, L"%s\\EBWebView\\%s\\EmbeddedBrowserWebView.dll",
                                 resolvedFolder, ArchFolder());
        if (written < 0 || GetFileAttributesW(out->clientDll) == INVALID_FILE_ATTRIBUTES) {
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
        SUCCEEDED(options->QueryInterface(__uuidof(ICoreWebView2EnvironmentOptions7),
                                          (void**)&options7))) {
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

typedef HRESULT(STDMETHODCALLTYPE* CreateWebViewEnvironmentWithOptionsInternalFn)(
    BOOL fromClientDll, int runtimeType, PCWSTR userDataFolder, IUnknown* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);

struct EnvironmentCreatedRetryHandler
    : ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    LONG refs = 1;
    CreateWebViewEnvironmentWithOptionsInternalFn create = nullptr;
    int runtimeType = 0;
    WCHAR* userDataFolder = nullptr;
    IUnknown* options = nullptr;
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* original = nullptr;
    int retries = 1;

    ~EnvironmentCreatedRetryHandler() {
        free(userDataFolder);
        if (options) {
            options->Release();
        }
        if (original) {
            original->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown ||
            riid == __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
            *ppv = (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*)this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG left = InterlockedDecrement(&refs);
        if (left == 0) {
            delete this;
        }
        return (ULONG)left;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                     ICoreWebView2Environment* environment) override {
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
        DefaultUserDataFolder(defaultFolder, (int)(sizeof(defaultFolder) / sizeof(WCHAR)));
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
        logf("wry: the WebView2 client dll has no CreateWebViewEnvironmentWithOptionsInternal\n");
        FreeLibrary(client);
        free(userDataOverride);
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
    EnvironmentCreatedRetryHandler* retry = new EnvironmentCreatedRetryHandler();
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
                     ? create(TRUE, rt.runtimeType, retry->userDataFolder, options, retry)
                     : E_OUTOFMEMORY;

    if (FAILED(hr) && retry->userDataFolder) {
        hr = create(TRUE, rt.runtimeType, retry->userDataFolder, options, retry);
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

template <typename I>
struct ComObj : I {
    LONG refs = 1;

    virtual ~ComObj() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == __uuidof(I)) {
            *ppv = (I*)this;
            this->AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG left = InterlockedDecrement(&refs);
        if (left == 0) {
            delete this;
        }
        return (ULONG)left;
    }
};

template <typename I, typename A1, typename A2>
struct Handler2 : ComObj<I> {
    void* ctx = nullptr;
    HRESULT (*fn)(void* ctx, A1 a1, A2 a2) = nullptr;
    void (*dropCtx)(void* ctx) = nullptr;

    ~Handler2() override {
        if (dropCtx) {
            dropCtx(ctx);
        }
    }

    HRESULT STDMETHODCALLTYPE Invoke(A1 a1, A2 a2) override { return fn(ctx, a1, a2); }
};

template <typename I, typename A1>
struct Handler1 : ComObj<I> {
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

struct DownloadStateHandler : ComObj<ICoreWebView2StateChangedEventHandler> {
    DownloadCallbackState* callback = nullptr;
    LONG fired = 0;

    ~DownloadStateHandler() override {
        if (callback) {
            callback->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2DownloadOperation* operation, IUnknown*) override {
        if (!operation || !callback ||
            InterlockedCompareExchange(&callback->alive, 0, 0) == 0 || !callback->fn) {
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

static void FreeDropPaths(Vec<Str>* paths) {
    for (int i = 0; i < paths->len; i++) {
        StrFree(paths->els[i]);
    }
    VecReset(*paths);
}

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

struct DragDropTarget : ComObj<IDropTarget> {
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
        if (!enterIsValid) {
            return S_OK;
        }
        Emit(DragDropKind::Enter, &paths, point);
        FreeDropPaths(&paths);
        cursorEffect = DROPEFFECT_COPY;
        if (effect) {
            *effect = cursorEffect;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
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
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL point, DWORD*) override {
        if (enterIsValid) {
            Vec<Str> paths;
            HDROP hdrop = nullptr;
            GetDropPaths(data, &paths, &hdrop);
            Emit(DragDropKind::Drop, &paths, point);
            FreeDropPaths(&paths);
            if (hdrop) {
                DragFinish(hdrop);
            }
        }
        return S_OK;
    }
};

struct DragDropController {
    Vec<DragDropTarget*> targets;

    ~DragDropController() {
        for (int i = 0; i < targets.len; i++) {
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

    DragDropEnumCtx* ctx = reinterpret_cast<DragDropEnumCtx*>(param);
    DragDropTarget* target = new DragDropTarget();
    target->hwnd = hwnd;
    target->ctx = ctx->handlerCtx;
    target->fn = ctx->handler;
    HRESULT revoked = RevokeDragDrop(hwnd);
    if (revoked != DRAGDROP_E_INVALIDHWND && SUCCEEDED(RegisterDragDrop(hwnd, target))) {
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
            PostQuitMessage((int)msg.wParam);
            return;
        }
        if (got == -1) {
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static bool DispatchToWindow(HWND hwnd, Func0 task) {
    Func0* posted = new Func0(task);
    if (!PostMessageW(hwnd, ExecMsgId(), (WPARAM)posted, 0)) {
        logf("wry: PostMessage failed; is the message queue full?\n");
        delete posted;
        return false;
    }
    return true;
}

static LRESULT CALLBACK MainThreadDispatcherProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                                 UINT_PTR, DWORD_PTR) {
    if (msg == ExecMsgId()) {
        Func0* task = (Func0*)wp;
        task->Call();
        delete task;
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INTERNALPAINT);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

struct EnvironmentOptions : ICoreWebView2EnvironmentOptions,
                            ICoreWebView2EnvironmentOptions2,
                            ICoreWebView2EnvironmentOptions3,
                            ICoreWebView2EnvironmentOptions4,
                            ICoreWebView2EnvironmentOptions5,
                            ICoreWebView2EnvironmentOptions6,
                            ICoreWebView2EnvironmentOptions7,
                            ICoreWebView2EnvironmentOptions8 {
    LONG refs = 1;
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

    ~EnvironmentOptions() {
        free(additionalBrowserArguments);
        free(language);
        free(targetCompatibleBrowserVersion);
        for (int i = 0; i < customSchemeRegistrations.len; i++) {
            customSchemeRegistrations[i]->Release();
        }
        VecReset(customSchemeRegistrations);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == __uuidof(ICoreWebView2EnvironmentOptions)) {
            *ppv = (ICoreWebView2EnvironmentOptions*)this;
        } else if (riid == __uuidof(ICoreWebView2EnvironmentOptions2)) {
            *ppv = (ICoreWebView2EnvironmentOptions2*)this;
        } else if (riid == __uuidof(ICoreWebView2EnvironmentOptions3)) {
            *ppv = (ICoreWebView2EnvironmentOptions3*)this;
        } else if (riid == __uuidof(ICoreWebView2EnvironmentOptions4)) {
            *ppv = (ICoreWebView2EnvironmentOptions4*)this;
        } else if (riid == __uuidof(ICoreWebView2EnvironmentOptions5)) {
            *ppv = (ICoreWebView2EnvironmentOptions5*)this;
        } else if (riid == __uuidof(ICoreWebView2EnvironmentOptions6)) {
            *ppv = (ICoreWebView2EnvironmentOptions6*)this;
        } else if (riid == __uuidof(ICoreWebView2EnvironmentOptions7)) {
            *ppv = (ICoreWebView2EnvironmentOptions7*)this;
        } else if (riid == __uuidof(ICoreWebView2EnvironmentOptions8)) {
            *ppv = (ICoreWebView2EnvironmentOptions8*)this;
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG left = InterlockedDecrement(&refs);
        if (left == 0) {
            delete this;
        }
        return (ULONG)left;
    }

    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = CoTaskMemDupW(additionalBrowserArguments);
        return *value ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR value) override {
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
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = CoTaskMemDupW(targetCompatibleBrowserVersion);
        return *value ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR value) override {
        WCHAR* copy = WStrDup(value ? value : L"");
        if (!copy) {
            return E_OUTOFMEMORY;
        }
        free(targetCompatibleBrowserVersion);
        targetCompatibleBrowserVersion = copy;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* allow) override {
        if (!allow) {
            return E_POINTER;
        }
        *allow = allowSingleSignOn;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL allow) override {
        allowSingleSignOn = allow;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_ExclusiveUserDataFolderAccess(BOOL* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = exclusiveUserDataFolderAccess;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_ExclusiveUserDataFolderAccess(BOOL value) override {
        exclusiveUserDataFolderAccess = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_IsCustomCrashReportingEnabled(BOOL* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = customCrashReportingEnabled;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_IsCustomCrashReportingEnabled(BOOL value) override {
        customCrashReportingEnabled = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCustomSchemeRegistrations(UINT32* count,
                                                            IUnknown*** values) override {
        if (!count || !values) {
            return E_POINTER;
        }
        *count = (UINT32)customSchemeRegistrations.len;
        *values = nullptr;
        if (customSchemeRegistrations.len == 0) {
            return S_OK;
        }
        IUnknown** copy = (IUnknown**)CoTaskMemAlloc(sizeof(IUnknown*) * (size_t)*count);
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
    HRESULT STDMETHODCALLTYPE SetCustomSchemeRegistrations(UINT32 count,
                                                            IUnknown** values) override {
        if (count > 0 && !values) {
            return E_POINTER;
        }
        Vec<IUnknown*> copy;
        for (UINT32 i = 0; i < count; i++) {
            if (!values[i] || !VecAppend(copy, values[i])) {
                for (int j = 0; j < copy.len; j++) {
                    copy[j]->Release();
                }
                VecReset(copy);
                return values[i] ? E_OUTOFMEMORY : E_POINTER;
            }
            values[i]->AddRef();
        }
        for (int i = 0; i < customSchemeRegistrations.len; i++) {
            customSchemeRegistrations[i]->Release();
        }
        VecReset(customSchemeRegistrations);
        customSchemeRegistrations = copy;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_EnableTrackingPrevention(BOOL* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = trackingPreventionEnabled;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_EnableTrackingPrevention(BOOL value) override {
        trackingPreventionEnabled = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_AreBrowserExtensionsEnabled(BOOL* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = browserExtensionsEnabled;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AreBrowserExtensionsEnabled(BOOL value) override {
        browserExtensionsEnabled = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_ChannelSearchKind(COREWEBVIEW2_CHANNEL_SEARCH_KIND* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = channelSearchKind;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_ChannelSearchKind(COREWEBVIEW2_CHANNEL_SEARCH_KIND value) override {
        channelSearchKind = value;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ReleaseChannels(COREWEBVIEW2_RELEASE_CHANNELS* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = releaseChannels;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_ReleaseChannels(COREWEBVIEW2_RELEASE_CHANNELS value) override {
        releaseChannels = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_ScrollBarStyle(COREWEBVIEW2_SCROLLBAR_STYLE* value) override {
        if (!value) {
            return E_POINTER;
        }
        *value = scrollBarStyle;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_ScrollBarStyle(COREWEBVIEW2_SCROLLBAR_STYLE value) override {
        scrollBarStyle = value;
        return S_OK;
    }
};

struct ProtocolCopy {
    Str name;
    void* ctx;
    void (*handler)(void* ctx, Str id, const Request* request, RequestResponder* responder);
};

struct WebViewEventState;

struct WebView {
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

    void* ctx = nullptr;
    void (*ipcHandler)(void* ctx, Str url, Str body) = nullptr;
    bool (*navigationHandler)(void* ctx, Str url) = nullptr;
    void (*documentTitleChangedHandler)(void* ctx, Str title) = nullptr;
    void (*onPageLoadHandler)(void* ctx, PageLoadEvent event, Str url) = nullptr;
    DownloadStartedHandler downloadStartedHandler = nullptr;
    DownloadCompletedHandler downloadCompletedHandler = nullptr;
    DownloadCallbackState* downloadCallbacks = nullptr;
    DragDropController* dragDropController = nullptr;
    bool oleInitialized = false;
    NewWindowResponse (*newWindowReqHandler)(void* ctx, Str url,
                                             const NewWindowFeatures* features,
                                             WebView** createdWebView) = nullptr;

    Vec<ProtocolCopy> protocols;

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
    HRESULT hr = webview->QueryInterface(__uuidof(ICoreWebView2_13), (void**)&wv13);
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

static bool SetBackgroundColor(ICoreWebView2Controller* controller, Rgba color) {
    ICoreWebView2Controller2* c2 = nullptr;
    if (FAILED(controller->QueryInterface(__uuidof(ICoreWebView2Controller2), (void**)&c2))) {
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

static bool SetWebViewSettings(ICoreWebView2* webview, const WebViewAttributes* attrs) {
    ICoreWebView2Settings* settings = nullptr;
    if (FAILED(webview->get_Settings(&settings)) || !settings) {
        return false;
    }
    HRESULT hr = settings->put_IsStatusBarEnabled(FALSE);
    if (SUCCEEDED(hr)) {
        hr = settings->put_AreDefaultContextMenusEnabled(attrs->defaultContextMenus ? TRUE : FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = settings->put_IsZoomControlEnabled(attrs->zoomHotkeysEnabled ? TRUE : FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = settings->put_AreDevToolsEnabled(attrs->devtools ? TRUE : FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = settings->put_IsScriptEnabled(attrs->javascriptDisabled ? FALSE : TRUE);
    }

    if (SUCCEEDED(hr) && attrs->userAgent.s) {
        ICoreWebView2Settings2* s2 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings2), (void**)&s2))) {
            hr = s2->put_UserAgent(ToCWstrTemp(attrs->userAgent));
            Rel(&s2);
        }
    }
    if (SUCCEEDED(hr) && !attrs->browserAcceleratorKeys) {
        ICoreWebView2Settings3* s3 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings3), (void**)&s3))) {
            hr = s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
            Rel(&s3);
        }
    }
    if (SUCCEEDED(hr)) {
        ICoreWebView2Settings5* s5 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings5), (void**)&s5))) {
            hr = s5->put_IsPinchZoomEnabled(attrs->zoomHotkeysEnabled ? TRUE : FALSE);
            Rel(&s5);
        }
    }
    if (SUCCEEDED(hr)) {
        ICoreWebView2Settings6* s6 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings6), (void**)&s6))) {
            hr = s6->put_IsSwipeNavigationEnabled(attrs->backForwardNavigationGestures ? TRUE
                                                                                       : FALSE);
            Rel(&s6);
        }
    }
    if (SUCCEEDED(hr)) {
        ICoreWebView2Settings9* s9 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings9), (void**)&s9))) {
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
                        SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER) != 0;
}

static bool ResizeToParent(WebView* wv) {
    int w = 0;
    int h = 0;
    if (!ParentBounds(wv->parent, &w, &h)) {
        return false;
    }
    return SetBoundsInner(wv, w, h, 0, 0);
}

static LRESULT CALLBACK ParentSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
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
                    if (SUCCEEDED(controller->get_ParentWindow(&child)) && child) {
                        SetWindowPos(child, nullptr, 0, 0, w, h,
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

                    SetWindowSubclass(hwnd, ParentSubclassProc, kParentSubclassId, 0);
                }
            }
            break;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void AttachParentSubclass(HWND parent, ICoreWebView2Controller* controller) {
    controller->AddRef();
    SetWindowSubclass(parent, ParentSubclassProc, kParentSubclassId, (DWORD_PTR)controller);
}

static void DetachParentSubclass(HWND parent) {
    SendMessageW(parent, kParentDestroyMessage, 0, 0);
    RemoveWindowSubclass(parent, ParentSubclassProc, kParentSubclassId);
}

static LRESULT CALLBACK ContainerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SETFOCUS) {

        HWND child = GetWindow(hwnd, GW_CHILD);
        if (child) {
            SetFocus(child);
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreateContainerHwnd(HWND parent, const WebViewAttributes* attrs, bool isChild) {
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
            x = ToPhysical(attrs->bounds.position.x, attrs->bounds.position.logical, scale);
            y = ToPhysical(attrs->bounds.position.y, attrs->bounds.position.logical, scale);
            w = ToPhysical(attrs->bounds.size.width, attrs->bounds.size.logical, scale);
            h = ToPhysical(attrs->bounds.size.height, attrs->bounds.size.logical, scale);
        } else {
            x = CW_USEDEFAULT;
            y = CW_USEDEFAULT;
            w = CW_USEDEFAULT;
            h = CW_USEDEFAULT;
        }
    } else if (!ParentBounds(parent, &w, &h)) {
        return nullptr;
    }

    HWND hwnd = CreateWindowExW(0, kClassName, nullptr, style, x, y, w, h, parent, nullptr, inst,
                                nullptr);
    if (!hwnd) {
        logf("wry: CreateWindowEx for the webview container failed, error %d\n",
             (int)GetLastError());
        return nullptr;
    }
    if (!SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                      SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOMOVE |
                          SWP_NOOWNERZORDER | SWP_NOSIZE)) {
        logf("wry: positioning the webview container failed, error %d\n", (int)GetLastError());
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

static ICoreWebView2Environment* CreateEnvironment(const WebViewAttributes* attrs) {
    EnvironmentOptions* options = new EnvironmentOptions();

    if (attrs->additionalBrowserArgs.s) {
        options->additionalBrowserArguments = WStrDupUtf8(attrs->additionalBrowserArgs);
    } else {
        base::StrBuilder args;
        args.Append(StrL("--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection"));
        if (attrs->autoplay) {
            args.Append(StrL(" --autoplay-policy=no-user-gesture-required"));
        }
        if (attrs->proxyConfig.kind != ProxyKind::None) {
            const char* scheme =
                attrs->proxyConfig.kind == ProxyKind::Http ? "http://" : "socks5://";
            args.Append(base::FormatTemp(" --proxy-server=%s%s:%s", Str(scheme),
                                         attrs->proxyConfig.host, attrs->proxyConfig.port));
        }
        options->additionalBrowserArguments = WStrDupUtf8(args.TakeStr());
    }

    WCHAR* browserArgumentsOverride = LoaderOverrideDup(
        L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", L"AdditionalBrowserArguments");
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

    options->browserExtensionsEnabled = attrs->browserExtensionsEnabled ? TRUE : FALSE;
    options->scrollBarStyle = attrs->scrollBarStyle == ScrollBarStyle::FluentOverlay
                                  ? kScrollBarStyleFluentOverlay
                                  : kScrollBarStyleDefault;

    WCHAR lang[LOCALE_NAME_MAX_LENGTH];
    lang[0] = 0;
    LANGID lcid = GetUserDefaultUILanguage();
    if (LCIDToLocaleName(lcid, lang, LOCALE_NAME_MAX_LENGTH, LOCALE_ALLOW_NEUTRAL_NAMES) > 0) {
        options->language = WStrDup(lang);
    }

    EnvWait* wait = new EnvWait();
    auto* handler =
        MkHandler<Handler2<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT,
                           ICoreWebView2Environment*>>(
            wait, [](void* ctx, HRESULT code, ICoreWebView2Environment* env) -> HRESULT {
                EnvWait* w = (EnvWait*)ctx;
                if (SUCCEEDED(code) && env) {
                    env->AddRef();
                    w->env = env;
                } else {
                    logf("wry: creating the WebView2 environment failed, hr 0x%x\n", (int)code);
                }
                w->done = true;
                return S_OK;
            }, ReleaseWaitState<EnvWait>);

    PCWSTR dataDirectory = attrs->dataDirectory.s ? ToCWstrTemp(attrs->dataDirectory) : nullptr;

    IUnknown* optionsUnknown = static_cast<ICoreWebView2EnvironmentOptions*>(options);
    HRESULT hr = CreateEnvironmentWithOptions(dataDirectory, optionsUnknown, handler);
    handler->Release();
    options->Release();
    if (FAILED(hr)) {
        logf("wry: CreateCoreWebView2EnvironmentWithOptions failed, hr 0x%x\n", (int)hr);
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

static ICoreWebView2Controller* CreateController(HWND hwnd, ICoreWebView2Environment* env,
                                                 bool incognito, const Rgba* backgroundColor) {
    ControllerWait* wait = new ControllerWait();
    auto* handler =
        MkHandler<Handler2<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT,
                           ICoreWebView2Controller*>>(
            wait, [](void* ctx, HRESULT code, ICoreWebView2Controller* controller) -> HRESULT {
                ControllerWait* w = (ControllerWait*)ctx;
                if (SUCCEEDED(code) && controller) {
                    controller->AddRef();
                    w->controller = controller;
                } else {
                    logf("wry: creating the WebView2 controller failed, hr 0x%x\n", (int)code);
                }
                w->done = true;
                return S_OK;
            }, ReleaseWaitState<ControllerWait>);

    HRESULT hr = E_FAIL;
    ICoreWebView2Environment10* env10 = nullptr;
    if (SUCCEEDED(env->QueryInterface(__uuidof(ICoreWebView2Environment10), (void**)&env10))) {
        ICoreWebView2ControllerOptions* opts = nullptr;
        hr = env10->CreateCoreWebView2ControllerOptions(&opts);
        if (SUCCEEDED(hr) && !opts) {
            hr = E_POINTER;
        }
        if (SUCCEEDED(hr) && backgroundColor) {
            ICoreWebView2ControllerOptions3* opts3 = nullptr;
            if (SUCCEEDED(opts->QueryInterface(__uuidof(ICoreWebView2ControllerOptions3),
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
            hr = env10->CreateCoreWebView2ControllerWithOptions(hwnd, opts, handler);
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

static bool AddScriptToExecuteOnDocumentCreated(ICoreWebView2* webview, Str js) {
    ScriptWait* wait = new ScriptWait();
    auto* handler = MkHandler<
        Handler2<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler, HRESULT,
                 LPCWSTR>>(wait, [](void* ctx, HRESULT code, LPCWSTR) -> HRESULT {
        ScriptWait* wait = (ScriptWait*)ctx;
        wait->result = code;
        wait->done = true;
        return S_OK;
    }, ReleaseWaitState<ScriptWait>);
    HRESULT hr = webview->AddScriptToExecuteOnDocumentCreated(ToCWstrTemp(js), handler);
    handler->Release();
    if (FAILED(hr)) {
        logf("wry: AddScriptToExecuteOnDocumentCreated failed, hr 0x%x\n", (int)hr);
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
    auto* handler =
        MkHandler<Handler2<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>>(
            held, [](void* ctx, HRESULT code, LPCWSTR result) -> HRESULT {
                EvalCallback* c = (EvalCallback*)ctx;
                if (c->fn) {
                    c->fn(c->ctx, SUCCEEDED(code) ? WstrToUtf8Temp(result) : Str());
                }
                return S_OK;
            }, DeleteEvalCallback);
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
        case 100: return L"Continue";
        case 101: return L"Switching Protocols";
        case 102: return L"Processing";
        case 103: return L"Early Hints";
        case 200: return L"OK";
        case 201: return L"Created";
        case 202: return L"Accepted";
        case 203: return L"Non Authoritative Information";
        case 204: return L"No Content";
        case 205: return L"Reset Content";
        case 206: return L"Partial Content";
        case 207: return L"Multi-Status";
        case 208: return L"Already Reported";
        case 226: return L"IM Used";
        case 300: return L"Multiple Choices";
        case 301: return L"Moved Permanently";
        case 302: return L"Found";
        case 303: return L"See Other";
        case 304: return L"Not Modified";
        case 305: return L"Use Proxy";
        case 307: return L"Temporary Redirect";
        case 308: return L"Permanent Redirect";
        case 400: return L"Bad Request";
        case 401: return L"Unauthorized";
        case 402: return L"Payment Required";
        case 403: return L"Forbidden";
        case 404: return L"Not Found";
        case 405: return L"Method Not Allowed";
        case 406: return L"Not Acceptable";
        case 407: return L"Proxy Authentication Required";
        case 408: return L"Request Timeout";
        case 409: return L"Conflict";
        case 410: return L"Gone";
        case 411: return L"Length Required";
        case 412: return L"Precondition Failed";
        case 413: return L"Payload Too Large";
        case 414: return L"URI Too Long";
        case 415: return L"Unsupported Media Type";
        case 416: return L"Range Not Satisfiable";
        case 417: return L"Expectation Failed";
        case 418: return L"I'm a teapot";
        case 421: return L"Misdirected Request";
        case 422: return L"Unprocessable Entity";
        case 423: return L"Locked";
        case 424: return L"Failed Dependency";
        case 425: return L"Too Early";
        case 426: return L"Upgrade Required";
        case 428: return L"Precondition Required";
        case 429: return L"Too Many Requests";
        case 431: return L"Request Header Fields Too Large";
        case 451: return L"Unavailable For Legal Reasons";
        case 500: return L"Internal Server Error";
        case 501: return L"Not Implemented";
        case 502: return L"Bad Gateway";
        case 503: return L"Service Unavailable";
        case 504: return L"Gateway Timeout";
        case 505: return L"HTTP Version Not Supported";
        case 506: return L"Variant Also Negotiates";
        case 507: return L"Insufficient Storage";
        case 508: return L"Loop Detected";
        case 510: return L"Not Extended";
        case 511: return L"Network Authentication Required";
        default: return L"OK";
    }
}

static ICoreWebView2WebResourceResponse* MakeResponse(ICoreWebView2Environment* env, int status,
                                                      Str headerBlock, const uint8_t* body,
                                                      int bodyLen) {
    IStream* stream = nullptr;
    if (body && bodyLen > 0) {
        stream = SHCreateMemStream(body, (UINT)bodyLen);
    }
    ICoreWebView2WebResourceResponse* res = nullptr;
    HRESULT hr = env->CreateWebResourceResponse(stream, status, HttpStatusReason(status),
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

static ICoreWebView2WebResourceResponse* MakeBadRequest(ICoreWebView2Environment* env,
                                                        HRESULT cause) {
    Str header = base::FormatTemp("X-Wry-Error: HRESULT 0x%08x\n", (uint32_t)cause);
    return MakeResponse(env, 400, header, nullptr, 0);
}

struct PendingResponse {
    RequestResponder* responder;
    int status;
    uint8_t* body;
    int bodyLen;

    Str headers;
};

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
        r->deferral->Release();
    }
    r->args->Release();
    r->env->Release();
    StrFree(p->headers);
    free(p->body);
    delete p;
    delete r;
}

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
            headers.Append(
                base::FormatTemp("%s: %s\n", response->headers[i].name, response->headers[i].value));
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

static HRESULT PrepareRequest(WebView* wv, ICoreWebView2WebResourceRequest* req, Str uri,
                              Str protocol, Request* out, Vec<Header>* headerStore,
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

static HRESULT OnWebResourceRequested(void* ctx, ICoreWebView2*,
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

    ProtocolCopy* found = nullptr;
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
    hr = PrepareRequest(wv, req, uri, found->name, &request, &headerStore, &bodyStore);
    Rel(&req);
    if (FAILED(hr)) {
        ICoreWebView2WebResourceResponse* response = MakeBadRequest(wv->env, hr);
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
    if (found->handler) {
        found->handler(found->ctx, wv->id, &request, responder);
    } else {
        Response response;
        response.status = 500;
        Respond(responder, &response);
    }

    VecReset(headerStore);
    VecReset(bodyStore);
    return S_OK;
}

static bool AttachCustomProtocolHandler(WebView* wv, EventRegistrationToken* token) {
    for (int i = 0; i < wv->protocols.len; i++) {
        Str filter =
            base::FormatTemp("%s*", WorkAroundUriPrefix(Str(wv->httpOrHttps), wv->protocols[i].name));
        ICoreWebView2_22* wv22 = nullptr;
        if (SUCCEEDED(wv->webview->QueryInterface(__uuidof(ICoreWebView2_22), (void**)&wv22))) {

            HRESULT hr = wv22->AddWebResourceRequestedFilterWithRequestSourceKinds(
                ToCWstrTemp(filter), kWebResourceContextAll,
                (COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS)
                    kWebResourceRequestSourceKindsAll);
            Rel(&wv22);
            if (FAILED(hr)) {
                return false;
            }
        } else {
            if (FAILED(wv->webview->AddWebResourceRequestedFilter(ToCWstrTemp(filter),
                                                                  kWebResourceContextAll))) {
                return false;
            }
        }
    }

    auto* handler =
        MkWebViewHandler<Handler2<ICoreWebView2WebResourceRequestedEventHandler,
                                  ICoreWebView2*,
                                  ICoreWebView2WebResourceRequestedEventArgs*>>(
            wv, OnWebResourceRequested);
    HRESULT hr = wv->webview->add_WebResourceRequested(handler, token);
    handler->Release();
    if (FAILED(hr)) {
        return false;
    }

    SetWindowSubclass(wv->hwnd, MainThreadDispatcherProc, kMainThreadDispatcherSubclassId, 0);
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

static HRESULT OnDocumentTitleChanged(void* ctx, ICoreWebView2* sender, IUnknown*) {
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

static HRESULT OnContentLoading(void* ctx, ICoreWebView2* sender,
                                ICoreWebView2ContentLoadingEventArgs*) {
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
        wv->onPageLoadHandler(wv->ctx, PageLoadEvent::Started, url);
    }
    return S_OK;
}

static HRESULT OnNavigationCompleted(void* ctx, ICoreWebView2* sender,
                                     ICoreWebView2NavigationCompletedEventArgs*) {
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
        wv->onPageLoadHandler(wv->ctx, PageLoadEvent::Finished, url);
    }
    return S_OK;
}

static HRESULT OnNavigationStarting(void* ctx, ICoreWebView2*,
                                    ICoreWebView2NavigationStartingEventArgs* args) {
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

static HRESULT OnNewWindowRequested(void* ctx, ICoreWebView2*,
                                    ICoreWebView2NewWindowRequestedEventArgs* args) {
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
    NewWindowResponse response = wv->newWindowReqHandler(wv->ctx, url, &features, &created);
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

static HRESULT OnPermissionRequested(void*, ICoreWebView2*,
                                     ICoreWebView2PermissionRequestedEventArgs* args) {
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

static HRESULT OnDownloadStarting(void* ctx, ICoreWebView2*,
                                  ICoreWebView2DownloadStartingEventArgs* args) {
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

static HRESULT OnWebMessageReceived(void* ctx, ICoreWebView2*,
                                    ICoreWebView2WebMessageReceivedEventArgs* args) {
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
        auto* h =
            MkWebViewHandler<Handler2<ICoreWebView2WindowCloseRequestedEventHandler,
                                      ICoreWebView2*, IUnknown*>>(wv, OnWindowCloseRequested);
        HRESULT hr = wv->webview->add_WindowCloseRequested(h, token);
        h->Release();
        if (FAILED(hr)) {
            return false;
        }
    }
    if (wv->documentTitleChangedHandler) {
        auto* h =
            MkWebViewHandler<Handler2<ICoreWebView2DocumentTitleChangedEventHandler,
                                      ICoreWebView2*, IUnknown*>>(wv, OnDocumentTitleChanged);
        HRESULT hr = wv->webview->add_DocumentTitleChanged(h, token);
        h->Release();
        if (FAILED(hr)) {
            return false;
        }
    }
    if (wv->onPageLoadHandler) {
        auto* started =
            MkWebViewHandler<Handler2<ICoreWebView2ContentLoadingEventHandler, ICoreWebView2*,
                                      ICoreWebView2ContentLoadingEventArgs*>>(wv,
                                                                             OnContentLoading);
        HRESULT hr = wv->webview->add_ContentLoading(started, token);
        started->Release();
        if (FAILED(hr)) {
            return false;
        }
        auto* finished = MkWebViewHandler<
            Handler2<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*,
                     ICoreWebView2NavigationCompletedEventArgs*>>(wv, OnNavigationCompleted);
        hr = wv->webview->add_NavigationCompleted(finished, token);
        finished->Release();
        if (FAILED(hr)) {
            return false;
        }
    }
    if (wv->navigationHandler) {
        auto* h = MkWebViewHandler<
            Handler2<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*,
                     ICoreWebView2NavigationStartingEventArgs*>>(
            wv, OnNavigationStarting);
        HRESULT hr = wv->webview->add_NavigationStarting(h, token);
        h->Release();
        if (FAILED(hr)) {
            return false;
        }
    }
    {
        auto* h = MkWebViewHandler<
            Handler2<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*,
                     ICoreWebView2NewWindowRequestedEventArgs*>>(
            wv, OnNewWindowRequested);
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
    if (FAILED(wv->webview->QueryInterface(__uuidof(ICoreWebView2_4), (void**)&webview4))) {
        logf("wry: this WebView2 runtime does not support download handlers\n");
        return false;
    }
    auto* handler =
        MkWebViewHandler<Handler2<ICoreWebView2DownloadStartingEventHandler, ICoreWebView2*,
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
            wv->webview,
            StrL("Object.defineProperty(window, 'ipc', { value: Object.freeze({ postMessage: s=> "
                 "window.chrome.webview.postMessage(s) }) });"))) {
        return false;
    }
    auto* h =
        MkWebViewHandler<Handler2<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*,
                                  ICoreWebView2WebMessageReceivedEventArgs*>>(
            wv, OnWebMessageReceived);
    HRESULT hr = wv->webview->add_WebMessageReceived(h, token);
    h->Release();
    return SUCCEEDED(hr);
}

static bool LoadUrlWithHeaders(WebView* wv, Str url, const Header* headers, int headerCount) {
    base::StrBuilder block;
    for (int i = 0; i < headerCount; i++) {
        block.Append(base::FormatTemp("%s: %s\n", headers[i].name, headers[i].value));
    }
    ICoreWebView2Environment9* env9 = nullptr;
    if (FAILED(wv->env->QueryInterface(__uuidof(ICoreWebView2Environment9), (void**)&env9))) {
        return false;
    }
    ICoreWebView2WebResourceRequest* request = nullptr;
    HRESULT hr = env9->CreateWebResourceRequest(ToCWstrTemp(url), L"GET", nullptr,
                                                ToCWstrTemp(block.TakeStr()), &request);
    Rel(&env9);

    if (FAILED(hr) || !request) {
        Rel(&request);
        return true;
    }

    ICoreWebView2_10* wv10 = nullptr;
    bool ok = SUCCEEDED(wv->webview->QueryInterface(__uuidof(ICoreWebView2_10), (void**)&wv10)) &&
              wv10 && SUCCEEDED(wv10->NavigateWithWebResourceRequest(request));
    Rel(&wv10);
    Rel(&request);
    return ok;
}

static bool LoadExtensions(ICoreWebView2* webview, Str extensionRoot) {

    if (extensionRoot.len == 0) {
        logf("wry: cannot enumerate an empty browser extension path\n");
        return false;
    }
    ICoreWebView2_13* webview13 = nullptr;
    if (FAILED(webview->QueryInterface(__uuidof(ICoreWebView2_13), (void**)&webview13))) {
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
    bool hasSeparator = rootLen > 0 && (root[rootLen - 1] == L'\\' || root[rootLen - 1] == L'/');
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
        logf("wry: cannot enumerate the browser extension directory, error %d\n",
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
            auto* handler =
                MkHandler<Handler2<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler,
                                   HRESULT, ICoreWebView2BrowserExtension*>>(
                    nullptr, [](void*, HRESULT code, ICoreWebView2BrowserExtension*) -> HRESULT {
                        if (FAILED(code)) {
                            logf("wry: loading a browser extension failed, hr 0x%x\n", (int)code);
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
            logf("wry: enumerating browser extensions failed, error %d\n", (int)GetLastError());
            ok = false;
        }
        FindClose(iter);
    }

    delete[] path;
    delete[] root;
    Rel(&profile7);
    return ok;
}

WebView* WebViewNew(void* parentWindow, const WebViewAttributes* attrs, bool asChild) {
    if (!parentWindow || !attrs) {
        return nullptr;
    }
    if (attrs->headerCount < 0 || (attrs->headerCount > 0 && !attrs->headers) ||
        attrs->initializationScriptCount < 0 ||
        (attrs->initializationScriptCount > 0 && !attrs->initializationScripts) ||
        attrs->customProtocolCount < 0 ||
        (attrs->customProtocolCount > 0 && !attrs->customProtocols)) {
        logf("wry: invalid Windows webview attribute array\n");
        return nullptr;
    }
    for (int i = 0; i < attrs->customProtocolCount; i++) {
        for (int j = 0; j < i; j++) {
            if (base::StrEq(attrs->customProtocols[i].name, attrs->customProtocols[j].name)) {
                logf("wry: duplicate custom protocol '%s'\n",
                     attrs->customProtocols[i].name);
                return nullptr;
            }
        }
    }
    HWND parent = (HWND)parentWindow;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HWND hwnd = CreateContainerHwnd(parent, attrs, asChild);
    if (!hwnd) {
        return nullptr;
    }

    ICoreWebView2Environment* env = (ICoreWebView2Environment*)attrs->webviewEnvironment;
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

    ICoreWebView2Controller* controller =
        CreateController(hwnd, env, attrs->incognito, hasBackground ? &background : nullptr);
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
    wv->ctx = attrs->ctx;
    wv->ipcHandler = attrs->ipcHandler;
    wv->navigationHandler = attrs->navigationHandler;
    wv->documentTitleChangedHandler = attrs->documentTitleChangedHandler;
    wv->onPageLoadHandler = attrs->onPageLoadHandler;
    wv->downloadStartedHandler = attrs->downloadStartedHandler;
    wv->downloadCompletedHandler = attrs->downloadCompletedHandler;
    if (attrs->downloadCompletedHandler) {
        wv->downloadCallbacks = new DownloadCallbackState();
        wv->downloadCallbacks->ctx = attrs->ctx;
        wv->downloadCallbacks->fn = attrs->downloadCompletedHandler;
    }
    wv->newWindowReqHandler = attrs->newWindowReqHandler;
    wv->httpOrHttps = attrs->useHttpsScheme ? "https" : "http";

    wv->id = attrs->id.s ? StrDup(attrs->id)
                               : StrDup(base::FormatTemp("%lld", (int64_t)(intptr_t)hwnd));
    for (int i = 0; i < attrs->customProtocolCount; i++) {
        ProtocolCopy p;
        p.name = StrDup(attrs->customProtocols[i].name);
        p.ctx = attrs->customProtocols[i].ctx;
        p.handler = attrs->customProtocols[i].handler;
        VecAppend(wv->protocols, p);
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
        if (!AddScriptToExecuteOnDocumentCreated(webview,
                                                 attrs->initializationScripts[i].script)) {
            WebViewFree(wv);
            return nullptr;
        }
    }
    if (attrs->clipboard) {
        auto* h = MkHandler<Handler2<ICoreWebView2PermissionRequestedEventHandler, ICoreWebView2*,
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
                url = ApplyUriWorkAround(url, Str(wv->httpOrHttps), wv->protocols[i].name);
                break;
            }
        }
        if (attrs->headers) {
            navigated = LoadUrlWithHeaders(wv, url, attrs->headers, attrs->headerCount);
        } else {
            navigated = SUCCEEDED(webview->Navigate(ToCWstrTemp(url)));
        }
    } else if (attrs->html.s) {
        navigated = SUCCEEDED(webview->NavigateToString(ToCWstrTemp(attrs->html)));
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
        (attrs->focused && FAILED(controller->MoveFocus(kMoveFocusReasonProgrammatic)))) {
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
        if (SUCCEEDED(controller->QueryInterface(__uuidof(ICoreWebView2Controller4),
                                                 (void**)&controller4))) {
            controller4->put_AllowExternalDrop(FALSE);
            Rel(&controller4);
        }
        wv->dragDropController = NewDragDropController(hwnd, attrs->ctx, attrs->dragDropHandler);
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
    for (int i = 0; i < wv->protocols.len; i++) {
        StrFree(wv->protocols[i].name);
    }
    VecReset(wv->protocols);
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

bool WebViewLoadUrlWithHeaders(WebView* wv, Str url, const Header* headers, int headerCount) {
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
    out->size = PhysicalSize((double)(r.right - r.left), (double)(r.bottom - r.top));
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
    if (FAILED(wv->webview->QueryInterface(__uuidof(ICoreWebView2_19), (void**)&wv19))) {
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
    if (FAILED(wv->webview->QueryInterface(__uuidof(ICoreWebView2_2), (void**)&webview2))) {
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

static constexpr int64_t kTimeMinUnixSeconds = -377705116800LL;
static constexpr int64_t kTimeMaxUnixSeconds = 253402300799LL;

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
    result.sameSite = sameSite == kCookieSameSiteStrict
                          ? CookieSameSite::Strict
                          : (sameSite == kCookieSameSiteLax ? CookieSameSite::Lax
                                                            : CookieSameSite::None);

    BOOL isSession = FALSE;
    double expires = -1;
    if (FAILED(source->get_IsSession(&isSession)) || FAILED(source->get_Expires(&expires))) {
        FreeCookieFields(&result);
        return false;
    }
    result.session = isSession != FALSE || expires == -1;
    if (!result.session) {
        result.hasExpires = CookieExpiryFromDouble(expires, &result.expiresUnixSeconds);
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
    auto* handler =
        MkHandler<Handler2<ICoreWebView2GetCookiesCompletedHandler, HRESULT,
                           ICoreWebView2CookieList*>>(
            wait, [](void* ctx, HRESULT code, ICoreWebView2CookieList* cookies) -> HRESULT {
                CookieWait* wait = (CookieWait*)ctx;
                wait->result = code;
                if (SUCCEEDED(code) && cookies) {
                    cookies->AddRef();
                    wait->cookies = cookies;
                }
                wait->done = true;
                return S_OK;
            }, ReleaseWaitState<CookieWait>);
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

static ICoreWebView2Cookie* CookieToWebView2(ICoreWebView2CookieManager* manager,
                                             const Cookie* source) {
    if (!manager || !source) {
        return nullptr;
    }
    ICoreWebView2Cookie* cookie = nullptr;
    HRESULT hr = manager->CreateCookie(ToCWstrTemp(source->name), ToCWstrTemp(source->value),
                                       ToCWstrTemp(source->domain), ToCWstrTemp(source->path),
                                       &cookie);
    if (FAILED(hr) || !cookie) {
        return nullptr;
    }
    if (source->hasMaxAge) {
        hr = cookie->put_Expires(
            (double)SaturatingCookieExpiry(UnixTimeNow(), source->maxAgeSeconds));
    } else if (source->hasExpires) {
        hr = cookie->put_Expires((double)source->expiresUnixSeconds);
    }
    if (SUCCEEDED(hr) && source->hasHttpOnly) {
        hr = cookie->put_IsHttpOnly(source->httpOnly ? TRUE : FALSE);
    }
    if (SUCCEEDED(hr) && source->hasSameSite) {
        int sameSite = source->sameSite == CookieSameSite::Strict
                           ? kCookieSameSiteStrict
                           : (source->sameSite == CookieSameSite::Lax ? kCookieSameSiteLax
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
    if (FAILED(wv->webview->QueryInterface(__uuidof(ICoreWebView2_13), (void**)&wv13))) {
        return false;
    }
    bool ok = false;
    ICoreWebView2Profile* profile = nullptr;
    if (SUCCEEDED(wv13->get_Profile(&profile)) && profile) {
        ICoreWebView2Profile2* profile2 = nullptr;
        if (SUCCEEDED(profile->QueryInterface(__uuidof(ICoreWebView2Profile2), (void**)&profile2))) {
            auto* h = MkHandler<Handler1<ICoreWebView2ClearBrowsingDataCompletedHandler, HRESULT>>(
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
