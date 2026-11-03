#include <catch2/catch_all.hpp>
#include "../../src/cpp1_expression_renderer.hpp"

#include <cppfrontier/expression.hpp>

#include <string>
#include <vector>

TEST_CASE("expression renderer applies text lowering in a fixed order", "[emitter][expressions]") {
    const auto text = cppfrontier::render_cpp1_expression("v[i](p*) = s.ssize();");
    CHECK(text == "CPP2_ASSERT_IN_BOUNDS(v, i)(*cpp2::impl::assert_not_null(p)) = CPP2_UFCS(ssize)(s);");
}

TEST_CASE("expression renderer leaves identifiers containing ssize intact", "[emitter][expressions]") {
    const auto text = cppfrontier::render_cpp1_expression("possessive = s . ssize ( );");
    CHECK(text == "possessive = CPP2_UFCS(ssize)(s);");
}

TEST_CASE("expression renderer handles nested pointer suffixes and indexed calls", "[emitter][expressions]") {
    const auto text = cppfrontier::render_cpp1_expression("(p*)[i](x*)");
    CHECK(text == "CPP2_ASSERT_IN_BOUNDS((*cpp2::impl::assert_not_null(p)), i)(*cpp2::impl::assert_not_null(x))");
}

TEST_CASE("expression renderer checks indexing after a dereference", "[emitter][expressions]") {
    const auto text = cppfrontier::render_cpp1_expression("(p*)[i]");
    CHECK(text == "CPP2_ASSERT_IN_BOUNDS((*cpp2::impl::assert_not_null(p)), i)");
}

TEST_CASE("allocation rendering respects nested template arguments", "[emitter][expressions][templates]") {
    CHECK(cppfrontier::render_cpp1_expression("new<std::vector<int>>(1, 2)") ==
          "cpp2_new<std::vector<int>>(1, 2)");
    CHECK(cppfrontier::render_cpp1_expression("std::vector<std::vector<int>>()") ==
          "std::vector<std::vector<int>>()");
}

TEST_CASE("allocator-qualified allocation keeps its allocator", "[emitter][expressions][allocation]") {
    CHECK(cppfrontier::render_cpp1_expression("unique.new<widget>(1, 2)") ==
          "CPP2_UFCS_TEMPLATE(cpp2_new<widget>)(cpp2::unique, 1, 2)");
    CHECK(cppfrontier::render_cpp1_expression("cpp2::shared.new<widget>()") ==
          "CPP2_UFCS_TEMPLATE(cpp2_new<widget>)(cpp2::shared)");
    CHECK(cppfrontier::render_cpp1_expression("allocator.new<std::vector<int>>(3)") ==
          "CPP2_UFCS_TEMPLATE(cpp2_new<std::vector<int>>)(allocator, 3)");
    CHECK(cppfrontier::render_cpp1_expression("unique . new<int>(1)") ==
          "CPP2_UFCS_TEMPLATE(cpp2_new<int>)(cpp2::unique, 1)");
}

TEST_CASE("multiplying an operand is distinct from postfix dereference", "[emitter][expressions][pointers]") {
    CHECK(cppfrontier::render_cpp1_expression("a * b") == "a * b");
    CHECK(cppfrontier::render_cpp1_expression("a*b") == "a*b");
    CHECK(cppfrontier::render_cpp1_expression("p*.field") == "(*cpp2::impl::assert_not_null(p)).field");
    CHECK(cppfrontier::render_cpp1_expression("p*[0]") == "CPP2_ASSERT_IN_BOUNDS_LITERAL((*cpp2::impl::assert_not_null(p)), 0)");
}

TEST_CASE("postfix * and & do not depend on spacing", "[emitter][expressions][pointers]") {
    CHECK(cppfrontier::render_cpp1_expression("p* -1") == cppfrontier::render_cpp1_expression("p * -1"));
    CHECK(cppfrontier::render_cpp1_expression("a& -1") == cppfrontier::render_cpp1_expression("a & -1"));
    CHECK(cppfrontier::render_cpp1_expression("a * (-1)") == "a * (-1)");
    CHECK(cppfrontier::render_cpp1_expression("a * !b") == "a * !b");
    CHECK(cppfrontier::render_cpp1_expression("a* !b") == "a* !b");
    CHECK(cppfrontier::render_cpp1_expression("p * -1").starts_with("*cpp2::impl::assert_not_null(p)"));
}

