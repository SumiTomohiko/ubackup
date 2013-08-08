#!/bin/sh

project="$(grep -w project < "$(dirname $0)/CMakeLists.txt" | sed -e "s/project(//" | sed -e "s/)//")"

tag="$1"
if [ -z "${tag}" ]; then
    echo "no tag given."
    echo "usage: $0 tag"
    exit 1
fi

name="${project}-${tag}"

git archive --format=tar --prefix=${name}/ ${tag} | xz > ${name}.tar.xz

# vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
