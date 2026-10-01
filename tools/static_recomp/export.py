#!/usr/bin/env python3
"""Offline PowerPC -> C++ exporter. Generated native entries contain no runtime instruction decoder.

Input is a post-load XeniOS code image (including title updates/import fixups),
not a BIOS, encrypted XEX, or an executable for another console. Unsupported
instructions reject export unless --allow-unsupported is explicitly selected;
that option emits fatal native guards, never an interpreter or JIT fallback.
"""
from __future__ import annotations
import argparse
import collections
import hashlib
import json
import pathlib
import re
import shutil
import struct
import sys
import tempfile

ABI = 1
LIMIT = 64 * 1024 * 1024
FORMAT = "xenios-aot-image-v1"

class Unsupported(ValueError):
    pass

def sx(value: int, bits: int) -> int:
    return (value & ((1 << (bits - 1)) - 1)) - (value & (1 << (bits - 1)))

def u(value: int) -> str:
    return f"0x{value & 0xffffffffffffffff:X}ull"

def decode(w: int, pc: int) -> tuple[str, str]:
    """Return a mnemonic and fixed C++ statements; all decoding is OFFLINE."""
    op, rt, ra, rb = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    xo, rc = (w >> 1) & 1023, bool(w & 1)
    R = lambda n: f"c.r[{n}]"
    A, B, S = R(ra), R(rb), R(rt)
    A0 = A if ra else "0ull"
    next_pc = (pc + 4) & 0xffffffff
    def result(name: str, expr: str, dest: int = rt, record: bool = rc):
        return name, f"{R(dest)} = {expr};" + (f" c.record({R(dest)});" if record else "")
    def branch(name: str, target: str, conditional: bool = False, ctr_target: bool = False):
        text = f"const uint32_t target = uint32_t({target}) & ~3u; "
        if conditional:
            bo, bi = rt, ra
            if ctr_target and not bo & 4:
                raise Unsupported("bcctr with CTR decrement is reserved")
            text += "bool count_ok = true; "
            if not bo & 4:
                text += f"--*c.ctr; count_ok = (*c.ctr != 0) != {str(bool(bo & 2)).lower()}; "
            condition = "true" if bo & 16 else f"c.cr_bit({bi}) == {str(bool(bo & 8)).lower()}"
            text += f"const bool take = count_ok && ({condition}); "
        if w & 1:
            text += f"*c.lr = {u(next_pc)}; "
        text += f"e.pc = {'take ? target : ' + u(next_pc) if conditional else 'target'}; return Status::kContinue;"
        return name, text
    if op in (14, 15):
        immediate = sx(w & 65535, 16) * (65536 if op == 15 else 1)
        return result("addis" if op == 15 else "addi", f"{A0} + {u(immediate)}", record=False)
    if op in (12, 13, 8):
        immediate = sx(w & 65535, 16)
        return result({12:"addic",13:"addic.",8:"subfic"}[op],
                      f"Add(c, {'~' + A if op == 8 else A}, {u(immediate)}, {1 if op == 8 else 0}, true, false)",
                      record=op == 13)
    if op == 7:
        return result("mulli", f"{A} * {u(sx(w & 65535, 16))}", record=False)
    if op in (24,25,26,27,28,29):
        symbol = {24:"|",25:"|",26:"^",27:"^",28:"&",29:"&"}[op]
        val = (w & 65535) << (16 if op & 1 else 0)
        return result({24:"ori",25:"oris",26:"xori",27:"xoris",28:"andi.",29:"andis."}[op],
                      f"{S} {symbol} {u(val)}", ra, op >= 28)
    if op in (10,11) or (op == 31 and xo in (0,32)):
        signed = op == 11 or (op == 31 and xo == 0)
        width = 64 if w & (1 << 21) else 32
        field = (w >> 23) & 7
        rhs = B if op == 31 else u(sx(w & 65535,16) if signed else w & 65535)
        lhs = A
        if width == 32:
            lhs, rhs = f"uint32_t({lhs})", f"uint32_t({rhs})"
        if signed:
            lhs = f"std::bit_cast<int{width}_t>({lhs})"
            rhs = f"std::bit_cast<int{width}_t>({('uint64_t(' + rhs + ')') if width == 64 else rhs})"
        return "cmp" + ("" if signed else "l"), f"c.cr_field({field}, ({lhs} < {rhs} ? 8u : {lhs} > {rhs} ? 4u : 2u) | (*c.xer_so != 0));"
    if op == 18:
        displacement = sx(w & 0x3fffffc,26)
        return branch("b", u(displacement if w & 2 else pc + displacement))
    if op == 16:
        displacement = sx(w & 0xfffc,16)
        return branch("bc", u(displacement if w & 2 else pc + displacement), True)
    if op == 19 and xo in (16,528):
        return branch("bclr" if xo == 16 else "bcctr", "*c.lr" if xo == 16 else "*c.ctr", True, xo == 528)
    if op == 19 and xo == 150:
        return "isync", "std::atomic_thread_fence(std::memory_order_seq_cst);"
    if op == 19 and xo == 0:
        dst, src = (w >> 23) & 7, (w >> 18) & 7
        return "mcrf", f"c.cr_field({dst}, (c.cr() >> {(7-src)*4}) & 15u);"
    cr_ops = {257:"a & b",129:"a & !b",289:"!(a ^ b)",225:"!(a & b)",33:"!(a | b)",449:"a | b",417:"a | !b",193:"a ^ b"}
    if op == 19 and xo in cr_ops:
        return "crlogical", f"const bool a = c.cr_bit({ra}), b = c.cr_bit({rb}); const uint32_t v = {cr_ops[xo]}; c.cr((c.cr() & ~(1u << {31-rt})) | (v << {31-rt}));"
    if op in (20,21,23):
        shift = f"unsigned({B} & 31)" if op == 23 else str(rb)
        mask = f"Mask({((w >> 6) & 31)+32}, {((w >> 1) & 31)+32})"
        expr = f"std::rotl((uint64_t(uint32_t({S})) << 32) | uint32_t({S}), {shift}) & {mask}"
        if op == 20:
            # rlwimi preserves the upper half of the 64-bit destination.
            expr = f"({A} & ~{mask}) | ({expr})"
        return result({20:"rlwimi",21:"rlwinm",23:"rlwnm"}[op], expr, ra)
    if op == 30:
        index = (w >> 2) & 7
        mb = ((w >> 6) & 31) | (w & 32)
        sh = rb | ((w & 2) << 4)
        if index < 4:
            mask = f"Mask({mb if index != 1 else 0}, {mb if index == 1 else 63 if index == 0 else 63-sh})"
            expr = f"std::rotl({S}, {sh}) & {mask}"
            if index == 3: expr = f"({A} & ~{mask}) | ({expr})"
            return result(["rldicl","rldicr","rldic","rldimi"][index], expr, ra)
        index = (w >> 1) & 15
        if index in (8,9):
            return result("rldcl" if index == 8 else "rldcr", f"std::rotl({S}, int({B} & 63)) & Mask({mb if index == 8 else 0}, {63 if index == 8 else mb})", ra)
    # Integer and FP memory accesses. Validate update encodings before emitting.
    d_mem = {32:(4,False,False),33:(4,False,True),34:(1,False,False),35:(1,False,True),
             40:(2,False,False),41:(2,False,True),42:(2,True,False),43:(2,True,True),
             36:(4,False,False),37:(4,False,True),38:(1,False,False),39:(1,False,True),44:(2,False,False),45:(2,False,True),
             48:(4,False,False),49:(4,False,True),50:(8,False,False),51:(8,False,True),52:(4,False,False),53:(4,False,True),54:(8,False,False),55:(8,False,True)}
    x_mem = {23:(4,False,False),55:(4,False,True),87:(1,False,False),119:(1,False,True),279:(2,False,False),311:(2,False,True),343:(2,True,False),375:(2,True,True),
             151:(4,False,False),183:(4,False,True),215:(1,False,False),247:(1,False,True),407:(2,False,False),439:(2,False,True),
             21:(8,False,False),53:(8,False,True),149:(8,False,False),181:(8,False,True),341:(4,True,False),373:(4,True,True),
             534:(4,False,False),790:(2,False,False),662:(4,False,False),918:(2,False,False),
             535:(4,False,False),567:(4,False,True),599:(8,False,False),631:(8,False,True),663:(4,False,False),695:(4,False,True),727:(8,False,False),759:(8,False,True),983:(4,False,False)}
    ds = op in (58,62) and (w & 3) < (3 if op == 58 else 2)
    if op in d_mem or (op == 31 and xo in x_mem) or ds:
        indexed = op == 31
        size, signed, update = ((8, False, (w & 3) == 1) if ds else x_mem[xo] if indexed else d_mem[op])
        if ds and op == 58 and (w & 3) == 2: size, signed = 4, True
        load = (op in (32,33,34,35,40,41,42,43,48,49,50,51,58) if not indexed else xo in (23,55,87,119,279,311,343,375,21,53,341,373,534,790,535,567,599,631))
        fp = (48 <= op <= 55) or (indexed and xo in (535,567,599,631,663,695,727,759,983))
        reverse = indexed and xo in (534,790,662,918)
        if update and (not ra or (load and not fp and ra == rt)):
            raise Unsupported("invalid update-form register combination")
        offset = B if indexed else u(sx(w & (0xfffc if ds else 65535),16))
        text = f"const uint32_t address = uint32_t({A0} + {offset}); uint64_t value = 0; "
        if load:
            text += f"if (!e.Load(address, {size}, value)) return e.status; "
            if reverse: text += f"value = ByteSwap(value, {size}); "
            if fp:
                text += f"c.f[{rt}] = " + ("double(std::bit_cast<float>(uint32_t(value))); " if size == 4 else "std::bit_cast<double>(value); ")
            else:
                text += f"{S} = " + (f"SignExtend(value, {size * 8}); " if signed else "value; ")
        else:
            if fp:
                if xo == 983 and indexed: text += f"value = uint32_t(std::bit_cast<uint64_t>(c.f[{rt}])); "
                elif size == 4: text += f"if (!FloatToMemory(e, c.f[{rt}], value)) return e.status; "
                else: text += f"value = std::bit_cast<uint64_t>(c.f[{rt}]); "
            else: text += f"value = {S}; "
            if reverse: text += f"value = ByteSwap(value, {size}); "
            text += f"if (!e.Store(address, {size}, value)) return e.status; "
        if update: text += f"{A} = address;"
        return ("load" if load else "store") + ("_fp" if fp else "") + str(size * 8), text
    if op in (46,47):
        if op == 46 and ra >= rt: raise Unsupported("lmw base overlaps destination registers")
        text = f"uint32_t address = uint32_t({A0} + {u(sx(w & 65535,16))}); "
        for reg in range(rt,32):
            text += (f"{{ uint64_t value; if (!e.Load(address, 4, value)) return e.status; c.r[{reg}] = value; }} " if op == 46 else f"if (!e.Store(address, 4, c.r[{reg}])) return e.status; ")
            text += "address += 4; "
        return "lmw" if op == 46 else "stmw", text
    if op == 31:
        if xo in (20,84):
            raise Unsupported("shared reservation monitor is not integrated")
        if xo in (150,214) and rc:
            raise Unsupported("shared reservation monitor is not integrated")
        if xo in (598,854):
            return "sync", "std::atomic_thread_fence(std::memory_order_seq_cst);"
        if xo in (54,86,246,278,470,982):
            return "cache_hint", "std::atomic_thread_fence(std::memory_order_seq_cst);"
        if xo == 1014:
            return "dcbz", f"uint32_t address = uint32_t({A0} + {B}) & ~127u; for (unsigned i = 0; i < 128; i += 8) if (!e.Store(address + i, 8, 0)) return e.status;"
        if xo == 19: return result("mfcr", "c.cr()", record=False)
        if xo == 144:
            fields = (w >> 12) & 255
            mask = sum(15 << (i * 4) for i in range(8) if fields & (1 << i))
            return "mtcrf", f"c.cr((c.cr() & ~uint32_t({u(mask)})) | (uint32_t({S}) & {u(mask)}));"
        if xo in (339,467,371):
            spr = ((w >> 16) & 31) | ((w >> 6) & 992)
            targets = {8:"*c.lr",9:"*c.ctr",256:"*c.vrsave"}
            if xo == 371 and spr in (268,269):
                return result("mftb", "uint32_t(e.host.Clock()" + (" >> 32" if spr == 269 else "") + ")", record=False)
            if spr == 1:
                if xo == 339:
                    return result("mfxer", "uint64_t((*c.xer_so << 31) | (*c.xer_ov << 30) | (*c.xer_ca << 29))", record=False)
                return "mtxer", f"*c.xer_so = ({S} >> 31) & 1; *c.xer_ov = ({S} >> 30) & 1; *c.xer_ca = ({S} >> 29) & 1;"
            if spr not in targets: raise Unsupported(f"SPR {spr}")
            if xo == 339: return result("mfspr", targets[spr], record=False)
            if xo == 467: return "mtspr", f"{targets[spr]} = {('uint32_t(' + S + ')') if spr == 256 else S};"
        logic = {28:f"{S} & {B}",60:f"{S} & ~{B}",444:f"{S} | {B}",412:f"{S} | ~{B}",316:f"{S} ^ {B}",284:f"~({S} ^ {B})",476:f"~({S} & {B})",124:f"~({S} | {B})"}
        if xo in logic: return result("logical", logic[xo], ra)
        if xo in (26,58): return result("cntlzw" if xo == 26 else "cntlzd", f"std::countl_zero({'uint32_t(' + S + ')' if xo == 26 else S})", ra)
        if xo in (954,922,986): return result("signextend", f"SignExtend({S}, { {954:8,922:16,986:32}[xo] })", ra)
        if xo in (24,536,27,539):
            width = 32 if xo in (24,536) else 64
            left = xo in (24,27)
            shift = f"unsigned({B} & {width * 2 - 1})"
            val = f"uint32_t({S})" if width == 32 else S
            return result("logicalshift", f"({shift} >= {width} ? 0ull : uint64_t({val} {'<<' if left else '>>'} {shift}))" + (" & 0xFFFFFFFFull" if width == 32 else ""), ra)
        if xo in (792,824,794) or ((xo & ~1) == 826):
            width = 32 if xo in (792,824) else 64
            sh = str(rb) if xo == 824 else str(rb | ((w & 2) << 4)) if (xo & ~1) == 826 else f"unsigned({B} & {width*2-1})"
            return result("arithmeticshift", f"ArithmeticShift({S}, {sh}, {width}, c.xer_ca)", ra)
        ar = xo & 511
        overflow = bool(w & 1024)
        if ar in (266,10,138,234,202,40,8,136,232,200,104):
            first,second,carry,setcarry = A,B,"0", ar != 266 and ar != 40 and ar != 104
            if ar in (138,234,202,136,232,200): carry = "*c.xer_ca"
            if ar in (234,232): second = "~0ull"
            if ar in (202,200): second = "0ull"
            if ar in (40,8,136,232,200,104): first = "~" + A
            if ar in (40,8,104): carry = "1"
            if ar == 104: second = "0ull"
            return result("integeraddsub", f"Add(c, {first}, {second}, {carry}, {str(setcarry).lower()}, {str(overflow).lower()})")
        if ar in (235,233):
            if overflow: raise Unsupported("multiply overflow variant")
            expr = f"{A} * {B}"
            if ar == 235: expr = f"SignExtend({A}, 32) * SignExtend({B}, 32)"
            return result("mullw" if ar == 235 else "mulld", expr)
        if xo in (75,11,73,9):
            if xo in (73,9): expr = f"MultiplyHigh({A}, {B}, {str(xo==73).lower()})"
            elif xo == 11: expr = f"(uint64_t(uint32_t({A})) * uint32_t({B})) >> 32"
            else: expr = f"SignExtend(uint64_t(int64_t(std::bit_cast<int32_t>(uint32_t({A}))) * int64_t(std::bit_cast<int32_t>(uint32_t({B})))) >> 32, 32)"
            return result("multiplyhigh", expr)
        if ar in (491,459,489,457):
            width = 32 if ar in (491,459) else 64
            signed = ar in (491,489)
            if overflow: raise Unsupported("divide overflow variant")
            cast = f"uint{width}_t"
            left,right = f"{cast}({A})",f"{cast}({B})"
            if signed: left,right = f"std::bit_cast<int{width}_t>({left})", f"std::bit_cast<int{width}_t>({right})"
            text = f"const auto a = {left}, b = {right}; if (!b"
            if signed: text += f" || (a == std::numeric_limits<int{width}_t>::min() && b == -1)"
            text += f") return e.Fail(Status::kArithmeticFault); {S} = "
            text += f"uint32_t(a / b);" if signed and width == 32 else "uint64_t(a / b);"
            if rc: text += f" c.record({S});"
            return "divide", text
    if op == 17:
        return "sc", "return e.Fail(Status::kMissingImport, e.pc);"
    if op in (59,63):
        # Bit-preserving FP moves/load/store do not depend on FPSCR rounding.
        if op == 63 and xo in (72,40,264,136):
            mask = {72:"",40:" ^ 0x8000000000000000ull",264:" & 0x7FFFFFFFFFFFFFFFull",136:" | 0x8000000000000000ull"}[xo]
            text = f"c.f[{rt}] = std::bit_cast<double>(std::bit_cast<uint64_t>(c.f[{rb}]){mask});"
            if rc: text += " c.cr_field(1, (*c.fpscr >> 28) & 15u);"
            return "fpmove", text
        if op == 63 and xo in (0,32):
            return "fcmp", f"if (!FloatCompare(e, {(w >> 23) & 7}, c.f[{ra}], c.f[{rb}], {str(xo==32).lower()})) return e.status;"
        small = (w >> 1) & 31
        fpu = {18:"Div",20:"Sub",21:"Add",25:"Mul",28:"Msub",29:"Madd",30:"Nmsub",31:"Nmadd"}
        if small in fpu:
            if op == 59: raise Unsupported("single-result arithmetic requires Xenon precision support")
            return "fp"+fpu[small].lower(), f"if (!FloatOp(e, FpOp::k{fpu[small]}, {rt}, {ra}, {rb}, {(w >> 6) & 31}, {str(op==59).lower()}, {str(rc).lower()})) return e.status;"
        if op == 63 and xo == 12:
            return "frsp", f"if (!FloatOp(e, FpOp::kRoundSingle, {rt}, {ra}, {rb}, 0, true, {str(rc).lower()})) return e.status;"
        if op == 63 and small == 23:
            return "fsel", f"c.f[{rt}] = c.f[{ra}] >= -0.0 ? c.f[{(w >> 6) & 31}] : c.f[{rb}];" + (" c.cr_field(1, (*c.fpscr >> 28) & 15u);" if rc else "")
    # SIMD: integer/logical byte operations with explicit Xenia lane semantics.
    if op == 4:
        vec = w & 2047
        logics = {1028:"a & b",1092:"a & ~b",1156:"a | b",1220:"a ^ b",1284:"~(a | b)"}
        if vec in logics:
            return "vmxlogical", f"for (unsigned i=0; i<16; ++i) {{ const uint8_t a=VectorByte(c,{ra},i), b=VectorByte(c,{rb},i); VectorByte(c,{rt},i,uint8_t({logics[vec]})); }}"
        if (w & 63) in (42,43,44):
            vc = (w >> 6) & 31
            if (w & 63) == 42: expr = "(a & ~d) | (b & d)"
            elif (w & 63) == 43: expr = f"d < 16 ? VectorByte(c,{ra},d) : VectorByte(c,{rb},d-16)"
            else:
                shift = vc & 15
                return "vsldoi", f"std::array<uint8_t,16> out{{}}; for (unsigned i=0;i<16;++i) out[i]=i+{shift}<16?VectorByte(c,{ra},i+{shift}):VectorByte(c,{rb},i+{shift}-16); for(unsigned i=0;i<16;++i) VectorByte(c,{rt},i,out[i]);"
            text = f"std::array<uint8_t,16> out{{}}; for(unsigned i=0;i<16;++i){{[[maybe_unused]] const uint8_t a=VectorByte(c,{ra},i), b=VectorByte(c,{rb},i), d=VectorByte(c,{vc},i){' & 31' if (w&63)==43 else ''}; out[i]=uint8_t({expr});}} for(unsigned i=0;i<16;++i)VectorByte(c,{rt},i,out[i]);"
            return "vsel" if (w & 63)==42 else "vperm", text
        if vec in (0,64,128,1024,1088,1152):
            width = {0:1,64:2,128:4,1024:1,1088:2,1152:4}[vec]
            operation = "+" if vec < 1024 else "-"
            return "vmxinteger", f"for(unsigned lane=0;lane<{16//width};++lane){{uint32_t a=0,b=0; for(unsigned j=0;j<{width};++j){{a=(a<<8)|VectorByte(c,{ra},lane*{width}+j);b=(b<<8)|VectorByte(c,{rb},lane*{width}+j);}}const uint32_t v=a{operation}b;for(unsigned j=0;j<{width};++j)VectorByte(c,{rt},lane*{width}+j,uint8_t(v>>(({width}-1-j)*8)));}}"
    if op == 31 and xo in (103,231):
        load = xo == 103
        text = f"const uint32_t address=uint32_t({A0}+{B})&~15u; "
        text += f"for(unsigned i=0;i<4;++i){{uint64_t value; if(!e.Load(address+i*4,4,value))return e.status; VectorWord(c,{rt},i,uint32_t(value));}}" if load else f"for(unsigned i=0;i<4;++i)if(!e.Store(address+i*4,4,VectorWord(c,{rt},i)))return e.status;"
        return "lvx" if load else "stvx", text
    raise Unsupported(f"opcode=0x{w:08X}, primary={op}, extended={xo}")

