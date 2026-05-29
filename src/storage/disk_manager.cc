#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "storage/disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/fcntl.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace buffer_manager {
namespace {

constexpr std::array<char, 8> kMagic = {'B', 'M', 'D', 'I', 'S', 'K', '0', '1'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr mode_t kFileCreateMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

struct DiskFileHeader final {
  std::array<char, 8> magic{};
  std::uint32_t version = 0;
  std::uint32_t page_size = 0;
  std::uint64_t max_page_count = 0;
  std::uint64_t header_size = 0;
};

static_assert(sizeof(DiskFileHeader) <= kPageSize);

[[nodiscard]] std::runtime_error SystemError(std::string_view operation) {
  const int error_number = errno;
  return std::runtime_error(std::string(operation) + ": " +
                            std::strerror(error_number));
}

[[nodiscard]] std::uint64_t CheckedAdd(std::uint64_t lhs, std::uint64_t rhs) {
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
    throw std::overflow_error("uint64 addition overflow");
  }

  return lhs + rhs;
}

[[nodiscard]] std::uint64_t CheckedMul(std::uint64_t lhs, std::uint64_t rhs) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    throw std::overflow_error("uint64 multiplication overflow");
  }

  return lhs * rhs;
}

[[nodiscard]] off_t CheckedOffset(std::uint64_t offset) {
  const auto max_offset =
      static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());

  if (offset > max_offset) {
    throw std::overflow_error("file offset does not fit into off_t");
  }

  return static_cast<off_t>(offset);
}

[[nodiscard]] std::uint64_t FileSize(int fd) {
  struct stat stat_buffer{};

  if (::fstat(fd, &stat_buffer) != 0) {
    throw SystemError("fstat failed");
  }

  if (stat_buffer.st_size < 0) {
    throw std::runtime_error("file size is negative");
  }

  return static_cast<std::uint64_t>(stat_buffer.st_size);
}

void ValidatePageAlignment(const Page& page) {
  const auto address = reinterpret_cast<std::uintptr_t>(page.data.data());

  if (address % kPageSize != 0) {
    throw std::logic_error("page buffer is not page-size aligned");
  }
}

void ReadAtMost(int fd, void* buffer, std::size_t size, std::uint64_t offset) {
  auto* out = static_cast<std::byte*>(buffer);
  std::size_t total_read = 0;

  while (total_read < size) {
    const std::uint64_t current_offset = CheckedAdd(offset, total_read);

    const ssize_t bytes_read = ::pread(fd, out + total_read, size - total_read,
                                       CheckedOffset(current_offset));

    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }

      throw SystemError("pread failed");
    }

    if (bytes_read == 0) {
      return;
    }

    total_read += static_cast<std::size_t>(bytes_read);
  }
}

void WriteFully(int fd, const void* buffer, std::size_t size,
                std::uint64_t offset) {
  const auto* in = static_cast<const std::byte*>(buffer);
  std::size_t total_written = 0;

  while (total_written < size) {
    const std::uint64_t current_offset = CheckedAdd(offset, total_written);

    const ssize_t bytes_written =
        ::pwrite(fd, in + total_written, size - total_written,
                 CheckedOffset(current_offset));

    if (bytes_written < 0) {
      if (errno == EINTR) {
        continue;
      }

      throw SystemError("pwrite failed");
    }

    if (bytes_written == 0) {
      throw std::runtime_error("pwrite wrote zero bytes");
    }

    total_written += static_cast<std::size_t>(bytes_written);
  }
}

[[nodiscard]] DiskFileHeader MakeHeader(const DiskManagerOptions& options) {
  return {
      .magic = kMagic,
      .version = kFormatVersion,
      .page_size = static_cast<std::uint32_t>(kPageSize),
      .max_page_count = options.max_page_count,
      .header_size = options.header_size,
  };
}

}  // namespace

DiskManager::DiskManager(DiskManagerOptions options)
    : options_(std::move(options)) {
  ValidateOptions();
  OpenFile();
  InitializeFile();
}

DiskManager::~DiskManager() {
  std::scoped_lock lock(file_mutex_);

  if (fd_ >= 0) {
    static_cast<void>(::close(fd_));
    fd_ = -1;
  }
}

void DiskManager::ReadPage(PageId page_id, Page* page) const {
  if (page == nullptr) {
    throw std::invalid_argument("page must not be null");
  }

  ValidatePageId(page_id);
  ValidatePageAlignment(*page);

  std::ranges::fill(page->data, std::byte{0});

  ReadAtMost(fd_, page->data.data(), kPageSize, OffsetForPage(page_id));
}

void DiskManager::WritePage(  // NOLINT(readability-make-member-function-const)
    PageId page_id, const Page& page) {
  ValidatePageId(page_id);
  ValidatePageAlignment(page);

  WriteFully(fd_, page.data.data(), kPageSize, OffsetForPage(page_id));
}

