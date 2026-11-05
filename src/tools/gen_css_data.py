"""Writes src/generated/CssData.sa: the CSS that qrun checks stylesheets
against, from MDN's machine-readable CSS data (the mdn-data package,
https://github.com/mdn/data) and src/tools/css_data_patch.json.

  - every property with its value grammar (CSS value definition syntax)
  - every production ("<color>", "rgb()") those grammars use
  - at-rules: the grammar of each one's prelude, what its block holds, and
    its descriptors with their grammars
  - pseudo-classes and pseudo-elements (and the grammar of their arguments)
  - units by type (length, angle, time, ...)

Run from compiler/:
    python src/tools/gen_css_data.py PATH/TO/mdn-data
(PATH/TO/mdn-data: the package folder, e.g. node_modules/mdn-data after
`npm install mdn-data`). --json OUT also writes the merged grammars as JSON,
which conformance/css_values.py hands to css-tree as its oracle.
"""
import json
import os
import re
import sys

OUT = "src/generated/CssData.sa"
PATCH = "src/tools/css_data_patch.json"

# productions the matcher (src/css/CssGrammar.sa) implements itself
BUILTIN = {
    "ident", "custom-ident", "dashed-ident", "custom-property-name", "string", "number", "integer",
    "percentage", "dimension", "length", "angle", "time", "frequency", "resolution", "flex", "decibel",
    "semitones", "zero", "hex-color", "declaration-value", "any-value", "an-plus-b", "urange",
    "ident-token", "function-token", "string-token", "hash-token", "number-token", "dimension-token",
    "percentage-token", "url-token", "delim-token", "id-selector", "an+b",
    # selectors in the arguments of :is(), :not(), :has() ...: src/css/CssCheck.sa checks them
    "selector", "forgiving-selector-list", "forgiving-relative-selector-list",
}


def load(folder, name):
    with open(os.path.join(folder, "css", name), encoding="utf-8") as f:
        return json.load(f)


def clean(syntax):
    """One line, single spaces, ASCII (the infinity sign becomes "inf")."""
    syntax = syntax.replace("∞", "inf")
    return " ".join(syntax.split())


def c_string(text):
    out = '"'
    for ch in text:
        if ch in '"\\':
            out += "\\" + ch
        elif ch == "?":
            out += "\\?"  # no trigraphs
        elif 32 <= ord(ch) < 127:
            out += ch
        else:
            out += "".join("\\x%02X" % b for b in ch.encode("utf-8"))
    return out + '"'


def table(name, rows, width):
    lines = ["const char *const %s[] = {" % name]
    for row in rows:
        assert len(row) == width, row
        lines.append("    " + ", ".join(c_string(v) for v in row) + ",")
    lines.append("    nullptr};")
    return lines


def references(syntax):
    return set(m.group(1) for m in re.finditer(r"<([a-zA-Z0-9+-]+(?:\(\))?)(?: \[[^\]]*\])?>", syntax))


