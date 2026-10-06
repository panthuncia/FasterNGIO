#include "Platform/WholeFile.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace FasterNGIO::Platform
{
	void WriteWholeFile(const std::filesystem::path& a_finalPath, std::span<const std::uint8_t> a_bytes)
	{
		// Written beside the target and renamed over it, so a crash, kill or full disk never leaves a
		// truncated .cgid that a later run (or the game) takes for a finished cache.
		auto a_path = a_finalPath;
		a_path += ".tmp";
#if defined(_WIN32)
		const HANDLE file = CreateFileW(a_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			throw std::runtime_error("cannot create " + a_path.string() + " (error " + std::to_string(GetLastError()) + ")");
		}
		std::size_t offset = 0;
		bool ok = true;
		while (ok && offset < a_bytes.size()) {
			const auto chunk = static_cast<DWORD>((std::min<std::size_t>)(a_bytes.size() - offset, 1u << 30));
			DWORD written = 0;
			ok = WriteFile(file, a_bytes.data() + offset, chunk, &written, nullptr) != 0 && written != 0;
			offset += written;
		}
		const auto error = ok ? 0 : GetLastError();
		CloseHandle(file);
		if (!ok) {
			DeleteFileW(a_path.c_str());
			throw std::runtime_error("cannot write " + a_path.string() + " (error " + std::to_string(error) + ")");
		}
		if (!MoveFileExW(a_path.c_str(), a_finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
			const auto moveError = GetLastError();
			DeleteFileW(a_path.c_str());
			throw std::runtime_error("cannot replace " + a_finalPath.string() + " (error " + std::to_string(moveError) + ")");
		}
#else
		const int file = ::open(a_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (file < 0) {
			throw std::runtime_error("cannot create " + a_path.string() + ": " + std::strerror(errno));
		}
		std::size_t offset = 0;
		while (offset < a_bytes.size()) {
			const auto written = ::write(file, a_bytes.data() + offset, a_bytes.size() - offset);
			if (written < 0 && errno == EINTR) {
				continue;
			}
			if (written <= 0) {
				const int error = errno;
				::close(file);
				throw std::runtime_error("cannot write " + a_path.string() + ": " + std::strerror(error));
			}
			offset += static_cast<std::size_t>(written);
		}
		if (::close(file) != 0) {
			::unlink(a_path.c_str());
			throw std::runtime_error("cannot write " + a_path.string() + ": " + std::strerror(errno));
		}
		if (::rename(a_path.c_str(), a_finalPath.c_str()) != 0) {
			const int error = errno;
			::unlink(a_path.c_str());
			throw std::runtime_error("cannot replace " + a_finalPath.string() + ": " + std::strerror(error));
		}
#endif
	}

	std::optional<std::vector<std::uint8_t>> ReadWholeFile(const std::filesystem::path& a_path)
	{
		std::ifstream file(a_path, std::ios::binary | std::ios::ate);
		if (!file) {
			return std::nullopt;
		}
		const auto size = file.tellg();
		if (size < 0) {
			return std::nullopt;
		}
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
		file.seekg(0);
		if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
			return std::nullopt;
		}
		return bytes;
	}
}
