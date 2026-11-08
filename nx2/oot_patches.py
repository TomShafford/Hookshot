"""Changes to the lifted C for Ocarina of Time, applied by lift.py after it writes the code (and by hand: python -I nx2/oot_patches.py).

Each is a runtime switch where it can be, so the game's own path stays reachable.

- Dynamic resolution (Lichii's built-in "Max Resolution" patch, main+0x805BD0): the engine's config flag at +0x94 picks the path that
  follows the GPU-time controller; with hos_fixed_resolution set it takes the other path, the 3D at full scale. The controller made
  a new set of render targets at each size, ~11 images a frame at times, and held the GPU near its budget.
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


if __name__ == "__main__":
    apply(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work", "sa", "gen"))
