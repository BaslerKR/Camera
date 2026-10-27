"""Configure Camera against synthetic SDK layouts; no SDK or compiler is used.

Run with --camera-source path/to/Camera/C++ --work-dir path/to/build/tests.
These checks exercise CMake discovery, not native SDK compilation or runtime.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--camera-source", type=Path,
                        default=Path(__file__).resolve().parents[1])
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    # Any single-configuration generator can configure the synthetic layouts.
    # Prefer Ninja; Unix hosts without it use the make tool they already have.
    ninja = shutil.which("ninja")
    make = None if ninja or os.name == "nt" else shutil.which("make")
    if not ninja and not make:
        parser.error("Ninja (or make on Linux/macOS) must be available on PATH")
    generator, make_program = ("Ninja", ninja) if ninja else ("Unix Makefiles", make)
    args.work_dir.mkdir(parents=True, exist_ok=True)
    camera = args.camera_source.resolve().as_posix()
    cases = [
        ("linux-default", "Linux", "config", "default", True),
        ("linux-root", "Linux", "config", "root", True),
        ("linux-dir", "Linux", "config", "dir", True),
        ("linux-prefix", "Linux", "config", "prefix", True),
        ("linux-config-first", "Linux", "both", "root", True),
        ("linux-find-module", "Linux", "module", "root", True),
        ("linux-missing", "Linux", "missing", "root", False),
        ("windows-find-module", "Windows", "module", "root", True),
        ("windows-pylon12", "Windows", "module", "root", True),
        ("windows-pylon12-overrides", "Windows", "module", "root", True),
        ("windows-explicit-finder", "Windows", "module", "root", True),
        ("macos-framework", "Darwin", "framework", "root", True),
        ("preloaded-target", "Linux", "preloaded", "root", True),
    ]
    for name, system, layout, hint, success in cases:
        with tempfile.TemporaryDirectory(prefix=name + "-", dir=args.work_dir) as tmp:
            root = Path(tmp)
            sdk = root / "sysroot/opt/pylon" if hint == "default" else root / "sdk with spaces"
            sdk.mkdir(parents=True)
            expected = "config" if layout in ("config", "both") else layout
            package = lambda marker: (
                "add_library(pylon::pylon INTERFACE IMPORTED)\n"
                f"set_property(TARGET pylon::pylon PROPERTY INTERFACE_COMPILE_DEFINITIONS SDK_{marker})\n"
                "set(pylon_FOUND TRUE)\nset(pylon_VERSION 8.0.0)\n")
            if layout in ("config", "both"):
                config = sdk / "share/pylon/cmake"
                config.mkdir(parents=True)
                (config / "pylon-config.cmake").write_text(package("config"))
            if layout in ("module", "both"):
                module = sdk / "lib/cmake"
                module.mkdir(parents=True)
                (module / "Findpylon.cmake").write_text(package("module"))
            if layout == "framework":
                framework = sdk / "Resources/CMake"
                framework.mkdir(parents=True)
                (framework / "pylon-targets.cmake").write_text(package("framework"))
                (framework / "genicam-targets.cmake").write_text("")
                (framework / "pylon-config-version.cmake").write_text("set(PACKAGE_VERSION 8.0.0)\n")
            checks = ""
            if name.startswith("windows-pylon12"):
                libraries = sdk / "lib/x64"
                libraries.mkdir(parents=True)
                (libraries / "PylonBase_v12.lib").touch()
                expected_base = (libraries / "PylonBase_v12.lib").as_posix()
                expected_genapi = (libraries / "GenApi_MD_VC142_v3_5_Basler_pylon_v1.lib").as_posix()
                if name.endswith("overrides"):
                    expected_base = (root / "explicit-base.lib").as_posix()
                    expected_genapi = (root / "explicit-genapi.lib").as_posix()
                checks = (f'if(NOT pylonBase STREQUAL "{expected_base}" OR NOT GenApi STREQUAL "{expected_genapi}")\n'
                          'message(FATAL_ERROR "Wrong Windows pylon library defaults or overrides")\nendif()\n')
            if name == "windows-explicit-finder":
                installed = root / "program-files/Basler/pylon/Development"
                (installed / "lib/cmake").mkdir(parents=True)
                (installed / "lib/cmake/Findpylon.cmake").write_text(package("wrong"))
                (installed / "lib/x64").mkdir()
                (installed / "lib/x64/PylonBase_v12.lib").touch()
                checks = ('if(DEFINED pylonBase)\n'
                          'message(FATAL_ERROR "Unselected default SDK overwrote library paths")\nendif()\n')
            source = root / "source"
            source.mkdir()
            preload = package("preloaded") if layout == "preloaded" else ""
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.16)\nproject(Discovery NONE)\n"
                + preload
                + f'add_subdirectory("{camera}" camera)\n'
                + "get_target_property(marker CameraPylonRuntime INTERFACE_COMPILE_DEFINITIONS)\n"
                + f'if(NOT marker STREQUAL "SDK_{expected}")\n'
                + 'message(FATAL_ERROR "Wrong SDK selected: ${marker}")\nendif()\n'
                + checks)
            # Configure/generate only: bypass compiler probes in cross-platform fixtures.
            toolchain = root / "toolchain.cmake"
            toolchain.write_text(
                f"set(CMAKE_SYSTEM_NAME {system})\n"
                'set(CMAKE_CXX_COMPILER "${CMAKE_COMMAND}")\n'
                "set(CMAKE_CXX_COMPILER_ID GNU)\n"
                "set(CMAKE_CXX_COMPILER_FORCED TRUE)\n"
                "set(CMAKE_CXX_COMPILER_WORKS TRUE)\n"
                'set(CMAKE_INSTALL_NAME_TOOL "${CMAKE_COMMAND}")\n')
            env = {k: v for k, v in os.environ.items()
                   if k.upper() not in ("PYLON_ROOT", "PYLON_DEV_DIR", "PYLON_DIR", "CMAKE_PREFIX_PATH", "CMAKE_MODULE_PATH", "PROGRAMFILES")}
            env["ProgramFiles"] = str(root / "unused-program-files")
            command = ["cmake", "-S", str(source), "-B", str(root / "build"),
                       "-G", generator, f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                       "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
                       "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
                       "-DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=OFF",
                       "-DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF"]
            # Supply the build tool explicitly because system-environment searching is disabled.
            command.append(f"-DCMAKE_MAKE_PROGRAM={make_program}")
            if name == "windows-pylon12-overrides":
                command += [f"-DpylonBase={expected_base}", f"-DGenApi={expected_genapi}"]
            if name == "windows-explicit-finder":
                env["ProgramFiles"] = str(root / "program-files")
                command.append(f"-DCMAKE_MODULE_PATH={sdk / 'lib/cmake'}")
            if hint != "default":
                command.extend(["-DCMAKE_IGNORE_PREFIX_PATH=/opt/pylon",
                                "-DCMAKE_IGNORE_PATH=/opt/pylon/lib/cmake"])
            if hint == "root":
                env["PYLON_ROOT"] = str(sdk)
            elif hint == "dir":
                command.append(f"-Dpylon_DIR={sdk / 'share/pylon/cmake'}")
            elif hint == "prefix":
                command.append(f"-DCMAKE_PREFIX_PATH={sdk}")
            else:
                command.extend([f"-DCMAKE_FIND_ROOT_PATH={root / 'sysroot'}",
                                "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY"])
            result = subprocess.run(command, env=env, capture_output=True, text=True)
            log = result.stdout + result.stderr
            (args.work_dir / f"{name}.log").write_text(log, encoding="utf-8")
            if (result.returncode == 0) != success:
                raise RuntimeError(f"{name}: unexpected configure result\n{log}")
            if not success and 'Could not find a package configuration file provided by "pylon"' not in log:
                raise RuntimeError(f"{name}: failure did not identify missing pylon\n{log}")
            print(f"PASS {name}")
    print(f"Passed {len(cases)}/{len(cases)} discovery cases (synthetic configure only).")


if __name__ == "__main__":
    main()
