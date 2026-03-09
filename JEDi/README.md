# JEDi - JED Interpreter

Convert non-standard JED files to standard JEDEC format for use with minipro.

## Problem

Some programmers export JED files that don't conform to the JEDEC standard,
causing minipro to reject them with "JED file format error!". JEDi converts
these non-standard files to proper JEDEC format.

## Supported Formats

### Wellon Programmer

Identified by `MODEL: Wellon` header.

| Issue | Standard | Wellon |
|-------|----------|--------|
| STX marker | Required (0x02) | Present |
| ETX marker | Required (0x03) | **Missing** |
| File checksum | After ETX | **Missing** |
| Token format | `*QF2194` | `QF0000*` |
| QF value | Actual fuse count | Often incorrect (0) |

### G540 Programmer

Identified by `TITL:` header.

| Issue | Standard | G540 |
|-------|----------|------|
| STX/ETX markers | Required | **Missing** |
| Token format | `*QF2194` | `QF2194*` |
| Fuse data | `*L00000 1111...` per row | `L0000` then data on separate lines |

## Building

```sh
gcc -Wall -Wextra -o jedi jedi.c
```

Or add to minipro build:

```sh
make jedi
```

## Usage

```
jedi <input.jed> [output.jed]
```

If output filename is omitted, writes to `<input>_fixed.jed`.

### Examples

```sh
# Convert with auto-generated output name
./jedi U212.jed
# Output: U212_fixed.jed

# Specify output filename
./jedi U212.jed U212_std.jed

# Then use with minipro
./minipro -p GAL16V8 -w U212_fixed.jed
```

### Sample Output

```
$ ./jedi U212.jed
Format: Wellon
Device: GAL16V8B
Pins: 20
Fuses: 2194
Checksum: 7741
Wrote: U212_fixed.jed
```

## What JEDi Fixes

1. **Adds STX/ETX markers** - Required framing bytes (0x02/0x03)
2. **Fixes token format** - Converts `QF2194*` to `*QF2194`
3. **Corrects QF value** - Determines actual fuse count from L token data
4. **Calculates checksums** - Proper fuse checksum (C) and file checksum
5. **Terminates the final record** - Emits the trailing `*` before ETX for strict JEDEC parsers
6. **Extracts metadata** - Device name from TYPE:/TITL: headers, pin count from QP

## Output Format

JEDi produces standard JEDEC files:

```
<STX>
Device: GAL16V8B

NOTE: Converted from Wellon format by JEDi

*QP20
*QF2194
*F0
*G0

*L00000 11111111111111111111111111111111
*L00032 11111111111111111111011011111111
...
*C7741
*<ETX>2BAB
```

## Adding Support for New Formats

To add support for another non-standard format:

1. Add format enum in `jed_format_t`
2. Add detection in `detect_format()`
3. Add device name extraction in `extract_device_name()`
4. Add fuse parsing function if format differs significantly
5. Update `format_name()` and usage text

## See Also

- [minipro](https://gitlab.com/DavidGriffith/minipro) - Open source TL866 programmer software
- [JEDEC Standard JESD3-C](https://www.jedec.org/) - Standard Test Data Format for PLDs