TEST_CASE("not followed by whitespace is !", "[emitter][expressions][names]") {
    CHECK(cppfrontier::render_cpp1_expression("not b && not(c)") == "! b && cpp2_not(c)");
}

TEST_CASE("postfix address-of lowers to prefix Cpp1 address-of",
          "[emitter][expressions][address-of]") {
    CHECK(cppfrontier::render_cpp1_expression("read(value&)") == "read(&(value))");
    CHECK(cppfrontier::render_cpp1_expression("value&") == "&(value)");
    CHECK(cppfrontier::render_cpp1_expression("object.member&") == "&(object.member)");
    CHECK(cppfrontier::render_cpp1_expression("call(value)&") == "&(call(value))");
    CHECK(cppfrontier::render_cpp1_expression("(p*)&") == "&((*cpp2::impl::assert_not_null(p)))");
    CHECK(cppfrontier::render_cpp1_expression("value.position&") == "&(value.position)");
}

TEST_CASE("binary and compound ampersands are not address-of suffixes",
          "[emitter][expressions][address-of]") {
    CHECK(cppfrontier::render_cpp1_expression("left & right") == "left & right");
    CHECK(cppfrontier::render_cpp1_expression("value &= 3") == "value &= 3");
    CHECK(cppfrontier::render_cpp1_expression("left && right") == "left && right");
}

TEST_CASE("as expressions retain spaced template types", "[emitter][expressions][templates]") {
    CHECK(cppfrontier::render_cpp1_expression("v as X< 1>") == "cpp2::impl::as_<X<1>>(v)");
    CHECK(cppfrontier::render_cpp1_expression("(v as std::vector<X< 1>>).size()") ==
          "CPP2_UFCS(size)((cpp2::impl::as_<std::vector<X<1>>>(v)))");
}

TEST_CASE("expression rewrites leave string contents untouched", "[emitter][expressions][literals]") {
    const auto expression = std::string{
        "std::cout << \"new<int>(x) p* s.ssize() a as T v[i](x) (:int=1)\""};
    CHECK(cppfrontier::render_cpp1_expression(expression) == expression);
}

TEST_CASE("expression rewrites leave escaped and character literals untouched",
          "[emitter][expressions][literals]") {
    CHECK(cppfrontier::render_cpp1_expression("\"escaped \\\" new<int> p*\"") ==
          "\"escaped \\\" new<int> p*\"");
    CHECK(cppfrontier::render_cpp1_expression("'n'") == "'n'");
    CHECK(cppfrontier::render_cpp1_expression("'\\''") == "'\\''");
}

TEST_CASE("expression rewrites leave raw strings untouched", "[emitter][expressions][literals][raw]") {
    const auto expression = std::string{
        "R\"tag(new<int>(x) p* s.ssize() a as T v[i](x) (:int=1))tag\""};
    CHECK(cppfrontier::render_cpp1_expression(expression) == expression);
}

TEST_CASE("expression rewrites leave comments untouched", "[emitter][expressions][comments]") {
    const auto expression = std::string{"value /* new<int> p* ssize as T */"};
    CHECK(cppfrontier::render_cpp1_expression(expression) == expression);
}

TEST_CASE("empty call arguments become default-initialized values",
          "[emitter][expressions][initializers]") {
    CHECK(cppfrontier::render_cpp1_expression("f((), x)") == "f({}, x)");
    CHECK(cppfrontier::render_cpp1_expression("f(x, ())") == "f(x, {})");
    CHECK(cppfrontier::render_cpp1_expression("T = ()") == "T = {}");
}

TEST_CASE("ordinary empty calls and repeated calls are unchanged",
          "[emitter][expressions][initializers]") {
    CHECK(cppfrontier::render_cpp1_expression("f()") == "f()");
    CHECK(cppfrontier::render_cpp1_expression("f()()") == "f()()");
    CHECK(cppfrontier::render_cpp1_expression("std::mt19937()()") == "std::mt19937()()");
}

