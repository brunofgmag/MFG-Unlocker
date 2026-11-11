import re
import struct

RESOURCE_DIRECTORY = 2
IMPORT_DIRECTORY = 1
EXPORT_DIRECTORY = 0
DELAY_IMPORT_DIRECTORY = 13
OPTIONAL_HEADER_SIZE_OFFSET = 20
DATA_DIRECTORIES_OFFSET = 112
STANDARD_RESOURCE_TYPES = {1: "CURSOR", 2: "BITMAP", 3: "ICON", 4: "MENU", 5: "DIALOG", 6: "STRING", 9: "ACCELERATOR",
                           10: "RCDATA", 14: "GROUP_ICON", 16: "VERSION", 24: "MANIFEST"}


class Section:
    def __init__(self, name, virtual_address, virtual_size, raw_offset, raw_size):
        self.name = name
        self.virtual_address = virtual_address
        self.virtual_size = virtual_size
        self.raw_offset = raw_offset
        self.raw_size = raw_size

    def covers(self, rva):
        return self.virtual_address <= rva < self.virtual_address + max(self.virtual_size, self.raw_size)


class PE:
    def __init__(self, data):
        self.data = data
        header = struct.unpack_from("<I", data, 0x3C)[0]
        if data[:2] != b"MZ" or data[header:header + 4] != b"PE\0\0":
            raise ValueError("not a PE image")
        section_count = struct.unpack_from("<H", data, header + 6)[0]
        optional_size = struct.unpack_from("<H", data, header + OPTIONAL_HEADER_SIZE_OFFSET)[0]
        self.optional = header + 24
        if struct.unpack_from("<H", data, self.optional)[0] != 0x20B:
            raise ValueError("not a PE32+ image")
        self.image_base = struct.unpack_from("<Q", data, self.optional + 24)[0]
        self.sections = []
        for index in range(section_count):
            at = self.optional + optional_size + index * 40
            name = data[at:at + 8].rstrip(b"\0").decode()
            size, address, raw_size, raw_offset = struct.unpack_from("<IIII", data, at + 8)
            self.sections.append(Section(name, address, size, raw_offset, raw_size))

    def section(self, name):
        return next(section for section in self.sections if section.name == name)

    def rva_to_offset(self, rva):
        for section in self.sections:
            if section.covers(rva):
                return rva - section.virtual_address + section.raw_offset
        return None

    def va_to_offset(self, address):
        return self.rva_to_offset(address - self.image_base)

    def data_directory(self, index):
        return struct.unpack_from("<II", self.data, self.optional + DATA_DIRECTORIES_OFFSET + index * 8)

    def string_at(self, offset):
        end = self.data.index(b"\0", offset)
        return self.data[offset:end]

    def string_at_va(self, address, limit=4096):
        offset = self.va_to_offset(address) if address else None
        if offset is None:
            return None
        end = self.data.find(b"\0", offset, offset + limit)
        return None if end < 0 else self.data[offset:end]

    def hex_string_at_va(self, address, pattern):
        text = self.string_at_va(address)
        return text.decode() if text is not None and re.fullmatch(pattern, text) else None

    def resources(self):
        rva, _ = self.data_directory(RESOURCE_DIRECTORY)
        base = self.rva_to_offset(rva)
        found = []

        def label(value):
            if value & 0x80000000:
                at = base + (value & 0x7FFFFFFF)
                length = struct.unpack_from("<H", self.data, at)[0]
                return self.data[at + 2:at + 2 + 2 * length].decode("utf-16le")
            return value

        def walk(offset, path):
            named, numbered = struct.unpack_from("<HH", self.data, base + offset + 12)
            for index in range(named + numbered):
                name, target = struct.unpack_from("<II", self.data, base + offset + 16 + index * 8)
                step = path + [label(name)]
                if target & 0x80000000:
                    walk(target & 0x7FFFFFFF, step)
                else:
                    data_rva, size, _, _ = struct.unpack_from("<IIII", self.data, base + target)
                    start = self.rva_to_offset(data_rva)
                    found.append((step[0], step[1], step[2], self.data[start:start + size]))

        walk(0, [])
        return found

    def exports(self):
        rva, _ = self.data_directory(EXPORT_DIRECTORY)
        if not rva:
            return None, []
        at = self.rva_to_offset(rva)
        name_rva, ordinal_base, function_count, name_count, functions, names, ordinals = struct.unpack_from(
            "<IIIIIII", self.data, at + 12)
        dll_name = self.string_at(self.rva_to_offset(name_rva)).decode()
        by_ordinal = {}
        for index in range(name_count):
            name_at = struct.unpack_from("<I", self.data, self.rva_to_offset(names) + index * 4)[0]
            slot = struct.unpack_from("<H", self.data, self.rva_to_offset(ordinals) + index * 2)[0]
            by_ordinal[ordinal_base + slot] = self.string_at(self.rva_to_offset(name_at)).decode()
        entries = []
        for slot in range(function_count):
            target = struct.unpack_from("<I", self.data, self.rva_to_offset(functions) + slot * 4)[0]
            if target:
                entries.append((ordinal_base + slot, by_ordinal.get(ordinal_base + slot)))
        return dll_name, entries

    def thunk_names(self, table_rva):
        names = []
        at = self.rva_to_offset(table_rva)
        while True:
            value = struct.unpack_from("<Q", self.data, at)[0]
            if not value:
                return names
            if value >> 63:
                names.append(f"#{value & 0xFFFF}")
            else:
                names.append(self.string_at(self.rva_to_offset(value & 0x7FFFFFFF) + 2).decode())
            at += 8

    def imports(self):
        rva, _ = self.data_directory(IMPORT_DIRECTORY)
        found = {}
        if not rva:
            return found
        at = self.rva_to_offset(rva)
        while True:
            lookup, _, _, name_rva, address_table = struct.unpack_from("<IIIII", self.data, at)
            if not name_rva:
                return found
            dll = self.string_at(self.rva_to_offset(name_rva)).decode().lower()
            found[dll] = sorted(self.thunk_names(lookup or address_table))
            at += 20

    def delay_imports(self):
        rva, _ = self.data_directory(DELAY_IMPORT_DIRECTORY)
        found = {}
        if not rva:
            return found
        at = self.rva_to_offset(rva)
        while True:
            _, name_rva, _, _, name_table = struct.unpack_from("<IIIII", self.data, at)
            if not name_rva:
                return found
            dll = self.string_at(self.rva_to_offset(name_rva)).decode().lower()
            found[dll] = sorted(self.thunk_names(name_table))
            at += 32


def resource_type_name(value):
    return STANDARD_RESOURCE_TYPES.get(value, str(value)) if isinstance(value, int) else value
