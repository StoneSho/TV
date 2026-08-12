Import("env")

from pathlib import Path


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
BUILD_DIR = Path(env.subst("$BUILD_DIR"))
BASE_DIR = PROJECT_DIR / "flash" / "base"
MERGED_BIN = PROJECT_DIR / "flash" / "lovetv-direct-merged.bin"
APP_OFFSET = 0x100000


def write_at(output, offset, source):
    data = source.read_bytes()
    output.seek(offset)
    output.write(data)


def build_merged(source, target, env):
    app_bin = BUILD_DIR / f"{env.subst('$PROGNAME')}.bin"
    required = {
        "bootloader": BASE_DIR / "xiaozhi-bootloader.bin",
        "partition table": BASE_DIR / "xiaozhi-partition-table.bin",
        "OTA data": BASE_DIR / "xiaozhi-ota-data-initial.bin",
        "application": app_bin,
    }
    missing = [f"{name}: {path}" for name, path in required.items() if not path.is_file()]
    if missing:
        raise RuntimeError("Missing merge input(s): " + ", ".join(missing))

    app_size = app_bin.stat().st_size
    if app_size > 0x600000:
        raise RuntimeError(f"Application is too large for ota_0: {app_size} bytes")

    MERGED_BIN.parent.mkdir(parents=True, exist_ok=True)
    with MERGED_BIN.open("wb") as output:
        output.write(b"\xff" * (APP_OFFSET + app_size))
        write_at(output, 0x0000, required["bootloader"])
        write_at(output, 0x8000, required["partition table"])
        write_at(output, 0xD000, required["OTA data"])
        write_at(output, APP_OFFSET, required["application"])

    print(f"LoveTV merged image: {MERGED_BIN} ({MERGED_BIN.stat().st_size} bytes)")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", build_merged)

# Upload the verified boot chain and app separately so the NVS partition at
# 0x9000 is preserved. The stock devkit board would write its own bootloader.
env.Replace(
    UPLOADCMD=(
        '"$PYTHONEXE" "$UPLOADER" --chip esp32s3 --port "$UPLOAD_PORT" '
        '--baud $UPLOAD_SPEED --before default_reset --after hard_reset '
        'write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB '
        f'0x0 "{BASE_DIR / "xiaozhi-bootloader.bin"}" '
        f'0x8000 "{BASE_DIR / "xiaozhi-partition-table.bin"}" '
        f'0xD000 "{BASE_DIR / "xiaozhi-ota-data-initial.bin"}" '
        f'0x100000 "{BUILD_DIR / "firmware.bin"}"'
    )
)
