#pragma once

#include <algorithm>
#include <cstdint>

namespace dxvk {

  class VideoRbspReader {

  public:

    VideoRbspReader(const uint8_t* begin, const uint8_t* end)
    : m_current(begin), m_end(end) { }

    bool valid() const {
      return !m_failed;
    }

    uint32_t readBits(uint32_t count) {
      uint32_t value = 0;

      for (; count && !m_failed; count--) {
        if (!m_bitsLeft && !readByte())
          break;

        value = (value << 1) | ((m_byte >> --m_bitsLeft) & 1u);
      }

      return value;
    }

    uint32_t readUe() {
      uint32_t leadingZeroes = 0;

      while (valid() && !readBits(1)) {
        if (++leadingZeroes >= 32) {
          m_failed = true;
          return 0;
        }
      }

      return ((1u << leadingZeroes) - 1u) + readBits(leadingZeroes);
    }

  private:

    const uint8_t* m_current;
    const uint8_t* m_end;
    uint8_t m_byte = 0;
    uint32_t m_bitsLeft = 0;
    uint32_t m_zeroBytes = 0;
    bool m_failed = false;

    bool readByte() {
      if (m_current == m_end) {
        m_failed = true;
        return false;
      }

      if (m_zeroBytes == 2 && *m_current == 3) {
        m_current++;
        m_zeroBytes = 0;

        if (m_current == m_end || *m_current > 3) {
          m_failed = true;
          return false;
        }
      }

      m_byte = *m_current++;
      m_bitsLeft = 8;
      m_zeroBytes = m_byte ? 0 : std::min(m_zeroBytes + 1, 2u);
      return true;
    }

  };

}
