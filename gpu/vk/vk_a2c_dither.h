#pragma once
// (tameem) NVIDIA's alpha-to-coverage dither, in a fragment shader, for AMD.
//
// OoT's grass, wheat and leaves are drawn with alpha-to-coverage into single-sample targets. On the Switch 2's NVIDIA GPU
// alpha-to-coverage dithers over 2x2 pixels (NVN's nvnMultisampleStateSetAlphaToCoverageDither; GL_NV_alpha_to_coverage_
// dither_control), so a blade at alpha 0.3 still covers a pixel in four and a fading blade thins out gradually. AMD's driver
// does not dither: with one sample the coverage is all or nothing at alpha 0.5, so the blades' soft edges snap on and off as
// they sway or the camera moves (the shimmer), and the blades the game fades out below 0.5 vanish, leaving the dark ground
// under the grass showing (the black patches).
//
// patch() takes a fragment module and returns it with, before each return of its entry point, the alpha it wrote to
// location 0 compared with a 2x2 ordered-dither threshold at gl_FragCoord (1/8, 5/8, 7/8, 3/8): below it, the fragment is
// discarded. The pipeline that uses it has alpha-to-coverage off.
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace a2cd {

inline bool is_fragment(const uint32_t* w, size_t n) {
    if (n < 5 || w[0] != 0x07230203u) return false;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (!len || i + len > n) return false;
        if (op == 15 /*OpEntryPoint*/ && len >= 3) return w[i + 1] == 4 /*Fragment*/;
        if (op == 54 /*OpFunction*/) return false;
        i += len;
    }
    return false;
}

