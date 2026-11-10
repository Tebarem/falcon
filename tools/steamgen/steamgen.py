"""Generates Falcon's C# bindings for the whole Steamworks flat API.

    python tools/steamgen/steamgen.py

Reads external/steamworks/include/steam/steam_api.json (scripts/fetch-steamworks.ps1 unpacks it)
and writes managed/Falcon.Engine/Steam/Generated/*.g.cs, which are committed: building the engine
needs neither the SDK nor this script.

Struct layouts aren't guessed. A probe program built from the SDK's own headers with MSVC (found
through vswhere) prints every struct's size and every field's offset, and every constant's value;
the C# structs use those offsets (explicit layout). A field whose C# type would come out a
different size (unions, nested types the JSON doesn't describe) becomes opaque bytes of the
measured size, so no layout can be wrong.

Calls go through function pointers looked up by name in steam_api64.dll (loaded by the engine), so
the bindings work with NativeAOT and need no import library.
"""

import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SDK = os.path.join(ROOT, "external", "steamworks", "include", "steam")
OUT = os.path.join(ROOT, "managed", "Falcon.Engine", "Steam", "Generated")

KEYWORDS = {
    "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class",
    "const", "continue", "decimal", "default", "delegate", "do", "double", "else", "enum", "event",
    "explicit", "extern", "false", "finally", "fixed", "float", "for", "foreach", "goto", "if",
    "implicit", "in", "int", "interface", "internal", "is", "lock", "long", "namespace", "new", "null",
    "object", "operator", "out", "override", "params", "private", "protected", "public", "readonly",
    "ref", "return", "sbyte", "sealed", "short", "sizeof", "stackalloc", "static", "string", "struct",
    "switch", "this", "throw", "true", "try", "typeof", "uint", "ulong", "unchecked", "unsafe",
    "ushort", "using", "virtual", "void", "volatile", "while",
}

PRIMITIVE = {
    "unsigned char": "byte", "signed char": "sbyte", "char": "byte", "uint8": "byte", "int8": "sbyte",
    "short": "short", "unsigned short": "ushort", "int16": "short", "uint16": "ushort",
    "int": "int", "unsigned int": "uint", "int32": "int", "uint32": "uint", "int32_t": "int", "uint32_t": "uint",
    "long long": "long", "unsigned long long": "ulong", "int64": "long", "uint64": "ulong",
    "int64_t": "long", "uint64_t": "ulong", "lint64": "long", "ulint64": "ulong",
    "intp": "long", "uintp": "ulong", "intptr_t": "nint", "size_t": "nuint",
    "float": "float", "double": "double", "bool": "Bool8",
    "CSteamID": "ulong", "CGameID": "ulong", "uint64_steamid": "ulong", "uint64_gameid": "ulong",
}
SIZES = {"byte": 1, "sbyte": 1, "short": 2, "ushort": 2, "int": 4, "uint": 4, "long": 8, "ulong": 8,
         "nint": 8, "nuint": 8, "float": 4, "double": 8, "Bool8": 1}
FIXED_OK = {"byte", "sbyte", "short", "ushort", "int", "uint", "long", "ulong", "float", "double"}


def ident(name):
    return "@" + name if name in KEYWORDS else name


class Types:
    def __init__(self, api):
        self.typedefs = {t["typedef"]: t["type"] for t in api["typedefs"]}
        self.enums = {}
        for e in api["enums"]:
            self.enums[e["enumname"]] = e
        for i in api["interfaces"]:
            for e in i.get("enums", []):
                self.enums[e["enumname"]] = e
                self.enums[i["classname"] + "::" + e["enumname"]] = e
        self.structs = {s["struct"]: s for s in api["structs"] + api["callback_structs"]}
        self.layout = {}  # struct -> (size, {field: (offset, size)})

    def base(self, t):
        """A C type without pointers or const: its C# type, or None (unknown)."""
        t = t.strip()
        if t in PRIMITIVE:
            return PRIMITIVE[t]
        if t in self.enums:
            return self.enums[t]["enumname"]
        if t in self.structs:
            return t
        if t.startswith("ISteam") or t.startswith("void (*)") or t.startswith("Fn") or t.startswith("FSteam") or t.endswith("CallbackPointer") or t == "SteamAPIWarningMessageHook_t":
            return "nint"
        if t in self.typedefs:
            target = self.typedefs[t]
            if "(*)" in target or target.endswith("*"):
                return "nint"
            if "[" in target:
                return None  # (an array typedef: by pointer only)
            return self.base(target)
        return None

    def pointer(self, t):
        """A parameter or return type: its C# type for the raw call (pointers stay pointers)."""
        t = t.strip()
        stars = 0
        if t.endswith("&"):
            t, stars = t[:-1].strip(), 1
        while t.endswith("*"):
            t, stars = t[:-1].strip(), stars + 1
        if t.startswith("const "):
            t = t[6:].strip()
        if t == "void" and stars:
            return "void" + "*" * stars
        if t == "void":
            return "void"
        b = self.base(t)
        if b is None:
            if stars:
                return "void" + "*" * stars  # (opaque: by pointer)
            raise SystemExit(f"steamgen: no C# type for {t}")
        if b == "nint" and stars and (t.startswith("ISteam")):
            stars -= 1  # (an interface pointer is itself the handle)
        return b + "*" * stars

    def size(self, cs):
        if cs.endswith("*"):
            return 8
        if cs in SIZES:
            return SIZES[cs]
        if cs in self.enums or any(e["enumname"] == cs for e in self.enums.values()):
            return 4
        if cs in self.layout:
            return self.layout[cs][0]
        return None


