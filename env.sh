# Source this from the repository root before building or running:
#   source env.sh
#
# Two Homebrew-specific problems and one macOS one are handled here.

export PATH="/opt/homebrew/bin:$PATH"

# 1. Homebrew puts the MoltenVK ICD manifest under etc/vulkan, not the
#    share/vulkan the loader searches. Without this the loader reports
#    "Found no drivers!" despite a correct install.
export VK_DRIVER_FILES="/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json"

# 2. Homebrew's validation layer manifest names its library relatively
#    ("libVkLayer_khronos_validation.dylib"), so dyld cannot find it and
#    vkCreateInstance fails with VK_ERROR_LAYER_NOT_PRESENT -- even though the
#    loader locates and parses the manifest correctly.
#
# 3. The obvious fix, DYLD_LIBRARY_PATH, does not survive a shell script:
#    macOS System Integrity Protection strips every DYLD_* variable when it
#    launches a protected binary, and /bin/sh is protected. So any wrapper
#    script would silently lose the layer even though running the same binary
#    straight from the terminal works.
#
# Rewriting the manifest with an absolute library path fixes both at once and
# needs no DYLD_* variable at all.
# Resolve against this script's own location, not the working directory.
# Sourcing it from elsewhere -- from a sibling checkout, say -- would otherwise
# create a build/ directory there.
if [ -n "${ZSH_VERSION:-}" ]; then
    _gba_src="$(eval 'echo ${(%):-%x}')"
else
    _gba_src="${BASH_SOURCE[0]:-$0}"
fi
_gba_root="$(cd "$(dirname "$_gba_src")" && pwd)"

_layer_src="/opt/homebrew/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"
_layer_dir="$_gba_root/build/vulkan/explicit_layer.d"
if [ -f "$_layer_src" ]; then
    mkdir -p "$_layer_dir"
    sed 's|"library_path": *"lib|"library_path": "/opt/homebrew/lib/lib|' \
        "$_layer_src" > "$_layer_dir/VkLayer_khronos_validation.json"
    export VK_LAYER_PATH="$_layer_dir"
else
    export VK_LAYER_PATH="/opt/homebrew/share/vulkan/explicit_layer.d"
fi
unset _layer_src _layer_dir _gba_src _gba_root
