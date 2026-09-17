# Serving PLMN Operator Names

## Contract

```c
#include "services/operator_name_db.h"

const char *operator_name_db_lookup(const char *mcc, const char *mnc);
```

Use the **numeric serving network**, not an IMSI-derived home network or a
modem-supplied alphabetic operator name. Both arguments are NUL-terminated C
strings: MCC must contain exactly three ASCII digits, MNC exactly two or three.
`"244", "05"` and `"244", "005"` are different keys. Do not strip zeroes,
guess MNC width from a country, or pad a two-digit MNC to three digits.
Whitespace, signs, separators, quotes, non-ASCII bytes, short/long fields, and
NULL arguments are rejected. Parsing examines at most four bytes per argument
and stops on an early terminator; this is not an API for unterminated buffers.

A hit returns a read-only, NUL-terminated, printable ASCII string with static
lifetime, at most `OPERATOR_NAME_DB_NAME_MAX` (16) bytes excluding NUL. No
allocation, initialization, mutable state, I/O, locale, SDK, or libc calls are
needed. Callers must not modify or free the result. Unknown or malformed input
returns NULL, never an empty string, numeric placeholder, or partial match.

The database helper implements only the first step of the display policy below.
The 16-byte database-name contract does not limit the integration's separate
49-byte UTF-8 SIM fallback buffer. `"001", "01"` deliberately remains unknown.

## Standby Naming

The modem service resolves the standby name in this order:

1. A database match for the current registered MCC and MNC.
2. The current SIM's service-provider name (EF-SPN), when present and readable.
3. The five or six PLMN digits, preserving leading zeroes and MNC width.

The numeric name is visible while an unknown network's SIM read is pending.
A SIM name identifies the subscription provider, not necessarily the serving
network. This fallback applies on home and visited networks by project policy;
it is not a claim to reproduce Nokia's original SPN display-condition rules or
the USIM PNN/OPL enhanced operator-name resolver. Net Monitor still exposes the
numeric serving identity and the modem's unmodified reported names.

The existing Telit `AT#RFSTS` signal response supplies the serving MCC/MNC.
After registration, that first sample takes priority over ordinary mailbox and
supplementary work, but never over calls or an in-flight command. Standby no
longer polls `AT+COPS?` for a display label. Subsequent signal samples also
refresh the name, including a PLMN change without a registration-loss URC.

Only a database miss schedules a local SIM read:
`AT+CRSM=176,28486,0,0,17` reads EF-SPN (`0x6f46`). Hexadecimal response data
avoids the active `CSCS="GSM"` text encoding's embedded control bytes and NUL.
No SIM, network-selection, character-set, or modem-profile setting is written.
The adapter validates the SIM status and payload before decoding to UTF-8.
A valid empty field is cached; a failed read permits one retry after 60 seconds,
then retains numeric fallback. Each command has a two-second service deadline.
Foreground requests remain ahead of SIM fallback work, and URC/SMS reception
continues during the read. A timed-out read keeps ownership of its final reply
for up to five more seconds; its late result cannot complete a queued call or
SMS command or publish a name. Shutdown also waits for that non-abortable read.
If no final arrives in this drain window, the service marks the AT channel
failed, cancels queued requests, and retains module power without resetting it.

Names are published only from a successful, complete serving-cell response in
the current registration generation. Loss of service clears the old name
immediately. SIM removal, PIN lock, modem restart, and transport/power failure
invalidate the SIM-name cache, so recovery cannot reuse a previous SIM's name.
The service also publishes the selected name source: database, SIM, numeric
PLMN, or none. The USB debug status reports this source and the exact MCC/MNC.

Protocol references: Telit LE910Cx ThreadX AT Commands Reference Guide
80586ST11193A Rev.4, `AT+CRSM` pp.780-783 and `AT#SPN` p.550;
3GPP TS 31.102 EF-SPN and TS 51.011 Annex B for SIM alpha identifiers.
The integrated path is host-tested; the new SIM read still needs DUT validation.

## Initial Coverage

Research/review date: **2026-09-16**. This is a deliberately partial, manually
selected database: **120 exact PLMNs, 66 distinct display strings, 29 MCCs,
28 countries**. It prioritizes recognizable public-network operators across
Europe, the Americas, Asia, Africa, and Oceania, with extra Finnish and US
entries. It is not a comprehensive allocation registry, a complete list of
every PLMN used by a covered operator, or a guarantee of present radio service.

