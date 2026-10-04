# OBS Parameter Animator 0.2.0

Нативный C++/Qt-плагин OBS: исходный шестизонный distortion-шейдер на GPU, независимые time-based анимации 30 параметров, easing, накопительные ADD, автоматический возврат, управление через OBS WebSocket Vendor API и визуальный редактор зон в dock-панели (зоны выставляются мышью прямо по кадру).

**Статус:** собранная Windows x64 DLL проверена в реальном OBS Studio 32.1.0 (и загружается в 32.2.x): модуль загружается, шейдер компилируется, все 30 параметров проходят round-trip через WebSocket vendor (`Set`/`Get`/`StartAll`), анимация идёт на GPU, визуальный редактор в dock-панели работает (перетаскивание зон пишет настройки фильтра; у зоны одна позиция, блоки `Duration and return` и `Test` — на первой вкладке). `ctest` — 3 теста, включая тест взаимодействий канваса. Подробности: [docs/VALIDATION.md](docs/VALIDATION.md). Штатное закрытие OBS завершается без crash-лога (проверено и в чистом профиле, и с включённым фильтром).

## Что изменилось относительно 0.1.0

Версия 0.1.0 была внешним для фильтров контроллером settings. В 0.2.0 приоритетом стал один собственный GPU-фильтр, содержащий именно предоставленный шейдер. obs-shaderfilter для него не требуется. Частые `obs_source_update()` заменены непосредственным заполнением uniforms в render callback. Таймер системного времени заменён `std::chrono::steady_clock`; WebSocket и GUI не выполняют графических операций.

Это целевая переработка: старые произвольные точки `point: RITM`, управление сторонними фильтрами и старое окно 0.1.0 не перенесены автоматически. Теперь адрес задаётся как источник + native filter + точное имя параметра либо UUID фильтра + parameterId. Действия Streamer.bot нужно обновить по [docs/API.md](docs/API.md). Имена Vendor и Add/Set/Reset/Get/List сохранены, формат requestData изменён.

## Быстрый запуск после сборки

1. Закрыть OBS, установить DLL и каталог data по инструкции ниже.
2. В фильтрах камеры или другого видеоисточника добавить **6-Zone Distortion + Parameter Animator**. Для примеров назвать источник `CAMERA`, фильтр `Distortion`.
3. В свойствах выбрать `Plugin animation`; оставить `Animation FPS = Match OBS rendering`.
4. В `Zone 1 → Magnitude`: target `1`, duration `1000` ms, easing `EaseOutCubic`, auto return включён, return value `0`, return duration `1000` ms. Нажать `Start` этой строки.
5. Значение плавно идёт от текущего к 1, затем к 0. Движение от отрицательной величины также поддерживается.
6. В меню **Docks / Док-панели** включить **Parameter Animator**. Вкладка **Visual** — визуальный редактор: круг с номером это зона, перетаскивание центра меняет center X/Y, перетаскивание за край — radius, колесо — magnitude, Shift+колесо — radius, двойной клик — вкл/выкл. У зоны одна позиция — состояние покоя, к которому всё возвращается. Блок **Duration and return** задаёт Move duration/easing, auto return, `Return delay after the last Add` и `Return duration`/easing (`Apply to this zone` / `Apply to all zones`), блок **Test** позволяет сразу проверить: выбрать Parameter, ввести Value / delta и нажать `Add (accumulate)`, `Set`, `Start saved`, `Stop` или `Back to rest`. Вкладка **Table (advanced)** — таблица 30 параметров и команды Add/Set/Stop/Reset, вкладка **Log** — метрики и лог.

В исходном шейдере radius и center выражены в процентах (0–100), magnitude — в диапазоне −1.3333…1.3333. Поэтому для magnitude отправлять, например, `+0.3`, а не `+30`.

## Сборка на Windows x64

Требуются Visual Studio 2022 (или новее) с **Desktop development with C++**, Windows SDK, CMake 3.28+, SDK libobs и obs-frontend-api, Qt 6 MSVC x64. Собирать с SDK версии OBS, на которой планируется запуск, и совместимым Qt из её набора зависимостей. Эта сборка сделана под **OBS 32.1.0** (SDK libobs/obs-frontend-api 32.1.0) и **Qt 6.8.3**; она же грузится в более новых OBS 32.x.

