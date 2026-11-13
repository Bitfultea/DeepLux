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
    namespace: tuple[str, ...]
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


def namespace_paths(tokens: list[Token]) -> list[tuple[str, ...]]:
    scopes: list[tuple[str, str]] = []
    paths = []
    for index, token in enumerate(tokens):
        paths.append(tuple(part for kind, name in scopes if kind == "namespace" for part in name.split("::")))
        if token.value == "{":
            start = index - 1
            while start >= 0 and tokens[start].value not in {";", "{", "}"}:
                start -= 1
            prefix = [item.value for item in tokens[start + 1 : index]]
            if "namespace" in prefix:
                namespace_index = prefix.index("namespace")
                cursor = namespace_index + 1
                names = []
                while cursor < len(prefix) and re.fullmatch(r"[A-Za-z_]\w*", prefix[cursor]):
                    names.append(prefix[cursor])
                    if cursor + 1 >= len(prefix) or prefix[cursor + 1] != "::":
                        break
                    cursor += 2
                if names:
                    scopes.append(("namespace", "::".join(names)))
                else:
                    scopes.append(("namespace", "<anonymous>"))
            else:
                scopes.append(("block", ""))
        elif token.value == "}" and scopes:
            scopes.pop()
    return paths


def function_body_specs(tokens: list[Token]) -> list[tuple[int, int, int]]:
    parens = matching(tokens, "(", ")")
    braces = matching(tokens, "{", "}")
    specs = []
    for body_start, token in enumerate(tokens):
        if token.value != "{" or body_start not in braces:
            continue
        header_end = body_start - 1
        while header_end >= 0 and tokens[header_end].value in {"const", "noexcept", "override", "final", "&", "&&"}:
            header_end -= 1
        if header_end >= 0 and tokens[header_end].value == ")" and header_end in parens:
            noexcept_open = parens[header_end]
            if noexcept_open > 0 and tokens[noexcept_open - 1].value == "noexcept":
                header_end = noexcept_open - 2
        if header_end < 0 or tokens[header_end].value != ")":
            arrow = next((i for i in range(body_start - 1, -1, -1) if tokens[i].value == "->"), None)
            if arrow is None:
                continue
            header_end = arrow - 1
            if header_end >= 0 and tokens[header_end].value == ")" and header_end in parens:
                noexcept_open = parens[header_end]
                if noexcept_open > 0 and tokens[noexcept_open - 1].value == "noexcept":
                    header_end = noexcept_open - 2
            elif header_end >= 0 and tokens[header_end].value == "noexcept":
                header_end -= 1
        if header_end < 0 or tokens[header_end].value != ")" or header_end not in parens:
            continue
        name_index = parens[header_end] - 1
        if name_index >= 0 and re.fullmatch(r"[A-Za-z_]\w*", tokens[name_index].value):
            if tokens[name_index].value not in CONTROL_NAMES:
                specs.append((body_start, braces[body_start], name_index))
    return specs


def namespace_visible(declaration: tuple[str, ...], use: tuple[str, ...]) -> bool:
    visible = tuple(part for part in declaration if part != "<anonymous>")
    current = tuple(part for part in use if part != "<anonymous>")
    return use[: len(declaration)] == declaration or current[: len(visible)] == visible


def extract_functions(path: str, tokens: list[Token]) -> list[Function]:
    namespaces = namespace_paths(tokens)
    functions = []
    for body_start, body_end, name_index in function_body_specs(tokens):
        name = tokens[name_index].value
        owner = tokens[name_index - 2].value if name_index >= 2 and tokens[name_index - 1].value == "::" else ""
        functions.append(
            Function(name, owner, path, namespaces[name_index], tuple(tokens[body_start + 1 : body_end]),
                     tokens[name_index].line)
        )
    return functions


def sequences(tokens: tuple[Token, ...] | list[Token], pattern: list[str]):
    values = [token.value for token in tokens]
    width = len(pattern)
    for start in range(len(values) - width + 1):
        if values[start : start + width] == pattern:
            yield start