def find_vcvars():
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Microsoft Visual Studio", "Installer", "vswhere.exe")
    path = subprocess.check_output([vswhere, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
    return os.path.join(path, "VC", "Auxiliary", "Build", "vcvars64.bat")


def probe(api, types):
    """Sizes and offsets from MSVC with the SDK's headers, and the constants' values. Types and
    constants the JSON names but the public headers don't define (Valve's datagram tickets) are
    left out: such types are only ever used by pointer."""
    missing = set()
    work = tempfile.mkdtemp(prefix="falcon-steamgen-")
    for attempt in range(4):
        lines = [
            "#define _ALLOW_KEYWORD_MACROS",
            "#define private public  // (gameserveritem_t's name)",
            '#include "steam/steam_api_flat.h"',
            '#include "steam/steam_gameserver.h"',
            '#include "steam/steamnetworkingfakeip.h"',
            "#undef private",
            "#include <cstdio>",
            "#include <cstddef>",
            "int main() {",
        ]
        for name, st in types.structs.items():
            if name in missing:
                continue
            lines.append(f'  std::printf("S {name} %zu\\n", sizeof({name}));')
            for f in st["fields"]:
                fn = f["fieldname"]
                if (name, fn) in missing:
                    continue
                lines.append(f'  std::printf("F {name} {fn} %zu %zu\\n", offsetof({name}, {fn}), sizeof((({name}*)nullptr)->{fn}));')
        lines.append('  std::printf("S CallbackMsg_t %zu\\n", sizeof(CallbackMsg_t));')
        for fn in ("m_hSteamUser", "m_iCallback", "m_pubParam", "m_cubParam"):
            lines.append(f'  std::printf("F CallbackMsg_t {fn} %zu %zu\\n", offsetof(CallbackMsg_t, {fn}), sizeof(((CallbackMsg_t*)nullptr)->{fn}));')
        for c in api["consts"]:
            n = c["constname"]
            if n in missing:
                continue
            if c["consttype"] == "float":
                lines.append(f'  std::printf("C {n} %.9g\\n", (double)({n}));')
            else:
                lines.append(f'  std::printf("C {n} %lld\\n", (long long)({n}));')
        for st in types.structs.values():
            if st["struct"] in missing:
                continue
            for c in st.get("consts", []):
                n = c["constname"]
                lines.append(f'  std::printf("C {st["struct"]}::{n} %lld\\n", (long long)({st["struct"]}::{n}));')
        lines += ["  return 0;", "}"]
        with open(os.path.join(work, "probe.cpp"), "w") as f:
            f.write("\n".join(lines) + "\n")
        bat = os.path.join(work, "build.bat")
        with open(bat, "w") as f:
            f.write(f'@call "{find_vcvars()}" >nul\n@cl /nologo /std:c++17 /EHsc /W0 /I"{os.path.dirname(SDK)}" probe.cpp /Feprobe.exe\n')
        r = subprocess.run(["cmd", "/d", "/c", bat], cwd=work, capture_output=True, text=True)
        if r.returncode == 0:
            break
        found = set(re.findall(r"use of undefined type '(\w+)'", r.stdout)) | set(re.findall(r"'(\w+)': undeclared identifier", r.stdout))
        found |= set(re.findall(r"'(\w+)': is not a member of '(\w+)'", r.stdout))
        found = {(m[1], m[0]) if isinstance(m, tuple) else m for m in found}
        if not found - missing:
            raise SystemExit("steamgen: the probe didn't build:\n" + r.stdout + r.stderr)
        missing |= found
    else:
        raise SystemExit("steamgen: the probe didn't build")
    for name in sorted(missing, key=str):
        if isinstance(name, tuple):
            print(f"steamgen: {name[0]}.{name[1]} is in a union: left as bytes")
            st = types.structs[name[0]]
            st["fields"] = [f for f in st["fields"] if f["fieldname"] != name[1]]
        else:
            print(f"steamgen: {name} isn't in the public headers: left out" + ("" if name.startswith("k_") else " (used by pointer)"))
            types.structs.pop(name, None)
    out = subprocess.check_output([os.path.join(work, "probe.exe")], text=True)
    consts = {}
    for line in out.splitlines():
        parts = line.split()
        if parts[0] == "S":
            types.layout[parts[1]] = (int(parts[2]), {})
        elif parts[0] == "F":
            types.layout[parts[1]][1][parts[2]] = (int(parts[3]), int(parts[4]))
        elif parts[0] == "C":
            consts[parts[1]] = parts[2]
    return consts


def header(w):
    w.append("// Generated by tools/steamgen/steamgen.py from the Steamworks SDK 1.65's steam_api.json, with")
    w.append("// layouts measured from its headers. Don't edit: run the generator again.")
    w.append("#nullable enable")
    w.append("#pragma warning disable CS1591, CS0649, IDE1006")
    w.append("using System;")
    w.append("using System.Runtime.InteropServices;")
    w.append("using System.Threading.Tasks;")
    w.append("")
    w.append("namespace Falcon.Steamworks;")
    w.append("")


def text_name(field, taken):
    n = field[2:] if field.startswith("m_") else field
    n = re.sub(r"^(sz|rgch|pch|ch)(?=[A-Z])", "", n)
    n = n[:1].upper() + n[1:]
    if n in taken:
        n += "Text"
    return n


def emit_types(api, types, consts, warnings):
    w = []
    header(w)
    # constants
    w.append("public static class SteamConstants")
    w.append("{")
    for c in api["consts"]:
        if c["constname"] not in consts:
            continue
        cs = types.pointer(c["consttype"]) if c["consttype"] != "float" else "float"
        v = consts[c["constname"]]
        if cs == "float":
            w.append(f"    public const float {c['constname']} = {v}f;")
        else:
            w.append(f"    public const {cs} {c['constname']} = unchecked(({cs}){v}L);")
    w.append("}")
    w.append("")
    # enums
    seen = set()
    for e in list(api["enums"]) + [e for i in api["interfaces"] for e in i.get("enums", [])]:
        if e["enumname"] in seen:
            continue
        seen.add(e["enumname"])
        w.append(f"public enum {e['enumname']}")
        w.append("{")
        for v in e["values"]:
            w.append(f"    {v['name']} = unchecked((int){v['value']}),")
        w.append("}")
        w.append("")
    # structs (callbacks too)
    callbacks = {s["struct"]: s["callback_id"] for s in api["callback_structs"]}
    flat_methods = {}
    for s in api["structs"]:
        if s.get("methods"):
            flat_methods[s["struct"]] = s["methods"]
    for name, s in types.structs.items():
        size, offsets = types.layout[name]
        iface = " : ISteamCallback" if name in callbacks else ""
        w.append(f"[StructLayout(LayoutKind.Explicit, Size = {size})]")
        w.append(f"public unsafe partial struct {name}{iface}")
        w.append("{")
        if name in callbacks:
            w.append(f"    public const int Id = {callbacks[name]};")
            w.append("    static int ISteamCallback.CallbackId => Id;")
        for c in s.get("consts", []):
            w.append(f"    public const int {c['constname']} = {consts[name + '::' + c['constname']]};")
        texts = []
        taken = {f["fieldname"] for f in s["fields"]} | {m["methodname"] for m in s.get("methods", [])}
        for f in s["fields"]:
            fn, ft = f["fieldname"], f["fieldtype"]
            offset, fsize = offsets[fn]
            m = re.fullmatch(r"(.+?)\s*\[(\d+)\]", ft)
            cs = None
            if m:
                elem = types.base(m.group(1))
                count = int(m.group(2))
                if elem in FIXED_OK and SIZES[elem] * count == fsize:
                    w.append(f"    [FieldOffset({offset})] public fixed {elem} {ident(fn)}[{count}];")
                    if m.group(1) == "char":
                        tn = text_name(fn, taken)
                        taken.add(tn)
                        texts.append((tn, fn, count))
                    continue
            else:
                try:
                    cs = types.pointer(ft) if ("*" in ft or "&" in ft) else types.base(ft)
                except SystemExit:
                    cs = None
                if cs is not None and types.size(cs) == fsize:
                    w.append(f"    [FieldOffset({offset})] public {cs} {ident(fn)};")
                    continue
            warnings.append(f"{name}.{fn} ({ft}): opaque, {fsize} bytes")
            w.append(f"    [FieldOffset({offset})] public fixed byte {ident(fn)}[{fsize}];  // ({ft})")
        for tn, fn, count in texts:
            w.append(f"    /// <summary>{fn} as text.</summary>")
            w.append(f"    public string {tn} {{ get {{ fixed (byte* p = {ident(fn)}) return SteamText.Read(p, {count}); }} }}")
        for meth in s.get("methods", []):
            emit_struct_method(w, types, name, meth)
        w.append("}")
        w.append("")
    # the manual dispatch's message (steam_api_common.h; not in the JSON)
    size, offsets = types.layout["CallbackMsg_t"]
    w.append(f"[StructLayout(LayoutKind.Explicit, Size = {size})]")
    w.append("public unsafe struct CallbackMsg_t")
    w.append("{")
    for fn, cs in (("m_hSteamUser", "int"), ("m_iCallback", "int"), ("m_pubParam", "byte*"), ("m_cubParam", "int")):
        w.append(f"    [FieldOffset({offsets[fn][0]})] public {cs} {fn};")
    w.append("}")
    w.append("")
    return w


def param_info(types, p):
    """(public type, raw type, kind) for a parameter."""
    t = p["paramtype"]
    flat = p.get("paramtype_flat")
    if flat in ("uint64_steamid", "uint64_gameid"):
        return "ulong", "ulong", "value"
    raw = types.pointer(t)
    stripped = t.strip()
    if stripped in ("const char *",):
        return "string?", "byte*", "string"
    if stripped.endswith("&") and raw.endswith("*") and not raw.startswith("void"):
        inner = raw[:-1]
        if "*" not in inner and inner != "byte":
            return ("in " if stripped.startswith("const ") else "ref ") + inner, raw, "ref"
    if raw == "Bool8":
        return "bool", "Bool8", "bool"
    return raw, raw, "value"


def return_info(types, m):
    t = m.get("returntype_flat") or m["returntype"]
    if t in ("uint64_steamid", "uint64_gameid"):
        return "ulong", "ulong", "value"
    if t.strip() == "const char *":
        return "string", "byte*", "string"
    raw = types.pointer(t)
    if raw == "Bool8":
        return "bool", "Bool8", "bool"
    return raw, raw, "value"


FLAT = {}  # flat name -> (raw return, [raw params])


def add_flat(name, ret, params):
    FLAT[name] = (ret, params)


def call_body(w, indent, flat, self_arg, params, ret_kind, ret_pub, async_result=None):
    pre, fixeds, args = [], [], []
    if self_arg is not None:
        args.append(self_arg)
    for (pname, pub, raw, kind) in params:
        if kind == "string":
            pre.append(f"byte[]? {pname.lstrip('@')}_utf8 = SteamText.Utf8({pname});")
            fixeds.append(f"fixed (byte* {pname.lstrip('@')}_p = {pname.lstrip('@')}_utf8)")
            args.append(f"{pname.lstrip('@')}_p")
        elif kind == "ref":
            fixeds.append(f"fixed ({raw} {pname.lstrip('@')}_p = &{pname})")
            args.append(f"{pname.lstrip('@')}_p")
        else:
            args.append(pname)
    call = f"SteamFlat.{flat}({', '.join(args)})"
    if ret_kind == "string":
        call = f"SteamText.Read({call})"
    for line in pre:
        w.append(indent + line)
    for line in fixeds:
        w.append(indent + line)
    inner = indent + ("    " if fixeds else "")
    if fixeds:
        w.append(indent + "{")
    w.append(inner + (f"return {call};" if ret_pub != "void" else f"{call};"))
    if fixeds:
        w.append(indent + "}")


OPERATORS = {"operator<": "IsLessThan", "operator==": "IsEqual", "operator=": "Assign"}


def emit_struct_method(w, types, struct, m):
    rpub, rraw, rkind = return_info(types, m)
    params = []
    for p in m["params"]:
        pub, raw, kind = param_info(types, p)
        params.append((ident(p["paramname"]), pub, raw, kind))
    add_flat(m["methodname_flat"], rraw, [f"{struct}*"] + [raw for (_, _, raw, _) in params])
    sig = ", ".join(f"{pub} {n}" for (n, pub, _, _) in params)
    w.append(f"    public {rpub} {OPERATORS.get(m['methodname'], m['methodname'])}({sig})")
    w.append("    {")
    w.append(f"        fixed ({struct}* self = &this)")
    w.append("        {")
    call_body(w, "            ", m["methodname_flat"], "self", params, rkind, rpub)
    w.append("        }")
    w.append("    }")


def emit_interfaces(api, types):
    w = []
    header(w)
    for i in api["interfaces"]:
        accessors = i.get("accessors", [])
        classes = [(a["name"].replace("_SteamAPI", ""), a["name_flat"], a["kind"]) for a in accessors] or [(i["classname"], None, None)]
        for (cls, accessor, kind) in classes:
            w.append(f"/// <summary>{i['classname']}" + (f" ({kind} accessor {accessor})" if accessor else ": each call names the object") + ".</summary>")
            w.append(f"public static unsafe partial class {cls}")
            w.append("{")
            if accessor:
                add_flat(accessor, "nint", [])
                w.append("    private static nint s_self;")
                w.append("    private static int s_epoch = -1;")
                w.append("")
                w.append("    /// <summary>The interface's object (Steam must be running: <see cref=\"SteamLibrary.IsRunning\"/>).</summary>")
                w.append("    public static nint Self")
                w.append("    {")
                w.append("        get")
                w.append("        {")
                w.append("            if (s_epoch != SteamLibrary.Epoch)")
                w.append("            {")
                w.append(f"                s_self = SteamLibrary.Ready(\"{cls}\") ? SteamFlat.{accessor}() : 0;")
                w.append("                s_epoch = SteamLibrary.Epoch;")
                w.append("            }")
                w.append(f"            return s_self != 0 ? s_self : throw new InvalidOperationException(\"Steam: {cls} isn't available (Steam not running?)\");")
                w.append("        }")
                w.append("    }")
                w.append("")
            for m in i["methods"]:
                rpub, rraw, rkind = return_info(types, m)
                params = []
                for p in m["params"]:
                    pub, raw, kindp = param_info(types, p)
                    params.append((ident(p["paramname"]), pub, raw, kindp))
                add_flat(m["methodname_flat"], rraw, ["nint"] + [raw for (_, _, raw, _) in params])
                sig_parts = ([] if accessor else ["nint self"]) + [f"{pub} {n}" for (n, pub, _, _) in params]
                w.append(f"    public static {rpub} {m['methodname']}({', '.join(sig_parts)})")
                w.append("    {")
                call_body(w, "        ", m["methodname_flat"], "Self" if accessor else "self", params, rkind, rpub)
                w.append("    }")
                if m.get("callresult") and rraw == "ulong":
                    args = ([] if accessor else ["self"]) + [("in " if pub.startswith("in ") else "ref " if pub.startswith("ref ") else "") + n for (n, pub, _, _) in params]
                    w.append(f"    /// <summary>{m['methodname']}, its result when it comes (<see cref=\"{m['callresult']}\"/>).</summary>")
                    w.append(f"    public static Task<{m['callresult']}> {m['methodname']}Async({', '.join(sig_parts)})")
                    w.append(f"        => SteamCallbacks.Result<{m['callresult']}>({m['methodname']}({', '.join(args)}));")
                w.append("")
            w.append("}")
            w.append("")
    return w


def emit_flat():
    w = []
    header(w)
    w.append("/// <summary>steam_api64.dll's flat functions, as they are (looked up when first called).</summary>")
    w.append("public static unsafe class SteamFlat")
    w.append("{")
    for name in sorted(FLAT):
        ret, params = FLAT[name]
        fp = "delegate* unmanaged[Cdecl]<" + ", ".join(params + [ret]) + ">"
        args = ", ".join(f"{t} a{k}" for k, t in enumerate(params))
        call = ", ".join(f"a{k}" for k in range(len(params)))
        w.append(f"    private static {fp} p_{name};")
        w.append(f"    public static {ret} {name}({args})")
        w.append(f"        => (p_{name} != null ? p_{name} : (p_{name} = ({fp})SteamLibrary.Proc(\"{name}\")))({call});")
    w.append("}")
    return w


def main():
    with open(os.path.join(SDK, "steam_api.json"), encoding="utf-8") as f:
        api = json.load(f)
    types = Types(api)
    consts = probe(api, types)
    warnings = []
    os.makedirs(OUT, exist_ok=True)
    files = {
        "SteamTypes.g.cs": emit_types(api, types, consts, warnings),
        "SteamInterfaces.g.cs": emit_interfaces(api, types),
    }
    files["SteamFlat.g.cs"] = emit_flat()
    for name, lines in files.items():
        with open(os.path.join(OUT, name), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
    for line in warnings:
        print("steamgen:", line)
    print(f"steamgen: {len(types.structs)} structs, {len(FLAT)} functions -> {OUT}")


if __name__ == "__main__":
    main()
