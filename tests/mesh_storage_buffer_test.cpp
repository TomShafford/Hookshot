// The task and mesh storage buffer fix (global_memory_to_storage_buffer_pass.cpp HighFromDescriptor, the task/mesh bias) on hand-made
// IR. Built from the repository root with clang++ -std=c++20 and the Vulkan back end's include paths (see /tmp/vkcc.sh in the session),
// with frontend/ir/{microinstruction,value,opcodes,type,ir_emitter,basic_block,condition,patch}.cpp
#include "shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp"
#include <cstdio>
using namespace Shader;
using namespace Shader::Optimization;
static IR::Inst* mk(IR::Opcode op, std::initializer_list<IR::Value> args) {
    auto* i = new IR::Inst(op, 0);
    size_t k = 0;
    for (const IR::Value& a : args) i->SetArg(k++, a);
    return i;
}
static IR::Value V(IR::Inst* i) { return IR::Value{i}; }
static IR::Value U(u32 v) { return IR::Value{v}; }
static IR::Inst* cbuf(u32 idx, u32 off) { return mk(IR::Opcode::GetCbufU32, {U(idx), U(off)}); }
static IR::Inst* load(IR::Value a) { return mk(IR::Opcode::LoadGlobal32, {a}); }
static IR::Inst* addr64(IR::Inst* lo, IR::Inst* hi) {
    return mk(IR::Opcode::PackUint2x32, {V(mk(IR::Opcode::CompositeConstructU32x2, {V(lo), V(hi)}))});
}
static IR::Inst* carry_hi(IR::Inst* hi_base, IR::Inst* lo_add) {
    IR::Inst* c = mk(IR::Opcode::GetCarryFromOp, {V(lo_add)});
    IR::Inst* sel = mk(IR::Opcode::SelectU32, {V(c), U(1u), U(0u)});
    return mk(IR::Opcode::IAdd32, {V(hi_base), V(sel)});
}
static int fails = 0;
static void expect(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); fails += !ok; }
struct Outcome { std::optional<StorageBufferAddr> biased, unbiased; bool high; };
static Outcome run(IR::Inst* access, u32 table) {
    const Bias b{.index = 0, .offset_begin = table, .offset_end = table + 256, .alignment = 16};
    const auto info = TrackLowAddress(access);
    Outcome o{};
    if (!info) return o;
    o.biased = Track(info->value, &b);
    o.unbiased = Track(info->value, nullptr);
    o.high = o.unbiased && HighFromDescriptor(info->high, *o.unbiased);
    return o;
}
int main() {
    IR::Inst* base_ptr_addr = addr64(cbuf(0, 0x770), cbuf(0, 0x774));
    {   // the six: a pointer loaded from memory plus an offset from c5+0x60
        IR::Inst* plo = load(V(base_ptr_addr));
        IR::Inst* phi = load(V(base_ptr_addr));
        IR::Inst* lo = mk(IR::Opcode::IAdd32, {V(plo), V(cbuf(5, 0x60))});
        IR::Inst* a = load(V(addr64(lo, carry_hi(phi, lo))));
        const Outcome o = run(a, 0x770);
        expect(!o.biased, "loaded pointer + c5 offset: not in the mesh table");
        expect(o.unbiased && o.unbiased->index == 5 && o.unbiased->offset == 0x60, "loaded pointer + c5 offset: unbiased track takes c5+0x60 (the old bug)");
        expect(!o.high, "loaded pointer + c5 offset: high word not c5+0x64 -> refused");
    }
    {   // the mesh table's buffer plus a c5 offset
        IR::Inst* lo = mk(IR::Opcode::IAdd32, {V(cbuf(0, 0x790)), V(cbuf(5, 0x60))});
        IR::Inst* a = load(V(addr64(lo, carry_hi(cbuf(0, 0x794), lo))));
        const Outcome o = run(a, 0x770);
        expect(o.biased && o.biased->index == 0 && o.biased->offset == 0x790, "table slot 2 + c5 offset: the mesh bias finds c0+0x790");
        const Bias maxwell{.index = 0, .offset_begin = 0x100, .offset_end = 0x700, .alignment = 16};
        const auto info = TrackLowAddress(a);
        expect(!Track(info->value, &maxwell), "table slot 2 + c5 offset: Maxwell's bias missed it (the old bug)");
    }
    {   // a real pointer kept in c5, plus an index loaded from memory
        IR::Inst* lo = mk(IR::Opcode::IAdd32, {V(cbuf(5, 0x60)), V(load(V(base_ptr_addr)))});
        IR::Inst* a = load(V(addr64(lo, carry_hi(cbuf(5, 0x64), lo))));
        const Outcome o = run(a, 0x770);
        expect(o.unbiased && o.unbiased->offset == 0x60 && o.high, "pointer in c5 + loaded index: kept bound");
    }
    {   // a real pointer kept in c5, read as 64 bits
        IR::Inst* v = mk(IR::Opcode::GetCbufU32x2, {U(5u), U(0x60u)});
        IR::Inst* lo = mk(IR::Opcode::CompositeExtractU32x2, {V(v), U(0u)});
        IR::Inst* hi = mk(IR::Opcode::CompositeExtractU32x2, {V(v), U(1u)});
        IR::Inst* a = load(V(addr64(lo, hi)));
        const Outcome o = run(a, 0x770);
        expect(o.unbiased && o.high, "64-bit pointer in c5: kept bound");
    }
    {   // an offset whose carry reaches c5+0x64 through the low word only: still refused
        IR::Inst* lo = mk(IR::Opcode::IAdd32, {V(load(V(base_ptr_addr))), V(mk(IR::Opcode::IAdd32, {V(cbuf(5, 0x60)), V(cbuf(5, 0x64))}))});
        IR::Inst* a = load(V(addr64(lo, carry_hi(load(V(base_ptr_addr)), lo))));
        const Outcome o = run(a, 0x770);
        expect(o.unbiased && !o.high, "c5+0x64 behind the carry only: refused");
    }
    {   // the whole decision, as the pass makes it for a mesh program
        ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        g_task_mesh_table = 0x770;
        IR::Inst* plo = load(V(base_ptr_addr));
        IR::Inst* lo = mk(IR::Opcode::IAdd32, {V(plo), V(cbuf(5, 0x60))});
        IR::Inst* a = load(V(addr64(lo, carry_hi(load(V(base_ptr_addr)), lo))));
        StorageInfo info;
        g_mesh_offsets_refused = 0;
        CollectStorageBuffers(block, *a, info);
        expect(info.set.empty() && info.to_replace.empty() && g_mesh_offsets_refused == 1, "pass: the six's access left global, counted");
        IR::Inst* lo2 = mk(IR::Opcode::IAdd32, {V(cbuf(0, 0x790)), V(cbuf(5, 0x60))});
        IR::Inst* b = load(V(addr64(lo2, carry_hi(cbuf(0, 0x794), lo2))));
        CollectStorageBuffers(block, *b, info);
        expect(info.set.size() == 1 && info.set.begin()->index == 0 && info.set.begin()->offset == 0x790, "pass: table slot bound");
        g_task_mesh_table = 0;   // a vertex program: as before
        StorageInfo info2;
        g_mesh_offsets_refused = 0;
        CollectStorageBuffers(block, *a, info2);
        expect(info2.set.size() == 1 && info2.set.begin()->index == 5 && g_mesh_offsets_refused == 0, "pass: other stages unchanged");
    }
    std::printf("%d failed\n", fails);
    return fails != 0;
}
namespace Shader::Optimization {
thread_local bool g_all_global_this = false;
thread_local u32 g_nvn2_compute_ssbo_base = 0, g_nvn2_compute_ssbo_count = 16;
}
