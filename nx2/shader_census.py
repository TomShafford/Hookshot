"""Every shader program in the game's archives, for the precompile before the first start.

    python -I nx2/shader_census.py <romfs> [--out <file>]

Walks romfs/Shader/*.bfsha.zs (and the shader packages inside other archives), and each BNSH inside them. A BNSH's variations
are programs: which stages go together, so what comes out are real pairs, not a heap of stages. --out writes them for the back
end to translate (gpu/vk/vk_shader.cpp); nothing here goes into the repository: the file is made from the player's own game.

(tameem) Also romfs/Effect/*.vsetb* (each effect's particle shaders: its BNSH, graphics and the emitters' compute) and the engine's
own archives (agl's BAHS .sharcb, in Lib/ and in Shader/ApplicationPackage, and its NDSR .pck, the upscaler's networks: their
compute shaders, each a program of its own). Those
were met only in play: a compute pipeline is built on the thread that records the dispatch, and the game stopped while the driver
compiled it (a second or more on jumps and sword swings, the slowest a minute).

After tools/shader_census.py of TotK Recomp (github.com/valentderah/totk-pc-recomp, MIT), itself after MacNativeGames'
prepare_shader_seed.py (MIT): the BNSH layout and the walk of its variations.

    <out>:  "HKSEED01", u32 programs, u32 stages; then the stages, each once:
            u64 key, u32 stage, u32 control bytes, u32 code bytes, control, code
            then the programs: u32 count, u32 stage indices
"""
import argparse
import collections
import hashlib
import os
import re
import struct
import sys

CONTROL_MAGIC = b"\x34\x12\x76\x98"
ZSTD = b"\x28\xb5\x2f\xfd"
# the stages a variation's program points at (a 0 offset: none), as far as the structure reaches
SLOTS = (8, 16, 24, 32, 40, 48, 56, 64, 72)


def dictionaries(romfs, zs):
    """The game's zstd dictionaries: the files of a SARC, Pack/ZsDic.pack.zs"""
    raw = zs.ZstdDecompressor().decompress(open(os.path.join(romfs, "Pack", "ZsDic.pack.zs"), "rb").read(), max_output_size=1 << 28)
    start = struct.unpack_from("<I", raw, 0x0c)[0]
    out = []
    for k in range(struct.unpack_from("<H", raw, 0x1a)[0]):
        _, _, st, end = struct.unpack_from("<IIII", raw, 0x20 + k * 16)
        out.append(raw[start + st:start + end])
    return out


def unpack(data, dicts, zs):
    if data[:4] != ZSTD:
        return data
    try:
        return zs.ZstdDecompressor().decompress(data, max_output_size=1 << 31)
    except zs.ZstdError:
        pass
    for d in dicts:
        try:
            return zs.ZstdDecompressor(dict_data=zs.ZstdCompressionDict(d, dict_type=zs.DICT_TYPE_FULLDICT)).decompress(
                data, max_output_size=1 << 31)
        except zs.ZstdError:
            continue
    raise ValueError("no dictionary fits")


def bnsh_programs(data, stats):
    """Each variation of a BNSH (its offsets are its own) as [(stage, control, code)...]"""
    if data[:4] != b"BNSH" or data[12:14] != b"\xff\xfe" or data[96:100] != b"grsc":
        raise ValueError("unsupported BNSH header")
    size = struct.unpack_from("<I", data, 28)[0]
    data = data[:size]
    count, variations = struct.unpack_from("<IQ", data, 124)
    for i in range(count):
        program = struct.unpack_from("<Q", data, variations + i * 64 + 16)[0]
        if not program or program + 80 > size:
            stats["no program"] += 1
            continue
        stages = []
        for rel in SLOTS:
            offset = struct.unpack_from("<Q", data, program + rel)[0]
            if not offset or offset + 32 > size:
                continue
            control_off, code_off, code_size, control_size = struct.unpack_from("<QQII", data, offset + 8)
            if not 0x60 <= control_size <= 0x1000 or control_off + control_size > size or code_off + code_size > size:
                continue
            control = data[control_off:control_off + control_size]
            if control[:4] != CONTROL_MAGIC:
                continue
            stage = struct.unpack_from("<H", control, 0x56)[0]
            stats["slot %d, stage %d" % (rel, stage)] += 1
            stages.append((stage, control, data[code_off:code_off + code_size]))
        if stages:
            yield stages