def main():
    args = sys.argv[1:]
    json_out = None
    if "--json" in args:
        k = args.index("--json")
        json_out = args[k + 1]
        del args[k:k + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    folder = args[0]
    with open(os.path.join(folder, "package.json"), encoding="utf-8") as f:
        version = json.load(f)["version"]
    with open(PATCH, encoding="utf-8") as f:
        patch = json.load(f)

    properties = {k: clean(v["syntax"]) for k, v in load(folder, "properties.json").items()}
    status = {k: v.get("status", "standard") for k, v in load(folder, "properties.json").items()}
    for k, v in patch["properties"].items():
        if k != "comment":
            properties[k] = clean(v["syntax"])
            status.setdefault(k, "standard")

    types = {k: clean(v["syntax"]) for k, v in load(folder, "syntaxes.json").items()}
    for k, v in load(folder, "functions.json").items():
        types.setdefault(k, clean(v["syntax"]))
    for k, v in patch["types"].items():
        types[k] = clean(v["syntax"])

    atrules_json = load(folder, "at-rules.json")
    atrules = []
    descriptors = []
    for name, info in sorted(patch["atrules"].items()):
        if name == "comment":
            continue
        atrules.append((name, clean(info["prelude"]), clean(info.get("blockPrelude", info["prelude"])), info["block"]))
        found = atrules_json.get("@" + name, {})
        for dname, d in sorted(found.get("descriptors", {}).items()):
            descriptors.append((name, dname, clean(d["syntax"])))
        for dname, syntax in sorted(patch["descriptors"].get(name, {}).items()):
            descriptors.append((name, dname, clean(syntax)))
    missing_rules = sorted(set(k[1:] for k in atrules_json) - set(r[0] for r in atrules))
    if missing_rules:
        sys.exit("at-rules without a prelude in the patch: %s" % missing_rules)

    pseudo_classes, pseudo_elements = [], []
    for key, info in load(folder, "selectors.json").items():
        if not key.startswith(":"):
            continue
        syntax = clean(info["syntax"])
        element = key.startswith("::")
        name = key[2:] if element else key[1:]
        args = ""
        if name.endswith("()"):
            name = name[:-2]
            m = re.match(r"^::?[A-Za-z0-9-]+\((.*)\)$", syntax)
            args = clean(m.group(1)) if m else "<any-value>"
            name += "()"
        (pseudo_elements if element else pseudo_classes).append((name, args))
    for key, args in patch["pseudoClasses"].items():
        if key != "comment" and key not in dict(pseudo_classes):
            pseudo_classes.append((key, clean(args)))
    for key, args in patch["pseudoElements"].items():
        if key != "comment" and key not in dict(pseudo_elements):
            pseudo_elements.append((key, clean(args)))

    units = []
    for group, names in patch["units"].items():
        if group == "comment":
            continue
        for u in names.split():
            units.append((u, group))
    known_units = set(u for u, _ in units)
    for u in load(folder, "units.json"):
        if u.lower() not in known_units:
            sys.exit("units.json has %r, which the patch does not place in a group" % u)

    # every production a grammar uses must be defined (or built in)
    defined = set(types) | BUILTIN
    used = set()
    for s in list(properties.values()) + list(types.values()) + [r[1] for r in atrules] + [r[2] for r in atrules] + \
            [d[2] for d in descriptors] + [a for _, a in pseudo_classes] + [a for _, a in pseudo_elements]:
        used |= references(s)
    undefined = sorted(u for u in used if u not in defined)
    if undefined:
        print("note: productions used but not defined (they match anything): %s" % ", ".join(undefined))

    out = [
        "// quantum: cpp",
        "// Generated by src/tools/gen_css_data.py from mdn-data %s (MDN's CSS data," % version,
        "// https://github.com/mdn/data) and src/tools/css_data_patch.json. Do not edit.",
        '#include "../css/CssData.h.sa"',
        "",
        "namespace css",
        "{",
        "namespace data",
        "{",
        "",
        'const char *const kVersion = "mdn-data %s";' % version,
        "",
        "// property, its value grammar, its status (standard, experimental, nonstandard, obsolete)",
    ]
    out += table("kProperties", [(k, properties[k], status[k]) for k in sorted(properties)], 3)
    out += ["", "// production (\"color\", \"rgb()\"), its grammar"]
    out += table("kTypes", [(k, types[k]) for k in sorted(types)], 2)
    out += ["", "// at-rule, its prelude's grammar, its prelude's grammar when it has a block, what the block holds"]
    out += table("kAtRules", atrules, 4)
    out += ["", "// at-rule, descriptor, its grammar"]
    out += table("kDescriptors", descriptors, 3)
    out += ["", "// pseudo-class (\"hover\", or \"nth-child()\" for a function), its arguments' grammar"]
    out += table("kPseudoClasses", sorted(pseudo_classes), 2)
    out += ["", "// pseudo-element, its arguments' grammar"]
    out += table("kPseudoElements", sorted(pseudo_elements), 2)
    out += ["", "// unit (lowercase), its type"]
    out += table("kUnits", sorted(units), 2)
    out += ["", "} // namespace data", "} // namespace css", ""]
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print("wrote %s: %d properties, %d productions, %d at-rules, %d descriptors, %d pseudo-classes, "
          "%d pseudo-elements, %d units" % (OUT, len(properties), len(types), len(atrules), len(descriptors),
                                            len(pseudo_classes), len(pseudo_elements), len(units)))
    if json_out:
        merged = {
            "version": version,
            "properties": properties,
            "types": types,
            "atrules": {r[0]: {"prelude": r[1], "blockPrelude": r[2], "block": r[3]} for r in atrules},
            "descriptors": [list(d) for d in descriptors],
            "units": dict(units),
        }
        with open(json_out, "w", encoding="utf-8") as f:
            json.dump(merged, f, indent=1)


if __name__ == "__main__":
    main()