def load_image(path: pathlib.Path):
    obj = json.loads(path.read_text(encoding="utf-8"))
    if obj.get("format") != FORMAT or obj.get("abi") != ABI:
        raise ValueError("not a supported XeniOS post-load image manifest")
    name = obj.get("name")
    if name in (".", "..") or not isinstance(name,str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,128}",name):
        raise ValueError("module name must be 1-128 ASCII filename characters")
    ranges, last, total = [], 0, 0
    for item in obj.get("ranges",[]):
        base, size = item["base"], item["size"]
        if type(base) is not int or type(size) is not int or base < last or base % 4 or size <= 0 or size % 4 or base + size > 1 << 32:
            raise ValueError("invalid or overlapping executable ranges")
        total += size
        if total > LIMIT: raise ValueError("code-image limit exceeded (64 MiB)")
        file = (path.parent / item["file"]).resolve()
        if not file.is_relative_to(path.parent.resolve()): raise ValueError("range file escapes manifest directory")
        if file.stat().st_size != size: raise ValueError("code range size mismatch")
        data = file.read_bytes()
        if hashlib.sha256(data).hexdigest() != item["sha256"]: raise ValueError("input code-image SHA-256 mismatch")
        ranges.append((base,data))
        last = base + size
    if not ranges: raise ValueError("manifest has no executable code")
    return name,ranges

