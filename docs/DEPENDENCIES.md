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
Несовпадение останавливает сборку до компиляции. Компилируются только `.c`,
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

Для обновления ABI меняется pin `sources.laobamac.revision`, подготавливается
чистый checkout нужной версии и проверяется наш `qwzairport.patch`. Затем
`scripts/airport-vendor.sh` повторяет импорт и обновляет UPSTREAM/SHA256SUMS.
`WCL.hpp` должен соответствовать той же версии upstream. Нельзя просто
пересчитать хеши изменённых файлов, чтобы обойти проверку.
Проверки `kc-abi.py --check` и `--returns` перед добавлением новой macOS
в allowlist остаются обязательными; checksum их не заменяет.
