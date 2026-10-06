#include "platform/crash_dump.h"

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <thread>

#include <crtdbg.h>  // _set_purecall_handler / _set_invalid_parameter_handler（MSVC）

#include "app/log.h"

#pragma comment(lib, "dbghelp.lib")

namespace fr::crash_dump {

namespace {

std::string g_dir;
std::atomic<bool> g_dumping{false};  // 递归保护：崩溃处理中再崩 → 直接退

// —— 伪造崩溃码约定：0xE000xxxx = 非硬件异常的"语义死法"，dump 里可辨识 ——
constexpr DWORD kCodeAbort = 0xE000AB01;         // abort()/SIGABRT（CRT 层）
constexpr DWORD kCodeTerminate = 0xE000AB02;     // std::terminate（未捕获 C++ 异常）
constexpr DWORD kCodePurecall = 0xE000AB03;      // 纯虚函数调用
constexpr DWORD kCodeInvalidParam = 0xE000AB04;  // CRT invalid parameter

std::string dump_path(const char* prefix) {
  char name[128];
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _MSC_VER
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  snprintf(name, sizeof(name), "%s-%04d%02d%02d-%02d%02d%02d-%lu.dmp", prefix, tm.tm_year + 1900,
           tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
           (unsigned long)GetCurrentProcessId());
  return g_dir + "/" + name;
}

// 统一出口：写 dump → 日志留证 → 进程立即退出。
// ★不可 return：abort() 在信号 handler 返回后会继续 __fastfail（绕过一切用户态
//   handler，进程瞬死、证据全丢），所以每条非 SEH 路径写完 dump 必须 ExitProcess。
void write_dump_then_die(const char* reason, DWORD code, EXCEPTION_POINTERS* ep) {
  bool expected = false;
  if (!g_dumping.compare_exchange_strong(expected, true)) {
    ExitProcess(3);  // 崩溃处理中再崩：防递归，放弃第二次取证
  }
  const std::string path = dump_path("crash");
  HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    // ★d282 修正（判决实验 build/test_ts/test_dump_types.cpp，实测逐位对照）：
    //   d271 加的 MiniDumpWithIndirectlyReferencedMemory 会**让 dbghelp 整份丢掉线程栈**
    //   —— 同进程同一次崩溃，只换类型位：Normal/Normal+DataSegs/Normal+ThreadInfo+DataSegs
    //   三种都有 stack.rva（0x41C2/0x1690A/0x16A16），一旦 OR 上 IndirectlyReferencedMemory
    //   立刻变成 rva=0x0（与是否传 ExceptionParam 无关）。后果：退出期崩溃的 dump 只剩
    //   "异常码 + 一个地址"，连崩溃线程的调用栈都没有，等于没有证据（2026-09-30 三份
    //   crash-*.dmp 全是这样）。
    //   ⇒ 改用与看门狗 hang dump **完全相同**的类型：Normal | WithThreadInfo | WithDataSegs。
    //   它已实测保留线程栈，同时给出线程起址/时间与数据段。堆损坏若真需要指针目标内存，
    //   再单独引入 FullMemory（体积代价另算），绝不牺牲栈。
    const MINIDUMP_TYPE mtype = static_cast<MINIDUMP_TYPE>(MiniDumpNormal |
                                                           MiniDumpWithThreadInfo |
                                                           MiniDumpWithDataSegs);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, mtype, ep ? &mei : nullptr,
                      nullptr, nullptr);
    CloseHandle(f);
    FR_LOG_ERROR("[CRASH] {}（code 0x{:08X}），minidump: {}", reason, (unsigned)code, path);
  } else {
    FR_LOG_ERROR("[CRASH] {}（minidump 写入失败）", reason);
  }
  ExitProcess(3);
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
  write_dump_then_die("未处理异常（SEH）", ep && ep->ExceptionRecord
                                                ? ep->ExceptionRecord->ExceptionCode
                                                : 0,
                      ep);
  return EXCEPTION_EXECUTE_HANDLER;  // 不可达（write_dump_then_die 内 ExitProcess）
}

// 非硬件异常的死法：RaiseException 伪造异常瞬间被 __except 捕获，得到真实的
// 线程 CONTEXT 与调用栈 —— dump 结构与真崩溃完全同构，dump_who.py 可直接解析。
void fake_crash_and_dump(const char* reason, DWORD code) {
  __try {
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
  } __except (on_crash(GetExceptionInformation())) {
    // 不可达：on_crash → write_dump_then_die → ExitProcess
  }
}

void on_signal_abort(int) { fake_crash_and_dump("abort()（SIGABRT，CRT 层死法）", kCodeAbort); }

void on_terminate() {
  fake_crash_and_dump("std::terminate（未捕获 C++ 异常）", kCodeTerminate);
}

void on_purecall() { fake_crash_and_dump("纯虚函数调用", kCodePurecall); }

void on_invalid_param(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
  fake_crash_and_dump("CRT invalid parameter", kCodeInvalidParam);
}

// GUI 子系统没有控制台：assert 文本、lib 内 printf 全走 stderr 凭空消失。
// 重定向到日志目录（无缓冲，崩溃前必落盘）；失败无妨，尽力而为。
void redirect_stdio(const std::string& dir) {
  FILE* err = freopen((dir + "/stderr.log").c_str(), "w", stderr);
  if (err) setvbuf(err, nullptr, _IONBF, 0);
  FILE* out = freopen((dir + "/stdout.log").c_str(), "w", stdout);
  if (out) setvbuf(out, nullptr, _IONBF, 0);
}

