// vg-chiaki: chương trình dòng lệnh nhỏ cho VietGame, dùng libchiaki (chiaki-ng, AGPL-3.0) để
//  - regist : ghép đôi (đăng ký) với PS5 bằng PIN, in kết quả JSON ra stdout
//  - wakeup : đánh thức PS5 đang ở chế độ nghỉ
//  - stream : kết nối Remote Play và đẩy video đã mã hóa (H.264/H.265) ra stdout
//
// Giao thức stream:
//  stdout (nhị phân): lặp lại [u32 LE độ dài N][u8 codec: 0=H264,1=H265][u8 cờ: bit0=mất khung][N byte Annex-B]
//  stderr (văn bản): mỗi dòng bắt đầu bằng '{' là sự kiện JSON, dòng khác là log của libchiaki
//  stdin: đóng hoặc nhận dòng "quit" thì dừng stream
//
// Copyright (C) 2026 Phuc Nguyen
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU Affero General Public License as published by the Free Software Foundation, version 3
// (with the OpenSSL additional permission inherited from chiaki-ng). See LICENSE.
// This program is distributed WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU AGPL for more details.

#include <chiaki/common.h>
#include <chiaki/log.h>
#include <chiaki/session.h>
#include <chiaki/regist.h>
#include <chiaki/discovery.h>
#include <chiaki/thread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#endif

#define RESULT_NONE 0
#define RESULT_OK 1
#define RESULT_FAIL 2
#define RESULT_CANCEL 3

