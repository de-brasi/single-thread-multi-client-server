// Unit tests for the message framing layer.
//
// This is the part of the server where the interesting mistakes live (TCP gives
// no message boundaries), and it is pure logic, so it is tested without any
// sockets, processes or timing.

#include "message_stream.h"
#include "test_framework.h"

#include <string>
#include <vector>

namespace {

// Feeds a chunk into the stream and collects every message it produces.
std::vector<std::string> Feed(MessageStream& stream, const std::string& chunk) {
    stream.Append(chunk.data(), chunk.size());

    std::vector<std::string> messages;
    std::string message;
    while (stream.NextMessage(&message)) {
        messages.push_back(message);
    }
    return messages;
}

void OneCompleteMessageIsExtracted() {
    MessageStream stream;
    CHECK_EQ(Feed(stream, "hello\n"), std::vector<std::string>{"hello"});
    CHECK_EQ(stream.pending_size(), size_t{0});
}

void SeveralMessagesInOneChunkAreSplit() {
    MessageStream stream;
    // The classic Nagle/coalescing case: three client sends arrive as one read.
    CHECK_EQ(Feed(stream, "one\ntwo\nthree\n"),
             (std::vector<std::string>{"one", "two", "three"}));
}

void MessageSplitAcrossChunksIsReassembled() {
    MessageStream stream;
    CHECK_EQ(Feed(stream, "hel"), std::vector<std::string>{});
    CHECK_EQ(Feed(stream, "lo wor"), std::vector<std::string>{});
    CHECK_EQ(Feed(stream, "ld\n"), std::vector<std::string>{"hello world"});
}

void IncompleteTailIsKeptForTheNextChunk() {
    MessageStream stream;
    CHECK_EQ(Feed(stream, "first\nsec"), std::vector<std::string>{"first"});
    CHECK_EQ(stream.pending_size(), size_t{3});
    CHECK_EQ(Feed(stream, "ond\n"), std::vector<std::string>{"second"});
}

void CarriageReturnIsStripped() {
    MessageStream stream;
    CHECK_EQ(Feed(stream, "windows\r\nunix\n"), (std::vector<std::string>{"windows", "unix"}));
}

void EmptyMessagesArePreserved() {
    MessageStream stream;
    CHECK_EQ(Feed(stream, "\n\n"), (std::vector<std::string>{"", ""}));
}

void EmbeddedZeroBytesArePreserved() {
    MessageStream stream;
    // Only '\n' is special: any other byte, including '\0', is payload.
    const std::string payload("bi\0nary", 7);
    CHECK_EQ(Feed(stream, payload + "\n"), std::vector<std::string>{payload});
}

void OrderIsPreservedAcrossChunks() {
    MessageStream stream;
    std::vector<std::string> received;
    for (int i = 0; i < 100; ++i) {
        // Split every message in half to make the boundaries land mid-message.
        const std::string message = "message-" + std::to_string(i);
        for (const std::string& part : Feed(stream, message.substr(0, 3))) {
            received.push_back(part);
        }
        for (const std::string& part : Feed(stream, message.substr(3) + "\n")) {
            received.push_back(part);
        }
    }

    std::vector<std::string> expected;
    for (int i = 0; i < 100; ++i) {
        expected.push_back("message-" + std::to_string(i));
    }
    CHECK_EQ(received, expected);
}

void PendingTailIsHandedOverOnDisconnect() {
    MessageStream stream;
    CHECK_EQ(Feed(stream, "done\nno newline here"), std::vector<std::string>{"done"});

    std::string tail;
    CHECK_TRUE(stream.TakePendingTail(&tail));
    CHECK_EQ(tail, std::string("no newline here"));
    CHECK_EQ(stream.pending_size(), size_t{0});
}

void NoTailWhenEverythingWasConsumed() {
    MessageStream stream;
    Feed(stream, "everything\n");

    std::string tail;
    CHECK_FALSE(stream.TakePendingTail(&tail));
}

void UnterminatedMessageOverTheLimitIsReported() {
    MessageStream stream(/*max_message_size=*/16);
    CHECK_EQ(Feed(stream, std::string(16, 'x')), std::vector<std::string>{});
    CHECK_FALSE(stream.Overflowed());

    CHECK_EQ(Feed(stream, "y"), std::vector<std::string>{});
    CHECK_TRUE(stream.Overflowed());
}

void BufferedCompleteMessagesDoNotCountAsOverflow() {
    MessageStream stream(/*max_message_size=*/8);
    // 20 buffered bytes, but they are complete messages waiting to be read out,
    // not an endless message from a misbehaving client.
    stream.Append("aaaa\nbbbb\ncccc\ndddd\n", 20);
    CHECK_FALSE(stream.Overflowed());
}

}  // namespace

int main() {
    RUN_TEST(OneCompleteMessageIsExtracted);
    RUN_TEST(SeveralMessagesInOneChunkAreSplit);
    RUN_TEST(MessageSplitAcrossChunksIsReassembled);
    RUN_TEST(IncompleteTailIsKeptForTheNextChunk);
    RUN_TEST(CarriageReturnIsStripped);
    RUN_TEST(EmptyMessagesArePreserved);
    RUN_TEST(EmbeddedZeroBytesArePreserved);
    RUN_TEST(OrderIsPreservedAcrossChunks);
    RUN_TEST(PendingTailIsHandedOverOnDisconnect);
    RUN_TEST(NoTailWhenEverythingWasConsumed);
    RUN_TEST(UnterminatedMessageOverTheLimitIsReported);
    RUN_TEST(BufferedCompleteMessagesDoNotCountAsOverflow);
    return testing::Summary();
}
