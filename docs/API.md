# OBS WebSocket 5.x Vendor API

Плагин использует стандартный OBS WebSocket сервер (обычный порт 4455), его настройку пароля и аутентификацию. Второго сервера нет. Register vendor выполняется в `obs_module_post_load` через официальный upstream header. При отсутствии obs-websocket работают свойства фильтра и Qt panel.

Нельзя отправлять `{"action":"add"}` прямо как верхнее сообщение OBS WebSocket: нужен стандартный `CallVendorRequest`.

## Envelope

После завершения обычной аутентификации OBS WebSocket отправить:

```json
{
  "op": 6,
  "d": {
    "requestType": "CallVendorRequest",
    "requestId": "add-001",
    "requestData": {
      "vendorName": "obs-parameter-animator",
      "requestType": "Add",
      "requestData": {
        "source": "CAMERA",
        "filter": "Distortion",
        "parameter": "point1_magnitude",
        "value": 0.3,
        "durationMs": 300,
        "easing": "EaseOutCubic",
        "returnToZero": true
      }
    }
  }
}
```

Streamer.bot: использовать действие, которое отправляет custom OBS request, с request type `CallVendorRequest`. В requestData передать объект из внутреннего поля `d.requestData` (vendorName/requestType/requestData). Если клиент принимает уже сериализованное полное сообщение протокола, использовать envelope целиком. `op`/`requestId` не помещать внутрь vendor requestData.

## Адресация

| Поле | Значение |
|---|---|
| source + filter | Точные имена OBS источника и фильтра |
| sourceUuid + filter | UUID источника и имя фильтра |
| filterUuid | UUID native filter, имеет приоритет над source/filter |
| parameter | Точное имя параметра, например point2_offset_y |
| parameterId | Числовой ID 0..14 внутри фильтра |

Числовые sourceId/filterId не используются: OBS API для источников предоставляет UUID. Список UUID получить Vendor request `List` с пустым requestData. UUID надёжнее имён после переименования источника. Не придумывать UUID и не подставлять scene item id.

Параметров 15: три точки (1 лоб, 2 переносица, 3 ниже подбородка) × пять параметров. Каждая точка рисуется на **каждом** найденном лице, поэтому анимация и накопление применяются сразу ко всем лицам.

Parameter ID = `(point - 1) * 5 + offset`:

| Offset | Параметр | Тип и bounds |
|---|---|---|
| 0 | enable | Внутреннее 0..1; GPU bool при >=0.5 |
| 1 | offset_x | −100..100 (сдвиг якоря по X, %) |
| 2 | offset_y | −100..100 (сдвиг якоря по Y, %) |
| 3 | radius | 0..100 (% высоты кадра) |
| 4 | magnitude | −1.3333..1.3333 |

Например point1_magnitude = 4, point2_radius = 8, point3_magnitude = 14. Если переданы parameter и parameterId одновременно, parameter имеет приоритет.

## Requests

| requestType | Поведение / обязательные поля |
|---|---|
| List | Вернуть filters с source/filter names и UUID; адрес не нужен |
| Get | Вернуть snapshot всего фильтра, либо одного parameter/parameterId |
| Add | parameter и numeric value; плавное накопление signed delta: пока идёт движение к цели или выдержка, дельты складываются |
| Set | parameter и numeric value; плавный переход к абсолютной цели |
| Start | parameter; запустить сохранённый target (по умолчанию он равен позиции покоя, поэтому Start возвращает параметр в покой) |
| Stop | parameter; вычислить current на момент команды и заморозить |
| Reset | parameter; плавный переход к сохранённому `Return endpoint` (auto return выключен для этого запуска) |
| StartAll | Все сохранённые цели, единый timestamp; параметры длительности в request не используются |
| StopAll | Заморозить все в current |
| ResetAll | Плавно вернуть все к сохранённым значениям покоя (position) |
| FaceTrack | Включить/выключить и настроить распознавание лиц: `enabled` (bool), `fps`, `maxFaces`, `score`, `smoothMs` (сглаживание точек), `blur` (bool, блюр лиц), `blurPx` (радиус блюра, px), `debug` (bool, рисовать точки), `faceScale` (bool, масштаб точек под размер лица). Все поля необязательны; в ответе всегда возвращаются лица и статус |
| ListEffects | Каталог мемных морфов; адрес не нужен. Возвращает `effects`: `effect` (ключ), `effectId` (0..5), `name` |
| EnableEffect | `effect`/`effectId` и `enabled` (bool); вкл/выкл морф. Опционально `value` задаёт его позицию покоя |
| SetEffect | `effect`/`effectId` и numeric `value`; плавный переход к абсолютной силе (−1..1) |
| AddEffect | `effect`/`effectId` и numeric `value`; накопительная сила: дельты складываются, затем возврат (как magnitude) |