TEST_CASE("Cpp2 move expressions lower to std::move",
          "[emitter][expressions][move]") {
    CHECK(cppfrontier::render_cpp1_expression("(move x)") == "(std::move(x))");
    CHECK(cppfrontier::render_cpp1_expression("f(move x, y)") == "f(std::move(x), y)");
    CHECK(cppfrontier::render_cpp1_expression("f(move object.member)") ==
          "f(std::move(object.member))");
    CHECK(cppfrontier::render_cpp1_expression("f(move values[0])") ==
          "f(std::move(CPP2_ASSERT_IN_BOUNDS_LITERAL(values, 0)))");
    CHECK(cppfrontier::render_cpp1_expression("f(move g(x), y)") ==
          "f(std::move(g(x)), y)");
    CHECK(cppfrontier::render_cpp1_expression("f(move x + 1)") ==
          "f(std::move(x + 1))");
    CHECK(cppfrontier::render_cpp1_expression("f(\"move x\", move y)") ==
          "f(\"move x\", std::move(y))");
    CHECK(cppfrontier::render_cpp1_expression("std::move(x)") == "std::move(x)");
    CHECK(cppfrontier::render_cpp1_expression("object.move(x)") == "CPP2_UFCS(move)(object, x)");
    CHECK(cppfrontier::render_cpp1_expression("move_name + move(x)") ==
          "move_name + move(x)");
}

TEST_CASE("ordinary string interpolation lowers to cpp2::to_string",
          "[emitter][expressions][interpolation]") {
    CHECK(cppfrontier::render_cpp1_expression("\"a (x)$ b\"") ==
          "\"a \" + cpp2::to_string(x) + \" b\"");
    CHECK(cppfrontier::render_cpp1_expression("\"(x)$\"") ==
          "\"\" + cpp2::to_string(x) + \"\"");
    CHECK(cppfrontier::render_cpp1_expression("\"(x)$(y)$\"") ==
          "\"\" + cpp2::to_string(x) + cpp2::to_string(y) + \"\"");
    CHECK(cppfrontier::render_cpp1_expression("\"(value:04d)$\"") ==
          "\"\" + cpp2::to_string(value,\"{:04d}\") + \"\"");
    CHECK(cppfrontier::render_cpp1_expression("\"(value:)$\"") ==
          "\"\" + cpp2::to_string(value,\"{:}\") + \"\"");
    CHECK(cppfrontier::render_cpp1_expression("\"(std::pair<int, int>(1, 2):)$\"") ==
          "\"\" + cpp2::to_string(std::pair<int, int>(1, 2),\"{:}\") + \"\"");
    CHECK(cppfrontier::render_cpp1_expression("\"(ns::value)$\"") ==
          "\"\" + cpp2::to_string(ns::value) + \"\"");
    CHECK(cppfrontier::render_cpp1_expression("\"((x + (1)))$\"") ==
          "\"\" + cpp2::to_string((x + (1))) + \"\"");
    CHECK(cppfrontier::render_cpp1_expression("\"cost $5: (x)$\"") ==
          "\"cost $5: \" + cpp2::to_string(x) + \"\"");
    CHECK(cppfrontier::render_cpp1_expression("\"\\\"(x)$\\\"\"") ==
          "\"\\\"\" + cpp2::to_string(x) + \"\\\"\"");
    const auto raw_text = std::string{R"cpp2(R"(a (x)$)")cpp2"};
    CHECK(cppfrontier::render_cpp1_expression(raw_text) == raw_text);
    CHECK(cppfrontier::render_cpp1_expression("\"plain\"") == "\"plain\"");
}

