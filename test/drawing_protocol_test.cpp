#include <cstdio>
#include <string>
#include <utility>

#include "check.h"
#include "common/drawing_protocol.h"

using chat::Message;
using chat::MsgType;

namespace {
void testDrawingFraming() {
    const Message state{MsgType::ScreenDrawState, {"owner", "1", "42"}};
    const auto frame = chat::encode(state);
    for (std::size_t split = 0; split < frame.size(); ++split) {
        auto bytes = frame.substr(0, split);
        Message decoded;
        CHECK(chat::decode(bytes, decoded) == chat::DecodeStatus::Incomplete);
        bytes += frame.substr(split) + frame;
        CHECK(chat::decode(bytes, decoded) == chat::DecodeStatus::Ok);
        CHECK(decoded.type == state.type && decoded.fields == state.fields);
        CHECK(chat::decode(bytes, decoded) == chat::DecodeStatus::Ok && bytes.empty());
    }
    for (auto bytes :
         {std::string("\0\0\0\0", 4), std::string("\0\0\0\1\1", 5), std::string("\0\1\0\0", 4)}) {
        Message decoded;
        CHECK(chat::decode(bytes, decoded) == chat::DecodeStatus::Malformed);
    }
    auto corrupt = frame;
    corrupt[5] = char(0x7f); // A field length larger than the remaining frame.
    Message decoded;
    CHECK(chat::decode(corrupt, decoded) == chat::DecodeStatus::Malformed);
}

void testScreenDraw() {
    chat::ScreenDraw draw;
    const Message valid{MsgType::ScreenDraw, {"alice", "7", "12", "0", "10000", "123", "456", "5"}};
    CHECK(chat::parseScreenDraw(valid, false, draw));
    CHECK(draw.sharer == "alice" && draw.epoch == 7 && draw.id == 12);
    CHECK(draw.x1 == 0 && draw.y1 == 10000 && draw.x2 == 123 && draw.y2 == 456 && draw.color == 5);
    CHECK(chat::screenDrawMessage(draw, false).fields == valid.fields);
    draw.sharer = std::string(64, 'a');
    draw.author = std::string(64, 'b');
    CHECK(chat::parseScreenDraw(chat::screenDrawMessage(draw, true), true, draw));
    auto badIdentity = chat::screenDrawMessage(draw, true);
    badIdentity.fields[0] = std::string(65, 'a');
    CHECK(!chat::parseScreenDraw(badIdentity, true, draw));
    badIdentity.fields[0] = std::string(64, 'z');
    CHECK(!chat::parseScreenDraw(badIdentity, true, draw));
    draw.sharer = "alice";
    draw.author = "bob";
    const Message forwarded = chat::screenDrawMessage(draw, true);
    CHECK(forwarded.fields[2] == "bob");
    CHECK(chat::parseScreenDraw(forwarded, true, draw));
    CHECK(draw.author == "bob");
    CHECK(!chat::parseScreenDraw(forwarded, false, draw));
    CHECK(!chat::parseScreenDraw(valid, true, draw));
    for (std::size_t field = 1; field < valid.fields.size(); ++field) {
        for (const std::string bad : {"", "-1", "+1", "1.5", "nan", "99999999999999999999"}) {
            Message invalid = valid;
            invalid.fields[field] = bad;
            // Epochs use the existing signed decimal parser, which accepts '+'.
            if (field == 1 && bad == "+1")
                continue;
            CHECK(!chat::parseScreenDraw(invalid, false, draw));
        }
    }
    for (std::size_t field = 3; field <= 7; ++field) {
        Message invalid = valid;
        invalid.fields[field] = field == 7 ? "6" : "10001";
        CHECK(!chat::parseScreenDraw(invalid, false, draw));
    }
    Message invalid = valid;
    invalid.fields[1] = "0";
    CHECK(!chat::parseScreenDraw(invalid, false, draw));
    invalid = valid;
    invalid.fields[2] = "4294967296";
    CHECK(!chat::parseScreenDraw(invalid, false, draw));
    invalid = valid;
    invalid.type = MsgType::ScreenDrawWatch;
    CHECK(!chat::parseScreenDraw(invalid, false, draw));
}

void testScreenLaser() {
    chat::ScreenLaser point{"alice", "bob", 9, 42, 0, 10000, 0x12ab34, true};
    for (const bool fromOwner : {false, true}) {
        const auto message = chat::screenLaserMessage(point, fromOwner);
        chat::ScreenLaser decoded;
        CHECK(chat::parseScreenLaser(message, fromOwner, decoded));
        CHECK(decoded.sharer == "alice" && decoded.epoch == 9 && decoded.id == 42);
        CHECK(decoded.x == 0 && decoded.y == 10000 && decoded.rgb == 0x12ab34 && decoded.visible);
        CHECK(decoded.author == (fromOwner ? "bob" : ""));
        CHECK(!chat::parseScreenLaser(message, !fromOwner, decoded));
        auto invalid = message;
        invalid.fields.push_back("extra");
        CHECK(!chat::parseScreenLaser(invalid, fromOwner, decoded));
        invalid = message;
        invalid.fields.pop_back();
        CHECK(!chat::parseScreenLaser(invalid, fromOwner, decoded));
    }
    point.visible = false;
    chat::ScreenLaser decoded;
    CHECK(chat::parseScreenLaser(chat::screenLaserMessage(point, true), true, decoded) &&
          !decoded.visible);
    const auto valid = chat::screenLaserMessage(point, false);
    for (std::size_t field = 0; field < valid.fields.size(); ++field) {
        auto invalid = valid;
        invalid.fields[field] = "";
        CHECK(!chat::parseScreenLaser(invalid, false, decoded));
    }
    for (const auto& [field, bad] : {std::pair{0, std::string(21, 'a')},
                                     {1, "0"},
                                     {2, "0"},
                                     {2, "4294967296"},
                                     {3, "-1"},
                                     {3, "10001"},
                                     {4, "nan"},
                                     {5, "16777216"},
                                     {6, "2"}}) {
        auto invalid = valid;
        invalid.fields[field] = bad;
        CHECK(!chat::parseScreenLaser(invalid, false, decoded));
    }
    auto invalid = chat::screenLaserMessage(point, true);
    invalid.fields[2] = "";
    CHECK(!chat::parseScreenLaser(invalid, true, decoded));
    invalid = valid;
    invalid.type = MsgType::ScreenDraw;
    CHECK(!chat::parseScreenLaser(invalid, false, decoded));
}

void testStrokeMetadataAndErase() {
    chat::ScreenDraw pen;
    pen.sharer = "alice";
    pen.author = "bob";
    pen.epoch = 9;
    pen.id = 2;
    pen.stroke = 1;
    pen.rgb = 0x12ab34;
    chat::ScreenDraw decoded;
    CHECK(chat::parseScreenDraw(chat::screenDrawMessage(pen, true), true, decoded));
    CHECK(decoded.stroke == 1 && decoded.rgb == 0x12ab34);
    auto invalid = chat::screenDrawMessage(pen, true);
    invalid.fields.back() = "16777216";
    CHECK(!chat::parseScreenDraw(invalid, true, decoded));
    chat::ScreenErase erase{"alice", "bob", 9, {1, 2}};
    chat::ScreenErase parsed;
    CHECK(chat::parseScreenErase(chat::screenEraseMessage(erase), parsed));
    CHECK(parsed.author == "bob" && parsed.strokes == erase.strokes);
    auto request = chat::screenEraseMessage(erase);
    request.fields.back() = "-1";
    CHECK(!chat::parseScreenErase(request, parsed));
    erase.strokes.resize(chat::kMaxEraseStrokes + 1, 1);
    CHECK(!chat::parseScreenErase(chat::screenEraseMessage(erase), parsed));
}

} // namespace

int main() {
    testDrawingFraming();
    testScreenDraw();
    testScreenLaser();
    testStrokeMetadataAndErase();
    std::puts("drawing_protocol_test: ok");
}
