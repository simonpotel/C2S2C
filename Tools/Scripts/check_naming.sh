#!/bin/sh
# Naming rules: folders PascalCase, files snake_case, no two paths that differ only by case.
# Why: macOS and Windows ignore case, so such a pair would overwrite each other there.
# Exit code 0 = OK. Prints every offending path.
cd "$(dirname "$0")/../.." || exit 1

# Every tracked or not-yet-committed file, minus build output.
files=$(git ls-files --cached --others --exclude-standard | grep -v '^Build/')

# 1. Folders: each path segment must be PascalCase. Tool dot-folders (.github, .cargo) are
#    allowed anywhere, and everything below a top-level one is the tool's business.
bad_dirs=$(echo "$files" | grep -v '^\.' | sed -n 's|/[^/]*$||p' | tr '/' '\n' | sort -u |
    grep -vE '^([A-Z][A-Za-z0-9]*|\.[a-z]+)$')

# 2. Files: snake_case (a leading underscore is allowed: Python imposes __init__.py), or an
#    upper-case root file (README.md, LICENSE...), or a name a tool imposes (CMake, Cargo).
bad_files=$(echo "$files" | sed 's|.*/||' |
    grep -vE '^(\.?_*[a-z0-9][a-z0-9_.-]*|[A-Z][A-Z0-9_]*(\.md)?|CMakeLists\.txt|CMakePresets\.json|Cargo\.(toml|lock))$')

# 3. Case-only collisions: lower-case every path and look for duplicates.
clashes=$(echo "$files" | tr 'A-Z' 'a-z' | sort | uniq -d)

# Report: any non-empty list is a failure.
status=0
for list in "$bad_dirs" "$bad_files" "$clashes"; do
    if [ -n "$list" ]; then echo "$list"; status=1; fi
done
exit $status
