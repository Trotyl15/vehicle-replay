# Vehicle‑Replay

![PixPin_2025-06-04_18-39-25-ezgif com-video-to-gif-converter](https://github.com/user-attachments/assets/ba72a7c0-5020-456a-995a-163d8a435607)


## 1 · Install Homebrew packages

```bash
# Apple Silicon  (replace /opt/homebrew with /usr/local on Intel)
brew install \
  protobuf           # protoc + libprotobuf 6.x
  abseil             # Abseil‑C++ libs required by protobuf
  openssl@3          # TLS backend for libwebsockets
  libwebsockets      # WebSocket client library
  pkg-config         # pulls correct cflags / libs
```

<details>
<summary>Optional (if you don’t bundle these dylibs)</summary>

```bash
brew install glfw assimp glm   # graphics stack
```

</details>


## 2 · Generate protobuf sources
If you dirst clone this repo or ever add or edit a .proto, run once:

```
protoc -I . --cpp_out=. car_pose.proto
```


## 3 · Configure workspace path

Create a file called **`.env`** in the repo root:

```env
WORKSPACE_PATH=/absolute/path/to/vehicle-replay
```

Change the value after `=` to the real location of **this** repository (e.g. `/Users/<you>/Projects/vehicle-replay`).


## 4 · Build & run

```bash
# compile — VS Code ⇧⌘B also works
./build.sh

# launch viewer
./app
```
