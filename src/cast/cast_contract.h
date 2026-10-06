// cast_contract.h —— 投屏源契约（v0.6.0 d266，AirPlay 线从零重写）
//
// == 许可注记（D4 拍板：随 AirPlay 引入 UxPlay 后本项目整体 GPLv3）==
//   · src/cast/ 适配器与 third_party/uxplay/（UxPlay lib，GPLv3）同进程链接，
//     因此携带 AirPlay 的 FlashRec 发行版以 GPLv3 授权；UxPlay 的版权与许可
//     声明随 third_party/uxplay/ 原样分发（本仓库不改其源码一行，规则 8）。
//   · 协议常量（ASC/cookie/features 位表等）为 AirPlay 协议规格事实，不构成
//     GPL 代码逻辑；适配器实现中的行为结论（注释里的行号出处）为自由文本。
//   · 关闭 FR_AIRPLAY 编译时（deps.cmake option 默认 OFF）不链接 UxPlay，
//     产物不含 GPL 组件。
//
// 本头是 src/cast/ 的契约文档头：规定投屏源（当前仅 AirPlay）与 FlashRec
// 播放栈（player/mpv）之间的数据面边界。控制面走 EventBus（DmrCommand /
// AirplayEvent），数据面走本机回环 TCP（stream_local）——两条流各自成流，
// 由适配器统一到一条媒体时间轴后喂 mpv。
//
// == 数据面三通道（AIRPLAY_EMBED_PLAN.md §5）==
//   1. 镜像视频：H.264 Annex-B（lib 已含 SPS/PPS 前置）→ 回环 TCP → mpv
//   2. 音频：    裸 AAC-ELD 帧（ELD 每帧三发，按 timestamp 去重）→
//                封装（M0.5 spike 定形，预期 LOAS）→ 回环 TCP → mpv
//   3. HLS/URL： 不走回环 —— location 直载 mpv（on_video_play → SetUri）
//
// == 控制面三路径（AIRPLAY_EMBED_PLAN.md §6）==
//   ① 写路径：回调线程只投 DmrCommand（source=AirPlay 标记会话来源；
//      load_format=回流裸流的 demuxer 提示），禁止跨线程改状态。
//   ② 读路径：EventBus::snapshot() 唯一真值快照（构协议应答用），禁缓存副本。
//   ③ 反馈事件：AirplayEvent 独立队列（Pin/ClientConnected/MirrorStarted 等），
//      主循环 drain_airplay 消费。
//
// == 时基契约（D1 定稿：音频采样域主钟）==
//   - 音频 PTS = RTP timestamp / 44100（采样计数直推，天然连续，主钟）
//   - 视频 PTS = 音频媒体轴 +（帧头 NTP − 锚点 NTP）；首包锚定一次，永不重校
//   - 音频为基准流：永不丢、不拉伸；视频跟随 mpv audio-master（早等/晚丢）
//
// == 适配器硬性纪律（UxPlay 深挖结论，逐条带出处）==
//   1. 回调数据是临时所有权 —— 返回前同步拷贝（raop_rtp.c:668 / raop_rtp_mirror.c:557）
//   2. httpd 单线程 select：全部信令回调必须快速返回（httpd.c:699）
//   3. 回调来自 httpd/rtp/mirror 三线程：控制面走 EventBus，数据面直写回环 socket
//   4. ELD 音频按 timestamp 去重（每帧三发，raop_rtp.c:573-575）
//   5. 解密自检：合法 ELD/ALAC 帧首字节 ∈ {8c,8d,8e,80,81,82,20}（raop_buffer.c:139-150）
//   6. video_set_codec 必须实现（mirror 路径 assert，raop_rtp_mirror.c:591）
//   7. NOHOLD 编译期单客户端（第二个客户端踢第一个）
//   8. lib 内 exit() 热点登记（crypto.c:56 等）—— M4 评估锚点改造
//   9. video_process 输入已是 Annex-B 且 SPS/PPS 已前置 —— 适配器不再处理 H.264 语法
//
// == 回流 URI 规范 ==
//   mpv 入口统一 "tcp://127.0.0.1:<port>"，demuxer 格式由适配器显式指定
//   （demuxer-lavf-format），不依赖 mpv 探测。回流 URI 不得进 last_uri_
//   （player_controller 重连语义只对 DLNA 直链有效）。
#pragma once

namespace fr::cast {

// 回环流 URI 前缀（stream_local 只绑 127.0.0.1；端口动态分配后拼在此后）
inline constexpr const char* kLoopbackUriPrefix = "tcp://127.0.0.1:";

}  // namespace fr::cast
