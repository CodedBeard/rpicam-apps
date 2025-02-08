#!/bin/bash
# sudo apt install -y libcurl4-openssl-dev libavcodec-dev libavdevice-dev libopencv-dev
# meson setup build -Denable_libav=enabled -Denable_drm=enabled -Denable_egl=disabled -Denable_qt=disabled -Denable_opencv=enabled -Denable_tflite=disabled -Denable_hailo=disabled -Denable_imx500=true -Ddownload_imx500_models=true --reconfigure
# Run the build command
ninja -C build -j 1

# Check if --install is passed as an argument
if [[ "$1" == "--install" ]]; then
    # Run the install command with sudo
    sudo ninja -C build install -j 1
    sudo ldconfig
fi

