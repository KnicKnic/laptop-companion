#pragma once

#include "CompanionProtocol.h"

#include <string>

namespace MeetingListProtocol {
constexpr size_t kMeetingsPerPage = 4;
constexpr size_t kNameMaxBytes = 64;
constexpr size_t kMessageMaxBytes = 80;
constexpr size_t kPayloadMaxBytes = 9 + kMessageMaxBytes + kMeetingsPerPage * (5 + kNameMaxBytes);
constexpr size_t kFragmentHeaderBytes = 5;

struct Meeting {
  uint16_t startMinutes = 0;
  uint16_t endMinutes = 0;
  std::string subject;
};

struct Page {
  bool valid = false;
  uint16_t revision = 0;
  uint16_t pageIndex = 0;
  uint16_t pageCount = 1;
  uint8_t status = 0;
  uint8_t count = 0;
  std::string message;
  Meeting meetings[kMeetingsPerPage];
};

inline uint16_t read16(const std::string& value, size_t offset) {
  return static_cast<uint8_t>(value[offset]) |
         static_cast<uint16_t>(static_cast<uint8_t>(value[offset + 1])) << 8;
}

inline bool decode(const std::string& value, Page& out) {
  if (value.size() < 9 || value.size() > kPayloadMaxBytes) return false;
  Page next;
  next.revision = read16(value, 0);
  next.pageIndex = read16(value, 2);
  next.pageCount = read16(value, 4);
  next.status = static_cast<uint8_t>(value[6]);
  next.count = static_cast<uint8_t>(value[7]);
  const size_t messageLength = static_cast<uint8_t>(value[8]);
  if (next.revision == 0 || next.pageCount == 0 || next.pageIndex >= next.pageCount ||
      next.status > 2 || next.count > kMeetingsPerPage || messageLength > kMessageMaxBytes ||
      9 + messageLength > value.size()) return false;
  next.message.assign(value, 9, messageLength);
  size_t offset = 9 + messageLength;
  for (uint8_t i = 0; i < next.count; ++i) {
    if (offset + 5 > value.size()) return false;
    Meeting& meeting = next.meetings[i];
    meeting.startMinutes = read16(value, offset);
    meeting.endMinutes = read16(value, offset + 2);
    const size_t nameLength = static_cast<uint8_t>(value[offset + 4]);
    offset += 5;
    if (meeting.startMinutes >= 1440 || meeting.endMinutes >= 1440 ||
        nameLength > kNameMaxBytes || offset + nameLength > value.size()) return false;
    meeting.subject.assign(value, offset, nameLength);
    offset += nameLength;
  }
  if (offset != value.size()) return false;
  next.valid = true;
  out = next;
  return true;
}

enum class FragmentResult { Invalid, Partial, Complete };

// Commit only complete pages; an interrupted transfer must not mix old titles and new join targets.
class Receiver {
 public:
  FragmentResult accept(const std::string& fragment, Page& page) {
    if (fragment.size() <= kFragmentHeaderBytes ||
        static_cast<uint8_t>(fragment[0]) != CompanionProtocol::PROTOCOL_VERSION) return invalid();
    const size_t offset = read16(fragment, 1);
    const size_t total = read16(fragment, 3);
    if (offset == 0) {
      buffer_.clear();
      total_ = total;
    }
    const size_t length = fragment.size() - kFragmentHeaderBytes;
    if (total < 9 || total > kPayloadMaxBytes || total != total_ ||
        offset != buffer_.size() || offset + length > total) return invalid();
    buffer_.append(fragment, kFragmentHeaderBytes, length);
    if (buffer_.size() < total) return FragmentResult::Partial;
    const bool valid = decode(buffer_, page);
    reset();
    return valid ? FragmentResult::Complete : FragmentResult::Invalid;
  }

  void reset() {
    buffer_.clear();
    total_ = 0;
  }

 private:
  FragmentResult invalid() {
    reset();
    return FragmentResult::Invalid;
  }
  std::string buffer_;
  size_t total_ = 0;
};
}  // namespace MeetingListProtocol
