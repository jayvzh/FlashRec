# cmake/uxplay.cmake —— 内嵌 AirPlay（UxPlay lib）自建静态库（d237）
#
# 为什么不用上游 CMakeLists：
#   lib/CMakeLists.txt 会注入 -march=native（发布包会在别的机器上跑，绝对不行）、
#   用 aux_source_directory 拾取源码（上游增删文件静默改变编译集）、
#   且 find_package(PkgConfig REQUIRED) 在 Windows 上无条件执行。
#   这里只"借用"它的编译定义与链接清单，源码显式罗列。
#
# 规则 8（第三方只读）：third_party/uxplay 一行不改。MSVC 缺的 POSIX 头用构建期
# 生成的 shim 目录补（落在 build/ 下），不往源码树里塞文件。
#
# 开关：-DFR_AIRPLAY=ON 启用；OFF 时本文件整体 return，依赖也不 find。

if (NOT FR_AIRPLAY)
  return()
endif()

set(UXPLAY_DIR ${FR_ROOT}/third_party/uxplay)
if (NOT EXISTS ${UXPLAY_DIR}/lib/raop.c)
  message(FATAL_ERROR
    "缺 third_party/uxplay（需 lib/raop.c）—— 先执行: bash tools/fetch_deps.sh")
endif()
set(UXPLAY_LIB ${UXPLAY_DIR}/lib)

# ---- 依赖：OpenSSL(libcrypto) + libplist ----
# Windows 走 third_party/vcpkg 预编译前缀（fetch_deps.sh 里 vcpkg install openssl libplist）；
# Linux 走系统包（libssl-dev / libplist-dev），由 deb 的 dpkg-shlibdeps 自动推导 Depends。
if (WIN32)
  set(FR_VCPKG_PREFIX ${FR_ROOT}/third_party/vcpkg/installed/x64-windows)
  if (NOT EXISTS ${FR_VCPKG_PREFIX}/include)
    message(FATAL_ERROR
      "缺 third_party/vcpkg/installed/x64-windows（OpenSSL/libplist）—— "
      "先执行: bash tools/fetch_deps.sh")
  endif()
  list(PREPEND CMAKE_PREFIX_PATH ${FR_VCPKG_PREFIX})
  find_package(OpenSSL REQUIRED)
  find_path(PLIST_INCLUDE_DIR NAMES plist/plist.h HINTS ${FR_VCPKG_PREFIX}/include NO_DEFAULT_PATH)
  find_library(PLIST_LIBRARY NAMES plist-2.0 plist HINTS ${FR_VCPKG_PREFIX}/lib NO_DEFAULT_PATH)
  # pthread：UxPlay 的 lib/threads.h 无条件 #include <pthread.h>（"Always use pthread
  # library"），Windows 上由 pthreads4w 提供（预编译前缀，同 mpv 的 third_party 模式）。
  set(FR_PTHREADS4W ${FR_ROOT}/third_party/pthreads4w/install)
  # ★用 DLL 版（pthreadC3.dll）而不是静态库：静态库要求宿主显式调
  #   pthread_win32_process_attach_np()，且必须与宿主 CRT 完全匹配（本项目 /MD，
  #   该静态库是别的配置编的 → 一调 pthread_create 就崩在库内部）。DLL 版自带
  #   DllMain 初始化，代价只是发布包多带一个 47KB 的 dll。
  find_library(FR_PTHREAD_LIBRARY NAMES pthreadC3 libpthreadC3s
    HINTS ${FR_PTHREADS4W}/lib NO_DEFAULT_PATH)
  find_path(FR_PTHREAD_INCLUDE_DIR NAMES pthread.h
    HINTS ${FR_PTHREADS4W}/include/pthread ${FR_PTHREADS4W}/include NO_DEFAULT_PATH)
  find_file(FR_PTHREAD_DLL NAMES pthreadC3.dll HINTS ${FR_PTHREADS4W}/lib ${FR_PTHREADS4W}/bin
    NO_DEFAULT_PATH)
  if (NOT FR_PTHREAD_LIBRARY OR NOT FR_PTHREAD_INCLUDE_DIR)
    message(FATAL_ERROR "缺 third_party/pthreads4w/install（pthread.h + lib）—— 先执行: bash tools/fetch_deps.sh")
  endif()
else()
  find_package(PkgConfig REQUIRED)
  find_package(OpenSSL 1.1.1 REQUIRED)
  pkg_check_modules(PLIST REQUIRED libplist-2.0)
  find_library(PLIST_LIBRARY NAMES plist-2.0 HINTS ${PLIST_LIBDIR})
  set(PLIST_INCLUDE_DIR ${PLIST_INCLUDE_DIRS})
  find_package(Threads REQUIRED)
