#include <catch2/catch_all.hpp>
#include <cppfrontier/translator.hpp>
#include <cppfrontier/type.hpp>

#include <algorithm>
#include "../../src/cpp1_type_renderer.hpp"

using cppfrontier::parse_type;
using cppfrontier::type_kind;

namespace {

auto translate(const char* source) -> std::string {
    const auto result = cppfrontier::translator{}.translate_source(source);
    INFO(result.cpp1);
    REQUIRE_FALSE(result.has_errors());
    return result.cpp1;
}

}

TEST_CASE("type-ids parse into a structured model", "[type-model]") {
    const auto name = parse_type("::std::map<int, std::vector<T>>");
    CHECK(name.kind == type_kind::name);
    CHECK(name.is_global);
    REQUIRE(name.components.size() == 2);
    CHECK(name.components[1].identifier == "map");
    REQUIRE(name.components[1].arguments.size() == 2);
    CHECK(name.components[1].arguments[1].components[1].identifier == "vector");

    const auto pointer = parse_type("* const * i32");
    CHECK(pointer.kind == type_kind::pointer);
    CHECK(cppfrontier::pointer_depth(pointer) == 2);
    CHECK(pointer.core().kind == type_kind::fundamental);

    const auto function = parse_type("(in x: i32, move _: std::string) -> forward T");
    REQUIRE(function.is_function());
    REQUIRE(function.parameters.size() == 2);
    CHECK(function.parameters[1].passing == cppfrontier::passing_style::move);
    CHECK(function.returns_forward);

    const auto constrained = parse_type("_ is std::integral");
    CHECK(constrained.is_wildcard());
    REQUIRE(constrained.constraint() != nullptr);
    CHECK(constrained.constraint()->text == "std::integral");

    CHECK(parse_type("T...").kind == type_kind::pack);
    CHECK(parse_type("decltype(x)").kind == type_kind::decltype_of);
    CHECK(parse_type("std::array<int, :i32 = 3>").components[1].arguments[1].kind ==
          type_kind::expression);
    CHECK(cppfrontier::mentions_name(parse_type("std::vector<T>"), "T"));
    CHECK_FALSE(cppfrontier::mentions_name(parse_type("U::T"), "T"));
}

TEST_CASE("type-ids render to Cpp1", "[type-model]") {
    CHECK(cppfrontier::render_cpp1_type(parse_type("* const i32")) == "int const*");
    CHECK(cppfrontier::render_cpp1_type(parse_type("std::vector<u8>")) ==
          "std::vector<unsigned char>");
    CHECK(cppfrontier::render_cpp1_type_declaration(parse_type("*(x: i32) -> *i32"), "p") ==
          "int*(*p)(cpp2::impl::in<int> x)");
    CHECK(cppfrontier::is_simple_type_specifier(parse_type("std::vector<int*>")));
    CHECK_FALSE(cppfrontier::is_simple_type_specifier(parse_type("*int")));
    CHECK_FALSE(cppfrontier::is_simple_type_specifier(parse_type("ulong")));
}

TEST_CASE("a function type keeps the declarator of its return type", "[type-model]") {
    const auto cpp1 = translate("f: (p: *(x: i32) -> *i32) -> i32 = 0;\n");
    CHECK(cpp1.find("auto f(int*(*p)(cpp2::impl::in<int> x)) -> int;") != std::string::npos);
}

TEST_CASE("dependent qualified type names take typename", "[type-model]") {
    const auto cpp1 = translate(
        "g: <T> (v: T) -> T::value_type = { r: T::value_type = v.front(); return r; }\n"
        "h: <T> (v: T) -> T::rebind<int> = { return (); }\n"
        "k: <T> (v: std::vector<T::value_type>) = { }\n");
    CHECK(cpp1.find("-> typename T::value_type;") != std::string::npos);
    CHECK(cpp1.find("typename T::value_type r {") != std::string::npos);
    CHECK(cpp1.find("-> typename T::template rebind<int>;") != std::string::npos);
    // A template argument may name a value, so it is left as written.
    CHECK(cpp1.find("std::vector<T::value_type>") != std::string::npos);
}

