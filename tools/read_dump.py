#!/usr/bin/env python3
"""解析 minidump：异常流 + 线程表 + RIP 模块映射 + 栈内返回地址扫描（x64 无帧指针启发式）。

用法:
  python tools/read_dump.py <dump.dmp>                  # 有异常流时默认扫崩溃线程
  python tools/read_dump.py <dump.dmp> --scan-tid TID
  python tools/read_dump.py <dump.dmp> --scan-all       # 逐线程扫（看门狗 hang dump 用）

★d282：补上 ExceptionStream（类型 6）解析 —— 退出期"沉默 dump"（日志早已关闭、只剩
  一个 crash-*.dmp 却无 [CRASH] 行）**只能靠这条流**定位是谁在哪儿崩的。
"""
import struct
import sys

# stream types
ThreadListStream, ModuleListStream, ExceptionStream, Memory64ListStream = 3, 4, 6, 9

# 常见异常码（含 crash_dump.cpp 的伪造码）
_EXC_NAMES = {
    0xC0000005: "ACCESS_VIOLATION",
    0xC000001D: "ILLEGAL_INSTRUCTION",
    0xC0000094: "INT_DIVIDE_BY_ZERO",
    0xC00000FD: "STACK_OVERFLOW",
    0xC0000374: "HEAP_CORRUPTION",
    0xC0000409: "__fastfail/STACK_BUFFER_OVERRUN",
    0x80000003: "BREAKPOINT",
    0xE06D7363: "C++ EXCEPTION (throw)",
    0xE000AB01: "伪造: abort()/SIGABRT",
    0xE000AB02: "伪造: std::terminate",
    0xE000AB03: "伪造: 纯虚函数调用",
    0xE000AB04: "伪造: CRT invalid parameter",
}


class Dump:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        sig, ver, n, dir_rva = struct.unpack_from("<IIII", self.d, 0)
        assert sig == 0x504D444D, "not MDMP"
        self.mods = []  # (base, size, name)
        self.threads = []  # dict per thread
        self.mem = []  # (start, size, file_off) from Memory64ListStream
        self.exc = None  # ExceptionStream
        for i in range(n):
            t, dsz, rva = struct.unpack_from("<III", self.d, dir_rva + 12 * i)
            if t == ModuleListStream:
                self._modules(rva)
            elif t == ThreadListStream:
                self._threads(rva)
            elif t == ExceptionStream:
                self._exc(rva)
            elif t == Memory64ListStream:
                self._mem64(rva)

    def _exc(self, rva):
        """MINIDUMP_EXCEPTION_STREAM(168B): ThreadId(4)+pad(4)+ExceptionRecord(152)+Context(4)+pad(4)
        MINIDUMP_EXCEPTION(152B): Code(4)+Flags(4)+Record(8)+Address(8)+NParams(4)+pad(4)+Info[15](120)"""
        tid, = struct.unpack_from("<I", self.d, rva)
        code, = struct.unpack_from("<I", self.d, rva + 8)
        flags, = struct.unpack_from("<I", self.d, rva + 12)
        addr, = struct.unpack_from("<Q", self.d, rva + 24)
        nparams, = struct.unpack_from("<I", self.d, rva + 32)
        params = list(struct.unpack_from("<15Q", self.d, rva + 40))
        self.exc = dict(tid=tid, code=code, flags=flags, addr=addr,
                        nparams=nparams, params=params[:max(0, min(15, nparams))])

    def _mem64(self, rva):
        n, base_rva = struct.unpack_from("<QQ", self.d, rva)
        off = base_rva
        for i in range(n):
            start, size = struct.unpack_from("<QQ", self.d, rva + 16 + 16 * i)
            self.mem.append((start, size, off))
            off += size

    def read_va(self, addr, size):
        """Read virtual memory via Memory64ListStream (full-memory dumps)."""
        out = bytearray()
        while size > 0:
            hit = None
            for start, sz, foff in self.mem:
                if start <= addr < start + sz:
                    hit = (start, sz, foff)
                    break
            if not hit:
                out += b"\x00" * size
                return bytes(out)
            start, sz, foff = hit
            take = min(sz - (addr - start), size)
            out += self.d[foff + (addr - start): foff + (addr - start) + take]
            addr += take
            size -= take
        return bytes(out)

    def _modules(self, rva):
        (n,) = struct.unpack_from("<I", self.d, rva)
        for i in range(n):
            b = rva + 4 + 108 * i
            base, size = struct.unpack_from("<QQ", self.d, b, )[0], struct.unpack_from("<I", self.d, b + 8)[0]
            name_rva = struct.unpack_from("<I", self.d, b + 20)[0]
            (slen,) = struct.unpack_from("<I", self.d, name_rva)
            name = self.d[name_rva + 4: name_rva + 4 + slen].decode("utf-16le", "replace")
            self.mods.append((base, size, name))

    def _threads(self, rva):
        (n,) = struct.unpack_from("<I", self.d, rva)
        for i in range(n):
            b = rva + 4 + 48 * i
            tid = struct.unpack_from("<I", self.d, b)[0]
            st_start = struct.unpack_from("<Q", self.d, b + 24)[0]
            st_size = struct.unpack_from("<I", self.d, b + 32)[0]
            st_rva = struct.unpack_from("<I", self.d, b + 36)[0]
            ctx_rva = struct.unpack_from("<I", self.d, b + 44)[0]
            rip = struct.unpack_from("<Q", self.d, ctx_rva + 0xF8)[0]
            rsp = struct.unpack_from("<Q", self.d, ctx_rva + 0x98)[0]
            self.threads.append(dict(tid=tid, st_start=st_start, st_size=st_size,
                                     st_rva=st_rva, rip=rip, rsp=rsp))

    def mod_of(self, addr):
        for base, size, name in self.mods:
            if base <= addr < base + size:
                short = name.replace("\\", "/").split("/")[-1]
                return f"{short}+0x{addr - base:X}"
        return None

    def read_mem(self, rva, size):
        return self.d[rva: rva + size]

    def covered(self, addr, size):
        """Memory64ListStream 是否覆盖该区间（全内存 dump 才有）。"""
        for start, sz, _foff in self.mem:
            if start <= addr and addr + size <= start + sz:
                return True
        return False

    def read_stack(self, t):
        """取线程栈内存。两条来源：
        · 全内存 dump（MiniDumpWithFullMemory，如 tools/dump_pid.py 产物）
          → Memory64ListStream 按 VA 读；
        · MiniDumpNormal（crash_dump 看门狗的 hang-*.dmp）→ 线程表自带的
          栈文件偏移 st_rva 直接读（**不依赖 Memory64ListStream**）。
        """
        if t.get("st_size"):
            if self.mem and self.covered(t["st_start"], t["st_size"]):
                return self.read_va(t["st_start"], t["st_size"])
            if t.get("st_rva"):
                return self.read_mem(t["st_rva"], t["st_size"])
        return b""

    def scan_stack(self, t):
        """栈内返回地址扫描（x64 无帧指针启发式）：返回 [(偏移, 地址, 模块+偏移)]。"""
        data = self.read_stack(t)
        lo, hi = t["st_start"], t["st_start"] + t["st_size"]
        hits = []
        for off in range(0, len(data) - 8, 8):
            (v,) = struct.unpack_from("<Q", data, off)
            if lo <= v < hi:
                continue
            m = self.mod_of(v)
            if m:
                hits.append((off, v, m))
        return hits


