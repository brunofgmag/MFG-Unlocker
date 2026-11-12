import argparse
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

PIN_LINE = re.compile(r"Assert-InputHash \(Join-Path \$(\w+) '([^']+)'\) '([0-9A-F]{64})'( -NormalizeText)?")
ROOT_NAMES = {"StreamlineRoot": "streamline", "ImGuiRoot": "imgui", "VulkanIncludeDirectory": "vulkan12",
              "XvkVulkanIncludeDirectory": "vulkan14", "XessSdkRoot": "xess"}
COPY_FOLDERS = {"streamline": "streamline", "xess": "xess", "imgui": "imgui-3912b3d", "vulkan12": "vulkan-1.2.176.1",
                "vulkan14": "vulkan-1.4.363.0"}
VULKAN12_TAG = "v1.2.176"
SUPPLEMENTARY_PINS = {
    "vulkan14": [
        ("vk_video/vulkan_video_codec_av1std.h", "110D61F53214322F4D0AEC8F77A7CEC40C1517829B125C0D01D6FC9FDF0FDD83"),
        ("vk_video/vulkan_video_codec_av1std_decode.h", "FA2E85E9669F1666BADFAB9211DE9DBE279BC2ADC3ED095635313712CED9219D"),
        ("vk_video/vulkan_video_codec_av1std_encode.h", "5BB49198AC7A63C810115C62C9AB44B2E60F20C60869A68A813730F30777DB5F"),
        ("vk_video/vulkan_video_codec_h264std.h", "84D669C6010443E268F940E57A26E3FF0C4050C9C434B79EFAAA1BFA3D3D203C"),
        ("vk_video/vulkan_video_codec_h264std_decode.h", "FBEE59666126C323B2BACC0B4B2C6FD1D1A28CED72E4C6791026A730F05BC129"),
        ("vk_video/vulkan_video_codec_h264std_encode.h", "CD7E050BC0062D791A5E4E3CC97EFDD4B9B3838214168785743FB48EBD5A041A"),
        ("vk_video/vulkan_video_codec_h265std.h", "D3AE5C05DCA0765D950E53AA1AAFC9526BE8BE6BE72B373803C70C94086665A1"),
        ("vk_video/vulkan_video_codec_h265std_decode.h", "4CE92DC8FF11F8F9652B01713992F1235B6C88694A658B8FE8777AC7C79FB5DF"),
        ("vk_video/vulkan_video_codec_h265std_encode.h", "182114CA162AAE8B3C1ED3C9693852126B2DDA009712E387A1AB6F475AB8058E"),
        ("vk_video/vulkan_video_codec_vp9std.h", "B4A35526DDF93652C6244E26A71F6D2D9536C0BD5AEBDE35ED777D1CD8A64978"),
        ("vk_video/vulkan_video_codec_vp9std_decode.h", "AC1AC9E1494A26855E327001FA6FFA26020EB5BF852DAB1EA3E89939CED3D5EE"),
        ("vk_video/vulkan_video_codecs_common.h", "CF775E4D74384E3D4ED5657FB7D87FAA4A7EAF6FEE21F4F4113640565494A924"),
        ("spirv/unified1/spirv.core.grammar.json", "B3A3BE9CE86E016BB0C0328EC4F2D5876BD7AF5F0386ADFAA544EEC1C89FE080"),
    ],
}
BYTE_ORDER_MARK = b"\xef\xbb\xbf"


class PinError(Exception):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest().upper()


def crlf(data):
    return re.sub(rb"\r?\n", b"\r\n", data)


def normalized_text(data):
    return data.removeprefix(BYTE_ORDER_MARK).replace(b"\r\n", b"\n")


class DirectorySource:
    def __init__(self, root):
        self.root = root

    def describe(self, relative):
        return str(self.root / relative)

    def read(self, relative):
        path = self.root / relative
        if not path.is_file():
            raise PinError(f"{path}: missing")
        return path.read_bytes()