TEST_CASE("a template argument with a pointer is a simple type specifier", "[type-model]") {
    const auto cpp1 = translate(
        "k: (x) -> std::vector<*int> = {\n"
        "    return inspect x -> std::vector<*int> { is int = (1); is _ = (); };\n"
        "}\n");
    CHECK(cpp1.find("else return std::vector<int*>{};") != std::string::npos);
    CHECK(cpp1.find("type_identity_t") == std::string::npos);
}

TEST_CASE("an out-of-line alias qualifies only names of its enclosing type", "[type-model]") {
    const auto cpp1 = translate(
        "optional: <T> type = { v: int = 0; }\n"
        "outer: type = {\n"
        "  opt: type = { i: int = 0; operator=: (out this) = { } z: optional<opt> == (); }\n"
        "}\n");
    CHECK(cpp1.find("inline constexpr optional<outer::opt> outer::opt::z {};") !=
          std::string::npos);
}

TEST_CASE("type constraints are checked against concepts in the translation unit",
          "[type-model][constraints]") {
    const auto codes = [](const char* source) {
        auto result = std::vector<std::string>{};
        for (const auto& diagnostic : cppfrontier::translator{}.translate_source(
                 std::string{
                     "two: <T, U> concept = std::convertible_to<T, U>;\n"
                     "one: <T> concept = std::integral<T>;\n"
                     "many: <T, Us...> concept = true;\n"
                     "defaulted: <T, U: type = int> concept = true;\n"
                     "positive: <N: int> concept = N > 0;\n"
                     "X: type = { }\n"
                     "n: namespace = { c: <T> concept = true; }\n"} + source).diagnostics) {
            result.push_back(diagnostic.code);
        }
        return result;
    };
    using codes_t = std::vector<std::string>;
    CHECK(codes("f: (x: _ is two<int>) = { }\n").empty());
    CHECK(codes("f: (x: _ is many<int, long>) = { }\n").empty());
    CHECK(codes("f: (x: _ is defaulted) = { }\nf: (x: _ is defaulted<long>) = { }\n").empty());
    CHECK(codes("f: (x: _ is n::c) = { }\n").empty());
    CHECK(codes("f: <C: type> (x: _ is C) = { }\n").empty());
    // Concepts declared elsewhere are left to the Cpp1 compiler.
    CHECK(codes("f: (x: _ is std::integral) = { }\n").empty());

    CHECK(codes("f: (x: _ is two) = { }\n") == codes_t{"constraint-argument-count"});
    CHECK(codes("f: (x: _ is one<int>) = { }\n") == codes_t{"constraint-argument-count"});
    CHECK(codes("f: (x) = { y: _ is one<int> = x; }\n") == codes_t{"constraint-argument-count"});
    CHECK(codes("f: (x: _ is X) = { }\n") == codes_t{"constraint-not-concept"});
    CHECK(codes("f: (x: _ is positive) = { }\n") == codes_t{"constraint-not-type-concept"});
}

TEST_CASE("a constraint that is not a name is not a concept", "[hunt][constraints]") {
    const auto messages = [](const char* source, bool type_operators = false) {
        auto options = cppfrontier::translation_options{};
        if (type_operators) REQUIRE(options.experiments.enable("type-operators"));
        auto result = std::vector<std::string>{};
        for (const auto& diagnostic :
             cppfrontier::translator{}.translate_source(source, options).diagnostics) {
            if (diagnostic.code == "constraint-not-concept") result.push_back(diagnostic.message);
        }
        return result;
    };
    using messages_t = std::vector<std::string>;
    CHECK(messages("f: (x: _ is int) = { }\n") ==
          messages_t{"'int' is not a concept, so it cannot constrain '_'"});
    CHECK(messages("f: (x: _ is *i32) = { }\n") ==
          messages_t{"'*i32' is not a concept, so it cannot constrain '_'"});
    CHECK(messages("f: (x: _ is const std::integral) = { }\n").size() == 1);
    CHECK(messages("f: (x: _ is decltype(1)) = { }\n").size() == 1);
    CHECK(messages("f: (x: _ is (y: int) -> int) = { }\n").size() == 1);
    CHECK(messages("f: <T: std::integral | type> () = { }\n", true) ==
          messages_t{"'type' is not a concept, so it cannot constrain 'T'"});
    CHECK(messages("f: <T: double & std::copyable> () = { }\n", true).size() == 1);
    CHECK(messages("f: (x: _ is ::std::integral) = { }\ng: <N: int> () = { }\n").empty());
}

