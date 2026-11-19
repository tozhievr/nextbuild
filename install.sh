#!/bin/sh
set -eu

bin_dir="${HOME}/.local/bin"
version=latest
usage() {
    printf '%s\n' 'Usage: sh install.sh [--version X.Y.Z] [--bin-dir DIRECTORY]'
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --version|--bin-dir)
            [ "$#" -ge 2 ] && [ -n "$2" ] || { usage >&2; exit 2; }
            if [ "$1" = --version ]; then version=$2; else bin_dir=$2; fi
            shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

fail() { printf 'nextbuild: %s\n' "$*" >&2; exit 1; }
download() { curl -fsSL --proto '=https' --proto-redir '=https' --retry 2 "$@"; }

case "$(uname -s):$(uname -m)" in
    Linux:x86_64|Linux:amd64) platform=Linux-X64 ;;
    Darwin:arm64|Darwin:aarch64) platform=macOS-ARM64 ;;
    *) fail "No release binary for $(uname -s) $(uname -m). See the source build instructions." ;;
esac

repo=https://github.com/tozhievr/nextbuild
if [ "$version" = latest ]; then
    url=$(download -o /dev/null -w '%{url_effective}' "$repo/releases/latest")
    case "$url" in
        "$repo"/releases/tag/v*) version=${url##*/} ;;
        *) fail 'Could not resolve the latest release.' ;;
    esac
fi
version=${version#v}
printf '%s\n' "$version" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' ||
    fail 'Expected a release version such as 0.2.1.'

work=$(mktemp -d)
temp_binary=
cleanup() {
    [ -z "$temp_binary" ] || rm -f -- "$temp_binary"
    rm -rf -- "$work"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
asset="nextbuild-$version-$platform.tar.gz"
base="$repo/releases/download/v$version"
download -o "$work/$asset" "$base/$asset"
download -o "$work/SHA256SUMS" "$base/SHA256SUMS"
expected=$(awk -v asset="$asset" '$2 == asset || $2 == "./" asset || $2 == "*" asset || $2 == "*./" asset {print tolower($1)}' "$work/SHA256SUMS")
if command -v sha256sum >/dev/null 2>&1; then
    checksum=$(sha256sum "$work/$asset")
elif command -v shasum >/dev/null 2>&1; then
    checksum=$(shasum -a 256 "$work/$asset")
else
    fail 'Install sha256sum or shasum to verify the archive.'
fi
actual=$(printf '%s\n' "$checksum" | awk '{ print tolower($1) }')
[ "$actual" = "$expected" ] || fail "SHA-256 mismatch for $asset."
tar -xzf "$work/$asset" -C "$work" nextbuild Parsley-LICENSE
[ -f "$work/nextbuild" ] && [ ! -L "$work/nextbuild" ] || fail 'Archive does not contain a regular executable.'

mkdir -p -- "$bin_dir"
bin_dir=$(cd -- "$bin_dir" && pwd)
[ ! -d "$bin_dir/nextbuild" ] || fail "Destination is a directory: $bin_dir/nextbuild"
temp_binary=$(mktemp "$bin_dir/.nextbuild.XXXXXX")
install -m 0755 "$work/nextbuild" "$temp_binary"
install -m 0644 "$work/Parsley-LICENSE" "$bin_dir/nextbuild-Parsley-LICENSE"
mv -f -- "$temp_binary" "$bin_dir/nextbuild"
temp_binary=
printf 'Installed nextbuild %s to %s\n' "$version" "$bin_dir/nextbuild"
case ":${PATH-}:" in
    *":$bin_dir:"*) ;;
    *) printf 'Add this directory to PATH: %s\n' "$bin_dir" ;;
esac
