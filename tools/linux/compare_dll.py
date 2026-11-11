import argparse
import hashlib
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pe_image import PE, resource_type_name


def resource_rows(pe):
    return {(resource_type_name(kind), identifier, language): (len(data), hashlib.sha256(data).hexdigest().upper())
            for kind, identifier, language, data in pe.resources()}


def report_sets(title, left, right, label):
    only_left = sorted(left - right, key=str)
    only_right = sorted(right - left, key=str)
    print(f"{title}: {len(left)} in {label[0]}, {len(right)} in {label[1]}, {len(left & right)} in both")
    for item in only_left:
        print(f"  only in {label[0]}: {item}")
    for item in only_right:
        print(f"  only in {label[1]}: {item}")
    return not only_left and not only_right


def compare_exports(release, built, label):
    release_name, release_exports = release.exports()
    built_name, built_exports = built.exports()
    print(f"export directory name: {label[0]} {release_name!r}, {label[1]} {built_name!r}")
    same = report_sets("exports (ordinal, name)", set(release_exports), set(built_exports), label)
    return same and release_name == built_name


def compare_imports(title, release, built, label):
    same = True
    print(f"{title}: {len(release)} libraries in {label[0]}, {len(built)} in {label[1]}")
    for dll in sorted(set(release) | set(built)):
        left, right = set(release.get(dll, [])), set(built.get(dll, []))
        if left != right:
            same = False
            print(f"  {dll}: only in {label[0]} {sorted(left - right)}, only in {label[1]} {sorted(right - left)}")
    return same


def describe_resource_difference(key, left, right, label):
    if key not in right:
        return f"only in {label[0]}: {key} {left[key][0]} bytes"
    if key not in left:
        return f"only in {label[1]}: {key} {right[key][0]} bytes"
    if left[key] != right[key]:
        return (f"differs: {key} {label[0]} {left[key][0]} bytes {left[key][1][:16]}, "
                f"{label[1]} {right[key][0]} bytes {right[key][1][:16]}")
    return None


def compare_resources(release, built, label):
    left, right = resource_rows(release), resource_rows(built)
    print(f"resources (type, id, language): {len(left)} in {label[0]}, {len(right)} in {label[1]}")
    differences = [describe_resource_difference(key, left, right, label)
                   for key in sorted(set(left) | set(right), key=str)]
    differences = [line for line in differences if line]
    for line in differences:
        print(f"  {line}")
    return not differences


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("release", type=Path)
    parser.add_argument("built", type=Path)
    args = parser.parse_args()
    label = ("release", "built")
    release, built = PE(args.release.read_bytes()), PE(args.built.read_bytes())
    print(f"size: {label[0]} {args.release.stat().st_size} bytes, {label[1]} {args.built.stat().st_size} bytes")
    results = [compare_exports(release, built, label),
               compare_imports("imports", release.imports(), built.imports(), label),
               compare_imports("delay-load imports", release.delay_imports(), built.delay_imports(), label),
               compare_resources(release, built, label)]
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
