Requirement:

CMake ≥ 3.21
C++17-Compiler (GCC ≥ 9 oder Clang ≥ 10)
Qt 6: Module Widgets und SerialPort
(Ninja)

Ubuntu / Debian:

sudo apt update
sudo apt install -y build-essential cmake ninja-build \
  qt6-base-dev qt6-base-dev-tools qt6-serialport-dev

Fedora:

sudo dnf install -y gcc-c++ cmake ninja-build \
  qt6-qtbase-devel qt6-qtserialport-devel

Arch / Manjaro:

sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-serialport

openSUSE (Leap/Tumbleweed):

sudo zypper install -y gcc-c++ cmake ninja \
  libqt6-qtbase-devel libqt6-qtserialport-devel

macOS (Intel & Apple Silicon)

xcode-select --install
brew install qt ninja cmake


Linux (all Architectures, native):

cd /path/to/mxprog-gui
cmake -S . -B build -G Ninja
cmake --build build -j
./build/mxprog_qt

macOS (Intel/ARM, native):
cd /path/to/mxprog-gui
QP="$(brew --prefix qt)"
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$QP"
cmake --build build -j
# App-Bundle:
open build/mxprog_qt.app
# or Symlink:
./build/mxprog_qt


Deployment (macOS App-Bundle, included QT6):

QP="$(brew --prefix qt)"
"$QP/bin/macdeployqt" build/mxprog_qt.app
