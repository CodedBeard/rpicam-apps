#!/bin/bash
sudo apt install -y libcurl4-openssl-dev libavcodec-dev libavdevice-dev libopencv-dev ninja-build meson python3
meson setup build -Denable_libav=enabled -Denable_drm=enabled -Denable_egl=disabled -Denable_qt=disabled -Denable_opencv=enabled -Denable_tflite=disabled -Denable_hailo=disabled -Denable_imx500=true -Ddownload_imx500_models=true --reconfigure
