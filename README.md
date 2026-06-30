# CallTree TUI

An interactive **C/C++ call-tree viewer** with a terminal user interface(TUI) using Ncurses. It parses C/C++ source files to build a **call graph**, which you can **browse interactively** expand/collapse nodes, navigate, and inspect function call relationships yourself with.

## Quick Start

### Build

```bash
make
```

This will compile both the library and the TUI frontend.

> **Note:** Requires **ncurses** development libraries.  
> On Ubuntu/Debian: `sudo apt install libncurses-dev`  
> On macOS: `brew install ncurses` (may need `-I/opt/homebrew/include -L/opt/homebrew/lib`)

### Usage

```bash
./calltui [directory]
```

If no directory is specified, the TUI opens in the **current working directory**. While keeping in mind it need a directory not a file.

#### What you get:
- **File browser** (left panel) to select `.c`/`.h` files
- **Call tree** (right panel) showing function calls starting from an entry point (default: `main`)
- **Interactive navigation** with expandable nodes, cycle detection, and unknown function indicators

---

## Interactive Controls

### General Keys
| Key | Action |
|-----|--------|
| `?` | Show help overlay |
| `q` | Quit |

### File Panel (left)
| Key | Action |
|-----|--------|
| `Up`/`Down` or `k`/`j` | Navigate files/directories |
| `Enter` | Enter a directory |
| `Backspace` | Go up one directory |
| `Space` | Toggle file selection |
| `a` | Select/deselect all `.c`/`.h` files in current directory |
| `t` | Switch to tree panel |
| `r` | Build tree from selected files *(only in tree panel)* |

### Tree Panel (right)
| Key | Action |
|-----|--------|
| `Up`/`Down` or `k`/`j` | Navigate call-tree nodes |
| `Space` / `Enter` | Expand/collapse a node |
| `e` | Set entry function (prompt appears at bottom) |
| `+` / `-` | Increase/decrease maximum recursion depth |
| `r` | (Re)build tree from selected files |
| `f` | Switch back to file panel |

---

## How It Works

The backend (`calltree.c`) parses C/C++ source files using a **heuristic tokenizer**:

- Skips comments, string literals, and preprocessor directives
- Detects function definitions when `identifier(` appears at depth 0
- Detects function calls when `identifier(` appears inside a function body
- Filters out C/C++ keywords to avoid false positives

The TUI frontend (`tui.c`) builds a tree structure from the entry function, recursively following function calls up to a configurable depth.

### Visual Indicators
- `▼` / `▶` — Expanded / collapsed node (click to toggle)
- `↺` — Cycle detected (recursive call) 
- `?` — Function not found (external/library call)
- Yellow — Entry point function
- Magenta — Cyclic node
- Red — Unknown function

---

## Project Structure

```
calltree/
├── calltree.c         # Parser core
├── calltree.h         # Public API
├── tui.c              # Ncurses TUI frontend
├── Makefile           # Build configuration
└── README.md          # This file
```

//without the .o file

---

## Notes & Limitations

- **Heuristic parser** — not a full C/C++ AST; may misidentify function-like macros or complex expressions as calls.
- **Ignores**:
  - Function pointers
  - C++ namespaces and overloads
  - Template instantiations
- **Max limits** (tunable in `calltree.c`):
  - `MAX_FUNCS 2048`
  - `MAX_CALLS 256` per function
  - `MAX_TREE_NODES 4096` in TUI

---

## License

This project is open-source and free to use. See the source files for details.

---

## Contributing

Feel free to open issues or submit pull requests. Contributions are welcome!