// ---------------------------------------------------------------------------
// Tiện ích
// ---------------------------------------------------------------------------
// Nhật ký ra file vg-chiaki.log (ghi cấp thấp, không phụ thuộc stdio) để chẩn đoán lỗi sập
#ifdef _WIN32
static HANDLE g_logfile = INVALID_HANDLE_VALUE;
static void flog_open(void)
{
	g_logfile = CreateFileA("vg-chiaki.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}
static void flog(const char *fmt, ...)
{
	if(g_logfile == INVALID_HANDLE_VALUE)
		return;
	char b[1200];
	int n = snprintf(b, 40, "%10lu ", (unsigned long)GetTickCount());
	va_list ap;
	va_start(ap, fmt);
	int m = vsnprintf(b + n, sizeof(b) - (size_t)n - 2, fmt, ap);
	va_end(ap);
	if(m < 0)
		return;
	n += m;
	if(n > (int)sizeof(b) - 2)
		n = (int)sizeof(b) - 2;
	b[n++] = '\n';
	DWORD w = 0;
	WriteFile(g_logfile, b, (DWORD)n, &w, NULL);
}
#else
static void flog_open(void) {}
static void flog(const char *fmt, ...) { (void)fmt; }
#endif

static void log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
	(void)user;
	flog("[chiaki %d] %s", (int)level, msg);
	const char *tag = level == CHIAKI_LOG_ERROR ? "E" : level == CHIAKI_LOG_WARNING ? "W" : "I";
	fprintf(stderr, "[%s] %s\n", tag, msg);
	fflush(stderr);
}

static int hex_val(char c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool hex_decode(const char *hex, uint8_t *out, size_t out_size)
{
	if(!hex || strlen(hex) != out_size * 2)
		return false;
	for(size_t i = 0; i < out_size; i++)
	{
		int a = hex_val(hex[i * 2]), b = hex_val(hex[i * 2 + 1]);
		if(a < 0 || b < 0)
			return false;
		out[i] = (uint8_t)(a * 16 + b);
	}
	return true;
}

static void hex_print(const uint8_t *buf, size_t n)
{
	for(size_t i = 0; i < n; i++)
		printf("%02x", buf[i]);
}

// base64 chuẩn -> byte (dùng cho PSN account id, đúng 8 byte)
static int b64_val(char c)
{
	if(c >= 'A' && c <= 'Z') return c - 'A';
	if(c >= 'a' && c <= 'z') return c - 'a' + 26;
	if(c >= '0' && c <= '9') return c - '0' + 52;
	if(c == '+') return 62;
	if(c == '/') return 63;
	return -1;
}

static size_t b64_decode(const char *in, uint8_t *out, size_t out_max)
{
	size_t n = 0;
	uint32_t acc = 0;
	int bits = 0;
	for(; *in && *in != '='; in++)
	{
		int v = b64_val(*in);
		if(v < 0)
			continue;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if(bits >= 8)
		{
			bits -= 8;
			if(n >= out_max)
				return (size_t)-1;
			out[n++] = (uint8_t)((acc >> bits) & 0xff);
		}
	}
	return n;
}

static const char *arg_value(int argc, char **argv, const char *name)
{
	for(int i = 2; i < argc - 1; i++)
		if(strcmp(argv[i], name) == 0)
			return argv[i + 1];
	return NULL;
}

static bool arg_flag(int argc, char **argv, const char *name)
{
	for(int i = 2; i < argc; i++)
		if(strcmp(argv[i], name) == 0)
			return true;
	return false;
}

static void json_escape_print(FILE *f, const char *s)
{
	fputc('"', f);
	for(; s && *s; s++)
	{
		unsigned char c = (unsigned char)*s;
		if(c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
		else if(c < 0x20) fprintf(f, "\\u%04x", c);
		else fputc(c, f);
	}
	fputc('"', f);
}

static void event_json(const char *event, const char *key, const char *value)
{
	fprintf(stderr, "{\"event\":\"%s\"", event);
	if(key)
	{
		fprintf(stderr, ",\"%s\":", key);
		json_escape_print(stderr, value);
	}
	fprintf(stderr, "}\n");
	fflush(stderr);
}

// ---------------------------------------------------------------------------
// regist
// ---------------------------------------------------------------------------
static ChiakiBoolPredCond regist_done;
static int regist_result = RESULT_NONE;
static ChiakiRegisteredHost regist_host;

static void regist_cb(ChiakiRegistEvent *event, void *user)
{
	(void)user;
	chiaki_bool_pred_cond_lock(&regist_done);
	switch(event->type)
	{
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS:
			regist_host = *event->registered_host;
			regist_result = RESULT_OK;
			break;
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED:
			regist_result = RESULT_CANCEL;
			break;
		default:
			regist_result = RESULT_FAIL;
			break;
	}
	regist_done.pred = true;
	chiaki_bool_pred_cond_signal(&regist_done);
	chiaki_bool_pred_cond_unlock(&regist_done);
}

static int cmd_regist(ChiakiLog *log, int argc, char **argv)
{
	const char *host = arg_value(argc, argv, "--host");
	const char *account = arg_value(argc, argv, "--account");
	const char *pin_s = arg_value(argc, argv, "--pin");
	bool ps4 = arg_flag(argc, argv, "--ps4");
	if(!host || !pin_s || !account)
	{
		printf("{\"ok\":false,\"error\":\"Thiếu tham số --host/--account/--pin\"}\n");
		return 2;
	}

	ChiakiRegistInfo info;
	memset(&info, 0, sizeof(info));
	info.target = ps4 ? CHIAKI_TARGET_PS4_10 : CHIAKI_TARGET_PS5_1;
	info.host = host;
	info.broadcast = false;
	info.pin = (uint32_t)strtoul(pin_s, NULL, 10);
	uint8_t acc[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	if(b64_decode(account, acc, sizeof(acc)) != sizeof(acc))
	{
		printf("{\"ok\":false,\"error\":\"PSN account id không hợp lệ\"}\n");
		return 2;
	}
	memcpy(info.psn_account_id, acc, sizeof(acc));

	chiaki_bool_pred_cond_init(&regist_done);
	ChiakiRegist regist;
	ChiakiErrorCode err = chiaki_regist_start(&regist, log, &info, regist_cb, NULL);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		printf("{\"ok\":false,\"error\":\"Không bắt đầu được ghép đôi: %s\"}\n", chiaki_error_string(err));
		return 1;
	}

	chiaki_bool_pred_cond_lock(&regist_done);
	err = chiaki_bool_pred_cond_timedwait(&regist_done, 60000);
	int result = regist_result;
	chiaki_bool_pred_cond_unlock(&regist_done);

	if(err != CHIAKI_ERR_SUCCESS || result == RESULT_NONE)
		chiaki_regist_stop(&regist);
	chiaki_regist_fini(&regist);

	if(result != RESULT_OK)
	{
		printf("{\"ok\":false,\"error\":\"%s\"}\n",
			result == RESULT_NONE ? "Hết thời gian chờ ghép đôi (kiểm tra PIN, địa chỉ IP và PS5 đang bật)"
			: "Ghép đôi thất bại (PIN sai/hết hạn, hoặc PSN account id không đúng)");
		return 1;
	}

	printf("{\"ok\":true,\"target\":%d,\"nickname\":", (int)regist_host.target);
	json_escape_print(stdout, regist_host.server_nickname);
	printf(",\"regist_key\":\"");
	hex_print((const uint8_t *)regist_host.rp_regist_key, sizeof(regist_host.rp_regist_key));
	printf("\",\"rp_key\":\"");
	hex_print(regist_host.rp_key, sizeof(regist_host.rp_key));
	printf("\"}\n");
	fflush(stdout);
	return 0;
}

// ---------------------------------------------------------------------------
// wakeup
// ---------------------------------------------------------------------------
static int cmd_wakeup(ChiakiLog *log, int argc, char **argv)
{
	const char *host = arg_value(argc, argv, "--host");
	const char *key_hex = arg_value(argc, argv, "--regist-key");
	bool ps4 = arg_flag(argc, argv, "--ps4");
	uint8_t key[16];
	if(!host || !hex_decode(key_hex, key, sizeof(key)))
	{
		printf("{\"ok\":false,\"error\":\"Thiếu/sai --host hoặc --regist-key\"}\n");
		return 2;
	}
	// Chiaki dùng chuỗi hex (ASCII) của regist key làm credential
	char txt[17];
	memcpy(txt, key, 16);
	txt[16] = 0;
	uint64_t credential = (uint64_t)strtoull(txt, NULL, 16);
	ChiakiErrorCode err = chiaki_discovery_wakeup(log, NULL, host, credential, !ps4);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		printf("{\"ok\":false,\"error\":\"Gửi gói đánh thức thất bại: %s\"}\n", chiaki_error_string(err));
		return 1;
	}
	printf("{\"ok\":true}\n");
	fflush(stdout);
	return 0;
}

// ---------------------------------------------------------------------------
// stream
// ---------------------------------------------------------------------------
static ChiakiSession session;
static ChiakiMutex out_mutex;
static uint8_t out_codec = 0;
static volatile bool quitting = false;
static volatile bool connected = false;

static bool video_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	(void)frame_recovered;
	(void)user;
	static unsigned long vcount = 0;
	vcount++;
	if(vcount <= 10 || vcount % 300 == 0)
		flog("video_cb #%lu size=%lu lost=%d", vcount, (unsigned long)buf_size, (int)frames_lost);
	if(quitting || !buf || buf_size == 0)
		return true;
	uint8_t hdr[6];
	hdr[0] = (uint8_t)(buf_size & 0xff);
	hdr[1] = (uint8_t)((buf_size >> 8) & 0xff);
	hdr[2] = (uint8_t)((buf_size >> 16) & 0xff);
	hdr[3] = (uint8_t)((buf_size >> 24) & 0xff);
	hdr[4] = out_codec;
	hdr[5] = frames_lost > 0 ? 1 : 0;
	chiaki_mutex_lock(&out_mutex);
	bool ok = fwrite(hdr, 1, sizeof(hdr), stdout) == sizeof(hdr) && fwrite(buf, 1, buf_size, stdout) == buf_size;
	if(ok)
		fflush(stdout);
	chiaki_mutex_unlock(&out_mutex);
	if(!ok)
	{
		// Người nhận (VietGame) đã đóng ống: dừng stream
		quitting = true;
		chiaki_session_stop(&session);
	}
	return true;
}

static ChiakiBoolPredCond session_ended;

static void event_cb(ChiakiEvent *event, void *user)
{
	(void)user;
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			flog("EVENT connected");
			connected = true;
			event_json("connected", NULL, NULL);
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			event_json("error", "message", "PS4 yêu cầu PIN đăng nhập (chưa hỗ trợ)");
			chiaki_session_stop(&session);
			break;
		case CHIAKI_EVENT_QUIT:
		{
			char buf[256];
			snprintf(buf, sizeof(buf), "%s%s%s", chiaki_quit_reason_string(event->quit.reason),
				event->quit.reason_str ? ": " : "", event->quit.reason_str ? event->quit.reason_str : "");
			fprintf(stderr, "{\"event\":\"quit\",\"error\":%s,\"reason\":", chiaki_quit_reason_is_error(event->quit.reason) ? "true" : "false");
			json_escape_print(stderr, buf);
			fprintf(stderr, ",\"code\":%d}\n", (int)event->quit.reason);
			fflush(stderr);
			chiaki_bool_pred_cond_lock(&session_ended);
			session_ended.pred = true;
			chiaki_bool_pred_cond_signal(&session_ended);
			chiaki_bool_pred_cond_unlock(&session_ended);
			break;
		}
		default:
			break;
	}
}

