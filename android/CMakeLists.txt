# Android (arm64-v8a) build of the recompiled Diablo II: libd2recomp.so, loaded by the launcher's
# org.d2recomp.D2Activity (an SDLActivity) next to the launcher's own libSDL2.so.
#   cmake -S android -B build-android -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DCMAKE_BUILD_TYPE=Release
cmake_minimum_required(VERSION 3.18)
project(d2recomp C)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SDL_BUILD /home/lesad/project/wow/android-combo/wc3-native/sdl)
file(GLOB GEN_SRC ${ROOT}/gen/*.c)
file(GLOB RT_SRC ${ROOT}/runtime/*.c)
add_library(d2recomp SHARED ${GEN_SRC} ${RT_SRC})
target_include_directories(d2recomp PRIVATE ${ROOT}/runtime ${ROOT}/gen
    ${SDL_BUILD}/include/SDL2 ${SDL_BUILD}/include-config-release/SDL2 ${SDL_BUILD}/include-config-relwithdebinfo/SDL2)
target_compile_definitions(d2recomp PRIVATE D2_NO_TTF)
target_compile_options(d2recomp PRIVATE -O2 -fno-strict-aliasing -w)
add_library(SDL2 SHARED IMPORTED)
set_target_properties(SDL2 PROPERTIES IMPORTED_LOCATION ${SDL_BUILD}/libSDL2.so)
target_link_libraries(d2recomp SDL2 GLESv3 log m)