TEST_CASE("an interpolation's format specification follows no colon inside a literal",
          "[emitter][expressions][interpolation][hunt]") {
    CHECK(cppfrontier::render_cpp1_expression("\"(\\\"a:b\\\")$\"") ==
          "\"\" + cpp2::to_string(\"a:b\") + \"\"");
    CHECK(cppfrontier::render_cpp1_expression("\"(f(':', 1'000):>4)$\"") ==
          "\"\" + cpp2::to_string(f(':', 1'000),\"{:>4}\") + \"\"");
    CHECK(cppfrontier::render_cpp1_expression(R"cpp2($R"(a ("x:y" + s)$)")cpp2") ==
          R"cpp2(R"(a )" + cpp2::to_string("x:y" + s))cpp2");
}

TEST_CASE("integer division and modulo operands receive runtime checks",
          "[emitter][expressions][division][safety]") {
    CHECK(cppfrontier::render_cpp1_expression("left / right") ==
          "left / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(left),right)");
    CHECK(cppfrontier::render_cpp1_expression("left /= right") ==
          "left /= CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(left),right)");
    CHECK(cppfrontier::render_cpp1_expression("left % 2") ==
          "left % CPP2_ASSERT_NOT_ZERO_LITERAL(CPP2_TYPEOF(left),2)");
    CHECK(cppfrontier::render_cpp1_expression("left / 2.5") ==
          "left / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(left),2.5)");
    CHECK(cppfrontier::render_cpp1_expression("left %= right") ==
          "left %= CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(left),right)");
}

TEST_CASE("division checks use the immediate numerator and denominator terms",
          "[emitter][expressions][division][precedence]") {
    CHECK(cppfrontier::render_cpp1_expression("a * b / c") ==
          "a * b / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(b),c)");
    CHECK(cppfrontier::render_cpp1_expression("a / b / c") ==
          "a / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(a),b) / "
          "CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(b),c)");
    CHECK(cppfrontier::render_cpp1_expression("f(a) / (b + c)") ==
          "f(a) / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(f(a)),(b + c))");
}

TEST_CASE("division checks can be disabled", "[emitter][expressions][division][options]") {
    const auto options = cppfrontier::cpp1_expression_options{false};
    CHECK(cppfrontier::render_cpp1_expression("left / right", options) == "left / right");
    CHECK(cppfrontier::render_cpp1_expression("left % right", options) == "left % right");
}

TEST_CASE("division checks leave literals and comments opaque",
          "[emitter][expressions][division][literals]") {
    const auto expression = std::string{"\"a / b\" /* c % d */"};
    CHECK(cppfrontier::render_cpp1_expression(expression) == expression);
}

TEST_CASE("division checks stay within their argument expression",
          "[emitter][expressions][division][calls]") {
    CHECK(cppfrontier::render_cpp1_expression("f(a / b, c / d)") ==
          "f(a / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(a),b), "
          "c / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(c),d))");
    CHECK(cppfrontier::render_cpp1_expression("a /* note */ / b") ==
          "a /* note */ / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(a),b)");
    CHECK(cppfrontier::render_cpp1_expression("a / b.member") ==
          "a / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(a),b.member)");
    CHECK(cppfrontier::render_cpp1_expression("a / b[0]") ==
          "a / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(a),CPP2_ASSERT_IN_BOUNDS_LITERAL(b, 0))");
}

TEST_CASE("unchecked utility names receive their support namespace",
          "[emitter][expressions][division][unchecked]") {
    CHECK(cppfrontier::render_cpp1_expression("unchecked_div(a, b)") ==
          "cpp2::unchecked_div(a, b)");
    CHECK(cppfrontier::render_cpp1_expression("unchecked_div(a / b, c % d)") ==
          "cpp2::unchecked_div(a / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(a),b), "
          "c % CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(c),d))");
    CHECK(cppfrontier::render_cpp1_expression("cpp2::unchecked_div(a, b)") ==
          "cpp2::unchecked_div(a, b)");
    CHECK(cppfrontier::render_cpp1_expression("unchecked_div(a, b) / c") ==
          "cpp2::unchecked_div(a, b) / "
          "CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(cpp2::unchecked_div(a, b)),c)");
    CHECK(cppfrontier::render_cpp1_expression("f(unchecked_div(a, b), c / d)") ==
          "f(cpp2::unchecked_div(a, b), c / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(c),d))");
    CHECK(cppfrontier::render_cpp1_expression("object.unchecked_div(a, b)") ==
          "cpp2::unchecked_div(object, a, b)");
    CHECK(cppfrontier::render_cpp1_expression("not_unchecked_div(a, b)") ==
          "not_unchecked_div(a, b)");
}

