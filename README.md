# DOK - Dynamic C Documentation System

A terminal-based interactive documentation tool for C projects. DOK helps you create, manage, and export comprehensive documentation for your C functions with an intuitive interface.

## Features

- **Interactive Terminal Interface** - Navigate through C files and functions with arrow keys
- **Function Discovery** - Automatically scans and parses C/H files in your project
- **Return Type Detection** - Automatically identifies function return types
- **Documentation Editor** - Built-in editor for function documentation with multiple fields
- **Multiple Export Formats** - Export documentation as TXT, Markdown, HTML, or PostScript
- **Search Functionality** - Find functions by name or documentation content
- **Undocumented Function Tracking** - Easily identify functions that need documentation
- **Progress Tracking** - Shows documentation coverage statistics

## Installation

```bash
git clone https://github.com/OscarJ12/dok.git
cd dok
gcc -o dok dok.c
```

## Usage

### Basic Usage

```bash
# Run DOK in current directory
./dok

# Run DOK in a specific project directory
./dok /path/to/your/c/project
```

### Navigation

- **↑/↓ Arrow keys** - Navigate through lists
- **Enter** - Select item/enter function detail view
- **'b'** - Go back to previous view
- **'q'** - Quit DOK

### Main Commands

- **'p'** - Print file documentation to terminal
- **'P'** - Export documentation to file (multiple formats)
- **'r'** - Rescan project files
- **'s'** - Search functions
- **'u'** - View undocumented functions
- **'e'** - Edit function documentation
- **'v'** - View function source code

### Documentation Fields

DOK supports comprehensive function documentation with these fields:

- **Description** - What the function does
- **Parameters** - Function parameters and their purposes
- **Return Value** - What the function returns
- **Example** - Usage examples
- **Notes** - Additional implementation notes

### Export Formats

Press 'P' to export documentation in multiple formats:

1. **Plain Text (.txt)** - Simple, printable format
2. **Markdown (.md)** - GitHub/web compatible
3. **HTML (.html)** - Browser-printable with CSS styling
4. **PostScript (.ps)** - Professional printing format

## Example Workflow

1. Navigate to your C project directory
2. Run `./dok`
3. Use arrow keys to browse files and functions
4. Press 'u' to see undocumented functions
5. Select a function and press 'e' to add documentation
6. Press 'P' to export your documentation when complete

## Documentation Storage

DOK stores documentation in a `.project_docs.txt` file in your project directory. This file is automatically created and updated as you add documentation.

## Sample Output

```
════════════════════════════════════════════════════════════════════════════════
                      DYNAMIC C PROJECT DOCUMENTATION                        
════════════════════════════════════════════════════════════════════════════════
📊 Project Stats: 3 files, 15 functions, 12 documented (80.0%)

SOURCE FILES
Use ↑/↓ to navigate, ENTER to view functions, 'p' to print file docs, 'P' to save printable docs, 'r' to rescan, 's' to search, 'u' for undocumented, 'q' to quit

► main.c (5 functions, 4 documented)
  utils.c (7 functions, 6 documented)
  parser.c (3 functions, 2 documented)
```

## Requirements

- GCC compiler
- Unix-like system (Linux, macOS)
- Terminal with ANSI color support

## License

This project is open source. Feel free to use and modify as needed.

## Author

Created as a practical tool for C developers who want to maintain good documentation practices without leaving the terminal.
