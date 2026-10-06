# UxPlay 请求原文「收件箱」补丁（d242，M1）
#
# 目的：把**客户端发来的每一个字节**（RTSP/HTTP 请求原文：请求行 + 全部头 + 报文体）
# 在**收包处**直接落盘，完全不经过 UxPlay 的解析分支、不经过它的 logger 级别过滤，
# 也不经过我方任何代码。
#
# 为什么需要它（照 d144 的 SOAP 收件箱先例）：
#   UxPlay 的 logger 只在 `-d` 级别打印请求，且它会先解析再打 —— 畸形请求、被拒请求、
#   走到某个提前 return 分支的请求，都可能什么也不留。真机排查"某个 App 连不上/
#   配对失败/协商中断"时，**决定性的证据是客户端到底发了什么**，而这条通道保证：
#   进来的字节一定在这里。
#
# 与 d144 的分工（两者互不依赖，可单独开关）：
#   · FR_SOAP_INBOX    → soap_inbox.log    ：DLNA 侧的请求原文（pupnp dispatch_request 入口）
#   · FR_AIRPLAY_INBOX → airplay_inbox.log ：AirPlay 侧的请求原文（UxPlay recv 处）
#
# 开关：-DFR_AIRPLAY_INBOX=OFF（默认）会把源码树**真正还原**（不是"不应用"）。
# 运行期由环境变量 FR_AIRPLAY_INBOX_LOG 指定文件路径（main.cpp 启动早期按
# settings.json 的 log.airplay_inbox 设置，与 FR_SOAP_INBOX_LOG 同一窗口）；
# 未设/为空时零成本直接返回。
#
# 锚点（两处，都在 httpd.c；各自必须**恰好命中 1 次**）：
#   ① `static THREAD_RETVAL / httpd_thread(void *arg) / {` —— 在此前插入函数定义。
#      （C 不允许嵌套函数，所以定义不能贴在调用点旁边，必须放函数外面）
#   ② `http_request_add_data(connection->request, buffer, recv_datalen);` 及其上一行注释
#      —— 在**解析之前**插调用，这是接收循环里最早的、能看到原始字节的位置。

option(FR_AIRPLAY_INBOX "UxPlay 请求原文收件箱：为 UxPlay lib 应用构建期锚点补丁" OFF)

include(${CMAKE_CURRENT_LIST_DIR}/pupnp_patch_util.cmake)

