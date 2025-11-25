#!/bin/bash

# ANSI codes for bold text
BOLD="\033[1m"
RESET="\033[0m"

# Function to print description in bold with an empty line before
print_desc() {
    echo
    echo -e "${BOLD}$1${RESET}"
}

# Test echo
print_desc "echo Hello World:"
echo Hello World

print_desc "echo Hello World with single quotes:"
echo '   to   one outside '

print_desc "echo Hello World with double quotes:"
echo "    of my shell     f "

# Test pwd
print_desc "pwd:"
pwd

# Test ls
print_desc "ls:"
ls

print_desc "ls -a:"
ls -a

print_desc "ls -l:"
ls -l

# Test cd
print_desc "cd ..:"
cd ..; pwd

print_desc "cd tests:"
cd tests; pwd

# Test mkdir
print_desc "mkdir t:"
mkdir t

print_desc "mkdir t1 t2 t3 t4 t5 t6 t7 t8:"
mkdir t1 t2 t3 t4 t5 t6 t7 t8

print_desc "ls:"
ls

# Test rm
print_desc "rm -r t1 t2 t3 t4 t5 t6 t7 t8:"
rm -r t1 t2 t3 t4 t5 t6 t7 t8

print_desc "touch new_file.txt:"
touch new_file.txt

print_desc "ls:"
ls

print_desc "rm -r t new_file.txt:"
rm -r t new_file.txt

# Test history
print_desc "history:"
history
