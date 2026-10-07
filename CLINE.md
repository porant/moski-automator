# CLINE.md — рабочие указания для агента Cline

Краткая шпаргалка: как собрать, протестировать, уложить в `stage` и задеплоить этот OBS-плагин
именно на этой машине. Не пересказывает архитектуру — см. `docs/` и `README.md`.

## Окружение (уже установлено)

- **Visual Studio 18 Community**, toolset MSVC 14.50.
  vcvars: `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat`
  (или `...\VC\Auxiliary\Build\vcvars64.bat`)
- **CMake 4.4.3** — доступен как `cmake` в PATH.
- **OBS SDK 32.1.0**: `C:/dev/obs-sdk-32.1.0`   ← ДЛЯ ТЕКУЩЕЙ ЦЕЛИ (OBS 32.1.0)
- **Qt 6.8.3** (для OBS 32.1.0): `C:/dev/opa-sdk/qt6-raw-6.8.3`
- (Альтернатива) OBS SDK 32.2.1 `C:/dev/obs-sdk` + Qt 6.11.1 `C:/dev/opa-sdk/Qt`.
- **OpenCV 4.10**: `C:/dev/opencv/opencv/build` (реальный путь к конфигу:
  `x64/vc16/lib/OpenCVConfig.cmake`; CMakeLists сам подберёт `OpenCV_RUNTIME=vc16`)

### ⚠️ ВАЖНО: сборка ДОЛЖНА совпадать с версией OBS/Qt

Цель сейчас — **OBS 32.1.0** (Qt 6.8.3). Набор `C:/dev/obs-sdk` + `C:/dev/opa-sdk/Qt`
(OBS 32.2.1, Qt 6.11.1) собирает плагин, который на OBS 32.1.0 (Qt 6.8.3) падает с
`Unhandled exception: c0000005`, Fault address в `qt6widgets.dll`, стек вида
`AnimatorPanel::pull ← select ← reload ← <таймер>` — это несовпадение ABI Qt.
Всегда собирай под набор, соответствующий запускаемому OBS:
его можно узнать в логе OBS по строке `Qt Version: <x> (runtime)`.

Готовый скрипт: `C:\dev\opa-sdk\build-32.1.0.bat` (конфигурирует, собирает, гоняет ctest,
ставит в `stage`). Он использует `C:/dev/obs-sdk-32.1.0` + `C:/dev/opa-sdk/qt6-raw-6.8.3`
и `-DOpenCV_DIR=C:/dev/opencv/opencv/build`.

### Отдельная засада: OpenCV_DIR кэшируется

При **чистой** конфигурации `find_package(OpenCV)` не находит OpenCV (`OpenCV_DIR-NOTFOUND`),
если не передать путь явно → face tracking молча выключается (в API не будет `OPA_FACE_TRACKING=1`,
пропадёт тест `head-pose`, станет 4 теста вместо 5). Всегда добавляй
`-DOpenCV_DIR=C:/dev/opencv/opencv/build`.

Каталог `build/` НЕ надо переиспользовать между разными наборами SDK/Qt: `Qt6_DIR` и
`libobs_DIR` кэшируются в `CMakeCache.txt`, и повторная конфигурация с другим
`CMAKE_PREFIX_PATH` их НЕ переберёт. Меняешь SDK → удаляй `build/` и конфигурируй заново.

## ВАЖНО: окружение MSVC

Простой `cmake --build` падает с `fatal error C1083: cannot open include file: 'type_traits'` —
компиляторный env не загружен. Всегда оборачивай build/install так:

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build build'
```

`cl.exe` вызывается с `/showIncludes`, поэтому в выводе масса строк «Примечание: включение файла».
Фильтруй результат:

```powershell
... 2>&1 | Select-String -Pattern 'error|warning C|Linking|FAILED|ninja' | Select-Object -Last 40
```

## Сборка (полный цикл под OBS 32.2.1)

Скрипт-обёртка «всё сразу» (configure + build + ctest + install в `stage`):

```powershell
C:\dev\opa-sdk\build-plugin.bat
```

Вручную (эквивалент; `OpenCV_DIR` обязателен, иначе face tracking пропадёт):

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_PREFIX_PATH=C:/dev/obs-sdk-32.1.0;C:/dev/opa-sdk/qt6-raw-6.8.3" -DBUILD_TESTING=ON -DOpenCV_DIR=C:/dev/opencv/opencv/build && cmake --build build && ctest --test-dir build --output-on-failure && cmake --install build --prefix stage'
```

Инкрементально (когда `build/` уже настроен):

```powershell
cmd /c '"C:\...\vcvars64.bat" >nul && cmake --build build'
```

Только плагин (без тестов):

```powershell
cmd /c '"C:\...\vcvars64.bat" >nul && cmake --build build --target obs-parameter-animator'
```

Успех = `Linking CXX shared module obs-parameter-animator.dll` и код возврата 0.
**Правило проекта: любая сборка завершается установкой в `stage`** — после `cmake --build build`
обязательно выполни `cmake --install build --prefix stage` и сверь хеши (раздел «Уложить в `stage`»).
При configure ожидай строку `-- OPA: face tracking enabled with OpenCV 4.10.0 (...)`.

## Тесты

```powershell
ctest --test-dir build --output-on-failure
```

