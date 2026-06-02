// libuv TCP echo benchmark server for Nintendo Switch.
//
// Listens on 0.0.0.0:1234 and echoes everything it receives. Designed to be
// driven from a PC with tcpkali (throughput / concurrent connections / conn
// rate). Prints live metrics (active/peak connections, total accepted, accept
// errors, bytes echoed, throughput) once per second to the console and to
// sdmc:/uvbench.log. Press + to stop.
//
// Tuned for many simultaneous connections: a custom socketInitConfig with
// modest per-socket buffers but high sb_efficiency + more bsd sessions, so the
// bsd: transfer-memory pool yields as many concurrent fds as possible.
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <uv.h>

#define PORT 1234

// Log: open+append+close per line so the file on the SD card is fully readable
// (size + contents flushed) while the app is still running.
#define LOG_PATH "sdmc:/uvbench.log"
static void blog(const char* fmt, ...) {
  char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  fputs(b, stdout);
  FILE* f = fopen(LOG_PATH, "a");
  if (f) { fputs(b, f); fclose(f); }
}
static void log_reset(void) { FILE* f = fopen(LOG_PATH, "w"); if (f) fclose(f); }

// --- metrics ---
static uint64_t m_active, m_peak, m_total_accepted, m_accept_errors;
static uint64_t m_bytes_rx, m_bytes_tx, m_writes, m_write_errors;
static uint64_t m_bytes_rx_last, m_bytes_tx_last;

static uv_loop_t* loop;
static uv_tcp_t server;
static uv_timer_t metrics_timer;
static PadState g_pad;

typedef struct { uv_tcp_t handle; } conn_t;
typedef struct { uv_write_t req; uv_buf_t buf; char* data; } write_ctx_t;

static void on_conn_closed(uv_handle_t* h) {
  free(h);                 // conn_t starts with uv_tcp_t handle
  m_active--;
}

static void after_write(uv_write_t* req, int status) {
  write_ctx_t* w = (write_ctx_t*)req;
  if (status < 0) m_write_errors++;
  free(w->data);
  free(w);
}

static void alloc_cb(uv_handle_t* h, size_t suggested, uv_buf_t* buf) {
  (void)h;
  buf->base = malloc(suggested);
  buf->len = buf->base ? suggested : 0;
}

static void on_read(uv_stream_t* s, ssize_t nread, const uv_buf_t* buf) {
  if (nread > 0) {
    m_bytes_rx += (uint64_t)nread;
    // echo: copy into a write ctx and send back
    write_ctx_t* w = malloc(sizeof *w);
    if (w) {
      w->data = malloc((size_t)nread);
      if (w->data) {
        memcpy(w->data, buf->base, (size_t)nread);
        w->buf = uv_buf_init(w->data, (unsigned)nread);
        if (uv_write(&w->req, s, &w->buf, 1, after_write) == 0) {
          m_bytes_tx += (uint64_t)nread;
          m_writes++;
        } else { m_write_errors++; free(w->data); free(w); }
      } else { free(w); }
    }
  } else if (nread < 0) {
    // EOF or error: close the connection
    uv_close((uv_handle_t*)s, on_conn_closed);
  }
  free(buf->base);
}

static void on_new_connection(uv_stream_t* srv, int status) {
  if (status < 0) { m_accept_errors++; return; }
  conn_t* c = malloc(sizeof *c);
  if (!c) { m_accept_errors++; return; }
  uv_tcp_init(loop, &c->handle);
  if (uv_accept(srv, (uv_stream_t*)&c->handle) == 0) {
    uv_tcp_nodelay(&c->handle, 1);
    m_total_accepted++;
    m_active++;
    if (m_active > m_peak) m_peak = m_active;
    uv_read_start((uv_stream_t*)&c->handle, alloc_cb, on_read);
  } else {
    m_accept_errors++;
    uv_close((uv_handle_t*)&c->handle, on_conn_closed);
    m_active++;  // on_conn_closed will decrement
  }
}