endif()

if (NOT PLIST_LIBRARY OR NOT PLIST_INCLUDE_DIR)
  message(FATAL_ERROR "缺 libplist（Windows: vcpkg；Linux: apt install libplist-dev）")
endif()
message(STATUS "[airplay] OpenSSL ${OPENSSL_VERSION} | libplist ${PLIST_LIBRARY}")

# libplist 版本分支（UxPlay 侧 PLIST_210 / PLIST_230 条件编译）。
# 注意：vcpkg 的 libplist-2.0.pc 里 Version 恒写 2.0，不可信，故显式给开关，
# 默认按「我们自控版本 ≥2.3」（方案 §4.1）取 ON；若换到 2.2 的系统包则置 OFF。
set(FR_PLIST_230 ON CACHE BOOL "libplist >= 2.3（决定 UxPlay 的 PLIST_230 分支）")

# ---- MSVC 的 POSIX 兼容 shim（只补 UxPlay 在 Windows 上缺的那几个符号）----
# 三个文件都是构建期生成到 build/ 下，不往源码树里塞东西：
#   fr_airplay_msvc.h —— /FI 强制包含（每个 TU 的第一个字节就是它）；
#   unistd.h / threads.h —— 靠 include 路径**优先于** lib/ 目录来顶替第三方同名头。
if (MSVC)
  set(FR_AIRPLAY_SHIM ${CMAKE_BINARY_DIR}/fr_airplay_shim)
  file(MAKE_DIRECTORY ${FR_AIRPLAY_SHIM})

  # ★顺序是本文件存在的全部意义：必须在第三方 compat.h 定义 snprintf 宏**之前**
  #   把 ucrt 的 stdio.h 处理完 —— stdio.h 一旦检测到 snprintf 被定义成宏就
  #   #error C1189（"Macro definition of snprintf conflicts with..."）。
  file(WRITE ${FR_AIRPLAY_SHIM}/fr_airplay_msvc.h
"#pragma once\n"
"/* 构建期生成（cmake/uxplay.cmake）：MSVC 强制包含头（/FI）。 */\n"
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <string.h>\n"
"#include <winsock2.h>\n"
"#include <ws2tcpip.h>\n"
"#include <windows.h>\n"
"#include <BaseTsd.h>\n"
"#include <unistd.h>\n"
"#include <pthread.h>\n"
"typedef SSIZE_T ssize_t;\n"
"#ifndef SOL_TCP\n"
"#define SOL_TCP IPPROTO_TCP\n"
"#endif\n"
"/* Windows 没有 clock_gettime（raop_ntp.c 用它取绝对时间）：给一份等价实现。 */\n"
"#define CLOCK_REALTIME 0\n"
"static int clock_gettime(int clk_id, struct timespec *ts) {\n"
"    (void)clk_id;\n"
"    static const unsigned long long kEpoch = 116444736000000000ULL;\n"
"    FILETIME ft; GetSystemTimeAsFileTime(&ft);\n"
"    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;\n"
"    t -= kEpoch;\n"
"    ts->tv_sec = (long)(t / 10000000ULL);\n"
"    ts->tv_nsec = (long)((t % 10000000ULL) * 100);\n"
"    return 0;\n"
"}\n"
"/* ★抢占 include guard：第三方 lib/threads.h 是 #include \"threads.h\"（引号形式），\n"
" *   编译器先查**源文件所在目录**，shim 目录排再前也顶替不了。这里先定义 THREADS_H\n"
" *   让它整体空转，再由本文件给出同样宏契约的自实现（失败分支不写 handle = 0，\n"
" *   因为 pthreads4w 的 pthread_t 是结构体、非标量，MSVC 报 C2440）。 */\n"
"#define THREADS_H\n"
"#define sleepms(x) usleep((x)*1000)\n"
"/* 句柄类型取**指针**而非 pthread_t 本身：mdnsd.c 里有 `thread = 0` / `!thread` /\n"
" *  `thread == 0` 三处标量用法，而 pthreads4w 的 pthread_t 是结构体（不可比较）。\n"
" *  指针类型天然满足这些用法，真正的 pthread_t 存在槽位里，join 后释放。 */\n"
"typedef struct fr_thread_slot_s { pthread_t t; } *thread_handle_t;\n"
"#define THREAD_RETVAL void *\n"
"#define THREAD_CREATE(handle, func, arg) do { (handle) = (thread_handle_t)calloc(1, sizeof(struct fr_thread_slot_s)); if ((handle)) { if (pthread_create(&((handle)->t), NULL, (func), (arg)) != 0) { free((handle)); (handle) = 0; } } } while (0)\n"
"#define THREAD_JOIN(handle) do { if ((handle)) { pthread_join((handle)->t, NULL); free((handle)); (handle) = 0; } } while (0)\n"
"typedef pthread_mutex_t mutex_handle_t;\n"
"typedef pthread_cond_t cond_handle_t;\n"
"#define MUTEX_CREATE(handle) pthread_mutex_init(&(handle), NULL)\n"
"/* ★懒初始化：UxPlay 多处结构体是 calloc 出来的，且**从不调用 MUTEX_CREATE**\n"
" *   （如 httpd.c 的 run_mutex、mdnsd 的 mutex）。在 POSIX/winpthreads 下\n"
" *   pthread_mutex_t 是值类型，全零 = PTHREAD_MUTEX_INITIALIZER 天然合法；\n"
" *   但 pthreads4w 的 pthread_mutex_t 是**指针**，零值即 NULL，\n"
" *   pthread_mutex_lock(NULL) 直接崩（实测异常访问地址 0x8 = 解引用空指针取成员）。\n"
" *   这里在每次使用前补一次懒初始化，把「零值即合法」这个假设还给 UxPlay。\n"
" *   竞态：首次初始化都发生在启动期单线程，无需额外加锁。 */\n"
"#define MUTEX_LOCK(handle) do { if (!(handle)) pthread_mutex_init(&(handle), NULL); pthread_mutex_lock(&(handle)); } while (0)\n"
"#define MUTEX_UNLOCK(handle) do { if (handle) pthread_mutex_unlock(&(handle)); } while (0)\n"
"#define MUTEX_DESTROY(handle) do { if (handle) pthread_mutex_destroy(&(handle)); } while (0)\n"
"#define COND_CREATE(handle) pthread_cond_init(&(handle), NULL)\n"
"#define COND_SIGNAL(handle) do { if (!(handle)) pthread_cond_init(&(handle), NULL); pthread_cond_signal(&(handle)); } while (0)\n"
"#define COND_DESTROY(handle) do { if (handle) pthread_cond_destroy(&(handle)); } while (0)\n")

  file(WRITE ${FR_AIRPLAY_SHIM}/unistd.h
"#pragma once\n"
"/* 构建期生成：UxPlay lib/threads.h 无条件 #include <unistd.h>。\n"
" * 只补它真正用到的 usleep（sleepms 宏）；1ms 粒度，够 NTP/镜像线程的休眠用。 */\n"
"#include <windows.h>\n"
"static int usleep(unsigned int useconds) {\n"
"    if (useconds == 0) return 0;\n"
"    Sleep((useconds + 999u) / 1000u);\n"
"    return 0;\n"
"}\n")

  # 顶替 lib/threads.h：原版的 THREAD_CREATE 在失败分支写 `handle = 0`，
  # 而 pthreads4w 的 pthread_t 是结构体（非标量），MSVC 下直接 C2440。
  # 这里按**同样的宏契约**自行实现（只用 pthreads4w 的公开 API），失败分支改为不置零。
  file(WRITE ${FR_AIRPLAY_SHIM}/threads.h
"#pragma once\n"
"/* 构建期生成：顶替 UxPlay lib/threads.h（MSVC + pthreads4w 专用）。\n"
" * 宏契约与原版逐条一致，唯一差异是 THREAD_CREATE 失败分支不写 handle = 0。 */\n"
"#include <pthread.h>\n"
"#include <unistd.h>\n"
"#define sleepms(x) usleep((x)*1000)\n"
"typedef pthread_t thread_handle_t;\n"
"#define THREAD_RETVAL void *\n"
"#define THREAD_CREATE(handle, func, arg) do { if (pthread_create(&(handle), NULL, func, arg) != 0) { (void)0; } } while (0)\n"
"#define THREAD_JOIN(handle) pthread_join(handle, NULL)\n"
"typedef pthread_mutex_t mutex_handle_t;\n"
"typedef pthread_cond_t cond_handle_t;\n"
"#define MUTEX_CREATE(handle) pthread_mutex_init(&(handle), NULL)\n"
"/* ★懒初始化：UxPlay 多处结构体是 calloc 出来的，且**从不调用 MUTEX_CREATE**\n"
" *   （如 httpd.c 的 run_mutex、mdnsd 的 mutex）。在 POSIX/winpthreads 下\n"
" *   pthread_mutex_t 是值类型，全零 = PTHREAD_MUTEX_INITIALIZER 天然合法；\n"
" *   但 pthreads4w 的 pthread_mutex_t 是**指针**，零值即 NULL，\n"
" *   pthread_mutex_lock(NULL) 直接崩（实测异常访问地址 0x8 = 解引用空指针取成员）。\n"
" *   这里在每次使用前补一次懒初始化，把「零值即合法」这个假设还给 UxPlay。\n"
" *   竞态：首次初始化都发生在启动期单线程，无需额外加锁。 */\n"
"#define MUTEX_LOCK(handle) do { if (!(handle)) pthread_mutex_init(&(handle), NULL); pthread_mutex_lock(&(handle)); } while (0)\n"
"#define MUTEX_UNLOCK(handle) do { if (handle) pthread_mutex_unlock(&(handle)); } while (0)\n"
"#define MUTEX_DESTROY(handle) do { if (handle) pthread_mutex_destroy(&(handle)); } while (0)\n"
"#define COND_CREATE(handle) pthread_cond_init(&(handle), NULL)\n"
"#define COND_SIGNAL(handle) do { if (!(handle)) pthread_cond_init(&(handle), NULL); pthread_cond_signal(&(handle)); } while (0)\n"
"#define COND_DESTROY(handle) do { if (handle) pthread_cond_destroy(&(handle)); } while (0)\n")
endif()