6 тестов: `animation-engine`, `original-shader-preserved`, `shader-compiles`, `head-pose`,
`json-messages`,
`zone-canvas-interaction`. Ожидаемо `100% tests passed out of 6`. Если их 4 и нет `head-pose` —
OpenCV не подключился, пересобери с `-DOpenCV_DIR=C:/dev/opencv/opencv/build`.

## Уложить в `stage`

`build-plugin.bat` уже делает install. Вручную:

```powershell
cmd /c '"C:\...\vcvars64.bat" >nul && cmake --build build && cmake --install build --prefix stage'
```

Проверка, что stage свежий (хеши должны совпасть):

```powershell
Get-FileHash .\build\obs-parameter-animator.dll, .\stage\obs-plugins\64bit\obs-parameter-animator.dll -Algorithm SHA256
```

Раскладка `stage/` = то, что копируется в OBS:
`stage/obs-plugins/64bit/*.dll` и `stage/data/obs-plugins/obs-parameter-animator/*`
(`6-zone.effect`, `face-points.effect`, `face_detection_yunet_2023mar.onnx`, `original-6-zone.effect`,
`original.sha256` + `opencv_world4100.dll`).

## Деплой в OBS

```powershell
.\tools\deploy.ps1 -From stage       # текущая сборка
.\tools\deploy.ps1 -From known-good  # откат к проверенному бинарю
```

Скрипт сам запрашивает UAC и отказывается работать, пока запущен `obs64`.

## Структура исходников

- `src/ui.cpp` / `src/ui.hpp` — Qt-док (вкладки Visual / Table (advanced) / Log / JSON).
- `src/filter.cpp` / `filter.hpp`, `animation.hpp` — GPU-фильтр и движок анимации.
- `src/face_tracker.*`, `one_euro.hpp`, `head_pose.hpp` — трекинг лиц (YuNet).
- `src/ws_vendor.cpp` / `ws_vendor.hpp` — WebSocket Vendor API.
- `src/zone_canvas.hpp` — чистый Qt-виджет превью зон (используется и в `tests/canvas_preview.cpp`).
- `src/json_builder.hpp` — генератор CallVendorRequest-сообщений для вкладки JSON; тест `json-tests`.
- `src/preset.hpp` — импорт/экспорт настроек автоматизации (вкладка Import / Export) в JSON-текст
  для копипаста; тест `preset-import-export`.

## Конвенции и грабли

- **Синхронизация ползунок ↔ спинбокс** в доке (вкладка Visual) держится на `setPair(which, v)` в
  `src/ui.cpp`: пишет число в оба контрола, удерживая флаг `updating`, чтобы не поймать рекурсию
  через `valueChanged`. Масштаб ползунков: offset/radius ×10, magnitude ×1000 — лямбды
  `QSlider::valueChanged` делят обратно (`/10.0`, `/1000.0`). Любую правку диапазона слайдера
  синхронизируй с `addRow(...)` и с масштабом в `setPair`.
- Параметры точки: `pointCount = 3` (Forehead / Nose / Mouth), `pointParamCount = 5`
  (`enable`, `offset_x`, `offset_y`, `radius`, `magnitude`); `paramCount = 15`.
- Репозиторий source-only: `build/`, `stage/`, `dist/`, `known-good/`, `working/`, `*.dll` — в `.gitignore`.
- Редиска: правка многострочных строковых литералов через редактор бывает хрупкой из-за
  выравнивания пробелов (clang-format). Надёжнее делать точечные однострочные правки.
- ⚠️ **После любой правки конструктора `AnimatorPanel` обязательно проверяй, что ВСЕ виджеты-члены
  создаются.** Легко случайно удалить блок `= new ...` — тогда указатель остаётся `nullptr`, и
  `pull()` падает в `QAbstractButton::setChecked`/`QComboBox::...` на `this = nullptr`
  (`Unhandled exception: c0000005`, Fault address в `qt6widgets.dll`, стек `pull ← select ← reload`).
  Быстрая проверка:
  ```powershell
  Select-String -Path .\src\ui.cpp -Pattern '^\s*(\w+)\s*=\s*new ' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique
  ```
  В списке должны быть `modeBox, blurBox, blurPxSpin, debugBox, scaleBox, faceBox, smoothSpin,
  pointBox, enabledBox, durSpin, easing, autoReturn, holdSpin, returnSpin, returnEasing,
  faceHoldSpin, faceFadeSpin, testParam,
  testValue, table, value, duration, tableEasing, returnToBase, metrics, logs, logFilter, pause,
  jsonAction, jsonParam, jsonFormat, jsonValue, presetEdit, presetStatus,
  hint` (плюс `sl`/`sp` из `addRow` создают `cxSld/cySld/radSld/magSld` и `*Spin`, а
  `magBox` — `magMinSpin/magMaxSpin`).
- Как быстро найти строку краха по crash-логу (через PDB, без отладчика): в crash-логе есть таблица
  модулей с базовыми адресами; `функция+N` из нашего DLL → RVA; сигнатура `pull+0x7e3` и т.п.
  сопоставляется с исходной строкой утилитой `dbghelp` (`SymGetLineFromAddr64`).
- `data/6-zone.effect` — оригинальный шейдер, сохраняется байт-в-байт (проверяется тестом
  `original-shader-preserved`). Не трогать без причины.
