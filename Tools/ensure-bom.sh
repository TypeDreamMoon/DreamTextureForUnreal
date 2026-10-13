#!/usr/bin/env bash
# 给所有源码文件补上 UTF-8 BOM。原因见 Docs/contributing.md 的 Source encoding 一节。
# 幂等：已经有 BOM 的不动。
set -e
cd "$(dirname "$0")/.."
n=0
while IFS= read -r -d '' f; do
	if [ "$(head -c 3 "$f" | od -An -tx1 | tr -d ' \n')" != "efbbbf" ]; then
		printf '\xEF\xBB\xBF' | cat - "$f" > "$f.bom" && mv "$f.bom" "$f"
		echo "  + $f"
		n=$((n+1))
	fi
done < <(find Source -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.cs' \) -print0)
echo "补了 $n 个文件的 BOM"