void DiskManager::Sync() const {
  std::scoped_lock lock(file_mutex_);

  if (fd_ < 0) {
    throw std::logic_error("disk file is not open");
  }

#if defined(__APPLE__)
  if (::fsync(fd_) != 0) {
    throw SystemError("fsync failed");
  }
#elif defined(__linux__)
  if (::fdatasync(fd_) != 0) {
    throw SystemError("fdatasync failed");
  }
#else
#error "DiskManager supports only Linux and macOS"
#endif
}

std::uint64_t DiskManager::OffsetForPage(PageId page_id) const {
  ValidatePageId(page_id);

  const std::uint64_t page_offset =
      CheckedMul(static_cast<std::uint64_t>(page_id), kPageSize);

  return CheckedAdd(options_.header_size, page_offset);
}

void DiskManager::ValidateOptions() const {
  if (options_.path.empty()) {
    throw std::invalid_argument("disk path must not be empty");
  }

  if (options_.max_page_count == 0) {
    throw std::invalid_argument("max_page_count must be greater than zero");
  }

  if (options_.header_size < sizeof(DiskFileHeader)) {
    throw std::invalid_argument("header_size is too small");
  }

  if (options_.header_size % kPageSize != 0) {
    throw std::invalid_argument("header_size must be a multiple of kPageSize");
  }

  static_cast<void>(ExpectedFileSize());
}

void DiskManager::ValidatePageId(PageId page_id) const {
  if (page_id >= options_.max_page_count) {
    throw std::out_of_range("page id is out of range");
  }
}

void DiskManager::OpenFile() {
  std::scoped_lock lock(file_mutex_);

  int flags = O_RDWR | O_CREAT;

#if defined(__linux__)
  if (options_.io_mode == IoMode::kDirect) {
    flags |= O_DIRECT;
  }
#endif

#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif

  if (options_.truncate_existing) {
    flags |= O_TRUNC;
  }

  fd_ = ::open(options_.path.c_str(), flags, kFileCreateMode);

  if (fd_ < 0) {
    throw SystemError("open failed");
  }

  ConfigureDirectIo();
}

void DiskManager::ConfigureDirectIo() const {
#if defined(__APPLE__)
  if (options_.io_mode == IoMode::kDirect) {
    int value = 1;

    if (::fcntl(fd_, F_NOCACHE, value) != 0) {
      throw SystemError("fcntl(F_NOCACHE) failed");
    }

    value = 0;
    if (::fcntl(fd_, F_RDAHEAD, value) != 0) {
      throw SystemError("fcntl(F_RDAHEAD) failed");
    }
  }
#elif defined(__linux__)
  // Linux direct I/O is configured through O_DIRECT in open().
#else
#error "DiskManager supports only Linux and macOS"
#endif
}

void DiskManager::InitializeFile() const {
  const std::uint64_t file_size = FileSize(fd_);

  if (file_size == 0 || options_.truncate_existing) {
    WriteFreshHeader();
  } else {
    ValidateExistingHeader();
  }

  if (options_.preallocate) {
    PreallocateFile();
  }
}

void DiskManager::WriteFreshHeader() const {
  Page header_page;
  std::ranges::fill(header_page.data, std::byte{0});

  const DiskFileHeader header = MakeHeader(options_);
  std::memcpy(header_page.data.data(), &header, sizeof(header));

  WriteFully(fd_, header_page.data.data(), kPageSize, 0);
}

void DiskManager::ValidateExistingHeader() const {
  Page header_page;
  std::ranges::fill(header_page.data, std::byte{0});

  ReadAtMost(fd_, header_page.data.data(), kPageSize, 0);

  DiskFileHeader header;
  std::memcpy(&header, header_page.data.data(), sizeof(header));

  if (header.magic != kMagic) {
    throw std::runtime_error("invalid disk file magic");
  }

  if (header.version != kFormatVersion) {
    throw std::runtime_error("unsupported disk file format version");
  }

  if (header.page_size != kPageSize) {
    throw std::runtime_error("disk file page size does not match build");
  }

  if (header.max_page_count != options_.max_page_count) {
    throw std::runtime_error("disk file max_page_count does not match options");
  }

  if (header.header_size != options_.header_size) {
    throw std::runtime_error("disk file header_size does not match options");
  }
}

void DiskManager::PreallocateFile() const {
  const std::uint64_t expected_size = ExpectedFileSize();

  if (expected_size >
      static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    throw std::overflow_error("expected file size does not fit into off_t");
  }

  if (::ftruncate(fd_, static_cast<off_t>(expected_size)) != 0) {
    throw SystemError("ftruncate failed");
  }
}

std::uint64_t DiskManager::ExpectedFileSize() const {
  const std::uint64_t page_area_size = CheckedMul(
      static_cast<std::uint64_t>(options_.max_page_count), kPageSize);

  return CheckedAdd(options_.header_size, page_area_size);
}

}  // namespace buffer_manager