// libchiaki gọi hàm này KHÔNG kiểm tra rỗng: bắt buộc phải đặt, nếu không sẽ sập (0xC0000005)
static void cantdisplay_cb(void *user, bool cant_display)
{
	(void)user;
	event_json(cant_display ? "cant_display" : "can_display", NULL, NULL);
}

static void *stdin_thread(void *arg)
{
	(void)arg;
	char line[128];
	while(fgets(line, sizeof(line), stdin))
	{
		if(strncmp(line, "quit", 4) == 0)
			break;
		// Chỉ xin khung chính khi đã kết nối: gọi sớm hơn sẽ chạm phần stream chưa khởi tạo và sập
		if(strncmp(line, "idr", 3) == 0 && connected && !quitting)
			chiaki_session_request_idr(&session);
	}
	// stdin đóng hoặc nhận "quit"
	quitting = true;
	chiaki_session_stop(&session);
	return NULL;
}

static int cmd_stream(ChiakiLog *log, int argc, char **argv)
{
	const char *host = arg_value(argc, argv, "--host");
	const char *key_hex = arg_value(argc, argv, "--regist-key");
	const char *morning_hex = arg_value(argc, argv, "--morning");
	const char *res_s = arg_value(argc, argv, "--res");
	const char *fps_s = arg_value(argc, argv, "--fps");
	const char *codec_s = arg_value(argc, argv, "--codec");
	const char *bitrate_s = arg_value(argc, argv, "--bitrate");
	bool ps4 = arg_flag(argc, argv, "--ps4");

	ChiakiConnectInfo ci;
	memset(&ci, 0, sizeof(ci));
	uint8_t key[16];
	if(!host || !hex_decode(key_hex, key, sizeof(key)) || !hex_decode(morning_hex, ci.morning, sizeof(ci.morning)))
	{
		event_json("error", "message", "Thiếu/sai --host, --regist-key hoặc --morning");
		return 2;
	}
	memcpy(ci.regist_key, key, sizeof(ci.regist_key));
	ci.ps5 = !ps4;
	ci.host = host;

	int res = res_s ? atoi(res_s) : 720;
	ChiakiVideoResolutionPreset rp = res >= 1080 ? CHIAKI_VIDEO_RESOLUTION_PRESET_1080p
		: res >= 720 ? CHIAKI_VIDEO_RESOLUTION_PRESET_720p
		: res >= 540 ? CHIAKI_VIDEO_RESOLUTION_PRESET_540p
		: CHIAKI_VIDEO_RESOLUTION_PRESET_360p;
	int fps = fps_s ? atoi(fps_s) : 30;
	chiaki_connect_video_profile_preset(&ci.video_profile, rp, fps >= 60 ? CHIAKI_VIDEO_FPS_PRESET_60 : CHIAKI_VIDEO_FPS_PRESET_30);
	if(bitrate_s && atoi(bitrate_s) > 0)
		ci.video_profile.bitrate = (unsigned int)atoi(bitrate_s);
	bool h265 = codec_s && strcmp(codec_s, "h265") == 0 && !ps4;
	ci.video_profile.codec = h265 ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
	out_codec = h265 ? 1 : 0;
	ci.video_profile_auto_downgrade = true;
	ci.enable_keyboard = false;
	ci.enable_dualsense = false;
	ci.audio_video_disabled = CHIAKI_AUDIO_DISABLED; // VietGame chỉ cần hình; tiếng vẫn phát trên TV
	ci.auto_regist = false;
	ci.packet_loss_max = 0.05;
	ci.enable_idr_on_fec_failure = true;

#ifdef _WIN32
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stdin), _O_BINARY);
#endif
	setvbuf(stdout, NULL, _IOFBF, 1 << 16);

	chiaki_mutex_init(&out_mutex, false);
	chiaki_bool_pred_cond_init(&session_ended);

	flog("stream: host=%s res=%d fps=%d codec=%s", host, res, fps, h265 ? "h265" : "h264");
	memset(&session, 0, sizeof(session));
	ChiakiErrorCode err = chiaki_session_init(&session, &ci, log);
	flog("session_init -> %d", (int)err);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		event_json("error", "message", chiaki_error_string(err));
		return 1;
	}
	chiaki_session_set_event_cb(&session, event_cb, NULL);
	chiaki_session_set_video_sample_cb(&session, video_cb, NULL);
	ChiakiCtrlDisplaySink display_sink;
	memset(&display_sink, 0, sizeof(display_sink));
	display_sink.cantdisplay_cb = cantdisplay_cb;
	chiaki_session_ctrl_set_display_sink(&session, &display_sink);

	err = chiaki_session_start(&session);
	flog("session_start -> %d", (int)err);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		event_json("error", "message", chiaki_error_string(err));
		chiaki_session_fini(&session);
		return 1;
	}
	event_json("starting", NULL, NULL);

	ChiakiThread in_thread;
	bool in_started = chiaki_thread_create(&in_thread, stdin_thread, NULL) == CHIAKI_ERR_SUCCESS;

	chiaki_session_join(&session);
	flog("session_join returned");
	chiaki_session_fini(&session);
	flog("session_fini done");
	(void)in_started; // luồng đọc stdin sẽ tự thoát khi tiến trình kết thúc
	return 0;
}