TEST_CASE("is and as before a global qualified name are operators", "[hunt][type-model]") {
    const auto cpp1 = translate(
        "f: (x: _ is ::std::integral) -> bool = x as ::std::size_t == 1u;\n"
        "g: (x: int) -> bool = x is ::std::int32_t;\n");
    CHECK(cpp1.find("auto f(::std::integral auto const& x) -> bool") != std::string::npos);
    CHECK(cpp1.find("cpp2::impl::as_<::std::size_t>(x)") != std::string::npos);
    CHECK(cpp1.find("cpp2::impl::is<::std::int32_t>(x)") != std::string::npos);
}

TEST_CASE("a template argument that is not a type-id is an expression", "[type-model][review]") {
    const auto cpp1 = translate(
        "a: std::array<int, (1 > 0) as int> = ();\n"
        "b: std::vector<*int> = ();\n");
    CHECK(cpp1.find("std::array<int, cpp2::impl::as_<int>((cpp2::impl::cmp_greater(1,0)))> a {};") != std::string::npos);
    CHECK(cpp1.find("std::vector<int*> b {};") != std::string::npos);
}

TEST_CASE("64-bit Cpp2 integer types are the exact-width Cpp1 types", "[type-model][review]") {
    const auto cpp1 = translate("x: i64 = 0;\ny: std::vector<u64> = ();\n");
    CHECK(cpp1.find("std::int64_t x {0};") != std::string::npos);
    CHECK(cpp1.find("std::vector<std::uint64_t> y {};") != std::string::npos);
}

TEST_CASE("a scope with dependent template arguments takes typename", "[type-model][review]") {
    const auto cpp1 = translate(
        "f: <T> (x: T) -> std::vector<T>::size_type = {\n"
        "    a: std::vector<T>::size_type = 0;\n"
        "    b: std::allocator_traits<T>::rebind_alloc<int> = ();\n"
        "    c: std::vector<int>::size_type = 0;\n"
        "    return a + c;\n"
        "}\n");
    CHECK(cpp1.find("-> typename std::vector<T>::size_type;") != std::string::npos);
    CHECK(cpp1.find("typename std::vector<T>::size_type a {0};") != std::string::npos);
    CHECK(cpp1.find("typename std::allocator_traits<T>::template rebind_alloc<int> b {};") !=
          std::string::npos);
    CHECK(cpp1.find("    std::vector<int>::size_type c {0};") != std::string::npos);
}

TEST_CASE("a parameter constraint without arguments or on a pack is valid Cpp1",
          "[type-model][constraints][review]") {
    const auto cpp1 = translate(
        "c: <T> concept = true;\n"
        "f: (x: _ is c<>) = { }\n"
        "g: (xs...: _ is c) = { }\n"
        "h: (forward xs...: _ is c) = { }\n");
    CHECK(cpp1.find("auto f(c<> auto const& x) -> void") != std::string::npos);
    CHECK(cpp1.find("auto g(c auto const& ...xs) -> void") != std::string::npos);
    CHECK(cpp1.find("requires ((c<CPP2_TYPEOF(xs)> && ...))") != std::string::npos);
}

// `C auto` deduces the value type, so it applies only where Cpp1 deduces one;
// `auto&&` would check `C<T&>`, so `move` and `forward` keep a requires clause.
TEST_CASE("a parameter constraint on a deduced value type orders overloads",
          "[type-model][constraints]") {
    const auto cpp1 = translate(
        "f: (x: _ is std::integral) -> int = 1;\n"
        "f: (x: _ is std::signed_integral) -> int = 2;\n"
        "a: (copy x: _ is std::integral) = { }\n"
        "b: (inout x: _ is std::integral) = { }\n"
        "c: (move x: _ is std::integral) = { }\n"
        "d: (forward x: _ is std::integral) = { }\n");
    CHECK(cpp1.find("auto f(std::integral auto const& x) -> int;") != std::string::npos);
    CHECK(cpp1.find("auto f(std::signed_integral auto const& x) -> int;") != std::string::npos);
    CHECK(cpp1.find("auto a(std::integral auto x) -> void;") != std::string::npos);
    CHECK(cpp1.find("auto b(std::integral auto& x) -> void;") != std::string::npos);
    CHECK(cpp1.find("auto c(auto&& x) -> void requires (std::integral<CPP2_TYPEOF(x)> && "
                    "!std::is_lvalue_reference_v<decltype(x)>);") != std::string::npos);
    CHECK(cpp1.find("auto d(auto&& x) -> void requires (std::integral<CPP2_TYPEOF(x)>);") !=
          std::string::npos);
}

