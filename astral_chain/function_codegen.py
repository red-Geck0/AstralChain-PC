"""Function-shaped C for guest functions (experimental, branch function-lifter).

The page backend (native_trace.emit_page) gives every guest instruction a case of its page's switch, so any address
can be entered and guest registers live in the Cpu struct across every instruction. Here a guest function that BL
targets becomes one C function, fn_<entry>(Cpu *c): entered only at its start, with labels at its branch targets,
direct C calls for BL, and the general registers it uses kept in local variables (the compiler can hold them in host
registers). The page code stays: every address still has its case, indirect branches still dispatch through the
pages, and the functions only replace direct calls.

Protocol, the same as a page function's: called with c->pc at the entry and c->x[30] at the return address; returns
with c->pc where guest execution continues. Normally that is the return address (RET). Anything the function does not
follow itself (an unresolved indirect branch, an instruction without a translation, a callee that came back
elsewhere) leaves with every register written back and c->pc at that address; the caller then sees c->pc != its
return address and leaves too, until page code or the dispatcher takes over. Registers are written back to the Cpu
before every call, helper that reads or writes general registers, and exit, and read back after calls and such helpers.
"""
import re

# Helpers the lifted code passes the Cpu to that touch neither the general registers nor sp nor pc (flags, floating
# point state, vector registers, exclusive monitor): third_party/LASRecomp/runtime/cpu.h, simd.h, simd_fp.h,
# runtime/exclusive_pair.h (fresh-bench/tools/helper_regs.py lists them). Any other call with the Cpu is unsafe.
SAFE_HELPERS = {
    'adds32', 'adds64', 'subs32', 'subs64', 'logic32', 'logic64', 'adc32', 'adc64', 'sbc32', 'sbc64', 'adcs32',
    'adcs64', 'sbcs32', 'sbcs64', 'fp_fma_d', 'fp_fma_s', 'fp_op_d', 'fp_op_s', 'cmp_d', 'cmp_s', 'frint_d', 'frint_s',
    'fromint_d', 'fromint_s', 'toint_d', 'toint_s', 'op_s', 'op_d', 'pair_s', 'pair_d', 'vcmp_s', 'vcmp_d', 'vfrint_d',
    'vfrint_s', 'vfromint_d', 'vfromint_s', 'vtoint_d', 'vtoint_s', 'vrecpe_s', 'vrsqrte_s', 'simd_d2s', 'simd_s2d',
    'sl_across', 'sl_addlp', 'sl_gen', 'sl_ldm', 'sl_ldr', 'sl_lds', 'sl_pmull64', 'sl_stm', 'sl_sts', 'sv_aes',
    'sv_fcvtl', 'sv_fcvtn', 'sv_sha1hash', 'sv_sha256hash', 'sv_sha_misc', 'sv_tbl', 'ldx8', 'ldx16', 'ldx32', 'ldx64',
    'stx8', 'stx16', 'stx32', 'stx64', 'rt_clear_pair_monitor', 'rt_store_pair',
    'COND_EQ', 'COND_NE', 'COND_CS', 'COND_CC', 'COND_MI', 'COND_PL', 'COND_VS', 'COND_VC', 'COND_HI', 'COND_LS',
    'COND_GE', 'COND_LT', 'COND_GT', 'COND_LE', 'FLAG_N', 'FLAG_Z', 'FLAG_C', 'FLAG_V',
}
_CPU_CALL = re.compile(r'\b([A-Za-z_][A-Za-z_0-9]*)\s*\(\s*c\s*[,)]')
_REG = re.compile(r'c->x\[(\d+)\]|c->sp\b')
_WRITE = re.compile(r'c->x\[(\d+)\]\s*=(?!=)|c->sp\s*=(?!=)')
MAX_INSTRUCTIONS = 20000


def _unsafe(code):
    return any(name not in SAFE_HELPERS for name in _CPU_CALL.findall(code)) or '&c->x' in code or '&c->sp' in code


def _regs(code):
    found = set()
    for m in _REG.finditer(code):
        found.add(int(m.group(1)) if m.group(1) else 'sp')
    return found


def _writes(code):
    found = set()
    for m in _WRITE.finditer(code):
        found.add(int(m.group(1)) if m.group(1) else 'sp')
    return found


def _local(r):
    return 'r_sp' if r == 'sp' else f'r{r}'


def _cpu(r):
    return 'c->sp' if r == 'sp' else f'c->x[{r}]'


_MEM = re.compile(r'\b(LD|ST)(8|16|32|64)\(')
# Guest loads and stores are volatile here (VLD/VST, native_build VOLATILE_MEMORY): with the registers in locals a
# polling loop (another thread's flag) has no other memory traffic, and the compiler would read the value once and
# spin for ever (a hang at the title screen with AC_ASYNC_SUBMIT=1). The page code stores c->pc at every branch, which
# the compiler cannot prove separate from guest memory, so it re-reads anyway.
VOLATILE_MEMORY = ''.join(
    f'#define VLD{n}(a) (*(volatile const {t} *)(uintptr_t)(a))\n#define VST{n}(a, v) (*(volatile {t} *)(uintptr_t)(a) = (uint{n}_t)(v))\n'
    for n, t in ((8, 'u8a'), (16, 'u16u'), (32, 'u32u'), (64, 'u64u')))


def _localize(code):
    code = _MEM.sub(lambda m: f'V{m.group(1)}{m.group(2)}(', code)
    return _REG.sub(lambda m: f'r{m.group(1)}' if m.group(1) else 'r_sp', code)