class GitSource:
    def __init__(self, repository, tag, prefix):
        self.repository = repository
        self.tag = tag
        self.prefix = prefix

    def describe(self, relative):
        return f"{self.repository} {self.tag}:{self.prefix}{relative}"

    def read(self, relative):
        shown = subprocess.run(["git", "-C", str(self.repository), "show", f"{self.tag}:{self.prefix}{relative}"],
                               capture_output=True)
        if shown.returncode != 0:
            raise PinError(f"{self.describe(relative)}: {shown.stderr.decode(errors='replace').strip()}")
        return shown.stdout


def read_pins(build_script):
    pins = {}
    for root, relative, expected, normalize in PIN_LINE.findall(build_script.read_text(encoding="utf-8-sig")):
        pins.setdefault(root, []).append((relative, expected, bool(normalize)))
    missing = [name for name in (*ROOT_NAMES, "sourceDirectory") if name not in pins]
    if missing:
        raise PinError(f"{build_script}: no pins found for {missing}")
    return pins


def matching_form(source, relative, expected):
    data = source.read(relative)
    if digest(data) == expected:
        return data, False
    if digest(crlf(data)) == expected:
        return crlf(data), True
    raise PinError(f"{source.describe(relative)}: sha256 {digest(data)} (CRLF form {digest(crlf(data))}) "
                   f"does not match the pin {expected}")


def verify_directory(root, pinned):
    for relative, expected, _ in pinned:
        path = root / relative
        if not path.is_file():
            raise PinError(f"{path}: missing")
        if digest(path.read_bytes()) != expected:
            raise PinError(f"{path}: sha256 {digest(path.read_bytes())} does not match the pin {expected}; "
                           f"delete {root} to rebuild it from the source")


def materialize(source, pinned, copy):
    staging = copy.with_name(copy.name + ".partial")
    shutil.rmtree(staging, ignore_errors=True)
    for relative, expected, _ in pinned:
        data, _ = matching_form(source, relative, expected)
        target = staging / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    copy.parent.mkdir(parents=True, exist_ok=True)
    staging.rename(copy)


def resolve(source, pinned, copy):
    if copy.is_dir():
        verify_directory(copy, pinned)
        return copy
    converted = [matching_form(source, relative, expected)[1] for relative, expected, _ in pinned]
    if not any(converted) and isinstance(source, DirectorySource):
        return source.root
    materialize(source, pinned, copy)
    verify_directory(copy, pinned)
    return copy


def check_native_sources(native, pinned):
    for relative, expected, normalize in pinned:
        path = native / relative
        if not path.is_file():
            raise PinError(f"{path}: missing")
        data = normalized_text(path.read_bytes()) if normalize else path.read_bytes()
        if digest(data) != expected:
            raise PinError(f"{path}: sha256 {digest(data)} does not match the pin {expected}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-script", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--streamline", type=Path, required=True)
    parser.add_argument("--xess", type=Path, required=True)
    parser.add_argument("--imgui", type=Path, required=True)
    parser.add_argument("--vulkan12-git", type=Path, required=True)
    parser.add_argument("--vulkan14-include", type=Path, required=True)
    parser.add_argument("--copies", type=Path, required=True)
    args = parser.parse_args()
    sources = {"streamline": DirectorySource(args.streamline), "xess": DirectorySource(args.xess),
               "imgui": DirectorySource(args.imgui),
               "vulkan12": GitSource(args.vulkan12_git, VULKAN12_TAG, "include/"),
               "vulkan14": DirectorySource(args.vulkan14_include)}
    try:
        pins = read_pins(args.build_script)
        check_native_sources(args.native, pins["sourceDirectory"])
        for build_name, key in ROOT_NAMES.items():
            pinned = pins[build_name] + [(relative, expected, False)
                                         for relative, expected in SUPPLEMENTARY_PINS.get(key, [])]
            root = resolve(sources[key], pinned, args.copies / COPY_FOLDERS[key])
            print(f"{key}={root}")
            print(f"{key}: {len(pinned)} pinned files verified", file=sys.stderr)
    except PinError as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
