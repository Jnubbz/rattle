// rattle — Rattlegram-compatible text messaging over a radio, keyed by CAT or RTS/DTR (no VOX).
//
// Modem: aicodix Rattlegram encoder/decoder (0BSD), unchanged, so it interoperates
// with the Rattlegram Android/iOS apps. Audio: miniaudio.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>

#define assert(expr) do {} while (0)
#include "encoder.hh"
#include "decoder.hh"
#undef assert

#define MA_NO_ENCODING_MP3
#define MA_NO_FLAC
#define MA_NO_MP3
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "serial.hpp"

static const char *VERSION = "0.1.0";
static const int RATE = 48000;
static const int SYMBOL_LENGTH = (1280 * RATE) / 8000;
static const int EXTENDED_LENGTH = SYMBOL_LENGTH + SYMBOL_LENGTH / 8;
static const int MAX_PAYLOAD = 170;

// ---------------------------------------------------------------- config

struct Config {
	std::map<std::string, std::string> kv = {
		{"callsign", "N0CALL"},
		{"carrier", "1500"},      // audio centre frequency, Hz (Rattlegram default)
		{"noise", "1"},           // leading noise symbols (0..6); helps the far receiver's AGC settle
		{"fancy", "false"},       // Rattlegram "fancy header" (callsign drawn in the waterfall)
		{"input", ""},            // capture device: name substring or index; empty = system default
		{"output", ""},           // playback device: name substring or index; empty = system default
		{"level", "0.5"},         // transmit audio level 0..1 (set radio drive/ALC with this)
		{"port", ""},             // serial port for PTT: COM5, /dev/cu.SLAB_USBtoUART, ...
		{"ptt", "none"},          // none | cat | rts | dtr
		{"baud", "115200"},
		{"cat_on", "TX;"},        // FX-4CR (Kenwood style). FT-710: TX1;
		{"cat_off", "RX;"},       // FX-4CR (Kenwood style). FT-710: TX0;
		{"lead_ms", "150"},       // wait after keying before audio starts
		{"tail_ms", "100"},       // wait after audio ends before unkeying
		{"max_tx_s", "8"},        // transmit watchdog
		{"log", ""},              // append received/sent messages to this file
		{"on_message", ""},       // shell command run per received message; env RATTLE_FROM, RATTLE_TEXT, RATTLE_TIME
		{"verbose", "false"},
		{"format", "text"},       // text | json (one JSON object per line, for the Radio Messages app)
		{"names", ""},            // display names, e.g. KN0BLE=Josh,W1AW=ARRL
		{"no_audio_s", "0"},      // exit with an error if the input goes silent-dead this long (0 = never)
	};

	std::string get(const std::string &k) const {
		auto it = kv.find(k);
		return it == kv.end() ? "" : it->second;
	}
	int geti(const std::string &k) const { return std::atoi(get(k).c_str()); }
	double getf(const std::string &k) const { return std::atof(get(k).c_str()); }
	bool getb(const std::string &k) const {
		std::string v = get(k);
		return v == "1" || v == "true" || v == "yes" || v == "on";
	}

	static std::string trim(std::string s) {
		auto ws = [](unsigned char c) { return std::isspace(c); };
		while (!s.empty() && ws(s.back())) s.pop_back();
		size_t i = 0;
		while (i < s.size() && ws(s[i])) ++i;
		return s.substr(i);
	}

	bool load(const std::string &path) {
		std::ifstream f(path);
		if (!f)
			return false;
		std::string line;
		while (std::getline(f, line)) {
			line = trim(line);
			if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
				continue;
			auto eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
			if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
				v = v.substr(1, v.size() - 2);
			if (!kv.count(k))
				std::fprintf(stderr, "warning: unknown setting '%s' in %s\n", k.c_str(), path.c_str());
			kv[k] = v;
		}
		return true;
	}
};

// ---------------------------------------------------------------- utils