def extent(entry, nodes, entries, switches):
    """The instructions of the function at entry (pcs), following its own control flow."""
    seen, work = set(), [entry]
    while work:
        pc = work.pop()
        if pc in seen:
            continue
        node = nodes.get(pc)
        if node is None or node[1] is None:
            continue  # (no translation here: an exit)
        seen.add(pc)
        if len(seen) > MAX_INSTRUCTIONS:
            return None
        op = node[1]
        if op.kind in ('op', 'bl', 'blr'):
            work.append(pc + 4)
        elif op.kind == 'b':
            if op.target not in entries or op.target == entry:
                work.append(op.target)
        elif op.kind == 'bcond':
            work.extend((op.target, pc + 4))
        elif op.kind == 'br':
            work.extend(switches.get(pc, ()))
    return seen


def emit_function(entry, pcs, nodes, entries, switches, direct_ok):
    """C source of fn_<entry>. direct_ok(target): a BL to target may call fn_<target> directly."""
    used, written = set(), set()
    for pc in pcs:
        op = nodes[pc][1]
        text = op.code + ' ' + (op.cond or '') + ' ' + (op.target if isinstance(op.target, str) else '')
        if op.kind == 'op' and _unsafe(op.code):
            continue  # (runs on the Cpu, after a write-back)
        used |= _regs(text)
        written |= _writes(op.code)
    used |= written
    order = sorted(used, key=lambda r: -1 if r == 'sp' else r)
    sync = ''.join(f'{_cpu(r)}={_local(r)};' for r in order if r in written)
    reload = ''.join(f'{_local(r)}={_cpu(r)};' for r in order)
    called = set()
    lines = [f'void fn_{entry:x}(Cpu *restrict c) {{']
    if order:
        lines.append(' uint64_t ' + ','.join(f'{_local(r)}={_cpu(r)}' for r in order) + ';')
    lines.append(f' goto L_{entry:x};')

    def leave(target_expr):
        return f'{sync} c->pc={target_expr}; return;'

    def go(target):
        return f'goto L_{target:x};' if target in pcs else leave(f'{target}ull')

    for pc in sorted(pcs):
        op = nodes[pc][1]
        nxt = pc + 4
        body = ''
        if op.kind == 'op':
            if _unsafe(op.code):
                body = f'{sync} c->pc={pc}ull; {op.code} {reload} ' + go(nxt)
            else:
                body = _localize(op.code) + ' ' + (go(nxt) if nxt not in pcs or nxt != _next(pcs, pc) else '')
        elif op.kind == 'b':
            if op.target in pcs and (op.target not in entries or op.target == entry):
                body = go(op.target)
            elif direct_ok(op.target):
                called.add(op.target)
                body = f'{sync} c->pc={op.target}ull; __attribute__((musttail)) return fn_{op.target:x}(c);'
            else:
                body = f'{sync} c->pc={op.target}ull; fn_guest_call(c,c->x[30]); return;'
        elif op.kind == 'bcond':
            body = f'if ({_localize(op.cond)}) {{ {go(op.target)} }} ' + (go(nxt) if nxt != _next(pcs, pc) else '')
        elif op.kind in ('bl', 'blr'):
            if op.kind == 'bl':
                target = f'{op.target}ull'
                if direct_ok(op.target):
                    called.add(op.target)
                    invoke = f'fn_{op.target:x}(c);'
                else:
                    invoke = f'fn_guest_call(c,{nxt}ull);'
                pre = ''
            else:
                pre = f'uint64_t t={_localize(op.target)}; '
                target = 't'
                invoke = f'fn_guest_call(c,{nxt}ull);'
            body = (f'{{ {pre}{sync} c->x[30]={nxt}ull; c->pc={target}; {invoke} '
                    f'if (c->pc!={nxt}ull) return; {reload} }} ' + (go(nxt) if nxt != _next(pcs, pc) else ''))
        elif op.kind == 'br':
            targets = [t for t in switches.get(pc, ()) if t in pcs]
            cases = ''.join(f'case {t}ull: goto L_{t:x}; ' for t in sorted(set(targets)))
            body = f'{{ uint64_t t={_localize(op.target)}; switch (t) {{ {cases}default: {leave("t")} }} }}'
        elif op.kind == 'ret':
            reg = (nodes[pc][0] >> 5) & 31
            value = _local(reg) if reg in used else f'c->x[{reg}]'
            body = f'{{ uint64_t t={value}; {leave("t")} }}'
        elif op.kind == 'trap':
            body = f'{sync} c->pc={pc}ull; {op.code} return;'
        else:
            body = leave(f'{pc}ull')
        lines.append(f'L_{pc:x}: {body}')
    lines.append('}')
    return '\n'.join(lines), called


def _next(pcs, pc):
    """The instruction emitted after pc (sorted order), for fall-through."""
    nxt = pc + 4
    return nxt if nxt in pcs else None


def plan(nodes, switches, pages=None, exclude=()):
    """Entries (BL targets with a translation) and their extents; pages: only functions entered in these pages."""
    entries = set()
    for pc, (word, op) in nodes.items():
        if op is not None and op.kind == 'bl' and op.target in nodes and nodes[op.target][1] is not None:
            entries.add(op.target)
    entries -= set(exclude)
    chosen = {e for e in entries if pages is None or (e >> 12) in pages}
    extents = {}
    for e in sorted(chosen):
        pcs = extent(e, nodes, entries, switches)
        if pcs:
            extents[e] = pcs
    return entries, extents
