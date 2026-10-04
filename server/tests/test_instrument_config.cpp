// Tests for config::load_instruments / parse_instruments: the shared instrument list
// (trading-project/config/instruments.json) that the sequencer and every engine_node read
// at startup. A bad file must stop a service at startup with a message that says
// what is wrong and where, never be half-used.

#include "config/instruments.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// Expects parse_instruments(text) to fail with a message containing `needle`.
void rejects(const std::string &text, const std::string &needle) {
    INFO("input: " << text);
    try {
        config::parse_instruments(text, "test.json");
        FAIL("accepted an invalid file");
    } catch (const config::ConfigError &error) {
        INFO("message: " << error.what());
        CHECK(std::string(error.what()).find(needle) != std::string::npos);
        CHECK(std::string(error.what()).rfind("test.json", 0) == 0); // always names the file
    }
}

// A valid file with one instrument replaced by `instrument` (a JSON object as text).
std::string with_instrument(const std::string &instrument) {
    return R"({"version": 1, "partitions": 2, "instruments": [)" + instrument + "]}";
}

} // namespace

TEST_CASE("instruments: a valid file is read in order, with every field", "[config]") {
    const auto cfg = config::parse_instruments(R"({
        "version": 7,
        "partitions": 2,
        "instruments": [
            {"id": 1, "ticker": "MOOG",  "partition": 0, "band_bps": 1000},
            {"id": 3, "ticker": "TESLO", "partition": 1, "band_bps": 500}
        ]
    })");
    CHECK(cfg.version == 7);
    CHECK(cfg.partitions == 2);
    REQUIRE(cfg.instruments.size() == 2);
    CHECK(cfg.instruments[0].id == 1);
    CHECK(cfg.instruments[0].ticker == "MOOG");
    CHECK(cfg.instruments[0].partition == 0);
    CHECK(cfg.instruments[0].band_bps == 1000);
    CHECK(cfg.instruments[1].id == 3);
    CHECK(cfg.instruments[1].ticker == "TESLO");
    CHECK(cfg.instruments[1].partition == 1);
    CHECK(cfg.instruments[1].band_bps == 500);

    REQUIRE(cfg.find(3) != nullptr);
    CHECK(cfg.find(3)->ticker == "TESLO");
    REQUIRE(cfg.find("MOOG") != nullptr);
    CHECK(cfg.find("MOOG")->id == 1);
    CHECK(cfg.find(2) == nullptr);
    CHECK(cfg.find("moog") == nullptr); // exact match only
}

TEST_CASE("instruments: the file shipped in the repo is valid", "[config]") {
    // The default path is this repo's config/instruments.json; a broken commit to it must fail here,
    // not when the exchange starts.
    const char *saved = std::getenv("EXCHANGE_INSTRUMENTS");
    const std::string saved_value = saved ? saved : "";
    unsetenv("EXCHANGE_INSTRUMENTS");

    const std::string path = config::instruments_path();
    CHECK(path.find("instruments.json") != std::string::npos);
    const auto cfg = config::load_instruments(path);
    CHECK(cfg.partitions >= 1);
    CHECK_FALSE(cfg.instruments.empty());

    if (saved)
        setenv("EXCHANGE_INSTRUMENTS", saved_value.c_str(), 1);
}

TEST_CASE("instruments: EXCHANGE_INSTRUMENTS overrides the default path", "[config]") {
    const auto file = std::filesystem::temp_directory_path() / "exchange_test_instruments.json";
    {
        std::ofstream out(file);
        out << R"({"version": 42, "partitions": 1, "instruments": [{"id": 9, "ticker": "ZED", "partition": 0, "band_bps": 100}]})";
    }
    setenv("EXCHANGE_INSTRUMENTS", file.c_str(), 1);
    CHECK(config::instruments_path() == file.string());
    const auto cfg = config::load_instruments(config::instruments_path());
    CHECK(cfg.version == 42);
    CHECK(cfg.find("ZED") != nullptr);

    setenv("EXCHANGE_INSTRUMENTS", "", 1);                                                  // empty counts as unset
    CHECK(config::instruments_path().find("config/instruments.json") != std::string::npos); // back to the default
    unsetenv("EXCHANGE_INSTRUMENTS");
    std::filesystem::remove(file);
}

TEST_CASE("instruments: a missing file names the path", "[config]") {
    try {
        config::load_instruments("/no/such/dir/instruments.json");
        FAIL("loaded a file that does not exist");
    } catch (const config::ConfigError &error) {
        CHECK(std::string(error.what()).find("/no/such/dir/instruments.json") != std::string::npos);
    }
}