def called_names(tokens: tuple[Token, ...], owner: str) -> set[tuple[str, str]]:
    calls = set()
    for index in range(len(tokens) - 1):
        if not re.fullmatch(r"[A-Za-z_]\w*", tokens[index].value) or tokens[index + 1].value != "(":
            continue
        previous = tokens[index - 1].value if index else ""
        if previous == "::" and index >= 2:
            calls.add((tokens[index].value, tokens[index - 2].value))
        elif previous == "->" and index >= 2 and tokens[index - 2].value == "this":
            calls.add((tokens[index].value, owner))
        elif previous not in {".", "->", "::"}:
            calls.add((tokens[index].value, ""))
    return calls


def global_variables(tokens: list[Token], functions: list[Function]) -> set[str]:
    function_ranges = []
    for body_start, body_end, _ in function_body_specs(tokens):
        function_ranges.append((body_start, body_end))

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
                if scope == "class" and all(item == "namespace" for item in scopes):
                    declaration_end = next((i for i in range(index + 1, len(tokens)) if tokens[i].value == ";"), index)
                    segments = []
                    segment = []
                    depth = 0
                    for item in tokens[index + 1 : declaration_end]:
                        if item.value == "," and depth == 0:
                            segments.append(segment)
                            segment = []
                        else:
                            segment.append(item.value)
                            if item.value in {"(", "[", "{"}:
                                depth += 1
                            elif item.value in {")", "]", "}"} and depth:
                                depth -= 1
                    segments.append(segment)
                    for part in segments:
                        boundary = next((i for i, value in enumerate(part) if value in {"=", "{", "("}), len(part))
                        candidate = next(
                            (value for value in reversed(part[:boundary]) if re.fullmatch(r"[A-Za-z_]\w*", value)),
                            None,
                        )
                        if candidate:
                            names.add(candidate)
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


def global_mutating_method_calls(tokens: tuple[Token, ...], globals_in_plugin: set[str], methods: set[str]):
    for index, token in enumerate(tokens):
        if token.value not in globals_in_plugin:
            continue
        if index > 0 and tokens[index - 1].value == "*" and index + 1 < len(tokens) and tokens[index + 1].value == ")":
            cursor = index + 2
            while cursor < len(tokens):
                if tokens[cursor].value == "[":
                    depth = 1
                    cursor += 1
                    while cursor < len(tokens) and depth:
                        if tokens[cursor].value == "[":
                            depth += 1
                        elif tokens[cursor].value == "]":
                            depth -= 1
                        cursor += 1
                    if depth:
                        break
                elif tokens[cursor].value in {".", "->"} and cursor + 1 < len(tokens):
                    method_index = cursor + 1
                    if tokens[method_index].value in methods and method_index + 1 < len(tokens) and tokens[
                        method_index + 1
                    ].value == "(":
                        yield method_index, tokens[method_index].value, token.value
                        break
                    cursor += 2
                else:
                    break
        cursor = index + 1
        while cursor < len(tokens):
            if tokens[cursor].value in {".", "->"} and cursor + 1 < len(tokens):
                method_index = cursor + 1
                if tokens[method_index].value in methods and method_index + 1 < len(tokens) and tokens[
                    method_index + 1
                ].value == "(":
                    yield method_index, tokens[method_index].value, token.value
                cursor += 2
            elif tokens[cursor].value == "[":
                depth = 1
                cursor += 1
                while cursor < len(tokens) and depth:
                    if tokens[cursor].value == "[":
                        depth += 1
                    elif tokens[cursor].value == "]":
                        depth -= 1
                    cursor += 1
                if depth:
                    break
            else:
                break


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
        if following not in {"=", ";", ",", "[", "{"}:
            continue
        pointer_or_reference_declaration = (
            previous in {"*", "&", "&&"}
            and index > 1
            and (re.fullmatch(r"[A-Za-z_]\w*", tokens[index - 2].value) or tokens[index - 2].value in {">", "]"})
        )
        if re.fullmatch(r"[A-Za-z_]\w*", previous) or previous in {">", "]"} or pointer_or_reference_declaration:
            declarations.setdefault(name, []).append(index)
    return declarations


