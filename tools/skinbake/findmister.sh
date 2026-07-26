#!/usr/bin/env bash
# 机器是 DHCP，重启后 IP 会漂。扫本网段列出存活主机的 MAC 和 SSH banner，
# 用来把 MiSTer 从一堆设备里认出来。
#
#   ./findmister.sh                  # 扫 192.168.5.0/24
#   SUBNET=192.168.1 ./findmister.sh
#
# MiSTer 的特征：SSH banner 是 Dropbear 或精简版 OpenSSH，认证方式只有
# publickey,password（没有 gssapi-*，有 gssapi 的基本是完整发行版的机器）。

set -u
SUBNET="${SUBNET:-192.168.5}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "扫描 $SUBNET.0/24 ..."

# 并发 ping，命中的直接落盘 —— 子 shell 改不了父进程的数组，用文件回收结果。
for i in $(seq 1 254); do
	{ ping -c1 -W1 "$SUBNET.$i" >/dev/null 2>&1 && echo "$SUBNET.$i" >> "$TMP/alive"; } &
done
wait

if [ ! -s "$TMP/alive" ]; then
	echo "网段里没有存活主机。检查网线/电源，或用 SUBNET= 换网段。"
	exit 1
fi

sort -t. -k4 -n "$TMP/alive" > "$TMP/sorted"
echo "存活 $(wc -l < "$TMP/sorted") 台："
echo

while read -r ip; do
	# WSL2 跑在自己的网络命名空间里，看不到宿主的 ARP 表，这里会是空的；
	# 原生 Linux 上才有值。
	mac=$(ip neigh show "$ip" 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="lladdr") print $(i+1)}' | head -1)
	# banner grab：SSH 服务端会抢先发版本号，读一行就够，不用真的握手。
	banner=$(timeout 2 bash -c "exec 3<>/dev/tcp/$ip/22 && head -c 60 <&3" 2>/dev/null | tr -d '\r\n')
	echo "$ip|${mac:-}|${banner:--}" >> "$TMP/rows"
done < "$TMP/sorted"

# MAC 一个都拿不到时就不打印那一列，免得输出全是问号。
if cut -d'|' -f2 "$TMP/rows" | grep -q .; then
	printf '%-16s %-19s %s\n' IP MAC SSH
	awk -F'|' '{printf "%-16s %-19s %s\n", $1, ($2==""?"?":$2), $3}' "$TMP/rows"
else
	printf '%-16s %s\n' IP SSH
	awk -F'|' '{printf "%-16s %s\n", $1, $3}' "$TMP/rows"
fi
