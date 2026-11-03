#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>

TEST_CASE("parallel translation preserves deterministic order", "[parallel]") {
    std::vector<cppfrontier::file_request> requests;
    for (int index = 0; index < 32; ++index) {
        requests.push_back({static_cast<std::uint64_t>(index), "hello: () = \"hello\";\n", {}});
    }
    auto results = cppfrontier::parallel_translator{4}.translate_files(requests);
    REQUIRE(results.size() == requests.size());
    for (std::size_t index = 0; index < results.size(); ++index) CHECK(results[index].index == index);
}

TEST_CASE("parallel translation handles an empty request list", "[parallel]") {
    const auto results = cppfrontier::parallel_translator{4}.translate_files({});
    CHECK(results.empty());
}

TEST_CASE("parallel translation does not invent results for cancelled requests", "[parallel]") {
    auto translator = cppfrontier::parallel_translator{4};
    translator.request_cancel();
    const auto results = translator.translate_files({
        {1, "hello: () = \"hello\";\n", {}},
        {2, "world: () = \"world\";\n", {}},
    });
    CHECK(results.size() == 2);
    CHECK(results[0].index == 1);
    CHECK(results[1].index == 2);
    CHECK_FALSE(results[0].completed);
    CHECK_FALSE(results[1].completed);
    CHECK(results[0].translation.cpp1.empty());
    CHECK(results[1].translation.cpp1.empty());

    const auto next = translator.translate_files({{3, "hello: () = \"hello\";\n", {}}});
    REQUIRE(next.size() == 1);
    CHECK(next[0].completed);
    CHECK_FALSE(next[0].translation.cpp1.empty());
}

TEST_CASE("parallel translation completes every request", "[parallel]") {
    std::vector<cppfrontier::file_request> requests;
    for (int index = 0; index < 9; ++index) {
        requests.push_back({static_cast<std::uint64_t>(index), "hello: () = \"hello\";\n", {}});
    }
    const auto results = cppfrontier::parallel_translator{4}.translate_files(requests);
    for (const auto& result : results) CHECK(result.completed);
}

TEST_CASE("parallel translation keeps a cancel that stopped no request", "[parallel]") {
    auto translator = cppfrontier::parallel_translator{4};
    translator.request_cancel();
    CHECK(translator.translate_files({}).empty());
    const auto stopped = translator.translate_files({{1, "hello: () = \"hello\";\n", {}}});
    REQUIRE(stopped.size() == 1);
    CHECK_FALSE(stopped[0].completed);
    const auto next = translator.translate_files({{2, "hello: () = \"hello\";\n", {}}});
    REQUIRE(next.size() == 1);
    CHECK(next[0].completed);
}
