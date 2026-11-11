import argparse
import ast
import collections
import hashlib
import itertools
import json
import os
import re
import shutil
import struct
import sys
import zipfile
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pe_image import PE

RELEASE_ZIP_SHA256 = "5bd19d03288e1ece1d0bb2030ac072b36758045028da192044dcaad73fd901dd"
RELEASE_DLL_NAME = "MFGUnlocker.dll"
RELEASE_DLL_SHA256 = "e6b6e6b70a6ed3af839c9647a32fc22660ae6b2d655062c5c072246c91fc10c7"
PLACEHOLDER = "@MFG_PREGENERATED_INPUTS@"

UPPER_SHA = rb"[0-9A-F]{64}"
LOWER_SHA = rb"[0-9a-f]{64}"
FIRST_KERNEL_ID = 4000
FIRST_ADDITIONAL_ID = 4200
FIRST_XESS_PROGRAM_ID = 7000
XESS_PROGRAM_IDS = 30
XESS_MANIFEST_ID = 7030
FIRST_XESS_SHADER_ID = 7100
CUDA_FATBIN_ID = 7300
FIRST_XVK_ID = 7400
XVK_BINDING_STRIDE = 4
XVK_ROW_FORMAT = "<QIIQIIII"
XVK_ROW_SIZE = struct.calcsize(XVK_ROW_FORMAT)
NATIVE_PROGRAM_FORMAT = "<QQQQI"
NATIVE_PROGRAM_SIZE = 40
ENTRY_SIZE = 24
CONTRACT_FORMAT = "<QI3IIIIIQI"
CONTRACT_SIZE = 56
NATIVE_PROGRAM_COUNT = 13
GLSLANG_GENERATOR = 0x0008000A
OP_FUNCTION_END = 56
OP_DECORATE = 71
DECORATION_BINDING = 33
SPIRV_MAGIC = struct.pack("<I", 0x07230203)
XVK_DESCRIPTOR_TYPES = 5

SOURCE_PINS = {
    "ui_color.frag": "4747aafbef6db4bc074547aab6bdc04dab336985e058621f3a2d895a420fa950",
    "xess/port/splat_motion_aware.hlsl": "4c172018c46bb0182fb998f3fa08bb1e7ef00a2edabb263d42b2a888739b8787",
    "xess/port/unet_conv.hlsl": "ca09a0d96f9233c95c103312522ad77e81c2e075059743030109781b6e6b3c00",
    "xess/cuda/xefg_cuda_layers.cu": "b1094f1c3b1ea08bcbae9eaad3050fb91cd7ffcf6d612268621072bf6f4e9a5f",
    "xess/cuda/xefg_cuda_params.h": "b520c18b841ac88d3c217e8e9a4c6f642a92ccdeaef002289370ee871f8c2adb",
    "xess/vulkan/vk_input_copy.comp": "090471caa0da37ee6bc1ed91111591f6be414be82f492e13e8cf0788dcdf82e2",
}

OUTPUT_PINS = {
    "native-cache-3109/ampere_native_manifest.inc": "b4f82f778bced208c61c2458bbc3d0078fefdd0999f4e7b1238942527cd74fd8",
    "native-cache-additional/additional-resources.json": "d2cac872e50154259dbd1bdfafca5d4315525eacd9966bd5cefda166f4ada9bf",
    "native-cache-additional/ampere_native_manifest_additional.inc": "ac3a4286d7070de4f1b6a37de41995a20345c1e817301445b53f0dd90f0ec6b0",
    "native-cache/ampere_native_manifest.inc": "acae411f9614017bccd2ad1a7a4e6f5529fd11cec76ce90b6d4193297efc1170",
    "pregenerated/embedded_xefg_cuda.generated.h": "59dd89147cce9ab2ba23bf7ca1434d02b389d9c4732102e0b300de553843845d",
    "pregenerated/embedded_xefg_cuda.rc.in": "d97025b979ecb2e683998878aac6687b775e3242677b0b904c600e000791bed9",
    "pregenerated/embedded_xess_shaders.generated.h": "72a6b76b5f4366e12f70d49a12e22376db719a0b4e4c8c22308ac4bb8bf0b446",
    "pregenerated/embedded_xess_shaders.rc.in": "2d02a1acd9c5fce4087b0480b6f07ecd8c478486f15d8cadc28692f40d504658",
    "pregenerated/embedded_xvk_programs.generated.h": "e3e77b1c56b0ad4f21490a174d0cd166567ff1fcc40e831f7c4fe8a7e948de4e",
    "pregenerated/embedded_xvk_programs.rc.in": "c8dfc9c1ec4adc8844ba23ac6ffe3b20ccee7eb60d9545674ad8c9282753dc7f",
    "pregenerated/vk_input_copy_spv.h": "056432fdb3a0c9f476c3c62ddbdf210ee990a71bf4abdd2d2958617640564d27",
}


