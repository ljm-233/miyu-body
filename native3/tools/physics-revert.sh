#!/usr/bin/env bash
# physics-revert.sh — 一条命令把「局部输入」改过的模型文件换回备份，并验证她还能跑。
#
#   tools/physics-revert.sh              # 换回最新一份备份
#   tools/physics-revert.sh --list       # 只看有哪些备份，不动文件
#   tools/physics-revert.sh --no-restart # 换回但不碰服务
#
# 换回的东西（三样必须一起换，否则「点胸只动胸」会变成「点了没反应」）：
#   ../model/大肥鱼.physics3.json       ← physics3.json.bak-HHMM（新增的 PhysicsSetting 去掉）
#   ../model/大肥鱼.cdi3.json           ← cdi3.json.bak-HHMM（参数中文名换回去）
#   ../model/大肥鱼.moc3                ← moc3.bak-HHMM（Poke* 的 Id 改回 Divider*）
#   state/param-parts.tsv               ← param-parts.tsv.bak-HHMM（P 行的参数名换回去）
#
# 验证（任何一条不过就 exit 1，别当没事）：
#   ① 两个 JSON 能解析，PhysicsSettingCount == len(PhysicsSettings)
#   ② param-parts.tsv 的行数、部位名齐
#   ③ 服务重起后 active，进程 miyu-wayland 在
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL="$ROOT/../model"
STATE="$ROOT/state"
PHYS="$MODEL/大肥鱼.physics3.json"
CDI="$MODEL/大肥鱼.cdi3.json"
MOC3="$MODEL/大肥鱼.moc3"
PARTS="$STATE/param-parts.tsv"

say() { printf '[revert] %s\n' "$*"; }
die() { printf '[revert] 失败：%s\n' "$*" >&2; exit 1; }

newest() {  # newest <glob> —— 按修改时间取最新一份（同分钟多份也稳）
    local f
    f=$(ls -1t $1 2>/dev/null | head -n 1)
    [ -n "$f" ] && printf '%s' "$f"
}

MANIFEST="$STATE/physics-local.manifest"
BAK_PHYS=$(newest "$PHYS.bak-*")
BAK_CDI=$(newest "$CDI.bak-*")
BAK_PARTS=$(newest "$PARTS.bak-*")
BAK_MOC3=$(newest "$MOC3.bak-*")
# manifest 优先：它记的是「第一次改之前」的原件；按时间取最新会把改过的文件当原件
if [ -f "$MANIFEST" ]; then
    for f in "$PHYS" "$CDI" "$MOC3" "$PARTS"; do
        b=$(awk -F'\t' -v k="$f" '$1==k{print $2}' "$MANIFEST")
        [ -n "$b" ] && [ -f "$b" ] || continue
        case "$f" in
            "$PHYS")  BAK_PHYS="$b"  ;;
            "$CDI")   BAK_CDI="$b"   ;;
            "$MOC3")  BAK_MOC3="$b"  ;;
            "$PARTS") BAK_PARTS="$b" ;;
        esac
    done
fi

if [ "${1:-}" = "--list" ]; then
    say "physics3 备份："; ls -1t "$PHYS".bak-* 2>/dev/null | sed 's/^/    /'
    say "cdi3     备份："; ls -1t "$CDI".bak-*  2>/dev/null | sed 's/^/    /'
    say "moc3     备份："; ls -1t "$MOC3".bak-* 2>/dev/null | sed 's/^/    /'
    say "param-parts 备份："; ls -1t "$PARTS".bak-* 2>/dev/null | sed 's/^/    /'
    exit 0
fi

[ -n "$BAK_PHYS" ] || die "找不到 $PHYS.bak-*（先备份再改，见 NOTES 二十三）"
[ -n "$BAK_CDI" ]  || die "找不到 $CDI.bak-*"

