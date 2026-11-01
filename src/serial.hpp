// Minimal serial port for PTT: CAT strings and RTS/DTR lines.
// Windows (COMx) and POSIX (/dev/cu.*, /dev/ttyUSB*, /dev/ttyACM*).
#pragma once

#include <string>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cerrno>
#endif

class SerialPort {
public:
	SerialPort() = default;
	SerialPort(const SerialPort &) = delete;
	SerialPort &operator=(const SerialPort &) = delete;
	~SerialPort() { close(); }

	// Opens the port with RTS and DTR low so opening never keys the radio.
	void open(const std::string &name, int baud) {
		close();
#ifdef _WIN32
		std::string path = name.rfind("\\\\.\\", 0) == 0 ? name : "\\\\.\\" + name;
		h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			throw std::runtime_error("cannot open serial port " + name + " (is another program like WSJT-X using it?)");
		DCB dcb;
		std::memset(&dcb, 0, sizeof dcb);
		dcb.DCBlength = sizeof dcb;
		GetCommState(h, &dcb);
		dcb.BaudRate = baud;
		dcb.ByteSize = 8;
		dcb.Parity = NOPARITY;
		dcb.StopBits = ONESTOPBIT;
		dcb.fBinary = TRUE;
		dcb.fOutxCtsFlow = FALSE;
		dcb.fOutxDsrFlow = FALSE;
		dcb.fDtrControl = DTR_CONTROL_DISABLE;
		dcb.fRtsControl = RTS_CONTROL_DISABLE;
		dcb.fOutX = FALSE;
		dcb.fInX = FALSE;
		if (!SetCommState(h, &dcb)) {
			close();
			throw std::runtime_error("cannot configure serial port " + name);
		}
		COMMTIMEOUTS to = {};
		to.ReadIntervalTimeout = MAXDWORD;
		to.WriteTotalTimeoutConstant = 500;
		SetCommTimeouts(h, &to);
		EscapeCommFunction(h, CLRRTS);
		EscapeCommFunction(h, CLRDTR);
#else
		fd = ::open(name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
		if (fd < 0)
			throw std::runtime_error("cannot open serial port " + name + ": " + std::strerror(errno));
		set_line(TIOCM_RTS, false);
		set_line(TIOCM_DTR, false);
		termios tio;
		if (tcgetattr(fd, &tio) == 0) {
			cfmakeraw(&tio);
			tio.c_cflag |= CLOCAL | CREAD;
			tio.c_cflag &= ~(HUPCL | CRTSCTS | CSTOPB | PARENB);
			speed_t sp = to_speed(baud);
			cfsetispeed(&tio, sp);
			cfsetospeed(&tio, sp);
			tcsetattr(fd, TCSANOW, &tio);
		}
		int flags = fcntl(fd, F_GETFL);
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
		set_line(TIOCM_RTS, false);
		set_line(TIOCM_DTR, false);
#endif
	}

	bool is_open() const {
#ifdef _WIN32
		return h != INVALID_HANDLE_VALUE;
#else
		return fd >= 0;
#endif
	}

	void write(const std::string &s) {
		if (!is_open() || s.empty())
			return;
#ifdef _WIN32
		DWORD n = 0;
		WriteFile(h, s.data(), (DWORD) s.size(), &n, nullptr);
		FlushFileBuffers(h);
#else
		ssize_t r = ::write(fd, s.data(), s.size());
		(void) r;
		tcdrain(fd);
#endif
	}

	void rts(bool on) {
		if (!is_open())
			return;
#ifdef _WIN32
		EscapeCommFunction(h, on ? SETRTS : CLRRTS);
#else
		set_line(TIOCM_RTS, on);
#endif
	}

	void dtr(bool on) {
		if (!is_open())
			return;
#ifdef _WIN32
		EscapeCommFunction(h, on ? SETDTR : CLRDTR);
#else
		set_line(TIOCM_DTR, on);
#endif
	}

	void close() {
#ifdef _WIN32
		if (h != INVALID_HANDLE_VALUE) {
			CloseHandle(h);
			h = INVALID_HANDLE_VALUE;
		}
#else
		if (fd >= 0) {
			::close(fd);
			fd = -1;
		}
#endif
	}

private:
#ifdef _WIN32
	HANDLE h = INVALID_HANDLE_VALUE;
#else
	int fd = -1;

	void set_line(int bit, bool on) {
		ioctl(fd, on ? TIOCMBIS : TIOCMBIC, &bit);
	}

	static speed_t to_speed(int baud) {
		switch (baud) {
			case 4800: return B4800;
			case 9600: return B9600;
			case 19200: return B19200;
			case 38400: return B38400;
			case 57600: return B57600;
			case 115200: return B115200;
#ifdef B230400
			case 230400: return B230400;
#endif
			default: throw std::runtime_error("unsupported baud rate " + std::to_string(baud));
		}
	}
#endif
};