TEST_CASE("explicit this receiver access lowers to a dereferenced pointer",
          "[emitter][expressions][this]") {
    CHECK(cppfrontier::render_cpp1_expression("this.value") == "(*this).value");
    CHECK(cppfrontier::render_cpp1_expression("this.get()") == "CPP2_UFCS(get)((*this))");
    CHECK(cppfrontier::render_cpp1_expression("f(this)") == "f((*this))");
    CHECK(cppfrontier::render_cpp1_expression("object.this") == "object.this");
    CHECK(cppfrontier::render_cpp1_expression("\"this.value\"") == "\"this.value\"");
}

TEST_CASE("range expressions lower to the Cpp2 range helper",
          "[emitter][expressions][range]") {
    CHECK(cppfrontier::render_cpp1_expression("0 ..< 3") == "cpp2::range(0,3)");
    CHECK(cppfrontier::render_cpp1_expression("0 ..= 3") == "cpp2::range(0,3,true)");
    CHECK(cppfrontier::render_cpp1_expression("-2 ..< 2") == "cpp2::range(-2,2)");
    CHECK(cppfrontier::render_cpp1_expression("first ..< last") ==
          "cpp2::range(first,last)");
    CHECK(cppfrontier::render_cpp1_expression("(1 ..= 20).sum()") ==
          "CPP2_UFCS(sum)((cpp2::range(1,20,true)))");
    CHECK(cppfrontier::render_cpp1_expression("\"((1 ..= 20).sum())$\"") ==
          "\"\" + cpp2::to_string(CPP2_UFCS(sum)((cpp2::range(1,20,true)))) + \"\"");
}

TEST_CASE("out parameters construct and explicit out arguments take addresses",
          "[emitter][expressions][out]") {
    const auto options = cppfrontier::cpp1_expression_options{
        .wrapped_parameters = {{"value", {}, true}}};
    CHECK(cppfrontier::render_cpp1_expression("value = 1;", options) ==
          "value.construct(1);");
    auto repeated = options;
    repeated.wrapped_parameters.front().out = false;
    CHECK(cppfrontier::render_cpp1_expression("value = 1; value = 2;", repeated) ==
          "value.value() = 1; value.value() = 2;");
    CHECK(cppfrontier::render_cpp1_expression("return value;", options) ==
          "return value.value();");
    CHECK(cppfrontier::render_cpp1_expression("std::cout << value;", options) ==
          "std::cout << value.value();");
    CHECK(cppfrontier::render_cpp1_expression("value = f(1, 2) + 3;", options) ==
          "value.construct(f(1, 2) + 3);");
    CHECK(cppfrontier::render_cpp1_expression("value += 1;", options) ==
          "value.value() += 1;");
    CHECK(cppfrontier::render_cpp1_expression("value.member", options) ==
          "value.value().member");
    CHECK(cppfrontier::render_cpp1_expression("value.member = 1;", options) ==
          "value.value().member = 1;");
    CHECK(cppfrontier::render_cpp1_expression("value = value + 2;", options) ==
          "value.construct(value.value() + 2);");
    CHECK(cppfrontier::render_cpp1_expression("value = 1; value = 2;", options) ==
          "value.construct(1); value.construct(2);");
    CHECK(cppfrontier::render_cpp1_expression("f(out x, out y)") ==
          "f(cpp2::impl::out(&x), cpp2::impl::out(&y))");
}

TEST_CASE("deferred initialization keeps typed construction in the caller",
          "[emitter][expressions][initializers]") {
    const auto constructions = std::vector<cppfrontier::construction>{{0, "small"}};
    const auto options = cppfrontier::cpp1_expression_options{
        .wrapped_parameters = {{"small", "unsigned char", false}},
        .constructions = &constructions};
    CHECK(cppfrontier::render_cpp1_expression("small = 42;", options) ==
          "small.construct(std::type_identity_t<unsigned char>{42});");
    CHECK(cppfrontier::render_cpp1_expression("return small;", options) ==
          "return small.value();");
}

