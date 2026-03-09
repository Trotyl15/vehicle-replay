# Vehicle‑Replay

![PixPin_2025-06-04_18-39-25-ezgif com-video-to-gif-converter](https://github.com/user-attachments/assets/ba72a7c0-5020-456a-995a-163d8a435607)
<img width="3456" height="2168" alt="image" src="https://github.com/user-attachments/assets/9d549a13-6c2b-4401-bc20-c8a826b833e5" />


## 1 · Install Homebrew packages

```bash
# Apple Silicon  (replace /opt/homebrew with /usr/local on Intel)
brew install \
  protobuf \
  abseil \
  openssl@3 \
  libwebsockets \
  pkg-config \
  glfw \
  assimp
```

> `glfw` and `assimp` are required at runtime — the bundled dylibs in `dependencies/library/` embed Homebrew install-name paths that must be resolvable on your machine.

> **assimp version note:** Homebrew currently ships assimp 6.x, but the bundled library carries a `libassimp.5` install name. After building (step 4), patch the binary once:
> ```bash
> install_name_tool -change \
>   /opt/homebrew/opt/assimp/lib/libassimp.5.dylib \
>   /opt/homebrew/opt/assimp/lib/libassimp.6.dylib \
>   ./app
> ```
> If a future Homebrew update changes the version again, adjust the path on the right-hand side accordingly.


## 2 · Generate protobuf sources

If you first clone this repo or ever add or edit a `.proto`, run once:

```bash
protoc -I . --cpp_out=. car_pose.proto
```


## 3 · Configure workspace path

Create a file called **`.env`** in the repo root:

```env
WORKSPACE_PATH=/absolute/path/to/vehicle-replay
```

Change the value after `=` to the real location of **this** repository (e.g. `/Users/<you>/Projects/vehicle-replay`).


## 4 · Build & run

```bash
# compile — VS Code ⇧⌘B also works
./build.sh

# launch viewer
./app
```
