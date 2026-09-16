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
        if (!base_IsDigit(fmt.format.s[off])) {
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
    while (off < f.len && base_IsDigit(f.s[off])) {
        width = (width * 10) + (f.s[off] - '0');
        off++;
    }

    int prec = -1;
    if (off < f.len && f.s[off] == '.') {
        off++;
        prec = 0;
        while (off < f.len && base_IsDigit(f.s[off])) {
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
    if (!name.s || name.len == 0) return false;
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
    if (!name.s || name.len == 0) return false;
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

static const char kEntityNames[] =
    "AMP\0AElig\0COPY\0CounterClockwiseContourIntegral\0GT\0LT\0QUOT\0REG\0"
    "amp\0apos\0cent\0copy\0euro\0gt\0hellip\0laquo\0ldquo\0lsquo\0lt\0"
    "mdash\0middot\0nbsp\0ndash\0pound\0quot\0raquo\0rdquo\0reg\0rsquo\0"
    "trade\0yen\0";
static const char kEntityValues[] =
    "&\0Æ\0©\0∳\0>\0<\0\"\0®\0&\0'\0¢\0©\0€\0>\0…\0«\0“\0‘\0<\0—\0"
    "·\0 \0–\0£\0\"\0»\0”\0®\0’\0™\0¥\0";

static Str NamedEntity(Str name) {
    int ix = SeqStrIndex(kEntityNames, name);
    return ix < 0 ? Str{} : SeqStrByIndex(kEntityValues, ix);
}

static uint32_t NumericEntity(Str value, int radix) {
    uint32_t cp = 0;
    bool any = false;
    for (int i = 0; i < value.len; i++) {
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
    for (int i = 0; i < value.len; i++) {
        if (value.s[i] == '&' || value.s[i] == '\r' || value.s[i] == 0) {
            needsDecode = true;
            break;
        }
    }
    if (!needsDecode) return ArenaStrDup(a, value);

    StrBuilder out(a);
    out.Reserve(value.len);
    for (int i = 0; i < value.len;) {
        if (value.s[i] != '&') {
            char c = value.s[i++] == '\r' ? '\n' : value.s[i - 1];
            if (c == '\n' && i < value.len && value.s[i] == '\n' &&
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
        if (i < value.len && value.s[i] == '#') {
            i++;
            int radix = 10;
            if (i < value.len && (value.s[i] == 'x' || value.s[i] == 'X')) {
                radix = 16;
                i++;
            }
            int digits = i;
            while (
                i < value.len &&
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
            if (i < value.len && value.s[i] == ';') i++;
            AppendCp(out, cp);
            continue;
        }
        int end = i;
        while (end < value.len && IsAlpha(value.s[end]) && end - i < 31) end++;
        if (end < value.len && html5ever_html5ever_IsDigit(value.s[end])) end++;
        int matched = -1;
        Str decoded = {};
        for (int n = end - i; n > 0; n--) {
            decoded = NamedEntity(Str(value.s + i, n));
            if (decoded.s) {
                matched = n;
                break;
            }
        }
        bool semi = matched > 0 && i + matched < value.len &&
                    value.s[i + matched] == ';';
        if (matched < 0 ||
            (attribute && !semi && i + matched < value.len &&
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
};

static void Emit(Scanner* s, const Token& token) {
    if (s->sink) s->sink(s->user, &token);
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
    while (s->at < s->source.len && IsSpace(s->source.s[s->at])) {
        if (s->source.s[s->at++] == '\n') s->line++;
    }
}

static ArenaStr ScanName(Scanner* s) {
    int start = s->at;
    while (s->at < s->source.len && IsNameChar(s->source.s[s->at])) s->at++;
    return LowerCopy(s->a, Str(s->source.s + start, s->at - start));
}

static Attribute* ScanAttrs(Scanner* s, bool* selfClosing) {
    Attribute* first = nullptr;
    Attribute* last = nullptr;
    *selfClosing = false;
    for (;;) {
        SkipSpace(s);
        if (s->at >= s->source.len) return first;
        char c = s->source.s[s->at];
        if (c == '>') {
            s->at++;
            return first;
        }
        if (c == '/' && s->at + 1 < s->source.len &&
            s->source.s[s->at + 1] == '>') {
            s->at += 2;
            *selfClosing = true;
            return first;
        }
        int nameStart = s->at;
        while (s->at < s->source.len && !IsSpace(s->source.s[s->at]) &&
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
        if (s->at < s->source.len && s->source.s[s->at] == '=') {
            s->at++;
            SkipSpace(s);
            int start = s->at;
            if (s->at < s->source.len &&
                (s->source.s[s->at] == '\'' || s->source.s[s->at] == '"')) {
                char quote = s->source.s[s->at++];
                start = s->at;
                while (s->at < s->source.len && s->source.s[s->at] != quote) {
                    if (s->source.s[s->at++] == '\n') s->line++;
                }
                value =
                    Decode(s->a, Str(s->source.s + start, s->at - start), true);
                if (s->at < s->source.len) s->at++;
            } else {
                while (s->at < s->source.len && !IsSpace(s->source.s[s->at]) &&
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
    for (int i = s->at; i + 2 + s->rawName.len <= s->source.len; i++) {
        if (s->source.s[i] != '<' || s->source.s[i + 1] != '/') continue;
        if (!StrEqI(Str(s->source.s + i + 2, s->rawName.len), s->rawName)) {
            continue;
        }
        int end = i + 2 + s->rawName.len;
        if (end >= s->source.len || IsSpace(s->source.s[end]) ||
            s->source.s[end] == '>') {
            return i;
        }
    }
    return s->source.len;
}

static void TokenizeRun(Scanner* s) {
    if (s->options.discardBom && s->source.len >= 3 &&
        (uint8_t)s->source.s[0] == 0xef && (uint8_t)s->source.s[1] == 0xbb &&
        (uint8_t)s->source.s[2] == 0xbf) {
        s->at = 3;
    }
    while (s->at < s->source.len) {
        if (s->rawName.s) {
            int end = FindRawClose(s);
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
                continue;
            }
            s->rawName = {};
            s->rcdata = false;
        }
        if (s->source.s[s->at] != '<') {
            int start = s->at;
            while (s->at < s->source.len && s->source.s[s->at] != '<') {
                if (s->source.s[s->at++] == '\n') s->line++;
            }
            Token text;
            text.kind = TokenKind::Character;
            text.data =
                Decode(s->a, Str(s->source.s + start, s->at - start), false);
            text.line = s->line;
            Emit(s, text);
            continue;
        }
        int tokenLine = s->line;
        if (s->at + 3 < s->source.len &&
            StrEq(Str(s->source.s + s->at, 4), StrL("<!--"))) {
            s->at += 4;
            int start = s->at;
            while (s->at + 2 < s->source.len &&
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
            if (s->at + 2 < s->source.len)
                s->at += 3;
            else
                Error(s, "eof in comment");
            Emit(s, comment);
            continue;
        }
        if (s->at + 2 < s->source.len && s->source.s[s->at + 1] == '!') {
            int start = s->at + 2;
            s->at = start;
            while (s->at < s->source.len && s->source.s[s->at] != '>') {
                s->at++;
            }
            Str body = StrTrimAscii(Str(s->source.s + start, s->at - start));
            if (s->at < s->source.len) s->at++;
            Token token;
            token.kind = TokenKind::Doctype;
            token.line = tokenLine;
            if (StrStartsWithI(body, StrL("doctype"))) {
                body = StrTrimAscii(Str(body.s + 7, body.len - 7));
                int n = 0;
                while (n < body.len && !IsSpace(body.s[n])) n++;
                token.name = LowerCopy(s->a, Str(body.s, n));
                token.forceQuirks =
                    !StrEqI(TokenName(s->a, &token), StrL("html"));
            } else {
                token.kind = TokenKind::Comment;
                token.data = ArenaStrDup(s->a, body);
            }
            Emit(s, token);
            continue;
        }
        if (s->at + 1 < s->source.len && s->source.s[s->at + 1] == '/') {
            s->at += 2;
            SkipSpace(s);
            Token token;
            token.kind = TokenKind::EndTag;
            token.name = ScanName(s);
            token.line = tokenLine;
            while (s->at < s->source.len && s->source.s[s->at] != '>') s->at++;
            if (s->at < s->source.len) s->at++;
            Emit(s, token);
            continue;
        }
        if (s->at + 1 < s->source.len && IsAlpha(s->source.s[s->at + 1])) {
            s->at++;
            Token token;
            token.kind = TokenKind::StartTag;
            token.name = ScanName(s);
            token.line = tokenLine;
            token.attrs = ArenaPtrOf(s->a, ScanAttrs(s, &token.selfClosing));
            Emit(s, token);
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
    for (int i = 0; i < value.len; i++) {
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
    if (data.len <= 0) return;
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
    if (!name.s || name.len == 0) return false;
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
    if (name.len == 2 && name.s[0] == 'h' && name.s[1] >= '1' &&
        name.s[1] <= '6') {
        for (int i = b->open.len - 1; i >= 0; i--) {
            Str n = NodeName(b->a, b->open[i]);
            if (n.len == 2 && n.s[0] == 'h' && n.s[1] >= '1' && n.s[1] <= '6') {
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
    for (int i = 0; i < reopen.len; i++) {
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
    for (int i = 0; i < value.len; i++) {
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
