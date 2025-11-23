# Run the build command
#ninja -C build -j 1
ninja -C build

# Check if --install is passed as an argument
if [[ "$1" == "--install" ]]; then
    # Run the install command with sudo
    #sudo ninja -C build install -j 1
    sudo ninja -C build install
    sudo ldconfig
fi

