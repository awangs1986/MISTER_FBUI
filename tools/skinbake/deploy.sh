#!/usr/bin/env bash
# 把编译好的 MiSTer 和烘焙好的皮肤包推到真机。
#
# 直连 MiSTer（不经跳板机）。密码认证用 python3 pexpect，因为 WSL 里没有
# sshpass；两者缺一时脚本会直接报出来，不做静默降级。

set -u
# 机器是 DHCP，IP 会变。找不到就 MISTER=<ip> ./deploy.sh ... 覆盖。
MISTER="${MISTER:-192.168.5.4}"
PASS="${MISTER_PASS:-1}"
SELF="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SELF/../.." && pwd)"

cat > /tmp/rcp.py <<PYEOF
import sys, pexpect
src, dst = sys.argv[1], sys.argv[2]
c = pexpect.spawn('scp', ['-O', '-o', 'StrictHostKeyChecking=no',
                          '-o', 'UserKnownHostsFile=/dev/null',
                          src, 'root@$MISTER:' + dst],
                  timeout=180, encoding='latin-1')
if c.expect(['assword:', pexpect.EOF], timeout=20) == 0:
    c.sendline('$PASS')
    c.expect(pexpect.EOF, timeout=180)
print(c.before or '', end='')
PYEOF

cat > /tmp/rsh.py <<PYEOF
import sys, pexpect
c = pexpect.spawn('ssh', ['-o', 'StrictHostKeyChecking=no',
                          '-o', 'UserKnownHostsFile=/dev/null',
                          'root@$MISTER', sys.argv[1]],
                  timeout=int(sys.argv[2]) if len(sys.argv) > 2 else 60,
                  encoding='latin-1')
if c.expect(['assword:', pexpect.EOF], timeout=20) == 0:
    c.sendline('$PASS')
    c.expect(pexpect.EOF, timeout=int(sys.argv[2]) if len(sys.argv) > 2 else 60)
print(c.before or '', end='')
PYEOF

rcp() { python3 /tmp/rcp.py "$1" "$2"; }
rsh() { python3 /tmp/rsh.py "$1" "${2:-60}"; }

case "${1:-all}" in
bin)
	rcp "$ROOT/bin/MiSTer" /media/fat/MiSTer.new
	rsh 'md5sum /media/fat/MiSTer.new'
	md5sum "$ROOT/bin/MiSTer"
	;;
skin)
	rsh 'mkdir -p /media/fat/skins/default/240p /media/fat/skins/default/1080p'
	rcp "$ROOT/skins/default/skin.ini" /media/fat/skins/default/skin.ini
	for p in 240p 1080p; do
		for f in "$ROOT/skins/default/$p"/*; do
			[ -f "$f" ] && rcp "$f" "/media/fat/skins/default/$p/$(basename "$f")"
		done
	done
	rsh 'find /media/fat/skins -type f | head -40; du -sh /media/fat/skins'
	;;
swap)
	rsh 'cp /media/fat/MiSTer /media/fat/MiSTer.bak 2>/dev/null; mv /media/fat/MiSTer.new /media/fat/MiSTer && chmod +x /media/fat/MiSTer && ls -la /media/fat/MiSTer*'
	;;
sh)
	rsh "$2" "${3:-60}"
	;;
*)
	echo "usage: deploy.sh {bin|skin|swap|sh <cmd> [timeout]}" >&2
	exit 1
	;;
esac
