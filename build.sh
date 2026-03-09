#!/bin/bash
source .env

WORKSPACE=$WORKSPACE_PATH # Replace with your actual workspace path

clang++ -std=c++17 -fcolor-diagnostics -Wall -fansi-escape-codes -g \
    -I${WORKSPACE} \
    -I${WORKSPACE}/res \
    -I${WORKSPACE}/dependencies/include \
    -I${WORKSPACE}/imgui \
    -I/opt/homebrew/include \
    -I/opt/homebrew/opt/abseil/include \
    -I/opt/homebrew/opt/openssl@3/include \
    -L${WORKSPACE}/dependencies/library \
    -L/opt/homebrew/lib \
    -L/opt/homebrew/opt/abseil/lib \
    -L/opt/homebrew/opt/openssl@3/lib \
    ${WORKSPACE}/main.cpp \
    ${WORKSPACE}/car_pose.pb.cc \
    ${WORKSPACE}/glad.c \
    ${WORKSPACE}/imgui/imgui.cpp \
    ${WORKSPACE}/imgui/imgui_demo.cpp \
    ${WORKSPACE}/imgui/imgui_draw.cpp \
    ${WORKSPACE}/imgui/imgui_tables.cpp \
    ${WORKSPACE}/imgui/imgui_widgets.cpp \
    ${WORKSPACE}/imgui/imgui_impl_glfw.cpp \
    ${WORKSPACE}/imgui/imgui_impl_opengl3.cpp \
    ${WORKSPACE}/dependencies/library/libglfw.3.4.dylib \
    /opt/homebrew/opt/assimp/lib/libassimp.dylib \
    -lwebsockets \
    $(pkg-config --cflags protobuf absl_log) \
    $(pkg-config --libs protobuf absl_log) \
    -framework OpenGL \
    -framework Cocoa \
    -framework IOKit \
    -framework CoreVideo \
    -framework CoreFoundation \
    -o ${WORKSPACE}/app \
    -Wno-deprecated