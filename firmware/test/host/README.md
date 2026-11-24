# Host tests: the load classifier

Unit tests and a fuzz target for the load and launcher detection
(`firmware/main/load_classify.c`), built and run on a PC. The classifier file
is compiled as the firmware has it; what it calls outside itself is faked
(`fake.c`): the saved calibration, `cfg autowhite`, the clock and the event
log. Virtual launchers (`sim.c`) feed it readings the way the sensor task does
(`main.c`): polled every 50 ms while empty, a flash every 10 ms while loaded,
launches that end with `load_launch_ended()`, a fresh load arming the next
launch as `launch.c` needs. The thresholds are the firmware's own
(`load_classify.h`).

## Build and run

Windows: MSYS2's **CLANG64** environment (clang, compiler-rt, cmake, ninja).
Linux: clang and cmake. From the repository root, in a CLANG64 shell:

```
cmake -S firmware/test/host -B firmware/test/host/build -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build firmware/test/host/build
ctest --test-dir firmware/test/host/build --output-on-failure
```

Unity is ESP-IDF's (set `IDF_PATH`, e.g. `C:/esp/v6.1/esp-idf`), or
`-DUNITY_DIR=<Unity's src folder>`, or it is downloaded. Everything is built
with AddressSanitizer and UndefinedBehaviorSanitizer (overflow, division by
zero, out of bounds), stopping at the first error, and warnings as errors.

## Unit tests (`test_load_classify.c`)

- debounce: a load needs `DEBOUNCE_COUNT` readings in a row; single spikes
  and ADC errors never load;
- launchers as calibrated (black, white detected after 3 s, back to black,
  `cfg autowhite off`): every launch re-armed, nothing re-learned;
- a launcher that reads higher than calibrated (empty at 55 % of the span):
  still loaded after the launch, then re-learned and re-armed; the event's
  levels; back to the saved calibration on a launcher as calibrated; dropped
  by `cal load`; nothing learned from readings that never settle;
- known limits, pinned so a change shows: a launcher whose reading barely
  drops when the top leaves stays stuck; so does one whose next top is fitted
  before the empty reading has settled (0.3 s after the launch);
- the calibrations of the boards made so far (`fixtures/launcher_cals.h`):
  each board's black and white launcher as calibrated, and a black launcher
  55 % high on each. Re-learning needs a drop of 25 mV at least (`LOAD_MIN_CONTRAST_MV`): the boards
  whose narrow calibration doesn't leave that are listed, not failed.

`fixtures/launcher_cals.h` is generated from the production database with
`python tools/export_fixtures.py [path/to/production.db]`. It reads only the
four load levels of each board, nothing that identifies a board or a
customer, so the header can be committed (the database must not be).

## Fuzzing (`fuzz_load_classify.c`)

A libFuzzer target: each input is a calibration, two virtual launchers and up
to 64 operations (readings with and without a top, launches, stray readings,
ADC errors, moving BX Tacho to the other launcher, `cal load`,
`cfg autowhite`). Launches, launcher moves and `cal load` happen as a person
can do them: a top fitted before each launch, fetched again afterwards, BX
Tacho off any launcher while moving it, and `cal load` from diag mode. Besides
the sanitizers' errors it checks that a re-learned scale is never narrower
than `LOAD_MIN_CONTRAST_MV`, and that a launcher the design can handle (on its
saved calibration, or by re-learning it, allowing for baseline tracking)
never stays stuck after a launch.

MSYS2's clang has no libFuzzer on Windows, so `fuzz_main.c` runs the target on
seeded random inputs (ctest: 20000, seed 1):

```
firmware/test/host/build/fuzz_load_classify -runs 1000000 -seed 7
```

A failing input is written to `crash-<seed>-<run>.bin`. Run it again, step by
step, with the trace on:

```
FUZZ_TRACE=1 firmware/test/host/build/fuzz_load_classify crash-7-1234.bin
```

On Linux, `-DFUZZ_LIBFUZZER=ON` builds the same target for libFuzzer
(coverage-guided).