def is_shadowed(tokens: tuple[Token, ...], name: str, index: int) -> bool:
    paths = scope_paths(tokens)
    return any(
        declaration < index and paths[index][: len(paths[declaration])] == paths[declaration]
        for declaration in local_declarations(tokens).get(name, [])
    )


def global_reference_aliases(
    tokens: tuple[Token, ...], globals_in_plugin: set[str]
) -> dict[str, list[tuple[str, int, bool]]]:
    aliases: dict[str, list[tuple[str, int, bool]]] = {}
    for index, token in enumerate(tokens):
        if token.value != "=":
            continue
        source_index = index + 1
        if source_index < len(tokens) and tokens[source_index].value == "&":
            source_index += 1
        if source_index >= len(tokens) or tokens[source_index].value not in globals_in_plugin:
            continue
        start = index - 1
        while start >= 0 and tokens[start].value not in {";", "{", "}"}:
            start -= 1
        declaration = tokens[start + 1 : index]
        if not any(item.value in {"&", "&&", "*"} for item in declaration):
            continue
        if is_shadowed(tokens, tokens[source_index].value, source_index):
            continue
        alias = next(
            (item.value for item in reversed(declaration) if re.fullmatch(r"[A-Za-z_]\w*", item.value)),
            None,
        )
        if alias:
            alias_index = next(i for i in range(start + 1, index) if tokens[i].value == alias)
            is_pointer = any(item.value == "*" for item in declaration)
            aliases.setdefault(alias, []).append((tokens[source_index].value, alias_index, is_pointer))
    return aliases


def is_pointer_rebinding(tokens: tuple[Token, ...], alias_index: int) -> bool:
    return (alias_index + 1 < len(tokens) and tokens[alias_index + 1].value in {"=", "+=", "-="}) or (
        alias_index > 0 and tokens[alias_index - 1].value in {"++", "--"}
    ) or (alias_index + 1 < len(tokens) and tokens[alias_index + 1].value in {"++", "--"})


def alias_global_target(
    tokens: tuple[Token, ...], alias: str, declaration: int, use: int, source: str, is_pointer: bool,
    globals_in_plugin: set[str]
) -> str | None:
    paths = scope_paths(tokens)
    if declaration >= use or paths[use][: len(paths[declaration])] != paths[declaration]:
        return None
    if any(
        position > declaration
        and position < use
        and paths[use][: len(paths[position])] == paths[position]
        for position in local_declarations(tokens).get(alias, [])
    ):
        return None
    target = source
    if is_pointer:
        for index in range(declaration + 1, use):
            if tokens[index].value != alias or not is_pointer_rebinding(tokens, index):
                continue
            if index + 1 >= len(tokens) or tokens[index + 1].value not in {"=", "+=", "-="}:
                target = None
                continue
            rhs = index + 2
            if rhs < len(tokens) and tokens[rhs].value == "&":
                rhs += 1
            target = (
                tokens[rhs].value
                if rhs < len(tokens)
                and tokens[rhs].value in globals_in_plugin
                and not is_shadowed(tokens, tokens[rhs].value, rhs)
                else None
            )
    return target