def archives(romfs):
    """The files that hold shaders: Shader/* (programs, and ApplicationPackage's compute), Layout2Shader/*, Effect/* (each effect's
    shaders), Lib/* (the engine's)"""
    for folder, marks in (("Shader", (".bfsha", ".sarc")), ("Layout2Shader", (".bfsha", ".sarc")), ("Effect", (".vsetb",)),
                          ("Lib", (".sarc", ".sharcb"))):
        top = os.path.join(romfs, folder)
        for root, dirs, files in sorted(os.walk(top)):
            dirs.sort()
            for name in sorted(files):
                if any(m in name for m in marks):
                    yield os.path.join(root, name)


def bahs_computes(data, stats):
    """The compute shaders of a BAHS (agl's shader archive, version 11) as [(stage, control, code)]. Its header: u32 the file's
    size at 8, the code at 0x18 (u32 offset, u32 size); before the code, the control sections and the records of the shaders: a
    record is u64 its code (from the file's code size: at that plus the size), u32 its control's size, s32 its control (from the
    record), and a control says how long its code is (+0x44). Its graphics shaders are left out: which go together is not known"""
    if data[:4] != b"BAHS" or len(data) < 0x20:
        return []
    code_at, code_size = struct.unpack_from("<II", data, 0x18)
    if not 0 < code_at <= len(data) or code_at + code_size > len(data):
        stats["BAHS: no code"] += 1
        return []
    controls = {m.start() for m in re.finditer(re.escape(CONTROL_MAGIC), data[:code_at]) if m.start() % 4 == 0}
    words = memoryview(data[:code_at - code_at % 4]).cast("i")
    out = {}
    for i in range(3, len(words)):
        record = (i - 3) * 4
        control_at = record + words[i]
        if control_at == record or control_at not in controls:
            continue
        code_rel, high, control_size = words[i - 3] & 0xffffffff, words[i - 2], words[i - 1]
        if high or not 0xd8 <= control_size <= 0x1000 or control_at + control_size > code_at:
            continue
        control = bytes(data[control_at:control_at + control_size])
        stage = struct.unpack_from("<H", control, 0x56)[0]
        end = struct.unpack_from("<I", control, 0x44)[0]
        code_off = code_rel + code_size
        if stage != 5 or code_off % 0x100 or not code_at <= code_off or code_off + end > len(data) or not end:
            continue
        out[(control_at, code_off)] = (stage, control, bytes(data[code_off:code_off + end]))
    stats["BAHS compute"] += len(out)
    return list(out.values())


def ndsr_computes(data, stats):
    """The compute shaders of an NDSR pack (agl's .pck: the upscaler's networks, its TAAU among them) as [(stage, control, code)]:
    a table at the front whose entries name a control (u32 offset) and, 8 bytes on, its code (u32 offset); the controls come
    after the code, 0xe0 bytes each. A control can be named by more than one entry (one control, more than one code)"""
    if data[:4] != b"NDSR":
        return []
    controls = [m.start() for m in re.finditer(re.escape(CONTROL_MAGIC), data) if m.start() + 0xe0 <= len(data)]
    if not controls:
        return []
    first = controls[0]
    out = {}
    for control_at in controls:
        control = bytes(data[control_at:control_at + 0xe0])
        stage = struct.unpack_from("<H", control, 0x56)[0]
        end = struct.unpack_from("<I", control, 0x44)[0]
        if stage != 5 or not end:
            continue
        for m in re.finditer(re.escape(struct.pack("<I", control_at)), data[:first]):
            if m.start() + 12 > first:
                continue
            code_off = struct.unpack_from("<I", data, m.start() + 8)[0]
            if code_off % 0x100 or code_off + end > first:
                continue
            out[(control_at, code_off)] = (stage, control, bytes(data[code_off:code_off + end]))
    stats["NDSR compute"] += len(out)
    return list(out.values())


