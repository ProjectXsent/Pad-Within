mkdir build
cd build
cmake .. -A "Win32" -DSDL2_DIR=".\SDL2\cmake"
cmake --build . --config Release
cd ..
pause