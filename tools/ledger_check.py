#!/usr/bin/env python3
"""FlashRec 版本台账流水号校验。

检查项：
  1. 编号 dN 全局连续（d1..dN），不跳号、不重号
  2. 单本台账内部编号连续
  3. INDEX.md 中每个版本的编号区间与条数，与实际条目一致
  4. 已归档编号区间豁免（d265 翻译线归档决策）：随版本线整体归档到
     git 分支的编号区间在主线 ledger/ 缺席属预期，不判断号、只提示

用法：
    python tools/ledger_check.py       # 退出码 0 = 通过，1 = 台账损坏
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LEDGER_DIR = ROOT / "ledger"
INDEX = LEDGER_DIR / "INDEX.md"
SKIP = {"README.md", "INDEX.md"}

ENTRY_RE = re.compile(r"^###\s+d(\d+)\s*[·:：\-]")
VER_RE = re.compile(r"^v[\w.\-]+$")
RANGE_RE = re.compile(r"^d(\d+)\s*[–\-~至]\s*d(\d+)$")

# 已归档编号区间（d265 决策：版本线终止后整体归档到 git 分支，主线台账不再持有）。
# 格式：(起始编号, 结束编号, 归档分支)。新增归档线时在此登记。
ARCHIVED_RANGES = [
    (229, 264, "archive/translate-line-v0.6.0"),
]


def parse_index() -> tuple[list[tuple[str, str, int, int, int | None, str]], list[str]]:
    """解析 INDEX.md 表格 -> [(版本, 文件名, 起始号, 结束号, 条数, 状态)]"""
    rows: list[tuple[str, str, int, int, int | None, str]] = []
    problems: list[str] = []
    if not INDEX.is_file():
        return rows, ["找不到 ledger/INDEX.md"]
    for line in INDEX.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) < 6:
            continue
        ver, fname, rng, _dates, cnt, status = cells[:6]
        if not VER_RE.match(ver):
            continue
        fn = fname.strip("`").strip()
        m = RANGE_RE.match(rng)
        if not m:
            problems.append(f"INDEX.md：{ver} 的编号区间 '{rng}' 无法解析（应为 dA – dB）")
            continue
        try:
            count = int(cnt)
        except ValueError:
            count = None
            problems.append(f"INDEX.md：{ver} 的条数 '{cnt}' 不是数字")
        rows.append((ver, fn, int(m.group(1)), int(m.group(2)), count, status))
    if not rows:
        problems.append("INDEX.md：没有解析到任何版本行")
    return rows, problems


def main() -> int:
    problems: list[str] = []
    notes: list[str] = []

    if not LEDGER_DIR.is_dir():
        print("[FAIL] 找不到 ledger/ 目录")
        return 1

    files = sorted(p for p in LEDGER_DIR.glob("*.md") if p.name not in SKIP)
    if not files:
        print("[FAIL] ledger/ 下没有台账文件")
        return 1

    per_file: dict[str, list[int]] = {}
    owners: dict[int, list[str]] = {}

    for f in files:
        nums: list[int] = []
        for line in f.read_text(encoding="utf-8").splitlines():
            m = ENTRY_RE.match(line.strip())
            if m:
                nums.append(int(m.group(1)))
        per_file[f.name] = nums
        if not nums:
            problems.append(f"{f.name}：未解析到条目（标题行须为 '### dN · 标题'）")
        for a, b in zip(nums, nums[1:]):
            if b != a + 1:
                problems.append(f"{f.name}：本台账内编号不连续，d{a} 之后直接跳到 d{b}")
        for n in nums:
            owners.setdefault(n, []).append(f.name)

    for n, fs in sorted(owners.items()):
        if len(fs) > 1:
            problems.append(f"d{n} 重复出现于 {', '.join(fs)}（编号全局唯一，禁止复用）")

    nums = sorted(owners)
    if nums:
        missing = [n for n in range(1, nums[-1] + 1) if n not in owners]
        true_missing = [
            n for n in missing
            if not any(lo <= n <= hi for lo, hi, _ in ARCHIVED_RANGES)
        ]
        for lo, hi, br in ARCHIVED_RANGES:
            in_range = [n for n in missing if lo <= n <= hi]
            if in_range:
                notes.append(
                    f"d{min(in_range)}–d{max(in_range)} 属已归档区间（分支 {br}），豁免"
                )
        if true_missing:
            problems.append("断号：缺失 " + ", ".join(f"d{n}" for n in true_missing))
        if not true_missing:
            notes.append(
                f"主线编号连续（含归档豁免）至 d{nums[-1]}，主线持有 {len(nums)} 条"
            )
            notes.append(f"下一个可用编号：d{nums[-1] + 1}")

    rows, idx_problems = parse_index()
    problems.extend(idx_problems)
    for ver, fn, lo, hi, count, _status in rows:
        if fn not in per_file:
            problems.append(f"INDEX.md：{ver} 指向的 '{fn}' 不存在于 ledger/")
            continue
        actual = per_file[fn]
        if not actual:
            continue
        amin, amax = min(actual), max(actual)
        if (lo, hi) != (amin, amax):
            problems.append(
                f"INDEX.md：{ver} 区间写的是 d{lo} – d{hi}，"
                f"实际是 d{amin} – d{amax}（{fn}）"
            )
        if count is not None and count != len(actual):
            problems.append(
                f"INDEX.md：{ver} 条数写的是 {count}，实际 {len(actual)}（{fn}）"
            )

    print("=" * 56)
    print(" FlashRec 台账校验")
    print("=" * 56)
    for n in notes:
        print(f"  [OK]   {n}")
    if problems:
        for p in problems:
            print(f"  [FAIL] {p}")
        print("-" * 56)
        print(f" 结果：台账损坏，{len(problems)} 处问题待修复")
        return 1
    print("-" * 56)
    print(" 结果：通过 —— 编号连续、无重号、与 INDEX 一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
