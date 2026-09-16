# Source this before building or running:  source env.sh
#
# Homebrew's Vulkan packages do not put the MoltenVK ICD manifest where the
# loader looks by default. Note it lives under etc/vulkan, NOT share/vulkan --
# without VK_DRIVER_FILES the loader reports "Found no drivers!" even though
# MoltenVK is correctly installed.

export PATH="/opt/homebrew/bin:$PATH"
export VK_DRIVER_FILES="/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json"
export VK_LAYER_PATH="/opt/homebrew/share/vulkan/explicit_layer.d"

# Uncomment to make validation errors abort immediately rather than just print.
# export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT

# The Homebrew validation layer manifest names its library relatively
# ("libVkLayer_khronos_validation.dylib"), so dyld cannot find it without a
# search path. Without this, vkCreateInstance fails with VK_ERROR_LAYER_NOT_PRESENT
# even though the loader locates and parses the manifest correctly.
export DYLD_LIBRARY_PATH="/opt/homebrew/lib:${DYLD_LIBRARY_PATH}"