static std::atomic<bool> g_quit{false};

static std::string now_str(bool date = false) {
	std::time_t t = std::time(nullptr);
	char buf[32];
	std::strftime(buf, sizeof buf, date ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S", std::localtime(&t));
	return buf;
}

static void sleep_ms(int ms) {
	if (ms > 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static std::string lower(std::string s) {
	for (auto &c : s) c = (char) std::tolower((unsigned char) c);
	return s;
}

static std::string cstr_trim(const uint8_t *p, size_t n) {
	size_t len = 0;
	while (len < n && p[len]) ++len;
	return Config::trim(std::string(reinterpret_cast<const char *>(p), len));
}

static std::mutex g_out_mutex;
static void say(const std::string &s) {
	std::lock_guard<std::mutex> lk(g_out_mutex);
	std::cout << s << std::endl;
}

static bool g_json = false;

static std::string json_escape(const std::string &in) {
	std::string o;
	for (unsigned char c : in) {
		switch (c) {
			case '"': o += "\\\""; break;
			case '\\': o += "\\\\"; break;
			case '\n': o += "\\n"; break;
			case '\r': o += "\\r"; break;
			case '\t': o += "\\t"; break;
			default:
				if (c < 0x20) {
					char b[8];
					std::snprintf(b, sizeof b, "\\u%04x", c);
					o += b;
				} else
					o += (char) c;
		}
	}
	return o;
}

// emit({"event","message"},{"from","KN0BLE"},...) -> {"event":"message","from":"KN0BLE",...}
static void emit(std::initializer_list<std::pair<const char *, std::string>> fields) {
	std::string line = "{";
	bool first = true;
	for (auto &f : fields) {
		if (!first) line += ",";
		first = false;
		line += "\"" + std::string(f.first) + "\":\"" + json_escape(f.second) + "\"";
	}
	line += "}";
	say(line);
}

static std::string display_name(const Config &cfg, const std::string &call) {
	std::stringstream ss(cfg.get("names"));
	std::string item;
	while (std::getline(ss, item, ',')) {
		auto eq = item.find('=');
		if (eq == std::string::npos) continue;
		std::string c = Config::trim(item.substr(0, eq));
		for (auto &ch : c) ch = (char) std::toupper((unsigned char) ch);
		if (c == call) return Config::trim(item.substr(eq + 1));
	}
	return call;
}

static void log_line(const Config &cfg, const std::string &dir, const std::string &call, const std::string &text) {
	std::string path = cfg.get("log");
	if (path.empty())
		return;
	std::ofstream f(path, std::ios::app);
	if (f)
		f << now_str(true) << '\t' << dir << '\t' << call << '\t' << text << '\n';
}

static void set_env(const char *k, const std::string &v) {
#ifdef _WIN32
	_putenv_s(k, v.c_str());
#else
	setenv(k, v.c_str(), 1);
#endif
}

static std::mutex g_hook_mutex;
static std::atomic<int> g_hooks_running{0};
static void wait_hooks(int max_ms) {
	for (int t = 0; g_hooks_running > 0 && t < max_ms; t += 20)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
}
static void run_hook(const Config &cfg, const std::string &from, const std::string &text) {
	std::string cmd = cfg.get("on_message");
	if (cmd.empty())
		return;
	// The message only ever reaches the command through environment variables,
	// never by pasting it into the command line, so received text can't inject shell syntax.
	++g_hooks_running;
	std::thread([cmd, from, text] {
		std::lock_guard<std::mutex> lk(g_hook_mutex);
		set_env("RATTLE_FROM", from);
		set_env("RATTLE_TEXT", text);
		set_env("RATTLE_TIME", now_str(true));
		int rc = std::system(cmd.c_str());
		(void) rc;
		--g_hooks_running;
	}).detach();
}

// ---------------------------------------------------------------- PTT

class Ptt {
public:
	explicit Ptt(const Config &cfg) : cfg(cfg) {
		mode = lower(cfg.get("ptt"));
		if (mode != "none" && mode != "cat" && mode != "rts" && mode != "dtr")
			throw std::runtime_error("ptt must be none, cat, rts or dtr");
		if (mode != "none" && cfg.get("port").empty())
			throw std::runtime_error("ptt=" + mode + " needs port= (e.g. COM5 or /dev/cu.SLAB_USBtoUART)");
	}

	// Opens the serial port on first use, so a busy or missing port never stops receiving.
	void prepare() {
		if (mode != "none" && !port.is_open())
			port.open(cfg.get("port"), cfg.geti("baud"));
	}
	~Ptt() { off(); }

	void on() {
		prepare();
		keyed = true;
		if (mode == "cat") port.write(cfg.get("cat_on"));
		else if (mode == "rts") port.rts(true);
		else if (mode == "dtr") port.dtr(true);
	}

	void off() {
		if (!keyed)
			return;
		keyed = false;
		if (mode == "cat") port.write(cfg.get("cat_off"));
		else if (mode == "rts") port.rts(false);
		else if (mode == "dtr") port.dtr(false);
	}

	bool keys_radio() const { return mode != "none"; }

private:
	const Config &cfg;
	std::string mode;
	SerialPort port;
	bool keyed = false;
};

static Ptt *g_ptt = nullptr; // for emergency unkey on Ctrl+C

static void on_signal(int) {
	g_quit = true;
}

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD) {
	g_quit = true;
	return TRUE;
}
#endif

// ---------------------------------------------------------------- audio devices

struct Audio {
	ma_context ctx;
	bool ok = false;
	Audio() {
		// RATTLE_NULL_AUDIO=1 uses a silent virtual device (for testing without a sound card).
		ma_backend null_backend = ma_backend_null;
		bool use_null = std::getenv("RATTLE_NULL_AUDIO") != nullptr;
		ok = ma_context_init(use_null ? &null_backend : nullptr, use_null ? 1 : 0, nullptr, &ctx) == MA_SUCCESS;
	}
	~Audio() { if (ok) ma_context_uninit(&ctx); }

	// Returns nullptr for the default device.
	const ma_device_id *find(ma_device_type type, const std::string &want, std::string &name_out) {
		ma_device_info *play, *cap;
		ma_uint32 nplay, ncap;
		if (ma_context_get_devices(&ctx, &play, &nplay, &cap, &ncap) != MA_SUCCESS)
			throw std::runtime_error("cannot list audio devices");
		ma_device_info *list = type == ma_device_type_capture ? cap : play;
		ma_uint32 n = type == ma_device_type_capture ? ncap : nplay;
		if (want.empty()) {
			name_out = "system default";
			return nullptr;
		}
		bool numeric = std::all_of(want.begin(), want.end(), ::isdigit);
		for (ma_uint32 i = 0; i < n; ++i) {
			bool match = numeric ? (std::to_string(i) == want) : lower(list[i].name).find(lower(want)) != std::string::npos;
			if (match) {
				name_out = list[i].name;
				ids.push_back(list[i].id);
				return &ids.back();
			}
		}
		throw std::runtime_error(std::string("no ") + (type == ma_device_type_capture ? "input" : "output") +
			" device matches '" + want + "' (run: rattle devices)");
	}

	void print() {
		ma_device_info *play, *cap;
		ma_uint32 nplay, ncap;
		if (ma_context_get_devices(&ctx, &play, &nplay, &cap, &ncap) != MA_SUCCESS)
			throw std::runtime_error("cannot list audio devices");
		std::printf("Input devices (input=):\n");
		for (ma_uint32 i = 0; i < ncap; ++i)
			std::printf("  %u: %s%s\n", i, cap[i].name, cap[i].isDefault ? "  [default]" : "");
		std::printf("Output devices (output=):\n");
		for (ma_uint32 i = 0; i < nplay; ++i)
			std::printf("  %u: %s%s\n", i, play[i].name, play[i].isDefault ? "  [default]" : "");
	}

private:
	std::deque<ma_device_id> ids; // stable addresses
};

// ---------------------------------------------------------------- encode

static std::vector<int16_t> encode_message(const Config &cfg, const std::string &text) {
	if (text.size() > MAX_PAYLOAD)
		throw std::runtime_error("message is " + std::to_string(text.size()) + " bytes; the limit is 170");
	std::string call = cfg.get("callsign");
	if (call.size() > 9)
		throw std::runtime_error("callsign is limited to 9 characters");
	for (auto &c : call) c = (char) std::toupper((unsigned char) c);

	uint8_t payload[MAX_PAYLOAD + 1] = {};
	std::memcpy(payload, text.data(), text.size());
	int8_t callsign[10] = {};
	std::memcpy(callsign, call.data(), call.size());

	auto enc = std::make_unique<Encoder<RATE>>();
	enc->configure(payload, callsign, cfg.geti("carrier"), std::clamp(cfg.geti("noise"), 0, 6), cfg.getb("fancy"));

	std::vector<int16_t> audio;
	std::vector<int16_t> sym(EXTENDED_LENGTH);
	while (enc->produce(sym.data(), 0))
		audio.insert(audio.end(), sym.begin(), sym.end());
	audio.insert(audio.end(), sym.begin(), sym.end()); // one symbol of trailing silence

	double level = std::clamp(cfg.getf("level"), 0.0, 1.0);
	for (auto &s : audio)
		s = (int16_t) std::lround(s * level);
	return audio;
}

static void write_wav(const std::string &path, const std::vector<int16_t> &audio) {
	ma_encoder_config ec = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 1, RATE);
	ma_encoder enc;
	if (ma_encoder_init_file(path.c_str(), &ec, &enc) != MA_SUCCESS)
		throw std::runtime_error("cannot write " + path);
	ma_encoder_write_pcm_frames(&enc, audio.data(), audio.size(), nullptr);
	ma_encoder_uninit(&enc);
}

// ---------------------------------------------------------------- decode

class Receiver {
public:
	explicit Receiver(const Config &cfg) : cfg(cfg), dec(std::make_unique<Decoder<RATE>>()) {}

	// Returns number of messages decoded in this call.
	int feed(const int16_t *samples, size_t n) {
		int got = 0;
		while (n) {
			int chunk = (int) std::min<size_t>(n, RATE / 50);
			if (dec->feed(samples, chunk, 0))
				got += handle(dec->process());
			samples += chunk;
			n -= chunk;
		}
		return got;
	}

private:
	const Config &cfg;
	std::unique_ptr<Decoder<RATE>> dec;
	float cfo = 0;
	int32_t mode = 0;
	uint8_t call[10] = {};
	uint8_t payload[MAX_PAYLOAD] = {};

	std::string staged_call() {
		dec->staged(&cfo, &mode, call);
		return cstr_trim(call, 9);
	}

	int handle(int status) {
		bool verbose = cfg.getb("verbose");
		char buf[128];
		switch (status) {
			case STATUS_SYNC: {
				std::string from = staged_call();
				if (verbose && !g_json) {
					std::snprintf(buf, sizeof buf, "[%s] receiving from %s (mode %d, centre %.1f Hz)",
						now_str().c_str(), from.c_str(), (int) mode, cfo);
					say(buf);
				}
				break;
			}
			case STATUS_DONE: {
				std::string from = cstr_trim(call, 9);
				int flips = dec->fetch(payload);
				if (flips < 0) {
					if (g_json) emit({{"event", "failed"}, {"from", from}, {"name", display_name(cfg, from)}, {"time", now_str(true)}});
					else say("[" + now_str() + "] " + from + ": decoding failed (signal too weak or distorted)");
					log_line(cfg, "RX", from, "(could not decode)");
					return 0;
				}
				std::string text = cstr_trim(payload, MAX_PAYLOAD);
				std::string line = "[" + now_str() + "] " + from + ": " + text;
				if (verbose) {
					std::snprintf(buf, sizeof buf, "  (centre %.1f Hz, %d bits corrected)", cfo, flips);
					line += buf;
				}
				if (g_json) emit({{"event", "message"}, {"from", from}, {"name", display_name(cfg, from)}, {"text", text}, {"time", now_str(true)}});
				else say(line);
				log_line(cfg, "RX", from, text);
				run_hook(cfg, from, text);
				return 1;
			}
			case STATUS_PING: {
				std::string from = staged_call();
				if (g_json) emit({{"event", "ping"}, {"from", from}, {"name", display_name(cfg, from)}, {"time", now_str(true)}});
				else say("[" + now_str() + "] ping from " + from);
				log_line(cfg, "RX", from, "(ping)");
				run_hook(cfg, from, "");
				break;
			}
			case STATUS_NOPE: {
				std::string from = staged_call();
				if (!g_json) say("[" + now_str() + "] " + from + ": unsupported mode " + std::to_string(mode));
				break;
			}
			case STATUS_FAIL:
				if (verbose && !g_json)
					say("[" + now_str() + "] preamble failed");
				break;
			case STATUS_HEAP:
				throw std::runtime_error("decoder out of memory");
		}
		return 0;
	}
};

// ---------------------------------------------------------------- live station

class Station {
public:
	explicit Station(const Config &cfg) : cfg(cfg), rx(cfg), ptt(cfg) {}

	~Station() {
		stop_capture();
	}

	void start_capture() {
		std::string name;
		const ma_device_id *id = audio.find(ma_device_type_capture, cfg.get("input"), name);
		ma_device_config dc = ma_device_config_init(ma_device_type_capture);
		dc.capture.pDeviceID = id;
		dc.capture.format = ma_format_s16;
		dc.capture.channels = 1;
		dc.sampleRate = RATE;
		dc.dataCallback = &Station::capture_cb;
		dc.pUserData = this;
		if (ma_device_init(&audio.ctx, &dc, &cap) != MA_SUCCESS)
			throw std::runtime_error("cannot open input device " + name);
		cap_open = true;
		started_ms = now_ms();
		if (ma_device_start(&cap) != MA_SUCCESS)
			throw std::runtime_error("cannot start input device " + name);
		if (g_json) emit({{"event", "listening"}, {"device", name}, {"can_send", ptt.keys_radio() ? "yes" : "no"}});
		else say("Listening on: " + name);
	}

	void stop_capture() {
		if (cap_open) {
			ma_device_uninit(&cap);
			cap_open = false;
		}
	}

	// Pull captured audio into the decoder. Call regularly from the main thread.
	void poll() {
		std::vector<int16_t> chunk;
		{
			std::lock_guard<std::mutex> lk(cap_mutex);
			chunk.assign(cap_buf.begin(), cap_buf.end());
			cap_buf.clear();
		}
		if (!chunk.empty())
			rx.feed(chunk.data(), chunk.size());
	}

	void transmit(const std::string &text) {
		std::vector<int16_t> sig = encode_message(cfg, text);
		if (!ptt.keys_radio()) {
			if (g_json) throw std::runtime_error("sending is not set up (ptt = none)");
			say("note: ptt=none, so the radio will not be keyed");
		}
		ptt.prepare(); // fail here, before touching audio, if the port can't be opened
		if (g_json) emit({{"event", "sending"}, {"text", text}});

		std::string name;
		const ma_device_id *id = audio.find(ma_device_type_playback, cfg.get("output"), name);
		ma_device_config dc = ma_device_config_init(ma_device_type_playback);
		dc.playback.pDeviceID = id;
		dc.playback.format = ma_format_s16;
		dc.playback.channels = 1;
		dc.sampleRate = RATE;
		dc.dataCallback = &Station::playback_cb;
		dc.pUserData = this;
		ma_device play;
		if (ma_device_init(&audio.ctx, &dc, &play) != MA_SUCCESS)
			throw std::runtime_error("cannot open output device " + name);

		tx_audio = &sig;
		tx_pos = 0;
		tx_done = false;
		muted = true; // don't decode while we transmit

		auto t0 = std::chrono::steady_clock::now();
		double max_s = std::max(1.0, cfg.getf("max_tx_s"));
		bool aborted = false;

		ptt.on();
		sleep_ms(cfg.geti("lead_ms"));
		ma_device_start(&play);
		while (!tx_done) {
			sleep_ms(10);
			double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			if (secs > max_s || g_quit) {
				aborted = true;
				break;
			}
		}
		// Let the device drain what it has buffered before unkeying.
		sleep_ms(aborted ? 0 : 60 + (int) (play.playback.internalPeriodSizeInFrames * play.playback.internalPeriods * 1000 / RATE));
		ma_device_uninit(&play);
		sleep_ms(cfg.geti("tail_ms"));
		ptt.off();
		tx_audio = nullptr;

		sleep_ms(150); // radio back to receive
		{
			std::lock_guard<std::mutex> lk(cap_mutex);
			cap_buf.clear();
		}
		muted = false;

		if (aborted) {
			if (g_json) emit({{"event", "send_error"}, {"message", "transmission stopped by the safety timer"}});
			else say("[" + now_str() + "] TRANSMIT ABORTED (watchdog or Ctrl+C), radio unkeyed");
		} else if (g_json) {
			emit({{"event", "sent"}, {"text", text}, {"time", now_str(true)}});
			log_line(cfg, "TX", cfg.get("callsign"), text.empty() ? "(ping)" : text);
		} else {
			char buf[64];
			std::snprintf(buf, sizeof buf, " (%.1f s)", sig.size() / double(RATE));
			say("[" + now_str() + "] sent as " + cfg.get("callsign") + ": " + (text.empty() ? "(ping)" : text) + buf);
			log_line(cfg, "TX", cfg.get("callsign"), text.empty() ? "(ping)" : text);
		}
	}

	void ptt_test(int ms) {
		if (!ptt.keys_radio())
			throw std::runtime_error("ptt=none; set ptt=cat, rts or dtr and port=");
		say("Keying for " + std::to_string(ms) + " ms (no audio) ...");
		ptt.on();
		auto t0 = std::chrono::steady_clock::now();
		while (!g_quit && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms))
			sleep_ms(10);
		ptt.off();
		say("Unkeyed.");
	}

	Ptt &get_ptt() { return ptt; }

private:
	const Config &cfg;
	Audio audio;
	Receiver rx;
	Ptt ptt;

	ma_device cap;
	bool cap_open = false;
	std::mutex cap_mutex;
	std::vector<int16_t> cap_buf;
	std::atomic<bool> muted{false};
	std::atomic<long long> last_audio_ms{0};

	static long long now_ms() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

public:
	// Seconds since the input device last delivered audio (0 while transmitting).
	double audio_silence_s() {
		if (muted) return 0;
		long long last = last_audio_ms;
		if (!last) last = started_ms;
		return (now_ms() - last) / 1000.0;
	}
	long long started_ms = now_ms();

private:

	std::vector<int16_t> *tx_audio = nullptr;
	std::atomic<size_t> tx_pos{0};
	std::atomic<bool> tx_done{false};

	static void capture_cb(ma_device *dev, void *, const void *in, ma_uint32 frames) {
		auto *self = static_cast<Station *>(dev->pUserData);
		if (self->muted)
			return;
		auto *s = static_cast<const int16_t *>(in);
		self->last_audio_ms = now_ms();
		std::lock_guard<std::mutex> lk(self->cap_mutex);
		if (self->cap_buf.size() < (size_t) RATE * 10) // never grow without bound
			self->cap_buf.insert(self->cap_buf.end(), s, s + frames);
	}

	static void playback_cb(ma_device *dev, void *out, const void *, ma_uint32 frames) {
		auto *self = static_cast<Station *>(dev->pUserData);
		auto *o = static_cast<int16_t *>(out);
		size_t pos = self->tx_pos;
		size_t total = self->tx_audio ? self->tx_audio->size() : 0;
		size_t n = std::min<size_t>(frames, total > pos ? total - pos : 0);
		if (n)
			std::memcpy(o, self->tx_audio->data() + pos, n * sizeof(int16_t));
		std::memset(o + n, 0, (frames - n) * sizeof(int16_t));
		self->tx_pos = pos + n;
		if (pos + n >= total)
			self->tx_done = true;
	}
};

