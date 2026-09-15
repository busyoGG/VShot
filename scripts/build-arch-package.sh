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

# Build from a clean working-tree snapshot while retaining uncommitted packaging changes.
tar \
    --exclude='./.git' \
    --exclude='./target' \
    --exclude='./build-qt' \
    --exclude='./dist' \
    --exclude='./.zcode' \
    -C "$project_root" -cf - . | tar -C "$tmp_dir" -xf -

makepkg_args=(--force --clean --noconfirm)
(
    cd "$tmp_dir"
    makepkg "${makepkg_args[@]}"
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
