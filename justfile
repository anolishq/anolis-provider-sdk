# Shared task runner for anolis providers. Copy to `justfile` at the repo root and
# set `preset` to the repo's primary CMake configure/test preset.
#
# Standard recipes (match the org convention): setup, fmt, fmt-check, lint, check, test.

# Primary CMake preset — override per repo (e.g. ci-linux-release).
preset := "ci-linux-release"

# C++ sources tracked by git (excludes generated build/ output).
cpp_files := "git ls-files '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx'"

# List available recipes.
default:
    @just --list

# Configure (vcpkg deps resolve during CMake configure).
setup:
    cmake --preset {{preset}}

# Format C++ sources in place (clang-format 18).
fmt:
    {{cpp_files}} | xargs clang-format -i

# Verify formatting without modifying files (CI gate).
fmt-check:
    {{cpp_files}} | xargs clang-format --dry-run --Werror

# Static analysis over the compile database (requires a configured build dir).
lint:
    run-clang-tidy -p build/{{preset}} $({{cpp_files}})

# CI-equivalent: formatting + lint.
check: fmt-check lint

# Build and run the test suite.
test:
    cmake --build --preset {{preset}} --parallel
    ctest --preset {{preset}} --output-on-failure
