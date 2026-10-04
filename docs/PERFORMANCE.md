# Проверка на вашем OBS

## Условия сравнения

Проверять одну и ту же OBS/driver версию, разрешение, FPS, SDR color settings, источник, сцену и output. Фоновую нагрузку не менять. В каждом случае дать сцене прогреться 30 секунд; записывать метрики 60 секунд, повторить трижды. Сравнивать медиану и худшие кадры, а не один случайный Task Manager пик.

| Case | Настройка |
|---|---|
| 0 | Исходный источник без distortion filters |
| A | Шесть отдельных исходных distortion filters, одинаковые zones/силы |
| B-static | Один native filter, Static mode |
| B-plugin | Тот же native filter, Plugin mode, 6 анимаций |
| B-stress | 30 active trajectories и видимый debug panel |
| B-hidden-debug | Те же trajectories, debug panel скрыт |

При сравнении стоимости controllers лучше сохранять примерно одинаковую активную площадь и ненулевые magnitude. Static с magnitude=0 и animated с включёнными шестью зонами измеряют разные GPU workload.

Сравнение A/B не доказывает визуальную эквивалентность: повторное sampling шести filters отличается от одного composed UV sampling. Проверка сохранения визуальной логики должна сравнивать B с оригинальным единым шестизонным shader в obs-shaderfilter.

## Что измерять

- OBS Stats: Average time to render frame, missed frames due to rendering lag, CPU, FPS. Encoding lag отдельно от render lag.
- Task Manager/Process Explorer: CPU процесса OBS, GPU 3D usage, dedicated GPU memory. VRAM меняется также из-за OBS/driver pooling, поэтому нельзя приписывать всю дельту одному effect.
- Debug panel: activeCount, CPU math last/max microseconds, evaluations, parameterUpdates, uniformCalls.
- При углублённом анализе: OBS profiler для CPU и GPU debugger/profiler для pass counts и draw workload. Это измерения вашей сборки; в данном архиве нет результатов реального Windows/GPU профилирования.

CPU math timing включает проход по 30 состояниям, easing/interpolation и сравнение gpuValue. Не включает mutex wait, Qt UI, WebSocket, OBS overhead, gs_effect setters, driver work и GPU render. Очень маленькие времена сравнимы с overhead измерения. Показатель max — максимум с момента создания фильтра, не percentile. Чтобы сбросить его, пересоздать фильтр; Clear очищает только log.

uniformCalls при одном render callback на кадр обычно растёт на 33 за кадр: ~990/сек при 30 FPS, 1980/сек при 60, 3960/сек при 120. Это количество вызовов маленьких effect setters, а не 33 GPU passes и не точное количество hardware constant uploads. Количество render callbacks определяет OBS и может отличаться при повторном использовании источника.

## Функциональные проверки

1. Linear 0 → 1 за 1000 ms на 30/60/120 OBS FPS. Измерять старт и первый кадр в endpoint. Ожидается около 1 секунды плюс render quantization/scheduling. Не требовать лишних виртуальных кадров при dropped frames.
2. На середине движения 0 → 100 Center X вызвать Set 0, затем Set 80, затем Reset. Значение и изображение должны продолжать движение от current; target может меняться скачком, current — не должен.
3. Magnitude Add +0.3 несколько раз во время Attack: pending target растёт до максимум 1.3333. Во время Return новая delta прибавляется к current, начинается новый attack.
4. Magnitude Set −1 с return value 0. Проверить плавное отрицательное движение и возврат; допустимый предел сохраняется.
5. Запустить StartAll по example. Убедиться в независимых current/target/progress/state; менять один parameter, остальные продолжают свои траектории.
6. Stop должен зафиксировать current. Reset выбранного поля возвращает к 0, ResetAll — к Base values.
7. Enable 1 → 0 при Linear: GPU bool переключается около 50% времени. Это ожидаемая ступень исходного bool, не «плавный fade».
8. Закрыть debug dock и убедиться, что анимация продолжается. Удалить источник/фильтр при открытом dock, нажать Refresh: не должно быть use-after-free. Переключить scene collection, переименовать источник, проверить addressing UUID.
9. Неверный parameter, неправильный type value, отрицательный duration, неверный easing: vendor ok=false, фильтр остаётся работоспособным.
10. Скрыть источник на время дольше attack+hold+return, затем показать. При первом render ожидается уже конечное значение; траектория не должна начинаться заново.

## Визуальная проверка сохранения shader

Сначала Static/animate=false в обоих filters. Использовать test chart с сеткой, окружностями и резкими границами. Сравнить screenshot при одинаковых всех 30 values и resolution; отдельно проверить 16:9, 9:16, 1:1.

Проверить positive/negative magnitude, radius=0, magnitude=0, disabled zones, центры на границах, пересечения зон. В B должна остаться та же последовательность вызовов distortZone и aspect ratio correction. Shader sine mode сравнивать при одинаковом elapsed_time/фазе; разное время создания filters создаёт ожидаемое несовпадение.

`python3 tests/verify_shader.py` подтверждает точное сохранение original source body. Эта проверка не заменяет GPU compiler/render test: libobs wrapper, texture sampler, SDR pipeline и backend всё равно необходимо проверить в OBS.

## Приёмка

Плавность: нет подстановки старого start/target при прерывании, duration не масштабируется с FPS, dropped frames догоняют wall interval.

Производительность: при переходе B-static → B-plugin не добавляется shader pass или image sampling instruction. CPU debug math должен быть мал относительно frame budget (16.67 ms при 60 FPS, 8.33 ms при 120). Конкретный допустимый прирост CPU/GPU процента выбирать по результатам вашей сцены; заранее фиксированное обещание процентов или коэффициента ускорения здесь не обосновано.

Режим покоя: пока все шесть magnitude равны нулю и не имеют активных траекторий, шейдер является чистой копией текстуры, поэтому фильтр вызывает `obs_source_skip_video_filter` и не выполняет ни CPU-математику, ни установку 33 uniforms, ни GPU pass. Это удобный нулевой baseline: при magnitude=0 в Static, Plugin и Shader sine режимах debug panel не должен наращивать evaluations и uniformCalls, а стоимость кадра должна совпадать с источником без фильтра. Как только любой magnitude становится ненулевым (или запускается его анимация), fast path выключается и кадр рисуется как обычно.
