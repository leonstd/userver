#include <fs/blocking/c_file.hpp>

#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <memory>

#include <fs/blocking/file_descriptor.hpp>
#include <logging/log.hpp>
#include <utils/assert.hpp>
#include <utils/check_syscall.hpp>

namespace fs::blocking {

namespace {

struct FileDeleter {
  void operator()(std::FILE* file) noexcept {
    // unique_ptr guarantees that file != nullptr
    if (std::fclose(file) == -1) {
      const int code = errno;
      LOG_ERROR() << "Error while closing a cache dump file: "
                  << std::error_code(code, std::generic_category());
    }
  }
};

// Produce the mode string for ::fdopen
const char* ToMode(OpenMode flags) noexcept {
  // Expect that the necessary flag checks have already been performed
  // by FileDescriptor. Always append 'b' flag. Always append 'm' flag to allow
  // the platform to use mmap under the hood, if possible.
  if (flags & OpenFlag::kWrite) {
    return flags & OpenFlag::kRead ? "w+bm" : "wbm";
  } else {
    return "rbm";
  }
}

};  // namespace

struct CFile::Impl {
  std::unique_ptr<std::FILE, FileDeleter> handle;

  int GetFileDescriptor() const {
    return utils::CheckSyscall(::fileno(handle.get()), "calling ::fileno");
  }
};

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
CFile::CFile() = default;

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
CFile::CFile(CFile&&) noexcept = default;

CFile& CFile::operator=(CFile&&) noexcept = default;

CFile::~CFile() = default;

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
CFile::CFile(const std::string& path, OpenMode flags, int perms) {
  auto fd = FileDescriptor::OpenFile(path, flags, perms);
  impl_->handle.reset(utils::CheckSyscallNotEquals(
      ::fdopen(fd.GetNativeFd(), ToMode(flags)), nullptr, "calling ::fdopen"));
  std::move(fd).Release();
}

bool CFile::IsOpen() const { return static_cast<bool>(impl_->handle); }

void CFile::Close() && {
  UASSERT(IsOpen());
  utils::CheckSyscall(std::fclose(impl_->handle.release()), "calling fclose");
}

std::size_t CFile::Read(char* buffer, std::size_t size) {
  UASSERT(IsOpen());

  const auto bytes_read = std::fread(buffer, 1, size, impl_->handle.get());
  if (bytes_read != size && !std::feof(impl_->handle.get())) {
    throw std::system_error(std::ferror(impl_->handle.get()),
                            std::generic_category(), "calling fread");
  }

  return bytes_read;
}

void CFile::Write(std::string_view data) {
  UASSERT(IsOpen());

  if (std::fwrite(data.data(), 1, data.size(), impl_->handle.get()) !=
      data.size()) {
    throw std::system_error(std::ferror(impl_->handle.get()),
                            std::generic_category(), "calling fwrite");
  }
}

void CFile::Flush() {
  UASSERT(IsOpen());
  utils::CheckSyscall(std::fflush(impl_->handle.get()), "calling fflush");
  utils::CheckSyscall(::fsync(impl_->GetFileDescriptor()), "calling ::fsync");
}

std::uint64_t CFile::GetPosition() const {
  UASSERT(IsOpen());
  const auto position =
      utils::CheckSyscall(std::ftell(impl_->handle.get()), "calling ftell");
  static_assert(sizeof(position) >= 8, "large file support is required");
  return position;
}

std::uint64_t CFile::GetSize() const {
  UASSERT(IsOpen());

  using stats_t = struct ::stat;
  stats_t stats{};
  static_assert(sizeof(stats.st_size) >= 8, "large file support is required");

  utils::CheckSyscall(::fstat(impl_->GetFileDescriptor(), &stats),
                      "calling ::fstat");
  return stats.st_size;
}

}  // namespace fs::blocking
