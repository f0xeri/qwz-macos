# Настройка зависимостей

**URL, revisions, пути и назначение источников сборки и импорта находятся
в `config/sources.json`.** Там же заданы источник, версия и SHA256 firmware.
Скрипты не содержат собственных копий этих настроек.

Все источники записаны одинаково в секции `sources`: `kind`, `usage`, `url`,
`revision`, `directory`. `usage=build` означает входы сборки, `usage=import` —
источник для обновления vendored-кода.

- OpenBSD ссылается через `manifest` на `patches/openbsd/FILES`. Этот один
  список используют sparse checkout, применение патчей и компиляция.
- Laobamac имеет только пути импорта ABI/WCL. Clone нужен для обновления
  импорта: `scripts/fetch-sources.sh --imports`. Обычная сборка использует
  готовые airport/abi и airport/WCL.hpp, без этого checkout.
- Для firmware `path` задаёт подкаталог релиза, `files` — имена и SHA256.
  Менять firmware, board/regulatory данные или их хеши без отдельного
  решения и аппаратной проверки нельзя.
- Исследовательские репозитории и настройки находятся в gitignored
  local/references/ и local/reference-sources.json. В общей конфигурации
  и подготовке сборки их нет.
- `airport/abi/UPSTREAM` и `SHA256SUMS` — результаты последнего импорта.
  Их версия должна соответствовать конфигу; это не независимые настройки.

Обычная сборка проверяет всё локально, без скачивания: состав OpenBSD,
соответствие экспортированным патчам, firmware и vendored ABI/WCL.
Несовпадение останавливает сборку до компиляции. Из OpenBSD компилируются только `.c`,
перечисленные в FILES. Проверить отдельно:

```sh
scripts/patches.sh check
python3 scripts/check-vendor.py
```

`scripts/fetch-sources.sh` читает конфиг и проверяет существующие checkout:
URL, commit и отсутствие локальных изменений. Он не сбрасывает изменённые
репозитории и не подменяет испорченные cached firmware автоматически.

Обновление OpenBSD выполняется через `scripts/patches.sh update <commit>`:
скрипт обновляет commit в общем конфиге и делает трёхстороннее слияние наших
патчей. После разрешения конфликтов: `scripts/patches.sh export`.
Просто изменить pin без пересоздания/обновления openbsd/ недостаточно.

## mbuf: алгоритмы отдельно от выделения памяти

`sys/kern/uipc_mbuf.c` включён в тот же OpenBSD FILES. В нём оставлены
upstream-алгоритмы цепочек, списков и очередей. Наш патч удаляет части,
которым нужны OpenBSD pools, статистика, pf, sysctl и DDB, и подключает
границу с allocator. Копии этих алгоритмов в compat больше нет.

`compat/mbuf_alloc.c` содержит malloc-backed mbufs и clusters, учёт общих
cluster-ссылок и освобождение. Это наша адаптация, её нельзя заменять
upstream-файлом целиком. `compat/include/qwzmac/mbuf.h` задаёт внутреннюю
границу: две функции, состояние общих ссылок и объявления общих данных.
Сам `sys/mbuf.h` в compat также остаётся нашей реализацией нужного API.

При обновлении OpenBSD `patches.sh update` сливает изменения алгоритмов
обычным способом. Отдельно просматриваются upstream-изменения allocator:
исправления в удалённой pool-части могут потребовать ручного переноса в
malloc-реализацию. Это не отдельная зависимость и не второй pin.
После изменений обязательны `scripts/test-compat.sh`, полная сборка и
KPI-проверка; host-тесты используют настоящие allocator и upstream-алгоритмы.

## Airport: воспроизводимый импорт ABI и WCL

```sh
scripts/fetch-sources.sh --imports
scripts/airport-vendor.sh check   # воспроизвести и сравнить, без записи
scripts/airport-vendor.sh apply   # применить подготовленный импорт
```

`apply` сначала проверяет, что импортированные файлы не правили вручную.
Изменения самого `qwzairport.patch` допустимы. Затем во временной папке
собираются нужные заголовки, LICENSE и WCL одной версии; патч применяется
через `git apply` без fuzzy-подбора контекста. Только после успеха заменяются
файлы проекта и обновляются UPSTREAM/SHA256SUMS. Ошибка патча сохраняет
прежний импорт. WCL импортируется без изменений, вместе с заголовками.

Чтобы изменить заголовок: править `airport/abi/Airport/`, затем выполнить
`scripts/airport-vendor.sh export`. Скрипт создаст патч относительно текущего
pin, проверит, что он воспроизводит заголовки, и обновит метаданные.
Произвольный пересчёт checksum не заменяет этот шаг. Изменённый WCL
экспорт не принимает: локальные изменения протокола требуют отдельного
решения, а не маскировки под неизменённый upstream.

Для обновления версии сначала экспортируются локальные правки на старом
pin. Затем меняется `sources.laobamac.revision` в общем JSON, подготавливается
чистый checkout нужного commit и запускается `apply`. Существующий checkout
fetch-скрипт автоматически не переключает: при несовпадении pin он откажет.
При конфликте правится `qwzairport.patch`, после чего `apply` повторяется.
И `apply`, и `check` проверяют URL, commit и чистоту источника.

После импорта просмотреть diff заголовков, WCL, патча и метаданных, затем:

```sh
scripts/airport-vendor.sh check
python3 scripts/check-vendor.py
scripts/build.sh
python3 scripts/kc-abi.py --check
python3 scripts/kc-abi.py --returns
python3 scripts/kpi-check.py --opencore build/QwzWifi.kext build/QwzAirport.kext
```

Проверки ABI перед добавлением macOS в allowlist обязательны; checksum
их не заменяет. Обычной сборке checkout Laobamac по-прежнему не нужен.
`QwzAirport*.cpp/.hpp` — наш развивающийся код с указанным авторством в
`airport/CREDITS`, а не повторно импортируемые файлы: скрипт их не заменяет.