The main allocation baseline is ITU's **2023-11-15** list, not a purported
complete 2026 register. Current national records supersede that baseline for
the selected US, Canadian, German, and some Finnish entries. Documented operator
sources establish display brands where legal/older names differ. Not every
subsequent ITU amendment has been audited; publication and access dates are
different. Unknown is preferable to an invented successor/brand mapping.

Each MNC below retains its exact published width. Source IDs resolve in the
next section; the C table carries the same IDs at each country group.

| MCC / Country | Selected MNCs -> Display Name | Sources |
| --- | --- | --- |
| 208 France | 01, 02 -> Orange; 10, 11 -> SFR; 15, 16 -> Free Mobile; 20, 21 -> Bouygues Telecom | ITU p.19; B-SFR |
| 214 Spain | 01 -> Vodafone; 03 -> Orange; 07 -> Movistar | DT; B-TEF |
| 228 Switzerland | 01 -> Swisscom; 02 -> Sunrise; 03 -> Salt | ITU p.55 |
| 232 Austria | 01 -> A1; 03 -> Magenta; 05 -> Drei | ITU p.6; B-MAGENTA |
| 234 United Kingdom | 10 -> O2; 15 -> Vodafone; 20 -> Three; 30, 31, 32, 33, 34 -> EE | ITU pp.58-59; B-O2; B-UK |
| 240 Sweden | 01 -> Telia; 02 -> Tre; 06, 08 -> Telenor; 07 -> Tele2 | ITU p.53; B-TRE |
| 242 Norway | 01 -> Telenor; 02 -> Telia; 14 -> ice | ITU p.45 |
| 244 Finland | 03, 04, 12, 13 -> DNA; 05, 06, 21 -> Elisa; 91 -> Telia | FI for 03/04/05/06/12/13; ITU p.18 for 21; FI-TELIA for 91 |
| 255 Ukraine | 01 -> Vodafone; 02, 03 -> Kyivstar; 06 -> lifecell | ITU p.58; B-UA |
| 262 Germany | 01 -> Telekom; 02 -> Vodafone; 03, 07 -> O2; 23 -> 1&1 | DE; B-TEF; B-1AND1 |
| 302 Canada | 220 -> TELUS; 250, 610, 640 -> Bell; 680 -> SaskTel; 720 -> Rogers; 880 -> TELUS/Bell | CA |
| 310 United States | 010, 012 -> Verizon; 016, 150, 170, 410 -> AT&T; 120, 240, 260 -> T-Mobile | US |
| 311 United States | 480 -> Verizon | US |
| 338 Jamaica | 050 -> Digicel | ITU p.34 |
| 404 India | 02, 03, 06, 10 -> Airtel | ITU p.26; Punjab, Himachal Pradesh, Karnataka, Delhi only |
| 425 Israel | 01 -> Partner; 02 -> Cellcom; 03 -> Pelephone | ITU p.33 |
| 440 Japan | 00, 20 -> SoftBank; 10 -> NTT DOCOMO; 11 -> Rakuten Mobile; 50, 51 -> KDDI | ITU p.34 |
| 450 South Korea | 05 -> SK Telecom; 06 -> LG U+; 08 -> KT | ITU p.36 |
| 460 China | 00 -> China Mobile; 01 -> China Unicom | ITU p.13 |
| 505 Australia | 01 -> Telstra; 02 -> Optus; 03 -> Vodafone | ITU p.4 |
| 525 Singapore | 01 -> Singtel; 03 -> M1; 05 -> StarHub | ITU p.50 |
| 530 New Zealand | 01 -> One NZ; 05 -> Spark | ITU p.44 for 01; NZ-SPARK for 05 |
| 542 Fiji | 01 -> Vodafone; 02 -> Digicel | ITU p.18 |
| 621 Nigeria | 30 -> MTN; 50 -> Globacom | ITU p.44 |
| 639 Kenya | 01, 02 -> Safaricom; 03 -> Airtel; 07 -> Telkom | ITU p.36 |
| 652 Botswana | 01 -> Mascom; 02 -> Orange | ITU p.9 |
| 655 South Africa | 01 -> Vodacom; 02 -> Telkom; 07 -> Cell C; 10 -> MTN | ITU p.51 |
| 724 Brazil | 02, 03, 04 -> TIM; 05 -> Claro; 06, 10, 11, 23 -> Vivo | ITU p.9; B-TEF |
| 730 Chile | 01, 10 -> Entel; 02, 07 -> Movistar | ITU p.13; B-TEF |

