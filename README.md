# wlpuppet

Tools for automated testing and remote control of Wayland applications. The
first one, `wlpuppet-mcp`, is an MCP server that lets an AI agent see and
operate GUI applications. Tools to record and replay user input are planned and
will share the same core.

## How it works

wlpuppet is a Wayland proxy. Applications connect to a socket of its own
(`wlpuppet-0`, ...) instead of the compositor's, and every connection is relayed
to the real compositor. The application keeps running in your normal session,
but because all of its traffic passes through wlpuppet, the proxy can

- **see the windows:** it keeps its own copy of the client's surfaces and
  buffers and composites screenshots itself, popup menus and hi-DPI scaling
  included. GPU-rendered buffers are read back through EGL.
- **inject input:** it sends pointer and keyboard events straight to the
  client, using a keymap of its own so any text can be typed whatever the
  layout. Your real input to these windows is blocked, and your desktop is
  never touched.
- **keep hidden windows alive:** windows on another workspace or minimized
  keep rendering, so they can still be operated.

The proxy understands every message because all Wayland protocol XML is
embedded in the binary at build time. Globals it has no XML for are hidden from
clients.

```
lib/core          the proxy: sessions, surface tracking, input, screenshots
lib/mcp           a generic MCP (stdio) server
apps/wlpuppet-mcp the MCP tools
tests/            unit tests and an end-to-end test on headless sway
```

## Building

Needs CMake ≥ 3.30, GCC ≥ 15 or Clang ≥ 19 (C23 `#embed`, C++23), libxkbcommon,
libpng and expat. wayland and wayland-protocols are needed at build time only.
EGL/GLESv2 are optional, for screenshots of GPU-rendered applications.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --install build/release --prefix ~/.local --strip
```

## Running the MCP server

The MCP client starts the server over stdio, so register it with your client.
For Claude Code:

```sh
claude mcp add --scope user wlpuppet -- ~/.local/bin/wlpuppet-mcp --agent-cursor
```

wlpuppet does not start applications. The agent calls `display_info`, starts
the application from its own shell with the environment it is given (mainly
`WAYLAND_DISPLAY=/run/user/1000/wlpuppet-0`), and calls `wait_for_window`.
Only applications started this way can be controlled. Each server instance
picks the first free socket name, so several sessions can run side by side.

Tools: `display_info`, `wait_for_window`, `list_windows`, `screenshot`,
`click`, `pointer_move`, `pointer_button`, `drag`, `scroll`, `key`, `type_text`
and `close_window`. Coordinates are pixels in the latest screenshot of the
window.

### Options

| Option | Meaning |
|---|---|
| `-s, --socket NAME` | Socket name (default: first free `wlpuppet-N`). |
| `--agent-cursor` | Draw a pink cursor in the window where the agent points, for people watching. It is not in screenshots. |
| `--log-file PATH` | Also append the log, including crash reports, to `PATH`. MCP clients usually discard stderr. |
| `--dmabuf MODE` | How applications may use GPU buffers: `auto` (default), `passthrough`, `linear` or `hide`. |
| `--no-keep-alive` | Let hidden windows stop rendering. |
| `--frame-timeout MS` | How long before the proxy answers frame callbacks that the compositor has not answered (default 100). |
| `--layout LAYOUT`, `--variant VARIANT` | XKB layout presented to applications (default `us`). |
| `--hide-global NAME` | Hide a Wayland global from applications (repeatable). |
| `--protocol-dir DIR` | Load extra protocol XML at startup (repeatable). |
| `--max-image-size PX` | Downscale screenshots larger than this (default 1568). |
| `-v, --verbose` / `--trace` | Debug logging / log every Wayland message. |

For testing on machines without a display, run wlpuppet on top of a headless
compositor such as `WLR_BACKENDS=headless sway`.

## Limitations

- Things the compositor checks against real input serials don't work for
  injected input: drag-and-drop between applications, and moving or resizing
  windows by their client-side decorations. Popup menus work.
- X11 (Xwayland) applications bypass the proxy.
- Touch, tablet and input-method input are not injected yet.
