#!/bin/sh
# Fail when a release tag disagrees with the version the firmware reports.
#
# ZZTop compares REVISION_MAJOR.REVISION_MINOR.REVISION_PATCH, read from the
# card, with the tag of the latest GitHub release. A tag that does not match
# the built firmware would make every board offer the same update forever.
#
# Usage: util/check_release_version.sh v2.8.1
#        util/check_release_version.sh v2.9.0-rc1   (suffix ignored)
set -eu

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <tag>" >&2
    exit 64
fi

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
main_c="$script_dir/../ZZ9000_proto.sdk/ZZ9000OS/src/main.c"

tag=$1
core=${tag#v}
core=${core%%-*}
case "$core" in
    *[!0-9.]*|.*|*.|*..*)
        echo "ERROR: tag '$tag' is not vMAJOR.MINOR.PATCH[-suffix]" >&2
        exit 1
        ;;
esac
tag_major=${core%%.*}
rest=${core#*.}
tag_minor=${rest%%.*}
tag_patch=${rest#*.}
if [ "$rest" = "$core" ] || [ "$tag_patch" = "$rest" ] ||
   [ "${tag_patch#*.}" != "$tag_patch" ]; then
    echo "ERROR: tag '$tag' is not vMAJOR.MINOR.PATCH[-suffix]" >&2
    exit 1
fi

revision() {
    value=$(sed -n "s/^#define REVISION_$1 \([0-9][0-9]*\)\$/\1/p" "$main_c" | tr -d '\r')
    if [ -z "$value" ]; then
        echo "ERROR: REVISION_$1 not found in $main_c" >&2
        exit 1
    fi
    echo "$value"
}

fw_major=$(revision MAJOR)
fw_minor=$(revision MINOR)
fw_patch=$(revision PATCH)

if [ "$tag_major" -ne "$fw_major" ] || [ "$tag_minor" -ne "$fw_minor" ] ||
   [ "$tag_patch" -ne "$fw_patch" ]; then
    echo "ERROR: tag $tag does not match firmware" \
        "$fw_major.$fw_minor.$fw_patch (REVISION_* in main.c)" >&2
    exit 1
fi
echo "Tag $tag matches firmware $fw_major.$fw_minor.$fw_patch"
