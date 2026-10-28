// view_image support: the pure pieces the tool relies on -- attachment loading
// with all of its limits, the synthetic flag, the bubble marker and the
// one-follow-up-turn-per-round rule. The tool lambda itself needs a live TUI and
// is verified by hand (see 04_VERIFY.md).
#include "doctest.h"

#include "client/models.h"
#include "core/image_input.h"
#include "core/turn_requests.h"
#include "json.hpp"
#include "tui/bubble_model.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// 8-byte PNG signature is all the mime sniffer looks at; the body is irrelevant
// because nothing decodes the image here.
const char kPngMagic[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

fs::path makeTempDir(const std::string& tag) {
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / ("projv_viewimg_" + tag);
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

void writePng(const fs::path& file) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    out.write(kPngMagic, sizeof(kPngMagic));
    const char filler[8] = {'b', 'o', 'd', 'y', 'b', 'o', 'd', 'y'};
    out.write(filler, sizeof(filler));
}

} // namespace

TEST_CASE("view_image: a valid path becomes an encoded attachment") {
    const fs::path ws = makeTempDir("ok");
    writePng(ws / "shot.png");

    std::vector<ImageAttachment> imgs;
    std::string err;
    REQUIRE(image_input::loadAttachments({"shot.png"}, ws.string(), imgs, err));
    CHECK(err.empty());
    REQUIRE(imgs.size() == 1);
    CHECK(imgs[0].mime == "image/png");
    CHECK(imgs[0].detail == "auto");
    CHECK(imgs[0].dataUrl.rfind("data:image/png;base64,", 0) == 0);
    CHECK(imgs[0].path.find("shot.png") != std::string::npos);
    std::error_code ec;
    fs::remove_all(ws, ec);
}

TEST_CASE("view_image: a directory expands to its top-level images") {
    const fs::path ws = makeTempDir("dir");
    writePng(ws / "in" / "a.png");
    writePng(ws / "in" / "b.png");
    std::ofstream(ws / "in" / "notes.txt") << "not an image";

    std::vector<ImageAttachment> imgs;
    std::string err;
    REQUIRE(image_input::loadAttachments({"in"}, ws.string(), imgs, err));
    CHECK(imgs.size() == 2); // the text file is skipped, not fatal
    std::error_code ec;
    fs::remove_all(ws, ec);
}

TEST_CASE("view_image: more than 8 images is refused as a whole") {
    const fs::path ws = makeTempDir("many");
    std::vector<std::string> paths;
    for (int i = 0; i < 9; ++i) {
        const std::string name = "img" + std::to_string(i) + ".png";
        writePng(ws / name);
        paths.push_back(name);
    }
    std::vector<ImageAttachment> imgs;
    std::string err;
    CHECK_FALSE(image_input::loadAttachments(paths, ws.string(), imgs, err));
    CHECK(imgs.empty()); // nothing is sent on failure
    CHECK(err.find("too many images") != std::string::npos);
    std::error_code ec;
    fs::remove_all(ws, ec);
}

TEST_CASE("view_image: non-images, missing files and empty lists are refused") {
    const fs::path ws = makeTempDir("bad");
    std::ofstream(ws / "notes.txt") << "plain text";
    {
        std::vector<ImageAttachment> imgs;
        std::string err;
        CHECK_FALSE(image_input::loadAttachments({"notes.txt"}, ws.string(), imgs, err));
        CHECK_FALSE(err.empty());
    }
    {
        std::vector<ImageAttachment> imgs;
        std::string err;
        CHECK_FALSE(image_input::loadAttachments({"nope.png"}, ws.string(), imgs, err));
        CHECK(err.find("not found") != std::string::npos);
    }
    {
        std::vector<ImageAttachment> imgs;
        std::string err;
        CHECK_FALSE(image_input::loadAttachments({}, ws.string(), imgs, err));
        CHECK_FALSE(err.empty());
    }
    std::error_code ec;
    fs::remove_all(ws, ec);
}

TEST_CASE("view_image: a synthetic user message carries the image block") {
    Message msg = Message::User("look at this");
    ImageAttachment img;
    img.path = "C:/tmp/shot.png";
    img.mime = "image/png";
    img.dataUrl = "data:image/png;base64,AAAA";
    msg.images.push_back(img);
    msg.synthetic = true;

    nlohmann::json j = msg;
    REQUIRE(j["content"].is_array());
    CHECK(j["content"][0]["type"] == "text");
    CHECK(j["content"][1]["type"] == "image_url");
    CHECK(j["content"][1]["image_url"]["url"] == "data:image/png;base64,AAAA");
    // The flag is local bookkeeping -- it must never reach the API.
    CHECK_FALSE(j.contains("synthetic"));

    const auto bubbles = bubble_model::deriveBubbles(msg, {});
    REQUIRE(bubbles.size() == 1);
    CHECK(bubbles[0].role == "user");
    CHECK(bubbles[0].synthetic);
    CHECK(bubbles[0].content.find("[Image] shot.png") != std::string::npos);
}

TEST_CASE("view_image: a user message without the flag is not marked") {
    const auto bubbles = bubble_model::deriveBubbles(Message::User("typed by hand"), {});
    REQUIRE(bubbles.size() == 1);
    CHECK_FALSE(bubbles[0].synthetic);
}

TEST_CASE("view_image: only one follow-up turn can be queued per round") {
    turn_requests::State st;
    CHECK_FALSE(st.busy());
    CHECK(st.claim(turn_requests::Kind::Image));
    CHECK(st.is(turn_requests::Kind::Image));
    CHECK_FALSE(st.claim(turn_requests::Kind::Skill)); // second caller is refused
    CHECK(st.refusedMessage() != nullptr);
    CHECK(std::string(st.refusedMessage()).find("view_image") != std::string::npos);

    st.release();
    CHECK_FALSE(st.busy());
    CHECK(st.claim(turn_requests::Kind::Skill)); // next round is fine
    CHECK(std::string(st.refusedMessage()).find("run_skill") != std::string::npos);
    CHECK_FALSE(st.claim(turn_requests::Kind::None));
}

TEST_CASE("view_image: the capability check still gates the tool") {
    CHECK(modelSupportsVision("deepseek-v4-flash"));
    CHECK_FALSE(modelSupportsVision("deepseek-v4-pro"));
}
