#pragma once
// 崩溃自捕获（平台差异，仅 _WIN32 有实体；其余平台空实现）。
// 进程级未处理异常 → 写 minidump 到日志目录，事后定位崩溃模块/偏移，
// 不再靠"时隐时现的段错误 + 无现场"排查。
#include <string>

namespace fr::crash_dump {

// 须在 paths::init() 之后调用（需要日志目录）。
void install(const std::string& log_dir);

// —— d282：主线程心跳 + 卡死看门狗 ——
// 背景：2026-09-30 两起真机 AppHang（进程"停止与 Windows 交互"被系统结束）——
// 这类故障**没有**任何异常、没有 dump、日志在卡死瞬间整齐断掉，排查时手上是空的
// （只能靠"日志最后一行 + WER 归档"反推）。看门狗让程序自己在卡死时交出现场。
//
// beat()：主线程每轮调一次（一次原子写，纳秒级）。**只能在主线程调**。
// start_hang_watchdog(hwnd, stall_ms)：起看门狗线程。判据是**双条件**：
//   ① 主线程心跳停滞 > stall_ms；
//   ② 窗口确实不响应 —— IsHungAppWindow(hwnd) 为真；hwnd 已销毁（IsWindow 假）
//      时视为真（退出期僵尸正是这种形态：窗没了、主线程却卡在收尾里）。
//   ② 的作用是排除**合法长阻塞**：用户拖拽窗口时主线程会陷在系统模态循环里
//      出不来的同时**消息仍在泵**（GLFW FAQ 3.5，本项目的渲染线程就是为它拆的），
//      IsHungAppWindow 返回假 ⇒ 不误抓。
// 命中即写 minidump 到日志目录（hang-*.dmp，含全部线程栈；MiniDumpNormal 已
// 足够走栈）并 FR_LOG_ERROR。**不杀进程**：真卡死交给系统处置，假阳性只多一份
// dump + 一条日志。每进程最多 3 份、两份间隔 ≥15s（防病态刷盘）。
// stop_hang_watchdog()：退出前调（join 掉看门狗线程，避免它在 spdlog 析构后
// 还去写日志）。最多阻塞一个 250ms 轮询间隔。
void beat();
void start_hang_watchdog(void* hwnd, int stall_ms);
void stop_hang_watchdog();

}  // namespace fr::crash_dump