Все requests кроме List и ListEffects требуют адрес фильтра. Add/Set/Start/Reset, а также EnableEffect/SetEffect/AddEffect требуют режима Plugin. Get/List/StopAll разрешены во всех режимах. Stop конкретного параметра тоже проверяет Plugin mode, поскольку это траекторная команда.

Для parameter requests опциональны `durationMs` (0..3600000) и `easing` (одно точное имя из списка ниже). Если отсутствуют, используется настройка данного параметра; его inherited defaults разрешены при загрузке конфигурации. Stop игнорирует duration/easing.

`returnToZero` — историческое имя опции включения auto return для одного запуска. Endpoint берётся из сохранённого `Return endpoint` (`pointN_*_return_value`): по умолчанию это позиция покоя параметра, для magnitude 0. Поэтому true не принудительно перезаписывает return endpoint нулём. Duration/easing самого возврата и `Return delay after the last Add` (`pointN_*_hold_ms`) задаются в properties или в dock-панели. Отсутствие returnToZero использует сохранённый Auto return параметра; false отключает возврат для текущего запуска. Конфигурация на следующий запуск сохраняется отдельно.

`Reset` возвращает параметр к сохранённому `Return endpoint`, а не к нулю: для Center X/Radius нулевая цель сломала бы композицию. `ResetAll` возвращает все параметры к их позициям покоя.

## Накопительный Add и возврат

Типовой сценарий (нажатие клавиши → всплеск дисторшена):

```json
{ "parameter": "point1_magnitude", "value": 0.3, "durationMs": 300, "returnToZero": true }
```

* Каждый `Add` складывается с ещё не достигнутой целью: 0.3 → 0.6 → 0.9 …, пока параметр движется к цели или выдерживает задержку. Во время возврата дельта прибавляется к текущему значению.
* Задержка перед возвратом (`pointN_*_hold_ms`, по умолчанию 200 ms) отсчитывается заново от каждого нового `Add`, поэтому серия быстрых нажатий держит эффект и отпускает его только после последнего.
* Затем за `pointN_*_return_ms` значение плавно возвращается к `pointN_*_return_value`.
* Значения ограничены bounds параметра, поэтому серия очень больших дельт не выходит за допустимый диапазон.

## Мемные морфы (morphs)

Морфы — геометрические искажения UV всего лица, применяемые поверх точечного distortion внутри
одного прохода шейдера. Сила каждого морфа анимируется и накапливается так же, как `magnitude`
точки, поэтому несколько морфов можно включать параллельно.

| effectId | effect (ключ) | Что делает |
|---|---|---|
| 0 | big_head | увеличивает / уменьшает лицо (value>0 крупнее, value<0 мельче) |
| 1 | squash | широкое / вытянутое лицо (value>0 шире, value<0 выше) |
| 2 | swirl | закрутка вокруг центра лица |
| 3 | melt | «плавит» лицо вниз |
| 4 | mirror | зеркалит лицо по горизонтали (по порогу abs(value) ≥ 0.5) |
| 5 | tilt | наклоняет лицо |

Знак `value` задаёт направление, `0` — выключено. `EnableEffect` включает морф; у выключенного
морфа сила трактуется как 0. Пример накопительного всплеска:

