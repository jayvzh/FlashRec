// annexb_scan.h —— Annex-B NAL 扫描/参数集抽取（d316，纯函数、可离线测试）
//
// 用途：跳直播前沿重载后，mpv 的新解码器从 GOP 中段接入 —— HEVC/H.264 解码需要
// 「参数集（VPS/SPS/PPS）+ 随机访问点（IDR/IRAP）」，手机在流中途两者都不补发
// （真机 22:25 现场：`PPS id out of range` 刷 19801 条、mpv core-idle 5 分钟）。
// 数据面据此：①会话首帧缓存参数集 ②重连后视频丢弃对齐到下一个 IDR。
//
// 判码规则（d294 教训：判「流里有什么」必须用该编码自己的 NAL 规则）：
//   H.264：nal_type = b[0] & 0x1f；IDR=5；参数集 = SPS(7)+PPS(8)
//   HEVC：nal_type = (b[0] >> 1) & 0x3f；IRAP=16..23（含 BLA/CRA/IDR）；参数集 = VPS(32)+SPS(33)+PPS(34)
// 起始码：00 00 01（3 字节）与 00 00 00 01（4 字节）都识别。
#ifndef FR_CAST_AIRPLAY_ANNEXB_SCAN_H
#define FR_CAST_AIRPLAY_ANNEXB_SCAN_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fr::cast {

enum AnnexbFlag : unsigned {
  kAnnexbIdr = 1u << 0,    // 含随机访问点（H264 IDR / HEVC IRAP）
  kAnnexbParams = 1u << 1, // 含参数集（H264 SPS+PPS / HEVC VPS+SPS+PPS）
};

// 扫描一个 Annex-B AU 的 NAL 类型集合（kAnnexbIdr | kAnnexbParams）。
inline unsigned annexb_scan(const uint8_t* d, size_t n, bool hevc) {
  unsigned flags = 0;
  size_t i = 0;
  while (i + 2 < n) {
    if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) {
      i++;
      continue;
    }
    const size_t nal = i + 3;  // NAL 头起始（越过 3 字节起始码；4 字节码多出的 00 属上一 NAL，不影响类型判读）
    if (nal >= n) break;
    const int t = hevc ? ((d[nal] >> 1) & 0x3f) : (d[nal] & 0x1f);
    if (hevc) {
      if (t >= 16 && t <= 23) flags |= kAnnexbIdr;
      if (t == 32 || t == 33 || t == 34) flags |= kAnnexbParams;
    } else {
      if (t == 5) flags |= kAnnexbIdr;
      if (t == 7 || t == 8) flags |= kAnnexbParams;
    }
    i = nal;  // 从 NAL 头继续找下一个起始码
  }
  return flags;
}

// 抽取 AU 中的参数集 NAL（各带 4 字节起始码 00 00 00 01，Annex-B）。
// 无参数集时返回空 vector。
inline std::vector<uint8_t> annexb_extract_params(const uint8_t* d, size_t n, bool hevc) {
  std::vector<uint8_t> out;
  static const uint8_t sc[4] = {0, 0, 0, 1};
  size_t i = 0;
  while (i + 2 < n) {
    if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) {
      i++;
      continue;
    }
    const size_t nal = i + 3;
    if (nal >= n) break;
    const int t = hevc ? ((d[nal] >> 1) & 0x3f) : (d[nal] & 0x1f);
    const bool param = hevc ? (t == 32 || t == 33 || t == 34) : (t == 7 || t == 8);
    // 找下一个起始码（NAL 结束边界；4 字节码的前导 00 不属于本 NAL）
    size_t j = nal;
    while (j + 2 < n) {
      if (d[j] == 0 && d[j + 1] == 0 && d[j + 2] == 1) break;
      j++;
    }
    size_t end = (j + 2 < n) ? j : n;
    while (end > nal && d[end - 1] == 0) end--;  // 去掉尾随 0（属于下一起始码）
    if (param && end > nal) {
      out.insert(out.end(), sc, sc + 4);
      out.insert(out.end(), d + nal, d + end);
    }
    i = nal;
  }
  return out;
}

}  // namespace fr::cast

#endif