def export_image(name, ranges, out: pathlib.Path, allow_unsupported=False, chunk_words=64):
    if out.exists(): raise ValueError("output already exists; choose a fresh directory")
    if not 1 <= chunk_words <= 1024: raise ValueError("chunk-words must be between 1 and 1024")
    digest=hashlib.sha256(name.encode()+b"".join(struct.pack(">I",base)+data for base,data in ranges)).hexdigest()
    prefix="aot_"+digest[:16]
    counts=collections.Counter(); unsupported=[]; chunks=[]; sources=[]
    for base,data in ranges:
        words=struct.unpack(">"+"I"*(len(data)//4),data)
        for start in range(0,len(words),chunk_words):
            end=min(start+chunk_words,len(words)); addr=base+start*4
            symbol=f"{prefix}_{addr:08x}"
            text=[f"Status {symbol}(Runtime& e) {{", "[[maybe_unused]] auto& c = e.cpu;", "switch(e.pc) {"]
            for index in range(start,end): text.append(f"case 0x{base+index*4:08x}u: goto L{index};")
            text += ["default: return e.Fail(Status::kMissingEntry, e.pc);", "}"]
            for index in range(start,end):
                pc=base+index*4; w=words[index]
                try: mnemonic,body=decode(w,pc)
                except Unsupported as error:
                    mnemonic="unsupported"
                    unsupported.append({"pc":pc,"word":w,"reason":str(error)})
                    body=f"return e.Fail(Status::kUnsupportedInstruction, 0x{w:08x}u);"
                counts[mnemonic]+=1
                text += [f"L{index}: if(!e.Tick(0x{pc:08x}u,0x{w:08x}u)) return e.status;", "{ "+body+" }"]
            text += [f"e.pc = uint32_t(0x{base+end*4:X}ull); return Status::kContinue;", "}"]
            chunks.append((addr,base+end*4,symbol)); sources.append("\n".join(text))
    report={"format":"xenios-aot-export-report-v1","abi":ABI,"name":name,"image_id":digest,
            "instructions_total":sum(counts.values()),"instructions_supported":sum(counts.values())-len(unsupported),
            "unsupported":unsupported,"mnemonics":dict(sorted(counts.items())),"chunks":len(chunks),
            "runtime":"prelinked-native-strict","gameplay_verified":False}
    if unsupported and not allow_unsupported:
        raise ValueError(f"{len(unsupported)} unsupported instruction words; first: {unsupported[0]}. No output generated. --allow-unsupported emits explicit fatal guards, not compatibility.")
    out.parent.mkdir(parents=True,exist_ok=True)
    tmp=pathlib.Path(tempfile.mkdtemp(prefix=".aot-",dir=out.parent))
    try:
        for index in range(0,len(sources),32):
            (tmp/f"{prefix}_{index//32:05d}.cc").write_text('#include "xenia/cpu/backend/static/operations.h"\nnamespace xe::cpu::aot {\n'+"\n".join(sources[index:index+32])+"\n}\n",encoding="utf-8")
        meta=['#include "xenia/cpu/backend/static/runtime.h"','namespace xe::cpu::aot {']
        meta += [f"Status {sym}(Runtime&);" for _,_,sym in chunks]
        meta += [f"static const CodeRange {prefix}_ranges[] = {{"]
        for base,data in ranges:
            values=",".join(f"0x{v:02x}" for v in hashlib.sha256(data).digest())
            meta += [f"{{0x{base:x}u,0x{base+len(data):x}ull, {{{values}}}}},"]
        meta += ["};",f"static const Chunk {prefix}_chunks[] = {{"]
        meta += [f"{{0x{a:x}u,0x{b:x}ull,&{sym}}}," for a,b,sym in chunks]
        meta += ["};",f'const Module& {prefix}_module() {{ static const Module module{{kAbiVersion,"{name}",{prefix}_ranges,{prefix}_chunks}}; return module; }}',"}"]
        (tmp/f"{prefix}_module.cc").write_text("\n".join(meta)+"\n")
        (tmp/"module.json").write_text(json.dumps({"format":"xenios-aot-link-v1","abi":ABI,"symbol":prefix+"_module","name":name,"image_id":digest,"sources":{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(tmp.glob("*.cc"))}},indent=2)+"\n")
        (tmp/"coverage.json").write_text(json.dumps(report,indent=2)+"\n")
        (tmp/".gitignore").write_text("*\n")
        tmp.rename(out)
    except BaseException:
        shutil.rmtree(tmp,ignore_errors=True); raise
    return report

def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("manifest",type=pathlib.Path)
    p.add_argument("--out",required=True,type=pathlib.Path)
    p.add_argument("--allow-unsupported",action="store_true")
    p.add_argument("--chunk-words",type=int,default=64)
    args=p.parse_args(argv)
    try:
        name,ranges=load_image(args.manifest)
        report=export_image(name,ranges,args.out,args.allow_unsupported,args.chunk_words)
    except (ValueError,OSError,KeyError,TypeError) as error:
        print(f"export failed: {error}",file=sys.stderr); return 1
    print(json.dumps({k:v for k,v in report.items() if k not in ("unsupported","mnemonics")},indent=2))
    if report["unsupported"]: print(f"WARNING: {len(report['unsupported'])} fatal unsupported guards emitted",file=sys.stderr)
    return 0
if __name__=="__main__": raise SystemExit(main())
