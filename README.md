# Shell
A Custom Shell for learning the Basics of OS, exploring the potentials of C and understanding the core of CLI

## Features
- Execute built-in commands: `cd`, `mkdir`, `touch`, `exit`, etc.
- Execute external programs using `fork()` and `exec()`
- Supports relative and absolute paths
- Handle multiple arguments per command
- Input/output redirection: `>`, `>>`, `<`
- Piping: `|`
- Quoted strings and escape characters
- Command history (optional)
- Job control (foreground/background)
- Signal handling: Ctrl+C, Ctrl+Z

## Tech Stack
- C / GCC compiler / Linux environment
- Git & GitHub for version control

## Folder Structure
- src/ → source code
- include/ → header files
- tests/ → test programs/scripts
- docs/ → documentation
- assets/ → diagrams/screenshots
- Makefile → build instructions

## Run
- In main.c at line 15 enter /home/[username]/myshell_history
- gcc src/main -o main
- ./main

## Testing
- cd tests
- ./test_myshell.sh
