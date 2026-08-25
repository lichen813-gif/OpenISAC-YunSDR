# Vendor SDK staging area

Place the verified target SDK here only when its license allows local copying:

```text
third_party/libyunsdr/
  include/   # public vendor headers
  lib/       # Windows x64 import libraries (ignored by Git)
  bin/       # runtime DLLs (ignored by Git)
  examples/  # only redistributable minimal examples
  VERSION.md # model, SDK version, commit, driver and firmware matrix
```

Prefer configuring an external SDK path when redistribution is restricted.
Do not rename vendor headers or libraries before the first smoke test; the exact
names and ABI are part of the integration evidence.
