#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "client/text_wrap.h"

namespace {

using Lines = std::vector<std::string>;

void testFitsOnOneLine() {
    CHECK((chat::wrapText("12:00 matt: ", "hello there", 80) == Lines{"12:00 matt: hello there"}));
}

void testHangingIndent() {
    // Continuation lines line up under the body, not under the timestamp.
    CHECK((chat::wrapText("> ", "one two three four", 11) ==
           Lines{"> one two", "  three", "  four"}));
}

void testLongWordIsSplit() {
    CHECK((chat::wrapText("", "abcdefghij", 4) == Lines{"abcd", "efgh", "ij"}));
    CHECK((chat::wrapText("> ", "abcdefgh", 6) == Lines{"> abcd", "  efgh"}));
}

void testWhitespaceCollapses() {
    CHECK((chat::wrapText("", "  a   b  ", 80) == Lines{"a b"}));
}

void testEmptyBody() {
    CHECK((chat::wrapText("* ", "", 80) == Lines{"* "}));
    CHECK((chat::wrapText("* ", "   ", 80) == Lines{"* "}));
}

void testDegenerateWidths() {
    CHECK((chat::wrapText("p ", "body", 0) == Lines{"p body"}));
    CHECK((chat::wrapText("p ", "body", 1) == Lines{"p body"}));
}

void testNoLineExceedsWidth() {
    const std::string body = "the quick brown fox jumps over the lazy dog "
                             "supercalifragilisticexpialidocious and then some";
    for (int width = 2; width <= 40; ++width) {
        for (const std::string& line : chat::wrapText("12:00 someone: ", body, width)) {
            CHECK(static_cast<int>(line.size()) <= width);
        }
    }
}

} // namespace

int main() {
    testFitsOnOneLine();
    testHangingIndent();
    testLongWordIsSplit();
    testWhitespaceCollapses();
    testEmptyBody();
    testDegenerateWidths();
    testNoLineExceedsWidth();
    std::puts("text_wrap_test: ok");
    return 0;
}
