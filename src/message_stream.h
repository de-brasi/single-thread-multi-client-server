#pragma once

#include <cstddef>
#include <string>

// Reassembles a raw TCP byte stream into newline delimited messages.
//
// TCP is a stream, not a sequence of packets: one send() on the client side can
// reach the server split across several read() calls, and several sends can be
// coalesced into a single read(). Treating "whatever one read() returned" as a
// message therefore corrupts both the content and the boundaries of the data.
// This class hides that: raw bytes go in through Append(), whole messages come
// out through NextMessage().
//
// Everything except '\n' is passed through untouched, so message payloads may
// contain arbitrary bytes, including '\0'. A single trailing '\r' is stripped so
// that CRLF clients (telnet, netcat on some systems) work as expected.
//
// The class deliberately performs no I/O, which makes the trickiest part of the
// server unit testable without sockets (see tests/unit_tests.cpp).
class MessageStream {
public:
    static constexpr size_t kDefaultMaxMessageSize = 1u << 20;  // 1 MiB

    explicit MessageStream(size_t max_message_size = kDefaultMaxMessageSize)
        : max_message_size_(max_message_size) {}

    // Adds freshly read bytes to the stream.
    void Append(const char* data, size_t size) {
        DropConsumedBytes();
        buffer_.append(data, size);
    }

    // Extracts the next complete message: everything up to the next '\n'.
    // Returns false when the buffered bytes do not form a whole message yet.
    bool NextMessage(std::string* message) {
        const size_t newline_position = buffer_.find('\n', consumed_);
        if (newline_position == std::string::npos) {
            return false;
        }

        size_t message_end = newline_position;
        if (message_end > consumed_ && buffer_[message_end - 1] == '\r') {
            --message_end;
        }

        message->assign(buffer_, consumed_, message_end - consumed_);
        consumed_ = newline_position + 1;
        return true;
    }

    // A peer may close the connection without terminating its last message.
    // Those bytes are still payload, so they are handed over instead of dropped.
    bool TakePendingTail(std::string* message) {
        if (pending_size() == 0) {
            return false;
        }
        message->assign(buffer_, consumed_, std::string::npos);
        buffer_.clear();
        consumed_ = 0;
        return true;
    }

    // True once an *unterminated* message grows past the configured limit.
    // Without this check a peer that never sends '\n' would make the server
    // buffer without bound. Buffered complete messages are not affected: they
    // are about to be handed out by NextMessage() anyway.
    bool Overflowed() const {
        return pending_size() > max_message_size_ &&
               buffer_.find('\n', consumed_) == std::string::npos;
    }

    // Number of bytes received but not yet handed out as a message.
    size_t pending_size() const { return buffer_.size() - consumed_; }

private:
    // Keeps the buffer from growing forever on a long lived connection: already
    // delivered bytes are released before new ones are appended.
    void DropConsumedBytes() {
        if (consumed_ == 0) {
            return;
        }
        buffer_.erase(0, consumed_);
        consumed_ = 0;
    }

    std::string buffer_;
    size_t consumed_ = 0;
    size_t max_message_size_;
};