Contract = collections.namedtuple("Contract", "name parameters threads shared registers abi flags metadata count")


class InputError(Exception):
    pass


def reject(path, reason):
    raise InputError(f"{path}: {reason}")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def digest_of(path):
    if not path.is_file():
        reject(path, "missing")
    return digest(path.read_bytes())


def expect_digest(path, expected):
    actual = digest_of(path)
    if actual.lower() != expected.lower():
        reject(path, f"sha256 {actual} does not match the pin {expected}")


def write_bytes(root, relative, data):
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write_text(root, relative, text):
    write_bytes(root, relative, text.encode("utf-8"))


class RepoPins:
    def __init__(self, native):
        self.native = native

    def assignments(self, relative):
        tree = ast.parse((self.native / relative).read_text(encoding="utf-8"))
        return {node.targets[0].id: node.value for node in tree.body
                if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)}

    def constant(self, relative, name):
        return ast.literal_eval(self.assignments(relative)[name])

    def xess_shaders(self):
        rows = self.assignments("xess/generate_xess_shaders.py")["SHADERS"].elts
        return [(ast.literal_eval(row.elts[0]), ast.literal_eval(row.elts[-1])) for row in rows]

    def input_copy(self):
        return self.constant("xess/vulkan/generate_vk_input_shaders.py", "VARIANTS")

    def overlay_header(self):
        pins = json.loads((self.native / "generated_sha256.json").read_text(encoding="utf-8"))
        return pins["overlay_color_spv.h"]

    def xess_manifest(self):
        return self.constant("xess/generate_xess_programs.py", "MANIFEST_SHA256")

    def xvk_manifest(self):
        return self.constant("xess/vulkan/generate_xvk_programs.py", "MANIFEST_SHA256")

    def cuda_fatbin(self):
        return self.constant("xess/cuda/generate_cuda_layers.py", "PINNED_FATBIN")


def read_release_dll(zip_path):
    if not zip_path.is_file():
        reject(zip_path, "release zip not found")
    if digest_of(zip_path) != RELEASE_ZIP_SHA256:
        reject(zip_path, f"sha256 {digest_of(zip_path)} is not the v1.7.0 release zip {RELEASE_ZIP_SHA256}; refusing to extract")
    with zipfile.ZipFile(zip_path) as archive:
        if RELEASE_DLL_NAME not in archive.namelist():
            reject(zip_path, f"has no {RELEASE_DLL_NAME}")
        data = archive.read(RELEASE_DLL_NAME)
    if digest(data) != RELEASE_DLL_SHA256:
        reject(zip_path, f"{RELEASE_DLL_NAME} sha256 {digest(data)} does not match {RELEASE_DLL_SHA256}")
    return data


def rdata_offsets(pe):
    section = pe.section(".rdata")
    return range(section.raw_offset, section.raw_offset + section.raw_size - 64, 8)


def read_kernel_rows(pe, start):
    rows = []
    while True:
        pointer, identifier = struct.unpack_from("<QI", pe.data, start + 16 * len(rows))
        sha = pe.hex_string_at_va(pointer, UPPER_SHA)
        if sha is None:
            return rows
        rows.append((sha, identifier))


