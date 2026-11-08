"""Changes to the lifted C for Ocarina of Time, applied by lift.py after it writes the code (and by hand: python -I nx2/oot_patches.py).

Each is a runtime switch where it can be, so the game's own path stays reachable.

- Dynamic resolution (Lichii's built-in "Max Resolution" patch, main+0x805BD0): the engine's config flag at +0x94 picks the path that
  follows the GPU-time controller; with hos_fixed_resolution set it takes the other path, the 3D at full scale. The controller made
  a new set of render targets at each size, ~11 images a frame at times, and held the GPU near its budget.
- GPU memory allocation (f_80522050) tried again when its heap has no room (GPU_ALLOC_ENTRY): the crash at backflip + attack.
"""
import glob
import os
import sys

DECL = "extern volatile int hos_fixed_resolution;\nextern volatile int hos_dynres_runs;\nextern volatile uint32_t hos_fixed_scale_bits;\n"
OLD = "if (!((c->x[8] >> 0) & 1)) { goto L_8082640c; }"
NEW = "if (hos_fixed_resolution || !((c->x[8] >> 0) & 1)) { goto L_8082640c; }\n    hos_dynres_runs++;"
LABEL = "L_8082640c:;"
SCALE_OLD = "c->x[9] = 0x3f800000ULL;"
SCALE_NEW = "c->x[9] = hos_fixed_scale_bits;"   # (the fixed path's scale, 1.0 as shipped)

SCENE_ENTRY = "void f_831f3200(Ctx *__restrict A64_NOESC c) {"
SCENE_SCALE_FIX = """
    // Fixed-resolution scale invalidation (experimental opt-in: HOS_FIX_RES=1).
    // The fixed controller result can request a resource refresh without
    // marking the cached scene scale dirty, leaving the last dynamic scale.
    static int hos_fix_res = -1;   /* (tameem: read once, not on every call) */
    if (hos_fix_res < 0) hos_fix_res = getenv("HOS_FIX_RES") && atoi(getenv("HOS_FIX_RES")) != 0;
    if (hos_fixed_resolution && hos_fix_res) {
        float requested, base, reference, previous;
        uint32_t u=hos_fixed_scale_bits; memcpy(&requested,&u,4);
        u=recomp_load32(c,c->x[0]+3664); memcpy(&base,&u,4);
        u=recomp_load32(c,c->x[0]+3672); memcpy(&reference,&u,4);
        u=recomp_load32(c,c->x[0]+3676); memcpy(&previous,&u,4);
        if (base > 0.0f && reference > 0.0f && requested > 0.0f && requested <= 2.0f &&
            previous != requested * reference / base) {
            uint32_t dirty=recomp_load32(c,c->x[0]+3628);
            recomp_store32(c,c->x[0]+3628,dirty|8u);
        }
    }
"""