static void metrics_cb(uv_timer_t* t) {
  (void)t;
  uint64_t drx = m_bytes_rx - m_bytes_rx_last;
  uint64_t dtx = m_bytes_tx - m_bytes_tx_last;
  m_bytes_rx_last = m_bytes_rx;
  m_bytes_tx_last = m_bytes_tx;
  blog("[m] active=%llu peak=%llu accepted=%llu aerr=%llu | rx=%.2fMB tx=%.2fMB | %.2f/%.2f MB/s | werr=%llu\n",
       (unsigned long long)m_active, (unsigned long long)m_peak,
       (unsigned long long)m_total_accepted, (unsigned long long)m_accept_errors,
       m_bytes_rx / 1048576.0, m_bytes_tx / 1048576.0,
       drx / 1048576.0, dtx / 1048576.0,
       (unsigned long long)m_write_errors);
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  consoleInit(NULL);

  // Socket config matching nx.js's known-good server settings. (An earlier
  // attempt with tiny 4K buffers + max_size=0 broke accept/poll readiness on
  // the firmware bsd: service after the first connection.)
  static const SocketInitConfig cfg = {
    .tcp_tx_buf_size     = 1 * 1024 * 1024,
    .tcp_rx_buf_size     = 1 * 1024 * 1024,
    .tcp_tx_buf_max_size = 4 * 1024 * 1024,
    .tcp_rx_buf_max_size = 4 * 1024 * 1024,
    .udp_tx_buf_size     = 0x2400,
    .udp_rx_buf_size     = 0xA500,
    .sb_efficiency       = 8,
    .num_bsd_sessions    = 3,
    .bsd_service_type    = BsdServiceType_Auto,
  };
  Result rc = socketInitialize(&cfg);

  log_reset();
  blog("=== libuv TCP echo benchmark server ===\n");
  blog("libuv %s  socketInit rc=0x%x  port=%d\n", uv_version_string(), (unsigned)rc, PORT);

  // show our IP
  u32 ip = gethostid();
  blog("listening on %u.%u.%u.%u:%d\n",
       ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, (ip >> 24) & 0xff, PORT);

  loop = uv_default_loop();
  uv_tcp_init(loop, &server);

  struct sockaddr_in addr;
  uv_ip4_addr("0.0.0.0", PORT, &addr);
  int r = uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);
  blog("bind: %s\n", r ? uv_strerror(r) : "ok");
  // large backlog so bursts of connections queue rather than RST
  r = uv_listen((uv_stream_t*)&server, 128, on_new_connection);
  blog("listen(backlog=128): %s\n", r ? uv_strerror(r) : "ok");

  uv_timer_init(loop, &metrics_timer);
  uv_timer_start(&metrics_timer, metrics_cb, 1000, 1000);

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pad);

  blog("--- running; press + to stop ---\n");

  // Non-blocking integration with the applet main loop: pump libuv with
  // UV_RUN_NOWAIT every iteration (processes all currently-ready socket/timer
  // events without blocking), and refresh the console + gamepad at ~30 Hz.
  // consoleUpdate() is throttled so it doesn't starve the socket service.
  u64 last_ui = 0;
  while (appletMainLoop()) {
    int more = uv_run(loop, UV_RUN_NOWAIT);

    u64 now = armGetSystemTick();
    if (now - last_ui >= 19200000ull / 30) {  // ~30 Hz
      last_ui = now;
      padUpdate(&g_pad);
      consoleUpdate(NULL);
      if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) break;
    }

    // If libuv has nothing pending, yield briefly so we don't spin at 100%.
    if (!more)
      svcSleepThread(500000ull);  // 0.5 ms
  }

  blog("--- stopping ---\n");
  blog("FINAL: accepted=%llu peak_concurrent=%llu accept_errors=%llu rx=%.2fMB tx=%.2fMB writes=%llu werr=%llu\n",
       (unsigned long long)m_total_accepted, (unsigned long long)m_peak,
       (unsigned long long)m_accept_errors, m_bytes_rx / 1048576.0,
       m_bytes_tx / 1048576.0, (unsigned long long)m_writes,
       (unsigned long long)m_write_errors);

  uv_close((uv_handle_t*)&server, NULL);
  uv_close((uv_handle_t*)&metrics_timer, NULL);
  uv_run(loop, UV_RUN_NOWAIT);
  uv_loop_close(loop);
  uv_library_shutdown();

  socketExit();
  consoleExit(NULL);
  return 0;
}
