#!/usr/bin/env python3
"""Check worker-affinity plugin execution paths for GUI/thread-unsafe APIs."""

from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
METADATA_ROOT = ROOT / "src" / "plugins"
RULES_PATH = ROOT / "scripts" / "affinity-rules.json"
TOKEN_RE = re.compile(
    r"//[^\n]*|/\*[\s\S]*?\*/|R\"([^ ()\\\t\r\n]*)\([\s\S]*?\)\1\"|"
    r"\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|[A-Za-z_]\w*|::|->|"
    r"\+\+|--|\+=|-=|\*=|/=|%=|&=|\|=|\^=|==|!=|<=|>=|&&|\|\||\S"
)
CONTROL_NAMES = {"if", "for", "while", "switch", "catch", "sizeof", "decltype", "alignof"}
MUTATION_OPERATORS = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "++", "--"}


@dataclass(frozen=True)
class Token:
    value: str
    line: int


@dataclass(frozen=True)
class Function:
    name: str
    owner: str
    path: str
    tokens: tuple[Token, ...]
    line: int


def tokenize(source: str) -> list[Token]:
    tokens = []
    for match in TOKEN_RE.finditer(source):
        value = match.group(0)
        if value.startswith("//") or value.startswith("/*") or value.startswith('"') or value.startswith("'"):
            continue
        tokens.append(Token(value, source.count("\n", 0, match.start()) + 1))
    return tokens


def matching(tokens: list[Token], opening: str, closing: str) -> dict[int, int]:
    stack = []
    pairs = {}
    for index, token in enumerate(tokens):
        if token.value == opening:
            stack.append(index)
        elif token.value == closing and stack:
            start = stack.pop()
            pairs[start] = index
            pairs[index] = start
    return pairs


def extract_functions(path: str, tokens: list[Token]) -> list[Function]:
    parens = matching(tokens, "(", ")")
    braces = matching(tokens, "{", "}")
    functions = []
    for body_start, token in enumerate(tokens):
        if token.value != "{" or body_start not in braces:
            continue
        header_end = body_start - 1
        while header_end >= 0 and tokens[header_end].value in {"const", "noexcept", "override", "final", "&", "&&"}:
            header_end -= 1
        if header_end < 0 or tokens[header_end].value != ")" or header_end not in parens:
            continue
        name_index = parens[header_end] - 1
        if name_index < 0:
            continue
        name = tokens[name_index].value
        if not re.fullmatch(r"[A-Za-z_]\w*", name) or name in CONTROL_NAMES:
            continue
        owner = tokens[name_index - 2].value if name_index >= 2 and tokens[name_index - 1].value == "::" else ""
        body_end = braces[body_start]
        functions.append(
            Function(name, owner, path, tuple(tokens[body_start + 1 : body_end]), tokens[name_index].line)
        )
    return functions


def sequences(tokens: tuple[Token, ...] | list[Token], pattern: list[str]):
    values = [token.value for token in tokens]
    width = len(pattern)
    for start in range(len(values) - width + 1):
        if values[start : start + width] == pattern:
            yield start


def called_names(tokens: tuple[Token, ...]) -> set[str]:
    return {
        tokens[index].value
        for index in range(len(tokens) - 1)
        if re.fullmatch(r"[A-Za-z_]\w*", tokens[index].value) and tokens[index + 1].value == "("
    }