function(fr_apply_airplay_inbox)
  if (NOT EXISTS "${FR_ROOT}/third_party/uxplay/lib/httpd.c")
    if (FR_AIRPLAY_INBOX)
      message(WARNING
        "[FlashRec] AirPlay 收件箱补丁：找不到 third_party/uxplay —— 请先跑 tools/fetch_deps.sh。")
    endif()
    return()
  endif()

  # AirPlay 整体关掉时强制还原：开关组合不该让源码树停在半补丁状态。
  set(_enable ${FR_AIRPLAY_INBOX})
  if (NOT FR_AIRPLAY)
    set(_enable OFF)
  endif()

  if (_enable)
    message(STATUS "[FlashRec] AirPlay 收件箱补丁：FR_AIRPLAY_INBOX=ON —— 应用补丁")
  else()
    message(STATUS "[FlashRec] AirPlay 收件箱补丁：FR_AIRPLAY_INBOX=OFF —— 还原源码树")
  endif()

  set(_httpd "lib/httpd.c")

  # —— ① 函数定义（插在 httpd_thread 之前）——
  fr_patch_toggle_file(${_enable}
    "${FR_ROOT}/third_party/uxplay/${_httpd}"
    "AirPlay 收件箱补丁"
    "${_httpd} 函数定义"
    [[static THREAD_RETVAL
httpd_thread(void *arg)
{]]
    [[/*!
 * \brief FlashRec local patch (d242): verbatim AirPlay request inbox.
 *
 * Appends every byte received on an RTSP/HTTP connection -- **before** UxPlay
 * parses it -- to the file named by the FR_AIRPLAY_INBOX_LOG environment
 * variable. It runs in the receive loop of httpd_thread(), which is earlier
 * than any UxPlay dispatch branch and earlier than any FlashRec callback, so
 * no later stage can filter, rewrite or silently drop what the client sent.
 *
 * Disabled when the variable is unset or empty. Single file, append-only,
 * truncated at FR_AIRPLAY_INBOX_MAX bytes per chunk (the tail is marked).
 * Body bytes are printed twice: escaped text (readable headers) plus a hex
 * block (binary bodies, which RTSP plists usually are).
 */
#define FR_AIRPLAY_INBOX_MAX (64u * 1024u)

static void fr_airplay_inbox(
	/*! [in] connection being read (only its socket fd is used). */
	http_connection_t *conn,
	/*! [in] bytes just received. */
	const char *buf,
	/*! [in] number of bytes just received. */
	int len)
{
	static const char *cached_path = NULL;
	static unsigned long seq = 0;
	static mutex_handle_t lock;
	static int lock_ready = 0;
	char ts[32];
	struct tm tmv;
	time_t now;
	FILE *f;
	unsigned long my_seq;
	int i;
	int shown;

	if (!cached_path) {
		cached_path = getenv("FR_AIRPLAY_INBOX_LOG");
		if (!cached_path) {
			cached_path = "";
		}
		if (cached_path[0] && !lock_ready) {
			MUTEX_CREATE(lock);
			lock_ready = 1;
		}
	}
	if (!cached_path[0]) {
		return;
	}
	if (len < 0) {
		len = 0;
	}
	shown = (len > (int)FR_AIRPLAY_INBOX_MAX) ? (int)FR_AIRPLAY_INBOX_MAX : len;

	time(&now);
#ifdef _WIN32
	localtime_s(&tmv, &now);
#else
	localtime_r(&now, &tmv);
#endif
	strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

	f = fopen(cached_path, "ab");
	if (!f) {
		return;
	}
	MUTEX_LOCK(lock);
	my_seq = ++seq;
	fprintf(f, "\n===== #%lu %s | fd=%d | recv=%dB%s\n", my_seq, ts,
		conn ? conn->socket_fd : -1, len, (len > shown) ? " (TRUNCATED)" : "");
	/* 可打印原样、换行/制表保留，其余 \xNN —— 逐字节保真，方便直接读请求头 */
	for (i = 0; i < shown; i++) {
		unsigned char c = (unsigned char) buf[i];
		if (c == ((unsigned char) 10) || c == ((unsigned char) 9)) {
			fputc((int) c, f);
		} else if (c >= 0x20 && c < 0x7f) {
			fputc((int) c, f);
		} else {
			fprintf(f, "\\x%02x", c);
		}
	}
	fprintf(f, "\n----- hex (first %d bytes) -----\n",
		(shown > 512) ? 512 : shown);
	for (i = 0; i < shown && i < 512; i++) {
		fprintf(f, "%02x%s", (unsigned char) buf[i], ((i + 1) % 16 == 0) ? "\n" : " ");
	}
	fprintf(f, "\n===== end #%lu\n", my_seq);
	fflush(f);
	MUTEX_UNLOCK(lock);
	fclose(f);
}

static THREAD_RETVAL
httpd_thread(void *arg)
{]])

  # —— ② 调用点（在 http_request_add_data 之前，即解析之前）——
  fr_patch_toggle_file(${_enable}
    "${FR_ROOT}/third_party/uxplay/${_httpd}"
    "AirPlay 收件箱补丁"
    "${_httpd} 调用点"
    [[            /* Parse HTTP request from data read from connection */
            http_request_add_data(connection->request, buffer, recv_datalen);]]
    [[            /* Parse HTTP request from data read from connection */
            fr_airplay_inbox(connection, buffer, recv_datalen); /* d242: 请求原文收件箱 */
            http_request_add_data(connection->request, buffer, recv_datalen);]])

  if (_enable)
    message(STATUS "[FlashRec] AirPlay 收件箱补丁：应用完成")
  else()
    message(STATUS "[FlashRec] AirPlay 收件箱补丁：还原完成")
  endif()
endfunction()
