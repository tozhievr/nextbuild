# Nextbuild

[![Сборка и тесты](https://github.com/tozhievr/nextbuild/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/tozhievr/nextbuild/actions/workflows/build.yml)
[![Релиз](https://img.shields.io/github/v/release/tozhievr/nextbuild)](https://github.com/tozhievr/nextbuild/releases/latest)
[![Лицензия](https://img.shields.io/github/license/tozhievr/nextbuild)](LICENSE)

[English](README.md) · **Русский**

Nextbuild собирает проекты на C и C++ по манифестам `.nextbuild`. Он
определяет порядок сборки именованных модулей, отслеживает заголовки и BMI
и выпускает исполняемые файлы, статические библиотеки и образы NXFL.
Тот же бинарник запускает языковой сервер для VS Code и Cursor.

[Установка](#установка) · [Быстрый старт](#быстрый-старт) ·
[Редакторы](#редакторы) · [Справочник манифеста](docs/manifest.ru.md) ·
[Разработка](CONTRIBUTING.md)

## Установка

Готовые бинарники доступны для **Windows x64**, **Linux x86-64** и **macOS
Apple Silicon**. Установщики скачивают последний стабильный релиз и
проверяют контрольную сумму SHA-256. Для установки в домашний каталог
права администратора не нужны.

**Windows PowerShell**

```powershell
& ([scriptblock]::Create((Invoke-RestMethod 'https://raw.githubusercontent.com/tozhievr/nextbuild/main/install.ps1')))
```

**Командная строка Windows**

```bat
curl.exe -fsSL "https://raw.githubusercontent.com/tozhievr/nextbuild/main/instal.bat" -o "%TEMP%\nextbuild-install.bat" && call "%TEMP%\nextbuild-install.bat"
```

**Linux и macOS**

```sh
( tmp=$(mktemp) && trap 'rm -f "$tmp"' EXIT && curl -fsSL 'https://raw.githubusercontent.com/tozhievr/nextbuild/main/install.sh' -o "$tmp" && sh "$tmp" )
```

На Windows бинарник устанавливается в `%LOCALAPPDATA%\Nextbuild\bin`, и
установщик добавляет этот каталог в пользовательский PATH. При необходимости
откройте новый терминал. На Linux и macOS каталог по умолчанию — `$HOME/.local/bin`.
При необходимости добавьте `export PATH="$HOME/.local/bin:$PATH"` в настройки
оболочки.

Проверьте установку командой `nextbuild --version`. Для обновления можно
повторить ту же команду установки. Чтобы выбрать версию или прочитать скрипт
до запуска, скачайте его и выполните подходящую команду:

```powershell
.\install.ps1 -Version 0.2.1 -BinDir "$HOME\bin" -NoPath
```

```bat
instal.bat -Version 0.2.1 -BinDir "%USERPROFILE%\bin" -NoPath
```

```sh
sh install.sh --version 0.2.1 --bin-dir "$HOME/bin"
```

Для ручной установки [скачайте архив](https://github.com/tozhievr/nextbuild/releases/latest),
проверьте его по `SHA256SUMS` и извлеките бинарник и уведомление о лицензии.
Для удаления Nextbuild удалите бинарник и `nextbuild-Parsley-LICENSE` из
каталога установки. Если каталог был добавлен в PATH только для Nextbuild,
удалите и эту запись.

## Быстрый старт

Создайте `.nextbuild` в корне проекта:

```text
Project : Hello
SetSource : @PROJECT_ROOT / src
SetStandard : c++23
```

Добавьте `src/main.cpp`:

```cpp
#include <iostream>

int main() {
    std::cout << "Hello from Nextbuild\n";
}
```

Выполните `nextbuild` в корне проекта. Бинарник появится в
`.build/hello/hello` или `.build/hello/hello.exe` на Windows.
В релиз входят сборщик и языковой сервер. Для компиляции проектов нужен
Clang, для статических архивов — `llvm-ar` или `ar`, для NXFL —
`ld.lld` и `llvm-objcopy`.

| Команда | Действие |
| --- | --- |
| `nextbuild --list` | Показать манифесты в рабочей области |
| `nextbuild apps/hello` | Собрать каталог или конкретный файл `.nextbuild` |
| `nextbuild -B apps/hello` | Принудительно перекомпилировать исходники |
| `nextbuild --clean apps/hello` | Удалить артефакты проекта |
| `nextbuild --root DIR --list` | Явно задать корень рабочей области |
| `nextbuild -q` | Скрыть обычный вывод |

Запустите `nextbuild -j 8` для параллельной сборки или
`nextbuild --graph > build.dot` для экспорта графа зависимостей.
[Руководство по System Runtime](docs/runtime.md) описывает режимы вывода,
цвета и API запуска подпроцессов.

[Справочник манифеста](docs/manifest.ru.md) описывает типы продуктов,
экспорт зависимостей, флаги и размещение секций NXFL.

## Редакторы

Скачайте VSIX из [последнего релиза](https://github.com/tozhievr/nextbuild/releases/latest).
В VS Code или Cursor выполните **Extensions: Install from VSIX…** и откройте
манифест в доверенной рабочей области. Расширение добавляет подсветку,
автодополнение, диагностику, подсказки, переход к определениям и символы
документа. В палитре есть **Nextbuild: Build** и **Nextbuild: Clean**.

Расширение запускает `nextbuild lsp --stdio`. Если редактор не видит
бинарник, перезапустите редактор после изменения PATH или задайте абсолютный
путь в `nextbuild.executablePath`.
[Документация расширения](extension/README.md)

## Сборка из исходников

Нужны CMake 3.30+, Ninja и LLVM Clang 22+ с `clang-scan-deps`. На Windows
используйте Developer PowerShell for Visual Studio с Windows SDK. На macOS
используйте upstream LLVM и Xcode Command Line Tools.

```sh
git clone https://github.com/tozhievr/nextbuild.git
cd nextbuild
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix build/install
```

Чтобы собрать Nextbuild его собственным манифестом, используйте
установленный бинарник или начальную сборку CMake:

```sh
nextbuild --root . .nextbuild
```

Результат самосборки находится в `.build/nextbuild/`. Работа с грамматикой
и PSL описана в [руководстве разработчика](CONTRIBUTING.md#manifest-parser-and-semantics).
Версии релизов берутся из [VERSION](VERSION), CI собирает их на трёх ОС.

## Лицензия

[MIT](LICENSE). Сгенерированный парсер Parsley распространяется с его
[уведомлением о лицензии](generated/manifest/LICENSE).
