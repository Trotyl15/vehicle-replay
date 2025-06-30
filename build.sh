#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------------------
# 0.  Workspace root
# ---------------------------------------------------------------------------
# If WORKSPACE_PATH is exported in .env we respect it; otherwise use the
# directory that contains this script.
SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
WORKSPACE="${WORKSPACE_PATH:-$SCRIPT_DIR}"

# Optional per-user overrides
[ -f "$WORKSPACE/.env" ] && source "$WORKSPACE/.env"

CXX=${CXX:-clang++}
CXXFLAGS=( -std=c++17 -Wall -Wextra -g -fcolor-diagnostics )
CXXFLAGS+=( -DGLFW_INCLUDE_NONE -DIMGUI_IMPL_OPENGL_LOADER_GLAD )
# ---------------------------------------------------------------------------
# 1.  Source files
# ---------------------------------------------------------------------------
SRC_FILES=(
  "$WORKSPACE/main.cpp"
  "$WORKSPACE/car_pose.pb.cc"
  "$WORKSPACE/glad.c"
  "$WORKSPACE/imgui/imgui.cpp"
  "$WORKSPACE/imgui/imgui_demo.cpp"
  "$WORKSPACE/imgui/imgui_draw.cpp"
  "$WORKSPACE/imgui/imgui_tables.cpp"
  "$WORKSPACE/imgui/imgui_widgets.cpp"
  "$WORKSPACE/imgui/imgui_impl_glfw.cpp"
  "$WORKSPACE/imgui/imgui_impl_opengl3.cpp"
)

# ---------------------------------------------------------------------------
# 2.  Include directories
# ---------------------------------------------------------------------------
INCLUDE_DIRS=(
  "-I$WORKSPACE"
  "-I$WORKSPACE/res"
  "-I$WORKSPACE/dependencies/include"
  "-I$WORKSPACE/imgui"
)

# ---------------------------------------------------------------------------
# 3.  Link flags (Linux vs. macOS)
# ---------------------------------------------------------------------------
case "$(uname -s)" in
  Darwin)
    # ------- macOS / Homebrew ------------------------------------------------
    LIB_FLAGS=(
      "$WORKSPACE/dependencies/library/libglfw.3.4.dylib"
      "$WORKSPACE/dependencies/library/libassimp.5.4.3.dylib"
      "-lwebsockets"
      "-framework OpenGL"
      "-framework Cocoa"
      "-framework IOKit"
      "-framework CoreVideo"
      "-framework CoreFoundation"
    )
    ;;
  *)
    # ------- Linux / Ubuntu --------------------------------------------------
    # ------- Linux / Ubuntu --------------------------------------------------
    LIB_FLAGS=(
        $(pkg-config --libs glfw3 assimp libwebsockets protobuf)   # system libs
        $(pkg-config --libs absl_log absl_flags_parse)             # Abseil bundles
        -L/usr/local/lib                                           # in case anything still lives here
        -lGL -ldl -pthread
    )

    ;;
esac

# ---------------------------------------------------------------------------
# 4.  Compile & link
# ---------------------------------------------------------------------------
"$CXX" "${CXXFLAGS[@]}" \
       "${INCLUDE_DIRS[@]}" \
       "${SRC_FILES[@]}" \
       "${LIB_FLAGS[@]}" \
       -o "$WORKSPACE/app"