TEST_CASE("a parameter whose type is a pack is diagnosed", "[type-model][review]") {
    const auto result = cppfrontier::translator{}.translate_source(
        "f: <Ts...: type> (xs: Ts...) = { }\n");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "parameter-pack-type");
    CHECK(result.diagnostics.front().message == "a parameter pack is declared as 'xs...: Ts'");
}

TEST_CASE("a member of a decltype scope takes typename", "[type-model][review]") {
    const auto cpp1 = translate("f: (x) = { y: decltype(x)::value_type = 0; z: type_of(x) = x; }\n");
    CHECK(cpp1.find("typename decltype(x)::value_type y {0};") != std::string::npos);
    CHECK(cpp1.find("CPP2_TYPEOF(x) z {x};") != std::string::npos);
}

TEST_CASE("a function type can return forward_ref _", "[type-model][review]") {
    const auto cpp1 = translate("k: (fp: *(copy x: std::string) -> forward_ref _) = { }\n");
    CHECK(cpp1.find("auto k(auto&&(*fp)(std::string x)) -> void;") != std::string::npos);
}

TEST_CASE("a scope naming a non-type template parameter takes typename", "[type-model][review]") {
    const auto cpp1 = translate(
        "f: <N: int> () -> int = { i: std::array<int, N>::size_type = N; return i as int; }\n");
    CHECK(cpp1.find("typename std::array<int, N>::size_type i {N};") != std::string::npos);
}

TEST_CASE("a const pointer to a function keeps its const", "[type-model][review]") {
    CHECK(cppfrontier::render_cpp1_type_declaration(parse_type("const * (x: i32) -> i32"), "p") ==
          "int(* const p)(cpp2::impl::in<int> x)");
    CHECK(cppfrontier::render_cpp1_type_declaration(parse_type("* const * (x: i32) -> i32"), "p") ==
          "int(* const*p)(cpp2::impl::in<int> x)");
    CHECK(cppfrontier::render_cpp1_type(parse_type("* const (x: i32) -> i32")) ==
          "int(*)(cpp2::impl::in<int> x)");
}

TEST_CASE("the qualifiers of a pack apply to each element", "[type-model][review]") {
    CHECK(cppfrontier::render_cpp1_type(parse_type("std::tuple<const Ts...>")) ==
          "std::tuple<Ts const ...>");
    CHECK(cppfrontier::render_cpp1_type(parse_type("std::tuple<* const Ts...>")) ==
          "std::tuple<Ts const*...>");
}

TEST_CASE("type_of in a template argument is CPP2_TYPEOF", "[type-model][review]") {
    const auto cpp1 = translate(
        "main: () = { v: std::vector<int> = (1); w: std::vector<type_of(v)::value_type> = (); }\n");
    CHECK(cpp1.find("std::vector<CPP2_TYPEOF(v)::value_type> w {};") != std::string::npos);
}

TEST_CASE("a function type returning a function pointer nests its declarator",
          "[type-model][review]") {
    CHECK(cppfrontier::render_cpp1_type_declaration(
              parse_type("*(n: i32) -> *(x: i32) -> i32"), "f") ==
          "int(*(*f)(cpp2::impl::in<int> n))(cpp2::impl::in<int> x)");
}