# ---- 源码清单（显式罗列，防上游增删静默改变编译集）----
# dns_sd/ 不编：那是 Apple/avahi 的 dns_sd 兼容层，需要 USE_DNS_SD；我们用内置 mdnsd。
set(UXPLAY_SRCS
  ${UXPLAY_LIB}/airplay_video.c
  ${UXPLAY_LIB}/byteutils.c
  ${UXPLAY_LIB}/compat.c
  ${UXPLAY_LIB}/crypto.c
  ${UXPLAY_LIB}/dnssd.c
  ${UXPLAY_LIB}/fairplay_playfair.c
  ${UXPLAY_LIB}/http_request.c
  ${UXPLAY_LIB}/http_response.c
  ${UXPLAY_LIB}/httpd.c
  ${UXPLAY_LIB}/logger.c
  ${UXPLAY_LIB}/mirror_buffer.c
  ${UXPLAY_LIB}/netutils.c
  ${UXPLAY_LIB}/pairing.c
  ${UXPLAY_LIB}/raop.c
  ${UXPLAY_LIB}/raop_buffer.c
  ${UXPLAY_LIB}/raop_ntp.c
  ${UXPLAY_LIB}/raop_rtp.c
  ${UXPLAY_LIB}/raop_rtp_mirror.c
  ${UXPLAY_LIB}/srp.c
  ${UXPLAY_LIB}/utils.c
  ${UXPLAY_LIB}/llhttp/api.c
  ${UXPLAY_LIB}/llhttp/http.c
  ${UXPLAY_LIB}/llhttp/llhttp.c
  ${UXPLAY_LIB}/playfair/hand_garble.c
  ${UXPLAY_LIB}/playfair/modified_md5.c
  ${UXPLAY_LIB}/playfair/omg_hax.c
  ${UXPLAY_LIB}/playfair/playfair.c
  ${UXPLAY_LIB}/playfair/sap_hash.c
  ${UXPLAY_LIB}/mdnsd/dnssd_mdnsd.c
  ${UXPLAY_LIB}/mdnsd/mdnsd.c
)

