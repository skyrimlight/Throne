#!/bin/bash
set -euo pipefail

USAGE="usage: pack_release.sh <windows|linux-amd64|linux-arm64|macos|debug> <tag>"
TARGET="${1:?$USAGE}"
export TAG="${2:?$USAGE}"
export ROOT="$PWD"
export DEPLOY="$ROOT/deployment"
export WORK="$ROOT/.pack"
JOBS="${PACK_JOBS:-$(nproc)}"

rm -rf "$WORK"
mkdir -p "$WORK/logs" "$DEPLOY"

# Common tarballs are skipped: every build tarball already carries its core.
untar() {
    local glob="${1:-*}"
    shift || true
    echo "=== Searching download-artifact for artifacts (glob: $glob) ==="
    local found=0
    while IFS= read -r -d '' tb; do
        local norm_tb="${tb//\\//}"
        if [[ "$norm_tb" == *"-Common-"* ]]; then
            echo "Skipping Common tarball: $norm_tb"
            continue
        fi
        echo "Found artifact archive: $norm_tb -> Extracting..."
        tar xzf "$tb" "$@"
        found=1
    done < <(find download-artifact -name "artifacts.tgz" -print0 2>/dev/null)

    if [ "$found" -eq 0 ]; then
        echo "WARNING: No artifacts.tgz found by -name in download-artifact! Listing tree:"
        find download-artifact 2>/dev/null || true
        # Fallback to any .tgz file
        while IFS= read -r -d '' tb; do
            echo "Fallback extracting: $tb"
            tar xzf "$tb" "$@"
            found=1
        done < <(find download-artifact -name "*.tgz" -not -path '*-Common-*' -print0 2>/dev/null)
    fi

    echo "=== Extracted deployment contents: ==="
    ls -la "$DEPLOY" 2>/dev/null || echo "DEPLOY directory ($DEPLOY) not found"
    if [ -d "$DEPLOY" ]; then
        ls -la "$DEPLOY"/* 2>/dev/null || true
    fi
}

build_installer() {
    local version="${TAG#v}"
    version="${version#V}"
    local parts
    IFS='.' read -r -a parts <<<"${version%%-*}"
    local iscc="${ISCC:-$(command -v iscc.exe || echo '/c/Program Files (x86)/Inno Setup 6/ISCC.exe')}"
    if [ ! -f "$iscc" ] && ! command -v iscc.exe >/dev/null 2>&1; then
        echo "Searching for ISCC.exe in Program Files..."
        local alt_iscc
        alt_iscc=$(find "/c/Program Files" "/c/Program Files (x86)" -name "ISCC.exe" 2>/dev/null | head -n 1 || true)
        if [ -n "$alt_iscc" ]; then
            iscc="$alt_iscc"
        fi
    fi
    echo "Using Inno Setup compiler: $iscc"
    # ISCC is a native Windows program: stop MSYS from rewriting its /-switches as paths.
    MSYS_NO_PATHCONV=1 "$iscc" /Q \
        "/DAppVersion=$version" \
        "/DAppVersionMajor=${parts[0]:-0}" \
        "/DAppVersionMinor=${parts[1]:-0}" \
        "/DAppVersionPatch=${parts[2]:-0}" \
        "/DAppVersionBuild=${parts[3]:-0}" \
        "/O$(cygpath -w "$DEPLOY")" \
        "/FThrone-$TAG-windows-universal-installer" \
        "$(cygpath -w "$ROOT/script/windows_installer.iss")"
}

# Hard links give the archive its Throne/ root without moving a dir other tasks still read.
zip_dir() {
    mkdir -p "$WORK/$2"
    cp -al "$DEPLOY/$1" "$WORK/$2/Throne" 2>/dev/null || cp -r "$DEPLOY/$1" "$WORK/$2/Throne"
    cd "$WORK/$2"
    if command -v zip >/dev/null; then
        zip -q -r "$DEPLOY/Throne-$TAG-$2.zip" Throne
    else
        # Windows runners ship 7-Zip but not Info-ZIP.
        7z a -tzip -mx=5 -bd -bso0 "$DEPLOY/Throne-$TAG-$2.zip" Throne
    fi
}

zip_app() {
    mkdir -p "$WORK/$2/Throne"
    mv "$DEPLOY/$1/Throne.app" "$WORK/$2/Throne/"
    cd "$WORK/$2"
    zip -q --symlinks -r "$DEPLOY/Throne-$TAG-$2.zip" Throne
}

zip_debug() {
    cd "$DEPLOY"
    zip -q -r debug-symbols.zip debug
}

pack_deb() {
    cd "$DEPLOY"
    bash "$ROOT/script/pack_debian.sh" "$TAG" "$@"
}

pack_rpm() {
    cd "$DEPLOY"
    bash "$ROOT/script/pack_rpm.sh" "$TAG" "$@"
}

run_task() {
    local name="$1"
    shift
    # No `|| rc=$?` here: errexit is ignored for any command on the left of || or inside an if test.
    (set -eo pipefail; "$@") >"$WORK/logs/$name.log" 2>&1
    local rc=$?
    printf '%4ds  %s%s\n' "$SECONDS" "$name" "$([[ $rc == 0 ]] || echo ' FAILED')" >>"$WORK/logs/times"
    return $rc
}

export -f build_installer zip_dir zip_app zip_debug pack_deb pack_rpm run_task

case "$TARGET" in
windows)
    untar 'windows*' --exclude='*.pdb'
    TASKS=(
        "installer build_installer"
        "zip-windows64 zip_dir windows-amd64 windows64"
    )
    if [ -d "$DEPLOY/windows-arm64" ]; then
        TASKS+=("zip-windows-arm64 zip_dir windows-arm64 windows-arm64")
    fi
    if [ -d "$DEPLOY/windowslegacy-386" ]; then
        TASKS+=("zip-windows32 zip_dir windowslegacy-386 windows32")
    fi
    if [ -d "$DEPLOY/windowslegacy-amd64" ]; then
        TASKS+=("zip-windowslegacy64 zip_dir windowslegacy-amd64 windowslegacy64")
    fi
    ;;
linux-amd64 | linux-arm64)
    arch="${TARGET#linux-}"
    untar "linux-$arch*" --exclude=Throne.debug
    TASKS=(
        "deb-$arch pack_deb $arch"
        "rpm-$arch pack_rpm $arch"
        "deb-$arch-system-qt pack_deb $arch systemqt"
        "rpm-$arch-system-qt pack_rpm $arch systemqt"
        "zip-linux-$arch zip_dir linux-$arch linux-$arch"
    )
    ;;
macos)
    untar 'darwin*' --exclude=Throne.dSYM
    TASKS=(
        "zip-macos-arm64 zip_app darwin-arm64 macos-arm64"
        "zip-macos-amd64 zip_app darwin-amd64 macos-amd64"
        "zip-macoslegacy-amd64 zip_app darwinlegacy-amd64 macoslegacy-amd64"
    )
    ;;
debug)
    untar linux-amd64 --wildcards '*/Throne.debug'
    untar linux-arm64 --wildcards '*/Throne.debug'
    untar 'windows*' --wildcards '*/Throne.pdb'
    untar 'darwin*' --wildcards '*/Throne.dSYM/*'
    cd "$DEPLOY"
    mkdir -p debug
    mv linux-amd64/Throne.debug "debug/Throne-$TAG-linux-amd64.debug"
    mv linux-arm64/Throne.debug "debug/Throne-$TAG-linux-arm64.debug"
    for dir in windows-amd64 windows-arm64 windowslegacy-386 windowslegacy-amd64; do
        mv "$dir/Throne.pdb" "debug/Throne-$TAG-$dir.pdb"
    done
    mv darwin-arm64/Throne.app/Contents/MacOS/Throne.dSYM "debug/Throne-$TAG-macos-arm64.dSYM"
    mv darwin-amd64/Throne.app/Contents/MacOS/Throne.dSYM "debug/Throne-$TAG-macos-amd64.dSYM"
    mv darwinlegacy-amd64/Throne.app/Contents/MacOS/Throne.dSYM "debug/Throne-$TAG-macoslegacy-amd64.dSYM"
    TASKS=("debug-symbols zip_debug")
    ;;
*)
    echo "$USAGE" >&2
    exit 1
    ;;
esac

rc=0
printf '%s\n' "${TASKS[@]}" | xargs -L1 -P "$JOBS" bash -c 'run_task "$@"' _ || rc=$?

for log in "$WORK"/logs/*.log; do
    echo "::group::$(basename "$log" .log)"
    cat "$log"
    echo "::endgroup::"
done
echo "Pack task durations:"
sort -rn "$WORK/logs/times"
rm -rf "$WORK"
exit "$rc"