def main():
    path = sys.argv[1]
    scan_tid = None
    if "--scan-tid" in sys.argv:
        scan_tid = int(sys.argv[sys.argv.index("--scan-tid") + 1])
    scan_all = "--scan-all" in sys.argv
    d = Dump(path)
    print(f"modules={len(d.mods)} threads={len(d.threads)} mem_ranges={len(d.mem)}")

    fault_tid = None
    if d.exc:
        e = d.exc
        fault_tid = e["tid"]
        name = _EXC_NAMES.get(e["code"], "")
        addr_s = d.mod_of(e["addr"]) or ("raw 0x%X" % e["addr"])
        print("异常: code=0x%08X%s addr=%s 崩溃线程 TID=%d nparams=%d"
              % (e["code"], (" (" + name + ")") if name else "", addr_s,
                 fault_tid, e["nparams"]))
        if e["params"]:
            print("  参数: " + " ".join("0x%X" % p for p in e["params"]))
    else:
        print("（无异常流：这是看门狗/手动 dump，不是崩溃现场）")

    # 无显式指定时，有异常流就默认看崩溃线程
    if scan_tid is None and not scan_all and fault_tid is not None:
        scan_tid = fault_tid

    for t in d.threads:
        m = d.mod_of(t["rip"]) or f"raw 0x{t['rip']:X}"
        tid = t["tid"]
        tags = []
        if fault_tid is not None and tid == fault_tid:
            tags.append("崩溃线程")
        if scan_tid is not None and tid == scan_tid:
            tags.append("指定")
        mark = ("  <== " + "/".join(tags)) if tags else ""
        print(f"TID {tid:>6}  RIP {m:<34} rsp=0x{t['rsp']:X}{mark}")
    if scan_all:
        print("（--scan-all：下面逐线程扫栈；见上方 \"崩溃线程\" 标注）")

    tids = [t["tid"] for t in d.threads] if scan_all else ([scan_tid] if scan_tid else [])
    for tid in tids:
        t = next((x for x in d.threads if x["tid"] == tid), None)
        if t is None:
            print(f"\n(无 TID {tid})")
            continue
        lo, hi = t["st_start"], t["st_start"] + t["st_size"]
        print(f"\n== 栈扫描 TID {tid} span 0x{lo:X}-0x{hi:X} ({t['st_size']}B) ==")
        hits = d.scan_stack(t)
        if not hits:
            print("  （无模块内返回地址：栈内存缺失？）")
        prev = None
        for off, v, m in hits:
            if m.split("+")[0] == prev:
                continue
            print(f"  +0x{off:<6X} -> {m}")
            prev = m.split("+")[0]


if __name__ == "__main__":
    main()