def find_kernel_table(pe):
    for start in rdata_offsets(pe):
        pointer, identifier = struct.unpack_from("<QI", pe.data, start)
        if identifier == FIRST_KERNEL_ID and pe.hex_string_at_va(pointer, UPPER_SHA):
            return read_kernel_rows(pe, start)
    reject(RELEASE_DLL_NAME, "kKernelResources table not found")


def read_entries(pe, address, count):
    start = pe.va_to_offset(address)
    if start is None:
        return None
    entries = []
    for index in range(count):
        ptx, fatbin, size = struct.unpack_from("<QQI", pe.data, start + ENTRY_SIZE * index)
        ptx_sha = pe.hex_string_at_va(ptx, UPPER_SHA)
        fatbin_sha = pe.hex_string_at_va(fatbin, UPPER_SHA)
        if not ptx_sha or not fatbin_sha:
            return None
        entries.append((ptx_sha, fatbin_sha, size))
    return entries


def read_contracts(pe, address, count):
    start = pe.va_to_offset(address)
    if start is None:
        return None
    contracts = []
    for index in range(count):
        fields = struct.unpack_from(CONTRACT_FORMAT, pe.data, start + CONTRACT_SIZE * index)
        name = pe.string_at_va(fields[0])
        metadata = pe.hex_string_at_va(fields[9], UPPER_SHA)
        if not name or not re.fullmatch(rb"[A-Za-z_][A-Za-z0-9_]{2,200}", name) or not metadata:
            return None
        contracts.append(Contract(name.decode(), fields[1], fields[2:5], *fields[5:9], metadata, fields[10]))
    return contracts


def find_native_programs(pe):
    programs = []
    for start in rdata_offsets(pe):
        entries_at, entry_count, contracts_at, contract_count, first = struct.unpack_from(
            NATIVE_PROGRAM_FORMAT, pe.data, start)
        if not (20 <= entry_count <= 38 and 3900 < first < 8000 and contract_count == entry_count):
            continue
        entries = read_entries(pe, entries_at, entry_count)
        contracts = read_contracts(pe, contracts_at, contract_count) if entries else None
        if entries and contracts:
            programs.append({"first": first, "entries": entries, "contracts": contracts})
    if len(programs) != NATIVE_PROGRAM_COUNT:
        reject(RELEASE_DLL_NAME, f"found {len(programs)} native program tables, expected {NATIVE_PROGRAM_COUNT}")
    return sorted(programs, key=lambda program: program["first"])


def format_entries(name, entries):
    rows = ",\n".join(f'    {{"{ptx}", "{fatbin}", {size}u}}' for ptx, fatbin, size in entries)
    return f"constexpr std::array<Entry, {len(entries)}> {name}{{{{\n{rows}\n}}}};\n"


def format_contract(contract):
    x, y, z = contract.threads
    return (f'    {{"{contract.name}", {contract.parameters}u, {{{x}u, {y}u, {z}u}}, {contract.shared}u, '
            f'{contract.registers}u, {contract.abi}u, {contract.flags}u, "{contract.metadata}", {contract.count}u}}')


def format_contracts(name, contracts):
    rows = ",\n".join(format_contract(contract) for contract in contracts)
    return f"constexpr std::array<NativeContract, {len(contracts)}> {name}{{{{\n{rows}\n}}}};\n"


def format_single_manifest(program):
    return (format_entries("kNativeManifest", program["entries"])
            + format_contracts("kNativeContracts", program["contracts"])
            + "#define MFG_AMPERE_NATIVE_MANIFEST_HAS_CONTRACTS\n")


def format_additional_manifest(programs):
    text = ""
    for index, program in enumerate(programs):
        text += format_entries(f"kAdditional{index}Entries", program["entries"])
        text += format_contracts(f"kAdditional{index}Contracts", program["contracts"])
    rows = ",\n".join(f"    {{kAdditional{index}Entries, kAdditional{index}Contracts, {program['first']}}}"
                      for index, program in enumerate(programs))
    return text + f"constexpr NativeProgram kAdditionalNativePrograms[]{{\n{rows}\n}};\n"