def global_variables(tokens: list[Token], functions: list[Function]) -> set[str]:
    function_ranges = []
    braces = matching(tokens, "{", "}")
    parens = matching(tokens, "(", ")")
    for body_start, token in enumerate(tokens):
        if token.value != "{" or body_start not in braces:
            continue
        end = body_start - 1
        while end >= 0 and tokens[end].value in {"const", "noexcept", "override", "final", "&", "&&"}:
            end -= 1
        if end >= 0 and tokens[end].value == ")" and end in parens:
            name_index = parens[end] - 1
            if name_index >= 0 and tokens[name_index].value not in CONTROL_NAMES:
                function_ranges.append((name_index, braces[body_start]))

    excluded = set()
    for start, end in function_ranges:
        excluded.update(range(start, end + 1))

    names = set()
    scopes = []
    statement = []
    for index, token in enumerate(tokens):
        if index in excluded:
            continue

        if token.value in {"{", "}"}:
            if token.value == "{":
                start = index - 1
                while start >= 0 and tokens[start].value not in {";", "{", "}"}:
                    start -= 1
                prefix = {item.value for item in tokens[start + 1 : index]}
                if "namespace" in prefix:
                    scopes.append("namespace")
                    statement = []
                elif prefix & {"class", "struct", "union", "enum"}:
                    scopes.append("class")
                    statement = []
                else:
                    scopes.append("block")
            else:
                scope = scopes.pop() if scopes else "block"
                if scope in {"namespace", "class"}:
                    statement = []
            continue

        if token.value != ";":
            if all(scope == "namespace" for scope in scopes):
                statement.append(index)
            continue

        if all(scope == "namespace" for scope in scopes):
            values = [tokens[item].value for item in statement]
            if values and not any(value in values for value in ("using", "typedef", "return")):
                if "=" not in values and "(" in values:
                    statement = []
                    continue
                segments = []
                segment = []
                depths = {"(": 0, "[": 0, "{": 0, "<": 0}
                closing = {")": "(", "]": "[", "}": "{", ">": "<"}
                for value in values:
                    if value == "," and not any(depths.values()):
                        segments.append(segment)
                        segment = []
                        continue
                    segment.append(value)
                    if value in depths:
                        depths[value] += 1
                    elif value in closing and depths[closing[value]]:
                        depths[closing[value]] -= 1
                segments.append(segment)
                for segment_index, part in enumerate(segments):
                    boundary = next((i for i, value in enumerate(part) if value in {"=", "{"}), len(part))
                    declaration = part[:boundary]
                    candidate = next(
                        (value for value in reversed(declaration) if re.fullmatch(r"[A-Za-z_]\w*", value)),
                        None,
                    )
                    if candidate and (len(declaration) >= 2 or segment_index > 0):
                        names.add(candidate)
            statement = []
    return names


def mutable_function_statics(tokens: tuple[Token, ...]) -> set[str]:
    names = set()
    for index, token in enumerate(tokens):
        if token.value != "static":
            continue
        end = next((i for i in range(index + 1, len(tokens)) if tokens[i].value == ";"), len(tokens))
        declaration = [item.value for item in tokens[index:end]]
        if "const" in declaration or "constexpr" in declaration:
            continue
        boundary = next((i for i, value in enumerate(declaration) if value in {"=", "{"}), len(declaration))
        candidate = next(
            (value for value in reversed(declaration[:boundary]) if re.fullmatch(r"[A-Za-z_]\w*", value)),
            None,
        )
        if candidate and len(declaration[:boundary]) >= 3:
            names.add(candidate)
    return names


def class_static_variables(tokens: list[Token]) -> set[tuple[str, str]]:
    braces = matching(tokens, "{", "}")
    parens = matching(tokens, "(", ")")
    excluded = set()
    for body_start, token in enumerate(tokens):
        if token.value != "{" or body_start not in braces:
            continue
        end = body_start - 1
        while end >= 0 and tokens[end].value in {"const", "noexcept", "override", "final", "&", "&&"}:
            end -= 1
        if end >= 0 and tokens[end].value == ")" and end in parens:
            name_index = parens[end] - 1
            if name_index >= 0 and tokens[name_index].value not in CONTROL_NAMES:
                excluded.update(range(body_start, braces[body_start] + 1))

    scopes: list[tuple[str, str]] = []
    result = set()
    for index, token in enumerate(tokens):
        if token.value == "{":
            start = index - 1
            while start >= 0 and tokens[start].value not in {";", "{", "}"}:
                start -= 1
            prefix = [item.value for item in tokens[start + 1 : index]]
            class_name = ""
            for keyword in ("class", "struct"):
                if keyword in prefix:
                    keyword_index = prefix.index(keyword)
                    if keyword_index + 1 < len(prefix):
                        class_name = prefix[keyword_index + 1]
                    break
            scopes.append(("class" if class_name else "block", class_name))
        elif token.value == "}" and scopes:
            scopes.pop()
        elif token.value == "static" and index not in excluded:
            enclosing = next((name for kind, name in reversed(scopes) if kind == "class"), "")
            if not enclosing:
                continue
            end = next((i for i in range(index + 1, len(tokens)) if tokens[i].value == ";"), len(tokens))
            declaration = [item.value for item in tokens[index + 1 : end]]
            if "(" in declaration:
                continue
            boundary = next((i for i, value in enumerate(declaration) if value in {"=", "{"}), len(declaration))
            member = next(
                (value for value in reversed(declaration[:boundary]) if re.fullmatch(r"[A-Za-z_]\w*", value)),
                "",
            )
            if member:
                result.add((enclosing, member))
    return result


