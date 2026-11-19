# nextbuild

Сборщик проектов NextOS и языковой сервер для файлов
.nextbuild и .nextbuild-dependency. Оба входят в один исполняемый файл на C++23.

## Сборка

Нужны CMake 3.21+ и компилятор со стандартной библиотекой C++23.
На Windows можно использовать Visual Studio с компонентом Desktop development
with C++. На Linux и macOS подходит современный Clang. Для сборки проектов
с модулями нужен Clang с поддержкой -fmodule-output. Упаковка NXFL также
использует LLD и llvm-objcopy. Статические библиотеки собирает llvm-ar или ar.

    cmake -S . -B build
    cmake --build build --config Release
    ctest --test-dir build -C Release --output-on-failure
    cmake --install build --config Release

На Windows исполняемый файл называется nextbuild.exe. При использовании
генератора Visual Studio он находится в build/Release.

## Использование

    nextbuild --root /path/to/NextOS --list
    nextbuild --root /path/to/NextOS path/to/project
    nextbuild --root /path/to/NextOS --clean path/to/project
    nextbuild lsp --stdio

Без аргументов сборщик ищет непустые .nextbuild в дереве проекта. Параметр
--root задаёт @NEXTOS_MAIN_ROOT и базу относительных путей. Прежние команды
python build.py и python -m nextbuild.lsp заменены командами выше.
