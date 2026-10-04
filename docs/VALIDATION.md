# Выполненная проверка 0.2.0

Дата: 2026-10-01. Среда: Linux, GCC 13.3, C++17. Проверяется предоставленный source package, а не установленный Windows OBS.

| Проверка | Результат |
|---|---|
| CMake 3.28.3 configure/build с BUILD_PLUGIN=OFF | успешно |
| CTest animation-engine | успешно, 3248 assertions/checks |
| CTest original-shader-preserved | успешно |
| Все четыре .cpp: syntax-only с настоящими OBS 31.1.2 headers и Qt 6.4.2 headers | успешно, без warnings проекта при -Wall -Wextra |
| ASan + UBSan для animation-tests | успешно; LeakSanitizer отключён из-за ограничений /proc/ptrace среды |
| Byte-for-byte original shader body в standalone wrapper | подтверждено |
| JSON examples | разобраны стандартным JSON parser |
| Windows MSVC build/link | не выполнялись |
| Загрузка DLL в OBS, GPU compiler D3D11/OpenGL, реальные кадры | не выполнялись |
| GUI visual/lifecycle тест в живом OBS | не выполнялся |
| Реальное соединение obs-websocket, auth и vendor roundtrip | не выполнялись |
| GPU/VRAM benchmark A/B на машине пользователя | не выполнялся |

Ядро покрывает endpoints всех 31 easing, конечность результатов на сетке progress, reference EaseOutCubic, overshoot Back, duration 30/60/120 FPS, несколько последовательных retargets, накопительный signed ADD, ADD в Return, Hold, отрицательный возврат, большой frame gap, duration=0, Stop, bool threshold, 30 независимых траекторий и отклонение NaN.

Оптимизированный CPU-only probe показал около 0.48 μs на sample 30 активных параметров с разными easing в этой среде. Это микротест горячего кода с повторяющимся progress, не стоимость реального OBS: нет mutex contention, OBS API, Qt, graphics setters, driver/GPU или video rendering. Он не является обещанием нагрузки на ПК пользователя.

SHA-256 предоставленного оригинала:

```text
9fd7939d84f38f309eae3d84b5e1191f616c1f4fb9ab8e73607e6208fbbf72f3
```

Original `distortZone`, `mainImage`, aspect corrections, uniforms/defaults и шесть зон сохранены без изменений. Standalone effect добавляет только host wrapper; фактическую совместимость sampler/color pipeline проверять по PERFORMANCE.md.

После Windows сборки обязательно выполнить: загрузку/компиляцию shader, retarget/ADD/return проверки, удаление источника при открытом dock, смену scene collection, vendor запросы по именам/UUID и визуальную сверку оригинального шестизонного shader.

## Автоматические тесты

`ctest` запускает три теста:

| Тест | Что проверяет |
|---|---|
| `animation-engine` | CPU-движок: easing, траектории, ADD/return, контракты утилит |
| `original-shader-preserved` | исходный шейдер не изменён (`original.sha256`) |
| `zone-canvas-interaction` | визуальный редактор: перетаскивание центра и края, колесо, Shift+колесо, двойной клик, выбор зоны, независимость Base/Target, клампы |

`zone-canvas-interaction` собирается вместе с плагином (цель `canvas-preview`) и
прогоняет реальные обработчики событий Qt через `QCoreApplication::sendEvent`,
поэтому проверяет именно ту логику, которая работает в dock-панели. Тот же
executable без `--selftest` рендерит канвас в PNG:

```powershell
build\canvas-preview.exe C:\temp\canvas-out
```

## Жизненный цикл в живом OBS

Дата: 2026-10-02. Портативный (portable) OBS Studio 32.2.1 x64, Qt 6.11.1,
собранная `stage`-версия плагина.

### Исправление: OBS падал при штатном закрытии

Первая сборка 0.2.0 завершалась **access violation при каждом нормальном выходе
из OBS** — воспроизведено 2 раза из 2 в чистом профиле. Crash-лог
`config\obs-studio\crashes\Crash *.txt`:

```text
Unhandled exception: c0000005
w32-pthreads.dll!pthread_mutex_unlock+0x1c
obs.dll!proc_handler_call+0x65
obs-parameter-animator.dll!unregister_websocket_vendor+0xcb
obs-parameter-animator.dll!obs_module_unload+0x9
obs.dll!free_module+0x3e
obs.dll!obs_shutdown+0x6ff
```

`obs_shutdown()` освобождает модули в порядке загрузки, поэтому obs-websocket
(и его proc-handler, который кэширует `obs-websocket-api.h`) исчезает раньше
нашего модуля. Вызов vendor API из `obs_module_unload()` уходит в
`proc_handler_call()` по уже освобождённому handler-у.

Что сделано:

* vendor-запросы снимаются по `OBS_FRONTEND_EVENT_EXIT` (frontend и все модули
  ещё живы), см. `src/ws_vendor.cpp`;
* `obs_module_unload()` больше не вызывает чужие API вообще (только забывает
  указатели), см. `src/plugin-main.cpp`; dock-виджет принадлежит главному окну
  OBS и уничтожается вместе с UI.

Проверка после исправления:

| Сценарий | Результат |
|---|---|
| чистый профиль, запуск → закрытие окна OBS | процесс завершился, нового crash-лога нет |
| источник с активным фильтром (`mode=2`, анимация идёт) → закрытие окна | процесс завершился, нового crash-лога нет |
| `[OPA] Version 0.2.0 loaded` в логе | есть в обоих запусках |
| импорты DLL против настоящего `C:\Program Files\obs-studio` | `ALL_IMPORTS_RESOLVED` |

### Исправление: панель перерисовывалась впустую

Dock-панель опрашивает движок каждые 100 мс. Раньше часть обновлений вызывала
repaint/layout даже когда ничего не менялось (на вкладке Table при отсутствии
выбранного фильтра `clearContents()` вызывался 10 раз в секунду, на вкладке Log
`metrics->setText()` заново размечал word-wrap label, канвас получал `update()`
без изменений, список фильтров мог пересобираться по таймеру 2 с из-за
неупорядоченного `obs_enum_sources()`). Теперь:

* текст и значения сравниваются перед записью (`metrics`, `zoneLabel`, `hint`);
* `ZoneCanvas::setLive()` не перерисовывается, если live-значения те же;
* таблица очищается один раз, а не каждый тик;
* список фильтров сравнивается как множество (порядок перечисления не важен).

Проверка: `ctest` — 3/3 (включая `zone-canvas-interaction`, который бьёт по тем
же обработчикам), сборка без предупреждений.

## Dock-панель: одна позиция зоны, накопительный Add и возврат

Дата: 2026-10-02. Тот же портативный OBS 32.2.1.

Изменения модели:

* У зоны одна позиция. Переключатель `Edit: Base / Target`, `show other state`,
  стрелка «база → цель» и кнопки `Target = Base` убраны: положение зоны — это
  состояние покоя и одновременно endpoint возврата. При записи позиции
  (`zoneN_*`) панель обновляет и `zoneN_*_target`, и `zoneN_*_return_value`,
  поэтому `Start`/`StartAll` больше не оставляют устаревшую цель.
* На первой вкладке появились блоки `Duration and return` (Move duration/easing,
  auto return, `Return delay after the last Add`, Return duration/easing и
  кнопки `Apply to this zone` / `Apply to all zones`) и `Test` (Parameter,
  Value / delta, `Add (accumulate)`, `Set`, `Start saved`, `Stop`,
  `Back to rest`).
* `Reset` возвращает параметр к сохранённому `Return endpoint`, а не к нулю.
* Дефолт `zoneN_*_hold_ms` — 200 ms: короткая пауза после последнего `Add`
  перед возвратом.
* Тест `animation-engine` дополнен серией накопительных `Add`: накопление
  дельт, перезапуск задержки последним `Add`, возврат к значению покоя
  (3254 проверки).

Проверка в живом OBS (портативный профиль, фильтр `OPA_PANEL_193929` /
`Distortion`, значения зоны 1: center 30/30, radius 22, magnitude 0.8):

