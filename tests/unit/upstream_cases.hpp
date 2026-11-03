#pragma once

#include <array>
#include <string_view>

namespace cppfrontier::test {

struct upstream_case final {
    std::string_view file_name;
    bool compare_semantics;
};

inline constexpr std::array upstream_cases{
    upstream_case{"mixed-allcpp1-hello.cpp2", true},
    upstream_case{"mixed-float-literals.cpp2", true},
    upstream_case{"mixed-hello.cpp2", true},
    upstream_case{"mixed-intro-example-three-loops.cpp2", true},
    upstream_case{"mixed-intro-for-with-counter-include-last.cpp2", true},
    upstream_case{"mixed-ufcs-multiple-template-arguments.cpp2", true},
    upstream_case{"pure2-assert-expected-not-null.cpp2", true},
    upstream_case{"pure2-assert-optional-not-null.cpp2", true},
    upstream_case{"pure2-bounds-safety-span.cpp2", true},
    upstream_case{"pure2-break-continue.cpp2", true},
    upstream_case{"pure2-bugfix-for-indexed-call.cpp2", true},
    upstream_case{"pure2-bugfix-for-requires-clause-unbraced-function-initializer.cpp2", true},
    upstream_case{"pure2-function-multiple-forward-arguments.cpp2", true},
    upstream_case{"pure2-function-single-expression-body-default-return.cpp2", true},
    upstream_case{"pure2-hello.cpp2", true},
    upstream_case{"pure2-intro-example-hello-2022.cpp2", true},
    upstream_case{"pure2-intro-example-three-loops.cpp2", true},
    upstream_case{"pure2-repeated-call.cpp2", true},
    upstream_case{"pure2-synthesize-rightshift-and-rightshifteq.cpp2", true},
    upstream_case{"pure2-template-parameter-lists.cpp2", true},
    upstream_case{"pure2-types-that-parameters.cpp2", false},
};

}