**Установленного OBS недостаточно:** нужны headers, import libraries `.lib`, CMake package files. Путь к `obs64.exe` не является SDK.

### Получение SDK из исходников OBS

Официальное руководство: https://github.com/obsproject/obs-studio/wiki/Build-Instructions-For-Windows

Пример для версии, под которую пересобрана эта DLL (OBS 32.1.0), из Developer PowerShell:

```powershell
git clone --recursive --branch 32.1.0 https://github.com/obsproject/obs-studio.git C:\dev\obs-studio
cd C:\dev\obs-studio
cmake --preset windows-x64
cmake --build --preset windows-x64
cmake --install build_x64 --config RelWithDebInfo --component Development --prefix C:/dev/obs-sdk
```

Если установлен другой OBS, выбрать соответствующий tag; зависимости и требования конкретного tag брать из его инструкции. Preset OBS скачивает свои зависимости. Для `QtPrefix` ниже использовать каталог Qt, реально использованный этой сборкой; найти его по `Qt6_DIR` в `build_x64/CMakeCache.txt`. `QtPrefix` — корень префикса, содержащего `lib/cmake/Qt6/Qt6Config.cmake`, а не путь к DLL. Тот же Qt можно взять из набора obs-deps для нужной версии OBS: версия obs-deps указана в `CMakePresets.json` целевого tag (пресет `dependencies`, поле `qt6.version`), а версия Qt — в `deps.qt/qt6.ps1` этого тега (для OBS 32.1.0: obs-deps `2025-08-23`, Qt **6.8.3**).

Если экспорт SDK требует дополнительных obs-deps package files, добавить их префиксы в `CMAKE_PREFIX_PATH`. При ошибке CMake сначала проверить существование `libobsConfig.cmake`, `obs-frontend-apiConfig.cmake` и `Qt6Config.cmake`. Не скачивать случайные headers отдельно для окончательной DLL.

### Сборка плагина

Из каталога проекта:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  "-DCMAKE_PREFIX_PATH=C:/dev/obs-sdk;C:/dev/qt-prefix" -DBUILD_TESTING=ON
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure
cmake --install build --config RelWithDebInfo --prefix stage
```

Заменить оба пути реальными. Либо:

```powershell
./tools/build-windows.ps1 -ObsSdk C:/dev/obs-sdk -QtPrefix C:/dev/qt-prefix
```

Можно открыть `build/obs-parameter-animator.sln` в Visual Studio. Выбрать x64 / RelWithDebInfo. Результат — `build/RelWithDebInfo/obs-parameter-animator.dll`. В `stage` лежит структура установки. Qt DLL должны соответствовать окружению OBS; не копировать произвольный другой комплект Qt поверх OBS.

## Установка Windows

При закрытом OBS скопировать содержимое `stage` в каталог OBS, например `C:\Program Files\obs-studio`:

| Из stage | В OBS |
|---|---|
| `obs-plugins/64bit/obs-parameter-animator.dll` | `obs-plugins/64bit/obs-parameter-animator.dll` |
| `data/obs-plugins/obs-parameter-animator/` | `data/obs-plugins/obs-parameter-animator/` |

Не устанавливать DLL 0.1.0 и 0.2.0 одновременно под разными именами: они используют одинаковое Vendor/Dock имя. Новая DLL заменяет старую. Сохранить резервную копию прежней сборки перед заменой.

Если фильтр не появился, открыть лог OBS: искать `[OPA]`, сообщение загрузки модуля, `Shader compilation failed`, отсутствующие DLL или несовместимый SDK. Шейдер должен находиться именно в `data/obs-plugins/obs-parameter-animator/6-zone.effect`. Отсутствие WebSocket не мешает локальному управлению.

Для удаления закрыть OBS и удалить только DLL этого плагина и его каталог данных.

## Linux и тесты без OBS

Linux модуль использует те же OBS/Qt API. Требуются development packages OBS 32.1+ и Qt 6.8 либо SDK, собранный из соответствующего OBS tag. Пример:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=/path/to/obs-sdk
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/stage
```

