## What is it about?
This is a GUI for https://github.com/cdhooper/mx29f1615_programmer, a hardware/software solution to programm mx29f1615 flash. See the project description for details.

Those flash can be utilized in Amiga Computers and can easily hold 4 different Amiga ROM Images typically provided as .bin binaries.

The GUI allows for either .rom or .bin files which are correctly byte swapped given a .rom file. Files smaller 512kB are concatenated to fill the corresponding bank.

Empty Banks are filled with ff.

Each programming step saves the current buffer with a timestamp. Images can also be read from a flash and saved to disk.

The GUI includes most or all functions available in command line.

## Screen
![mxprog_gui](https://github.com/user-attachments/assets/c51b2e0c-54e0-471f-968d-861f6ed38431)


## Dependencies:

* mxprog installed (see above)
* CMake ≥ 3.21
* C++17-Compiler (GCC ≥ 9 oder Clang ≥ 10)
* Qt 6: Module Widgets und SerialPort
* (Ninja)
* (usbipd for WSL)

## Ubuntu / Debian:

sudo apt update

sudo apt install -y 
build-essential cmake ninja-build qt6-base-dev qt6-base-dev-tools qt6-serialport-dev

(For wsl usbipd is needed for USB discovery, see https://learn.microsoft.com/en-us/windows/wsl/connect-usb)

## Fedora:

sudo dnf install -y gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qtserialport-devel

## Arch / Manjaro:

sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-serialport

## openSUSE (Leap/Tumbleweed):

sudo zypper install -y gcc-c++ cmake ninja libqt6-qtbase-devel libqt6-qtserialport-devel

## macOS (Intel & Apple Silicon)

xcode-select --install

brew install qt ninja cmake

# Build Instructions
## Linux (all Architectures, native):

cd /path/to/mxprog-gui

cmake -S . -B build -G Ninja

cmake --build build -j

./build/mxprog_qt

## macOS (Intel/ARM, native):
cd /path/to/mxprog-gui

QP="$(brew --prefix qt)"

cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$QP"

cmake --build build -j

## App-Bundle:
open build/mxprog_qt.app

## or Symlink:
./build/mxprog_qt


Deployment (macOS App-Bundle, included QT6):

QP="$(brew --prefix qt)"
"$QP/bin/macdeployqt" build/mxprog_qt.app
