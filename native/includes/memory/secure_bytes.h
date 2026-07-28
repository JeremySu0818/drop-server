#ifndef DROP_SERVER_MEMORY_SECURE_BYTES_H_
#define DROP_SERVER_MEMORY_SECURE_BYTES_H_

#include <cstddef>
#include <cstdint>
#include <memory>

namespace drop_server {

void SecureZero(void *pointer, std::size_t size) noexcept;
void ReleaseUnusedMemory() noexcept;

class SecureBytes {
public:
  SecureBytes() noexcept = default;
  explicit SecureBytes(std::size_t size);
  ~SecureBytes();

  SecureBytes(const SecureBytes &) = delete;
  SecureBytes &operator=(const SecureBytes &) = delete;
  SecureBytes(SecureBytes &&other) noexcept;
  SecureBytes &operator=(SecureBytes &&other) noexcept;

  std::uint8_t *data() noexcept;
  const std::uint8_t *data() const noexcept;
  std::size_t size() const noexcept;

  void CopyAt(std::size_t offset, const std::uint8_t *source,
              std::size_t length);
  void Reset() noexcept;

private:
  std::unique_ptr<std::uint8_t[]> data_;
  std::size_t size_ = 0;
};

} // namespace drop_server

#endif