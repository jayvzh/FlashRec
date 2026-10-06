#pragma once
// audio_wrap：UxPlay 裸音频帧 → 可探测流容器（AIRPLAY_EMBED_PLAN.md §5.2）。
//
// 背景：lib 的 audio_process 给的是**已解密的裸压缩帧**（gstreamer 靠 caps 的
// codec_data 传参；ADTS 头在 UxPlay 十年实测中从未出现，audio_renderer.c:330），
// mpv/lavf 探测不了裸 AAC-ELD（ELD AOT=39 超出 ADTS 2-bit profile 域）与裸 ALAC。
// 三种包装（纯字节操作，无状态机外依赖）：
//   · AAC-ELD（ct=8）→ LOAS/LATM AudioSyncStream（每帧内联 StreamMuxConfig + ASC；
//     lavf "loas"/TS stream_type 0x11 均可解）；
//   · ALAC（ct=2）→ CAF 流（data chunk size = -1 未定长，lavf caf 支持）；
//   · AAC-LC（ct=4）→ ADTS（预留：cn 限制后基本不出现）。
//
// 常量出处（协议常量，非 GPL 代码逻辑；§12 许可注记覆盖）：
//   · ELD ASC = f8 e8 50 00 —— UxPlay audio_renderer.c:68 的 codec_data
//    （解码 = AOT39/44100/2ch/frameLength 480，逐位验证过）；
//   · ALAC magic cookie（36B）—— UxPlay audio_renderer.c:61-62 的 caps codec_data。

#include <cstddef>
#include <cstdint>

namespace fr::cast {

// ELD AudioSpecificConfig（AOT39 / 44100 / 2ch / frameLengthFlag=1 → spf 480）
// 32 位原始值 f8 e8 50 00；LOAS 内联时 FFmpeg 的 ELD 路径只消费前 30 位
//（mpeg4audio 19 + ELDSpecificConfig 11，末 2 位是结构外补零），写 30 位。
inline constexpr uint8_t kEldAsc[4] = {0xf8, 0xe8, 0x50, 0x00};
inline constexpr uint32_t kEldAscBits30 = 0xF8E85000u >> 2;  // 前 30 位

// AAC-ELD 裸帧 → LOAS AudioSyncStream。
// ★d275 起镜像 TS 路径不再经此（改裸帧直通 + PMT 描述符供参，见 ts_muxer.cpp
// emit_psi——LOAS 锁死 aac_latm 解码器，容器无法供 sample_rate → 音频全灭）。
// 本函数保留给 AudioOnly 回环通道（latm demuxer 可探测）与潜在直连场景。
// 帧头 3B（syncword 0x2B7 + audioMuxLengthBytes）+ 内联 audioMuxElement
//（useSameStreamMux=0 每帧重申 StreamMuxConfig：ASC(30b) + frameLengthType=0 +
// BufferFullness + otherDataPresent=0 + crcCheckPresent=0 + MuxSlotLengthBytes 链）
// + 载荷（★不字节对齐，紧跟长度链按当前位相位写入——FFmpeg 从当前位直接续读）。
// ★d275 修复记录：ISO 14496-3 StreamMuxConfig 在 BufferFullness 后有
// otherDataPresent/crcCheckPresent 两个语法位，此前漏写导致 FFmpeg 把长度链
// 当 other-data 解析、声明载荷长度错成杂凑值（"frame length mismatch" 每帧
// 必拒）；ASC 写 30 位（FFmpeg ELD 路径恰好消费 30 位）。
// 返回写出字节数；out_cap 不足返回 0（调用方按帧丢弃）。
size_t latm_wrap(const uint8_t* frame, size_t len, uint8_t* out, size_t out_cap);

// AAC-LC 裸帧 → ADTS（无 ASC 输入场景的兜底；profile=LC）。返回写出字节数。
size_t adts_wrap(const uint8_t* frame, size_t len, uint8_t* out, size_t out_cap);

// ALAC：CAF 流头部（首包一次性）+ 后续裸帧透传。
// write_header 输出 [CAF 头 | kuki(36B cookie) | data chunk(size=-1)]；之后每次
// wrap(frame) = memcpy 透传。cookie 原样取自 UxPlay audio_renderer.c:61-62。
class CafWriter {
 public:
  // 生成 CAF 头（文件头 + AudioDescription chunk(kuki) + data chunk 头）。
  // 返回字节数；out_cap 不足返回 0。会话首包前调用一次。
  size_t write_header(uint8_t* out, size_t out_cap);
  // 裸帧透传（返回字节数；out_cap 不足返回 0）
  size_t wrap(const uint8_t* frame, size_t len, uint8_t* out, size_t out_cap);
};

}  // namespace fr::cast
