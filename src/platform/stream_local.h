// 本机回环流服务端 —— AirPlay 数据面的进程内 TCP 饲喂通道
// （AIRPLAY_EMBED_PLAN.md §4.1：镜像 H.264 裸流与音频帧经这里喂 mpv）。
//
// 为什么选 127.0.0.1 TCP（而非命名管道/FIFO）：双平台一套实现、天然 EOF
// （close = 对端读到 0）、无 FIFO 的阻塞 open 语义、mpv/lavf 原生支持 tcp://。
//
// 安全边界（§11.4）：本服务**只绑 127.0.0.1**，回环流不暴露到任何外网接口；
// 流量无认证但远程不可达，且预期只有一个客户端（mpv，backlog = 1）。
//
// 风格与依赖同 net_if.h：纯 C、无日志依赖、错误全走返回值；
// 平台宏只出现在 stream_local.c（AGENTS.md 规则 5）。
//
// 线程说明：listen fd 可由 lib 线程 accept（竞态无害，只会有 mpv 一个客户端
// 连上来）；fr_stream_write 仅各自的数据线程调用，多个 conn fd 互不相干。

#ifndef FR_PLATFORM_STREAM_LOCAL_H
#define FR_PLATFORM_STREAM_LOCAL_H

#ifdef __cplusplus
extern "C" {
#endif

// 创建回环 listen socket：绑定 127.0.0.1。
//   *port 入参 0 = 系统自动分配；出参回填实际端口（调用方据此拼
//   tcp://127.0.0.1:<port> 交给 mpv）。
// 返回 listen fd；失败（参数非法 / bind / listen / getsockname）返回 -1。
// listen fd 是非阻塞 socket（accept 的超时等待依赖它），只能传给
// fr_stream_accept() 与 fr_stream_close()。
int fr_stream_listen(unsigned short* port);

// conn 的**写超时**（毫秒）。与 accept 等待解耦：accept 允许 timeout_ms<=0
// （立返回，绝不为等连接而阻塞数据线程），但**写永远有界**（恒为本常量）。
// ★d282：无界写是唯一能让数据线程永久挂死、并把"持同一把锁的主线程"一起拖死的
//   路径 —— 2026-09-30 真机两起 AppHang（暂停后 mpv 不再读回环流 → send 永久阻塞
//   → 攥住 Channel::mu_ → 主线程 5s 汇总取该锁 → 消息泵停摆）的根因就在这里。
// ★★Win32 上这是 **join() 的唯一上界**：shutdown() 唤不醒正在阻塞的 send
//   （见 fr_stream_shutdown 的平台差异说明），所以本常量**绝不能再被派生/放宽** ——
//   否则 stop 路径重新变成一个"等多久由调用方随手传的数"决定的隐藏延迟。
#define FR_STREAM_WRITE_TIMEOUT_MS 250

// fr_stream_write 的三态返回（调用方据此区分"对端还活着只是不读"与"通道死了"）
#define FR_STREAM_WRITE_OK 0     // 全部写出
#define FR_STREAM_WRITE_STALL 1  // 超时/暂不可写：对端活着但不消费（mpv 暂停/缓存满）
#define FR_STREAM_WRITE_ERROR (-1)  // 硬错误：对端关闭/参数非法，通道已死

// 在 lfd 上等待并接收一个连接，最多等 timeout_ms 毫秒。
// 新 conn 的属性（都是硬性契约，任一 setsockopt 失败即整体失败）：
//   · 阻塞模式 —— POSIX 下 accept 会从 listener 继承非阻塞，这里显式改回；
//   · SO_SNDTIMEO = FR_STREAM_WRITE_TIMEOUT_MS 恒常量（**写恒有界**，且与
//     accept 等待**无关** —— 传入多大的 accept 超时都不会放宽写超时）；
//     SO_RCVTIMEO = timeout_ms（0 = 不设，本模块从不读）；
//   · TCP_NODELAY（AIRPLAY_EMBED_PLAN.md §5.3 细则⑤：音频 LATM 帧仅数百
//     字节，Nagle 攒包最多可加 ~200ms 延迟，必须禁掉）。
// 超时 / 无连接 / 出错返回 -1，**lfd 仍然有效**（可继续 accept 或 close）。
int fr_stream_accept(int lfd, int timeout_ms);

// 全量写 len 字节：阻塞循环 send 直到全部完成（单次阻塞受 conn 的 SO_SNDTIMEO
// 约束）。返回 FR_STREAM_WRITE_OK / _STALL / _ERROR 三态 —— 超时与硬错误必须
// 分开：超时（对端活着不读）的正确处置是**丢这一片、管道继续跑**，硬错误才收尾。
// EPIPE 的静默语义（不崩进程、不打印——POSIX 侧用 MSG_NOSIGNAL 压掉 SIGPIPE）
// 由本模块保证。
int fr_stream_write(int fd, const void* buf, int len);

// 禁止后续收发（shutdown(SD_BOTH)），使**此后的**写立刻以硬错误失败。
// ★d282：可在任意线程调用，且调用方无需持有任何锁 —— stop 路径用它让 mux 线程在
//   下一轮循环立刻收尾（send 拿到 WSAESHUTDOWN / EPIPE → _ERROR）。
//
// ⚠️平台差异（判决实验 build/test_ts/test_shutdown_wake.cpp 实测，**务必别记反**）：
//   · POSIX：shutdown 会**中断**正在阻塞的 send()（该 send 立刻返回）。
//   · Win32：shutdown **不中断**正在阻塞的 send() —— 实测把 SO_SNDTIMEO 设成 8s、
//     对端只连不读，shutdown 之后那次 send 仍要等满 7504ms 才以 WSAETIMEDOUT 返回。
//   ⇒ **Win32 上 join() 真正的界是 FR_STREAM_WRITE_TIMEOUT_MS（250ms），不是本函数。**
//     本函数只保证"下一轮 send 不会再来一次 250ms"（省掉一次超时等待）。这就是写超时
//     必须恒为小常量、绝不与 accept 等待耦合的原因 —— 否则 Windows 上随时真挂。
// 幂等；fd 无效 / 未连接 / 已关返回 -1（调用方忽略即可，不抛错）。
int fr_stream_shutdown(int fd);

// 关闭 fd（listen fd 与 conn fd 均用此函数）；fd < 0 为空操作。
void fr_stream_close(int fd);

#ifdef __cplusplus
}
#endif

#endif  // FR_PLATFORM_STREAM_LOCAL_H
