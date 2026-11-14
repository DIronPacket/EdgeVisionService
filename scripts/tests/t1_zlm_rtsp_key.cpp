// T1 验收程序：P0-6（stop 的 key 与注册一致 + ref_cnt 不泄漏）+ P0-7（入参校验顺序）
// 用 scripts/tests/run_t1_zlm_rtsp_key.sh 编译并运行（会真的起/停 RTSP 服务）。
// 记录见 docs/history/TESTLOG.md §5.5（L2 · T1）。
#include "zlm_push_server.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_fails = 0;

static void check(bool cond, const char *msg)
{
  if (cond) {
    printf("ok   : %s\n", msg);
  } else {
    printf("FAIL : %s\n", msg);
    ++g_fails;
  }
}

// 端口是否在监听：读 `ss -ltn`，完全不建立 TCP 连接。
// 为什么不用 connect 探测：stop 之后每 50ms 建一次连接，ZLM 会把每个连接当作
// 一个 RTSP 会话（日志 RTSP播放器(//)断开），反而干扰它自己的关闭流程。
static bool port_listening(uint16_t port)
{
  FILE *p = ::popen("ss -ltn 2>/dev/null", "r");
  if (p == nullptr)
    return false;
  char line[512];
  char needle[16];
  snprintf(needle, sizeof(needle), ":%u ", static_cast<unsigned>(port));
  bool found = false;
  while (fgets(line, sizeof(line), p) != nullptr) {
    if (strstr(line, needle) != nullptr) {
      found = true;
      break;
    }
  }
  ::pclose(p);
  return found;
}

// 观察端口状态（只打印，不作断言）：stop 之后端口由 ZLM 的 TcpServer 析构
// 异步关闭，短时内可能仍被占用，因此端口不能当"服务已停"的判据。
static void observe_port(uint16_t port, const char *when)
{
  printf("info : %s -> 端口 %u 监听=%d, streams=%d, servers=%d\n", when, static_cast<unsigned>(port),
         static_cast<int>(port_listening(port)), zlm_rtsp_stream_count(), zlm_rtsp_server_count());
}

// 等端口释放：ZLM 的 TcpServer 析构在 poller 线程异步完成，
// 同端口立刻重启会撞 "address already in use"（实测第 2 轮必失败）。
static bool wait_port_free(uint16_t port, int timeout_ms = 3000)
{
  for (int waited = 0; waited < timeout_ms; waited += 50) {
    if (!port_listening(port))
      return true;
    usleep(50 * 1000);
  }
  return !port_listening(port);
}