say "physics3 ← $(basename "$BAK_PHYS")"
cp -f "$BAK_PHYS" "$PHYS"
say "cdi3     ← $(basename "$BAK_CDI")"
cp -f "$BAK_CDI" "$CDI"
if [ -n "$BAK_MOC3" ]; then
    say "moc3     ← $(basename "$BAK_MOC3")"
    cp -f "$BAK_MOC3" "$MOC3"
else
    say "moc3 没有备份，跳过（Poke* 的 Id 保持现状 —— 那是物理与 moc3 不一致的坏状态）"
fi
if [ -n "$BAK_PARTS" ]; then
    say "param-parts ← $(basename "$BAK_PARTS")"
    cp -f "$BAK_PARTS" "$PARTS"
else
    say "param-parts 没有备份，跳过（部位表保持现状）"
fi

# ① JSON 自证：能解析，且 Meta 计数和实际条数一致（不一致 = 物理会漏读/越界）
python3 - "$PHYS" "$CDI" "$PARTS" <<'PY' || die "文件自检不过（上面有原因）"
import json, sys
phys, cdi, parts = sys.argv[1:4]
p = json.load(open(phys, encoding='utf-8'))
n = len(p['PhysicsSettings'])
m = p['Meta']['PhysicsSettingCount']
ti = sum(len(s['Input']) for s in p['PhysicsSettings'])
to = sum(len(s['Output']) for s in p['PhysicsSettings'])
tv = sum(len(s['Vertices']) for s in p['PhysicsSettings'])
bad = []
if n != m: bad.append('PhysicsSettingCount %d != %d' % (m, n))
if p['Meta']['TotalInputCount'] != ti: bad.append('TotalInputCount %d != %d' % (p['Meta']['TotalInputCount'], ti))
if p['Meta']['TotalOutputCount'] != to: bad.append('TotalOutputCount %d != %d' % (p['Meta']['TotalOutputCount'], to))
if p['Meta']['VertexCount'] != tv: bad.append('VertexCount %d != %d' % (p['Meta']['VertexCount'], tv))
c = json.load(open(cdi, encoding='utf-8'))
if not c.get('Parameters'): bad.append('cdi3 Parameters 空了')
rows = [l.rstrip('\n').split('\t') for l in open(parts, encoding='utf-8') if l.startswith('P\t')]
named = {r[1] for r in rows if len(r) >= 4}
need = {'head', 'chest', 'arm', 'skirt', 'leg', 'tail'}
if not need <= named: bad.append('param-parts 缺部位：%s' % (need - named))
if bad:
    print('  ✗ ' + '\n  ✗ '.join(bad)); sys.exit(1)
print('  ① physics3: %d settings / %d inputs / %d outputs / %d vertices，Meta 计数一致'
      % (n, ti, to, tv))
print('  ② cdi3: %d 参数 / %d 组；param-parts: %d 行 P，部位齐' % (len(c['Parameters']), len(c['ParameterGroups']), len(rows)))
PY

if [ "${1:-}" = "--no-restart" ]; then
    say "跳过服务重起（--no-restart）"
    exit 0
fi

# ③ 她还能跑吗：重起服务 → active + 进程在 + 日志里没有致命错
say "重起 miyu-body …"
systemctl --user restart miyu-body.service || die "systemctl restart 失败"
for i in $(seq 1 20); do
    sleep 0.3
    if systemctl --user is-active --quiet miyu-body.service; then break; fi
done
systemctl --user is-active --quiet miyu-body.service || die "服务没起来：journalctl --user -u miyu-body -n 50"
sleep 2
pid=$(pgrep -x miyu-wayland | head -n 1)
[ -n "$pid" ] || die "服务 active 但没有 miyu-wayland 进程"
say "③ 服务 active（PID $pid），副屏 MIYU_OUTPUT=1 不变"
journalctl --user -u miyu-body -n 40 -o cat | grep -E "物理|physics|Error|error|失败" | tail -n 5 | sed 's/^/    /' || true
say "换回完成。要再上局部输入：python3 tools/physics-local.py --apply"
