#!/bin/bash
# Canonical release-variant matrix. Both build drivers consume these rows.
# Fields: name|bitstream|label|ZORRO2|ZORRO3|ZZ9500|2MB|SUPERDENISE|Z3_FASTRAM|CAPTURE_C28
VARIANT_MATRIX=(
    'zorro3|bootimage_work/zz9000_ps_wrapper.bit|Zorro III, A3000/A4000, E7M capture|0|1|0|0|0|1|0'
    'zorro3-aga|bootimage_work/variants/zz9000_ps_wrapper-zorro3-aga.bit|A4000/A4000T AGA video-slot capture, Zorro III Fast RAM|0|1|0|0|0|1|1'
    'zorro2|bootimage_work/variants/zz9000_ps_wrapper-zorro2.bit|Zorro II 4MB, A2000|1|0|0|0|0|0|0'
    'zorro2-2mb|bootimage_work/variants/zz9000_ps_wrapper-zorro2-2mb.bit|Zorro II 2MB, A2000|1|0|0|1|0|0|0'
    'a500|bootimage_work/variants/zz9000_ps_wrapper-a500.bit|A500 4MB, ZZ9500CX Denise adapter|1|0|1|0|0|0|0'
    'a500-2mb|bootimage_work/variants/zz9000_ps_wrapper-a500-2mb.bit|A500 2MB, ZZ9500CX Denise adapter|1|0|1|1|0|0|0'
    'a500plus|bootimage_work/variants/zz9000_ps_wrapper-a500plus.bit|A500+ or Super Denise, ZZ9500CX Denise adapter|1|0|0|0|1|0|0'
)

variant_matrix_rows() {
    printf '%s\n' "${VARIANT_MATRIX[@]}"
}

variant_matrix_row() {
    local variant=$1 row
    for row in "${VARIANT_MATRIX[@]}"; do
        if [[ ${row%%|*} == "$variant" ]]; then
            printf '%s\n' "$row"
            return 0
        fi
    done
    return 1
}
