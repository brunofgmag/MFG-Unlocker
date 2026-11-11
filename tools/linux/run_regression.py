import runpy
import shutil
import subprocess
import sys
from pathlib import Path

WINE = str(Path(__file__).resolve().parent / "wine.sh")
RUNNER = Path(__file__).resolve().parents[2] / "tests" / "release_regression" / "run.py"
copy_with_metadata = shutil.copy2


class WinePopen(subprocess.Popen):
    def __init__(self, command, *args, **kwargs):
        super().__init__([WINE, *command], *args, **kwargs)


def copy_if_built(source, destination, **options):
    if Path(source).is_file():
        return copy_with_metadata(source, destination, **options)
    print(f"not built, not copied: {source}", file=sys.stderr)
    return destination


subprocess.CREATE_NO_WINDOW = 0
subprocess.Popen = WinePopen
shutil.copy2 = copy_if_built
sys.argv = [str(RUNNER), *sys.argv[1:]]
runpy.run_path(str(RUNNER), run_name="__main__")