def additional_resources(programs, manifest_text):
    resources = [{"id": program["first"] + index, "sha256": fatbin, "bytes": size}
                 for program in programs for index, (_, fatbin, size) in enumerate(program["entries"])]
    return json.dumps({"schema": 1, "manifest_sha256": digest(manifest_text.encode()).upper(),
                       "resources": resources}, indent=1) + "\n"


def embed_order(single_manifests, resources):
    assigned = []
    ordered = [fatbin for program in single_manifests for _, fatbin, _ in program["entries"]]
    ordered += [resource["sha256"] for resource in resources]
    for sha in ordered:
        if sha not in assigned:
            assigned.append(sha)
    return [(sha, FIRST_KERNEL_ID + index) for index, sha in enumerate(assigned)]


def spirv_header_ok(data, start):
    version, _, bound, schema = struct.unpack_from("<IIII", data, start + 4)
    return schema == 0 and 0 < bound <= 100000 and (version >> 24) == 0


def instruction_ends(data, start, limit):
    position = start + 20
    ends = []
    while position + 4 <= limit:
        word = struct.unpack_from("<I", data, position)[0]
        count, opcode = word >> 16, word & 0xFFFF
        if count == 0 or opcode > 6000 or position + 4 * count > limit:
            break
        position += 4 * count
        ends.append((position, opcode))
    return ends


def spirv_modules(pe):
    section = pe.section(".rdata")
    limit = section.raw_offset + section.raw_size
    start = section.raw_offset
    modules = []
    while True:
        start = pe.data.find(SPIRV_MAGIC, start, limit)
        if start < 0:
            return modules
        if start % 4 == 0 and spirv_header_ok(pe.data, start):
            generator = struct.unpack_from("<I", pe.data, start + 8)[0]
            modules.append((start, generator, instruction_ends(pe.data, start, limit)))
        start += 4


def words_array(name, raw):
    words = struct.unpack(f"<{len(raw) // 4}I", raw)
    lines = [f"inline constexpr uint32_t {name}[] = {{"]
    for at in range(0, len(words), 8):
        lines.append("    " + ",".join(f"0x{word:08x}u" for word in words[at:at + 8]) + ",")
    return lines + ["};"]


def overlay_header_text(blobs):
    lines = ["#pragma once", "#include <cstdint>", "namespace single_overlay::color {"]
    for mode, blob in zip((1, 2, 3), blobs):
        lines += words_array(f"shader{mode}", blob)
    return "\n".join(lines + ["}"]) + "\n"


def input_copy_header_text(blobs):
    lines = ["#pragma once", "#include <cstdint>", "namespace xess_vulkan::input_copy {"]
    for target in sorted(blobs):
        lines += words_array(f"shader{target}", blobs[target])
    return "\n".join(lines + ["}"]) + "\n"


def find_input_copy(pe, pins):
    wanted = {value.upper(): target for target, value in pins.items()}
    found = {}
    taken = set()
    for start, _, ends in spirv_modules(pe):
        for end, _ in ends:
            target = wanted.get(digest(pe.data[start:end]).upper())
            if target is not None:
                found[target] = pe.data[start:end]
                taken.add(start)
    if sorted(found) != sorted(pins):
        reject(RELEASE_DLL_NAME, f"vk_input_copy SPIR-V variants found {sorted(found)}, expected {sorted(pins)}")
    return found, taken


def find_overlay(pe, taken, pin):
    candidates = [(start, ends) for start, generator, ends in spirv_modules(pe)
                  if generator == GLSLANG_GENERATOR and start not in taken]
    if len(candidates) != 3:
        reject(RELEASE_DLL_NAME, f"found {len(candidates)} overlay SPIR-V modules, expected 3")
    choices = [[end for end, opcode in ends if opcode == OP_FUNCTION_END] for _, ends in candidates]
    for combination in itertools.product(*choices):
        blobs = [pe.data[start:end] for (start, _), end in zip(candidates, combination)]
        if digest(overlay_header_text(blobs).encode()).upper() == pin.upper():
            return blobs
    reject(RELEASE_DLL_NAME, f"no overlay SPIR-V split reproduces overlay_color_spv.h pin {pin}")