// ——————————————— d282：主线程心跳 + 卡死看门狗 ———————————————

std::atomic<uint64_t> g_beat_ms{0};    // 主线程最近一次心跳（GetTickCount64）
std::atomic<bool> g_watch_stop{false};
std::thread g_watch_thr;
std::atomic<int> g_hang_dumps{0};      // 已抓份数（限流，防病态刷盘）
uint64_t g_last_hang_dump_ms = 0;      // 上次抓取时刻（仅看门狗线程读写）
constexpr int kMaxHangDumps = 3;
constexpr uint64_t kHangDumpMinGapMs = 15000;

// 判据第二条：窗口**真的**不响应吗？
//   · 窗口还在   → 交给 IsHungAppWindow（拖拽/模态循环里消息仍在泵，返回假）；
//   · 窗口已销毁 → 视为真（退出期僵尸：窗没了、主线程却卡在收尾里，正是 d150 那类）。
bool window_is_hung(void* hwnd) {
  if (!hwnd) return true;
  const HWND h = static_cast<HWND>(hwnd);
  if (!IsWindow(h)) return true;
  return IsHungAppWindow(h) != FALSE;
}

// 卡死取证：写一份 minidump（含全部线程栈），**然后继续运行** —— 不 ExitProcess：
// 真卡死由系统处置（进程本来就废了），假阳性不该把用户会话一起丢掉。
// 用 MiniDumpNormal 而非 FullMemory：前者已包含"所有线程的栈"，体积仅数 MB，
// 足够还原每条线程卡在哪个调用上（tools/read_dump.py 直接从线程表取栈内存扫描）。
void write_hang_dump(int stall_ms) {
  const std::string path = dump_path("hang");
  HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    FR_LOG_ERROR("[HANG] 主线程停滞 {}ms，但 minidump 写入失败", stall_ms);
    return;
  }
  const MINIDUMP_TYPE mtype = static_cast<MINIDUMP_TYPE>(
      MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithDataSegs);
  // 走 null exception：这不是崩溃，dump 里只有线程与栈（read_dump.py 照样可解析）
  const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, mtype,
                                    nullptr, nullptr, nullptr);
  CloseHandle(f);
  if (ok)
    FR_LOG_ERROR("[HANG] 主线程停滞 {}ms 且窗口不响应 —— 已落现场 {}（tools/read_dump.py 看各线程栈）",
                 stall_ms, path);
  else
    FR_LOG_ERROR("[HANG] 主线程停滞 {}ms，MiniDumpWriteDump 失败 err={}", stall_ms,
                 (unsigned)GetLastError());
}

void watchdog_main(void* hwnd, int stall_ms) {
  while (!g_watch_stop.load(std::memory_order_relaxed)) {
    Sleep(250);
    const uint64_t now = GetTickCount64();
    const uint64_t beat = g_beat_ms.load(std::memory_order_relaxed);
    if (beat == 0) continue;                          // 主循环还没跑起来
    if (now - beat <= (uint64_t)stall_ms) continue;   // 心跳正常
    if (!window_is_hung(hwnd)) continue;              // 合法长阻塞（拖拽窗口等）
    if (g_hang_dumps.load() >= kMaxHangDumps) continue;
    if (g_last_hang_dump_ms && now - g_last_hang_dump_ms < kHangDumpMinGapMs) continue;
    g_last_hang_dump_ms = now;
    g_hang_dumps.fetch_add(1);
    write_hang_dump((int)(now - beat));
  }
}

}  // namespace

void install(const std::string& log_dir) {
  g_dir = log_dir;
  redirect_stdio(log_dir);
  SetUnhandledExceptionFilter(on_crash);
  // d271：22:24 数据面崩溃零证据复盘 —— SetUnhandledExceptionFilter 只接得住
  // SEH 硬件异常；abort()/terminate()/纯虚调用/CRT 参数校验四类死法全部绕行，
  // 进程无声蒸发。各自挂钩后统一走 write_dump_then_die 拿到同构 dump。
  std::signal(SIGABRT, on_signal_abort);
  std::set_terminate(on_terminate);
  _set_purecall_handler(on_purecall);
  _set_invalid_parameter_handler(on_invalid_param);
  // 清除 _CALL_REPORTFAULT：否则 SIGABRT handler 返回后 abort 仍会转 fastfail
  //（好在我们的 handler 不会返回，此行是双保险——防其它 CRT 路径直接 fastfail）。
  _set_abort_behavior(0, _CALL_REPORTFAULT);
}

void beat() { g_beat_ms.store(GetTickCount64(), std::memory_order_relaxed); }

void start_hang_watchdog(void* hwnd, int stall_ms) {
  if (stall_ms <= 0) stall_ms = 5000;
  g_watch_stop.store(false, std::memory_order_relaxed);
  g_watch_thr = std::thread(watchdog_main, hwnd, stall_ms);
  FR_LOG_INFO("[HANG] 主线程卡死看门狗已启动（阈值 {}ms；只在窗口不响应时抓现场，拖拽窗口不误报）",
              stall_ms);
}

void stop_hang_watchdog() {
  g_watch_stop.store(true, std::memory_order_relaxed);
  if (g_watch_thr.joinable()) g_watch_thr.join();  // 最多等一个 250ms 轮询间隔
}

}  // namespace fr::crash_dump

#else  // !_WIN32

namespace fr::crash_dump {
void install(const std::string&) {}
void beat() {}
void start_hang_watchdog(void*, int) {}
void stop_hang_watchdog() {}
}  // namespace fr::crash_dump

#endif