add_library(uxplay_airplay STATIC ${UXPLAY_SRCS})

target_include_directories(uxplay_airplay PUBLIC
  ${FR_AIRPLAY_SHIM}          # MSVC：POSIX shim（必须排在 lib 前，才能顶替 threads.h/unistd.h）
  ${UXPLAY_LIB}
  ${UXPLAY_LIB}/llhttp
  ${UXPLAY_LIB}/playfair
  ${UXPLAY_LIB}/mdnsd
  ${PLIST_INCLUDE_DIR}
  ${OPENSSL_INCLUDE_DIR}
)
if (MSVC)
  target_include_directories(uxplay_airplay PUBLIC ${FR_PTHREAD_INCLUDE_DIR})
endif()

target_compile_definitions(uxplay_airplay PUBLIC
  NOHOLD                              # 单客户端语义（新连接挤掉旧的），UxPlay 默认
  OPENSSL_API_COMPAT=0x10101000L      # 上游 Windows/Linux 分支都这么定
)
if (WIN32)
  # 注意：这里**不能**定义 PTW32_STATIC_LIB —— 那是给静态库用的；用 DLL 版时
  # 定义了会把 pthread_win32_*_np 声明成静态导入符号，链接/调用约定都不对。
endif()
if (FR_PLIST_230)
  target_compile_definitions(uxplay_airplay PUBLIC PLIST_230 PLIST_210)
