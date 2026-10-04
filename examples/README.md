# Примеры WebSocket-запросов (актуальная модель)

Важно: в текущей сборке модель изменилась по сравнению с 0.2.0. Вместо **шести зон**
теперь **три точки на каждом найденном лице** (1 — глаза, 2 — нос, 3 — рот). Поэтому параметры называются `point1_*`, `point2_*`, `point3_*`
(старые имена `zone1_*` больше не принимаются и вернут `{"ok": false, ...}`).

Полный справочник протокола — [../docs/API.md](../docs/API.md). Имена параметров и то,
что реально принимает Vendor, задаются в `src/animation.hpp` и `src/ws_vendor.cpp`.

## Адресация фильтра

Любой из вариантов внутри `requestData`:

| Поля | Значение |
|---|---|
| `source` + `filter` | точные имена источника и фильтра |
| `sourceUuid` + `filter` | UUID источника + имя фильтра |
| `filterUuid` | UUID нативного фильтра (приоритет выше остальных) |

Самый надёжный способ узнать адрес — `List` (см. ниже). Числовые `sourceId`/`filterId`
и `sceneItemId` не используются.

## 15 параметров

`point<1..3>_<enable|offset_x|offset_y|radius|magnitude>`.

`parameterId` = `(точка − 1) * 5 + смещение` (0..14), где
`enable=0, offset_x=1, offset_y=2, radius=3, magnitude=4`.
Если переданы и `parameter`, и `parameterId` — приоритет у `parameter`.

Значения `magnitude` в диапазоне −1.3333…1.3333 (в шейдере это не проценты).

## Envelope

Общий конверт (обычное сообщение протокола OBS WebSocket, `op: 6`):

```json
{
  "op": 6,
  "d": {
    "requestType": "CallVendorRequest",
    "requestId": "any-id",
    "requestData": {
      "vendorName": "obs-parameter-animator",
      "requestType": "Add",
      "requestData": { "source": "CAMERA", "filter": "Distortion", "parameter": "point1_magnitude", "value": 0.3 }
    }
  }
}
```

## Команды

| requestType | Обязательные поля | Что делает |
|---|---|---|
| `List` | нет (адрес не нужен) | список `filters` с именами и UUID |
| `Get` | нет (можно `parameter`/`parameterId`) | snapshot фильтра или одного параметра |
| `Add` | `parameter` + `value` | накопление signed delta |
| `Set` | `parameter` + `value` | плавный переход к абсолютной цели |
| `Start` | `parameter` | запуск сохранённой цели (`*_target`) |
| `Stop` | `parameter` | заморозить в текущем значении |
| `Reset` | `parameter` | плавно к сохранённому `*_return_value` |
| `StartAll` | нет | все сохранённые цели с одной меткой времени |
| `StopAll` | нет | заморозить всё |
| `ResetAll` | нет | плавно ко всем Base values |
| `FaceTrack` | нет | вкл/выкл и тюнинг трекинга лиц |

Общие необязательные поля для Add/Set/Start/Stop/Reset: `durationMs` (0..3600000),
`easing` (имя из списка в API.md) и `returnToZero` (bool; историческое имя, включает/
выключает авто-возврат для этого запуска; endpoint берётся из `pointN_*_return_value`).

## Готовые запросы

Пульс (накопление) по величине точки 1 — файл `add-magnitude.json`:

```json
{ "vendorName": "obs-parameter-animator", "requestType": "Add",
  "requestData": { "source": "CAMERA", "filter": "Distortion",
    "parameter": "point1_magnitude", "value": 0.3,
    "durationMs": 300, "easing": "EaseOutCubic", "returnToZero": true } }
```

Абсолютный переход (Set):

```json
{ "vendorName": "obs-parameter-animator", "requestType": "Set",
  "requestData": { "source": "CAMERA", "filter": "Distortion",
    "parameter": "point1_radius", "value": 20, "durationMs": 500 } }
```

Список фильтров (узнать имена/UUID):

```json
{ "vendorName": "obs-parameter-animator", "requestType": "List", "requestData": {} }
```

Старт/сброс одной точки:

```json
{ "vendorName": "obs-parameter-animator", "requestType": "Start",
  "requestData": { "source": "CAMERA", "filter": "Distortion", "parameter": "point1_magnitude" } }
```

Запуск всех сохранённых целей — файл `start-six-zones.json` (имя файла историческое):

```json
{ "vendorName": "obs-parameter-animator", "requestType": "StartAll",
  "requestData": { "source": "CAMERA", "filter": "Distortion" } }
```

Настройка свойств фильтра (обычный `SetSourceFilterSettings`) — файл
`six-zones-settings.json` (имя файла историческое):

```json
{ "op": 6, "d": { "requestType": "SetSourceFilterSettings", "requestId": "configure-points",
  "requestData": { "sourceName": "CAMERA", "filterName": "Distortion", "overlay": true,
    "filterSettings": { "mode": 2, "face_tracking": true,
      "point1_magnitude_target": 0.3, "point1_magnitude_auto_return": true } } } }
```

Трекинг лиц (`FaceTrack`), все поля необязательны:

```json
{ "vendorName": "obs-parameter-animator", "requestType": "FaceTrack",
  "requestData": { "enabled": true, "fps": 12, "maxFaces": 4, "score": 0.75 } }
```

## Разные точки на всех лицах

Анимация хранится **на точку**, а не на лицо: `magnitude`/`radius`/`offset` точки —
общее значение для **всех** найденных лиц. Отдельного адреса «лицо №2» нет: лица
сортируются слева направо и не имеют идентичности. Отсюда:

* один `Add` по `pointN_magnitude` применяется сразу ко **всем** лицам на этой точке;
* разные значения/дельты для разных точек = **отдельный запрос на каждую точку**
  (например `point1_magnitude`, `point2_magnitude`, `point3_magnitude`).

Пример: подсветить три точки на всех лицах с разной силой — три сообщения:

```json
{ "vendorName": "obs-parameter-animator", "requestType": "Add",
  "requestData": { "source": "Dji_racurs2", "filter": "moskiR2",
    "parameter": "point1_magnitude", "value": 0.1,
    "durationMs": 1000, "easing": "EaseOutCubic", "returnToZero": true } }
```

```json
{ "vendorName": "obs-parameter-animator", "requestType": "Add",
  "requestData": { "source": "Dji_racurs2", "filter": "moskiR2",
    "parameter": "point2_magnitude", "value": 0.2,
    "durationMs": 1000, "easing": "EaseOutCubic", "returnToZero": true } }
```

```json
{ "vendorName": "obs-parameter-animator", "requestType": "Add",
  "requestData": { "source": "Dji_racurs2", "filter": "moskiR2",
    "parameter": "point3_magnitude", "value": 0.3,
    "durationMs": 1000, "easing": "EaseOutCubic", "returnToZero": true } }
```

Одна команда сразу на все точки есть только через сохранённые цели: задать в свойствах
`pointN_magnitude_target` + auto return, затем один `StartAll` (это абсолютный переход,
без накопления). Для накопления при повторных нажатиях нужен `Add` по каждой точке.

Обязательные условия, чтобы точка реально рисовалась: включён трекинг лиц
(`face_tracking` / запрос `FaceTrack`), `pointN_enable = true`, `pointN_radius > 0`
и `pointN_magnitude != 0`.

## Вкладка JSON в док-панели

В док-панели есть вкладка **JSON**: она генерирует готовое к копированию сообщение
`CallVendorRequest` **на каждую точку** (Forehead / Nose / Mouth). В сообщении подставляются
реальные `source` и `filter` выбранного фильтра и **собственные настройки точки** —
`_duration_ms`, `_easing` и `_auto_return` — поэтому длина, плавность и возврат совпадают с
тем, что реально выполнит вендор.

Сверху выбирают `Action` (Add / Set / Start / Reset / Stop), `Parameter`
(Enable / Offset X / Offset Y / Radius / Magnitude) и `Value / delta` (для Add / Set), а также
формат. По умолчанию — `Vendor request (d)`: внутренний объект `d` **без** `op` и `requestId`,
ровно в том виде, который Streamer.bot принимает как тело custom OBS request. Второй вариант,
`Full message (op 6)`, — целое сообщение протокола OBS WebSocket. У каждой точки своя кнопка `Copy`,
а `Copy all points` кладёт в буфер сразу три сообщения. Любое изменение контролов
перегенерирует текст; сборкой строки занимается `src/json_builder.hpp`.

## Вкладка Import / Export в док-панели

Рядом с **JSON** есть вкладка **Import / Export**: она отдаёт **все настройки автоматизации**
(режим, опции лиц/оверлея и три точки с их таймингами) одним JSON-текстом, который можно
скопировать и вставить куда угодно. Текст — плоский объект с теми же ключами, что и настройки
фильтра (`mode`, `face_tracking`, `face_smooth_ms`, `effect_blur`, `face_blur_px`, `effect_debug`,
`face_scale`, `default_duration_ms`, `default_easing` и на каждую точку `pointN_enable`,
`pointN_offset_x`, `pointN_offset_y`, `pointN_radius`, `pointN_magnitude`,
`pointN_magnitude_min` / `_max`, `pointN_magnitude_duration_ms`, `pointN_magnitude_easing`,
`pointN_magnitude_auto_return`, `pointN_magnitude_hold_ms`, `pointN_magnitude_return_ms`,
`pointN_magnitude_return_easing`). Поэтому его можно вставить и в `filterSettings` сообщения
`SetSourceFilterSettings` (как в `six-zones-settings.json`).

- `Export from filter` — заполнить поле настройками выбранного фильтра (также заполняется
  автоматически при смене фильтра в списке сверху);
- `Copy` / `Paste` — обычная работа с буфером обмена;
- `Apply` — разобрать текст и записать его в выбранный фильтр (позиции покоя пишутся вместе с
  `_target` / `_return_value`, чтобы `Start` и авто-возврат пришли в импортированное состояние).

Разбор терпим к ручным правкам: числа можно писать в кавычках, значения вне диапазона
подрезаются, отсутствующие ключи сохраняют текущие значения, а вместо плоского пресета можно
вставить целое сообщение OBS (`d` → `requestData` → `filterSettings`). Ошибка разбора
показывается под полем. Сборкой и разбором текста занимается `src/preset.hpp` (тест
`preset-import-export`).

Отдельно в строке **Magnitude** (вкладка **Visual**) есть кнопка `Reset to 0`: она сбрасывает
величину текущей точки в 0 — её состояние покоя.

## Ответы


Успешный `CallVendorRequest` возвращает вложенный `responseData` с `ok`, `mode`,
`parameters[]`, `faces[]` и метриками. Ошибка приложения — это
`{"ok": false, "error": "..."}`, при этом `requestStatus` самого запроса OBS может
быть успешным: проверяйте оба уровня.

Примечание: перед `Add`/`Set`/`Start` фильтр должен быть в режиме
`Plugin animation` (`mode: 2`), иначе придёт ошибка
`Select Plugin animation mode first`.