def forbidden_type_aliases(tokens: list[Token], forbidden: set[str]) -> list[tuple[str, tuple[str, ...]]]:
    namespaces = namespace_paths(tokens)
    function_ranges = [(start, end) for start, end, _ in function_body_specs(tokens)]

    def in_function(index: int) -> bool:
        return any(start < index < end for start, end in function_ranges)

    aliases: list[tuple[str, tuple[str, ...]]] = []

    def alias_is_forbidden(type_tokens: list[Token], namespace: tuple[str, ...]) -> bool:
        for item in type_tokens:
            if item.value in forbidden:
                return True
            if any(
                alias == item.value and namespace[: len(alias_namespace)] == alias_namespace
                for alias, alias_namespace in aliases
            ):
                return True
        return False

    for index, token in enumerate(tokens):
        if in_function(index):
            continue
        if token.value == "using" and index + 2 < len(tokens) and tokens[index + 2].value == "=":
            end = next((i for i in range(index + 3, len(tokens)) if tokens[i].value == ";"), len(tokens))
            if alias_is_forbidden(tokens[index + 3 : end], namespaces[index]):
                aliases.append((tokens[index + 1].value, namespaces[index]))
        elif token.value == "using":
            end = next((i for i in range(index + 1, len(tokens)) if tokens[i].value == ";"), len(tokens))
            declaration = tokens[index + 1 : end]
            if len(declaration) >= 3 and all(
                declaration[position].value == "::" for position in range(1, len(declaration), 2)
            ):
                imported_namespace = tuple(declaration[position].value for position in range(0, len(declaration), 2))[:-1]
                imported_name = declaration[-1].value
                if any(alias == imported_name and namespace == imported_namespace for alias, namespace in aliases):
                    aliases.append((imported_name, namespaces[index]))
        elif token.value == "typedef":
            end = next((i for i in range(index + 1, len(tokens)) if tokens[i].value == ";"), len(tokens))
            declaration = tokens[index + 1 : end]
            if alias_is_forbidden(declaration, namespaces[index]):
                name = next(
                    (item.value for item in reversed(declaration) if re.fullmatch(r"[A-Za-z_]\w*", item.value)), None
                )
                if name:
                    aliases.append((name, namespaces[index]))
    return aliases


def namespace_scope_flags(tokens: list[Token]) -> list[bool]:
    scopes: list[str] = []
    flags = []
    for index, token in enumerate(tokens):
        flags.append(all(scope == "namespace" for scope in scopes))
        if token.value == "{":
            start = index - 1
            while start >= 0 and tokens[start].value not in {";", "{", "}"}:
                start -= 1
            prefix = {item.value for item in tokens[start + 1 : index]}
            scopes.append("namespace" if "namespace" in prefix else "block")
        elif token.value == "}" and scopes:
            scopes.pop()
    return flags


def declared_types(tokens: list[Token]) -> list[tuple[str, tuple[str, ...], int]]:
    namespaces = namespace_paths(tokens)
    namespace_scope = namespace_scope_flags(tokens)
    function_ranges = [(start, end) for start, end, _ in function_body_specs(tokens)]
    return [
        (tokens[index + 1].value, namespaces[index], tokens[index].line)
        for index, token in enumerate(tokens[:-1])
        if token.value in {"class", "struct", "enum"}
        and re.fullmatch(r"[A-Za-z_]\w*", tokens[index + 1].value)
        and namespace_scope[index]
        and not any(start < index < end for start, end in function_ranges)
    ]


def included_source_paths(path: str, sources: dict[str, str]) -> set[str]:
    visible = {path}
    pending = [path]
    while pending:
        current = pending.pop()
        for include in re.findall(r'^\s*#\s*include\s*"([^"]+)"', sources[current], re.MULTILINE):
            candidate = (Path(current).parent / include).as_posix()
            if candidate not in sources:
                matches = [source_path for source_path in sources if Path(source_path).name == Path(include).name]
                if len(matches) != 1:
                    continue
                candidate = matches[0]
            if candidate not in visible:
                visible.add(candidate)
                pending.append(candidate)
    return visible


def qualified_alias_used(tokens: tuple[Token, ...], alias: str, namespace: tuple[str, ...]) -> bool:
    if namespace:
        pattern = []
        for component in namespace:
            pattern.extend([component, "::"])
        pattern.append(alias)
        return next(sequences(tokens, pattern), None) is not None
    return any(index > 0 and tokens[index - 1].value == "::" for index, token in enumerate(tokens) if token.value == alias)


