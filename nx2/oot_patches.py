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
/* (tameem) f_80522050 with its failures tried again (nx2/oot_patches.py GPU_ALLOC_ENTRY) */
void hos_log(const char *fmt, ...);
void hos_sleep_ns(int64_t ns);
static void f_80522050_game(Ctx *__restrict A64_NOESC c);
static int gpu_alloc_readable(uint64_t a) { return a >= 0x80000000ULL && a < (1ULL << 39) - 64; }
static void gpu_alloc_name(Ctx *c, uint64_t at, char *out, int n) {   /* (a guest C string, or "?") */
    int i = 0;
    if (gpu_alloc_readable(at))
        for (; i < n - 1; i++) {
            const char ch = (char)recomp_load8(c, at + i);
            if (!ch) break;
            out[i] = ch >= 32 && ch < 127 ? ch : '?';
        }
    if (!i) out[i++] = '?';
    out[i] = 0;
}
void f_80522050(Ctx *__restrict A64_NOESC c) {
    const uint64_t a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], lr = c->x[30];
    f_80522050_game(c);
    if (c->x[0] & 1) return;
    static int on = -1;
    if (on < 0) on = !getenv("HOS_GPU_ALLOC_RETRY") || atoi(getenv("HOS_GPU_ALLOC_RETRY")) != 0;
    if (!on || !gpu_alloc_readable(a1)) return;
    const uint64_t count = recomp_load64(c, a1), heap = recomp_load64(c, a1 + 8), what = recomp_load64(c, a1 + 0x20);
    int waits = 0, root = 0;
    for (; waits < 8 && !(c->x[0] & 1); waits++) {   /* (16 ms at most: a frame's frees) */
        hos_sleep_ns(2000000);
        c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[30] = lr;
        f_80522050_game(c);
    }
    uint64_t root_heap = 0;
    if (!(c->x[0] & 1) && heap) {   /* (sead's heap manager: [[GOT 0x84c9dff0]] + 8, f_80522050's own default) */
        const uint64_t got = recomp_load64(c, 0x84c9dff0ULL);
        const uint64_t mgr = gpu_alloc_readable(got) ? recomp_load64(c, got) : 0;
        root_heap = gpu_alloc_readable(mgr) ? recomp_load64(c, mgr + 8) : 0;
        if (root_heap && root_heap != heap) {
            recomp_store64(c, a1 + 8, root_heap);
            c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[30] = lr;
            f_80522050_game(c);
            recomp_store64(c, a1 + 8, heap);
            root = 1;
        }
    }
    static volatile long told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 40) {
        char name[64], heap_name[64];
        gpu_alloc_name(c, what, name, sizeof name);
        gpu_alloc_name(c, gpu_alloc_readable(heap) ? recomp_load64(c, heap + 0x28) : 0, heap_name, sizeof heap_name);
        hos_log("oot: GPU memory for %s, %#llx bytes from heap %#llx (%s), failed (lr %#llx): %s", name,
                (unsigned long long)(count * a2), (unsigned long long)heap, heap_name, (unsigned long long)lr,
                (c->x[0] & 1) ? (root ? "made in the root heap" : "made after a wait") : "failed again: the game goes on without it");
        if (root && !(c->x[0] & 1)) hos_log("oot: GPU memory: the root heap %#llx had no room either", (unsigned long long)root_heap);
        if (!root && (c->x[0] & 1)) hos_log("oot: GPU memory: made after %d waits of 2 ms", waits);
    }
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
    return source.replace(header, header + '#include <stdlib.h>\n' + GPU_ALLOC_WRAPPER, 1)


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
