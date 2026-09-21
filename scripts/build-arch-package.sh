#!/usr/bin/env bash
set -Eeuo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
output_dir=${1:-"$project_root/dist"}

command -v makepkg >/dev/null 2>&1 || {
    printf 'error: makepkg is required\n' >&2
    exit 1
}
command -v tar >/dev/null 2>&1 || {
    printf 'error: tar is required\n' >&2
    exit 1
}

mkdir -p "$output_dir"
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

# Where makepkg keeps the sources it downloads.
#
# This script builds in a fresh `mktemp -d` every run, and makepkg's own source
# cache -- its default `SRCDEST` -- is *inside the directory it is building in*.
# That is fine for a package built in place twice; here the directory is gone by
# the next run, so every build starts with an empty cache and re-downloads the
# OCR models.  They are 30 MB and their checksums never change, so a fixed cache
# outside the throwaway tree is what makes the download happen once.
#
# `XDG_CACHE_HOME` rather than the project, because this is a cache: it can be
# deleted at any time and the next build will simply fetch the files again.
# Setting it in the environment lets a user's own `SRCDEST` (or a `makepkg.conf`
# that names one) still take precedence, since makepkg reads the environment
# first.
: "${XDG_CACHE_HOME:=$HOME/.cache}"
source_cache=${SRCDEST:-$XDG_CACHE_HOME/vshot/makepkg-sources}
mkdir -p "$source_cache"

# Build from a clean working-tree snapshot while retaining uncommitted packaging changes.
tar \
    --exclude='./.git' \
    --exclude='./target' \
    --exclude='./build-qt' \
    --exclude='./dist' \
    --exclude='./.zcode' \
    --exclude='./.Trash-0' \
    --exclude='./.workbuddy' \
    -C "$project_root" -cf - . | tar -C "$tmp_dir" -xf -

makepkg_args=(--force --clean --noconfirm)
(
    cd "$tmp_dir"
    SRCDEST="$source_cache" makepkg "${makepkg_args[@]}"
)

shopt -s nullglob
packages=("$tmp_dir"/vshot-*.pkg.tar.*)
if ((${#packages[@]} != 1)); then
    printf 'error: expected exactly one package, found %d\n' "${#packages[@]}" >&2
    exit 1
fi

package_path=${packages[0]}
final_path="$output_dir/$(basename -- "$package_path")"
install -Dm644 "$package_path" "$final_path"
printf '%s\n' "$final_path"