TEST_CASE("initializer-list assignments preserve brace construction",
          "[emitter][expressions][initializers][upstream-1446]") {
    auto options = cppfrontier::cpp1_expression_options{};
    options.wrapped_parameters = {{"value", "std::array<signed char, 3>", false}};
    const auto constructions = std::vector<cppfrontier::construction>{{0, "value"}};
    options.constructions = &constructions;
    CHECK(cppfrontier::render_cpp1_expression("value = (1, 2, 3);", options) ==
          "value.construct_from([&]{ return std::type_identity_t<std::array<signed char, 3>>{1, "
          "2, 3}; });");
    options.constructions = nullptr;
    CHECK(cppfrontier::render_cpp1_expression("value = (4, 5, 6);", options) ==
          "value.value() = std::type_identity_t<std::array<signed char, 3>>{4, 5, 6};");
}

TEST_CASE("function types render with declarator placement", "[emitter][types][functions]") {
    CHECK(cppfrontier::render_cpp1_type(
              "*( _: int ) -> void") ==
          "void(*)([[maybe_unused]] cpp2::impl::in<int> unnamed_param_1)");
    CHECK(cppfrontier::render_cpp1_type_declaration(
              "*( _: int ) -> void", "intfuncptr") ==
          "void(*intfuncptr)([[maybe_unused]] cpp2::impl::in<int> unnamed_param_1)");
    CHECK(cppfrontier::render_cpp1_type_declaration(
              "*(i: i32) -> std::string", "pf2") ==
          "std::string(*pf2)(cpp2::impl::in<int> i)");
    CHECK(cppfrontier::render_cpp1_type(
              "std::function< (i: int) -> int >") ==
          "std::function<int(cpp2::impl::in<int> i)>");
}

TEST_CASE("type qualifiers retain left-to-right order", "[emitter][types][qualifiers]") {
    CHECK(cppfrontier::render_cpp1_type_declaration("const * * const i32", "p") ==
          "int const** const p");
    CHECK(cppfrontier::render_cpp1_type_declaration("*i32", "p") == "int* p");
}

TEST_CASE("dependent type template arguments use typename", "[emitter][types][templates][upstream-727]") {
    CHECK(cppfrontier::render_cpp1_type("std::vector<type T::value_type>") ==
          "std::vector<typename T::value_type>");
    CHECK(cppfrontier::render_cpp1_type(
              "std::pair<type T::first_type, type T::second_type>") ==
          "std::pair<typename T::first_type, typename T::second_type>");
    CHECK(cppfrontier::render_cpp1_type("std::vector<T::value_type>") ==
          "std::vector<T::value_type>");
    CHECK(cppfrontier::render_cpp1_type("cpp2::impl::in<type T::value_type>") ==
          "cpp2::impl::in<typename T::value_type>");
    CHECK(cppfrontier::render_cpp1_type_declaration("type T::value_type", "value") ==
          "typename T::value_type value");
    CHECK(cppfrontier::render_cpp1_type("std::vector<type_name>") ==
          "std::vector<type_name>");
    CHECK(cppfrontier::render_cpp1_type("std::common_type<T>::type") ==
          "std::common_type<T>::type");
}

TEST_CASE("dependent type template arguments use typename in expressions",
          "[emitter][expressions][templates][upstream-727]") {
    CHECK(cppfrontier::render_cpp1_expression("std::vector<type T::value_type>()") ==
          "std::vector<typename T::value_type>()");
    CHECK(cppfrontier::render_cpp1_expression("f<int, type T::size_type>(type)") ==
          "f<int, typename T::size_type>(type)");
    CHECK(cppfrontier::render_cpp1_expression("x.type + type") == "x.type + type");
}