#ifdef _WIN32
// Khi sập: ghi vị trí lỗi và vết gọi hàm vào vg-chiaki.log (offset so với base, đối chiếu bảng ký hiệu của bản build)
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
	unsigned long long info0 = ep->ExceptionRecord->NumberParameters > 0 ? (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0] : 0;
	unsigned long long info1 = ep->ExceptionRecord->NumberParameters > 1 ? (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1] : 0;
	unsigned long long base = (unsigned long long)(uintptr_t)GetModuleHandle(NULL);
	unsigned long long addr = (unsigned long long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
	flog("CRASH code=0x%08lx addr=0x%llx base=0x%llx offset=0x%llx rw=%llu target=0x%llx thread=%lu",
		(unsigned long)ep->ExceptionRecord->ExceptionCode, addr, base, addr - base, info0, info1, (unsigned long)GetCurrentThreadId());
	void *frames[32];
	USHORT n = CaptureStackBackTrace(0, 32, frames, NULL);
	for(USHORT i = 0; i < n; i++)
		flog("  frame %u: 0x%llx (offset 0x%llx)", (unsigned)i, (unsigned long long)(uintptr_t)frames[i], (unsigned long long)(uintptr_t)frames[i] - base);
	fprintf(stderr, "{\"event\":\"crash\",\"code\":\"0x%08lx\",\"offset\":\"0x%llx\"}\n", (unsigned long)ep->ExceptionRecord->ExceptionCode, addr - base);
	fflush(stderr);
	return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char **argv)
{
#ifdef _WIN32
	SetUnhandledExceptionFilter(crash_filter);
#endif
	if(argc >= 2 && strcmp(argv[1], "stream") == 0)
		flog_open();
	if(argc < 2)
	{
		fprintf(stderr, "Dùng: vg-chiaki regist|wakeup|stream ...\n");
		return 2;
	}
	if(chiaki_lib_init() != CHIAKI_ERR_SUCCESS)
	{
		fprintf(stderr, "chiaki_lib_init thất bại\n");
		return 1;
	}
	ChiakiLog log;
	chiaki_log_init(&log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING | CHIAKI_LOG_INFO, log_cb, NULL);

	int r;
	if(strcmp(argv[1], "regist") == 0) r = cmd_regist(&log, argc, argv);
	else if(strcmp(argv[1], "wakeup") == 0) r = cmd_wakeup(&log, argc, argv);
	else if(strcmp(argv[1], "stream") == 0) r = cmd_stream(&log, argc, argv);
	else
	{
		fprintf(stderr, "Lệnh không hợp lệ: %s\n", argv[1]);
		r = 2;
	}
	fflush(stdout);
	fflush(stderr);
	// Thoát ngay để không bị kẹt chờ luồng stdin
	_Exit(r);
}