def spirv_bindings(raw):
    bindings = set()
    position = 20
    while position + 4 <= len(raw):
        word = struct.unpack_from("<I", raw, position)[0]
        count, opcode = word >> 16, word & 0xFFFF
        if count == 0:
            break
        if opcode == OP_DECORATE and struct.unpack_from("<I", raw, position + 8)[0] == DECORATION_BINDING:
            bindings.add(struct.unpack_from("<I", raw, position + 12)[0])
        position += 4 * count
    return bindings


def read_xvk_rows(pe, start):
    rows = []
    while True:
        key, resource, _, spirv, size, push, first, count = struct.unpack_from(
            XVK_ROW_FORMAT, pe.data, start + XVK_ROW_SIZE * len(rows))
        key_sha = pe.hex_string_at_va(key, LOWER_SHA)
        spirv_sha = pe.hex_string_at_va(spirv, LOWER_SHA)
        if not (key_sha and spirv_sha and FIRST_XVK_ID <= resource < FIRST_XVK_ID + 200):
            return rows
        rows.append((key_sha, resource, spirv_sha, size, push, first, count))


def read_xvk_bindings(pe, table_start, rows):
    total = rows[-1][5] + rows[-1][6]
    at = table_start - XVK_BINDING_STRIDE * total
    return [struct.unpack_from("<HH", pe.data, at + XVK_BINDING_STRIDE * index) for index in range(total)]


def find_xvk_table(pe):
    for start in rdata_offsets(pe):
        pointer, identifier = struct.unpack_from("<QI", pe.data, start)
        if identifier == FIRST_XVK_ID and pe.hex_string_at_va(pointer, LOWER_SHA):
            rows = read_xvk_rows(pe, start)
            return rows, read_xvk_bindings(pe, start, rows)
    reject(RELEASE_DLL_NAME, "xvk program table not found")


def format_xvk_header(manifest, rows, bindings):
    lines = ["#pragma once", '#include "xvk_embedded.h"', "namespace xvk::embedded {",
             f'inline constexpr char manifestSha256[] = "{manifest}";', "inline constexpr BindingRow bindings[] = {"]
    lines += [f"    {{{binding}, {kind}}}," for binding, kind in bindings]
    lines += ["};", "inline constexpr ProgramAsset programs[] = {"]
    lines += [f'    {{"{key}", {resource}, "{spirv}", {size}, {push}, {first}, {count}}},'
              for key, resource, spirv, size, push, first, count in rows]
    return "\n".join(lines + ["};", "}"]) + "\n"


def resource_script(entries):
    lines = ["#include <windows.h>", "LANGUAGE LANG_NEUTRAL, SUBLANG_NEUTRAL"]
    lines += [f'{identifier} RCDATA "{PLACEHOLDER}/{relative}"' for identifier, relative in entries]
    return "\n".join(lines) + "\n"


def format_xess_shader_header(shaders):
    lines = ["#pragma once", '#include "runtime_payload.h"', "namespace xess_payload::embedded {",
             "inline constexpr Asset shaders[] = {"]
    lines += [f'    {{{identifier}, "shaders/{name}.dxil", "{digest(data)}", {len(data)}}},'
              for identifier, name, data in shaders]
    return "\n".join(lines + ["};", "}"]) + "\n"


def format_cuda_header(data):
    return "\n".join(["#pragma once", '#include "runtime_payload.h"', "namespace xefg_cuda::embedded {",
                      f'inline constexpr xess_payload::Asset fatbin{{{CUDA_FATBIN_ID}, "cuda/xefg_cuda_layers.fatbin", '
                      f'"{digest(data)}", {len(data)}}};', "}"]) + "\n"


