#include "memory/secure_bytes.h"

#include <cstring>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <strings.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#endif

namespace drop_server {

void SecureZero(void *pointer, std::size_t size) noexcept {
  if (pointer == nullptr || size == 0) {
    return;
  }

#if defined(_WIN32)
  SecureZeroMemory(pointer, size);
#elif defined(__GLIBC__) || defined(__linux__) || defined(__APPLE__) ||        \
    defined(__FreeBSD__) || defined(__OpenBSD__)
  explicit_bzero(pointer, size);
#else
  volatile auto *bytes = static_cast<volatile unsigned char *>(pointer);
  while (size-- > 0) {
    *bytes++ = 0;
  }
#endif
}

void ReleaseUnusedMemory() noexcept {
#if defined(__GLIBC__)
  malloc_trim(0);
#endif
}

SecureBytes::SecureBytes(std::size_t size)
    : data_(size == 0 ? nullptr : std::make_unique<std::uint8_t[]>(size)),
      size_(size) {}

SecureBytes::~SecureBytes() { Reset(); }

SecureBytes::SecureBytes(SecureBytes &&other) noexcept
    : data_(std::move(other.data_)), size_(std::exchange(other.size_, 0)) {}

SecureBytes &SecureBytes::operator=(SecureBytes &&other) noexcept {
  if (this != &other) {
    Reset();
    data_ = std::move(other.data_);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

std::uint8_t *SecureBytes::data() noexcept { return data_.get(); }

const std::uint8_t *SecureBytes::data() const noexcept { return data_.get(); }

std::size_t SecureBytes::size() const noexcept { return size_; }

void SecureBytes::CopyAt(std::size_t offset, const std::uint8_t *source,
                         std::size_t length) {
  if (offset > size_ || length > size_ - offset) {
    throw std::out_of_range("Secure byte write exceeds allocation.");
  }
  if (length > 0) {
    std::memcpy(data_.get() + offset, source, length);
  }
}

void SecureBytes::Reset() noexcept {
  if (data_ != nullptr) {
    SecureZero(data_.get(), size_);
    data_.reset();
    size_ = 0;
  }
}

} // namespace drop_server
