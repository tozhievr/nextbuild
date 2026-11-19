# Справочник манифеста

[English](manifest.md) · **Русский** · [README](../README.ru.md)

Манифест — текстовый файл UTF-8. Директивы и присваивания переменных имеют форму `имя : значение`. Имена директив чувствительны к регистру; комментарии начинаются с `#` или `//`.

```text
Project : Example
Version : 1.2.0
@Sources : @PROJECT_ROOT / 'source tree'
SetSource : @Sources
EnableCompileFlag : '-Wall'
EnableCompileFlag : '-Iinclude'
```

Манифест поддерживает одинарные и двойные кавычки, логические значения `true` и `false`, десятичные и шестнадцатеричные числа, версии с точками и блоки. Оператор `/` соединяет части пути. Сборщик разрешает относительные пути от `@PROJECT_ROOT`. Переменные вычисляются в порядке объявления.

## Директивы проекта

| Директива | Повтор | Назначение |
| --- | --- | --- |
| `Project` | — | Имя проекта. Из него берётся имя артефакта по умолчанию |
| `Version` | — | Версия проекта в выводе сборки |
| `SetVersionFile` | — | Читает версию из файла и задаёт макрос `NEXTBUILD_VERSION` |
| `SetSource` | — | Основной файл или каталог исходников |
| `AddSource` | Да | Дополнительный файл или каталог исходников |
| `SetBuildRoot` | — | Каталог сборки. По умолчанию `.build/<имя>` |
| `SetOutputName` | — | Переопределяет имя артефакта |
| `SetProductKind` | — | `host`, `library` или `nxflat` |
| `SetLibraryKind` | — | `host` или `freestanding` |
| `SetExecutableLayout` | — | Имена и адреса секций NXFL |
| `SetTargetTriple` | — | Целевая платформа для Clang |
| `SetCompiler` | — | Исполняемый файл или путь к Clang-совместимому компилятору |
| `SetMinCompilerVer` | — | Минимальная версия компилятора |
| `SetStandard` | — | Стандарт C++. По умолчанию `c++20`. Файлы C используют C23 |
| `EnableCompileFlag` | Да | Один аргумент компилятора |
| `EnableLinkFlag` | Да | Один аргумент линковщика |
| `AddDefine` | Да | Макрос без префикса `-D` |
| `AddModulePath` | Да | Каталог с совместимыми предварительно собранными BMI |
| `LinkLibrary` | Да | Путь к библиотеке для линковки |
| `Depend` | Да | Сначала собрать другой проект |
| `UseDependency` | Да | Подключить экспорт из `.nextbuild-dependency` |
| `SetNextBuildDrawProgress` | — | Показывать прогресс при `true` |
| `SetGeneratePlatformModule` | — | Генерировать `cmake_generated` для библиотеки |
| `EnableFeature` | Да | Включить тег возможностей в `cmake_generated` |

## Типы продуктов

Обычный проект даёт нативное приложение. Для статического архива используйте `SetProductKind : library`. Профиль `freestanding` по умолчанию собирается для `x86_64-unknown-none-elf`.

```text
SetProductKind : nxflat
SetExecutableLayout : {
    - start = 0x10000
}
EnableLinkFlag : '-Wl,--gc-sections'
```

NXFL использует промежуточный x86-64 ELF. Линковщик находит секции `.start` и `.text.start`, размещает их по заданным адресам и сохраняет карту в `.labels.json`. Здесь `start` также задаёт символ точки входа.

## Экспорт зависимостей

Добавьте в проект-потребитель `UseDependency : @PROJECT_ROOT / libs / core / '.nextbuild-dependency'` и создайте указанный файл.

```text
Dependency : Core
RequireProject : @PROJECT_ROOT / libs / core
AddModulePath : @DEP_PCM
LinkLibrary : @DEP_LIBRARY
```

`RequireProject` задаёт проект-поставщик. `@DEP_BUILD_ROOT`, `@DEP_PCM` и `@DEP_LIBRARY` доступны при обработке экспортируемых путей. Файл экспорта также принимает `Version`, `LibraryKind`, `EnableCompileFlag`, `EnableLinkFlag`, `AddDefine` и вложенные `UseDependency`.

## Артефакты и диагностика

Сборщик пишет `compile_commands.json` в каталог сборки и обновляет общую базу в корне. Изменения команд, заголовков или BMI инвалидируют соответствующие объекты. Для просмотра сообщений компилятора запустите сборку без `-q`.

Если BMI несовместимы, пересоберите поставщика и потребителя тем же Clang с одинаковым стандартом и целевой платформой. Если индексация в редакторе пустая, проверьте `nextbuild.executablePath` и канал **Nextbuild** в панели Output.
