#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
base=("$cli" -C "$work")
package="$root/package"
mkdir -p "$package"
printf '%s\n' '---' 'name: deep-package' 'description: worker stack regression' '---' >"$package/SKILL.md"
deep="$package"
for ((level=1; level<=128; level++)); do
  deep="$deep/d"
  mkdir "$deep"
done
dd if=/dev/zero of="$deep/large" bs=65536 count=3 status=none
chmod 755 "$deep/large"
installed="$root/config/cai/skills/deep-package"
run --install-skill "$package"; ok
cmp "$deep/large" "${deep/#$package/$installed}/large"
[[ $(stat -c %a "${deep/#$package/$installed}/large") == 700 ]]
# Replacement removes the previous deep package on the same default worker.
run --install-skill "$package" --force; ok
cmp "$deep/large" "${deep/#$package/$installed}/large"
mkdir "$deep/too-deep"
printf 'must reject\n' >"$deep/too-deep/file"
run --install-skill "$package" --force; bad
contains "$root/stderr" '128 directory levels'
[[ ! -e "${deep/#$package/$installed}/too-deep" ]]
[[ -z $(find "$root/config/cai/skills" -maxdepth 1 -name '.skill-install-*' -print -quit) ]]
rmdir "$deep/too-deep" 2>/dev/null || rm -r "$deep/too-deep"
# Rejected artifacts must neither replace the previous package nor survive rollback.
printf 'outside survives\n' >"$root/outside"
ln -s "$root/outside" "$deep/symlink"
run --install-skill "$package" --force; bad
[[ ! -L "${deep/#$package/$installed}/symlink" ]]
contains "$root/outside" 'outside survives'
[[ -z $(find "$root/config/cai/skills" -maxdepth 1 -name '.skill-install-*' -print -quit) ]]