def collect_rcdata(pe):
    return {identifier: data for kind, identifier, _, data in pe.resources() if kind == 10}


def collect_xess_programs(rcdata, pins):
    manifest = rcdata[XESS_MANIFEST_ID]
    if digest(manifest) != pins.xess_manifest():
        reject(f"{RELEASE_DLL_NAME} RCDATA {XESS_MANIFEST_ID}", "XeSS program manifest does not match the pin")
    by_digest = {digest(rcdata[identifier]): rcdata[identifier]
                 for identifier in range(FIRST_XESS_PROGRAM_ID, FIRST_XESS_PROGRAM_ID + XESS_PROGRAM_IDS)}
    programs = {}
    for line in manifest.decode("ascii").splitlines():
        key, wanted = line.split("\t")
        if wanted not in by_digest:
            reject(f"{RELEASE_DLL_NAME} program {key}", f"no RCDATA resource has sha256 {wanted}")
        programs[key] = by_digest[wanted]
    return manifest, programs


def collect_xess_shaders(rcdata, pins):
    shaders = []
    for index, (name, pinned) in enumerate(pins.xess_shaders()):
        identifier = FIRST_XESS_SHADER_ID + index
        if digest(rcdata[identifier]) != pinned:
            reject(f"{RELEASE_DLL_NAME} RCDATA {identifier} ({name})", f"sha256 does not match the generator pin {pinned}")
        shaders.append((identifier, name, rcdata[identifier]))
    return shaders


def write_ampere_inputs(pe, rcdata, root):
    kernel_table = find_kernel_table(pe)
    programs = find_native_programs(pe)
    single = [program for program in programs if program["first"] in (4000, 4100)]
    additional = [program for program in programs if program["first"] >= FIRST_ADDITIONAL_ID]
    additional_text = format_additional_manifest(additional)
    resources_text = additional_resources(additional, additional_text)
    if embed_order(single, json.loads(resources_text)["resources"]) != kernel_table:
        reject(RELEASE_DLL_NAME, "the native program tables do not reproduce the release's kernel resource order")
    for sha, identifier in kernel_table:
        write_bytes(root, f"kernels/{sha}.fatbin", rcdata[identifier])
    write_text(root, "native-cache/ampere_native_manifest.inc", format_single_manifest(single[0]))
    write_text(root, "native-cache-3109/ampere_native_manifest.inc", format_single_manifest(single[1]))
    write_text(root, "native-cache-additional/ampere_native_manifest_additional.inc", additional_text)
    write_text(root, "native-cache-additional/additional-resources.json", resources_text)


def write_xess_inputs(rcdata, root, pins):
    manifest, xess_programs = collect_xess_programs(rcdata, pins)
    write_bytes(root, "xess-programs/programs/programs.tsv", manifest)
    for key, data in xess_programs.items():
        write_bytes(root, f"xess-programs/programs/{key}.dxil", data)
    shaders = collect_xess_shaders(rcdata, pins)
    for _, name, data in shaders:
        write_bytes(root, f"pregenerated/xess_shaders/{name}.dxil", data)
    write_text(root, "pregenerated/embedded_xess_shaders.generated.h", format_xess_shader_header(shaders))
    write_text(root, "pregenerated/embedded_xess_shaders.rc.in",
               resource_script([(identifier, f"xess_shaders/{name}.dxil") for identifier, name, _ in shaders]))
    write_bytes(root, "pregenerated/xess_cuda/xefg_cuda_layers.fatbin", rcdata[CUDA_FATBIN_ID])
    write_text(root, "pregenerated/embedded_xefg_cuda.generated.h", format_cuda_header(rcdata[CUDA_FATBIN_ID]))
    write_text(root, "pregenerated/embedded_xefg_cuda.rc.in",
               resource_script([(CUDA_FATBIN_ID, "xess_cuda/xefg_cuda_layers.fatbin")]))


