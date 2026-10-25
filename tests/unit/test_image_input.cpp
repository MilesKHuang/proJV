// Tests for vision input (doc 01): "@path" parsing, base64/magic sniffing,
// content-block serialization and token accounting.
#include "doctest.h"

#include "client/models.h"
#include "core/session.h"
#include "core/image_input.h"
#include "tools/base64.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Minimal byte sequences whose magic numbers identify the four formats.
const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(32, 'P');
const std::string kJpg = std::string("\xFF\xD8\xFF\xE0", 4) + std::string(32, 'J');
const std::string kGif = std::string("GIF89a", 6) + std::string(32, 'G');
const std::string kWebp =
    std::string("RIFF", 4) + std::string(4, '\0') + std::string("WEBP", 4) + std::string(32, 'W');

void writeFake(const fs::path& p, const std::string& bytes) {
    std::ofstream ofs(p, std::ios::binary);
    ofs.write(bytes.data(), (std::streamsize)bytes.size());
}

// RAII temp workspace: <temp>/projv_img_<n>/
struct TmpDir {
    fs::path path;
    TmpDir() {
        static int n = 0;
        path = fs::temp_directory_path() / ("projv_img_" + std::to_string(++n));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

} // namespace

TEST_CASE("base64: RFC 4648 alphabet + padding") {
    CHECK(imgutil::base64Encode(std::string("")) == "");
    CHECK(imgutil::base64Encode(std::string("f")) == "Zg==");
    CHECK(imgutil::base64Encode(std::string("fo")) == "Zm8=");
    CHECK(imgutil::base64Encode(std::string("foo")) == "Zm9v");
    CHECK(imgutil::base64Encode(std::string("hello")) == "aGVsbG8=");
    CHECK(imgutil::base64Encode(std::string("\x00\x01\x02", 3)) == "AAEC");
}

TEST_CASE("magic sniffing: JPEG / PNG / GIF / WebP / not an image") {
    CHECK(imgutil::sniffImageMime(kPng) == "image/png");
    CHECK(imgutil::sniffImageMime(kJpg) == "image/jpeg");
    CHECK(imgutil::sniffImageMime(kGif) == "image/gif");
    CHECK(imgutil::sniffImageMime(kWebp) == "image/webp");
    CHECK(imgutil::sniffImageMime("hello world") == "");
    CHECK(imgutil::sniffImageMime("") == "");
    // RIFF without the WEBP tag is not an image.
    CHECK(imgutil::sniffImageMime(std::string("RIFF....WAVE", 12)) == "");
}

TEST_CASE("markers: plain text is never touched") {
    const auto r1 = image_input::parseMarkers("hello @world and a@b.com", "");
    CHECK(r1.ok());
    CHECK(r1.hadMarkers == false);
    CHECK(r1.text == "hello @world and a@b.com");
    CHECK(r1.images.empty());

    // "@param" is ordinary text (no path shape follows the '@').
    const auto r2 = image_input::parseMarkers("@param is a doc tag", "");
    CHECK(r2.hadMarkers == false);
    CHECK(r2.text == "@param is a doc tag");
}

TEST_CASE("markers: quoted absolute path is stripped and encoded") {
    TmpDir tmp;
    const fs::path png = tmp.path / "shot.png";
    writeFake(png, kPng);

    const std::string input = "what is this? @\"" + png.string() + "\"";
    const auto r = image_input::parseMarkers(input, "");

    REQUIRE(r.ok());
    CHECK(r.hadMarkers);
    CHECK(r.text == "what is this?");
    REQUIRE(r.images.size() == 1);
    CHECK(r.images[0].mime == "image/png");
    CHECK(r.images[0].detail == "auto");
    CHECK(r.images[0].dataUrl.rfind("data:image/png;base64,", 0) == 0);
}

TEST_CASE("markers: relative ./ path resolves against the workspace") {
    TmpDir tmp;
    writeFake(tmp.path / "a.jpg", kJpg);

    const auto r = image_input::parseMarkers("look @./a.jpg", tmp.str());
    REQUIRE(r.ok());
    REQUIRE(r.images.size() == 1);
    CHECK(r.images[0].mime == "image/jpeg");
    CHECK(fs::path(r.images[0].path) == tmp.path / "a.jpg");
    CHECK(r.text == "look");
}

TEST_CASE("markers: directory expands top-level images in name order") {
    TmpDir tmp;
    writeFake(tmp.path / "b.png", kPng);
    writeFake(tmp.path / "a.gif", kGif);
    writeFake(tmp.path / "notes.txt", "not an image");
    fs::create_directories(tmp.path / "sub");
    writeFake(tmp.path / "sub" / "c.png", kPng); // recursion is not allowed

    const auto r = image_input::parseMarkers("@\"" + tmp.str() + "\"", "");
    REQUIRE(r.ok());
    REQUIRE(r.images.size() == 2);
    CHECK(fs::path(r.images[0].path).filename().string() == "a.gif");
    CHECK(fs::path(r.images[1].path).filename().string() == "b.png");
    CHECK(r.text.empty());
}

TEST_CASE("markers: two markers merge in written order") {
    TmpDir tmp;
    writeFake(tmp.path / "one.png", kPng);
    writeFake(tmp.path / "two.webp", kWebp);

    const auto r =
        image_input::parseMarkers("@\"" + (tmp.path / "one.png").string() + "\" and @\"" +
                                      (tmp.path / "two.webp").string() + "\" please",
                                  "");
    REQUIRE(r.ok());
    REQUIRE(r.images.size() == 2);
    CHECK(r.images[0].mime == "image/png");
    CHECK(r.images[1].mime == "image/webp");
    CHECK(r.text == "and please");
}

TEST_CASE("markers: rejections stop the send with a readable reason") {
    TmpDir tmp;
    writeFake(tmp.path / "notes.txt", "plain text");
    for (int i = 0; i < 9; ++i)
        writeFake(tmp.path / ("img" + std::to_string(i) + ".png"), kPng);

    SUBCASE("unsupported format") {
        const auto r =
            image_input::parseMarkers("@\"" + (tmp.path / "notes.txt").string() + "\"", "");
        CHECK_FALSE(r.ok());
        CHECK(r.error.find("unsupported format") != std::string::npos);
    }

    SUBCASE("missing file") {
        const auto r =
            image_input::parseMarkers("@\"" + (tmp.path / "nope.png").string() + "\"", "");
        CHECK_FALSE(r.ok());
        CHECK(r.error.find("not found") != std::string::npos);
    }

    SUBCASE("more than 8 images in one message") {
        const auto r = image_input::parseMarkers("@\"" + tmp.str() + "\"", "");
        CHECK_FALSE(r.ok());
        CHECK(r.error.find("at most 8") != std::string::npos);
    }
}

TEST_CASE("token accounting: 1024 tokens per image") {
    CHECK(Session::estimateImageTokens(0) == 0);
    CHECK(Session::estimateImageTokens(1) == 1024);
    CHECK(Session::estimateImageTokens(8) == 8192);
    CHECK(image_input::imageTokens(3) == 3072);
}

TEST_CASE("serialization: user images become text + image_url blocks") {
    Message msg = Message::User("what is this?");
    ImageAttachment img;
    img.path = "C:/pics/a.png";
    img.mime = "image/png";
    img.dataUrl = "data:image/png;base64,AAAA";
    msg.images.push_back(img);

    nlohmann::json j;
    to_json(j, msg);

    REQUIRE(j["content"].is_array());
    REQUIRE(j["content"].size() == 2);
    CHECK(j["content"][0]["type"] == "text");
    CHECK(j["content"][0]["text"] == "what is this?");
    CHECK(j["content"][1]["type"] == "image_url");
    CHECK(j["content"][1]["image_url"]["url"] == "data:image/png;base64,AAAA");
    CHECK(j["content"][1]["image_url"]["detail"] == "auto");
}

TEST_CASE("serialization: messages without images keep the string form") {
    Message msg = Message::User("plain");
    nlohmann::json j;
    to_json(j, msg);
    CHECK(j["content"].is_string());
    CHECK(j["content"] == "plain");
}

TEST_CASE("serialization: non-user roles drop images (API returns 400)") {
    Message msg = Message::Assistant("done");
    ImageAttachment img;
    img.dataUrl = "data:image/png;base64,AAAA";
    msg.images.push_back(img);

    nlohmann::json j;
    to_json(j, msg);
    CHECK(j["content"].is_string());
    CHECK(j["content"] == "done");
}

TEST_CASE("serialization: an unreadable path degrades to a text note") {
    Message msg = Message::User("x");
    ImageAttachment img;
    img.path = "C:/definitely/missing.png"; // no dataUrl cache
    msg.images.push_back(img);

    nlohmann::json j;
    to_json(j, msg);
    REQUIRE(j["content"].is_array());
    REQUIRE(j["content"].size() == 2);
    CHECK(j["content"][1]["type"] == "text");
    CHECK(j["content"][1]["text"].get<std::string>().find("image unavailable") !=
          std::string::npos);
}

TEST_CASE("serialization: from_json restores text and image parts") {
    nlohmann::json j = {
        {"role", "user"},
        {"content",
         nlohmann::json::array(
             {{{"type", "text"}, {"text", "hi"}},
              {{"type", "image_url"},
               {"image_url", {{"url", "data:image/png;base64,AAAA"}, {"detail", "low"}}}}})}};
    Message msg = j.get<Message>();
    CHECK(msg.content == "hi");
    REQUIRE(msg.images.size() == 1);
    CHECK(msg.images[0].dataUrl == "data:image/png;base64,AAAA");
    CHECK(msg.images[0].detail == "low");
}

TEST_CASE("model support: vision is a Flash-line capability") {
    CHECK(modelSupportsVision("deepseek-flash"));
    CHECK(modelSupportsVision("deepseek-v4-flash"));
    CHECK(modelSupportsVision("deepseek-v4-flash-vision-exp"));
    CHECK_FALSE(modelSupportsVision("deepseek-v4-pro"));
    CHECK_FALSE(modelSupportsVision("deepseek-reasoner"));
}
