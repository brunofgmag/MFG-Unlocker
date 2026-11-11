import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pe_image import PE

EXPORT_DIRECTORY = 0
BASE_OFFSET = 16
FUNCTIONS_OFFSET = 28
NAME_OFFSET = 12


def finalize(path, export_name):
    data = bytearray(path.read_bytes())
    pe = PE(bytes(data))
    rva, _ = pe.data_directory(EXPORT_DIRECTORY)
    directory = pe.rva_to_offset(rva)
    name_at = pe.rva_to_offset(struct.unpack_from("<I", data, directory + NAME_OFFSET)[0])
    current = pe.string_at(name_at)
    replacement = export_name.encode()
    if len(replacement) > len(current):
        raise SystemExit(f"{path}: export directory name {current.decode()!r} is too short to hold {export_name!r}")
    data[name_at:name_at + len(current) + 1] = replacement + b"\0" * (len(current) + 1 - len(replacement))
    functions = struct.unpack_from("<I", data, directory + FUNCTIONS_OFFSET)[0]
    if functions % 4:
        raise SystemExit(f"{path}: export address table at RVA {functions:#x} is not 4-byte aligned; "
                         "SelectProxyExports in single_module.cpp rejects the module")
    path.write_bytes(data)


if __name__ == "__main__":
    finalize(Path(sys.argv[1]), sys.argv[2])
