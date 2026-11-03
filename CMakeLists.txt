cmake_minimum_required(VERSION 3.25)

project(cppfrontier
  VERSION 0.1.0
  DESCRIPTION "Cpp2 to Cpp1 compiler (rewrite)"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
  lexy
  GIT_REPOSITORY https://github.com/foonathan/lexy.git
  GIT_TAG        v2025.05.0
)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1
)

# Before the dependencies, so that they are built with the same flags.
option(CPPFRONTIER_SANITIZE "Build with ASan, UBSan, and libstdc++ assertions" OFF)
if(CPPFRONTIER_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_compile_definitions(_GLIBCXX_ASSERTIONS)
  add_link_options(-fsanitize=address,undefined)
endif()

FetchContent_MakeAvailable(lexy Catch2)

find_path(CPPFRONTIER_LIBCLANG_INCLUDE clang-c/Index.h
  HINTS /usr/lib/llvm-21/include /usr/lib/llvm-20/include /usr/lib/llvm-19/include)
find_library(CPPFRONTIER_LIBCLANG_LIBRARY clang
  HINTS /usr/lib/llvm-21/lib /usr/lib/llvm-20/lib /usr/lib/llvm-19/lib)

set(CPPFRONTIER_UPSTREAM_BIN "" CACHE FILEPATH
  "Optional upstream cppfront executable for live oracle comparisons")
if(NOT CPPFRONTIER_UPSTREAM_BIN)
  find_program(CPPFRONTIER_UPSTREAM_BIN cppfront
    HINTS "${CMAKE_CURRENT_SOURCE_DIR}/../cppfront" NO_DEFAULT_PATH)
endif()

add_library(cppfrontier
  src/source.cpp
  src/lexy_lexer.cpp
  src/cpp2_validation.cpp
  src/try_operator.cpp
  src/function_expressions.cpp
  src/parser.cpp
  src/parser_context.cpp
  src/statement_parser.cpp
  src/ir_builder.cpp
  src/symbol_resolver.cpp
  src/scope_model.cpp
  src/include_policy.cpp
  src/reflection.cpp
  src/regex_metafunction.cpp
  src/trait_metafunction.cpp
  src/pretty_print.cpp
  src/autodiff_metafunction.cpp
  src/generated_members.cpp
  src/known_types.cpp
  src/safety.cpp
  src/lowering.cpp
  src/lowering_detail.cpp
  src/cpp1_expression_renderer.cpp
  src/cpp1_type_renderer.cpp
  src/type_model.cpp
  src/constraint_checker.cpp
  src/exclusivity.cpp
  src/inspect_patterns.cpp
  src/expression_parser.cpp
  src/translator.cpp
  src/language_server.cpp
  src/json.cpp
  src/lsp.cpp
)

target_include_directories(cppfrontier PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(cppfrontier PRIVATE
  foonathan::lexy
)
target_compile_definitions(cppfrontier PUBLIC CPPFRONTIER_VERSION="${PROJECT_VERSION}")

add_executable(cppfrontier-bin src/main.cpp)
target_link_libraries(cppfrontier-bin PRIVATE cppfrontier)
set_target_properties(cppfrontier-bin PROPERTIES OUTPUT_NAME cppfrontier)

add_executable(cppfrontier-lsp src/lsp_main.cpp)
target_link_libraries(cppfrontier-lsp PRIVATE cppfrontier)

enable_testing()

add_executable(cppfrontier-tests
  tests/unit/autodiff_test.cpp
  tests/unit/cli_test.cpp
  tests/unit/cpp2_validation_test.cpp
  tests/unit/emitter_test.cpp
  tests/unit/experiment_checked_arithmetic_test.cpp
  tests/unit/experiment_defer_test.cpp
  tests/unit/experiment_exclusivity_test.cpp
  tests/unit/experiment_unsafe_test.cpp
  tests/unit/experiment_destructuring_test.cpp
  tests/unit/experiment_given_test.cpp
  tests/unit/experiment_trait_test.cpp
  tests/unit/experiment_adt_test.cpp
  tests/unit/experiment_patterns_test.cpp
  tests/unit/experiment_condition_bindings_test.cpp
  tests/unit/experiment_let_else_test.cpp
  tests/unit/experiment_next_list_test.cpp
  tests/unit/experiment_try_operator_test.cpp
  tests/unit/experiment_newtype_test.cpp
  tests/unit/experiment_delegate_test.cpp
  tests/unit/experiment_linear_test.cpp
  tests/unit/experiment_use_after_move_test.cpp
  tests/unit/experiment_interface_defaults_test.cpp
  tests/unit/experiment_sealed_test.cpp
  tests/unit/experiment_lazy_test.cpp
  tests/unit/experiment_comprehension_test.cpp
  tests/unit/experiment_type_operators_test.cpp
  tests/unit/experiments_test.cpp
  tests/unit/golden_test.cpp
  tests/unit/main.cpp
  tests/unit/language_server_test.cpp
  tests/unit/lexer_test.cpp
  tests/unit/lsp_test.cpp
  tests/unit/ir_walk_test.cpp
  tests/unit/mutation_test.cpp
  tests/unit/parallel_test.cpp
  tests/unit/pipeline_test.cpp
  tests/unit/cpp1_expression_renderer_test.cpp
  tests/unit/parser_test.cpp
  tests/unit/parser_context_test.cpp
  tests/unit/parser_trace_test.cpp
  tests/unit/parser_hang_test.cpp
  tests/unit/upstream_corpus_test.cpp
  tests/unit/upstream_integration_test.cpp
  tests/unit/reflection_test.cpp
  tests/unit/regex_test.cpp
  tests/unit/runtime_test.cpp
  tests/unit/safety_test.cpp
  tests/unit/scope_model_test.cpp
  tests/unit/source_map_test.cpp
  tests/unit/source_model_test.cpp
  tests/unit/type_model_test.cpp
)

add_dependencies(cppfrontier-tests cppfrontier-bin cppfrontier-lsp)

target_compile_definitions(cppfrontier-tests PRIVATE
  CPPFRONTIER_UPSTREAM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../cppfront"
  CPPFRONTIER_RUNTIME_INCLUDE="${CMAKE_CURRENT_SOURCE_DIR}/runtime"
  CPPFRONTIER_GXX="${CMAKE_CXX_COMPILER}"
  CPPFRONTIER_CLI="$<TARGET_FILE:cppfrontier-bin>"
  CPPFRONTIER_LSP="$<TARGET_FILE:cppfrontier-lsp>"
  CPPFRONTIER_UPSTREAM_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../cppfront/regression-tests/test-results"
  CPPFRONTIER_UPSTREAM_BIN="${CPPFRONTIER_UPSTREAM_BIN}"
)

if(CPPFRONTIER_LIBCLANG_INCLUDE AND CPPFRONTIER_LIBCLANG_LIBRARY)
  target_sources(cppfrontier-tests PRIVATE
    tests/support/clang_semantic_oracle.cpp
    tests/unit/upstream_semantic_test.cpp
  )
  target_include_directories(cppfrontier-tests PRIVATE
    ${CPPFRONTIER_LIBCLANG_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
  )
  target_link_libraries(cppfrontier-tests PRIVATE ${CPPFRONTIER_LIBCLANG_LIBRARY})
  target_compile_definitions(cppfrontier-tests PRIVATE CPPFRONTIER_HAS_LIBCLANG=1)
endif()

target_link_libraries(cppfrontier-tests PRIVATE
  cppfrontier
  Catch2::Catch2WithMain
)

# Warnings for the project's own code only. Aggregates are initialized
# partially by design, leaving the other members to their defaults.
foreach(target cppfrontier cppfrontier-bin cppfrontier-lsp cppfrontier-tests)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wswitch -Wno-missing-field-initializers)
endforeach()

include(CTest)
include(Catch)
# Each test case runs as its own process under `ctest -j`; scratch files stay in
# the build tree.
set(CPPFRONTIER_TEST_TMPDIR ${CMAKE_CURRENT_BINARY_DIR}/test-tmp)
file(MAKE_DIRECTORY ${CPPFRONTIER_TEST_TMPDIR})
catch_discover_tests(cppfrontier-tests
  PROPERTIES ENVIRONMENT "TMPDIR=${CPPFRONTIER_TEST_TMPDIR}")


# Whole-corpus checks with the tools/ scripts (label `corpus`); see "Verification
# Layers" in docs/ARCHITECTURE.md. pure2-last-use iterates single values as
# `for (x) do`, where `(x)` is grouping (docs/UPSTREAM_AUDIT.md, "`for (x) do`"),
# and uses assignments as values ("Assignments as values"), so it is
# rewritten first (tools/upstream-rewrites.pl).
# With Clang, upstream's Clang results are the reference, and pure2-last-use
# also fails to compile there as it does upstream.
set(CPPFRONTIER_CORPUS_ENVIRONMENT
  "BIN=$<TARGET_FILE:cppfrontier-bin>"
  "UPSTREAM=${CMAKE_CURRENT_SOURCE_DIR}/../cppfront"
  "CXX=${CMAKE_CXX_COMPILER}"
  "JOBS=4" "REWRITTEN=pure2-last-use")
# pure2-forward-return-diagnostics-error's `return i;` of a local `int` is a
# by-value `-> forward _` return here (docs/UPSTREAM_DIFFERENCES.md).
set(CPPFRONTIER_CORPUS_ALLOW "pure2-forward-return-diagnostics-error")
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  list(APPEND CPPFRONTIER_CORPUS_ENVIRONMENT "RESULTS=clang-19-c++23-libcpp")
  string(APPEND CPPFRONTIER_CORPUS_ALLOW
    " pure2-last-use pure2-hashable pure2-interpolation mixed-is-as-variant")
endif()
list(APPEND CPPFRONTIER_CORPUS_ENVIRONMENT "ALLOW=${CPPFRONTIER_CORPUS_ALLOW}")
foreach(check corpus-pure2 corpus-mixed diagnostics-parity)
  add_test(NAME ${check} COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/${check}.sh)
  set_tests_properties(${check} PROPERTIES LABELS corpus PROCESSORS 4
    ENVIRONMENT "${CPPFRONTIER_CORPUS_ENVIRONMENT};OUT=${CMAKE_CURRENT_BINARY_DIR}/corpus/${check}")
endforeach()
# Upstream cppfront built from our translation of its .h2 sources translates the
# corpus exactly as upstream's own build does.
add_test(NAME self-host COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/self-host.sh)
set_tests_properties(self-host PROPERTIES LABELS corpus PROCESSORS 4
  ENVIRONMENT "${CPPFRONTIER_CORPUS_ENVIRONMENT};OUT=${CMAKE_CURRENT_BINARY_DIR}/corpus/self-host")
add_test(NAME fuzz-truncation COMMAND python3
  ${CMAKE_CURRENT_SOURCE_DIR}/tools/fuzz-truncation.py --bin $<TARGET_FILE:cppfrontier-bin>
  --out ${CMAKE_CURRENT_BINARY_DIR}/corpus/fuzz-truncation)
add_test(NAME fuzz-mutation COMMAND python3
  ${CMAKE_CURRENT_SOURCE_DIR}/tools/fuzz-mutation.py --bin $<TARGET_FILE:cppfrontier-bin>
  --out ${CMAKE_CURRENT_BINARY_DIR}/corpus/fuzz-mutation --per-file 5 --jobs 4)
# Random programs translated by cppfrontier and upstream, compiled, run and compared.
add_test(NAME diff-fuzz COMMAND python3
  ${CMAKE_CURRENT_SOURCE_DIR}/tools/diff-fuzz.py --bin $<TARGET_FILE:cppfrontier-bin>
  --upstream-bin ${CMAKE_CURRENT_BINARY_DIR}/upstream/cppfront
  --out ${CMAKE_CURRENT_BINARY_DIR}/corpus/diff-fuzz --seeds 1:41 --jobs 4 --no-reduce)
set_tests_properties(fuzz-truncation fuzz-mutation diff-fuzz PROPERTIES LABELS corpus PROCESSORS 4)
# wild-corpus.py downloads outside Cpp2 code, so only its manifest is tested.
add_test(NAME wild-corpus-manifest COMMAND python3
  ${CMAKE_CURRENT_SOURCE_DIR}/tools/wild-corpus.py --check-manifest)
set_tests_properties(wild-corpus-manifest PROPERTIES LABELS corpus)
# A subset of the corpus compiles with -Werror, sanitizers and library assertions,
# and runs; `tools/strict-compile.sh --matrix` checks every program with each compiler.
add_test(NAME strict-compile COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/strict-compile.sh --fast)
set_tests_properties(strict-compile PROPERTIES LABELS corpus PROCESSORS 4
  ENVIRONMENT "${CPPFRONTIER_CORPUS_ENVIRONMENT};OUT=${CMAKE_CURRENT_BINARY_DIR}/corpus/strict-compile")
# Large generated inputs that once took quadratic time or worse (label `scaling`).
add_test(NAME scaling COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/scaling.py
  --bin $<TARGET_FILE:cppfrontier-bin> --out ${CMAKE_CURRENT_BINARY_DIR}/scaling)
set_tests_properties(scaling PROPERTIES LABELS scaling TIMEOUT 900)

# Programs ported by hand from Cpp1 material (label `ports`); see tests/ports/README.md.
file(GLOB_RECURSE CPPFRONTIER_PORTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/ports/*.cpp2)
foreach(port ${CPPFRONTIER_PORTS})
  file(RELATIVE_PATH port_name ${CMAKE_CURRENT_SOURCE_DIR}/tests/ports ${port})
  string(REGEX REPLACE "\\.cpp2$" "" port_name ${port_name})
  string(REPLACE "/" "-" port_name ${port_name})
  add_test(NAME port-${port_name} COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/run-port.sh
    $<TARGET_FILE:cppfrontier-bin> ${CMAKE_CXX_COMPILER}
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime ${port}
    ${CMAKE_CURRENT_BINARY_DIR}/ports/${port_name})
  set_tests_properties(port-${port_name} PROPERTIES LABELS ports)
endforeach()
