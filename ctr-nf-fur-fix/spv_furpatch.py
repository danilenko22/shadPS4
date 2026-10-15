"""Pelaje CTR NF en shadPS4: descarta las capas de pelaje 1..5 (shell_index > 0) de la PIEL.
Solo queda la capa base -> piel lisa, sin puntitos y sin tapar el cabello.
Uso: python -I spv_furpatch.py <dump_dir> <patch_dir>"""
import sys, os, struct, glob
OpDecorate, OpAccessChain, OpLoad, OpFOrdLessThan, OpConstant = 71, 65, 61, 184, 43
OpLabel, OpPhi, OpSelectionMerge, OpBranchConditional, OpKill = 248, 245, 247, 250, 252
DecLocation = 30
# Shaders de PELO (cabello) que usan el mismo sistema de capas: no tocarlos.
EXCLUIR = {'bd4b295e'}
def inst(op, *ops): return [((len(ops) + 1) << 16) | op, *ops]
def parse(w):
    i, out = 5, []
    while i < len(w):
        wc, op = w[i] >> 16, w[i] & 0xffff
        out.append((i, wc, op, w[i+1:i+wc])); i += wc
    return out
def patch(path):
    data = open(path, 'rb').read()
    w = list(struct.unpack('<%dI' % (len(data)//4), data))
    if w[0] != 0x07230203: return None
    ins = parse(w)
    loc = {o[0]: o[2] for _, _, op, o in ins if op == OpDecorate and len(o) >= 3 and o[1] == DecLocation}
    zeros = {o[1] for _, _, op, o in ins if op == OpConstant and len(o) == 3 and o[2] == 0}
    inputs = {v for v, l in loc.items() if l == 9}
    if not inputs: return None
    chains = {o[1] for _, _, op, o in ins if op == OpAccessChain and o[2] in inputs}
    loads = {o[1] for _, _, op, o in ins if op == OpLoad and o[2] in chains}
    cmp = [(k, o) for k, (_, _, op, o) in enumerate(ins) if op == OpFOrdLessThan and o[2] in zeros and o[3] in loads]
    if len(cmp) != 1: return None
    k, o = cmp[0]
    cond = o[1]
    # bloque que contiene la comparacion
    blk = max(j for j in range(k) if ins[j][2] == OpLabel)
    old_label = ins[blk][3][0]
    bound = w[3]; kill_l, cont_l = bound, bound + 1; w[3] = bound + 2
    out = w[:5]
    for j, (pos, wc, op, ops) in enumerate(ins):
        words = w[pos:pos+wc]
        if op == OpPhi:  # predecesor viejo -> bloque de continuacion
            words = [words[0], words[1], words[2]] + [cont_l if (x % 2 == 1 and v == old_label) else v
                                                     for x, v in enumerate(words[3:])]
        out += words
        if j == k:
            out += inst(OpSelectionMerge, cont_l, 0)
            out += inst(OpBranchConditional, cond, kill_l, cont_l)
            out += inst(OpLabel, kill_l)
            out += inst(OpKill)
            out += inst(OpLabel, cont_l)
    return struct.pack('<%dI' % len(out), *out)
if __name__ == '__main__':
    dump, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    for f in sorted(glob.glob(os.path.join(dump, 'fs_*.spv'))):
        name = os.path.basename(f)
        if any(h in name for h in EXCLUIR):
            print('excluido (pelo)', name); continue
        blob = patch(f)
        if blob:
            open(os.path.join(outdir, name), 'wb').write(blob); print('parcheado', name)