def forbidden_local_type_aliases(tokens: tuple[Token, ...], forbidden: set[str]) -> set[str]:
    paths = scope_paths(tokens)
    aliases: list[tuple[str, tuple[int, ...], int]] = []

    def alias_is_forbidden(type_tokens: tuple[Token, ...] | list[Token], index: int) -> bool:
        return any(item.value in forbidden for item in type_tokens) or any(
            alias == item.value
            and declaration < index
            and paths[index][: len(scope)] == scope
            for item in type_tokens
            for alias, scope, declaration in aliases
        )

    result = set()
    local_types = {
        (tokens[index + 1].value, paths[index])
        for index, token in enumerate(tokens[:-1])
        if token.value in {"class", "struct", "enum"}
        and re.fullmatch(r"[A-Za-z_]\w*", tokens[index + 1].value)
    }
    for index, token in enumerate(tokens):
        if token.value == "using" and index + 2 < len(tokens) and tokens[index + 2].value == "=":
            end = next((i for i in range(index + 3, len(tokens)) if tokens[i].value == ";"), len(tokens))
            alias = tokens[index + 1].value
            if alias_is_forbidden(tokens[index + 3 : end], index):
                aliases.append((alias, paths[index], index))
                result.add(alias)
        elif token.value == "typedef":
            end = next((i for i in range(index + 1, len(tokens)) if tokens[i].value == ";"), len(tokens))
            declaration = tokens[index + 1 : end]
            alias = next(
                (item.value for item in reversed(declaration) if re.fullmatch(r"[A-Za-z_]\w*", item.value)), None
            )
            if alias and alias_is_forbidden(declaration, index):
                aliases.append((alias, paths[index], index))
                result.add(alias)
    return {
        alias
        for alias in result
        if not any(
            local_name == alias and paths[-1][: len(scope)] == scope
            for local_name, scope in local_types
        )
    }


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

    reachable: set[Function] = set()
    pending = [function for function in functions if function.name in {"process", "execute"}]
    while pending:
        function = pending.pop()
        if function in reachable:
            continue
        reachable.add(function)
        for called, qualifier in called_names(function.tokens, function.owner):
            candidates = by_name.get(called, [])
            if qualifier:
                candidates = [candidate for candidate in candidates if candidate.owner == qualifier]
            else:
                same_owner = [
                    candidate
                    for candidate in candidates
                    if function.owner and candidate.owner == function.owner
                    and namespace_visible(candidate.namespace, function.namespace)
                ]
                free_functions = [
                    candidate
                    for candidate in candidates
                    if not candidate.owner
                    and namespace_visible(candidate.namespace, function.namespace)
                ]
                candidates = same_owner or free_functions
            pending.extend(candidate for candidate in candidates if candidate not in reachable)

    functions_by_path: dict[str, list[Function]] = {}
    for function in functions:
        functions_by_path.setdefault(function.path, []).append(function)
    globals_by_path = {
        path: global_variables(tokens, functions_by_path.get(path, [])) for path, tokens in tokenized.items()
    }
    globals_in_plugin = set().union(*globals_by_path.values())
    static_members = set().union(*(class_static_variables(tokens) for tokens in tokenized.values()))
    forbidden = set(rules["forbidden_identifiers"])
    aliases_by_path = {path: forbidden_type_aliases(tokens, forbidden) for path, tokens in tokenized.items()}
    types_by_path = {path: declared_types(tokens) for path, tokens in tokenized.items()}
    findings = []
    for function in sorted(reachable, key=lambda item: (item.path, item.line)):
        name = function.name
        body = function.tokens
        forbidden_aliases = forbidden_local_type_aliases(body, forbidden)
        local_paths = scope_paths(body)
        local_types = [
            (body[index + 1].value, local_paths[index], index)
            for index, token in enumerate(body[:-1])
            if token.value in {"class", "struct", "enum"}
            and re.fullmatch(r"[A-Za-z_]\w*", body[index + 1].value)
        ]
        visible_files = included_source_paths(function.path, sources)
        function_namespace = tuple(part for part in function.namespace if part != "<anonymous>")
        for source_path in visible_files:
            for alias, alias_namespace in aliases_by_path.get(source_path, []):
                lookup_namespace = tuple(part for part in alias_namespace if part != "<anonymous>")
                qualified_used = qualified_alias_used(body, alias, lookup_namespace)
                if function_namespace[: len(lookup_namespace)] != lookup_namespace and not qualified_used:
                    continue
                shadowed = any(
                    type_name == alias
                    and len(tuple(part for part in type_namespace if part != "<anonymous>")) >= len(lookup_namespace)
                    and function_namespace[: len(tuple(part for part in type_namespace if part != "<anonymous>"))]
                    == tuple(part for part in type_namespace if part != "<anonymous>")
                    and (type_path != function.path or type_line < function.line)
                    for type_path, declarations in types_by_path.items()
                    if type_path in visible_files
                    for type_name, type_namespace, type_line in declarations
                )
                if qualified_used or not shadowed:
                    forbidden_aliases.add(alias)

        def report(line: int, message: str):
            findings.append(f"{function.path}:{line}: {plugin_id}::{name}: {message}")

        for index, token in enumerate(body):
            shadowed_local_type = any(
                type_name == token.value
                and declaration < index
                and local_paths[index][: len(scope)] == scope
                for type_name, scope, declaration in local_types
            )
            if (token.value in forbidden or token.value in forbidden_aliases) and not shadowed_local_type:
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
                if called == method and body[index].value != "GlobalVarManager":
                    report(body[index].line, f"GlobalVarManager write call {method}()")

        for method in rules["run_engine_write_calls"]:
            pattern = ["RunEngine", "::", "instance", "(", ")", ".", method, "("]
            for index in sequences(body, pattern):
                report(body[index].line, f"RunEngine write call {method}()")
            for index, called in singleton_calls(body, "RunEngine"):
                if called == method and body[index].value != "RunEngine":
                    report(body[index].line, f"RunEngine write call {method}()")

        for index, method in singleton_calls(body, "RunEngine"):
            allowed = any(
                item["plugin_id"] == plugin_id and item["function"] == name and item["method"] == method
                for item in rules["run_engine_read_allowlist"]
            )
            if not allowed:
                report(body[index].line, f"unregistered RunEngine singleton access ({method})")

        aliases = global_reference_aliases(body, globals_in_plugin)
        for global_name in globals_in_plugin:
            for index, token in enumerate(body):
                if token.value == global_name and not is_shadowed(body, global_name, index) and is_mutated_lvalue(body, index):
                    report(token.line, f"write to file-scope variable {global_name}")
            for alias, sources in aliases.items():
                for index, token in enumerate(body):
                    if token.value != alias:
                        continue
                    if not is_mutated_lvalue(body, index):
                        continue
                    if any(
                        declaration != index
                        and alias_global_target(body, alias, declaration, index, source, is_pointer, globals_in_plugin)
                        == global_name
                        and not (is_pointer and is_pointer_rebinding(body, index))
                        for source, declaration, is_pointer in sources
                    ):
                        report(token.line, f"write to file-scope variable {global_name} via reference alias {alias}")
            mutating_methods = set(rules.get("global_mutating_methods", []))
            roots = {global_name}
            roots.update(aliases)
            for index, method, root in global_mutating_method_calls(body, roots, mutating_methods):
                if root == global_name and is_shadowed(body, global_name, index):
                    continue
                if root != global_name and not any(
                    alias_global_target(body, root, declaration, index, source, is_pointer, globals_in_plugin) == global_name
                    for source, declaration, is_pointer in aliases.get(root, [])
                ):
                    continue
                report(body[index].line, f"possible in-place mutation of file-scope variable {global_name} via {method}()")

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