int main(int argc, char **argv)
{
  // 默认 18550，避开现场正在跑的 8550；可用第一个参数覆盖
  const uint16_t PORT =
      (argc > 1) ? static_cast<uint16_t>(atoi(argv[1])) : static_cast<uint16_t>(18550);
  int code = 0;

  printf("=== P0-7 入参校验顺序 ===\n");
  // 修复前：这里会因 *out_code 解引用空指针而崩溃
  ZlmServerStreamHandle bad = zlm_rtsp_onekey_start(
      PORT, ZLM_FRAME_VIDEO_H265, "live", "t1", 1280, 720, 30, nullptr, nullptr, nullptr);
  check(bad == nullptr, "out_code=nullptr 直接返回 nullptr（不崩溃）");

  code = 0;
  check(zlm_rtsp_onekey_start(PORT, ZLM_FRAME_VIDEO_H265, "live", "", 1280, 720, 30,
                              nullptr, nullptr, &code) == nullptr &&
            code == ZLM_ERR_INVALID_PARAM,
        "空 stream 被拒 -> ZLM_ERR_INVALID_PARAM");

  code = 0;
  check(zlm_rtsp_onekey_start(PORT, ZLM_FRAME_VIDEO_H265, "live", "t1", 0, 0, 30,
                              nullptr, nullptr, &code) == nullptr &&
            code == ZLM_ERR_INVALID_PARAM,
        "0x0 尺寸被拒 -> ZLM_ERR_INVALID_PARAM");

  code = 0;
  check(zlm_rtsp_onekey_start(PORT, ZLM_FRAME_VIDEO_H265, "live", "t1", 1280, 720, 0,
                              nullptr, nullptr, &code) == nullptr &&
            code == ZLM_ERR_INVALID_PARAM,
        "fps=0 被拒 -> ZLM_ERR_INVALID_PARAM");

  printf("\n=== P0-6 起停与 key 一致性 ===\n");
  code = 0;
  ZlmServerStreamHandle h1 = zlm_rtsp_onekey_start(
      PORT, ZLM_FRAME_VIDEO_H265, "live", "t1", 1280, 720, 30, nullptr, nullptr, &code);
  check(h1 != nullptr && code == ZLM_ERR_OK, "start t1 -> OK");
  check(port_listening(PORT), "RTSP 端口已监听");
  check(zlm_rtsp_stream_count() == 1 && zlm_rtsp_server_count() == 1,
        "起 1 路后 streams=1 / servers=1");

  code = 0;
  ZlmServerStreamHandle dup = zlm_rtsp_onekey_start(
      PORT, ZLM_FRAME_VIDEO_H265, "live", "t1", 1280, 720, 30, nullptr, nullptr, &code);
  check(code == ZLM_ERR_STREAM_EXIST && dup != nullptr, "重复注册同名流 -> ZLM_ERR_STREAM_EXIST");
  check(dup == h1, "重复注册返回同一句柄");

  code = 0;
  ZlmServerStreamHandle h2 = zlm_rtsp_onekey_start(
      PORT, ZLM_FRAME_VIDEO_H265, "live", "t2", 1280, 720, 30, nullptr, nullptr, &code);
  check(h2 != nullptr && code == ZLM_ERR_OK, "start t2（同一 server，ref_cnt=2）-> OK");

  // P0-6 核心断言：修复前 stop 永不生效，这里会一直返回 STREAM_EXIST
  zlm_rtsp_stop(h1);
  code = 0;
  ZlmServerStreamHandle h1b = zlm_rtsp_onekey_start(
      PORT, ZLM_FRAME_VIDEO_H265, "live", "t1", 1280, 720, 30, nullptr, nullptr, &code);
  check(h1b != nullptr && code == ZLM_ERR_OK, "P0-6: stop 之后同名流可重新注册（旧实现永为 STREAM_EXIST）");
  check(port_listening(PORT), "停 t1 不影响 t2（服务仍在监听）");

  // 送帧路径：有效句柄应返回 OK（data 非空即可，ZLM 内部按需丢弃）
  {
    const uint8_t dummy[4] = {0, 0, 0, 1};
    int r = zlm_rtsp_input_encodeData(h2, ZLM_FRAME_VIDEO_H265, dummy, sizeof(dummy), 0, 0);
    check(r == ZLM_ERR_OK, "有效句柄送帧 -> ZLM_ERR_OK");
  }

  // 幂等：重复 stop 不得崩溃（修复前第二次 stop 是 use-after-free）
  zlm_rtsp_stop(h1b);
  zlm_rtsp_stop(h1b);
  check(true, "重复 stop 幂等且不崩溃");

  // 已停止的句柄送帧：应返回 NOT_FOUND 而不是崩溃
  {
    const uint8_t dummy[4] = {0, 0, 0, 1};
    int r = zlm_rtsp_input_encodeData(h1b, ZLM_FRAME_VIDEO_H265, dummy, sizeof(dummy), 0, 0);
    check(r == ZLM_ERR_NOT_FOUND, "已 stop 的句柄送帧 -> ZLM_ERR_NOT_FOUND（不崩溃）");
  }

  zlm_rtsp_stop(h2);
  check(zlm_rtsp_stream_count() == 0 && zlm_rtsp_server_count() == 0,
        "全部 stop 后 g_stream_map / g_server_map 均为空（server 已回收）");
  observe_port(PORT, "全部 stop 后");

  printf("\n=== start/stop x10 ===\n");
  bool all_ok = true;
  int retries = 0;
  for (int i = 0; i < 10; ++i) {
    // 同端口重启需要等上一轮监听 socket 真正释放，起流失败就退避重试
    int c = 0;
    ZlmServerStreamHandle h = nullptr;
    for (int attempt = 0; attempt < 60 && h == nullptr; ++attempt) {
      if (attempt > 0) {
        ++retries;
        usleep(50 * 1000);
      }
      c = 0;
      h = zlm_rtsp_onekey_start(PORT, ZLM_FRAME_VIDEO_H265, "live", "cyc", 1280, 720, 30,
                                nullptr, nullptr, &c);
    }
    if (h == nullptr || c != ZLM_ERR_OK) {
      printf("       第 %d 轮 start 失败 code=%d h=%p\n", i, c, static_cast<void *>(h));
      all_ok = false;
      break;
    }
    if (zlm_rtsp_stream_count() != 1 || zlm_rtsp_server_count() != 1) {
      printf("       第 %d 轮 start 后计数异常 streams=%d servers=%d\n", i,
             zlm_rtsp_stream_count(), zlm_rtsp_server_count());
      all_ok = false;
      break;
    }
    zlm_rtsp_stop(h);
    if (zlm_rtsp_stream_count() != 0 || zlm_rtsp_server_count() != 0) {
      printf("       第 %d 轮 stop 后残留 streams=%d servers=%d\n", i, zlm_rtsp_stream_count(),
             zlm_rtsp_server_count());
      all_ok = false;
      break;
    }
  }
  check(all_ok, "start/stop x10：每轮 streams/servers 起 1 落 0（无 key 残留、无 ref_cnt 漂移）");
  printf("info : x10 期间为等端口释放共退避重试 %d 次\n", retries);
  observe_port(PORT, "x10 之后");

  code = 0;
  ZlmServerStreamHandle hf = zlm_rtsp_onekey_start(
      PORT, ZLM_FRAME_VIDEO_H265, "live", "cyc", 1280, 720, 30, nullptr, nullptr, &code);
  check(hf != nullptr && code == ZLM_ERR_OK, "x10 之后仍可重新建流（两张表无残留）");
  zlm_rtsp_stop(hf);

  printf("\n==== %s (fails=%d) ====\n", g_fails == 0 ? "ALL PASS" : "HAS FAILURE", g_fails);
  return g_fails == 0 ? 0 : 1;
}
