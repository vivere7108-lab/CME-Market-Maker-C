#include "harvester/replay/pyrandom.hpp"
#include "harvester/util/format.hpp"
#include "harvester/util/json.hpp"
#include "helpers.hpp"

using namespace test;

TEST_SUITE("util") {

TEST_CASE("PyRandom reproduces CPython's random module") {
    // Values produced by CPython 3.11's ``random.Random(seed)`` in this
    // exact call order: 3 random(), 3 randint(6, 20), 3 randint(-30, 30),
    // 4 choice((1, -1)), 2 expovariate(1/0.03), 1 randint(20, 200).
    struct Pin {
        std::uint64_t seed;
        std::array<double, 3> random;
        std::array<int, 3> r6_20;
        std::array<int, 3> r30;
        std::array<int, 4> choice;
        std::array<double, 2> expo;
        int r20_200;
    };
    const std::vector<Pin> pins = {
        {7, {0.32383276483316237, 0.15084917392450192, 0.65093447303985374}, {7, 19, 14}, {-24, -7, 7}, {1, 1, 1, 1},
         {0.017056061881173083, 0.0021724573918603237}, 43},
        {3, {0.23796462709189137, 0.54422922529595186, 0.36995516654807925}, {15, 13, 16}, {7, -26, 8}, {1, -1, -1, 1},
         {0.0063862970287418613, 0.037884948754879799}, 158},
        {0, {0.84442185152504812, 0.75795440294030247, 0.420571580830845}, {10, 14, 13}, {-5, 28, 20}, {-1, -1, -1, 1},
         {0.021076953057579971, 0.0099317967593476573}, 44},
        {123456789, {0.64140061618587263, 0.54218926809694945, 0.99317506628327212}, {19, 20, 18}, {8, -5, 29},
         {-1, -1, -1, -1}, {0.047952549334380763, 0.01744537553843661}, 196},
        {5000000000ULL, {0.08666010167354754, 0.60679492998459084, 0.015038337409537528}, {18, 10, 13}, {-3, 6, 7},
         {1, -1, -1, 1}, {0.058137843686758596, 0.016189210916483746}, 35},
    };
    for (const Pin& pin : pins) {
        CAPTURE(pin.seed);
        PyRandom r(pin.seed);
        for (const double v : pin.random) CHECK(r.random() == v);
        for (const int v : pin.r6_20) CHECK(r.randint(6, 20) == v);
        for (const int v : pin.r30) CHECK(r.randint(-30, 30) == v);
        const std::array<int, 2> sides{1, -1};
        for (const int v : pin.choice) CHECK(r.choice(sides) == v);
        for (const double v : pin.expo) CHECK(r.expovariate(1.0 / 0.03) == v);
        CHECK(r.randint(20, 200) == pin.r20_200);
    }
}

TEST_CASE("the JSON writer matches Python's compact dumps") {
    json::Writer w;
    w.begin_object();
    w.member("ts", 1.5).member("side", 1).member("price", 5000.0).member("none", std::optional<double>{});
    w.member("nan", std::nan("")).member("text", "a\"b\n").member("flag", true);
    w.key("reasons").value(std::vector<std::string>{"x", "y"});
    w.key("empty").begin_array().end_array();
    w.end_object();
    CHECK(w.str() == R"({"ts":1.5,"side":1,"price":5000.0,"none":null,"nan":null,"text":"a\"b\n","flag":true,"reasons":["x","y"],"empty":[]})");
}

TEST_CASE("the JSON reader parses what the writer wrote") {
    const json::Value v = json::parse(R"({"ts":1.5,"side":-1,"reasons":["x"],"nested":{"a":null},"e":1e-05,"t":true})");
    CHECK(*v.number("ts") == 1.5);
    CHECK(*v.number("side") == -1);
    CHECK(v.get("reasons")->array()[0].string() == "x");
    CHECK(v.get("nested")->get("a")->is_null());
    CHECK(*v.number("e") == doctest::Approx(1e-5));
    CHECK(std::get<bool>(v.get("t")->data));
    CHECK_THROWS(json::parse("{"));
    CHECK_THROWS(json::parse("[1,]"));
}

