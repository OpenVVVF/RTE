# Au units library (single-file vendored copy)

This is a custom single-file packaging of [Au](https://github.com/aurora-opensource/au),
generated for the RTE/InverterCodegen project.

## Included units

- `amperes` (current)
- `volts` (voltage)
- `radians` (angle)
- `seconds` (time)
- `hertz` (frequency)
- `newtons`, `meters` (composed into newton-meters for torque)
- `celsius` (temperature quantities via `celsius_qty()`)
- `unos` (dimensionless)

## Regenerating

```bash
git clone --depth 1 https://github.com/aurora-opensource/au.git /tmp/au-temp
cd /tmp/au-temp
python3 tools/bin/make-single-file \
  --noio \
  --units amperes volts radians seconds hertz newtons meters celsius unos \
  > /path/to/RTE/Lib/InverterCodegen/third_party/au/au.hh
```

The `--noio` flag excludes `<iostream>` support to keep embedded builds lean.