Pages above are the printed, one-based PDF pages. The returned names are
project-selected display labels, not a claim to implement a GSMA display-name
specification. Capitalization is intentional. Legal suffixes and radio-system
suffixes are omitted; some recognizable names are shortened. `Bouygues Telecom`
uses all 16 characters without truncation. KDDI is an operator label, not a
choice between its retail brands. Shared `302/880` is deliberately `TELUS/Bell`,
not a guess based on the inserted SIM. Vodafone and Three retain distinct
labels for the selected UK PLMNs despite their corporate merger.

Deliberate gaps include most MVNOs, private/test/rail networks, international
shared MCCs, many countries, most Indian circles (including unreviewed Jio
allocations), and unverified successor mappings. In particular, do not infer
all of MCC 404/405 as one operator, translate all historic US Sprint codes by
range, or reuse old Nokia display strings. US `310/120` is T-Mobile because
the current US register explicitly assigns that exact code to T-Mobile.
`310/410` is AT&T Mobility in that register; `311/480` is Verizon Wireless.
Finnish `244/36` is omitted pending an explicit shared-network naming decision.
The old ITU CDMA label for `460/03` is not used as a contemporary China mapping.

## Source Provenance

All links were consulted on 2026-09-16. Only primary allocation publications
and operator-authored documentation inform the selected mappings. This is
manual factual curation of selected identifiers and names, not a bulk import
from Wikipedia, commercial/community MCC-MNC datasets, GSMA brand tables, or
a Nokia ROM. No third-party dataset/code/license is vendored. The source
publications and trademarks remain their owners' material; these citations
do not assert that entire source databases are freely licensed for copying.

