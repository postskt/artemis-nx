# Packaging a homebrew NSP that actually boots on Switch

This note documents the packaging bugs found while bringing up **Artemis NX**
on real hardware (Atmosphère, firmware 23.5.0).  It is intentionally about the
container/NPDM layer, not about the game code.

## Symptom

Installing the NSP succeeded, but every launch immediately produced:

```text
Error Code: 2168-0008 (0x10A8)
Type: Undefined System Call
Svc Id: 0x28
PC: svcReturnFromException
X0: 0xf801
```

`0x28` is `svcReturnFromException`; `0xf801` is the abort value used by
libnx's `__libnx_exception_entry` when no exception handler is installed.
The process never reached `main`.

## Root causes

### 1. The NPDM had no syscall whitelist

`npdmtool` accepts a `syscalls` kernel capability.  If it is omitted, the
generated NPDM has **no allowed SVCs**.  Normal application SVCs such as
`svcGetSystemTick` (0x1e) and `svcReturnFromException` (0x28) are then rejected
by the kernel with `2168-0008`.

A working NPDM must contain a capability like:

```json
{
  "type": "syscalls",
  "value": {
    "svcSetHeapSize": "0x01",
    "svcGetSystemTick": "0x1e",
    "svcReturnFromException": "0x28",
    "...": "..."
  }
}
```

The easiest safe choice for a normal application is to grant `0x01` through
`0x7f`, matching the reference NSP.  The generated `main.npdm` should then
show this in `hactool -t npdm --info`:

```text
Allowed SVCs: svcGetSystemTick (0x1e)
              svcReturnFromException (0x28)
              ...
Application Type: Application
```

### 2. The NPDM application type was implicit

`npdmtool` does not necessarily emit an application-type descriptor by
default.  Add it explicitly:

```json
{
  "type": "application_type",
  "value": 1
}
```

`1` means **Application**.  Verify with:

```text
Application Type: Application
```

### 3. A fake ticket is not needed by DBI

The reference port on real hardware is a **no-ticket** NSP containing exactly
three files:

```text
Program NCA
Meta NCA
Control NCA
```

A fake `cert`/`tik` pair can be installed by DBI, but it is not required and
introduces a second launch path.  For homebrew on Atmosphère, reproduce the
working layout first:

```text
PFS0: 3 files, no cert/tik
```

If a ticket is required for your distribution, validate the ticketed and
no-ticket layouts separately instead of assuming they behave the same.

### 4. NACP title encoding

The title must be valid UTF-8 in `control.nacp`.  A source file accidentally
containing mojibake creates a garbled title in the home menu/DBI even when the
NSP itself is valid:

```python
# wrong: mojibake literal
TITLE_NAME = "銈偆銉欍儰"
# correct
TITLE_NAME = "アイベヤ"
```

Verify the first title field starts with the UTF-8 bytes:

```text
e3 82 a2 e3 82 a4 e3 83 99 e3 83 a4  # アイベヤ
```

## Recommended build flow

1. Build the Switch ELF (`artemis_runner.elf`).
2. Convert it to NSO with `elf2nso`.
3. Generate `main.npdm` with `npdmtool` from a JSON that contains:
   - `main_thread_stack_size` (8 MB is safe for SDL2 + TTF + Lua),
   - `application_type = 1`,
   - the full `syscalls` capability,
   - the required `service_access` entries.
4. Put `main`, `main.npdm`, control data and RomFS into the staging layout.
5. Pack with `hacBrewPack` to get the program/control/meta NCAs.
6. Assemble a PFS0 NSP with those three NCAs only (no ticket) for DBI.
7. Validate:

```bash
hactool -t pfs0 --info game.nsp
hactool -t npdm --info exefs/main.npdm
```

Expected NSP contents:

```text
Control NCA
Meta NCA
Program NCA
```

Expected NPDM:

```text
Application Type: Application
Allowed SVCs:     svcReturnFromException (0x28), ...
```

## Useful references

- [The-4n/hacBrewPack](https://github.com/The-4n/hacBrewPack) - NCA/NSP writer
- [SciresM/hactool](https://github.com/SciresM/hactool) - inspection/validation
- [switchbrew/libnx](https://github.com/switchbrew/libnx) - `crt0` exception
  entry behavior and the `0xf801` abort value
- [switchbrew NPDM](https://switchbrew.org/wiki/NPDM) - kernel capability
  format

## Notes

`prod.keys` and any game assets are user-supplied and must never be committed.
The commands above are for local packaging only.
