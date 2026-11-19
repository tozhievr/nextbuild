# nextbuild

`nextbuild` собирает обычные C++-проекты, проекты с именованными модулями и
freestanding-образы. Рецепты хранятся в `.nextbuild`; экспорт зависимостей
описывается в `.nextbuild-dependency`. Тот же исполняемый файл запускает
языковой сервер для обоих форматов.

## Быстрый старт

Сохраните в корне проекта файл `.nextbuild`:

```text
Project : Hello
SetSource : @PROJECT_ROOT / src
SetStandard : c++26
```

Добавьте `src/main.cpp`:

```cpp
#include <iostream>

int main() {
    std::cout << "Hello from nextbuild\n";
}
```

В корне проекта выполните `nextbuild`. Программа появится в
`.build/hello/hello` (`hello.exe` на Windows). Нативное приложение — тип
сборки по умолчанию. `SetStandard` управляет компилятором целевого проекта;
без этой директивы используется C++20.

## Установка из исходников

Нужны CMake 3.30+, Ninja и LLVM Clang 22+ с `clang-scan-deps`. На Windows
используйте Developer PowerShell for VS с Windows SDK. На macOS
используйте upstream LLVM и Xcode Command Line Tools. На Linux добавьте
каталог соответствующей версии LLVM в `PATH`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix build/install
```

Добавьте `build/install/bin` в `PATH`. Для сборки приложений и библиотек
нужен Clang, для архивов — `llvm-ar` или `ar`, для NXFL — LLD и
`llvm-objcopy`. Установленный `nextbuild` использует встроенный парсер.

## Команды

| Команда | Действие |
| --- | --- |
| `nextbuild` | Собрать проекты, найденные в рабочей области |
| `nextbuild apps/hello` | Собрать проект в каталоге или по пути к манифесту |
| `nextbuild --root DIR --list` | Показать найденные манифесты от указанного корня |
| `nextbuild -B apps/hello` | Принудительно пересобрать цель |
| `nextbuild --clean apps/hello` | Удалить каталог артефактов цели |
| `nextbuild lsp --stdio` | Запустить языковой сервер |

`--root` задаёт `@PROJECT_ROOT` и базу относительных путей. Без него
сборщик использует ближайший родительский каталог с `.nextbuild` либо
текущий каталог. `@NEXTOS_MAIN_ROOT` принимается как совместимый псевдоним
для существующих рецептов. Компиляция учитывает изменения исходников,
заголовков, модулей и аргументов компилятора.

### Типы продуктов

| Рецепт | Артефакт |
| --- | --- |
| `SetProductKind : host` | Нативное приложение |
| `SetProductKind : library` | Статический архив и BMI |
| `SetLibraryKind : freestanding` | Статический архив и BMI для bare-metal цели |
| `SetExecutableLayout : { ... }` | Образ NXFL и JSON-карта секций |

`SetTargetTriple` переопределяет целевой triple. Для NXFL размещение секций
задаётся адресами, например `- start = 0x10000`. NXFL использует промежуточный
x86-64 ELF. Базы `compile_commands.json` записываются в каталог сборки и
в корень рабочей области. Если библиотека должна экспортировать модуль
`cmake_generated`, включите `SetGeneratePlatformModule : True`.
Директива `EnableFeature` также включает этот модуль.

### Зависимости

Директива `Depend` задаёт порядок сборки. `UseDependency` загружает описание
экспорта, собирает поставщика и подключает его модули, архивы и флаги.
Пример файла `libs/core/.nextbuild-dependency`:

```text
Dependency : Core
RequireProject : @PROJECT_ROOT / libs / core
AddModulePath : @DEP_PCM
LinkLibrary : @DEP_LIBRARY
```

## Языковой сервер

Настройте в редакторе команду `nextbuild lsp --stdio` для файлов
`.nextbuild` и `.nextbuild-dependency`. Сервер предоставляет подсказки
директив и путей, справку, переход к определениям переменных и диагностику.
Семантика описана в PSL и проверяет имена директив, ссылки на переменные и
типы значений для путей, логических настроек и блоков размещения.

## Самосборка Nextbuild

Корневой `.nextbuild` компилирует модули C++26 и сгенерированные исходники
Parsley C23. Нужен Clang 22+ и установленный Nextbuild. Если бинарника ещё
нет, соберите его через CMake по инструкции выше.

```sh
nextbuild --root . .nextbuild
```

Результат находится в `.build/nextbuild/nextbuild` или
`.build/nextbuild/nextbuild.exe`. `AddSource` подключает дополнительные
исходники, а `SetVersionFile` читает `VERSION` и передаёт его компилятору.
В CI CMake собирает начальный бинарник, затем этот бинарник собирает и
проверяет релизную версию на Windows, Linux и macOS.

## Расширение для VS Code и Cursor

Установите архив `nextbuild-<версия>.vsix` из GitHub Release через команду
**Extensions: Install from VSIX**. Бинарник Nextbuild должен быть в
`PATH`; альтернативно укажите его путь в `nextbuild.executablePath`.
Расширение запускает сервер для манифестов и добавляет команды сборки,
очистки и перезапуска LSP. [Подробнее о расширении](extension/README.md)

## Версии и релизы

`VERSION` в корне задаёт версию CLI, LSP и расширения. Обновите все три
файла метаданных одной командой и закоммитьте результат:

```sh
node tools/version.mjs bump minor
node tools/version.mjs verify
```

Поддерживаются `patch`, `minor` и `major`. После изменения `VERSION` в
`main` GitHub Actions проверяет сборки, затем публикует тег `v<версия>`,
три архива с бинарниками, VSIX и `SHA256SUMS`. В Actions можно вручную
запустить **Release Nextbuild** и выбрать повышение версии. В этом режиме
workflow сам коммитит обновление в `main`, если правила ветки разрешают
это. Для повторной публикации уже подготовленной версии выберите `none`.

## Разработка

Точка входа находится в `src/app/main.cpp`. Остальной C++ расположен в
`.cppm` по подсистемам в `src/build`, `src/manifest`, `src/lsp`,
`src/platform` и `src/support`. В `generated/manifest` хранятся
сгенерированные файлы ISO C23 и лицензия Parsley.

При изменении грамматики или `NextbuildManifest.psem` используйте
Parsley 2.35.1:

```sh
cmake -S . -B build -DPARSLEY_EXECUTABLE=/path/to/parsley
cmake --build build --target manifest-regenerate
cmake --build build --target manifest-check
ctest --test-dir build --output-on-failure
```

Сохраняйте изменения в `grammar/` вместе с обновлёнными
`generated/manifest/`. `manifest-check` проверяет примеры и сравнивает
свежий вывод генератора с сохранёнными файлами.