# (tameem, 11-08) The crash at backflip + attack. The "OptimizeChangeBank" thread rebuilds a model's material block
# ("gsys::ModelResource(MatBlock)", caller main+0x456f18) and asks f_80522050 for GPU memory from the heap it names. When that
# heap has no room the game does not stop: it goes on with no memory pool (main+0x457248), and two calls later writes the
# block through it: guest address 0 (CRASH ... write, guest 0, hookshot.exe+0x19b65be0; before host/nvn_native.c answered a
# null pool, GetFlags read it). A failure is tried again after a few short waits (frees in flight: the game frees a block after
# the GPU is done with it), then in the game's root heap (its own default, f_80522050's path with no heap), and logged.
# HOS_GPU_ALLOC_RETRY=0: as shipped.
GPU_ALLOC_ENTRY = "void f_80522050(Ctx *__restrict A64_NOESC c) {"
GPU_ALLOC_GAME = "static void f_80522050_game(Ctx *__restrict A64_NOESC c) {"
GPU_ALLOC_WRAPPER = r"""
/* (tameem) f_80522050 with its failures tried again (nx2/oot_patches.py GPU_ALLOC_ENTRY). A sead heap here (vtable 0x84c172c8, the
   heap class of these): +0x20 its name, +0x28 its memory, +0x30 its size, +0x38 its parent, vtable +0x80 its free size (ExpHeap:
   the free list summed, main+0x834916d0). */
void hos_log(const char *fmt, ...);
void hos_sleep_ns(int64_t ns);
int64_t hos_now_ns(void);
static void f_80522050_game(Ctx *__restrict A64_NOESC c);
static int gpu_alloc_readable(uint64_t a) { return a >= 0x80000000ULL && a < (1ULL << 39) - 0x100; }
static int gpu_alloc_name(Ctx *c, uint64_t at, char *out, int n) {   /* (a guest C string: 1 if it reads as one) */
    int i = 0, ok = gpu_alloc_readable(at);
    for (; ok && i < n - 1; i++) {
        const char ch = (char)recomp_load8(c, at + i);
        if (!ch) break;
        if (ch < 32 || ch >= 127) ok = 0;
        out[i] = ch;
    }
    if (!ok || i < 2) i = 0, out[i++] = '?', ok = 0;
    out[i] = 0;
    return ok;
}
static uint64_t gpu_alloc_free_size(Ctx *c, uint64_t heap) {   /* (the heap's own getFreeSize, called as the game would; ~0: unknown) */
    if (!gpu_alloc_readable(heap)) return ~0ULL;
    const uint64_t vt = recomp_load64(c, heap);
    if (!gpu_alloc_readable(vt)) return ~0ULL;
    A64Fn fn = a64_lookup(recomp_load64(c, vt + 0x80));
    if (!fn) return ~0ULL;
    uint64_t x[31];
    const uint8_t n = c->n, z = c->z, cf = c->c, v = c->v;
    memcpy(x, c->x, sizeof x);
    c->x[0] = heap;
    fn(c);
    const uint64_t r = c->x[0];
    memcpy(c->x, x, sizeof x);
    c->n = n, c->z = z, c->c = cf, c->v = v;
    return r;
}
static void gpu_alloc_describe(Ctx *c, uint64_t heap, char *out, int n) {
    char name[48];
    if (!gpu_alloc_readable(heap)) { snprintf(out, n, "heap %#llx", (unsigned long long)heap); return; }
    if (!gpu_alloc_name(c, recomp_load64(c, heap + 0x20), name, sizeof name)) gpu_alloc_name(c, recomp_load64(c, heap + 0x28), name, sizeof name);
    const uint64_t free_size = gpu_alloc_free_size(c, heap);
    char free_kb[32];
    if (free_size == ~0ULL) snprintf(free_kb, sizeof free_kb, "?");
    else snprintf(free_kb, sizeof free_kb, "%llu", (unsigned long long)(free_size >> 10));
    snprintf(out, n, "heap %#llx \"%s\" (%llu KB, %s KB free)", (unsigned long long)heap, name,
             (unsigned long long)(recomp_load64(c, heap + 0x30) >> 10), free_kb);
}
static int gpu_alloc_in(Ctx *c, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t lr, uint64_t heap, uint64_t in) {
    recomp_store64(c, a1 + 8, in);
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[30] = lr;
    f_80522050_game(c);
    recomp_store64(c, a1 + 8, heap);
    return (int)(c->x[0] & 1);
}
void f_80522050(Ctx *__restrict A64_NOESC c) {
    const uint64_t a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], lr = c->x[30];
    f_80522050_game(c);
    static int on = -1;
    if (on < 0) on = !getenv("HOS_GPU_ALLOC_RETRY") || atoi(getenv("HOS_GPU_ALLOC_RETRY")) != 0;
    if (!on || !gpu_alloc_readable(a1)) return;
    const uint64_t count = recomp_load64(c, a1), heap = recomp_load64(c, a1 + 8), what = recomp_load64(c, a1 + 0x20);
    char name[64], where[160];
    if (c->x[0] & 1) {   /* (made: every 20 s, how full its heap is, to see a heap fill over a session) */
        static volatile int64_t next_ns;
        const int64_t now = hos_now_ns();
        if (now >= next_ns && heap) {
            next_ns = now + 20000000000LL;
            const uint64_t r0 = c->x[0];
            gpu_alloc_name(c, what, name, sizeof name);
            gpu_alloc_describe(c, heap, where, sizeof where);
            c->x[0] = r0;
            hos_log("oot: GPU memory: %s, last for %s", where, name);
        }
        return;
    }
    gpu_alloc_name(c, what, name, sizeof name);
    gpu_alloc_describe(c, heap, where, sizeof where);
    static volatile long told;
    const int tell = __atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 60;
    if (tell)
        hos_log("oot: GPU memory for %s, %#llx bytes from %s, failed (lr %#llx)", name, (unsigned long long)(count * a2), where,
                (unsigned long long)lr);
    int ok = 0, waits = 0;
    for (; waits < 3 && !ok; waits++) {   /* (frees in flight) */
        hos_sleep_ns(2000000);
        ok = gpu_alloc_in(c, a0, a1, a2, lr, heap, heap);
    }
    if (ok) {
        if (tell) hos_log("oot: GPU memory: made after %d waits of 2 ms", waits);
        return;
    }
    /* (the heaps it was made in: their parents up to the root, the free left in them after their children) */
    uint64_t h = heap;
    for (int up = 0; up < 8 && gpu_alloc_readable(h) && !ok; up++) {
        const uint64_t parent = recomp_load64(c, h + 0x38);
        if (!gpu_alloc_readable(parent) || parent == h) break;
        h = parent;
        const uint64_t free_size = gpu_alloc_free_size(c, h);
        if (free_size != ~0ULL && free_size < count * a2 + 0x2000) continue;
        ok = gpu_alloc_in(c, a0, a1, a2, lr, heap, h);
        if (tell) {
            gpu_alloc_describe(c, h, where, sizeof where);
            hos_log("oot: GPU memory: %s in its parent %s", ok ? "made" : "not made either", where);
        }
    }
    if (!ok) {   /* (the game's own default when no heap is named: the thread's heap, or the root) */
        ok = gpu_alloc_in(c, a0, a1, a2, lr, heap, 0);
        if (tell) hos_log("oot: GPU memory: %s in the game's default heap", ok ? "made" : "not made either");
    }
    if (!ok && tell) hos_log("oot: GPU memory: no heap had room: the game goes on without it (and crashes writing to it)");
}
"""


