/*
** richpresence.cpp
**
** [Karto] Discord Rich Presence over Discord's local IPC pipe. One thread owns the connection, so a missing, closed
** or hung Discord never stalls the game or tic threads.
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "version.h"

namespace
{
	using Clock = std::chrono::steady_clock;

	enum
	{
		OpHandshake = 0,
		OpFrame = 1,
		OpClose = 2,
		OpPing = 3,
		OpPong = 4,
	};

	struct FPresenceState
	{
		std::mutex Lock;
		std::condition_variable Wake;
		bool Started = false;
		bool Dirty = false;
		bool Enabled = false;
		std::string Details;
		std::string State;
		std::string LargeText;
		std::string AppId;
	};

	FPresenceState& GetState()
	{
		// [Karto] Never destroyed, so the detached thread cannot touch freed state while the process exits.
		static FPresenceState* state = new FPresenceState;
		return *state;
	}

	class FPipe
	{
	public:
		~FPipe()
		{
			Close();
		}

#ifdef _WIN32
		bool Open()
		{
			for (int i = 0; i < 10; i++)
			{
				char name[32];
				snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
				Handle = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
				if (Handle != INVALID_HANDLE_VALUE) return true;
			}
			return false;
		}

		void Close()
		{
			if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle);
			Handle = INVALID_HANDLE_VALUE;
		}

		bool IsOpen() const
		{
			return Handle != INVALID_HANDLE_VALUE;
		}

		bool Write(const void* data, uint32_t size)
		{
			auto p = (const char*)data;
			while (size > 0)
			{
				DWORD written = 0;
				if (!WriteFile(Handle, p, size, &written, nullptr) || written == 0) return false;
				p += written;
				size -= written;
			}
			return true;
		}

		int Available()
		{
			DWORD avail = 0;
			if (!PeekNamedPipe(Handle, nullptr, 0, nullptr, &avail, nullptr)) return -1;
			return (int)std::min<DWORD>(avail, 0x7fffffff);
		}

		bool Read(void* data, uint32_t size)
		{
			auto p = (char*)data;
			while (size > 0)
			{
				DWORD got = 0;
				if (!ReadFile(Handle, p, size, &got, nullptr) || got == 0) return false;
				p += got;
				size -= got;
			}
			return true;
		}

	private:
		HANDLE Handle = INVALID_HANDLE_VALUE;
#else
		bool Open()
		{
			const char* envs[] = { "XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP" };
			const char* subdirs[] = { "", "app/com.discordapp.Discord/", "snap.discord/" };
			const char* base = "/tmp";
			for (auto env : envs)
			{
				auto value = getenv(env);
				if (value && *value)
				{
					base = value;
					break;
				}
			}
			for (auto subdir : subdirs)
			{
				for (int i = 0; i < 10; i++)
				{
					sockaddr_un addr = {};
					addr.sun_family = AF_UNIX;
					snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%sdiscord-ipc-%d", base, subdir, i);
					Socket = socket(AF_UNIX, SOCK_STREAM, 0);
					if (Socket < 0) return false;
#ifdef SO_NOSIGPIPE
					int one = 1;
					setsockopt(Socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
					if (connect(Socket, (sockaddr*)&addr, sizeof(addr)) == 0) return true;
					Close();
				}
			}
			return false;
		}

		void Close()
		{
			if (Socket >= 0) close(Socket);
			Socket = -1;
		}

		bool IsOpen() const
		{
			return Socket >= 0;
		}

		bool Write(const void* data, uint32_t size)
		{
#ifdef MSG_NOSIGNAL
			const int flags = MSG_NOSIGNAL;
#else
			const int flags = 0;
#endif
			auto p = (const char*)data;
			while (size > 0)
			{
				auto sent = send(Socket, p, size, flags);
				if (sent < 0 && errno == EINTR) continue;
				if (sent <= 0) return false;
				p += sent;
				size -= (uint32_t)sent;
			}
			return true;
		}

		int Available()
		{
			pollfd pfd = { Socket, POLLIN, 0 };
			if (poll(&pfd, 1, 0) < 0) return errno == EINTR ? 0 : -1;
			if (pfd.revents & (POLLERR | POLLNVAL)) return -1;
			if (!(pfd.revents & (POLLIN | POLLHUP))) return 0;
			int avail = 0;
			// [Karto] Readable with nothing to read means the peer closed the socket.
			if (ioctl(Socket, FIONREAD, &avail) < 0 || avail <= 0) return -1;
			return avail;
		}

		bool Read(void* data, uint32_t size)
		{
			auto p = (char*)data;
			while (size > 0)
			{
				auto got = recv(Socket, p, size, 0);
				if (got < 0 && errno == EINTR) continue;
				if (got <= 0) return false;
				p += got;
				size -= (uint32_t)got;
			}
			return true;
		}

	private:
		int Socket = -1;
#endif
	};

	bool SendFrame(FPipe& pipe, uint32_t op, const std::string& json)
	{
		uint32_t header[2] = { op, (uint32_t)json.size() };
		std::string frame((const char*)header, sizeof(header));
		frame += json;
		return pipe.Write(frame.data(), (uint32_t)frame.size());
	}

	int WaitForBytes(FPipe& pipe, int size, Clock::time_point deadline)
	{
		for (;;)
		{
			int avail = pipe.Available();
			if (avail < 0) return -1;
			if (avail >= size) return 1;
			if (Clock::now() >= deadline) return 0;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}

	int ReadFrame(FPipe& pipe, uint32_t& op, std::string& payload, Clock::time_point deadline)
	{
		uint32_t header[2];
		int ready = WaitForBytes(pipe, sizeof(header), deadline);
		if (ready <= 0) return ready;
		if (!pipe.Read(header, sizeof(header)) || header[1] > 65536) return -1;
		op = header[0];
		payload.resize(header[1]);
		if (header[1] == 0) return 1;
		// [Karto] A half-read frame would desync the stream, so a body that never arrives drops the connection.
		if (WaitForBytes(pipe, header[1], Clock::now() + std::chrono::seconds(2)) <= 0) return -1;
		return pipe.Read(&payload[0], header[1]) ? 1 : -1;
	}

	bool HandleFrame(FPipe& pipe, uint32_t op, const std::string& payload)
	{
		if (op == OpPing) return SendFrame(pipe, OpPong, payload);
		return op != OpClose;
	}

	bool Drain(FPipe& pipe)
	{
		uint32_t op;
		std::string payload;
		for (;;)
		{
			int got = ReadFrame(pipe, op, payload, Clock::now());
			if (got < 0) return false;
			if (got == 0) return true;
			if (!HandleFrame(pipe, op, payload)) return false;
		}
	}

	void AppendString(std::string& out, const std::string& text)
	{
		out += '"';
		for (unsigned char c : text)
		{
			if (c == '"' || c == '\\')
			{
				out += '\\';
				out += c;
			}
			else if (c < 0x20)
			{
				char escape[8];
				snprintf(escape, sizeof(escape), "\\u%04x", c);
				out += escape;
			}
			else
			{
				out += c;
			}
		}
		out += '"';
	}

	bool Connect(FPipe& pipe, const std::string& appId)
	{
		if (!pipe.Open()) return false;
		std::string hello = "{\"v\":1,\"client_id\":";
		AppendString(hello, appId);
		hello += '}';
		if (SendFrame(pipe, OpHandshake, hello))
		{
			auto deadline = Clock::now() + std::chrono::seconds(30);
			uint32_t op;
			std::string payload;
			while (ReadFrame(pipe, op, payload, deadline) > 0 && HandleFrame(pipe, op, payload))
			{
				if (op == OpFrame && payload.find("\"READY\"") != std::string::npos) return true;
			}
		}
		pipe.Close();
		return false;
	}

	// [Karto] Discord rejects the whole activity for a text field outside 2-128 characters.
	void AppendField(std::string& json, const char* key, std::string text)
	{
		if (text.size() < 2) return;
		if (text.size() > 128)
		{
			size_t cut = 128;
			while (cut > 0 && (text[cut] & 0xc0) == 0x80) cut--;
			text.resize(cut);
		}
		json += ",\"";
		json += key;
		json += "\":";
		AppendString(json, text);
	}

	bool SendActivity(FPipe& pipe, bool enabled, const std::string& details, const std::string& state, const std::string& largeText, int64_t startTime, unsigned nonce)
	{
#ifdef _WIN32
		auto pid = (unsigned long)GetCurrentProcessId();
#else
		auto pid = (unsigned long)getpid();
#endif
		std::string json = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid);
		if (enabled)
		{
			json += ",\"activity\":{\"timestamps\":{\"start\":" + std::to_string(startTime) + "}";
			AppendField(json, "details", details);
			AppendField(json, "state", state);
			json += ",\"assets\":{\"large_image\":\"game-image\"";
			AppendField(json, "large_text", largeText);
			json += "}}";
		}
		json += "},\"nonce\":\"" + std::to_string(nonce) + "\"}";
		return SendFrame(pipe, OpFrame, json);
	}

	void PresenceMain()
	{
		auto& state = GetState();
		FPipe pipe;
		std::string connectedAppId;
		auto nextConnect = Clock::now();
		auto nextSend = Clock::now();
		int retrySeconds = 5;
		int64_t startTime = 0;
		unsigned nonce = 0;
		bool pending = false;

		std::unique_lock<std::mutex> guard(state.Lock);
		for (;;)
		{
			pending |= state.Dirty;
			state.Dirty = false;
			bool enabled = state.Enabled;
			std::string details = state.Details;
			std::string status = state.State;
			std::string largeText = state.LargeText;
			std::string appId = state.AppId;
			guard.unlock();

			auto now = Clock::now();
			if (pipe.IsOpen() && (!enabled || appId != connectedAppId))
			{
				// [Karto] The activity can outlive a closed connection, so clear it first.
				SendActivity(pipe, false, {}, {}, {}, 0, ++nonce);
				pipe.Close();
				nextConnect = now;
			}
			if (!pipe.IsOpen() && enabled && now >= nextConnect)
			{
				if (Connect(pipe, appId))
				{
					connectedAppId = appId;
					retrySeconds = 5;
					pending = true;
				}
				else
				{
					nextConnect = Clock::now() + std::chrono::seconds(retrySeconds);
					retrySeconds = (std::min)(retrySeconds * 2, 60);
				}
				now = Clock::now();
			}
			if (pipe.IsOpen())
			{
				bool alive = Drain(pipe);
				// [Karto] Discord allows about five activity updates per 20 seconds; quick level changes coalesce.
				if (alive && pending && now >= nextSend)
				{
					if (startTime == 0) startTime = (int64_t)time(nullptr);
					alive = SendActivity(pipe, true, details, status, largeText, startTime, ++nonce);
					pending = false;
					nextSend = now + std::chrono::seconds(4);
				}
				if (!alive)
				{
					pipe.Close();
					pending = true;
					nextConnect = now + std::chrono::seconds(retrySeconds);
				}
			}
			if (!pipe.IsOpen() && !enabled) pending = false;

			guard.lock();
			if (state.Dirty) continue;
			if (pipe.IsOpen())
			{
				auto wake = pending ? nextSend : Clock::now() + std::chrono::seconds(1);
				state.Wake.wait_until(guard, wake, [&] { return state.Dirty; });
			}
			else if (enabled)
			{
				state.Wake.wait_until(guard, nextConnect, [&] { return state.Dirty; });
			}
			else
			{
				state.Wake.wait(guard, [&] { return state.Dirty; });
			}
		}
	}
}

void I_UpdateDiscordPresence(bool SendPresence, const char* details, const char* status, const char* largetext, const char* appid)
{
	auto& state = GetState();
	std::lock_guard<std::mutex> guard(state.Lock);
	if (!state.Started && !SendPresence) return;
	if (!details) details = "";
	if (!status) status = "";
	if (!largetext) largetext = "";
	if (!appid || !*appid) appid = DEFAULT_DISCORD_APP_ID;
	if (state.Started && state.Enabled == SendPresence && state.Details == details && state.State == status &&
		state.LargeText == largetext && state.AppId == appid)
	{
		return;
	}
	if (!state.Started)
	{
		try
		{
			// [Karto] Detached: a joinable thread left at exit() calls std::terminate.
			std::thread(PresenceMain).detach();
		}
		catch (...)
		{
			return;
		}
		state.Started = true;
	}
	state.Enabled = SendPresence;
	state.Details = details;
	state.State = status;
	state.LargeText = largetext;
	state.AppId = appid;
	state.Dirty = true;
	state.Wake.notify_one();
}