inline bool patch(const uint32_t* w, size_t n, std::vector<uint32_t>& out, std::string& why) {
    if (n < 5 || w[0] != 0x07230203u) return why = "not SPIR-V", false;
    const uint32_t version = w[1];
    uint32_t bound = w[3];
    struct Inst { size_t at; uint32_t op, len; };
    std::vector<Inst> insts;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (!len || i + len > n) return why = "bad instruction stream", false;
        insts.push_back({i, op, len});
        i += len;
    }
    // what is there
    size_t entry_inst = SIZE_MAX, first_function = SIZE_MAX, last_annotation = SIZE_MAX, first_global = SIZE_MAX;
    uint32_t entry_fn = 0;
    std::unordered_map<uint32_t, uint32_t> location, index, builtin;   // (variable -> decoration value)
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> var;   // id -> (pointer type, storage class)
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> ptr;   // id -> (storage class, pointee)
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> vec;   // id -> (component, count)
    uint32_t t_float = 0, t_uint = 0, t_bool = 0;
    for (size_t k = 0; k < insts.size(); k++) {
        const Inst& in = insts[k];
        const uint32_t* o = w + in.at + 1;
        switch (in.op) {
        case 15:
            if (o[0] == 4 && entry_inst == SIZE_MAX) entry_inst = k, entry_fn = o[1];
            break;
        case 71:   // OpDecorate
            if (in.len >= 4 && o[1] == 30) location[o[0]] = o[2];
            if (in.len >= 4 && o[1] == 32) index[o[0]] = o[2];
            if (in.len >= 4 && o[1] == 11) builtin[o[0]] = o[2];
            [[fallthrough]];
        case 72: case 73: case 74: case 75: case 332: case 5632:   // (member, groups, id, string decorations)
            if (first_function == SIZE_MAX) last_annotation = k;
            break;
        case 20: if (!t_bool) t_bool = o[0]; break;
        case 21: if (!t_uint && o[1] == 32 && o[2] == 0) t_uint = o[0]; break;
        case 22: if (!t_float && o[1] == 32 && in.len == 3) t_float = o[0]; break;
        case 23: vec[o[0]] = {o[1], o[2]}; break;
        case 32: ptr[o[0]] = {o[1], o[2]}; break;
        case 59: if (first_function == SIZE_MAX) var[o[1]] = {o[0], o[2]}; break;
        case 54: if (first_function == SIZE_MAX) first_function = k; break;
        default: break;
        }
        if (first_global == SIZE_MAX && first_function == SIZE_MAX &&
            ((in.op >= 19 && in.op <= 39) || (in.op >= 41 && in.op <= 52) || in.op == 59))
            first_global = k;
    }
    if (entry_inst == SIZE_MAX) return why = "no fragment entry point", false;
    if (first_function == SIZE_MAX || first_global == SIZE_MAX) return why = "no functions", false;
    if (!t_float) return why = "no 32-bit float type", false;
    // the colour written to location 0 (index 0): a float vec4 output
    uint32_t out0 = 0, t_vec4 = 0;
    for (const auto& [id, v] : var) {
        if (v.second != 3 /*Output*/) continue;
        auto l = location.find(id);
        if (l == location.end() || l->second != 0) continue;
        if (auto ix = index.find(id); ix != index.end() && ix->second != 0) continue;
        auto p = ptr.find(v.first);
        if (p == ptr.end()) continue;
        auto vv = vec.find(p->second.second);
        if (vv == vec.end() || vv->second.first != t_float || vv->second.second != 4) continue;
        out0 = id, t_vec4 = p->second.second;
    }
    if (!out0) return why = "no float vec4 output at location 0", false;
    uint32_t fragcoord = 0;
    for (const auto& [id, b] : builtin)
        if (b == 15 /*FragCoord*/ && var.count(id) && var[id].second == 1 /*Input*/) fragcoord = id;
    // new declarations, before the first function
    std::vector<uint32_t> decl, annot;
    const auto inst = [](std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> ops) {
        v.push_back((uint32_t)(ops.size() + 1) << 16 | op);
        v.insert(v.end(), ops);
    };
    if (!t_uint) t_uint = bound++, inst(decl, 21, {t_uint, 32, 0});
    if (!t_bool) t_bool = bound++, inst(decl, 20, {t_bool});
    const auto cu = [&](uint32_t v) { const uint32_t id = bound++; inst(decl, 43, {t_uint, id, v}); return id; };
    const auto cf = [&](float f) { uint32_t b; std::memcpy(&b, &f, 4); const uint32_t id = bound++; inst(decl, 43, {t_float, id, b}); return id; };
    const uint32_t c1 = cu(1), c2 = cu(2), c3 = cu(3), c15 = cu(15), cpat = cu(0x1320u), chalf = cf(0.5f), cquarter = cf(0.25f);
    const uint32_t p_out_float = bound++;
    inst(decl, 32, {p_out_float, 3, t_float});
    bool new_fragcoord = false;
    uint32_t t_vec4_in = t_vec4;   // (gl_FragCoord is a float vec4 too)
    if (!fragcoord) {
        const uint32_t p_in_vec4 = bound++;
        inst(decl, 32, {p_in_vec4, 1, t_vec4_in});
        fragcoord = bound++;
        inst(decl, 59, {p_in_vec4, fragcoord, 1});
        inst(annot, 71, {fragcoord, 11, 15});
        new_fragcoord = true;
    }
    // the entry point's body: before each OpReturn, the dither test
    const uint32_t kill_op = version >= 0x00010600u ? 4416u /*OpTerminateInvocation*/ : 252u /*OpKill*/;
    out.clear();
    out.reserve(n + 256);
    out.insert(out.end(), w, w + 5);
    bool in_entry = false;
    size_t returns = 0;
    for (size_t k = 0; k < insts.size(); k++) {
        const Inst& in = insts[k];
        if (k == first_global && !annot.empty() && last_annotation == SIZE_MAX) out.insert(out.end(), annot.begin(), annot.end());
        if (k == first_function) out.insert(out.end(), decl.begin(), decl.end());
        if (k == entry_inst && new_fragcoord) {   // (the new input on the entry point's interface)
            out.push_back((in.len + 1) << 16 | in.op);
            out.insert(out.end(), w + in.at + 1, w + in.at + in.len);
            out.push_back(fragcoord);
            continue;
        }
        if (in.op == 54) in_entry = w[in.at + 2] == entry_fn;
        if (in.op == 253 /*OpReturn*/ && in_entry) {
            returns++;
            const uint32_t pa = bound++, a = bound++, fc = bound++, fx = bound++, fy = bound++, ux = bound++, uy = bound++,
                           bx = bound++, by = bound++, by2 = bound++, ix = bound++, sh = bound++, nib = bound++, b = bound++,
                           bf = bound++, t0 = bound++, t = bound++, kill = bound++, lkill = bound++, lmerge = bound++;
            inst(out, 65, {p_out_float, pa, out0, c3});        // OpAccessChain
            inst(out, 61, {t_float, a, pa});                   // OpLoad
            inst(out, 61, {t_vec4_in, fc, fragcoord});
            inst(out, 81, {t_float, fx, fc, 0});               // OpCompositeExtract
            inst(out, 81, {t_float, fy, fc, 1});
            inst(out, 109, {t_uint, ux, fx});                  // OpConvertFToU
            inst(out, 109, {t_uint, uy, fy});
            inst(out, 199, {t_uint, bx, ux, c1});              // OpBitwiseAnd
            inst(out, 199, {t_uint, by, uy, c1});
            inst(out, 196, {t_uint, by2, by, c1});             // OpShiftLeftLogical
            inst(out, 197, {t_uint, ix, bx, by2});             // OpBitwiseOr
            inst(out, 196, {t_uint, sh, ix, c2});
            inst(out, 194, {t_uint, nib, cpat, sh});           // OpShiftRightLogical
            inst(out, 199, {t_uint, b, nib, c15});
            inst(out, 112, {t_float, bf, b});                  // OpConvertUToF
            inst(out, 129, {t_float, t0, bf, chalf});          // OpFAdd
            inst(out, 133, {t_float, t, t0, cquarter});        // OpFMul
            inst(out, 184, {t_bool, kill, a, t});              // OpFOrdLessThan
            inst(out, 247, {lmerge, 0});                       // OpSelectionMerge
            inst(out, 250, {kill, lkill, lmerge});             // OpBranchConditional
            inst(out, 248, {lkill});                           // OpLabel
            out.push_back(1u << 16 | kill_op);
            inst(out, 248, {lmerge});
            out.push_back(1u << 16 | 253u);
            continue;
        }
        out.insert(out.end(), w + in.at, w + in.at + in.len);
        if (k == last_annotation && !annot.empty()) out.insert(out.end(), annot.begin(), annot.end());
    }
    if (!returns) return why = "the entry point has no return", false;
    out[3] = bound;
    return true;
}

}  // namespace a2cd