TEST_CASE("typed values lower inside template argument lists",
          "[emitter][expressions][templates][upstream-790]") {
    CHECK(cppfrontier::render_cpp1_expression(
              "f(std::integral_constant<int, :int = 0>());") ==
          "f(std::integral_constant<int, int{0}>());");
    CHECK(cppfrontier::render_cpp1_expression("a < b, c = d > e") ==
          "cpp2::impl::cmp_less(a,b), c = cpp2::impl::cmp_greater(d,e)");
}

TEST_CASE("a joined '>=' or '>>=' is an operator, never a template list's close",
          "[emitter][expressions][templates]") {
    CHECK(cppfrontier::render_cpp1_expression("f(a < b, c >= d)") ==
          "f(cpp2::impl::cmp_less(a,b), cpp2::impl::cmp_greater_eq(c,d))");
    CHECK(cppfrontier::render_cpp1_expression("std::is_same_v<int, long> >= 0") ==
          "cpp2::impl::cmp_greater_eq(std::is_same_v<int, long>,0)");
    CHECK(cppfrontier::render_cpp1_expression("a<b>>=c").find("a<b>") == std::string::npos);
}

TEST_CASE("declarator rendering preserves a pre-escaped name", "[emitter][types][names]") {
    CHECK(cppfrontier::render_cpp1_type_declaration("*i32", "cpp2_class") ==
          "int* cpp2_class");
}

TEST_CASE("expression tree bounds operands by precedence",
          "[emitter][expressions][tree]") {
    CHECK(cppfrontier::render_cpp1_expression("assert( 10 as i32 == 10 );") ==
          "assert( cpp2::impl::as_<cpp2::i32, 10>() == 10 );");
    CHECK(cppfrontier::render_cpp1_expression("x as std::string + y") ==
          "cpp2::impl::as_<std::string>(x) + y");
    CHECK(cppfrontier::render_cpp1_expression("a.f*.g()") == "CPP2_UFCS(g)((*cpp2::impl::assert_not_null(a.f)))");
    CHECK(cppfrontier::render_cpp1_expression("f(s* is Shape)") ==
          "f(cpp2::impl::is<Shape>(*cpp2::impl::assert_not_null(s)))");
    CHECK(cppfrontier::render_cpp1_expression("0 ..< n - 1") == "cpp2::range(0,n - 1)");
}

TEST_CASE("typed values lower only in expression position",
          "[emitter][expressions][tree][initializers]") {
    CHECK(cppfrontier::render_cpp1_expression("assert( v == :vec = (2, 3) );") ==
          "assert( v == vec{2, 3} );");
    CHECK(cppfrontier::render_cpp1_expression("v: std::vector<int> = (3, 1, 2)") ==
          "v: std::vector<int> = (3, 1, 2)");
    CHECK(cppfrontier::render_cpp1_expression("v is (:(x) = x>3;)") ==
          "v is (:(x) = cpp2::impl::cmp_greater(x,3);)");
}

TEST_CASE("postfix increments lower to Cpp1 prefix operators",
          "[emitter][expressions][tree]") {
    CHECK(cppfrontier::render_cpp1_expression("i++;") == "++i;");
    CHECK(cppfrontier::render_cpp1_expression("p*++;") == "++*cpp2::impl::assert_not_null(p);");
    CHECK(cppfrontier::render_cpp1_expression("x = i--;") == "x = --i;");
    CHECK(cppfrontier::render_cpp1_expression("i++.f()") == "CPP2_UFCS(f)((++i))");
}

TEST_CASE("UFCS template calls are recognized from the parsed name", "[review][ufcs]") {
    using cppfrontier::render_cpp1_expression;
    CHECK(render_cpp1_expression("a.operator<(b)") == "CPP2_UFCS(operator<)(a, b)");
    CHECK(render_cpp1_expression("a.operator<=(b)") == "CPP2_UFCS(operator<=)(a, b)");
    CHECK(render_cpp1_expression("a.f<>()") == "CPP2_UFCS_TEMPLATE(f<>)(a)");
    CHECK(render_cpp1_expression("a.N::M::f<*int, (x::y)>()") ==
          "N::M::f<int*, (x::y)>(a)");
    CHECK(render_cpp1_expression("a.::N::f<int>()") == "::N::f<int>(a)");
    CHECK(render_cpp1_expression("a.N::f(b)") == "N::f(a, b)");
    CHECK(render_cpp1_expression("a.::f()") == "::f(a)");
    CHECK(render_cpp1_expression("a.f<N::T>()") == "CPP2_UFCS_TEMPLATE(f<N::T>)(a)");
    CHECK(render_cpp1_expression("a.unique.new<int>(1)") ==
          "CPP2_UFCS_TEMPLATE(cpp2_new<int>)(a.unique, 1)");
}

