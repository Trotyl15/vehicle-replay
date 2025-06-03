# Vehicle‑Replay
![PixPin_2025-06-03_16-43-13-ezgif com-loop-count](https://github.com/user-attachments/assets/54f45cf8-ae15-4ae7-a755-e24af564fdb7)

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