| Шаг | Наблюдение |
|---|---|
| `Docks → MoskiAutomator` | панель появляется: вкладки Visual / Table / Log, обе новые группы на первой вкладке |
| Значения блока | Move duration 1000 ms, Return delay 200 ms (новый дефолт), Return duration 1000 ms, auto return включён, Parameter = Magnitude, Value / delta = 0.3 |
| Задать Move duration 3000 ms, Return delay 2000 ms, Return duration 4000 ms и нажать `Add (accumulate)` | `zone1_magnitude` 0.8000 → 1.1000 (состояние ANIMATE: накопленная дельта +0.3 к позиции), затем RETURN → 0.8000 и IDLE |
| Вкладка Table | current / GPU value / target / progress / state соответствуют траектории; после возврата target = 0.8000 (позиция зоны, а не 0) |
| Закрытие OBS после работы с панелью | штатное завершение, новых crash-логов нет |


## Пересборка и проверка под OBS 32.1.0

Дата: 2026-10-03. Задача — перевести плагин с OBS 32.2.1 (Qt 6.11.1) на **OBS 32.1.0**.

### Что и почему менялось

* Прежняя DLL была собрана против SDK libobs 32.2.1 (`LIBOBS_API_MINOR_VER 2`) и
  Qt 6.11.1. OBS **отвергает** модуль, собранный под более новый libobs, поэтому в
  OBS 32.1.0 (`LIBOBS_API_MINOR_VER 1`) она не грузилась. Одной правки строк
  версии в документации недостаточно — нужна пересборка против заголовков 32.1.0
  и Qt 6.8.3.
* Собран «лёгкий» SDK libobs/obs-frontend-api **32.1.0**:
  * заголовки — из исходников OBS tag `32.1.0` (`libobs/`, `frontend/api/`);
  * `libobs/obs-config.h` этого tag даёт API `32.1.0` (MAJOR 32, MINOR 1, PATCH 0);
  * import-библиотеки `obs.lib` (1782 символа) и `obs-frontend-api.lib`
    (96 символов) сгенерированы из **реальных DLL** OBS 32.1.0
    (`dumpbin /exports` + `lib /def`).
* Qt — **6.8.3**, ровно та версия, что идёт внутри OBS 32.1.0: OBS 32.1.0 тянет
  obs-deps `2025-08-23`, а `deps.qt/qt6.ps1` этого тега содержит
  `Version = '6.8.3'`. Взят готовый бинарный Qt из
  `windows-deps-qt6-2025-08-23-x64.zip` (тот самый сбор, что кладёт в OBS).
* Исходный код плагина **не менялся** — он использует только давно стабильные
  OBS-символы. В `CMakeLists.txt` поправлен лишь тест `zone-canvas-interaction`:
  `QT_QPA_PLATFORM=offscreen\;minimal`, потому что Qt из obs-deps не содержит
  платформенного плагина `offscreen`, и без fallback тест не стартовал.

### Команды сборки

```powershell
cmake -S . -B build-32.1.0 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  "-DCMAKE_PREFIX_PATH=C:/dev/obs-sdk-32.1.0;C:/dev/qt6-6.8.3" -DBUILD_TESTING=ON
cmake --build build-32.1.0
ctest --test-dir build-32.1.0 --output-on-failure
cmake --install build-32.1.0 --prefix stage
```

### Результаты

| Проверка | Результат |
|---|---|
| `ctest` (animation-engine, original-shader-preserved, zone-canvas-interaction) | 3/3 passed |
| Зависимости DLL | `obs.dll`, `obs-frontend-api.dll`, `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`, CRT |
| Импорты против DLL из OBS 32.1.0 | `ALL_IMPORTS_RESOLVED` (obs 78, obs-frontend-api 3, Qt6Core 56, Qt6Gui 48, Qt6Widgets 497) |
| Запуск портативного OBS 32.1.0 x64 | `OBS 32.1.0 (64-bit, windows)`, `Qt Version: 6.8.3`, `[OPA] Version 0.2.0 loaded`, фильтр `opa_6zone_distortion` создан |
| Совместимость с 32.2.x | та же DLL грузится в OBS 32.2.1 (Qt 6.11.1): API 32.1 ≤ 32.2, Qt 6.8.3 совместим со свежими 6.x |

Итог: пересобранная DLL работает в OBS 32.1.0 и остаётся совместимой с 32.2.x.