TEST_CASE("a function expression in a template names its dependent types with typename",
          "[type-model][review]") {
    const auto cpp1 = translate(
        "f: <T> (v: T) -> int = {\n"
        "    g := :(x: T::value_type) -> int = x * 2;\n"
        "    h := :() -> T::value_type = 3;\n"
        "    return g(v.front()) + h();\n"
        "}\n");
    CHECK(cpp1.find("[](cpp2::impl::in<typename T::value_type> x) -> int") != std::string::npos);
    CHECK(cpp1.find("[]() -> typename T::value_type") != std::string::npos);
}

TEST_CASE("the fundamental aliases are the documented ones", "[type-model][review]") {
    // common.md "Fundamental data types"; `f32` and `f64` are ordinary names.
    const auto cpp1 = translate(
        "f64: type == double;\n"
        "main: () = {\n"
        "    a: uint = 3u;\n"
        "    b := :std::vector<uint> = (1u);\n"
        "    c := sizeof(uint);\n"
        "    d := std::vector<f64>(2);\n"
        "}\n");
    CHECK(cpp1.find("unsigned int a {3u};") != std::string::npos);
    CHECK(cpp1.find("std::vector<cpp2::uint>{1u}") != std::string::npos);
    CHECK(cpp1.find("sizeof(cpp2::uint)") != std::string::npos);
    CHECK(cpp1.find("std::vector<f64>(2)") != std::string::npos);
    CHECK(cpp1.find("using f64 = double;") != std::string::npos);
}

TEST_CASE("a function type's parameter is a name with a type", "[type-model][review]") {
    for (const auto* parameter : {"int", "x", "forward a: int", "this, x: int", "x: int = 3",
                                  "args...: int"}) {
        INFO(parameter);
        const auto result = cppfrontier::translator{}.translate_source(
            std::string{"sq: (x: int) -> int = x * x;\np: *("} + parameter + ") -> int = sq&;\n");
        CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& issue) { return issue.code == "function-type-parameter"; }));
    }
    const auto valid = cppfrontier::translator{}.translate_source(
        "sq: (inout x: int) -> int = x * x;\np: *(inout x: int) -> int = sq&;\n");
    CHECK_FALSE(valid.has_errors());
}

TEST_CASE("'long double' suggests 'longdouble'", "[type-model][review]") {
    const auto result =
        cppfrontier::translator{}.translate_source("x: long double = 1.0;\n");
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics.front().message.ends_with("did you mean 'longdouble'?"));
}

TEST_CASE("a member template of a type-marked scope takes template", "[type-model][hunt]") {
    const auto cpp1 = translate(
        "f: <T> (t: T) = {\n"
        "    a := sizeof(type T::y<int>);\n"
        "    b := (type T::y<int>)();\n"
        "    c: std::vector<type T::y<int>> = ();\n"
        "    d := std::is_same_v<type T::y<int>, int>;\n"
        "    e := std::is_same_v<type T::y, int>;\n"
        "}\n");
    CHECK(cpp1.find("sizeof(typename T::template y<int>)") != std::string::npos);
    CHECK(cpp1.find("auto b {typename T::template y<int>()};") != std::string::npos);
    CHECK(cpp1.find("std::vector<typename T::template y<int>> c {};") != std::string::npos);
    CHECK(cpp1.find("std::is_same_v<typename T::template y<int>, int>") != std::string::npos);
    CHECK(cpp1.find("std::is_same_v<typename T::y, int>") != std::string::npos);
}

TEST_CASE("a type requirement and a concept body name dependent member templates",
          "[type-model][hunt]") {
    const auto cpp1 = translate(
        "c: <T> concept = requires { typename T::y<int>; } && sizeof(type T::y<int>) > 0u;\n"
        "d: <T> (t: T) -> bool = requires { typename T::y<int>; typename std::vector<T>; };\n"
        "e: <T> (t: T) = { m: std::map<int, std::vector<type T::y<int>>> = (); }\n");
    CHECK(cpp1.find("concept c = requires { typename T::template y<int>; } && "
                    "cpp2::impl::cmp_greater(sizeof(typename T::template y<int>),0u);") !=
          std::string::npos);
    CHECK(cpp1.find("requires { typename T::template y<int>; typename std::vector<T>; }") !=
          std::string::npos);
    CHECK(cpp1.find("std::map<int, std::vector<typename T::template y<int>>> m {};") !=
          std::string::npos);
}
