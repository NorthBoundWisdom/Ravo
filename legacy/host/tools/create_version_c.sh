#!/bin/sh

set -e

C_FILE="$1"
NEW_VERSION="$2"

if [ -z "$NEW_VERSION" ]; then
  echo "A project version is required" >&2
  exit 1
fi

MAJOR_VERSION=0
MINOR_VERSION=0
PATCH_VERSION=0
N_COMMITS=0
if echo "$NEW_VERSION" | grep -q "^[0-9]\+\.[0-9]\+\.[0-9]\+"; then
  MAJOR_VERSION=$(echo "$NEW_VERSION" | sed "s/^\([0-9]\+\)\.\([0-9]\+\)\.\([0-9]\+\).*/\1/")
  MINOR_VERSION=$(echo "$NEW_VERSION" | sed "s/^\([0-9]\+\)\.\([0-9]\+\)\.\([0-9]\+\).*/\2/")
  PATCH_VERSION=$(echo "$NEW_VERSION" | sed "s/^\([0-9]\+\)\.\([0-9]\+\)\.\([0-9]\+\).*/\3/")
fi
if echo "$NEW_VERSION" | grep -q "^[0-9]\+\.[0-9]\+\.[0-9]\++[0-9]\+"; then
  N_COMMITS=$(echo "$NEW_VERSION" | sed "s/^\([0-9]\+\)\.\([0-9]\+\)\.\([0-9]\+\)+\([0-9]\+\).*/\4/")
fi

LAST_COMMIT_YEAR=$(date -u "+%Y")
TMP_C_FILE=$(mktemp "${C_FILE}.tmp.XXXXXX")

cleanup_tmp_file() {
  if [ -e "$TMP_C_FILE" ]; then
    rm "$TMP_C_FILE"
  fi
}
trap cleanup_tmp_file EXIT HUP INT TERM

{
  echo "#ifndef RC_BUILD"
  echo "  #ifdef HAVE_CONFIG_H"
  echo "    #include \"config.h\""
  echo "  #endif"

  echo "  const char darktable_package_version[] = \"${NEW_VERSION}\";"
  echo "  const char darktable_package_string[] = PACKAGE_NAME \" ${NEW_VERSION}\";"
  echo "  const char darktable_last_commit_year[] = \"${LAST_COMMIT_YEAR}\";"
  echo "#else"
  echo "  #define DT_MAJOR ${MAJOR_VERSION}"
  echo "  #define DT_MINOR ${MINOR_VERSION}"
  echo "  #define DT_PATCH ${PATCH_VERSION}"
  echo "  #define DT_N_COMMITS ${N_COMMITS}"
  echo "  #define LAST_COMMIT_YEAR \"${LAST_COMMIT_YEAR}\""
  echo "#endif";
} > "$TMP_C_FILE"

# create_version_gen is an ALL target. Preserve the generated file timestamp when
# its contents are unchanged so that ordinary builds do not relink lib_darktable
# and every module merely to reproduce the same version string.
if ! cmp -s "$TMP_C_FILE" "$C_FILE"; then
  mv "$TMP_C_FILE" "$C_FILE"
fi

echo "Version string: ${NEW_VERSION}"