def write_xvk_inputs(pe, rcdata, root, pins):
    rows, bindings = find_xvk_table(pe)
    for key, resource, _, _, _, _, _ in rows:
        write_bytes(root, f"pregenerated/xvk/{key}.spv", rcdata[resource])
    write_text(root, "pregenerated/embedded_xvk_programs.generated.h",
               format_xvk_header(pins.xvk_manifest(), rows, bindings))
    write_text(root, "pregenerated/embedded_xvk_programs.rc.in",
               resource_script([(resource, f"xvk/{key}.spv") for key, resource, _, _, _, _, _ in rows]))


def write_spirv_inputs(pe, root, pins):
    input_copy, taken = find_input_copy(pe, pins.input_copy())
    for target, blob in input_copy.items():
        write_bytes(root, f"pregenerated/vk_input_copy_{target}.spv", blob)
    write_text(root, "pregenerated/vk_input_copy_spv.h", input_copy_header_text(input_copy))
    for mode, blob in enumerate(find_overlay(pe, taken, pins.overlay_header()), 1):
        write_bytes(root, f"pregenerated/ui_color_{mode}.spv", blob)


def build_outputs(pe, root, pins):
    rcdata = collect_rcdata(pe)
    write_ampere_inputs(pe, rcdata, root)
    write_xess_inputs(rcdata, root, pins)
    write_xvk_inputs(pe, rcdata, root, pins)
    write_spirv_inputs(pe, root, pins)
    write_text(root, "release.sha256", f"{RELEASE_ZIP_SHA256}  MFGUnlocker-v1.7.0.zip\n"
                                      f"{RELEASE_DLL_SHA256}  {RELEASE_DLL_NAME}\n")


def verify_pinned_files(root):
    for relative, expected in OUTPUT_PINS.items():
        expect_digest(root / relative, expected)


def verify_sources(native):
    for relative, expected in SOURCE_PINS.items():
        path = native / relative
        if not path.is_file():
            reject(path, "missing")
        actual = digest(path.read_bytes().replace(b"\r\n", b"\n"))
        if actual != expected:
            reject(path, f"sha256 {actual} does not match {expected}: the pre-built shader output extracted from the "
                         "release was made from another source, and Linux cannot compile this one")


def listed_kernels(root):
    listed = {}
    for relative in ("native-cache/ampere_native_manifest.inc", "native-cache-3109/ampere_native_manifest.inc"):
        text = (root / relative).read_text(encoding="utf-8")
        for fatbin, size in re.findall(r'\{"[0-9A-F]{64}", "([0-9A-F]{64})", ([0-9]+)u\}', text):
            listed[fatbin] = int(size)
    resources = json.loads((root / "native-cache-additional/additional-resources.json").read_text(encoding="utf-8"))
    for resource in resources["resources"]:
        listed[resource["sha256"]] = resource["bytes"]
    return listed


def verify_kernels(root):
    wanted = listed_kernels(root)
    for sha, size in wanted.items():
        path = root / "kernels" / f"{sha}.fatbin"
        expect_digest(path, sha)
        if path.stat().st_size != size:
            reject(path, f"is {path.stat().st_size} bytes, the manifest says {size}")
    present = {path.stem for path in (root / "kernels").iterdir()}
    if present != set(wanted):
        reject(root / "kernels", f"holds {sorted(present ^ set(wanted))[:3]} that no manifest lists or is missing them")


def verify_xess_programs(root, pins):
    directory = root / "xess-programs/programs"
    expect_digest(directory / "programs.tsv", pins.xess_manifest())
    for line in (directory / "programs.tsv").read_text(encoding="ascii").splitlines():
        key, wanted = line.split("\t")
        expect_digest(directory / f"{key}.dxil", wanted)


def verify_xess_shaders(root, pins):
    for name, pinned in pins.xess_shaders():
        expect_digest(root / f"pregenerated/xess_shaders/{name}.dxil", pinned)
    expect_digest(root / "pregenerated/xess_cuda/xefg_cuda_layers.fatbin", pins.cuda_fatbin())


