# Offline production NVS fixtures

`production_nvs_fixture.py` prepares a **new, separately archived test case**. It has no flash, serial or network implementation. It does not alter the shipping binary, clock, RTC guard or an active failure case. Only the hardware owner may later admit and install the prepared partition after checking the actual chip security state, partition layout, identity and prior case closure.

Require a complete private original flash backup for each board before fixture installation. The tool takes its exact 20 KiB NVS partition, verifies a supplied SHA256 and requires a regular nonsymlink input with mode 0600. Use an output directory outside the source checkout; the tool creates it with mode 0700 and each output with mode 0600. `before-nvs.bin` is an exact mandatory partition backup, `fixture-nvs.bin` is the proposed partition, and `result.json` records hashes, named scope and closed host children. These files contain private device data: do not commit them or print their contents.

The parser verifies page/entry/data CRCs and all live logical keys. It discovers the existing blob index and every chunk dynamically, refuses ambiguity/partial records and reparses the result. No copied fixed policy offset is used. It proves all undeclared logical values and bytes are unchanged. Appending a timezone requires an existing provisioning namespace and enough unused tail space on the single active page; it refuses instead of performing garbage collection.

Examples use placeholders and do not perform installation:

```sh
python3 -B tools/production_nvs_fixture.py --input-nvs <private-input> --input-sha256 <exact-sha> --out <new-private-output> --case-id <new-case-id> --installed-version 6.4.102 --new-case-policy-absence sense
python3 -B tools/production_nvs_fixture.py --input-nvs <private-LCD-input> --input-sha256 <exact-sha> --out <new-private-LCD-output> --case-id <same-new-case-id> --installed-version 6.4.102 --new-case-policy-absence lcd
```

Policy-absence mode retires only the explicit Sense policy/coordinator/legacy debt keys or LCD durable/maintenance-arm keys named in the tool. It retains namespace IDs, boot/generation identity, provisioning/WiFi/owner/auth values and separate failure/guard history. Historical campaign records remain in the immutable backup. This is prepared new-case state, not evidence of factory-fresh claiming and not a supported in-case debt refund. Both boards' fixture and backup hashes must be joined before use; the tool does not claim a paired installation.

For a **separate later-day admission case**, use `--exhausted-yesterday` on an existing valid production DEFERRED record whose paired target is newer than the installed version. The exact production C++ codec sets a consistent exhausted previous-UTC-day record with valid shape/CRC; it verifies that normal calendar rollover grants the production allowance and a non-calendar opportunity does not. It preserves the actual target and campaign. This mode cannot be combined with policy absence and never resets credit during a live failure case.

Optional `--timezone <fixed-POSIX-offset>` sets the actual `halo_prov/tz` key. The native helper uses the unchanged production `NightlySchedule.h` to derive the next local 02:00 event from the genuine preparation clock. `--normal-due-epoch <epoch>` can assert that result; a mismatch refuses. An accelerated later-day case must put that genuine future calendar event in the current UTC day so it is later than the prepared exhausted day. The elapsed overnight interval is synthetic and must be labeled so. Actual fresh clock, accepted normal-calendar timer, on-chip rollover, bounded attempt, accounting and outcome still require device evidence.

Restore Pacific with `--timezone 'PST8PDT,M3.2.0,M11.1.0'` from a fresh partition backup after the case closes, without another policy mutation. No timezone edit is available for the LCD-owned new-case mode. A real power interruption or USB-free recovery test requires the actual physical action; this tool does not substitute NVS or RTC edits for it.

Run `python3 -B tools/test_production_nvs_fixture.py -v` for synthetic parser, codec, preservation, refusal, backup and calendar checks. No real credentials or saved device contents are used by those tests.