namespace {

// The tokens of `node`, without whitespace.
auto spelling(const cppfrontier::expression_text& text, const cppfrontier::expression& node)
    -> std::string {
    auto result = std::string{};
    for (auto index = node.first; index < node.last; ++index) result += text.tokens[index].text;
    return result;
}

}

TEST_CASE("folds are parsed with their operands", "[emitter][expressions][folds]") {
    using cppfrontier::expression_kind;
    const auto operands = [](std::string_view source) {
        const auto text = cppfrontier::parse_expression_text(source);
        auto result = std::vector<std::string>{};
        const auto& root = text.root;
        if (root.children.size() != 1 || root.children.front().kind != expression_kind::fold) {
            return result;
        }
        const auto& fold = root.children.front();
        CHECK(text.tokens[fold.op].text == "...");
        for (const auto& child : fold.children) result.push_back(spelling(text, child));
        return result;
    };
    CHECK(operands("(... * xs)") == std::vector<std::string>{"xs"});
    // A `*` or `&` before `...` is postfix: a dereferencing expansion, not a fold.
    CHECK(operands("(xs * ...)").empty());
    CHECK(operands("(xs& ...)").empty());
    CHECK(operands("(xs* + ...)") == std::vector<std::string>{"xs*"});
    CHECK(operands("(... & xs)") == std::vector<std::string>{"xs"});
    CHECK(operands("(1 << ... << xs.f())") == std::vector<std::string>{"1", "xs.f()"});
    CHECK(operands("(x >> ... >> ys)") == std::vector<std::string>{"x", "ys"});
    CHECK(operands("(xs is int && ...)") == std::vector<std::string>{"xsisint"});
    CHECK(operands("((f(xs), g()), ...)") == std::vector<std::string>{"(f(xs),g())"});
    CHECK(operands("(a + b + ...)").empty());
    CHECK(operands("(x + ... - ys)").empty());
    CHECK(operands("(sizeof...(Ts) + 1)").empty());
    CHECK(cppfrontier::render_cpp1_expression("(... * xs)") == "(... * xs)");
}

TEST_CASE("requires-expressions are parsed into parameters and requirements",
          "[emitter][expressions][requires]") {
    using cppfrontier::expression_kind;
    const auto text = cppfrontier::parse_expression_text(
        "requires (T const& c, inout m: T) { c.f(); typename T::v; "
        "{ c.g() } noexcept -> C<int>; requires N > 1; }");
    REQUIRE(text.root.children.size() == 1);
    const auto& node = text.root.children.front();
    REQUIRE(node.kind == expression_kind::requires_expression);
    REQUIRE(node.children.size() == 6);
    CHECK(node.children[0].kind == expression_kind::sequence);
    CHECK(spelling(text, node.children[0]) == "Tconst&c");
    const auto& parameter = node.children[1];
    CHECK(parameter.kind == expression_kind::parameter);
    CHECK(text.tokens[parameter.op].text == "m");
    CHECK(spelling(text, parameter.children.front()) == "T");
    auto requirements = std::vector<std::vector<std::string>>{};
    for (auto index = std::size_t{2}; index < node.children.size(); ++index) {
        CHECK(node.children[index].kind == expression_kind::requirement);
        auto& parts = requirements.emplace_back();
        for (const auto& child : node.children[index].children) {
            parts.push_back(spelling(text, child));
        }
    }
    CHECK(requirements == std::vector<std::vector<std::string>>{
        {"c.f()"}, {"T::v"}, {"c.g()", "C<int>"}, {"N>1"}});
}