def verify_xvk(root):
    header = (root / "pregenerated/embedded_xvk_programs.generated.h").read_text(encoding="utf-8")
    bindings = [int(binding) for binding, _ in re.findall(r"\{([0-9]+), ([0-9]+)\},", header)]
    rows = re.findall(r'\{"([0-9a-f]{64})", ([0-9]+), "([0-9a-f]{64})", ([0-9]+), [0-9]+, ([0-9]+), ([0-9]+)\},', header)
    if len(rows) != 83:
        reject(root / "pregenerated/embedded_xvk_programs.generated.h", f"lists {len(rows)} programs, expected 83")
    for key, _, spirv, size, first, count in rows:
        path = root / f"pregenerated/xvk/{key}.spv"
        expect_digest(path, spirv)
        if path.stat().st_size != int(size):
            reject(path, f"is {path.stat().st_size} bytes, the program table says {size}")
        declared = set(bindings[int(first):int(first) + int(count)])
        if declared != spirv_bindings(path.read_bytes()):
            reject(path, "SPIR-V bindings differ from the program table")


def verify_shader_blobs(root, pins):
    for target, pinned in pins.input_copy().items():
        expect_digest(root / f"pregenerated/vk_input_copy_{target}.spv", pinned)
    blobs = [(root / f"pregenerated/ui_color_{mode}.spv").read_bytes() for mode in (1, 2, 3)]
    if digest(overlay_header_text(blobs).encode()).upper() != pins.overlay_header().upper():
        reject(root / "pregenerated/ui_color_1.spv", "the three overlay SPIR-V files do not reproduce the overlay_color_spv.h pin")


def verify_release_record(root):
    expected = (f"{RELEASE_ZIP_SHA256}  MFGUnlocker-v1.7.0.zip\n{RELEASE_DLL_SHA256}  {RELEASE_DLL_NAME}\n")
    path = root / "release.sha256"
    if not path.is_file() or path.read_text(encoding="utf-8") != expected:
        reject(path, "does not record the v1.7.0 release zip and DLL")


def verify(root, native):
    pins = RepoPins(native)
    verify_release_record(root)
    verify_pinned_files(root)
    verify_xess_programs(root, pins)
    verify_xess_shaders(root, pins)
    verify_shader_blobs(root, pins)
    verify_kernels(root)
    verify_xvk(root)
    verify_sources(native)


def extract(zip_path, output, native):
    pe = PE(read_release_dll(zip_path))
    partial = output.with_name(output.name + f".partial-{os.getpid()}")
    shutil.rmtree(partial, ignore_errors=True)
    try:
        build_outputs(pe, partial, RepoPins(native))
        verify(partial, native)
        output.parent.mkdir(parents=True, exist_ok=True)
        partial.rename(output)
    finally:
        shutil.rmtree(partial, ignore_errors=True)


def pinned_candidates(root):
    suffixes = (".inc", ".json", ".h", ".in")
    return sorted(str(path.relative_to(root)) for path in root.rglob("*") if path.is_file() and path.suffix in suffixes)


def save_release_dll(zip_path, output):
    data = read_release_dll(zip_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)


def print_pins(root):
    for relative in pinned_candidates(root):
        print(f'    "{relative}": "{digest_of(root / relative)}",')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("extract", "verify", "pins", "release-dll"))
    parser.add_argument("--zip", type=Path)
    parser.add_argument("--inputs", type=Path)
    parser.add_argument("--native", type=Path, default=Path(__file__).resolve().parents[2] / "source" / "native")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "extract":
            if args.zip is None or args.inputs is None:
                parser.error("extract needs --zip and --inputs")
            extract(args.zip, args.inputs, args.native)
            print(f"extracted and verified {args.inputs}")
        elif args.command == "verify":
            verify(args.inputs, args.native)
            print(f"verified {args.inputs}")
        elif args.command == "release-dll":
            if args.zip is None or args.output is None:
                parser.error("release-dll needs --zip and --output")
            save_release_dll(args.zip, args.output)
        else:
            print_pins(args.inputs)
    except InputError as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
