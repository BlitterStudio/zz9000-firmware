#!/bin/sh
# Reject a module-local newlib heap and require WebM's allocator interposition.
set -eu

require_shims=0
if [ "${1:-}" = "--require-shims" ]; then
    require_shims=1
    shift
fi

if [ "$#" -ne 1 ]; then
    echo "usage: $0 [--require-shims] module.elf" >&2
    exit 2
fi

elf=$1
: "${READELF:=arm-none-eabi-readelf}"

"$READELF" -Ws "$elf" | awk -v require_shims="$require_shims" '
BEGIN {
    split("_malloc_r __malloc_av_", forbidden, " ")
    if (require_shims)
        split("_sbrk _sbrk_r", sbrk_symbols, " ")
    split("malloc free calloc realloc memalign posix_memalign aligned_alloc", allocator, " ")
    for (i in forbidden) bad[forbidden[i]] = 1
    for (i in sbrk_symbols) bad[sbrk_symbols[i]] = 1
    for (i in allocator) alloc[allocator[i]] = 1
}
$1 ~ /^[0-9]+:$/ {
    ndx = $7
    name = $8
    if (name in bad && ndx != "ABS") {
        print "module-local newlib heap symbol: " name > "/dev/stderr"
        failed = 1
    }
    if (name in alloc && ndx == "UND") {
        print "unresolved allocator symbol: " name > "/dev/stderr"
        failed = 1
    }
    if (name ~ /^__wrap_(malloc|free|calloc|realloc|memalign|posix_memalign|aligned_alloc)$/ && ndx != "UND")
        shim[name] = 1
}
END {
    if (require_shims) {
        split("malloc free calloc realloc memalign posix_memalign aligned_alloc", required, " ")
        for (i in required) {
            name = "__wrap_" required[i]
            if (!(name in shim)) {
                print "missing WebM allocator shim: " name > "/dev/stderr"
                failed = 1
            }
        }
    }
    exit failed
}'
