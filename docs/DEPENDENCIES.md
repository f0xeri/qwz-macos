# Настройка зависимостей

**URL, revisions и пути источников сборки находятся в `config/sources.json`.** Там же заданы источник, версия и SHA256 firmware.
Скрипты не содержат собственных копий этих настроек.

Все источники записаны одинаково в секции `sources`: `kind`, `url`,
`revision`, `directory`; `scripts/fetch-sources.sh` скачивает их все.

- OpenBSD ссылается через `manifest` на `patches/openbsd/FILES`. Этот один
  список используют sparse checkout, применение патчей и компиляция.
- Для firmware `path` задаёт подкаталог релиза, `files` — имена и SHA256.
  Менять firmware, board/regulatory данные или их хеши без отдельного
  решения и аппаратной проверки нельзя.
- Исследовательские репозитории и настройки находятся в gitignored
  local/references/ и local/reference-sources.json. В общей конфигурации
  и подготовке сборки их нет.

Обычная сборка проверяет всё локально, без скачивания: состав OpenBSD,
соответствие экспортированным патчам и SHA256 firmware.
Несовпадение останавливает сборку до компиляции. Из OpenBSD компилируются только `.c`,
перечисленные в FILES. Проверить отдельно:

```sh
scripts/patches.sh check
python3 scripts/check-firmware.py
```

`scripts/fetch-sources.sh` читает конфиг и проверяет существующие checkout:
URL, commit и отсутствие локальных изменений. Он не сбрасывает изменённые
репозитории и не подменяет испорченные cached firmware автоматически.

Обновление OpenBSD выполняется через `scripts/patches.sh update <commit>`:
скрипт обновляет commit в общем конфиге и делает трёхстороннее слияние наших
патчей. После разрешения конфликтов: `scripts/patches.sh export`.
Просто изменить pin без пересоздания/обновления openbsd/ недостаточно.

## mbuf: наша адаптация OpenBSD

`compat/uipc_mbuf.c` объединяет взятые из OpenBSD алгоритмы и нашу замену
pool allocator на malloc. Это адаптация, а не оригинальный upstream-файл;
авторство и исходная версия указаны в начале файла. `compat/include/sys/mbuf.h`
также реализует нужную нам часть API.

При обновлении OpenBSD изменения `sys/kern/uipc_mbuf.c` просматриваются
вручную и нужные исправления переносятся в compat. `patches.sh update`
этот файл не обновляет. После изменений запускаются `scripts/test-compat.sh`
и полная сборка. Попытка включить сильно обрезанный mbuf в FILES отменена:
большой удаляющий патч не оправдывал усложнение обновления.

## Airport: заголовки IO80211 и WCL

`airport/abi/` и `airport/WCL.hpp` — наши файлы. Они взяты из laobamac/itlwm
(коммит и команда для сравнения с форком — в `airport/abi/UPSTREAM`) и с тех пор
правятся на месте; наши изменения помечены `QWZAIRPORT`/`QWZMAC`. Отдельного
импорта нет: исправления из форка переносятся вручную, после `diff` из UPSTREAM.

Заголовки описывают недокументированный C++ ABI Apple, поэтому проверка одна —
по ядру этого Mac. Перед добавлением сборки macOS в `QwzAirportBuilds` и после
любой правки заголовков:

```sh
scripts/build.sh
python3 scripts/kc-abi.py --check
python3 scripts/kc-abi.py --returns
python3 scripts/kpi-check.py --opencore build/QwzWifi.kext build/QwzAirport.kext
```