// ---------------------------------------------------------------- commands

static void usage() {
	std::printf(
		"rattle %s - Rattlegram-compatible messaging over radio (CAT/RTS PTT, no VOX)\n\n"
		"usage:\n"
		"  rattle [chat]               listen, and send each line you type (default)\n"
		"  rattle listen               receive only\n"
		"  rattle send \"message\"       send one message and exit (empty message = ping)\n"
		"  rattle devices              list audio devices\n"
		"  rattle ptt-test [ms]        key the radio with no audio (default 1000 ms)\n"
		"  rattle send \"msg\" --wav out.wav    write the signal to a WAV file instead\n"
		"  rattle listen --wav in.wav         decode a WAV file\n\n"
		"settings come from rattle.ini (current folder, then next to the program), or --config FILE;\n"
		"any setting can be overridden on the command line, e.g. --ptt cat --port COM5\n",
		VERSION);
}

static std::string exe_dir(const char *argv0) {
	std::string p = argv0;
	auto slash = p.find_last_of("/\\");
	return slash == std::string::npos ? "" : p.substr(0, slash + 1);
}

int main(int argc, char **argv) {
	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);
#ifdef _WIN32
	SetConsoleCtrlHandler(console_handler, TRUE);
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
#endif

	Config cfg;
	std::string command, wav, config_path;
	std::vector<std::string> positional;
	std::map<std::string, std::string> overrides;

	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "-h" || a == "--help") {
			usage();
			return 0;
		} else if (a == "--version") {
			std::printf("rattle %s\n", VERSION);
			return 0;
		} else if (a.rfind("--", 0) == 0) {
			std::string k = a.substr(2);
			std::replace(k.begin(), k.end(), '-', '_');
			if (i + 1 >= argc) {
				std::fprintf(stderr, "missing value for %s\n", a.c_str());
				return 2;
			}
			std::string v = argv[++i];
			if (k == "wav") wav = v;
			else if (k == "config") config_path = v;
			else overrides[k] = v;
		} else if (command.empty()) {
			command = a;
		} else {
			positional.push_back(a);
		}
	}
	if (command.empty())
		command = "chat";

	if (!config_path.empty()) {
		if (!cfg.load(config_path)) {
			std::fprintf(stderr, "cannot read %s\n", config_path.c_str());
			return 2;
		}
	} else if (!cfg.load("rattle.ini")) {
		cfg.load(exe_dir(argv[0]) + "rattle.ini");
	}
	for (auto &kv : overrides) {
		if (!cfg.kv.count(kv.first)) {
			std::fprintf(stderr, "unknown option --%s\n", kv.first.c_str());
			return 2;
		}
		cfg.kv[kv.first] = kv.second;
	}

	g_json = lower(cfg.get("format")) == "json";

	try {
		if (command == "devices") {
			Audio a;
			if (!a.ok) throw std::runtime_error("cannot initialise audio");
			a.print();
			return 0;
		}

		if (command == "send" && !wav.empty()) {
			std::string text = positional.empty() ? "" : positional[0];
			auto audio = encode_message(cfg, text);
			write_wav(wav, audio);
			std::printf("wrote %s (%.2f s)\n", wav.c_str(), audio.size() / double(RATE));
			return 0;
		}

		if (command == "listen" && !wav.empty()) {
			ma_decoder_config dc = ma_decoder_config_init(ma_format_s16, 1, RATE);
			ma_decoder d;
			if (ma_decoder_init_file(wav.c_str(), &dc, &d) != MA_SUCCESS)
				throw std::runtime_error("cannot read " + wav);
			Receiver rx(cfg);
			std::vector<int16_t> buf(RATE);
			ma_uint64 got = 0;
			int messages = 0;
			while (ma_decoder_read_pcm_frames(&d, buf.data(), buf.size(), &got) == MA_SUCCESS && got)
				messages += rx.feed(buf.data(), (size_t) got);
			std::vector<int16_t> pad(RATE, 0); // flush the decoder's pipeline
			messages += rx.feed(pad.data(), pad.size());
			ma_decoder_uninit(&d);
			wait_hooks(10000);
			return messages ? 0 : 1;
		}

		if (command != "chat" && command != "listen" && command != "send" && command != "ptt-test") {
			usage();
			return 2;
		}

		Station st(cfg);
		g_ptt = &st.get_ptt();

		if (command == "ptt-test") {
			st.ptt_test(positional.empty() ? 1000 : std::atoi(positional[0].c_str()));
			return 0;
		}
		if (command == "send") {
			st.transmit(positional.empty() ? "" : positional[0]);
			return 0;
		}

		if (!g_json) std::printf("rattle %s  callsign %s  ptt %s%s%s\n", VERSION, cfg.get("callsign").c_str(),
			cfg.get("ptt").c_str(), cfg.get("port").empty() ? "" : " on ", cfg.get("port").c_str());
		st.start_capture();

		std::mutex q_mutex;
		std::deque<std::string> queue;
		if (command == "chat") {
			if (!g_json) std::printf("Type a message and press Enter to send (max 170 bytes). Empty line = ping. Ctrl+C quits.\n");
			std::thread([&] {
				std::string line;
				while (!g_quit && std::getline(std::cin, line)) {
					if (!line.empty() && line.back() == '\r') line.pop_back();
					std::lock_guard<std::mutex> lk(q_mutex);
					queue.push_back(line);
				}
				if (g_json) g_quit = true; // the app that started us is gone
			}).detach();
		}

		while (!g_quit) {
			std::string msg;
			bool have = false;
			{
				std::lock_guard<std::mutex> lk(q_mutex);
				if (!queue.empty()) {
					msg = queue.front();
					queue.pop_front();
					have = true;
				}
			}
			if (have) {
				try {
					st.transmit(msg);
				} catch (const std::exception &e) {
					if (g_json) emit({{"event", "send_error"}, {"message", e.what()}});
					else say(std::string("error: ") + e.what());
				}
			}
			st.poll();
			double limit = cfg.getf("no_audio_s");
			if (limit > 0 && st.audio_silence_s() > limit)
				throw std::runtime_error("no audio from the input device (is the radio on and plugged in?)");
			sleep_ms(20);
		}
		st.get_ptt().off();
		g_ptt = nullptr;
		if (!g_json) std::printf("\nbye\n");
		std::fflush(stdout);
		std::_Exit(0); // the stdin reader thread may still be blocked in getline
	} catch (const std::exception &e) {
		if (g_ptt) g_ptt->off();
		if (g_json) emit({{"event", "error"}, {"message", e.what()}});
		else std::fprintf(stderr, "error: %s\n", e.what());
		std::fflush(stdout);
		std::_Exit(1); // don't wait on a stuck audio device or the stdin thread
	}
}