CMake устанавливает `.so` в `lib/obs-plugins` или `lib64/obs-plugins` согласно GNUInstallDirs; данные — `share/obs/obs-plugins/obs-parameter-animator`. Использовать пути поиска модулей именно вашей установки OBS. Сборка macOS bundle в этом пакете не реализована.

Чистые тесты движка не требуют OBS/Qt:

```bash
cmake -S . -B build-tests -DBUILD_PLUGIN=OFF -DBUILD_TESTING=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

Либо `g++ -O2 -std=c++17 -Isrc tests/animation_tests.cpp -o animation-tests` и запуск полученного executable. `python3 tests/verify_shader.py` проверяет сохранение исходного шейдера.

## Управление и сохранение

Свойства native filter содержат для каждой из шести зон Enabled, Center X/Y, Radius, Magnitude — это одна позиция зоны (состояние покоя). Для каждого параметра доступны Target, Move duration, Move easing, Auto return, Return endpoint, Return delay after the last Add, Return duration/easing, Start/Stop/Reset. Duration `-1` и Easing `Use default` используют общие defaults.

`Start` использует настроенную цель (по умолчанию она равна позиции зоны, поэтому Start возвращает зону в покой). `Set` плавно движется к введённой цели. `Add` накапливает signed дельту: пока идёт движение к цели или выдерживается задержка, дельты складываются, а во время возврата дельта добавляется к текущему значению. После последнего `Add` значение стоит `Return delay after the last Add`, затем за `Return duration` возвращается к `Return endpoint` (по умолчанию — позиция зоны, для magnitude 0). `Stop` останавливает в текущей точке. `Reset` плавно возвращает выбранный параметр к `Return endpoint`. `ResetAll` плавно возвращает все параметры к сохранённым значениям покоя. `StartAll` запускает все настроенные цели с общей временной меткой.

Dock-панель состоит из трёх вкладок. **Visual** — визуальный редактор: канвас принимает реальные пропорции источника (панель подстраивается под источник, а не наоборот; 16:9 по умолчанию, пока кадр ещё не получен; при нехватке места появляется прокрутка), зоны нарисованы кругами с номерами, активная зона выделена белым; под кругами показывается реальный кадр источника выбранного фильтра (его собственные фильтры — маски, блур, зум и т.п. — на время захвата временно отключаются, поэтому виден чистый исходный кадр; обновляется несколько раз в секунду, пока панель открыта на этой вкладке), поэтому зоны расставляются прямо по картинке; перетаскивание центра меняет `center_x`/`center_y`, перетаскивание за край круга — `radius`, колесо — `magnitude` (шаг 0.05), Shift+колесо — `radius` (шаг 1), двойной клик — вкл/выкл, а белый пунктирный круг показывает живое положение во время анимации. **У зоны одна позиция:** задавать больше нечего, положение зоны — это состояние покоя и одновременно значение, к которому приходит автоматический возврат (при перемещении зоны `zoneN_*_target` и `zoneN_*_return_value` следуют за ней). Под канвасом блок `Duration and return` (Move duration/easing, auto return, Return delay after the last Add, Return duration/easing, кнопки `Apply to this zone` / `Apply to all zones` — пишутся `zoneN_*_duration_ms`, `zoneN_*_easing`, `zoneN_*_auto_return`, `zoneN_*_hold_ms`, `zoneN_*_return_ms`, `zoneN_*_return_easing`) и блок `Test`, в котором всё проверяется на месте: Parameter (Enable/Center X/Center Y/Radius/Magnitude), Value / delta и кнопки `Add (accumulate)` / `Set` / `Start saved` / `Stop` / `Back to rest`. Ниже `Play all zones` / `Play this zone` / `Stop all` / `All zones back to rest`. Правки сразу пишутся в те же настройки, что и свойства фильтра. **Table (advanced)** показывает current, значение uniform, target, progress, phase, easing, active count. **Log** содержит CPU math update time, счётчики изменений и setter calls, фильтрацию command log, Pause, Clear, Copy. UI опрашивает состояние 10 раз/сек и пропускает обновление, когда панель скрыта; анимация от UI не зависит.

OBS сохраняет Base values, режим и конфигурации анимаций в настройках фильтра. Мгновенные runtime-значения, траектории, лог и счётчики не сохраняются и не записываются в scene collection каждый кадр. После перезапуска стартуют Base values. Прямое изменение Base value вручную прекращает анимацию этого параметра и сразу задаёт новое значение; для плавного изменения использовать Set/Start.

Пока все шесть magnitude равны нулю и их анимации не запущены, эффект искажения отсутствует (шейдер — просто копия текстуры). В этом состоянии покоя фильтр вообще не расходует CPU и GPU: он пропускает кадр через `obs_source_skip_video_filter` и не выполняет ни математику анимации, ни установку uniforms, ни shader pass. Как только любой magnitude становится ненулевым или запускается его анимация (Add/Set/Start/StartAll), фильтр снова обрабатывает кадр, а после возврата magnitude к нулю снова переходит в режим покоя.

## Пример всех шести зон одновременно

1. Создать фильтр на `CAMERA`, назвать `Distortion`.
2. В свойствах поставить цели из таблицы. В dock-панели позиция зоны — это состояние покоя, а `Target` нужен только для `Start`/`StartAll`; по умолчанию он равен позиции, то есть `Start` просто возвращает зону в покой.

| Зона и параметр | Позиция → Target (свойства) | Move duration | Easing | Возврат |
|---|---|---|---|---|
| 1 magnitude | 0 → 1 | 1000 ms | EaseOutCubic | 0 за 1000 ms |
| 2 radius | 10 → 40 | 1000 ms | EaseOutCubic | выключен |
| 3 center_x | 75 → 80 | 1000 ms | EaseOutCubic | выключен |
| 4 center_y | 75 → 20 | 1000 ms | EaseOutCubic | выключен |
| 5 magnitude | 0 → −1 | 1000 ms | EaseOutCubic | 0 за 1000 ms |
| 6 radius | 10 → 50 | 1000 ms | EaseOutCubic | выключен |

**Пульс через ADD (частый случай со Streamer.bot):** оставить magnitude зоны 1 в покое 0, задать в dock-панели `Move duration` 300 ms, `Return delay after the last Add` 200 ms, `Return duration` 800 ms и нажимать `Add (accumulate)` с дельтой `0.3`. Значение накапливается (0.3 → 0.6 → 0.9 …), каждый `Add` перезапускает задержку, после последнего оно возвращается к 0 за указанное время. То же самое по WebSocket делает `Add` с `value: 0.3`.

**Обратите внимание:** в вашем оригинале zone3_center_x = 75, а не 25, zone3/zone6_center_y = 10. Они сохранены. Для примера 25 → 80 нужно отдельно поставить Base Center X зоны 3 = 25.

Нажать `Start all`. По WebSocket можно отправить `examples/six-zones-settings.json`, дождаться успешного ответа и обновления настроек (хотя бы одного video tick), затем `examples/start-six-zones.json`. Это два сообщения по уже аутентифицированному OBS WebSocket соединению, а не файлы для импортирования в OBS. Заменить CAMERA/Distortion своими именами. После SetSourceFilterSettings обновление OBS может быть отложено до video tick; для точного сценария сначала настроить свойства в GUI, затем вызвать StartAll.

Если magnitude других зон остаётся 0, изменение их center/radius видно в debug, но не меняет изображение. Чтобы визуально оценить все шесть зон, поставить небольшую ненулевую Base magnitude, например 0.2, для зон 2, 3, 4, 6.

## 3 точки на каждое лицо (новая модель)

**Важно:** модель изменилась относительно 0.2.0. Раньше было 6 независимых зон с ручными
позициями; теперь их заменили **три точки на каждом найденном лице**: **1 — глаза**, **2 —
нос**, **3 — рот**. Точки привязаны напрямую к ключевым точкам YuNet (середина глаз, кончик носа,
середина рта), поэтому лежат на самих чертах лица. Настройки точки задаются **заранее, до детекта**
(смещение, радиус, величина и параметры
анимации), а когда лица появляются, анимация и накопление `Add` применяются к выбранной точке
**сразу на всех лицах**.

При включённом **Face tracking** (флажок в свойствах фильтра или в dock-панели) фильтр несколько
раз в секунду берёт **маленький уменьшенный** кадр своего входа (по умолчанию высота 180 px,
10 FPS) и отдаёт его фоновому потоку с OpenCV YuNet (`cv::FaceDetectorYN`). Для каждого лица
рендерится одна круговая зона на каждую включённую точку: центр = `якорь + offset`, радиус и
magnitude берутся из параметров точки. Координаты — проценты 0–100 (как в шейдере); лица
сортируются слева направо. Позиции точек **интерполируются по времени** (`face_smooth_ms`, по
умолчанию 120 мс): детекция идёт ~10 FPS, но якоря плавно подводятся к последней детекции на
каждом кадре рендера, поэтому точки не дёргаются (у каждого лица ведётся трек с сопоставлением по
ближайшему центру). Точки строятся вдоль **осей лица** (из ключевых точек YuNet), поэтому следуют и
за **наклоном** головы (roll/pitch); флаг `face_scale` (вкл по умолчанию) масштабирует радиус и
смещения под **размер** лица — дальнее лицо получает пропорционально меньшие точки.

Параметры: `point1_*` (глаза), `point2_*` (нос), `point3_*` (рот) —
`enable`, `offset_x`, `offset_y`, `radius`, `magnitude` плюс обычные ключи анимации
(`_target`, `_duration_ms`, `_easing`, `_auto_return`, `_return_value`, `_hold_ms`, `_return_ms`,
`_return_easing`). Диапазон `magnitude` настраивается для каждой точки через
`pointN_magnitude_min` / `pointN_magnitude_max` (по умолчанию −1.3333..1.3333): движение, накопление
и возврат всегда ограничены этими границами. Глобальные настройки лиц: `face_tracking`, `face_fps`, `face_max`, `face_score`,
`face_height`, `face_smooth_ms` (сглаживание).
WebSocket: `requestType: "FaceTrack"` для вкл/выкл и тюнинга; в ответе `Get` есть
`faces` (с `anchors`) и статус трекинга.

Это дёшево: детекция идёт в отдельном потоке (никогда не на графическом), захват ограничен FPS и
выполняется только когда эффект реально виден (если все magnitude равны 0, фильтр по-прежнему
пропускает кадр и ничего не захватывает). Шейдер переписан на динамический массив зон
(`zone_data[]` + `zone_count`, файл `data/face-points.effect`); оригинальный шестизонный шейдер
сохранён байт-в-байт в `data/6-zone.effect`. Дополнительно есть **независимые** от distortion
опции (чекбоксы в dock и свойствах): **Blur faces** (`effect_blur`) — блюр всей рамки лица (сила
в `face_blur_px`, px), и **Debug points** (`effect_debug`) — оверлей рамки детекции лица (квадрат)
и трёх точек-якорей: красный = глаза, зелёный = нос, синий = рот. Требуется сборка с OpenCV
(`-DOPA_FACE_TRACKING=ON`, по умолчанию) и модель `data/face_detection_yunet_2023mar.onnx`.
Подробности и ограничения: [docs/FACE_TRACKING.md](docs/FACE_TRACKING.md).

## Документация

- [Распознавание лиц и привязка зон](docs/FACE_TRACKING.md)
- [Архитектура, таймер, easing, CPU/GPU, частота](docs/ARCHITECTURE.md)
- [WebSocket API и Streamer.bot](docs/API.md)
- [Проверка производительности и визуальной совместимости](docs/PERFORMANCE.md)
- [Что фактически проверено](docs/VALIDATION.md)
- [Происхождение и лицензии](NOTICE.md)