def is_mutated_lvalue(tokens: tuple[Token, ...], index: int) -> bool:
    suffix = index + 1
    while suffix < len(tokens):
        if tokens[suffix].value in {".", "->"} and suffix + 1 < len(tokens):
            suffix += 2
        elif tokens[suffix].value == "[":
            depth = 1
            suffix += 1
            while suffix < len(tokens) and depth:
                if tokens[suffix].value == "[":
                    depth += 1
                elif tokens[suffix].value == "]":
                    depth -= 1
                suffix += 1
            if depth:
                return False
        else:
            break
    after = suffix < len(tokens) and tokens[suffix].value in MUTATION_OPERATORS
    before = index > 0 and tokens[index - 1].value in {"++", "--"}
    return after or before


def scope_paths(tokens: tuple[Token, ...]) -> list[tuple[int, ...]]:
    stack = []
    paths = []
    for index, token in enumerate(tokens):
        paths.append(tuple(stack))
        if token.value == "{":
            stack.append(index)
        elif token.value == "}" and stack:
            stack.pop()
    return paths


def local_declarations(tokens: tuple[Token, ...]) -> dict[str, list[int]]:
    declarations: dict[str, list[int]] = {}
    for index in range(1, len(tokens) - 1):
        name = tokens[index].value
        if not re.fullmatch(r"[A-Za-z_]\w*", name):
            continue
        previous = tokens[index - 1].value
        following = tokens[index + 1].value
        if following not in {"=", ";", ",", "["}:
            continue
        if re.fullmatch(r"[A-Za-z_]\w*", previous) or previous in {"*", "&", "&&"}:
            declarations.setdefault(name, []).append(index)
    return declarations


def singleton_aliases(tokens: tuple[Token, ...], singleton: str) -> dict[str, list[int]]:
    aliases: dict[str, list[int]] = {}
    marker = [singleton, "::", "instance", "(", ")"]
    for call in sequences(tokens, marker):
        if call + len(marker) >= len(tokens) or tokens[call + len(marker)].value != ";":
            continue
        start = call - 1
        while start >= 0 and tokens[start].value not in {";", "{", "}"}:
            start -= 1
        equals = next((i for i in range(start + 1, call) if tokens[i].value == "="), None)
        if equals is None:
            continue
        alias = next(
            (tokens[i].value for i in range(equals - 1, start, -1) if re.fullmatch(r"[A-Za-z_]\w*", tokens[i].value)),
            None,
        )
        if alias:
            aliases.setdefault(alias, []).append(call)
    return aliases


def singleton_calls(tokens: tuple[Token, ...], singleton: str):
    for index in sequences(tokens, [singleton, "::", "instance", "(", ")", "."]):
        method_index = index + 6
        if method_index + 1 < len(tokens) and tokens[method_index + 1].value == "(":
            yield index, tokens[method_index].value
    paths = scope_paths(tokens)
    declarations = local_declarations(tokens)
    for alias, alias_positions in singleton_aliases(tokens, singleton).items():
        for alias_position in alias_positions:
            for index, token in enumerate(tokens[:-2]):
                if token.value != alias or tokens[index + 1].value not in {".", "->"}:
                    continue
                if not paths[alias_position][:] == paths[index][: len(paths[alias_position])]:
                    continue
                shadowed = any(
                    declaration != alias_position
                    and alias_position < declaration < index
                    and paths[declaration][:] == paths[index][: len(paths[declaration])]
                    for declaration in declarations.get(alias, [])
                )
                if shadowed:
                    continue
                method_index = index + 2
                if method_index + 1 < len(tokens) and tokens[method_index + 1].value == "(":
                    yield index, tokens[method_index].value


