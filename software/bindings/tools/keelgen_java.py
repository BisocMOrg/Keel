# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""The Java emitter of keelgen.py: the header-derived sources of io.github.bisocmorg.keel.

keelgen.py parses keel.h and camera.h into one model each and calls
outputs(model, notice, sentinel, packages) for each. For keel.h:

- one enum per value or flag enum of keel.h (23), and the constant classes Abi
  and Limits;
- the data records OpenReport, Fault, Snapshot, ButtonState, RangeMeasurement,
  SimState and CycleInput (OpenOptions is hand-written: a record with a
  builder);
- internal/Layouts.java (the struct layouts and function descriptors),
  internal/StructCodec.java (conversion between struct memory and the records)
  and internal/HeaderVersion.java (the header's version, Keel.VERSION).

For camera.h, under io.github.bisocmorg.keel.camera and its subpackages:

- one enum per value or flag enum of camera.h (15), and the constant classes
  CameraAbi and CameraLimits;
- the data records of its output structs (ErrorReport, CameraInfo, SensorMode,
  StreamInfo, FrameInfo, ChunkInfo, OutputStats, CameraStats, StillInfo; the
  option records are hand-written, with builders);
- internal/camera/CameraLayouts.java and internal/camera/CameraStructCodec.java.

keelgen's package tables place each enum and record in its package. A
generated file imports every type of another package that its code or its
Javadoc links name. Every generated member carries the header's doc comment,
turned into Javadoc that passes doclint.

checks(model, notice, sentinel, packages) compares the pom.xml version with the
header's KEEL_VERSION_* macros and reports every source that carries the
generator notice but is not an output, such as the file a type leaves behind
when it moves to another package.
"""

import os
import re
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
JAVA = os.path.normpath(os.path.join(HERE, "..", "java"))
PACKAGE = "io.github.bisocmorg.keel"
INTERNAL = PACKAGE + ".internal"
SOURCES = os.path.join(JAVA, "src", "main", "java", *PACKAGE.split("."))
LINE_LIMIT = 120

# Structs whose Java data type is hand-written (records with builders); only their layouts and writers are generated.
HAND_WRITTEN = {"keel_open_options", "keel_camera_options", "keel_camera_encoder_options",
                "keel_camera_output_options", "keel_camera_still_options"}

# Field kinds of keelgen's STRUCTS table that are not enums.
SIZE, INT, PATH, TEXT, RESERVED = "struct_size", "int", "path", "text", "reserved"

# Derived conveniences of the data records: (paragraphs, block tags, code).
EXTRAS = {
    "keel_fault": [
        (["Whether a fault was ever recorded (the status is not {@link Status#OK})."],
         ["@return {@code true} when this record describes a fault"],
         "public boolean recorded() {\n    return statusValue != 0;\n}"),
        (["Whether the cause is sticky: {@code keel_clear_fault()} refuses it until the device is closed and "
          "reopened."],
         ["@return {@code true} when {@link FaultFlag#STICKY} is set"],
         "public boolean sticky() {\n    return (flagsValue & FaultFlag.STICKY.mask()) != 0;\n}"),
        (["Whether {@code keel_clear_fault()} has acknowledged this record."],
         ["@return {@code true} when {@link FaultFlag#CLEARED} is set"],
         "public boolean cleared() {\n    return (flagsValue & FaultFlag.CLEARED.mask()) != 0;\n}"),
    ],
    "keel_range": [
        (["Whether this result carries a distance. No status means \"path clear\"."],
         ["@return {@code true} when the status is {@link RangeStatus#VALID}"],
         "public boolean valid() {\n    return statusValue == RangeStatus.VALID.code();\n}"),
    ],
    "keel_camera_info": [
        (["Whether the camera has an autofocus lens (Camera Module 3)."],
         ["@return {@code true} when {@link CameraInfoFlag#AUTOFOCUS} is set"],
         "public boolean autofocus() {\n    return (flagsValue & CameraInfoFlag.AUTOFOCUS.mask()) != 0;\n}"),
        (["Whether another process held the camera while it was listed, so its modes have no frame rates."],
         ["@return {@code true} when {@link CameraInfoFlag#IN_USE} is set"],
         "public boolean inUse() {\n    return (flagsValue & CameraInfoFlag.IN_USE.mask()) != 0;\n}"),
    ],
    "keel_camera_mode": [
        (["The highest frame rate in frames per second."],
         ["@return {@code maxFramerateMilli / 1000}; 0 when unknown"],
         "public double maxFramerate() {\n    return maxFramerateMilli / 1000.0;\n}"),
    ],
    "keel_camera_stream_info": [
        (["The requested frame rate in frames per second."],
         ["@return {@code framerateMilli / 1000}"],
         "public double framerate() {\n    return framerateMilli / 1000.0;\n}"),
    ],
    "keel_camera_encoded_info": [
        (["Whether the chunk decodes on its own: an H.264 IDR picture, or any MJPEG image."],
         ["@return {@code true} when {@link ChunkFlag#KEYFRAME} is set"],
         "public boolean keyframe() {\n    return (flagsValue & ChunkFlag.KEYFRAME.mask()) != 0;\n}"),
        (["Whether the chunk carries the H.264 sequence and picture parameter sets."],
         ["@return {@code true} when {@link ChunkFlag#HEADERS} is set"],
         "public boolean headers() {\n    return (flagsValue & ChunkFlag.HEADERS.mask()) != 0;\n}"),
    ],
    "keel_camera_still_info": [
        (["Whether libjpeg encoded the still, because it is larger than the hardware encoder takes."],
         ["@return {@code true} when {@link StillFlag#SOFTWARE} is set"],
         "public boolean software() {\n    return (flagsValue & StillFlag.SOFTWARE.mask()) != 0;\n}"),
    ],
}


class Layer:
    """Where the private sources of one header go, and what they say about it."""

    def __init__(self, package, layouts, codec, library, layout_file, version):
        self.package = package
        self.layouts = layouts
        self.codec = codec
        self.library = library
        self.layout_file = layout_file
        self.version = version

    def directory(self):
        return os.path.join(SOURCES, *self.package[len(PACKAGE) + 1:].split("."))


LAYERS = {
    "keel": Layer(INTERNAL, "Layouts", "StructCodec", "libkeel", "layout.c", "HeaderVersion"),
    "camera": Layer(INTERNAL + ".camera", "CameraLayouts", "CameraStructCodec", "libkeel-camera",
                    "camera_layout.c", None),
}
SONAMES = {"libkeel": "libkeel.so.1", "libkeel-camera": "libkeel-camera.so.1"}


class Packages:
    """The package of every generated type, from keelgen's PACKAGES table ("" is the root package)."""

    def __init__(self, table):
        self.table = table

    def of(self, type_name):
        sub = self.table[type_name]
        return PACKAGE + "." + sub if sub else PACKAGE

    def directory(self, type_name):
        sub = self.table[type_name]
        return os.path.join(SOURCES, *sub.split(".")) if sub else SOURCES

    def imports(self, names, package):
        """The imports a file of package needs for the named types."""
        return {"%s.%s" % (self.of(name), name) for name in names if self.of(name) != package}

    def named(self, text):
        """The generated types a piece of hand-written Java (an EXTRAS entry) names."""
        return {name for name in re.findall(r"\b[A-Z]\w*\b", text) if name in self.table}


def camel(name, upper=False):
    parts = name.split("_")
    text = parts[0] + "".join(part.capitalize() for part in parts[1:])
    return text[0].upper() + text[1:] if upper else text


# ------------------------------------------------------------------ Javadoc ----
class Docs:
    """Doxygen text of keel.h as Javadoc HTML that passes doclint."""

    def __init__(self, model):
        self.links = {}
        self.linked = set()
        for enum in list(model.foreign.values()) + list(model.enums.values()):
            self.links[enum.name] = enum.type_name
            for enumerator in enum.enumerators:
                self.links[enumerator.name] = "%s#%s" % (enum.type_name, enumerator.member)

    def take_linked(self):
        """The types {@link} tags named since the last call, which the file must import from other packages."""
        linked, self.linked = self.linked, set()
        return linked

    def paragraphs(self, text, here=None):
        """The paragraphs of a doc comment body; here is the Java type the comment belongs to."""
        text = text.replace("@brief", "")
        parts = [" ".join(part.split()) for part in re.split(r"\n\s*\n", text)]
        return [self.convert(part, here) for part in parts if part]

    def sentence(self, text, here=None):
        return " ".join(self.paragraphs(text, here))

    def convert(self, text, here):
        tokens = []  # Javadoc markup, kept out of the HTML escaping below

        def keep(markup):
            tokens.append(markup)
            return "\0%d\0" % (len(tokens) - 1)

        def code(value):
            return keep("{@code %s}" % value)

        def link(match):
            target = self.links.get(match.group(1))
            if target is None:
                return code(match.group(1))
            if here and target.startswith(here + "#"):
                target = target[len(here):]
            else:
                self.linked.add(target.split("#")[0])
            return keep("{@link %s}" % target)

        text = text.replace("\\#", "#")
        text = re.sub(r"<tt>(.*?)</tt>", lambda m: code(m.group(1)), text)
        text = re.sub(r"\b(keel_[a-z_]+)\((@p \w+(?:, \w+)*)\)",
                      lambda m: code("%s(%s)" % (m.group(1), m.group(2).replace("@p ", ""))), text)
        text = re.sub(r"@[cp]\s+([\w.\->]*\w)", lambda m: code(m.group(1)), text)
        text = re.sub(r"@e\s+(\w+)", lambda m: keep("<em>%s</em>" % m.group(1)), text)
        text = re.sub(r"\b(keel_[a-z_]+)::(\w+)", lambda m: code("%s.%s" % m.groups()), text)
        text = re.sub(r"(?<![\w:])::(KEEL_\w+|keel_[a-z_0-9]+)\b", link, text)
        text = re.sub(r"\b(keel_[a-z_]+\(\))", lambda m: code(m.group(1)), text)
        text = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("@", "&#64;")
        return re.sub(r"\0(\d+)\0", lambda m: tokens[int(m.group(1))], text)


def javadoc(paragraphs, indent, tags=()):
    """A Javadoc block of paragraphs (HTML) and block tags."""
    prefix = " " * indent
    width = LINE_LIMIT - indent - 3
    lines = [prefix + "/**"]
    for number, paragraph in enumerate(paragraphs):
        if number:
            lines.append(prefix + " *")
            paragraph = "<p>" + paragraph
        for line in textwrap.wrap(paragraph, width, break_long_words=False, break_on_hyphens=False):
            lines.append(prefix + " * " + line)
    if tags:
        if paragraphs:
            lines.append(prefix + " *")
        for tag in tags:
            for line in textwrap.wrap(tag, width, break_long_words=False, break_on_hyphens=False,
                                      subsequent_indent="    "):
                lines.append(prefix + " * " + line)
    lines.append(prefix + " */")
    return lines


def short_javadoc(paragraphs, indent):
    """A one-line Javadoc comment when it fits, else a block."""
    line = " " * indent + "/** " + " ".join(paragraphs) + " */"
    if len(paragraphs) == 1 and len(line) <= LINE_LIMIT:
        return [line]
    return javadoc(paragraphs, indent)


def wrap_signature(start, parameters, end):
    """A declaration's lines: parameters joined on the first line, continued with an 8-space indent."""
    lines, line = [], start
    for number, parameter in enumerate(parameters):
        text = parameter + (end if number == len(parameters) - 1 else ",")
        candidate = line + ("" if line.endswith("(") else " ") + text
        if len(candidate) > LINE_LIMIT and not line.endswith("("):
            lines.append(line)
            line = "            " + text
        else:
            line = candidate
    lines.append(line)
    return lines


def header(notice, package, imports=(), static_imports=()):
    lines = ["// SPDX-License-Identifier: MIT", "// Copyright (c) 2026 BisocM", "// " + notice,
             "package %s;" % package, ""]
    if static_imports:
        lines += ["import static %s;" % name for name in sorted(static_imports)] + [""]
    if imports:
        lines += ["import %s;" % name for name in sorted(imports)] + [""]
    return lines


def source(notice, package, body, imports=(), static_imports=()):
    """A whole source file: the header with its imports, then the body lines."""
    return "\n".join(header(notice, package, imports, static_imports) + body) + "\n"


# -------------------------------------------------------------------- enums ----
def value_enum(enum, docs, notice, sentinel, packages):
    here = enum.type_name
    package = packages.of(here)
    lines = javadoc(docs.paragraphs(enum.doc, here) + [
        "Generated from {@code enum %s}. {@link #of(int)} returns the constant of a value and {@link #%s} "
        "for a value this binding does not know, for example one a newer library added; the raw value stays "
        "available wherever the value came from." % (enum.name, sentinel)], 0)
    lines.append("public enum %s {" % here)
    for enumerator in enum.enumerators:
        lines += short_javadoc(docs.paragraphs(enumerator.doc, here), 4)
        lines.append("    %s(%d)," % (enumerator.member, enumerator.value))
        lines.append("")
    lines += javadoc(["A value this binding does not know, for example one a newer library added. {@link #code()} "
                      "throws for it; the raw value stays available wherever the value came from."], 4)
    lines += ["    %s(0);" % sentinel, "",
              "    private final int code;", "",
              "    %s(int code) {" % here,
              "        this.code = code;",
              "    }", ""]
    lines += javadoc(["Returns the C value of this constant."], 4, [
        "@return the value of the {@code %s} enumerator" % enum.name,
        "@throws IllegalStateException for {@link #%s}, which has no C value" % sentinel])
    lines += ["    public int code() {",
              "        if (this == %s) {" % sentinel,
              '            throw new IllegalStateException("%s.%s has no C value");' % (here, sentinel),
              "        }",
              "        return code;",
              "    }", ""]
    lines += javadoc(["Returns the constant of a C value."], 4, [
        "@param code a {@code %s} value" % enum.name,
        "@return the constant, or {@link #%s} for a value this binding does not know" % sentinel])
    lines += ["    public static %s of(int code) {" % here,
              "        return switch (code) {"]
    for enumerator in enum.enumerators:
        lines.append("            case %d -> %s;" % (enumerator.value, enumerator.member))
    lines += ["            default -> %s;" % sentinel,
              "        };",
              "    }",
              "}"]
    return source(notice, package, lines, packages.imports(docs.take_linked(), package))


def flag_enum(enum, docs, notice, packages):
    here = enum.type_name
    package = packages.of(here)
    composite = [e.member for e in enum.enumerators if bin(e.value).count("1") != 1]
    note = ("Generated from {@code enum %s}. {@link #maskOf(%s...)} encodes members as a mask and {@link "
            "#setOf(int)} decodes a mask into its single-bit members. Bits this binding does not know are left out "
            "of a decoded set and kept in every raw mask." % (enum.name, here))
    if composite:
        note += " %s can be encoded but %s never decoded." % (
            " and ".join("{@link #%s}" % member for member in composite), "are" if len(composite) > 1 else "is")
    lines = javadoc(docs.paragraphs(enum.doc, here) + [note], 0)
    lines.append("public enum %s {" % here)
    for number, enumerator in enumerate(enum.enumerators):
        lines += short_javadoc(docs.paragraphs(enumerator.doc, here), 4)
        end = ";" if number == len(enum.enumerators) - 1 else ","
        lines.append("    %s(%d)%s" % (enumerator.member, enumerator.value, end))
        lines.append("")
    lines += ["    private final int mask;", "",
              "    %s(int mask) {" % here,
              "        this.mask = mask;",
              "    }", ""]
    lines += javadoc(["Returns the bits of this member."], 4, ["@return the value of the {@code %s} enumerator"
                                                                % enum.name])
    lines += ["    public int mask() {",
              "        return mask;",
              "    }", ""]
    lines += javadoc(["Encodes members as a mask."], 4, ["@param members the members to combine",
                                                         "@return the bitwise OR of their masks; 0 for none"])
    lines += ["    public static int maskOf(%s... members) {" % here,
              "        int result = 0;",
              "        for (%s member : members) {" % here,
              "            result |= member.mask;",
              "        }",
              "        return result;",
              "    }", ""]
    lines += javadoc(["Decodes a mask into its single-bit members."], 4, [
        "@param mask a {@code %s} mask" % enum.name,
        "@return an unmodifiable set of the single-bit members whose bit is set"])
    lines += ["    public static Set<%s> setOf(int mask) {" % here,
              "        EnumSet<%s> result = EnumSet.noneOf(%s.class);" % (here, here),
              "        for (%s member : values()) {" % here,
              "            if (Integer.bitCount(member.mask) == 1 && (mask & member.mask) != 0) {",
              "                result.add(member);",
              "            }",
              "        }",
              "        return Collections.unmodifiableSet(result);",
              "    }",
              "}"]
    imports = packages.imports(docs.take_linked(), package)
    imports |= {"java.util.Collections", "java.util.EnumSet", "java.util.Set"}
    return source(notice, package, lines, imports)


def constants(enum, docs, notice, packages):
    here = enum.type_name
    package = packages.of(here)
    lines = javadoc(docs.paragraphs(enum.doc, here) + [
        "Generated from {@code enum %s}: the values of the header this binding was generated from." % enum.name], 0)
    lines += ["public final class %s {" % here,
              "    private %s() {" % here,
              "    }"]
    for enumerator in enum.enumerators:
        lines.append("")
        lines += short_javadoc(docs.paragraphs(enumerator.doc, here), 4)
        lines.append("    public static final int %s = %d;" % (enumerator.member, enumerator.value))
    lines.append("}")
    return source(notice, package, lines, packages.imports(docs.take_linked(), package))


# ------------------------------------------------------------------ records ----
def exposed(struct):
    """The fields a record carries: every field but struct_size and the reserved ones."""
    return [field for field in struct.fields if field.binding not in (SIZE, RESERVED)]


def is_enum(field, model):
    return model.enum_of(field.binding) is not None


def component(field, model):
    """(Java type, component name) of a field."""
    if field.binding in (PATH, TEXT):
        return "String", camel(field.name)
    if is_enum(field, model):
        return "int", camel(field.name) + "Value"
    return {"uint32_t": "long", "int32_t": "int", "uint64_t": "long"}[field.ctype], camel(field.name)


def record(struct, model, docs, notice, packages):
    here = struct.type_name
    package = packages.of(here)
    fields = exposed(struct)
    imports = {"java.io.Serializable"}
    used = {model.enum_of(f.binding).type_name for f in fields if is_enum(f, model)}
    if any(is_enum(f, model) and model.enum_of(f.binding).kind == "flags" for f in fields):
        imports.add("java.util.Set")
    tags = []
    for field in fields:
        java_type, name = component(field, model)
        text = docs.sentence(field.doc, here)
        if is_enum(field, model):
            kind = "value" if model.enum_of(field.binding).kind == "value" else "mask"
            text = "raw {@code %s} %s of {@code %s}: %s" % (field.binding, kind, field.name, text)
        elif field.ctype == "uint32_t":
            text = "{@code %s} ({@code uint32_t}, never negative): %s" % (field.name, text)
        elif field.ctype == "uint64_t":
            text = "{@code %s} ({@code uint64_t} in two's complement): %s" % (field.name, text)
        elif field.ctype == "char":
            text = "{@code %s}, decoded up to the first NUL: %s" % (field.name, text)
        else:
            text = "{@code %s}: %s" % (field.name, text)
        tags.append("@param %s %s" % (name, text))
    note = ("Generated from {@code %s} (%d bytes): one component per field, in C order, except {@code struct_size}, "
            "which the binding sets, and the reserved fields." % (struct.name, struct.size))
    if any(is_enum(f, model) for f in fields):
        note += (" A value or flag field {@code x} is the raw component {@code xValue} plus a typed accessor {@code "
                 "x()}, so a value this binding does not know is never lost.")
    lines = javadoc(docs.paragraphs(struct.doc, here) + [note], 0, tags)
    components = ["%s %s" % component(field, model) for field in fields]
    one_line = "public record %s(%s) implements Serializable {" % (here, ", ".join(components))
    if len(one_line) <= LINE_LIMIT:
        lines.append(one_line)
    else:
        lines.append("public record %s(" % here)
        for number, text in enumerate(components):
            end = ") implements Serializable {" if number == len(components) - 1 else ","
            lines.append("        " + text + end)
    for field in fields:
        if not is_enum(field, model):
            continue
        enum = model.enum_of(field.binding)
        name = camel(field.name)
        lines.append("")
        if enum.kind == "value":
            lines += javadoc(["Returns the typed {@code %s} field." % field.name], 4, [
                "@return the constant, or {@link %s#UNRECOGNIZED} for a value this binding does not know"
                % enum.type_name])
            lines += ["    public %s %s() {" % (enum.type_name, name),
                      "        return %s.of(%sValue);" % (enum.type_name, name),
                      "    }"]
        else:
            lines += javadoc(["Returns the typed {@code %s} field." % field.name], 4, [
                "@return the single-bit members that are set; {@link #%sValue()} keeps every bit" % name])
            lines += ["    public Set<%s> %s() {" % (enum.type_name, name),
                      "        return %s.setOf(%sValue);" % (enum.type_name, name),
                      "    }"]
    for paragraphs, block_tags, code in EXTRAS.get(struct.name, []):
        lines.append("")
        lines += javadoc(paragraphs, 4, block_tags)
        lines += ["    " + line for line in code.split("\n")]
        used |= packages.named(" ".join(paragraphs + block_tags + [code]))
    lines.append("}")
    return source(notice, package, lines, imports | packages.imports(used | docs.take_linked(), package))


# ------------------------------------------------------------------ internal ----
def layout_name(struct):
    return struct.name[len("keel_"):].upper()


def value_layout(ctype):
    return {"uint32_t": "JAVA_INT", "int32_t": "JAVA_INT", "uint64_t": "JAVA_LONG"}[ctype]


def argument_layout(param):
    return "ADDRESS" if param.pointers else value_layout(param.base)


def c_declaration(function):
    def spell(param):
        text = ("const " if param.const else "") + param.base
        return text + (" " + "*" * param.pointers if param.pointers else "")
    params = ", ".join(spell(p) + ("" if p.pointers else " ") + p.name for p in function.params)
    result = spell(function.result)
    return "%s%s%s(%s)" % (result, "" if result.endswith("*") else " ", function.name, params or "void")


def layouts(model, notice, layer):
    lines = javadoc([
        "The memory layouts of the %s structs and the descriptors of the %s functions." % (layer.library,
                                                                                            layer.library),
        "Every struct is explicitly padded, so each layout is its fields in order. Element names are the C field "
        "names, which the tests compare with the offsets that {@code tests/abi/%s} pins." % layer.layout_file], 0)
    lines += ["public final class %s {" % layer.layouts,
              "    private %s() {" % layer.layouts,
              "    }"]
    for struct in model.structs.values():
        lines.append("")
        lines += short_javadoc(["Layout of {@code %s} (%d bytes)." % (struct.name, struct.size)], 4)
        lines.append("    public static final StructLayout %s = MemoryLayout.structLayout(" % layout_name(struct))
        for number, field in enumerate(struct.fields):
            element = ("MemoryLayout.sequenceLayout(%d, JAVA_BYTE)" % field.length if field.ctype == "char"
                       else value_layout(field.ctype))
            end = ').withName("%s");' % struct.name if number == len(struct.fields) - 1 else ","
            lines.append('            %s.withName("%s")%s' % (element, field.name, end))
    for function in model.functions.values():
        lines.append("")
        lines += short_javadoc(["Descriptor of {@code %s}." % c_declaration(function)], 4)
        arguments = [argument_layout(param) for param in function.params]
        if function.result.base == "void" and not function.result.pointers:
            call = "FunctionDescriptor.ofVoid(%s)" % ", ".join(arguments)
        else:
            call = "FunctionDescriptor.of(%s)" % ", ".join([argument_layout(function.result)] + arguments)
        declaration = "    public static final FunctionDescriptor %s = %s;" % (function.name.upper(), call)
        if len(declaration) <= LINE_LIMIT:
            lines.append(declaration)
        else:
            lines.append("    public static final FunctionDescriptor %s =" % function.name.upper())
            lines.append("            %s;" % call)
    lines.append("")
    lines += javadoc(["The exported functions of {@code %s}, in header order." % SONAMES[layer.library]], 4)
    lines.append("    public static final List<Signature> FUNCTIONS = List.of(")
    names = list(model.functions)
    for number, name in enumerate(names):
        end = ");" if number == len(names) - 1 else ","
        lines.append('            new Signature("%s", %s)%s' % (name, name.upper(), end))
    lines.append("")
    lines += javadoc(["One exported function."], 4, ["@param name the C name", "@param descriptor its descriptor"])
    lines += ["    public record Signature(String name, FunctionDescriptor descriptor) {",
              "    }",
              "}"]
    return source(notice, layer.package, lines,
                  ["java.lang.foreign.FunctionDescriptor", "java.lang.foreign.MemoryLayout",
                   "java.lang.foreign.StructLayout", "java.util.List"],
                  ["java.lang.foreign.ValueLayout.ADDRESS", "java.lang.foreign.ValueLayout.JAVA_BYTE",
                   "java.lang.foreign.ValueLayout.JAVA_INT", "java.lang.foreign.ValueLayout.JAVA_LONG"])


def read_expression(field, model):
    if field.ctype == "char":
        return "string(segment, %d, %d)" % (field.offset, field.length)
    if field.ctype == "uint64_t":
        return "segment.get(JAVA_LONG, %d)" % field.offset
    if field.ctype == "uint32_t" and not is_enum(field, model):
        return "Integer.toUnsignedLong(segment.get(JAVA_INT, %d))" % field.offset
    return "segment.get(JAVA_INT, %d)" % field.offset


def struct_codec(model, notice, packages, layer):
    records = [s for s in model.structs.values() if s.name not in HAND_WRITTEN]
    imports = {"java.lang.foreign.Arena", "java.lang.foreign.MemorySegment", "java.lang.foreign.StructLayout",
               "java.nio.charset.StandardCharsets"} | packages.imports([s.type_name for s in records], layer.package)
    lines = javadoc([
        "Conversion between struct memory and the binding's data records.",
        "The offsets are those of the frozen 1.x layout, which {@link %s} describes. A struct the binding "
        "passes has {@code struct_size} set to the size the binding was generated against and every other byte "
        "zero; {@link #allocate(Arena, StructLayout)} returns such a struct." % layer.layouts], 0)
    lines += ["public final class %s {" % layer.codec,
              "    private %s() {" % layer.codec,
              "    }", ""]
    lines += javadoc(["Allocates a zeroed struct whose {@code struct_size} is the size of its layout."], 4, [
        "@param arena the arena that owns the memory",
        "@param layout one of the struct layouts of {@link %s}" % layer.layouts,
        "@return the struct"])
    lines += ["    public static MemorySegment allocate(Arena arena, StructLayout layout) {",
              "        MemorySegment segment = arena.allocate(layout);",
              "        segment.set(JAVA_INT, 0, (int) layout.byteSize());",
              "        return segment;",
              "    }"]
    for struct in records:
        fields = exposed(struct)
        lines.append("")
        lines += javadoc(["Reads a {@code %s}." % struct.name], 4, [
            "@param segment the struct, at least %d bytes" % struct.size, "@return its fields"])
        lines += ["    public static %s %s(MemorySegment segment) {" % (struct.type_name,
                                                                         camel(struct.name[len("keel_"):])),
                  "        return new %s(" % struct.type_name]
        for number, field in enumerate(fields):
            end = ");" if number == len(fields) - 1 else ","
            lines.append("                %s%s" % (read_expression(field, model), end))
        lines.append("    }")
    for struct in model.structs.values():
        if struct.direction != "in":
            continue
        fields = [f for f in struct.fields if f.binding not in (SIZE, RESERVED)]
        lines.append("")
        if struct.name in HAND_WRITTEN:
            params, tags = [], ["@param segment the struct, %d bytes, zeroed" % struct.size]
            for field in fields:
                name = camel(field.name)
                if field.ctype == "char":
                    params.append("byte[] " + name)
                    tags.append("@param %s the encoded {@code %s} without its NUL, at most %d bytes"
                                % (name, field.name, field.length - 1))
                else:
                    params.append(("long " if field.ctype == "uint64_t" else "int ") + name)
                    tags.append("@param %s {@code %s} as its C bit pattern" % (name, field.name))
            tags.append("@throws IllegalArgumentException if a string does not fit with its NUL")
            lines += javadoc(["Writes the fields of a {@code %s}; {@code struct_size} stays as it is." % struct.name],
                             4, tags)
            lines += wrap_signature("    public static void write%s(" % struct.type_name,
                                    ["MemorySegment segment"] + params, ") {")
            for field in fields:
                if field.ctype == "char":
                    lines.append("        putString(segment, %d, %d, %s);" % (field.offset, field.length,
                                                                            camel(field.name)))
                else:
                    lines.append("        segment.set(%s, %d, %s);" % (value_layout(field.ctype), field.offset,
                                                                     camel(field.name)))
        else:
            lines += javadoc(["Writes the fields of a {@code %s}; {@code struct_size} stays as it is." % struct.name],
                             4, ["@param segment the struct, %d bytes" % struct.size,
                                 "@param input the field values"])
            lines.append("    public static void write%s(MemorySegment segment, %s input) {"
                         % (struct.type_name, struct.type_name))
            for field in fields:
                java_type, name = component(field, model)
                value = "input.%s()" % name
                if java_type == "long" and field.ctype == "uint32_t":
                    value = "(int) " + value
                lines.append("        segment.set(%s, %d, %s);" % (value_layout(field.ctype), field.offset, value))
        lines.append("    }")
    lines += ["",
              "    private static String string(MemorySegment segment, long offset, int size) {",
              "        int length = 0;",
              "        while (length < size && segment.get(JAVA_BYTE, offset + length) != 0) {",
              "            length++;",
              "        }",
              "        byte[] bytes = new byte[length];",
              "        MemorySegment.copy(segment, JAVA_BYTE, offset, bytes, 0, length);",
              "        return new String(bytes, StandardCharsets.UTF_8);",
              "    }",
              "",
              "    private static void putString(MemorySegment segment, long offset, int size, byte[] bytes) {",
              "        if (bytes.length >= size) {",
              '            throw new IllegalArgumentException("a string of " + bytes.length + " bytes does not fit in a "',
              '                    + size + "-byte field with its NUL");',
              "        }",
              "        MemorySegment.copy(bytes, 0, segment, JAVA_BYTE, offset, bytes.length);",
              "        segment.set(JAVA_BYTE, offset + bytes.length, (byte) 0);",
              "    }",
              "}"]
    return source(notice, layer.package, lines, imports,
                  ["java.lang.foreign.ValueLayout.JAVA_BYTE", "java.lang.foreign.ValueLayout.JAVA_INT",
                   "java.lang.foreign.ValueLayout.JAVA_LONG"])


def header_version(model, notice):
    lines = javadoc(["The version of the header this binding was generated from."], 0)
    lines += ["public final class HeaderVersion {",
              "    private HeaderVersion() {",
              "    }", ""]
    lines += short_javadoc(["{@code KEEL_VERSION_MAJOR.KEEL_VERSION_MINOR.KEEL_VERSION_PATCH} of {@code keel.h}."],
                           4)
    lines += ['    public static final String TEXT = "%d.%d.%d";' % model.version,
              "}"]
    return source(notice, INTERNAL, lines)


# --------------------------------------------------------------------- entry ----
def outputs(model, notice, sentinel, table):
    """{absolute path: text} of every generated Java source; table is keelgen's PACKAGES."""
    docs = Docs(model)
    packages = Packages(table)
    files = {}
    for enum in model.enums.values():
        if enum.kind == "constants":
            text = constants(enum, docs, notice, packages)
        elif enum.kind == "flags":
            text = flag_enum(enum, docs, notice, packages)
        else:
            text = value_enum(enum, docs, notice, sentinel, packages)
        files[os.path.join(packages.directory(enum.type_name), enum.type_name + ".java")] = text
    for struct in model.structs.values():
        if struct.name not in HAND_WRITTEN:
            files[os.path.join(packages.directory(struct.type_name), struct.type_name + ".java")] = \
                record(struct, model, docs, notice, packages)
    layer = LAYERS[model.spec.name]
    files[os.path.join(layer.directory(), layer.layouts + ".java")] = layouts(model, notice, layer)
    files[os.path.join(layer.directory(), layer.codec + ".java")] = struct_codec(model, notice, packages, layer)
    if layer.version:
        files[os.path.join(layer.directory(), layer.version + ".java")] = header_version(model, notice)
    return files


def version_check(model):
    """The project version in java/pom.xml equals keel.h's version; camera.h versions only the camera ABI."""
    if model.spec.name != "keel":
        return []
    version = "%d.%d.%d" % model.version
    with open(os.path.join(JAVA, "pom.xml"), encoding="utf-8") as handle:
        match = re.search(r"<artifactId>keel</artifactId>\s*<version>([^<]+)</version>", handle.read())
    if not match or match.group(1) != version:
        return ["java/pom.xml has version %s; keel.h says %s" % (match.group(1) if match else "none", version)]
    return []


def stale_sources(model, notice, sentinel, table):
    """Sources under src/main/java that carry the generator notice but that keelgen does not write."""
    written = {os.path.normpath(path) for path in outputs(model, notice, sentinel, table)}
    problems = []
    for directory, _, names in os.walk(SOURCES):
        for name in sorted(names):
            path = os.path.normpath(os.path.join(directory, name))
            if not name.endswith(".java") or path in written:
                continue
            with open(path, encoding="utf-8") as handle:
                if notice in handle.read():
                    relative = os.path.relpath(path, os.path.dirname(JAVA)).replace(os.sep, "/")
                    problems.append("%s carries the generator notice, but keelgen does not write it; delete it"
                                    % relative)
    return problems


def checks(model, notice, sentinel, table):
    """Every problem of the Java sources other than a difference in a generated file."""
    return version_check(model) + stale_sources(model, notice, sentinel, table)