TEST_CASE("instruments: broken or wrongly shaped files are rejected", "[config]") {
    rejects("{ not json", "not valid JSON");
    rejects("[]", "top level must be an object");
    rejects(R"({"partitions": 1, "instruments": [{"id": 1, "ticker": "A", "partition": 0, "band_bps": 1}]})",
            "missing field \"version\"");
    rejects(R"({"version": 1, "instruments": [{"id": 1, "ticker": "A", "partition": 0, "band_bps": 1}]})",
            "missing field \"partitions\"");
    rejects(R"({"version": 1, "partitions": 1})", "missing field \"instruments\"");
    rejects(R"({"version": 1, "partitions": 1, "instruments": []})", "non-empty list");
    rejects(R"({"version": 1, "partitions": 1, "instruments": {}})", "non-empty list");
    rejects(R"({"version": 1, "partitions": 1, "instruments": [5]})", "must be an object");
}

TEST_CASE("instruments: a typo in a field name is an error, not ignored", "[config]") {
    rejects(R"({"version": 1, "partitions": 1, "partiton": 2,
                "instruments": [{"id": 1, "ticker": "A", "partition": 0, "band_bps": 1}]})",
            "unknown field \"partiton\"");
    rejects(with_instrument(R"({"id": 1, "ticker": "A", "partiton": 0, "band_bps": 1})"),
            "instruments[0]: unknown field \"partiton\"");
}

TEST_CASE("instruments: top-level numbers must be whole and in range", "[config]") {
    const std::string list = R"("instruments": [{"id": 1, "ticker": "A", "partition": 0, "band_bps": 1}])";
    rejects(R"({"version": 0, "partitions": 1, )" + list + "}", "\"version\" must be between 1");
    rejects(R"({"version": 1.5, "partitions": 1, )" + list + "}", "\"version\" must be a whole number");
    rejects(R"({"version": 1, "partitions": 0, )" + list + "}", "\"partitions\" must be between 1 and 1024");
    rejects(R"({"version": 1, "partitions": 1025, )" + list + "}", "\"partitions\" must be between 1 and 1024");
    rejects(R"({"version": 1, "partitions": "2", )" + list + "}", "\"partitions\" must be a whole number");
}

TEST_CASE("instruments: each instrument's fields are checked", "[config]") {
    rejects(with_instrument(R"({"ticker": "A", "partition": 0, "band_bps": 1})"),
            "instruments[0]: missing field \"id\"");
    rejects(with_instrument(R"({"id": 0, "ticker": "A", "partition": 0, "band_bps": 1})"), "\"id\" must be between 1");
    rejects(with_instrument(R"({"id": 4294967296, "ticker": "A", "partition": 0, "band_bps": 1})"),
            "\"id\" must be between 1 and 4294967295");
    rejects(with_instrument(R"({"id": -1, "ticker": "A", "partition": 0, "band_bps": 1})"), "\"id\" must be between");
    rejects(with_instrument(R"({"id": 1.0, "ticker": "A", "partition": 0, "band_bps": 1})"),
            "\"id\" must be a whole number");
    rejects(with_instrument(R"({"id": 1, "ticker": "A", "partition": 2, "band_bps": 1})"),
            "\"partition\" must be between 0 and 1");
    rejects(with_instrument(R"({"id": 1, "ticker": "A", "partition": -1, "band_bps": 1})"),
            "\"partition\" must be between 0 and 1");
    rejects(with_instrument(R"({"id": 1, "ticker": "A", "partition": 0, "band_bps": 0})"),
            "\"band_bps\" must be between 1 and 9999");
    rejects(with_instrument(R"({"id": 1, "ticker": "A", "partition": 0, "band_bps": 10000})"),
            "\"band_bps\" must be between 1 and 9999");
}

TEST_CASE("instruments: tickers are 1-12 of A-Z and 0-9, starting with a letter", "[config]") {
    const std::string bad = "\"ticker\" must be 1-12 characters";
    for (const char *ticker :
         {R"("")", R"("moog")", R"("MO OG")", R"("1MOOG")", R"("MOOg")", R"("ABCDEFGHIJKLM")", "5", "null"})
        rejects(
            with_instrument(std::string(R"({"id": 1, "ticker": )") + ticker + R"(, "partition": 0, "band_bps": 1})"),
            bad);

    const auto ok = config::parse_instruments(
        with_instrument(R"({"id": 1, "ticker": "ABCDEFGHIJK2", "partition": 0, "band_bps": 1})"));
    CHECK(ok.instruments[0].ticker == "ABCDEFGHIJK2"); // 12 characters, digits after the first: fine
}

TEST_CASE("instruments: duplicate ids or tickers are rejected", "[config]") {
    rejects(with_instrument(R"({"id": 1, "ticker": "A", "partition": 0, "band_bps": 1},
                               {"id": 1, "ticker": "B", "partition": 1, "band_bps": 1})"),
            "instruments[1]: duplicate id 1");
    rejects(with_instrument(R"({"id": 1, "ticker": "A", "partition": 0, "band_bps": 1},
                               {"id": 2, "ticker": "A", "partition": 1, "band_bps": 1})"),
            "instruments[1]: duplicate ticker \"A\"");
}