```json
{ "effect": "big_head", "value": 0.3, "durationMs": 300, "returnToZero": true }
```

Морфы применяются ко **всем** обнаруженным лицам и сосуществуют с точками, блюром и друг с другом.

## Easing names

```text
Linear
EaseInQuad     EaseOutQuad     EaseInOutQuad
EaseInCubic    EaseOutCubic    EaseInOutCubic
EaseInQuart    EaseOutQuart    EaseInOutQuart
EaseInQuint    EaseOutQuint    EaseInOutQuint
EaseInSine     EaseOutSine     EaseInOutSine
EaseInExpo     EaseOutExpo     EaseInOutExpo
EaseInCirc     EaseOutCirc     EaseInOutCirc
EaseInBack     EaseOutBack     EaseInOutBack
EaseInElastic  EaseOutElastic  EaseInOutElastic
EaseInBounce   EaseOutBounce   EaseInOutBounce
```

Неподдерживаемое имя, неправильный числовой тип, NaN/Infinity, отрицательная длительность и несуществующий parameter отклоняются. Endpoint и delta после суммирования ограничиваются допустимыми bounds; большое конечное value не является ошибкой.

## Get и ответы

В успешном `CallVendorRequest` стандартный OBS ответ содержит вложенное vendor `responseData`. Внутри него:

```json
{
  "ok": true,
  "mode": 2,
  "activeAnimations": 1,
  "cpuUpdateUs": 1.2,
  "cpuMaxUpdateUs": 3.5,
  "parameterUpdates": 40,
  "uniformCalls": 1980,
  "parameters": [
    {
      "parameter": "point1_magnitude",
      "parameterId": 4,
      "current": 0.734,
      "gpuValue": 0.734,
      "target": 1,
      "progress": 0.357,
      "state": "ANIMATE",
      "active": true,
      "easing": "EaseOutCubic"
    }
  ],
  "faces": [
    { "centerX": 42.5, "centerY": 55.9, "width": 18.0, "height": 27.5, "score": 0.94 }
  ],
  "faceTracking": true,
  "faceAvailable": true,
  "faceSequence": 87,
  "faceDetectMs": 32.5
}
```

Поля `faces` (центры и размеры в процентах 0–100) и статус трекинга присутствуют всегда; при
выключенном трекинге `faces` пуст. Лица отсортированы слева направо.

Ответ также содержит `effects` — состояние каждого мемного морфа: `effect`, `effectId`, `enabled`,
`value`, `gpuValue`, `target`, `progress`, `state`, `active`, `easing`:

```json
"effects": [
  { "effect": "big_head", "effectId": 0, "enabled": true, "value": 0.45,
    "gpuValue": 0.45, "target": 0.45, "state": "IDLE", "active": false }
]
```

Цифры выше иллюстративные. Progress — raw time progress, не нормализованный current; при easing current 0.734 не означает progress 73.4%. State: IDLE, ANIMATE, HOLD, RETURN. Active включает hold. При окончании state=IDLE и progress сохраняет последний stage progress. Get не вызывает CPU/GPU render и возвращает последнее вычисленное состояние.

Ошибка приложения: `{"ok":false,"error":"..."}` внутри vendor response. Стандартный OBS requestStatus может быть успешным, поскольку Vendor callback выполнился: проверять оба уровня ответа.

## Одновременный запуск

StartAll берёт target/duration/easing/return policy каждого параметра и использует ОДНУ метку времени для всех 30. Остальные 24 параметра могут иметь target=base и не менять изображение.

Для шести зон настроить properties по README и вызвать StartAll. В examples есть standard SetSourceFilterSettings envelope и следующий Vendor StartAll envelope. Обычный SetSourceFilterSettings — настройка конфигурации, не путь обновления uniforms на каждом кадре. Операция OBS update может применяться на следующем video tick; не предполагать, что network ack означает уже отрисованный новый settings snapshot.

Исходники protocol: https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md
Vendor API: https://github.com/obsproject/obs-websocket/blob/master/lib/obs-websocket-api.h
