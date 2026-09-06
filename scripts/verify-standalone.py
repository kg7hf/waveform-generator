#!/usr/bin/env python3
"""Audit resolved generator build inputs without executing builds or donor code.

Use --require-build-evidence for a gated post-build audit. Configure CMake File
API codemodel-v2 and cmakeFiles-v1 queries, export compile_commands.json, compile
with full header dependencies (-MD), and retain depfiles (Ninja: -d keepdepfile).
The audit does not itself prove an extraction build succeeded, forbid arbitrary
network activity, or prove runtime behavior. Run the documented clean extraction
build separately and retain its logs alongside this report. No Git is required.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import sys
import unittest
import uuid


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def words(command: str) -> list[str]:
    # Windows CMake commands contain backslashes that POSIX shlex would remove.
    tokens = shlex.split(command, posix=os.name != "nt")
    return [token[1:-1] if len(token) > 1 and token[0] == token[-1] == '"'
            else token for token in tokens]


def dependency_paths(contents: str) -> list[str]:
    """Read GCC make-style dependencies, retaining Windows drive colons."""
    contents = contents.replace("\\\r\n", "").replace("\\\n", "")
    result = []
    for rule in contents.splitlines():
        separator = re.search(r":(?:\s|$)", rule)
        if separator is None:
            if rule.strip():
                raise ValueError("malformed compiler dependency rule")
            continue
        rhs = rule[separator.end():]
        tokens = re.findall(r"(?:\\[ #\\]|[^\s])+", rhs)
        result.extend(re.sub(r"\\([ #\\])", r"\1", token).replace("$$", "$")
                      for token in tokens)
    return result


def latest_file_api_records(reply: Path) -> tuple[list[Path], list[Path]]:
    """Return cmakeFiles and target records referenced by the newest index."""
    reply = reply.resolve()

    def record(raw: str) -> Path:
        resolved = (reply / raw).resolve()
        if not within(resolved, reply):
            raise ValueError(f"CMake File API record escapes reply directory: {resolved}")
        return resolved

    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        return [], []
    index = json.loads(indexes[-1].read_text(encoding="utf-8-sig"))
    cmake_files: list[Path] = []
    targets: list[Path] = []
    for item in index.get("objects", []):
        filename = record(item["jsonFile"])
        if item.get("kind") == "cmakeFiles":
            cmake_files.append(filename)
        elif item.get("kind") == "codemodel":
            codemodel = json.loads(filename.read_text(encoding="utf-8-sig"))
            for configuration in codemodel.get("configurations", []):
                targets.extend(record(target["jsonFile"])
                               for target in configuration.get("targets", []))
    return cmake_files, targets


class Audit:
    def __init__(self, root: Path, toolchain_roots: list[Path] | None = None):
        self.root = root.resolve(strict=True)
        self.allowed = [path.resolve(strict=True) for path in toolchain_roots or []]
        if not self.root.is_dir():
            raise ValueError("generator root must be a directory")
        for allowed in self.allowed:
            if within(self.root, allowed) or allowed == Path(allowed.anchor):
                raise ValueError("toolchain exception cannot contain the generator or an entire drive")
        self.inputs: dict[tuple[str, str], dict] = {}
        self.violations: list[str] = []
        self.notes: list[str] = []
        self.coverage = {name: 0 for name in (
            "translation_units", "cmake_inputs", "target_records",
            "dependency_files", "dependency_headers", "link_commands", "map_inputs",
            "source_manifest_files")}
        self.required_depfiles: dict[Path, Path] = {}
        self.full_dependency_units = 0
        self.c_family_units = 0

    def path(self, raw: str | Path, base: Path, kind: str,
             *, system_allowed: bool = False, must_exist: bool = True) -> Path:
        raw = str(raw).strip('"')
        if not raw or "$<" in raw or "${" in raw:
            raise ValueError(f"unresolved {kind} path: {raw!r}")
        path = Path(raw)
        resolved = (path if path.is_absolute() else base / path).resolve()
        local = within(resolved, self.root)
        exception = system_allowed and any(within(resolved, item) for item in self.allowed)
        if not local and not exception:
            self.violations.append(f"{kind} escapes generator: {resolved}")
        if must_exist and not resolved.exists():
            self.violations.append(f"{kind} is missing: {resolved}")
        self.inputs[(kind, str(resolved))] = {
            "kind": kind, "path": str(resolved), "local": local,
            "declared_toolchain_exception": bool(exception),
        }
        return resolved

    def filesystem(self) -> None:
        # Never follow an escaping link while checking containment. Artifacts are
        # explicitly runtime data. Local build files are checked from evidence.
        for directory, folders, files in os.walk(self.root, followlinks=False):
            base = Path(directory)
            for name in list(folders):
                candidate = base / name
                if name in {".git", "artifacts", "build", ".venv", "__pycache__"}:
                    if name == ".git" and base != self.root:
                        self.violations.append(f"nested Git metadata: {candidate}")
                    folders.remove(name)
                    continue
                resolved = candidate.resolve()
                if not within(resolved, self.root):
                    self.violations.append(f"source directory escapes generator: {candidate} -> {resolved}")
                    folders.remove(name)
            for name in files:
                candidate = base / name
                if name == ".git":
                    self.violations.append(f"Git pointer file is not a portable source snapshot: {candidate}")
                if not within(candidate.resolve(), self.root):
                    self.violations.append(f"source file escapes generator: {candidate}")

    def command(self, tokens: list[str], base: Path, *, compiler: bool = False,
                responses: set[Path] | None = None) -> None:
        responses = set() if responses is None else responses
        index = 0
        if compiler and tokens:
            executable = tokens[0]
            if "/" in executable or "\\" in executable:
                self.path(executable, base, "compiler", system_allowed=True)
            index = 1
        while index < len(tokens):
            token = tokens[index]
            if token.startswith("@"):
                response = self.path(token[1:], base, "response_file")
                if response in responses:
                    raise ValueError(f"recursive response file: {response}")
                if response.is_file() and within(response, self.root):
                    self.command(words(response.read_text(encoding="utf-8")), base,
                                 responses=responses | {response})
            elif token.startswith("-Wl,"):
                self.command(token[4:].split(","), base, responses=responses)
            elif token in {"-I", "-isystem", "-iquote", "-idirafter", "-include",
                           "-imacros", "-L", "-T", "--script", "--sysroot", "-isysroot"}:
                index += 1
                if index == len(tokens):
                    raise ValueError(f"missing argument to {token}")
                is_script = token in {"-T", "--script"}
                self.path(tokens[index], base, "linker_script" if is_script else "compiler_path",
                          system_allowed=not is_script)
            elif token.startswith(("--script=", "--sysroot=")):
                option, argument = token.split("=", 1)
                self.path(argument, base, "linker_script" if option == "--script" else "compiler_path",
                          system_allowed=option != "--script")
            elif any(token.startswith(prefix) and len(token) > len(prefix)
                     for prefix in ("-isystem", "-iquote", "-idirafter", "-include", "-imacros", "-I", "-L", "-T")):
                prefix = next(prefix for prefix in
                              ("-isystem", "-iquote", "-idirafter", "-include", "-imacros", "-I", "-L", "-T")
                              if token.startswith(prefix))
                self.path(token[len(prefix):], base,
                          "linker_script" if prefix == "-T" else "compiler_path",
                          system_allowed=prefix != "-T")
            elif not token.startswith("-") and Path(token).suffix.lower() in {
                    ".c", ".cc", ".cpp", ".cxx", ".s", ".h", ".hpp", ".ld", ".a", ".lib", ".o", ".obj"}:
                self.path(token, base, "command_input", system_allowed=Path(token).suffix.lower() in {".a", ".lib"})
            index += 1

    def build(self, build_dir: Path, dependency_files: list[Path] | None = None) -> None:
        build = self.path(build_dir, self.root, "build_directory")
        self.source_manifest(build / "source-manifest.json")
        commands = build / "compile_commands.json"
        if commands.is_file():
            for item in json.loads(commands.read_text(encoding="utf-8-sig")):
                cwd = self.path(item["directory"], build, "compile_directory")
                self.path(item["file"], cwd, "translation_unit")
                self.coverage["translation_units"] += 1
                tokens = item.get("arguments") or words(item["command"])
                self.command(tokens, cwd, compiler=True)
                if Path(item["file"]).suffix.lower() in {".c", ".cc", ".cpp", ".cxx"}:
                    self.c_family_units += 1
                    dependency_flags = [token for token in tokens if token in {"-MD", "-MMD"}]
                    if dependency_flags and dependency_flags[-1] == "-MD":
                        self.full_dependency_units += 1
                    # CMake's exported command may omit its appended -MD/-MF.
                    # The generated Make rules and retained depfile verify them.
                    if "-MF" in tokens:
                        depfile = cwd / tokens[tokens.index("-MF") + 1]
                    elif item.get("output"):
                        depfile = build / (item["output"] + ".d")
                    elif "-o" in tokens:
                        depfile = cwd / (tokens[tokens.index("-o") + 1] + ".d")
                    else:
                        raise ValueError(f"cannot identify compiler dependency file: {item['file']}")
                    self.required_depfiles[depfile.resolve()] = cwd
        make_full_dependency_rules = 0
        for filename in build.rglob("build.make"):
            for line in filename.read_text(encoding="utf-8-sig").splitlines():
                if " -c " in line and " -MF " in line:
                    flags = re.findall(r"(?<!\S)-M(?:M)?D(?=\s|$)", line)
                    if flags and flags[-1] == "-MD":
                        make_full_dependency_rules += 1
        if self.c_family_units and max(self.full_dependency_units, make_full_dependency_rules) < self.c_family_units:
            self.notes.append("Full system-header dependency generation (-MD) not established for every C/C++ translation unit")
        reply = build / ".cmake/api/v1/reply"
        cmake_records, target_records = latest_file_api_records(reply)
        for filename in cmake_records:
            filename = self.path(filename, reply, "cmake_file_api_reply")
            data = json.loads(filename.read_text(encoding="utf-8-sig"))
            source = Path(data.get("paths", {}).get("source", str(self.root)))
            self.path(source, build, "cmake_source_root")
            for item in data.get("inputs", []):
                self.path(item["path"], source,
                          "cmake_input", system_allowed=bool(item.get("isCMake") or item.get("isExternal")))
                self.coverage["cmake_inputs"] += 1
        for filename in target_records:
            filename = self.path(filename, reply, "cmake_file_api_reply")
            data = json.loads(filename.read_text(encoding="utf-8-sig"))
            self.coverage["target_records"] += 1
            for item in data.get("sources", []):
                self.path(item["path"], self.root, "target_source")
            for group in data.get("compileGroups", []):
                for item in group.get("includes", []):
                    self.path(item["path"], self.root, "target_include", system_allowed=True)
            link = data.get("link", {})
            if link.get("commandFragments"):
                fragments = " ".join(item["fragment"] for item in link["commandFragments"])
                target_build = build / data.get("paths", {}).get("build", ".")
                self.command(words(fragments), target_build)
                self.coverage["link_commands"] += 1
        # GNU Make's link.txt is an actual command, including its response files.
        # Its working directory is the target's binary directory above CMakeFiles.
        for filename in build.rglob("link.txt"):
            target_build = next((parent.parent for parent in filename.parents if parent.name == "CMakeFiles"), build)
            self.command(words(filename.read_text(encoding="utf-8-sig")), target_build, compiler=True)
            self.coverage["link_commands"] += 1
        for filename in build.rglob("*.map"):
            self.path(filename, build, "link_map")
            for line in filename.read_text(encoding="utf-8-sig").splitlines():
                if line.startswith("LOAD "):
                    raw = line[5:].strip()
                    if raw == "linker stubs":
                        continue  # GNU ld's synthetic veneers, not an input file.
                    self.path(raw, filename.parent, "linked_input", system_allowed=True)
                    self.coverage["map_inputs"] += 1
        depfiles = set(build.rglob("*.d")) | set(dependency_files or []) | set(self.required_depfiles)
        for filename in sorted(depfiles):
            filename = self.path(filename, build, "dependency_file")
            if not filename.is_file() or not within(filename, self.root):
                continue
            self.coverage["dependency_files"] += 1
            for item in dependency_paths(filename.read_text(encoding="utf-8-sig")):
                self.path(item, self.required_depfiles.get(filename, build), "header_dependency", system_allowed=True)
                if Path(item).suffix.lower() in {".h", ".hpp", ".hxx", ".inc"}:
                    self.coverage["dependency_headers"] += 1

    def source_manifest(self, filename: Path) -> None:
        self.path(filename, self.root, "source_manifest")
        if not filename.is_file():
            return
        data = json.loads(filename.read_text(encoding="utf-8-sig"))
        if data.get("schema_version") != "wfg-source-manifest/1":
            self.violations.append("unsupported source manifest schema")
            return
        expected: set[str] = set()
        for item in data["files"]:
            path = Path(item["path"])
            if path.is_absolute() or ".." in path.parts:
                self.violations.append(f"source manifest path is not a contained relative path: {path}")
                continue
            relative = path.as_posix()
            if relative in expected:
                self.violations.append(f"duplicate source manifest entry: {relative}")
            expected.add(relative)
            self.check_hash(item, self.root, relative)
            self.coverage["source_manifest_files"] += 1
        actual: set[str] = set()
        root_exclusions = {"build", "artifacts", "third_party", ".venv", ".git", "__pycache__"}
        for directory, folders, files in os.walk(self.root, followlinks=False):
            base = Path(directory)
            folders[:] = [name for name in folders
                          if name != "__pycache__" and not (base == self.root and name in root_exclusions)
                          and within((base / name).resolve(), self.root)]
            for name in files:
                if base == self.root and name in root_exclusions:
                    continue
                actual.add((base / name).relative_to(self.root).as_posix())
        for extra in sorted(actual - expected):
            self.violations.append(f"first-party file absent from source manifest: {extra}")
        for missing in sorted(expected - actual):
            self.violations.append(f"source manifest entry outside current first-party file set: {missing}")

    def provenance(self) -> None:
        lock_path = self.root / "dependencies.lock.json"
        imports_path = self.root / "docs/source-imports.json"
        for filename in (lock_path, imports_path):
            if not filename.is_file():
                self.violations.append(f"missing provenance inventory: {filename}")
        if lock_path.is_file():
            lock = json.loads(lock_path.read_text(encoding="utf-8-sig"))
            for dependency in lock["dependencies"]:
                status = dependency["status"]
                if status not in {"planned", "materialized", "unresolved"}:
                    self.violations.append(f"unknown dependency status: {dependency['name']}: {status}")
                destination = self.path(dependency["destination"], self.root, "dependency_destination",
                                        must_exist=status == "materialized")
                if status != "materialized":
                    self.notes.append(f"Dependency {dependency['name']}: {status}")
                    continue
                if not re.fullmatch(r"[0-9a-f]{40}", dependency.get("commit") or "") or not dependency.get("files"):
                    self.violations.append(f"materialized dependency lacks exact commit/file inventory: {dependency['name']}")
                for item in dependency.get("files", []):
                    self.check_hash(item, destination, item["path"])
                expected = {(destination / item["path"]).resolve() for item in dependency.get("files", [])}
                if within(destination, self.root) and destination.is_dir():
                    actual = {path.resolve() for path in destination.rglob("*") if path.is_file()}
                    for extra in actual - expected:
                        self.violations.append(f"unlisted materialized dependency file: {extra}")
        if imports_path.is_file():
            inventory = json.loads(imports_path.read_text(encoding="utf-8-sig"))
            for item in inventory["imports"]:
                status = item["status"]
                if status not in {"planned", "materialized", "unresolved"}:
                    self.violations.append(f"unknown import status: {item['id']}: {status}")
                if status == "materialized":
                    self.check_hash({"sha256": item.get("local_sha256"), "size_bytes": item.get("local_size_bytes")},
                                    self.root, item["destination"])
                else:
                    self.notes.append(f"Import {item['id']}: {status}")

    def check_hash(self, item: dict, base: Path, raw: str) -> None:
        path = self.path(raw, base, "provenance_file")
        if not within(path, base):
            self.violations.append(f"provenance file escapes its component: {path}")
        if path.is_file() and within(path, self.root):
            if path.stat().st_size != item.get("size_bytes") or sha256(path) != item.get("sha256"):
                self.violations.append(f"provenance hash/size mismatch: {path}")

    def result(self, require_build: bool) -> dict:
        missing = [key for key, count in self.coverage.items() if not count]
        if any(note.startswith("Full system-header") for note in self.notes):
            missing.append("full_system_header_dependency_generation")
        if require_build and missing:
            self.violations.append("incomplete build-input coverage: " + ", ".join(missing))
        inputs = sorted(self.inputs.values(), key=lambda item: (item["kind"], item["path"]))
        inventory_hash = hashlib.sha256(json.dumps(inputs, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        return {"schema_version": 1, "root": str(self.root),
                "toolchain_exceptions": [str(item) for item in self.allowed],
                "status": "fail" if self.violations else ("pass" if not missing else "incomplete"),
                "scope": "Resolved build-input containment only; not extraction-build or hardware qualification.",
                "separate_checks_required": ["Clean extraction build and test logs", "Runtime Python import resolution", "Custom-command behavior and network isolation"],
                "git_metadata_required": False, "coverage": self.coverage, "missing_coverage": missing,
                "violations": sorted(set(self.violations)), "notes": sorted(set(self.notes)),
                "resolved_inputs_sha256": inventory_hash, "resolved_inputs": inputs}


class BoundaryTests(unittest.TestCase):
    def setUp(self):
        test_parent = Path(__file__).resolve().parents[1] / "build/boundary-self-tests"
        test_parent.mkdir(parents=True, exist_ok=True)
        self.test_parent = test_parent.resolve()
        # tempfile's owner-only ACL conflicts with some managed Windows tokens.
        # A unique directory with inherited workspace permissions is sufficient.
        self.base = test_parent / ("case-" + uuid.uuid4().hex)
        self.base.mkdir()
        self.root = self.base / "generator"
        self.root.mkdir()
        (self.root / "main.cpp").write_text('#include "local.h"\n', encoding="utf-8")
        (self.root / "local.h").write_text("// local\n", encoding="utf-8")
        (self.base / "outside.h").write_text("// forbidden\n", encoding="utf-8")

    def tearDown(self):
        resolved = self.base.resolve()
        if resolved.parent != self.test_parent or not resolved.name.startswith("case-"):
            raise ValueError("refusing cleanup outside this test's private directory")
        shutil.rmtree(resolved)

    def test_external_forced_header_rejected(self):
        audit = Audit(self.root)
        audit.command(["-include", str(self.base / "outside.h")], self.root)
        self.assertTrue(any("escapes generator" in item for item in audit.violations))

    def test_external_linker_script_in_response_rejected(self):
        script = self.base / "outside.ld"
        script.write_text("SECTIONS {}", encoding="utf-8")
        response = self.root / "link.rsp"
        response.write_text('-Wl,-T,"' + script.as_posix() + '"', encoding="utf-8")
        audit = Audit(self.root)
        audit.command(["@link.rsp"], self.root)
        self.assertTrue(any("linker_script escapes" in item for item in audit.violations))

    def test_empty_build_cannot_pass(self):
        audit = Audit(self.root)
        self.assertEqual(audit.result(True)["status"], "fail")

    def test_local_inputs_and_explicit_toolchain(self):
        sdk = self.base / "compiler"
        sdk.mkdir()
        audit = Audit(self.root, [sdk])
        audit.command(["-I.", "-isystem", str(sdk), "main.cpp"], self.root)
        self.assertEqual(audit.violations, [])

    def test_source_escape_even_with_toolchain_exception(self):
        sdk = self.base / "compiler"
        sdk.mkdir()
        source = sdk / "forbidden.c"
        source.write_text("int x;", encoding="utf-8")
        audit = Audit(self.root, [sdk])
        audit.path(source, self.root, "translation_unit")
        self.assertTrue(audit.violations)

    def test_dependency_spaces_and_drive_colons(self):
        self.assertEqual(dependency_paths("C:/build/main.o: C:/src/main.cpp C:/a\\ b/local.h\n"),
                         ["C:/src/main.cpp", "C:/a b/local.h"])

    def test_hash_mismatch_rejected(self):
        audit = Audit(self.root)
        audit.check_hash({"sha256": "0" * 64, "size_bytes": 0}, self.root, "local.h")
        self.assertTrue(any("hash/size mismatch" in item for item in audit.violations))

    def test_source_manifest_must_cover_new_files(self):
        build = self.root / "build"
        build.mkdir()
        source = self.root / "main.cpp"
        manifest = build / "source-manifest.json"
        manifest.write_text(json.dumps({"schema_version": "wfg-source-manifest/1", "files": [
            {"path": "main.cpp", "size_bytes": source.stat().st_size, "sha256": sha256(source)}
        ]}), encoding="utf-8")
        audit = Audit(self.root)
        audit.source_manifest(manifest)
        self.assertIn("first-party file absent from source manifest: local.h", audit.violations)

    def test_transitive_outside_header_rejected(self):
        build = self.root / "build"
        build.mkdir()
        depfile = build / "main.o.d"
        depfile.write_text("main.o: " + (self.base / "outside.h").as_posix(), encoding="utf-8")
        audit = Audit(self.root)
        for path in dependency_paths(depfile.read_text(encoding="utf-8")):
            audit.path(path, build, "header_dependency", system_allowed=True)
        self.assertTrue(any("header_dependency escapes generator" in item for item in audit.violations))

    def test_file_api_uses_newest_index_only(self):
        reply = self.root / "build/.cmake/api/v1/reply"
        reply.mkdir(parents=True)
        stale_target = reply / "target-stale.json"
        fresh_target = reply / "target-fresh.json"
        stale_target.write_text("{}", encoding="utf-8")
        fresh_target.write_text("{}", encoding="utf-8")
        for label, target in (("old", stale_target), ("new", fresh_target)):
            codemodel = reply / f"codemodel-v2-{label}.json"
            codemodel.write_text(json.dumps({"configurations": [{"targets": [
                {"jsonFile": target.name}
            ]}]}), encoding="utf-8")
            cmake_files = reply / f"cmakeFiles-v1-{label}.json"
            cmake_files.write_text("{}", encoding="utf-8")
            index = reply / ("index-2026-01-01T00-00-00.json" if label == "old"
                             else "index-2026-01-02T00-00-00.json")
            index.write_text(json.dumps({"objects": [
                {"kind": "codemodel", "jsonFile": codemodel.name},
                {"kind": "cmakeFiles", "jsonFile": cmake_files.name}
            ]}), encoding="utf-8")
        cmake_records, target_records = latest_file_api_records(reply)
        self.assertEqual(cmake_records, [reply / "cmakeFiles-v1-new.json"])
        self.assertEqual(target_records, [fresh_target])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--toolchain-root", type=Path, action="append", default=[])
    parser.add_argument("--dependency-file", type=Path, action="append", default=[])
    parser.add_argument("--input", type=Path, action="append", default=[], help="Additional generator-owned custom build input")
    parser.add_argument("--check-provenance", action="store_true")
    parser.add_argument("--require-build-evidence", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    if arguments.self_test:
        result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(BoundaryTests))
        return 0 if result.wasSuccessful() else 1
    try:
        audit = Audit(arguments.root, arguments.toolchain_root)
        audit.filesystem()
        if arguments.build_dir:
            audit.build(arguments.build_dir, arguments.dependency_file)
        for item in arguments.input:
            audit.path(item, audit.root, "explicit_build_input")
        if arguments.check_provenance:
            audit.provenance()
        report = audit.result(arguments.require_build_evidence)
        encoded = json.dumps(report, indent=2) + "\n"
        if arguments.output:
            output = arguments.output.resolve()
            if not within(output, audit.root):
                raise ValueError("audit output must remain within the generator")
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(encoded, encoding="utf-8")
        print(encoded, end="")
        return 1 if report["violations"] else 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Boundary audit could not complete: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