def analyze_plugin(sources: dict[str, str], plugin_id: str, rules: dict) -> list[str]:
    tokenized = {path: tokenize(source) for path, source in sources.items()}
    functions = [function for path, tokens in tokenized.items() for function in extract_functions(path, tokens)]
    by_name: dict[str, list[Function]] = {}
    for function in functions:
        by_name.setdefault(function.name, []).append(function)

    reachable = set()
    pending = [name for name in ("process", "execute") if name in by_name]
    while pending:
        name = pending.pop()
        if name in reachable:
            continue
        reachable.add(name)
        for function in by_name[name]:
            pending.extend(called_names(function.tokens) & by_name.keys() - reachable)

    functions_by_path: dict[str, list[Function]] = {}
    for function in functions:
        functions_by_path.setdefault(function.path, []).append(function)
    globals_in_plugin = set().union(
        *(global_variables(tokens, functions_by_path.get(path, [])) for path, tokens in tokenized.items())
    )
    static_members = set().union(*(class_static_variables(tokens) for tokens in tokenized.values()))
    findings = []
    for name in sorted(reachable):
        for function in by_name[name]:
            body = function.tokens
            def report(line: int, message: str):
                findings.append(f"{function.path}:{line}: {plugin_id}::{name}: {message}")

            for token in body:
                if token.value in rules["forbidden_identifiers"]:
                    report(token.line, f"forbidden execution-path symbol {token.value}")

            for call in rules["forbidden_calls"]:
                for index in sequences(body, [call, "("]):
                    report(body[index].line, f"forbidden execution-path call {call}()")

            for pattern in rules["forbidden_sequences"]:
                for index in sequences(body, pattern["tokens"]):
                    report(body[index].line, f"forbidden execution-path sequence {' '.join(pattern['tokens'])}")

            for method in rules["global_manager_write_calls"]:
                pattern = ["GlobalVarManager", "::", "instance", "(", ")", ".", method, "("]
                for index in sequences(body, pattern):
                    report(body[index].line, f"GlobalVarManager write call {method}()")
                for index, called in singleton_calls(body, "GlobalVarManager"):
                    if called == method and body[index].value not in {"GlobalVarManager"}:
                        report(body[index].line, f"GlobalVarManager write call {method}()")

            for method in rules["run_engine_write_calls"]:
                pattern = ["RunEngine", "::", "instance", "(", ")", ".", method, "("]
                for index in sequences(body, pattern):
                    report(body[index].line, f"RunEngine write call {method}()")
                for index, called in singleton_calls(body, "RunEngine"):
                    if called == method and body[index].value not in {"RunEngine"}:
                        report(body[index].line, f"RunEngine write call {method}()")

            for index, method in singleton_calls(body, "RunEngine"):
                allowed = any(
                    item["plugin_id"] == plugin_id
                    and item["function"] == name
                    and item["method"] == method
                    for item in rules["run_engine_read_allowlist"]
                )
                if not allowed:
                    report(body[index].line, f"unregistered RunEngine singleton access ({method})")

            for global_name in globals_in_plugin:
                for index, token in enumerate(body):
                    if token.value == global_name and is_mutated_lvalue(body, index):
                        report(token.line, f"write to file-scope variable {global_name}")

            for class_name, member in static_members:
                for index, token in enumerate(body):
                    if token.value != member:
                        continue
                    qualified = index >= 2 and body[index - 1].value == "::" and body[index - 2].value == class_name
                    prefix_index = index - 3 if qualified else index - 1
                    prefix_increment = prefix_index >= 0 and body[prefix_index].value in {"++", "--"}
                    unqualified_static = function.owner == class_name and not local_declarations(body).get(member)
                    if (qualified and (prefix_increment or is_mutated_lvalue(body, index))) or (
                        unqualified_static and is_mutated_lvalue(body, index)
                    ):
                        report(token.line, f"write to static member {class_name}::{member}")

            for static_name in mutable_function_statics(body):
                for index, token in enumerate(body):
                    if token.value == static_name and is_mutated_lvalue(body, index):
                        report(token.line, f"write to function-static variable {static_name}")

    return findings


def analyze_source(path: str, source: str, plugin_id: str, rules: dict) -> list[str]:
    return analyze_plugin({path: source}, plugin_id, rules)


def main() -> int:
    try:
        rules = json.loads(RULES_PATH.read_text(encoding="utf-8"))
        for item in rules["run_engine_read_allowlist"]:
            evidence_path, test_name = item["evidence"].split("::", 1)
            evidence = ROOT / evidence_path
            if not evidence.is_file() or test_name not in evidence.read_text(encoding="utf-8"):
                raise ValueError(f"allowlist evidence not found: {item['evidence']}")
        worker_metadata = []
        for metadata_path in sorted(METADATA_ROOT.rglob("metadata.json")):
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            if metadata.get("execution", {}).get("affinity", "gui") == "worker":
                worker_metadata.append((metadata_path, metadata))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"affinity lint: unable to read metadata/rules: {error}", file=sys.stderr)
        return 2

    findings = []
    checked = 0
    for metadata_path, metadata in worker_metadata:
        plugin_dir = metadata_path.parent
        source_paths = sorted([*plugin_dir.glob("*.cpp"), *plugin_dir.glob("*.h")])
        if not source_paths:
            findings.append(f"{metadata_path.relative_to(ROOT)}: worker plugin has no C++ source files")
            continue
        checked += len(source_paths)
        sources = {
            path.relative_to(ROOT).as_posix(): path.read_text(encoding="utf-8", errors="replace")
            for path in source_paths
        }
        findings.extend(analyze_plugin(sources, metadata.get("id", plugin_dir.name), rules))

    if findings:
        print("Worker plugin affinity violations:")
        print("\n".join(findings))
        return 1

    print(f"Worker plugin affinity lint passed: {len(worker_metadata)} plugins, {checked} C++ files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