TEST_CASE("Python's number formats") {
    CHECK(fmt::commas(1512.4) == "1,512");
    CHECK(fmt::commas(-1512.6) == "-1,513");
    CHECK(fmt::commas(999.0) == "999");
    CHECK(fmt::commas(1234567.891, 2) == "1,234,567.89");
    CHECK(fmt::commas(0.0) == "0");
    CHECK(fmt::signed_int(5) == "+5");
    CHECK(fmt::signed_int(0) == "+0");
    CHECK(fmt::pct0(0.675) == "68%");
    CHECK(fmt::repr(5000.0) == "5000.0");
    CHECK(fmt::repr(0.1) == "0.1");
    CHECK(fmt::repr(-11.07) == "-11.07");
    CHECK(fmt::repr(1700000000.0) == "1700000000.0");
    CHECK(fmt::repr(1e16) == "1e+16");
    CHECK(fmt::repr(1.5e-5) == "1.5e-05");
    CHECK(fmt::repr(0.0001) == "0.0001");
    CHECK(fmt::repr(123456789012345680.0) == "1.2345678901234568e+17");
    CHECK(fmt::repr(0.0) == "0.0");
    CHECK(fmt::repr(-0.5) == "-0.5");
    CHECK(fmt::repr(5000.1875) == "5000.1875");
    CHECK(fmt::repr(1e15) == "1000000000000000.0");
    CHECK(fmt::repr(0.6563964564598195) == "0.6563964564598195");
    CHECK(fmt::g(0.5) == "0.5");
    CHECK(fmt::g(1.0) == "1");
}

TEST_CASE("the product converts between points, ticks and fixed prices") {
    const Product& p = es();
    CHECK(p.tick_int() == 250'000'000);
    CHECK(p.fixed(5000.125) == P(5000.0));  // half-even: 20000.5 -> 20000
    CHECK(p.fixed(5000.375) == P(5000.5));  // 20001.5 -> 20002
    CHECK(p.price(P(4999.75)) == 4999.75);
    CHECK(p.ticks(1.0) == 4.0);
    CHECK(p.round_to_tick(4999.8) == 4999.75);
    CHECK(p.tick_value() == 12.5);
    CHECK(get_product(" mes ").name == "MES");
    CHECK_THROWS_AS(get_product("XX"), UnknownProduct);
}

}

#include "harvester/execution/bid64.hpp"

TEST_SUITE("bid64") {

TEST_CASE("BID64 encodes, prints and parses the way Intel's library does") {
    using namespace harvester::bid64;
    // Known encodings: 1 = 0x31c0000000000001, 0 = 0x31c0000000000000.
    CHECK(from_string("1") == 0x31c0000000000001ULL);
    CHECK(from_string("0") == 0x31c0000000000000ULL);
    CHECK(to_string(from_string("1")) == "+1E+0");
    CHECK(to_string(from_string("2.5")) == "+25E-1");
    CHECK(to_string(from_string("-0.01")) == "-1E-2");
    CHECK(to_string(from_string("1e3")) == "+1E+3");
    CHECK(to_double(from_string("2.5")) == 2.5);
    CHECK(to_double(from_string("9999999999999999")) == 9999999999999999.0);
    CHECK(to_double(from_string("12345678901234567")) == doctest::Approx(12345678901234568.0));
    CHECK(to_string(from_double(2.0)) == "+2E+0");
    CHECK(to_string(from_double(1.5)) == "+15E-1");
    CHECK(to_string(from_double(-3.0)) == "-3E+0");
    CHECK(to_double(from_double(4999.75)) == 4999.75);
    CHECK(decode(from_string("")).nan);
    CHECK(decode(from_string("abc")).nan);
    CHECK(to_string(from_string("NaN")) == "+NaN");
    CHECK(std::isnan(to_double(nan())));
    // A coefficient above 2^53 uses the second encoding form and round-trips.
    const Bits big = from_string("9007199254740993");
    CHECK(decode(big).coefficient == 9007199254740993ULL);
    CHECK(to_string(big) == "+9007199254740993E+0");
    CHECK(to_double(from_string("1e400")) == std::numeric_limits<double>::infinity());
}

}