else()
  target_compile_definitions(uxplay_airplay PUBLIC PLIST_210)
endif()

# 编译选项：第三方一律降噪；MSVC 还需 UTF-8 与 Windows 头瘦身（同项目其他第三方）
# ★/Dsnprintf=snprintf 已试过**无效**：ucrt 的 stdio.h 里那句 #error 判的是
#   "#if defined(snprintf)"（只要定义过就炸，与展开成什么无关），所以正解是让
#   stdio.h 在 compat.h 定义宏之前就被处理掉 —— 用 /FI 强制包含（见上）。
#   注意：这些**必须逐项独立写** —— 写成 "/utf-8 /w" 会被 CMake 当成一个参数，
#   ninja 再套引号传给 cl，cl 收到的是单个无效 token（静默失效，极难排查）。
target_compile_options(uxplay_airplay PRIVATE
  "$<$<C_COMPILER_ID:MSVC>:/utf-8>"
  "$<$<C_COMPILER_ID:MSVC>:/w>"
  "$<$<C_COMPILER_ID:MSVC>:/FI${FR_AIRPLAY_SHIM}/fr_airplay_msvc.h>"
  "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>"
)
target_compile_definitions(uxplay_airplay PRIVATE
  _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
)

if (WIN32)
  target_link_libraries(uxplay_airplay PUBLIC
    ${FR_PTHREAD_LIBRARY}
    OpenSSL::Crypto
    ${PLIST_LIBRARY}
    ws2_32 wsock32 iphlpapi
  )
else()
  target_link_libraries(uxplay_airplay PUBLIC
    Threads::Threads
    OpenSSL::Crypto
    ${PLIST_LIBRARY}
    m
  )
endif()

# 导出给根 CMakeLists：AirPlay 适配器源码 + 需随包带走的 DLL
set(FR_AIRPLAY_SOURCES
  src/cast/cast_contract.h
  src/cast/airplay/airplay_source.cpp
  src/cast/airplay/airplay_stream.cpp
  src/cast/airplay/audio_wrap.cpp
  src/cast/airplay/client_registry.cpp
  src/cast/airplay/ts_muxer.cpp
  src/platform/stream_local.c
)
if (WIN32)
  # 打包自检用（tools/package_windows.sh 会比对这些文件是否进了 ZIP/Inno）
  set(FR_AIRPLAY_DLLS
    ${FR_VCPKG_PREFIX}/bin/libcrypto-3-x64.dll
    ${FR_VCPKG_PREFIX}/bin/plist-2.0.dll
  )
  if (FR_PTHREAD_DLL)
    list(APPEND FR_AIRPLAY_DLLS ${FR_PTHREAD_DLL})
  endif()
endif()

if (MSVC)
  # ★OPENSSL_Applink：Windows 上 libcrypto 通过 FILE* 读 PEM（raop_init2 的 keyfile）
  #   时会向 **宿主 exe** 索要 OPENSSL_Applink 符号；缺了就是启动即崩 ——
  #   控制台只留一行 "OPENSSL_Uplink(...): no OPENSSL_Applink"，日志到此为止，
  #   极易误判成"程序没起来"。官方配套文件 applink.c 必须编进 exe（它自带
  #   __declspec(dllexport)，链接器不会把它优化掉）。
  #   从 vcpkg 的源码树取（构建期复制到 build/，不进 src/ —— 规则 8 第三方只读）。
  file(GLOB FR_APPLINK_SRC ${FR_ROOT}/third_party/vcpkg/buildtrees/openssl/src/*/ms/applink.c)
  if (NOT FR_APPLINK_SRC)
    message(FATAL_ERROR
      "缺 openssl 的 ms/applink.c（vcpkg 源码树）— 重新执行: bash tools/fetch_deps.sh")
  endif()
  list(GET FR_APPLINK_SRC 0 FR_APPLINK_SRC)
  file(COPY ${FR_APPLINK_SRC} DESTINATION ${FR_AIRPLAY_SHIM})
  list(APPEND FR_AIRPLAY_SOURCES ${FR_AIRPLAY_SHIM}/applink.c)
endif()