def sarc_files(data):
    start = struct.unpack_from("<I", data, 0x0c)[0]
    count = struct.unpack_from("<H", data, 0x1a)[0]
    for k in range(count):
        _, _, st, end = struct.unpack_from("<IIII", data, 0x20 + k * 16)
        yield data[start + st:start + end]


def blobs(data, dicts, zs):
    """The data itself, or each file when it is an archive"""
    if data[:4] == b"SARC":
        for inner in sarc_files(data):
            yield from blobs(unpack(inner, dicts, zs), dicts, zs)
    else:
        yield data


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("romfs")
    p.add_argument("--out")
    a = p.parse_args()
    import zstandard as zs
    dicts = dictionaries(a.romfs, zs)
    stats = collections.Counter()
    stage_index, stage_list, programs, seen = {}, [], [], set()
    by_folder = collections.Counter()
    quiet = 0
    for path in archives(a.romfs):
        made = 0

        def add(stages):
            nonlocal made
            ids = []
            for stage, control, code in stages:
                key = int.from_bytes(hashlib.sha1(control + code).digest()[:8], "little")
                if key not in stage_index:
                    stage_index[key] = len(stage_list)
                    stage_list.append((key, stage, control, code))
                ids.append(stage_index[key])
            ids = tuple(ids)
            if ids not in seen:
                seen.add(ids)
                programs.append(ids)
                made += 1

        try:
            outer = unpack(open(path, "rb").read(), dicts, zs)
        except (OSError, ValueError) as ex:   # (one file the archives cannot open: the rest go on)
            stats["unreadable file"] += 1
            print("%-64s left out: %s" % (os.path.basename(path)[:64], ex), flush=True)
            continue
        for data in blobs(outer, dicts, zs):
            view = memoryview(data)
            for m in re.finditer(b"BNSH", data):
                if data[m.start() + 96:m.start() + 100] != b"grsc":
                    continue
                try:
                    for stages in bnsh_programs(bytes(view[m.start():m.start() + struct.unpack_from("<I", data, m.start() + 28)[0]]), stats):
                        add(stages)
                except (ValueError, struct.error):
                    stats["unreadable BNSH"] += 1
            if data[:4] == b"BAHS":
                for stage in bahs_computes(data, stats):
                    add([stage])
            if data[:4] == b"NDSR":
                for stage in ndsr_computes(data, stats):
                    add([stage])
        folder = os.path.relpath(path, a.romfs).split(os.sep)[0]
        by_folder[folder] += made
        if folder != "Effect":
            print("%-64s %6d programs" % (os.path.basename(path)[:64], made), flush=True)
        else:   # (some 1400 of them: a line every 100)
            quiet += 1
            if quiet % 100 == 0:
                print("Effect: %d files, %d programs so far" % (quiet, by_folder[folder]), flush=True)
    print("programs by folder:", dict(by_folder))
    kinds = collections.Counter(tuple(sorted(stage_list[i][1] for i in ids)) for ids in programs)
    print("%d programs from %d stages" % (len(programs), len(stage_list)))
    print("by stages:", kinds.most_common(12))
    print("slots:", sorted(stats.items()))
    if a.out:
        with open(a.out, "wb") as f:
            f.write(b"HKSEED01" + struct.pack("<II", len(programs), len(stage_list)))
            for key, stage, control, code in stage_list:
                f.write(struct.pack("<QIII", key, stage, len(control), len(code)) + control + code)
            for ids in programs:
                f.write(struct.pack("<I%dI" % len(ids), len(ids), *ids))
        print("wrote", a.out, os.path.getsize(a.out) >> 20, "MB")


if __name__ == "__main__":
    sys.exit(main())