def apply_gpu_alloc_retry(source):
    if GPU_ALLOC_GAME in source:
        return source
    if source.count(GPU_ALLOC_ENTRY) != 1:
        raise ValueError("Expected exactly one GPU memory allocation function")
    header = '#include "a64.h"\n'
    if source.count(header) != 1:
        raise ValueError("Expected exactly one guest header")
    source = source.replace(GPU_ALLOC_ENTRY, GPU_ALLOC_GAME, 1)   # (first: the wrapper below has the same opening line)
    return source.replace(header, header + '#include <stdlib.h>\n#include <stdio.h>\n#include <string.h>\n' + GPU_ALLOC_WRAPPER, 1)


def apply_scene_scale_fix(source):
    if SCENE_SCALE_FIX in source:
        return source
    if source.count(SCENE_ENTRY) != 1:
        raise ValueError("Expected exactly one scene-resolution function")
    header = '#include "a64.h"\n'
    if source.count(header) != 1:
        raise ValueError("Expected exactly one guest header")
    source = source.replace(header, header + '#include <stdlib.h>\n' + DECL, 1)
    return source.replace(SCENE_ENTRY, SCENE_ENTRY + SCENE_SCALE_FIX, 1)


def apply(gen):
    done = 0
    for path in sorted(glob.glob(os.path.join(gen, "code_*.c"))):
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        if OLD not in s:
            continue
        s = s.replace(OLD, NEW, 1)
        at = s.index(LABEL)
        scale = s.index(SCALE_OLD, at)
        s = s[:scale] + SCALE_NEW + s[scale + len(SCALE_OLD):]
        s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + DECL, 1)
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(s)
        done += 1
        print("oot_patches: dynamic resolution switch in", os.path.basename(path))
    if done != 1:
        print("oot_patches: WARNING: the dynamic resolution branch was found %d times" % done)
    scenes = 0
    for path in sorted(glob.glob(os.path.join(gen, "code_*.c"))):
        with open(path, encoding="utf-8", newline="") as f:
            source = f.read()
        if SCENE_ENTRY not in source:
            continue
        updated = apply_scene_scale_fix(source)
        if updated != source:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(updated)
        scenes += 1
    if scenes != 1:   # (tameem: a warning, not a failed build: this game version's code may differ)
        print("oot_patches: WARNING: the scene-resolution function was found %d times: HOS_FIX_RES does nothing" % scenes)
    allocs = 0
    for path in sorted(glob.glob(os.path.join(gen, "code_*.c"))):
        with open(path, encoding="utf-8", newline="") as f:
            source = f.read()
        if GPU_ALLOC_ENTRY not in source and GPU_ALLOC_GAME not in source:
            continue
        try:
            updated = apply_gpu_alloc_retry(source)
        except ValueError as e:
            print("oot_patches: WARNING: GPU memory retry not applied in %s: %s" % (os.path.basename(path), e))
            continue
        if updated != source:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(updated)
        allocs += 1
        print("oot_patches: GPU memory allocation tried again on failure in", os.path.basename(path))
    if allocs != 1:
        print("oot_patches: WARNING: the GPU memory allocation function was found %d times: no retry" % allocs)


if __name__ == "__main__":
    apply(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work", "sa", "gen"))
