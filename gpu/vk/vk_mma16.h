// (tameem) HMMA on 16x16x16 cooperative matrices (AMD's RDNA cards: their matrix units offer 16x16x16 f16, not the
// console's 16x8x16). The translator emits HMMA as a call to its software function (emit_spirv_warp.cpp,
// DefineSoftwareMatrixFunction: uvec2 f(uvec4 a, uvec2 b, uvec2 c), each lane's fragment words in the console's layout).
// Here that function's body is replaced in the SPIR-V, when a module is made, by one that goes through the matrix units:
// each lane writes its fragment words into the subgroup's part of a shared array (A row major, B column major, C row
// major: in the console's layout a lane's word is two neighbouring halves of one of those, so it is copied as it is),
// the subgroup loads A (16x16), B (16x16, its columns 8-15 unused) and C (16x16, likewise) as cooperative matrices,
// multiplies, stores D, and each lane reads its two D words back. Columns 8-15 of D are never read.
//
// Done on the module rather than in the translator so that a change here does not change the shader stamp (a change
// there re-translates every program and rebuilds every library: hours). Nothing here depends on anything but the words.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace mkwvk::mma16 {

enum : uint32_t {
    OpExtension = 10, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16,
    OpCapability = 17, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeArray = 28, OpTypeStruct = 30,
    OpTypePointer = 32, OpTypeFunction = 33, OpConstant = 43, OpFunction = 54, OpFunctionParameter = 55, OpFunctionEnd = 56,
    OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpDecorate = 71, OpMemberDecorate = 72,
    OpCompositeConstruct = 80, OpCompositeExtract = 81, OpIAdd = 128, OpIMul = 132, OpBitcast = 124,
    OpShiftRightLogical = 194, OpShiftLeftLogical = 196, OpBitwiseAnd = 199, OpControlBarrier = 224, OpLabel = 248,
    OpReturnValue = 254, OpExecutionModeId = 331,
    OpTypeCooperativeMatrixKHR = 4456, OpCooperativeMatrixLoadKHR = 4457, OpCooperativeMatrixStoreKHR = 4458,
    OpCooperativeMatrixMulAddKHR = 4459,
    CapFloat16 = 9, CapGroupNonUniform = 61, CapWorkgroupMemoryExplicitLayout = 4428, CapCooperativeMatrix = 6022,
    StorageInput = 1, StorageWorkgroup = 4, DecBlock = 2, DecArrayStride = 6, DecBuiltIn = 11, DecAliased = 20, DecOffset = 35,
    BuiltInSubgroupId = 40, BuiltInSubgroupLocalInvocationId = 41, ModeLocalSize = 17, ModeLocalSizeId = 38,
    ScopeSubgroup = 3, SemAcquireRelease = 0x8, SemWorkgroupMemory = 0x100, GlslPackHalf2x16 = 58, GlslUnpackHalf2x16 = 62,
};

inline void put_string(std::vector<uint32_t>& w, const char* s) {
    const size_t n = std::strlen(s) + 1, words = (n + 3) / 4;
    const size_t at = w.size();
    w.resize(at + words, 0);
    std::memcpy(&w[at], s, n);
}
inline std::string get_string(const uint32_t* w, size_t words) {
    std::string s;
    const char* c = reinterpret_cast<const char*>(w);
    for (size_t i = 0; i < words * 4 && c[i]; i++) s += c[i];
    return s;
}

