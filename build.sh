#!/bin/bash

# Run the build command
ninja -C build

# Check if --install is passed as an argument
if [[ "$1" == "--install" ]]; then
    # Run the install command with sudo
    sudo ninja -C build install
fi

