#pragma once
// 事件总线：全项目唯一的跨层通信通道（AGENTS.md 规则 2）。
// - libupnp 回调线程只 publish_dmr()，绝不直接改状态 / 碰 UI（规则 4）
// - 主线程每帧 drain_*() 串行消费
// - PlaybackSnapshot 是 UI / SOAP 层读取播放状态的唯一来源（规则 3）
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace fr {

// DMR 状态机六态（唯一真值：player/player_controller，DMR_SOAP_SPEC §2）
enum class TransportState { Idle, Ready, Transitioning, Playing, PausedPlayback, Stopped };

// 投屏来源（d266 会话互斥：DLNA/AirPlay 双协议常驻监听，任意新投屏无条件
// 接管 —— 后到即最新内容，跨协议共用同一套 player 会话语义）。
enum class CastSource { Dlna, AirPlay };

const char* transport_state_name(TransportState s);   // → GetTransportInfo 用的大写串
const char* transport_state_upnp(TransportState s);   // → NO_MEDIA_PRESENT / STOPPED / ...

// 播放状态只读快照（规则 3：UI 与 SOAP 层只读，禁止缓存第二份副本）
struct PlaybackSnapshot {
  TransportState state = TransportState::Idle;
  std::string uri;            // 当前会话 URI（进度保持期内 = 停止时刻的 URI）
  std::string metadata;       // CurrentURIMetaData（DIDL-Lite 原文）
  std::string title;          // 从 DIDL dc:title 提取
  std::string next_uri;
  std::string next_metadata;
  double position = 0.0;      // 秒（真实 RelTime；保持期内 = 停止时刻）
  double duration = 0.0;      // 秒；直播恒 0（R6，禁止把滑窗时长当总时长）
  bool is_live = false;       // seekable=no 或 duration<=0（R6）
  bool seekable = true;
  bool picture_ready = false; // file-loaded，画面可显示
  int volume = 100;           // 0-100（百分比与 dB 共用此真值，R4）
  bool muted = false;
  double speed = 1.0;         // 播放倍速（d169；mpv speed 属性，1.0 = 原速）
  bool has_session = false;   // true = 有会话（含保持期）
  double stopped_at = 0.0;    // 进入 STOPPED 的单调时刻；0 = 非保持期
  CastSource source = CastSource::Dlna;  // 当前会话来源（d267；UI 徽章/协议应答）
};

// libupnp 回调线程 → 主线程 的指令（只投递，不执行）
struct DmrCommand {
  enum class Type {
    SetUri, SetNextUri, Play, Pause, Stop, Seek, SetVolume, SetMute, SelectPreset,
    SetSpeed,  // d169：本地 UI 倍速（非 UPnP 动作；不产生 AVT 事件变量）
    AddAudioTrack  // d267：AirPlay 镜像外挂配音轨（音频回环流并入既有视频会话）
  };
  Type type = Type::Play;
  std::string uri;
  std::string metadata;
  std::string preset;
  double seek_to = 0.0;       // 秒（绝对位置）
  int volume = 0;             // 0-100
  bool mute = false;
  double speed = 1.0;         // SetSpeed：倍速（0.25..4.0）
  int req_id = 0;             // req#N 日志关联号
  CastSource source = CastSource::Dlna;  // 会话来源（AirPlay 回调线程投递时置 AirPlay）
  std::string title;          // 会话名（AirPlay 客户端名 / 媒体标题；DIDL 提取归 Dlna 路径）
  std::string load_format;    // 回流裸流 demuxer 提示（"loas"/"mpegts"/"h264"；空=探测）
};

// 主线程 → DMR / UI 的事件（不可变，单向）
struct PlayerEvent {
  // d169：Speed 只更新快照供 UI 显示；倍速不是 UPnP AVT 变量，SOAP/GENA 侧忽略。
  enum class Kind { StateChanged, Position, MediaInfo, Volume, Speed };
  Kind kind = Kind::StateChanged;
  PlaybackSnapshot snap;
};

// d146 R2/R3/R4：「投屏开始」信号（独立第三队列；主线程消费，执行呈现管线
// P1 选屏 → P2 摆位 → P3 显窗 → P4 自动全屏 → P5 记忆写回）。
// 生产者 = player_controller::handle_set_uri 的两个分支（DuplicateURI 与接受新 URI
// ——二者都是"用户在投屏"）；Play/Pause/Stop/Seek/SetVolume/SetMute/SetNextUri、
// EOF、clear_session **不发**（不是投屏开始）。菜单"下一个"与剪贴板"播放"走
// handle_set_uri，天然统一。
struct CastEvent {
  std::string uri;         // 原始 URI（含 fragment）
  bool duplicate = false;  // true = DuplicateURI 分支（同 URI 二次推送；防抖参考标记）
};

// d267：AirPlay 控制面反馈事件（第四条独立队列）。生产者 = UxPlay lib 回调
// 线程（httpd/mirror 簇，任意线程可 publish）；消费者 = 主循环 drain_airplay
//（单消费者，照 casts_ 的队列所有权纪律 —— 与 events_ 抢食即丢事件）。
// 容量 16 丢最旧（PIN/连接类事件低频，16 足以吞峰值；丢最旧同 casts_ 语义）。
struct AirplayEvent {
  enum class Kind { Pin, ClientConnected, ClientDisconnected, MirrorStarted, MirrorStopped };
  Kind kind = Kind::ClientConnected;
  char pin[8] = {};        // Kind::Pin 有效：四位数字 + NUL（规格 §9 pin_auth）
};

class EventBus {
 public:
  // 任意线程可调
  void publish_dmr(DmrCommand c) {
    std::lock_guard<std::mutex> lk(mu_);
    cmds_.push_back(std::move(c));
  }
  // 仅主线程可调（publish_player 在主线程）
  void publish_player(PlayerEvent e) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      snapshot_ = e.snap;
      events_.push_back(std::move(e));
    }
    // d47：发布即推进版本号 —— 主循环据此做"快照是否变过"的廉价检测
    //（原子读 vs 每轮 mutex+字符串拷贝的 snapshot()），按需决定 publish+wake。
    snap_version_.fetch_add(1, std::memory_order_release);
  }
  // 仅主线程
  std::vector<DmrCommand> drain_commands() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<DmrCommand> out(cmds_.begin(), cmds_.end());
    cmds_.clear();
    return out;
  }
  std::vector<PlayerEvent> drain_events() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<PlayerEvent> out(events_.begin(), events_.end());
    events_.clear();
    return out;
  }
  // d146：投屏呈现信号（第三条独立队列）。
  // ★events_ 的唯一消费者是 SOAP 层（drain 是"取走即删"），主线程若去 drain 同一条
  // 队列就是抢食 ⇒ 投屏通知必须独立队列、各走各的 drain（队列所有权问题，不是复用）。
  // publish_cast 仅主线程可调（handle_set_uri 在主线程）；复用同一把 mu_。
  void publish_cast(CastEvent e) {
    std::lock_guard<std::mutex> lk(mu_);
    if (casts_.size() >= 8) casts_.pop_front();  // 容量 8 丢最旧（最新投屏才是用户关心的）
    casts_.push_back(std::move(e));
  }
  // 单消费者：只有 main 主循环（代码即文档，勿在他处调用）
  std::vector<CastEvent> drain_casts() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<CastEvent> out(casts_.begin(), casts_.end());
    casts_.clear();
    return out;
  }
  // d267：AirPlay 反馈事件（第四队列，任意线程 publish —— lib 回调线程生产）。
  void publish_airplay(AirplayEvent e) {
    std::lock_guard<std::mutex> lk(mu_);
    if (airplays_.size() >= 16) airplays_.pop_front();  // 容量 16 丢最旧
    airplays_.push_back(std::move(e));
  }
  // 单消费者：只有 main 主循环 drain_airplay（FR_AIRPLAY 才有生产者；OFF 时
  // 队列恒空、零开销 —— 结构无条件保留以维持编译面稳定）。
  std::vector<AirplayEvent> drain_airplay() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<AirplayEvent> out(airplays_.begin(), airplays_.end());
    airplays_.clear();
    return out;
  }
  // 任意线程：返回快照拷贝（libupnp 回调线程构造 SOAP 应答时读取）
  PlaybackSnapshot snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snapshot_;
  }
  // d47：快照版本号（任意线程；publish_player 每次调用 +1）。
  // 主循环拿它与上一轮比较：没变就不 publish/wake 渲染线程。
  uint64_t snapshot_version() const {
    return snap_version_.load(std::memory_order_acquire);
  }

 private:
  mutable std::mutex mu_;
  std::deque<DmrCommand> cmds_;
  std::deque<PlayerEvent> events_;
  std::deque<CastEvent> casts_;      // d146：投屏呈现信号（容量 8，丢最旧）
  std::deque<AirplayEvent> airplays_;  // d267：AirPlay 反馈（容量 16，丢最旧）
  PlaybackSnapshot snapshot_;
  std::atomic<uint64_t> snap_version_{0};  // d47：快照代数（见 snapshot_version）
};

}  // namespace fr