/// Whether a module has the software function (a quick look over its declarations, for every module made)
inline bool has_software_function(const uint32_t* code, size_t n) {
    if (n < 5 || code[0] != 0x07230203) return false;
    std::unordered_map<uint32_t, uint32_t> vec_u32;   // (vector id -> count, of 32-bit unsigned ints)
    uint32_t u32 = 0;
    for (size_t at = 5; at < n;) {
        const uint32_t op = code[at] & 0xffff, wc = code[at] >> 16;
        if (!wc || at + wc > n) return false;
        if (op == OpFunction) return false;
        if (op == OpTypeInt && wc == 4 && code[at + 2] == 32 && code[at + 3] == 0) u32 = code[at + 1];
        if (op == OpTypeVector && wc == 4 && u32 && code[at + 2] == u32) vec_u32[code[at + 1]] = code[at + 3];
        if (op == OpTypeFunction && wc == 6) {
            const auto is = [&](uint32_t id, uint32_t count) {
                const auto it = vec_u32.find(id);
                return it != vec_u32.end() && it->second == count;
            };
            if (is(code[at + 2], 2) && is(code[at + 3], 4) && is(code[at + 4], 2) && is(code[at + 5], 2)) return true;
        }
        at += wc;
    }
    return false;
}

/// The module with every software matrix function's body replaced (false, and why, when it cannot be done: the module is
/// then used as it is). acc32: the device's 16x16x16 multiply accumulates in f32 (C and D in f32), else in f16.
/// shared_limit: the device's maxComputeSharedMemorySize.
inline bool patch(const uint32_t* code, size_t n, bool acc32, uint32_t shared_limit, std::vector<uint32_t>& out, std::string& why) {
    if (n < 5 || code[0] != 0x07230203) return why = "not SPIR-V", false;
    const uint32_t version = code[1];
    uint32_t bound = code[3];
    struct Insn { size_t at; uint32_t op, wc; };
    std::vector<Insn> insns;
    for (size_t at = 5; at < n;) {
        const uint32_t op = code[at] & 0xffff, wc = code[at] >> 16;
        if (!wc || at + wc > n) return why = "a broken instruction", false;
        insns.push_back({at, op, wc});
        at += wc;
    }
    const auto w = [&](const Insn& i, uint32_t k) { return code[i.at + k]; };

    // ---- what the module has ----
    uint32_t u32 = 0, f16 = 0, f32 = 0, glsl = 0, sli = 0, sgid = 0, fn_type = 0;
    std::unordered_map<uint32_t, uint32_t> vec_u32, vec_f32, constants;   // (vector id -> count; constant id -> value)
    std::unordered_map<uint32_t, std::vector<uint32_t>> types;            // (struct / array / pointer: their words)
    std::unordered_map<uint32_t, uint32_t> array_stride, member0_offset;
    std::vector<uint32_t> caps, workgroup_vars;
    bool has_ext = false, explicit_layout = false;
    uint32_t local[3] = {0, 0, 0}, local_ids[3] = {0, 0, 0};
    size_t first_function = insns.size(), first_global = insns.size(), last_cap = 0, last_ext = SIZE_MAX, last_annotation = SIZE_MAX;
    for (size_t k = 0; k < insns.size(); k++) {
        const Insn& i = insns[k];
        switch (i.op) {
        case OpCapability: caps.push_back(w(i, 1)), last_cap = k; if (w(i, 1) == CapWorkgroupMemoryExplicitLayout) explicit_layout = true; break;
        case OpExtension: last_ext = k; if (get_string(&code[i.at + 1], i.wc - 1) == "SPV_KHR_cooperative_matrix") has_ext = true; break;
        case OpExtInstImport: if (get_string(&code[i.at + 2], i.wc - 2) == "GLSL.std.450") glsl = w(i, 1); break;
        case OpExecutionMode:
            if (w(i, 2) == ModeLocalSize && i.wc == 6) local[0] = w(i, 3), local[1] = w(i, 4), local[2] = w(i, 5);
            break;
        case OpExecutionModeId:
            if (w(i, 2) == ModeLocalSizeId && i.wc == 6) local_ids[0] = w(i, 3), local_ids[1] = w(i, 4), local_ids[2] = w(i, 5);
            break;
        case OpDecorate:
            last_annotation = k;
            if (i.wc == 4 && w(i, 2) == DecBuiltIn && w(i, 3) == BuiltInSubgroupLocalInvocationId) sli = w(i, 1);
            if (i.wc == 4 && w(i, 2) == DecBuiltIn && w(i, 3) == BuiltInSubgroupId) sgid = w(i, 1);
            if (i.wc == 4 && w(i, 2) == DecArrayStride) array_stride[w(i, 1)] = w(i, 3);
            break;
        case OpMemberDecorate:
            last_annotation = k;
            if (i.wc == 5 && w(i, 2) == 0 && w(i, 3) == DecOffset) member0_offset[w(i, 1)] = w(i, 4);
            break;
        case 73: case 74: case 75: case 332: case 5632: case 5633: last_annotation = k; break;   // (the other annotations)
        default: break;
        }
        if (k < first_function && i.op == OpFunction) first_function = k;
        if (first_global == insns.size() && k < first_function &&
            (i.op == OpTypeInt || i.op == OpTypeFloat || (i.op >= 19 && i.op <= 39) || i.op == OpConstant || i.op == OpVariable ||
             (i.op >= 41 && i.op <= 52) || i.op == OpTypeCooperativeMatrixKHR))
            first_global = k;
        if (k >= first_function) continue;
        if (i.op == OpTypeInt && i.wc == 4 && w(i, 2) == 32 && w(i, 3) == 0) u32 = w(i, 1);
        if (i.op == OpTypeFloat && i.wc == 3 && w(i, 2) == 16) f16 = w(i, 1);
        if (i.op == OpTypeFloat && i.wc == 3 && w(i, 2) == 32) f32 = w(i, 1);
        if (i.op == OpTypeVector && i.wc == 4 && u32 && w(i, 2) == u32) vec_u32[w(i, 1)] = w(i, 3);
        if (i.op == OpTypeVector && i.wc == 4 && f32 && w(i, 2) == f32) vec_f32[w(i, 1)] = w(i, 3);
        if (i.op == OpConstant && i.wc == 4) constants[w(i, 2)] = w(i, 3);
        if (i.op == OpTypeStruct || i.op == OpTypeArray || i.op == OpTypePointer)
            types[w(i, 1)] = std::vector<uint32_t>(&code[i.at + 2], &code[i.at + i.wc]);
        if (i.op == OpVariable && i.wc >= 4 && w(i, 3) == StorageWorkgroup) workgroup_vars.push_back(w(i, 2));
        if (i.op == OpTypeFunction && i.wc == 6) {
            const auto is = [&](uint32_t id, uint32_t count) {
                const auto it = vec_u32.find(id);
                return it != vec_u32.end() && it->second == count;
            };
            if (is(w(i, 2), 2) && is(w(i, 3), 4) && is(w(i, 4), 2) && is(w(i, 5), 2)) fn_type = w(i, 1);
        }
    }
    if (!fn_type || !u32) return why = "no software matrix function", false;
    if (first_function == insns.size() || first_global >= first_function) return why = "no functions", false;
    uint32_t uvec2 = 0, f32x2 = 0;
    for (const Insn& i : insns)
        if (i.op == OpTypeFunction && w(i, 1) == fn_type) uvec2 = w(i, 2);
    for (const auto& [id, count] : vec_f32)
        if (count == 2) f32x2 = id;
    // the software functions (one, as the translator makes them) and their parameters
    struct Target { size_t first, end; uint32_t a, b, c; };
    std::vector<Target> targets;
    for (size_t k = first_function; k < insns.size(); k++) {
        if (insns[k].op != OpFunction || w(insns[k], 4) != fn_type) continue;
        size_t e = k;
        while (e < insns.size() && insns[e].op != OpFunctionEnd) e++;
        if (e == insns.size() || k + 3 >= e || insns[k + 1].op != OpFunctionParameter || insns[k + 2].op != OpFunctionParameter ||
            insns[k + 3].op != OpFunctionParameter)
            return why = "a software function without its three parameters", false;
        targets.push_back({k, e, w(insns[k + 1], 2), w(insns[k + 2], 2), w(insns[k + 3], 2)});
        k = e;
    }
    if (targets.empty()) return why = "the software function was not found", false;
    for (int d = 0; d < 3; d++)
        if (local_ids[d]) {
            const auto it = constants.find(local_ids[d]);
            if (it == constants.end()) return why = "a workgroup size that is not a plain constant", false;
            local[d] = it->second;
        }
    const uint32_t threads = local[0] * local[1] * local[2];
    if (!threads || threads % 32) return why = "a workgroup that is not whole subgroups of 32", false;
    const uint32_t subgroups = threads / 32;
    const uint32_t region = acc32 ? 768u : 512u;   // (words per subgroup: A 128, B 128, C and D 128 each, or 256 each in f32)
    // the shared memory there already is: with an explicit layout every Workgroup block starts at 0 (they alias), so ours
    // goes after the largest; without one each variable is its own
    uint32_t extent = 0;
    bool blocks = false, plain = false;
    for (const uint32_t var : workgroup_vars) {
        uint32_t ptr = 0;
        for (const Insn& i : insns)
            if (i.op == OpVariable && w(i, 2) == var) { ptr = w(i, 1); break; }
        const auto pt = types.find(ptr);
        if (pt == types.end() || pt->second.size() != 2) return why = "a shared variable of an unknown type", false;
        uint32_t pointee = pt->second[1], base = 0;
        const auto st = types.find(pointee);
        if (st == types.end()) return why = "a shared variable of an unknown type", false;
        bool is_block = false;
        for (const Insn& i : insns)
            if (i.op == OpDecorate && i.wc >= 3 && w(i, 1) == pointee && w(i, 2) == DecBlock) is_block = true;
        if (is_block) {
            if (st->second.empty()) return why = "an empty shared block", false;
            base = member0_offset.count(pointee) ? member0_offset[pointee] : 0;
            pointee = st->second[0];
        }
        (is_block ? blocks : plain) = true;
        const auto at = types.find(pointee);
        uint32_t length = 0, stride = 4;
        for (const Insn& i : insns)
            if (i.op == OpTypeArray && w(i, 1) == pointee) {
                const auto c = constants.find(w(i, 3));
                if (c == constants.end()) return why = "a shared array of an unknown length", false;
                length = c->second;
                stride = array_stride.count(pointee) ? array_stride[pointee] : 4;   // (u32 arrays without a layout: 4 bytes)
            }
        if (at == types.end() || !length) return why = "a shared variable that is not an array", false;
        extent = is_block ? std::max(extent, base + length * stride) : extent + length * stride;
    }
    if (blocks && plain) return why = "shared variables with and without a layout", false;
    const bool our_block = blocks;   // (as the others are: a module may not mix the two)
    (void)explicit_layout;
    const uint32_t our_offset = our_block ? (extent + 15) & ~15u : 0;
    const uint32_t total = (our_block ? our_offset : extent) + subgroups * region * 4;
    if (shared_limit && total > shared_limit) return why = "more shared memory than the device has", false;

    // ---- the new declarations ----
    std::vector<uint32_t> caps_add, globals, annotations, interface_add;
    const auto has_cap = [&](uint32_t c) { for (const uint32_t x : caps) if (x == c) return true; return false; };
    if (!has_cap(CapCooperativeMatrix)) caps_add.push_back(CapCooperativeMatrix);
    if (!has_cap(CapFloat16)) caps_add.push_back(CapFloat16);
    if (!has_cap(CapGroupNonUniform)) caps_add.push_back(CapGroupNonUniform);
    const auto emit = [](std::vector<uint32_t>& v, uint32_t op, std::initializer_list<uint32_t> operands) {
        v.push_back(((uint32_t)operands.size() + 1) << 16 | op);
        v.insert(v.end(), operands);
    };
    std::unordered_map<uint32_t, uint32_t> made;   // (value -> our u32 constant)
    const auto c = [&](uint32_t value) {
        const auto it = made.find(value);
        if (it != made.end()) return it->second;
        const uint32_t id = bound++;
        emit(globals, OpConstant, {u32, id, value});
        return made[value] = id;
    };
    if (!f16) f16 = bound++, emit(globals, OpTypeFloat, {f16, 16});
    if (acc32 && !f32) f32 = bound++, emit(globals, OpTypeFloat, {f32, 32});
    if (acc32 && !f32x2) f32x2 = bound++, emit(globals, OpTypeVector, {f32x2, f32, 2});
    const uint32_t scope = c(ScopeSubgroup), c16 = c(16);
    const uint32_t ta = bound++, tb = bound++, tc = bound++;
    emit(globals, OpTypeCooperativeMatrixKHR, {ta, f16, scope, c16, c16, c(0)});
    emit(globals, OpTypeCooperativeMatrixKHR, {tb, f16, scope, c16, c16, c(1)});
    emit(globals, OpTypeCooperativeMatrixKHR, {tc, acc32 ? f32 : f16, scope, c16, c16, c(2)});
    const uint32_t arr = bound++, p_word = bound++, p_shared = bound++, shared = bound++;
    emit(globals, OpTypeArray, {arr, u32, c(subgroups * region)});
    uint32_t pointee = arr;
    if (our_block) {
        const uint32_t st = bound++;
        emit(globals, OpTypeStruct, {st, arr});
        emit(annotations, OpDecorate, {arr, DecArrayStride, 4});
        emit(annotations, OpDecorate, {st, DecBlock});
        emit(annotations, OpMemberDecorate, {st, 0, DecOffset, our_offset});
        emit(annotations, OpDecorate, {shared, DecAliased});
        pointee = st;
    }
    emit(globals, OpTypePointer, {p_shared, StorageWorkgroup, pointee});
    emit(globals, OpTypePointer, {p_word, StorageWorkgroup, u32});
    emit(globals, OpVariable, {p_shared, shared, StorageWorkgroup});
    if (version >= 0x00010400) interface_add.push_back(shared);
    if (!sli || !sgid) {
        const uint32_t p_in = bound++;
        emit(globals, OpTypePointer, {p_in, StorageInput, u32});
        for (uint32_t* var : {&sli, &sgid})
            if (!*var) {
                *var = bound++;
                emit(globals, OpVariable, {p_in, *var, StorageInput});
                emit(annotations, OpDecorate, {*var, DecBuiltIn, var == &sli ? (uint32_t)BuiltInSubgroupLocalInvocationId : (uint32_t)BuiltInSubgroupId});
                interface_add.push_back(*var);   // (Input: in the interface whatever the version)
            }
    }
    std::vector<uint32_t> ext_import;
    if (acc32 && !glsl) {
        glsl = bound++;
        ext_import = {0, glsl};
        put_string(ext_import, "GLSL.std.450");
        ext_import[0] = (uint32_t)ext_import.size() << 16 | OpExtInstImport;
    }
    const uint32_t sem = c(SemAcquireRelease | SemWorkgroupMemory);
    // (constants the body uses, made here so that they are among the declarations)
    const uint32_t c0 = c(0), c1 = c(1), c2 = c(2), c3 = c(3), c4 = c(4), c8 = c(8), c31 = c(31), c64 = c(64), c128 = c(128),
                   c256 = c(256), c384 = c(384), c512 = c(512), c_region = c(region), c_stride_c = c(acc32 ? 16 : 8);
    (void)c1, (void)c4, (void)c384, (void)c512;

    // ---- the body ----
    const auto body = [&](std::vector<uint32_t>& v, uint32_t pa, uint32_t pb, uint32_t pc) {
        const auto op2 = [&](uint32_t op, uint32_t type, uint32_t x, uint32_t y) {
            const uint32_t id = bound++;
            emit(v, op, {type, id, x, y});
            return id;
        };
        const auto load = [&](uint32_t type, uint32_t ptr) { const uint32_t id = bound++; emit(v, OpLoad, {type, id, ptr}); return id; };
        const auto word_ptr = [&](uint32_t index) {
            const uint32_t id = bound++;
            if (our_block) emit(v, OpAccessChain, {p_word, id, shared, c0, index});
            else emit(v, OpAccessChain, {p_word, id, shared, index});
            return id;
        };
        const auto add = [&](uint32_t x, uint32_t value) { return op2(OpIAdd, u32, x, c(value)); };
        const auto extract = [&](uint32_t type, uint32_t from, uint32_t k) {
            const uint32_t id = bound++;
            emit(v, OpCompositeExtract, {type, id, from, k});
            return id;
        };
        const uint32_t label = bound++;
        emit(v, OpLabel, {label});
        const uint32_t lane = op2(OpBitwiseAnd, u32, load(u32, sli), c31);
        const uint32_t base = op2(OpIMul, u32, load(u32, sgid), c_region);
        const uint32_t g = op2(OpShiftRightLogical, u32, lane, c2), i = op2(OpBitwiseAnd, u32, lane, c3);
        const uint32_t at = op2(OpIAdd, u32, base, op2(OpIAdd, u32, op2(OpShiftLeftLogical, u32, g, c3), i));   // (base + g*8 + i)
        for (uint32_t k = 0; k < 4; k++)   // (A, row major, 8 words a row: word k is row g + (k&1)*8, columns 2i + (k>>1)*8 and on)
            emit(v, OpStore, {word_ptr(add(at, (k & 1) * 64 + (k >> 1) * 4)), extract(u32, pa, k)});
        for (uint32_t k = 0; k < 2; k++)   // (B, column major, 8 words a column: word k is column g, rows 2i + k*8 and on)
            emit(v, OpStore, {word_ptr(add(at, 128 + k * 4)), extract(u32, pb, k)});
        uint32_t at_c = 0;
        if (!acc32) {
            for (uint32_t k = 0; k < 2; k++)   // (C, row major: word k is row g + k*8, columns 2i and 2i + 1)
                emit(v, OpStore, {word_ptr(add(at, 256 + k * 64)), extract(u32, pc, k)});
        } else {   // (C in f32, 16 words a row: base + 256 + g*16 + 2i)
            at_c = op2(OpIAdd, u32, base, op2(OpIAdd, u32, op2(OpShiftLeftLogical, u32, g, c4), op2(OpShiftLeftLogical, u32, i, c1)));
            for (uint32_t k = 0; k < 2; k++) {
                const uint32_t pair = bound++;
                emit(v, OpExtInst, {f32x2, pair, glsl, GlslUnpackHalf2x16, extract(u32, pc, k)});
                for (uint32_t h = 0; h < 2; h++) {
                    const uint32_t bits = bound++;
                    emit(v, OpBitcast, {u32, bits, extract(f32, pair, h)});
                    emit(v, OpStore, {word_ptr(add(at_c, 256 + k * 128 + h)), bits});
                }
            }
        }
        emit(v, OpControlBarrier, {scope, scope, sem});
        const auto matrix = [&](uint32_t type, uint32_t offset, uint32_t layout, uint32_t stride) {
            const uint32_t id = bound++;
            emit(v, OpCooperativeMatrixLoadKHR, {type, id, word_ptr(add(base, offset)), c(layout), stride});
            return id;
        };
        const uint32_t ma = matrix(ta, 0, 0, c8), mb = matrix(tb, 128, 1, c8), mc = matrix(tc, 256, 0, c_stride_c);
        const uint32_t md = bound++;
        emit(v, OpCooperativeMatrixMulAddKHR, {tc, md, ma, mb, mc});
        const uint32_t d_offset = acc32 ? 512 : 384;
        emit(v, OpCooperativeMatrixStoreKHR, {word_ptr(add(base, d_offset)), md, c0, c_stride_c});
        emit(v, OpControlBarrier, {scope, scope, sem});
        uint32_t d[2];
        for (uint32_t k = 0; k < 2; k++) {
            if (!acc32) {
                d[k] = load(u32, word_ptr(add(at, d_offset + k * 64)));
            } else {
                uint32_t half[2];
                for (uint32_t h = 0; h < 2; h++) {
                    half[h] = bound++;
                    emit(v, OpBitcast, {f32, half[h], load(u32, word_ptr(add(at_c, d_offset + k * 128 + h)))});
                }
                const uint32_t pair = bound++;
                emit(v, OpCompositeConstruct, {f32x2, pair, half[0], half[1]});
                d[k] = bound++;
                emit(v, OpExtInst, {u32, d[k], glsl, GlslPackHalf2x16, pair});
            }
        }
        const uint32_t result = bound++;
        emit(v, OpCompositeConstruct, {uvec2, result, d[0], d[1]});
        emit(v, OpReturnValue, {result});
        (void)c64, (void)c128, (void)c256;
    };

    std::vector<std::vector<uint32_t>> bodies(targets.size());
    for (size_t t = 0; t < targets.size(); t++) body(bodies[t], targets[t].a, targets[t].b, targets[t].c);

    // ---- the module again, with all that in its place ----
    out.clear();
    out.insert(out.end(), code, code + 5);
    size_t next_target = 0;
    const size_t annotations_at = last_annotation == SIZE_MAX ? first_global : last_annotation + 1;
    for (size_t k = 0; k < insns.size(); k++) {
        const Insn& i = insns[k];
        if (k == annotations_at) out.insert(out.end(), annotations.begin(), annotations.end());
        if (k == first_function) out.insert(out.end(), globals.begin(), globals.end());
        if (next_target < targets.size() && k == targets[next_target].first) {   // (its declaration and parameters, then our body)
            const Target& t = targets[next_target];
            for (size_t j = k; j <= k + 3; j++) out.insert(out.end(), &code[insns[j].at], &code[insns[j].at + insns[j].wc]);
            out.insert(out.end(), bodies[next_target].begin(), bodies[next_target].end());
            out.insert(out.end(), &code[insns[t.end].at], &code[insns[t.end].at + insns[t.end].wc]);
            k = t.end;
            next_target++;
            continue;
        }
        if (i.op == OpEntryPoint) {   // (its interface, with what was added)
            out.push_back((i.wc + (uint32_t)interface_add.size()) << 16 | OpEntryPoint);
            out.insert(out.end(), &code[i.at + 1], &code[i.at + i.wc]);
            out.insert(out.end(), interface_add.begin(), interface_add.end());
        } else {
            out.insert(out.end(), &code[i.at], &code[i.at + i.wc]);
        }
        if (k == last_cap)
            for (const uint32_t cap : caps_add) out.push_back(2u << 16 | OpCapability), out.push_back(cap);
        if (k == (last_ext == SIZE_MAX ? last_cap : last_ext) && !has_ext) {
            std::vector<uint32_t> e{0};
            put_string(e, "SPV_KHR_cooperative_matrix");
            e[0] = (uint32_t)e.size() << 16 | OpExtension;
            out.insert(out.end(), e.begin(), e.end());
            out.insert(out.end(), ext_import.begin(), ext_import.end());
            ext_import.clear();
        } else if (k == (last_ext == SIZE_MAX ? last_cap : last_ext) && !ext_import.empty()) {
            out.insert(out.end(), ext_import.begin(), ext_import.end());
            ext_import.clear();
        }
    }
    out[3] = bound;
    return true;
}

} // namespace mkwvk::mma16
