# d2recomp

Статическая рекомпиляция Diablo II: Lord of Destruction 1.13c (x86 → C) с рантаймом на SDL2 / OpenGL ES 3:
ПК (Linux) и Android (вкладка Diablo II в лаунчере Blizz4A). Только для личного использования.

Игровых файлов и сгенерированного кода в репозитории нет: `gen/` создаётся из своих файлов игры 1.13c.

## Сборка

```sh
python3 tools/recomp.py /путь/к/игре gen   # Game.exe, D2*.dll, Storm.dll, Fog.dll... 1.13c -> gen/*.c
make -j16                                   # ПК: build/d2recomp
./run.sh 60                                 # окно; WIDE=1 — 16:9
```

Android (arm64), рядом с общим libSDL2.so лаунчера:

```sh
cmake -S android -B build-android -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DCMAKE_BUILD_TYPE=Release
cmake --build build-android -j16          # libd2recomp.so; Java-сторона: android/java/org/d2recomp/D2Activity.java
```

## Устройство

- `tools/analyze.py` — разбор PE, граф потока, таблицы переходов; `tools/recomp.py` — x86 → C (`void f_XXXXXXXX(Cpu*)`).
- `runtime/` — загрузчик и диспетчер (`main.c`), Win32 (`k32.c`, `user.c`), Glide3x на GLES3 (`glide.c`), звук
  (`dsound.c`), сеть на BSD-сокетах (`net.c`), сенсорное управление (`touch.c`), отладочный оверлей (`overlay.c`).

Автотесты на ПК: `D2AUTO="ms:x,y;ms:key=N;ms:shot=name;ms:dump"`, пальцы `ms:f1d=x,y` (с `D2TOUCH=1`).