- **ITU**: [ITU E.212 MNC list, annex to Operational Bulletin 1280](https://www.itu.int/dms_pub/itu-t/opb/sp/T-SP-E.212B-2023-PDF-E.pdf), position 15 November 2023. National-administration allocation baseline; use the country/page references above. This is allocation evidence, not evidence that every listed old network is still operating.
- **US**: [US IMSI Administrator HNI assignments](https://imsiadmin.com/assignments/hni/), live register. HNI rows retain three-digit MNCs. The selected rows explicitly identify AT&T Mobility, T-Mobile USA, and Verizon Wireless; shorten to AT&T, T-Mobile, Verizon respectively. No customer/MVNO inference.
- **CA**: [Canadian Numbering Administrator IMSI/MNC status](https://cnac.ca/data/MNC_Codes.htm), exported 15 September 2026. All selected rows are marked in service. `302/880` is explicitly a Telus/Bell shared allocation.
- **DE**: [Bundesnetzagentur assigned IMSI blocks](https://www.bundesnetzagentur.de/DE/Fachthemen/Telekommunikation/Nummerierung/IMSI/DL/imsi_zugbloecke.html), dated 3 August 2026. In particular, `262/03` is assigned to Telefonica Germany, not the old E-Plus name. The register still labels `262/23` Drillisch Netz AG; B-1AND1 establishes its successor name.
- **FI**: [Traficom approved MNCs](https://eservices.traficom.fi/LicensesServices/Forms/NureMnctunnukset.aspx?langid=en). The retrieved page directly lists 03/04/05/06/12/13. Do not mistake a paginated first page for the entire register. [Traficom numbering regulation 32 U/2023](https://www.traficom.fi/sites/default/files/media/regulation/M_32_U_2023_EN.pdf), section 23, establishes Finland's MCC 244 and explicitly permits two- and three-digit MNCs.
- **FI-TELIA**: [Telia Finland technical appendix](https://www.telia.fi/dam/jcr%3A0a5203cf-06e8-4c1a-b1b8-96651b5add3a/liite_2.pdf), 1 July 2022, explicitly lists E.212 `244/91`. The similarly labeled E.214/E.164 numbers are not PLMNs.
- **DT**: [Deutsche Telekom IoT device configuration](https://hub.iot.telekom.com/docs/apis/device-to-cloud/device-configuration/), operator-authored public-network settings. Its Spanish entries explicitly give 21401 Vodafone, 21403 Orange, and 21407 Telefonica. B-TEF supplies Movistar as the display brand.
- **NZ-SPARK**: [Spark/Skinny mobile connection support](https://www.spark.co.nz/help/mobile-help/general/general/issues-with-my-mobile-data-or-network-connection), explicitly supplies `530/05` and identifies the underlying Spark network. Display Spark, not the Skinny subscription brand.
- **B-TEF**: [Telefonica's brand architecture](https://www.telefonica.com/en/about-us/brands/), establishing Movistar in Spain/Hispanic America, O2 in Europe, and Vivo in Brazil. Use only alongside the exact allocation evidence above; it does not justify mapping unrelated PLMNs by geography.
- **B-O2**: [O2 UK company details](https://www.o2.co.uk/abouto2/company-details), identifying Telefonica UK Limited behind O2.
- **B-UK**: [Vodafone FY26 preliminary results](https://www.vodafone.com/~/media/Files/V/vodafone/corp/results-and-presentations/vodafone-fy26-preliminary-results.pdf), May 2026, describes the continuing Vodafone/Three consumer brands after the merger. This corroborates display policy, not additional PLMN allocations.
- **B-MAGENTA**: [Magenta company information](https://www.magenta.at/unternehmen), explicitly identifies Magenta Telekom as T-Mobile Austria GmbH. The display label is shortened to Magenta.
- **B-TRE**: [Tre privacy notice](https://www.tre.se/om-tre/sakerhet/dataskyddsinformation), 3 February 2026, explicitly identifies Hi3G Access AB as Tre.
- **B-SFR**: [SFR legal identity](https://www.sfr.fr/mentions-legales.html), associates SFR with Societe Francaise du Radiotelephone.
- **B-UA**: [Vodafone Ukraine operator policy](https://content.vodafone.ua/vf-strapi-prod/ENG_Politika_chesnogo_koristuvannya_Apps_Flexx_09_09_V2_1301e12b21.pdf), published 9 September 2025, identifies VF Ukraine and its Vodafone network/brand.
- **B-1AND1**: [1&1 corporate announcement](https://unternehmen.1und1.de/corporate-news/2021/11-ag-schliesst-vertraege-mit-vantage-towers-und-11-versatel-fuer-den-rollout-ihres-mobilfunknetzes-und-veroeffentlicht-prognose-fuer-das-geschaeftsjahr-2022/), 2021, explicitly records Drillisch Netz AG's renaming to 1&1 Mobilfunk GmbH on 5 November 2021.

## Representation And Flash

`src/services/operator_name_db.c` is the maintained source of truth. One local
X-macro list declares the names; another declares explicit
`(MCC, decimal MNC, MNC digit count, name ID)` rows. MNC integer literals must
not have leading zeroes (C would interpret them as octal); the separate digit
count carries the published width. The coverage table above uses padded text
for readability. There is no runtime compression or generated build step.

Each record is one `uint32_t`:

```text
key    = 2 * (1000 * MCC + numeric_MNC) + (MNC_digits - 2)
record = (key << 11) | string_offset
```

The key requires 21 bits even for `999/999`; the low 11 bits allow a string
pool up to 2,048 bytes. Rows sort strictly by MCC, numeric MNC, then digit
count. Binary search takes at most seven iterations for this 120-row table.
The pool is an all-char struct: each member is sized from its literal, so
`offsetof` supplies string offsets without pointer tables or manually maintained
offsets. Duplicate display labels share a single member. Compile-time checks
reject overlong/empty names, overflowing pool size, and out-of-range row fields.
Standalone tests check ASCII, ordering, duplicates, offsets, and every lookup.

Exact data size for this snapshot:

| Component | Bytes |
| --- | ---: |
| 120 packed rows x 4 | 480 |
| 66 strings, including all NUL terminators | 481 |
| Total immutable data | **961** |
| Static writable data / BSS | **0 / 0** |

Measured with **Arm GNU Toolchain 15.3.Rel1, GCC 15.3.1 (20260627)** for the
repository's Cortex-M33 target, without LTO:

| Optimization | Lookup `.text` | Data | Raw flash payload | Rounded to 4 bytes |
| --- | ---: | ---: | ---: | ---: |
| `-Os` | 148 | 961 | **1,109** | **1,112** |
| `-O3` (repository Release optimization) | 236 | 961 | **1,197** | **1,200** |

These are exact object-section measurements and a four-byte-rounded budget,
not a claimed whole-firmware binary delta. Link order/alignment, compiler
version, flags, and LTO can change the final code/padding contribution. The
normal RP2350 XIP link places these `static const` objects in flash; the object
confirms both tables are read-only sections and `.data`/`.bss` are empty.
The lookup has no undefined symbols (`nm -u` is empty), so it pulls in no
allocator, string routine, division helper, or SDK dependencies. Ordinary
automatic locals use the call stack, not persistent RAM. Debug metadata,
symbol tables, `.comment`, `.ARM.attributes`, and link-time relocation records
are not flash payload; do not use the unfiltered `size -A` total as flash cost.

Reproduce the size measurement from the repository root, substituting `-O3`
for `-Os` for the second row (tools may need their `/opt/homebrew/bin/` prefix):

```sh
arm-none-eabi-gcc -std=c11 -Wall -Wextra -Werror -Wpedantic \
  -Wconversion -Wshadow -mcpu=cortex-m33 -mthumb \
  -march=armv8-m.main+fp+dsp -mfloat-abi=softfp -mcmse \
  -Os -ffunction-sections -fdata-sections \
  -fno-unwind-tables -fno-asynchronous-unwind-tables \
  -Iinclude -c src/services/operator_name_db.c \
  -o /tmp/sisu-operator-name-db.o
arm-none-eabi-size -A /tmp/sisu-operator-name-db.o
arm-none-eabi-nm -S --size-sort /tmp/sisu-operator-name-db.o
arm-none-eabi-nm -u /tmp/sisu-operator-name-db.o
arm-none-eabi-readelf -SW /tmp/sisu-operator-name-db.o
```

## Standalone Tests

No build-system, modem, or DUT integration is required. The test includes the
implementation as a single translation unit to inspect its private data,
following existing focused service tests. **Compile the test alone**, not both
the test and implementation as separate source arguments:

```sh
cc -std=c11 -Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow \
  -O2 -Iinclude tests/test_operator_name_db.c \
  -o /tmp/sisu-test-operator-name-db
/tmp/sisu-test-operator-name-db

cc -std=c11 -Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow \
  -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iinclude tests/test_operator_name_db.c \
  -o /tmp/sisu-test-operator-name-db-sanitize
/tmp/sisu-test-operator-name-db-sanitize
```

Tests cover independently written name
fixtures, exact `310/410` and `311/480`, unknown `001/01`, width/zero preservation,
malformed fields (including all non-digit byte values), short-object bounds,
whole-string pool offsets, printable ASCII and the 16-character boundary,
strict sorting/uniqueness, unused/duplicate pool names, input immutability,
pointer lifetime, and an unpacked-oracle sweep of **all 1,100,000 syntactically
valid numeric inputs**, yielding exactly 120 hits. The oracle bypasses the
packing and binary search; it is not independent source-provenance verification.

## Updating

1. Verify each exact MCC/MNC and its digit count against the relevant national
   administrator or an ITU notification. Verify changed brands against the
   operator itself. Record source URL, publication/access dates, and coverage
   in this document. Do not infer range-wide assignments or successors from
   mergers alone. Omit uncertain entries and leave fallback to integration.
2. Reuse a name ID or add a printable ASCII literal of 1..16 bytes. Edit only
   explicit rows and keep numeric tuple order, retaining MNC digit counts.
3. Add independent fixtures for changed identities, especially any width
   collision or leading-zero case. Run the standalone and sanitizer commands;
   the count/name-count checks require deliberate snapshot updates.
4. Reconcile this coverage table with every changed row, remeasure section
   sizes for the target toolchain, and update counts/costs above. For a release,
   check newer allocation notices/reassignments and the integrated link map.

The normal `tests/run_tests.sh` suite also exercises the Telit decoder and the
integrated modem scheduler, including registration/SIM changes and incoming
calls and SMS while a SIM-name query is pending.
