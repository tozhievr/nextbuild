# nextbuild

Сборщик проектов NextOS и языковой сервер для файлов .nextbuild и
.nextbuild-dependency. Сборщик компилирует C++ модули и ассемблер, создаёт
нативные приложения и статические библиотеки, упаковывает freestanding-образы
NextOS в формат NXFL. LSP поддерживает диагностику, автодополнение, справку и
переход к определениям переменных.

## Требования

| Компонент | Назначение |
| --- | --- |
| CMake 3.30+ и Ninja | Конфигурация, сборка и тестирование |
| LLVM Clang 22+ и системная стандартная библиотека C++ | Компиляция nextbuild в режиме C++26 и сгенерированного парсера в режиме C23 |
| Clang с поддержкой -fmodule-output | Сборка проектов с C++ модулями |
| llvm-ar или ar | Архивация статических библиотек |
| LLD и llvm-objcopy | Линковка и упаковка NXFL |
| Parsley 2.35.1 | Регенерация парсера после изменения грамматики |

На Windows запустите команды из Developer PowerShell for VS с установленным
Windows SDK. На macOS установите Xcode Command Line Tools и используйте
upstream LLVM, например из Homebrew. На Linux добавьте каталог bin выбранной
версии LLVM в PATH. Обычная сборка использует сохранённые результаты
генерации и выполняется без Parsley, Python и доступа к сети.

## Сборка и установка

    cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure
    cmake --install build --prefix build/install

Исполняемый файл находится в build/install/bin. На Windows имя файла
nextbuild.exe. Чтобы использовать команду из примеров, добавьте этот каталог
в PATH. Если Clang для тестовых проектов находится вне PATH, передайте
-DNEXTBUILD_TEST_CLANG=/path/to/clang++ при конфигурации.

## Использование

    nextbuild --root /path/to/NextOS --list
    nextbuild --root /path/to/NextOS apps/demo
    nextbuild --root /path/to/NextOS -B apps/demo
    nextbuild --root /path/to/NextOS --clean apps/demo

Цель можно указать каталогом проекта или файлом с расширением .nextbuild.
Без цели сборщик обходит дерево в поисках проектов с непустым файлом
.nextbuild. Каталог docs исключается из поиска. Параметр --root задаёт
@NEXTOS_MAIN_ROOT и базу относительных путей. Параметр -B принудительно
пересобирает артефакты, а -q скрывает обычный вывод. Команда --clean удаляет
каталоги сборки выбранных проектов. По умолчанию они находятся в .build/<имя>.

Пример apps/demo/.nextbuild для приложения с main в apps/demo/src.

    Project : Demo
    Version : 0.1.0
    SetProductKind : host
    SetStandard : c++26
    SetCompiler : clang++
    SetSource : @NEXTOS_MAIN_ROOT / apps / demo / src

SetStandard управляет стандартом исходников целевого проекта. Значение по
умолчанию для проекта — c++20. Кавычки нужны для значений с пробелами и
удобны для путей и флагов. Комментарии начинаются с # или //.

### Форматы сборки

| Тип | Конфигурация | Результат |
| --- | --- | --- |
| host | SetProductKind : host | Нативное приложение |
| library | SetLibraryKind : host или next | Статическая библиотека и каталог pcm |
| nxflat | SetExecutableLayout с именами секций и адресами | .nxflat и .labels.json |

Записи блока SetExecutableLayout имеют вид - start = 0x10000.
По умолчанию NXFL и библиотеки профиля next собираются под
x86_64-unknown-none-elf. Манифест может задать компилятор, стандарт,
целевую платформу, флаги, зависимости и каталог результатов.

Чтобы использовать экспорт другой библиотеки, укажите в проекте
UseDependency с путём к .nextbuild-dependency. В этом файле RequireProject
задаёт путь к проекту поставщика, AddModulePath : @DEP_PCM подключает BMI, а
LinkLibrary : @DEP_LIBRARY передаёт архив линковщику. Сборщик обновляет
compile_commands.json в каталоге сборки и объединённую базу в корне.

### Настройка редактора

Укажите nextbuild как исполняемый файл языкового сервера и передайте ему
аргументы lsp и --stdio. Зарегистрируйте сервер для файлов .nextbuild и
.nextbuild-dependency. Корень рабочей области передаётся через LSP initialize.

## Структура проекта

| Каталог | Назначение |
| --- | --- |
| src/app | Точка входа и выбор CLI или LSP |
| src/build | Конфигурация проектов, компиляция, линковка и NXFL |
| src/manifest | Адаптер Parsley, модель и вычисление выражений |
| src/lsp | Обработчики LSP и справочник директив |
| src/platform | Запуск дочерних процессов Windows и POSIX |
| src/support | Работа с файлами, путями и JSON |
| grammar | Исходная грамматика манифеста |
| generated/manifest | Вывод генератора Parsley |
| tests | Тесты манифеста, LSP и сборки продуктов |

## Обновление грамматики

Внесите изменения в grammar/NextbuildManifest.parsley и пересоздайте
парсер той же версией Parsley. Если в PATH установлена другая версия,
передайте путь к нужному бинарнику при конфигурации.

    cmake -S . -B build -DPARSLEY_EXECUTABLE=/path/to/parsley
    cmake --build build --target manifest-regenerate
    cmake --build build --target manifest-check
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

Оба таргета запускают примеры из грамматики перед генерацией.
manifest-check сравнивает свежий вывод с файлами в generated/manifest.
Изменения грамматики и сгенерированные файлы следует сохранять вместе.
Парсер подключается статически через C ABI. Его лицензия хранится в
generated/manifest/LICENSE и устанавливается вместе с программой.
