# Hamurabi LED Controller
# ESP32-C6 based multi-LED controller with MQTT voice control
#
# Features:
#   - 60× RGBW NeoPixels (SK6812)
#   - Charlieplex LED Matrix (IS31FL3731)
#   - nOOds 12V LED Filament
#   - Voice control via IFTTT + Adafruit IO

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(hamurabi_led_controller)
