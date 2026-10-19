"""Build-time C ABI extraction through Clang's original type/layout API."""
import ctypes as C
import json
import os
import pathlib
import subprocess
import sys


class String(C.Structure):
    _fields_ = [("data", C.c_void_p), ("flags", C.c_uint)]


class Type(C.Structure):
    _fields_ = [("kind", C.c_int), ("data", C.c_void_p * 2)]


class Cursor(C.Structure):
    _fields_ = [("kind", C.c_int), ("xdata", C.c_int), ("data", C.c_void_p * 3)]


class Location(C.Structure):
    _fields_ = [("data", C.c_void_p * 2), ("value", C.c_uint)]


class Unsaved(C.Structure):
    _fields_ = [("name", C.c_char_p), ("contents", C.c_char_p), ("length", C.c_ulong)]


def extract(options):
    clang = pathlib.Path(subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip())
    library = C.CDLL(str(clang.parent.parent / "lib/libclang.dylib"))

    def function(name, result, *arguments):
        fn = getattr(library, "clang_" + name)
        fn.restype, fn.argtypes = result, list(arguments)
        return fn

    get_string = function("getCString", C.c_char_p, String)
    dispose_string = function("disposeString", None, String)

    def text(value):
        data = get_string(value)
        result = data.decode() if data else ""
        dispose_string(value)
        return result

    create_index = function("createIndex", C.c_void_p, C.c_int, C.c_int)
    dispose_index = function("disposeIndex", None, C.c_void_p)
    parse = function("parseTranslationUnit2", C.c_int, C.c_void_p, C.c_char_p, C.POINTER(C.c_char_p), C.c_int, C.POINTER(Unsaved), C.c_uint, C.c_uint, C.POINTER(C.c_void_p))
    dispose_tu = function("disposeTranslationUnit", None, C.c_void_p)
    root_cursor = function("getTranslationUnitCursor", Cursor, C.c_void_p)
    cursor_name = function("getCursorSpelling", String, Cursor)
    mangling = function("Cursor_getMangling", String, Cursor)
    cursor_kind = function("getCursorKindSpelling", String, C.c_uint)
    cursor_type = function("getCursorType", Type, Cursor)
    cursor_location = function("getCursorLocation", Location, Cursor)
    spelling_location = function("getSpellingLocation", None, Location, C.POINTER(C.c_void_p), C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint))
    file_name = function("getFileName", String, C.c_void_p)
    cursor_policy = function("getCursorPrintingPolicy", C.c_void_p, Cursor)
    cursor_printed = function("getCursorPrettyPrinted", String, Cursor, C.c_void_p)
    dispose_policy = function("PrintingPolicy_dispose", None, C.c_void_p)
    canonical = function("getCanonicalType", Type, Type)
    underlying_type = function("getTypedefDeclUnderlyingType", Type, Cursor)
    named_type = function("Type_getNamedType", Type, Type)
    modified_type = function("Type_getModifiedType", Type, Type)
    type_name = function("getTypeSpelling", String, Type)
    type_kind = function("getTypeKindSpelling", String, C.c_int)
    type_size = function("Type_getSizeOf", C.c_longlong, Type)
    type_align = function("Type_getAlignOf", C.c_longlong, Type)
    pointee = function("getPointeeType", Type, Type)
    is_const = function("isConstQualifiedType", C.c_uint, Type)
    declaration = function("getTypeDeclaration", Cursor, Type)
    result_type = function("getResultType", Type, Type)
    num_args = function("getNumArgTypes", C.c_int, Type)
    arg_type = function("getArgType", Type, Type, C.c_uint)
    is_variadic = function("isFunctionTypeVariadic", C.c_uint, Type)
    arg_cursor = function("Cursor_getArgument", Cursor, Cursor, C.c_uint)
    storage = function("Cursor_getStorageClass", C.c_uint, Cursor)
    enum_type = function("getEnumDeclIntegerType", Type, Cursor)
    array_element = function("getArrayElementType", Type, Type)
    array_size = function("getArraySize", C.c_longlong, Type)
    field_offset = function("Cursor_getOffsetOfField", C.c_longlong, Cursor)
    is_anonymous_record = function("Cursor_isAnonymousRecordDecl", C.c_uint, Cursor)
    is_bitfield = function("Cursor_isBitField", C.c_uint, Cursor)
    bit_width = function("getFieldDeclBitWidth", C.c_int, Cursor)
    visitor_type = C.CFUNCTYPE(C.c_uint, Cursor, Cursor, C.c_void_p)
    visit_children = function("visitChildren", C.c_uint, Cursor, visitor_type, C.c_void_p)
    field_visitor_type = C.CFUNCTYPE(C.c_uint, Cursor, C.c_void_p)
    visit_fields = function("Type_visitFields", C.c_uint, Type, field_visitor_type, C.c_void_p)
    diagnostic_count = function("getNumDiagnostics", C.c_uint, C.c_void_p)
    diagnostic_get = function("getDiagnostic", C.c_void_p, C.c_void_p, C.c_uint)
    diagnostic_level = function("getDiagnosticSeverity", C.c_uint, C.c_void_p)
    diagnostic_text = function("getDiagnosticSpelling", String, C.c_void_p)
    diagnostic_category = function("getDiagnosticCategoryText", String, C.c_void_p)
    diagnostic_free = function("disposeDiagnostic", None, C.c_void_p)

    types, functions, variables, headers = {}, {}, {}, []
    index = create_index(0, 0)
    include = pathlib.Path(options["include"])
    target = options["target"]
    arch = "arm-linux-androideabi" if target.startswith("armv7") else "aarch64-linux-android"
    arguments = ["-target", target, "-x", "c", "-std=gnu11", "-D_GNU_SOURCE", "-D__ANDROID_API__=36", "-Wno-everything", "-I" + str(include / arch), "-I" + str(include)]
    arguments.extend(options.get("arguments", []))

    def location(cursor):
        file, line, column, offset = C.c_void_p(), C.c_uint(), C.c_uint(), C.c_uint()
        spelling_location(cursor_location(cursor), C.byref(file), C.byref(line), C.byref(column), C.byref(offset))
        return {"file": text(file_name(file)).replace(str(include), "<ndk>"), "offset": offset.value}

    def children(cursor):
        found = []

        @visitor_type
        def visitor(child, parent, data):
            found.append(child)
            return 1

        visit_children(cursor, visitor, None)
        return found

    def fields(value):
        # Type_visitFields includes the implicit fields which carry anonymous
        # records. Cursor children alone omit these storage-bearing fields.
        found = []

        @field_visitor_type
        def visitor(field, data):
            found.append(field)
            return 1

        visit_fields(value, visitor, None)
        return found

    def describe(value):
        original = value
        spelling = text(type_name(original)).replace(str(include), "<ndk>")
        value = canonical(value)
        key = spelling
        size, align = type_size(value), type_align(value)
        previous = types.get(key)
        if previous and (previous["size"] >= 0 or size < 0):
            return key
        kind = text(type_kind(value.kind))
        # Clang classifies a typedef by its canonical kind, but structural
        # queries on CXType_Typedef return invalid pointees/array elements.
        # Keep original spellings for ABI semantics and unwrap only the query.
        structure = original if text(type_kind(original.kind)) == kind else value
        record = {"name": spelling, "kind": kind, "size": size, "alignment": align, "const": bool(is_const(value))}
        aliases, alias = [], original
        while True:
            alias_kind = text(type_kind(alias.kind))
            if alias_kind == "Typedef":
                declared = declaration(alias)
                aliases.append(text(cursor_name(declared)))
                alias = underlying_type(declared)
            elif alias_kind == "Elaborated":
                alias = named_type(alias)
            elif alias_kind == "Attributed":
                alias = modified_type(alias)
            else:
                break
        if aliases:
            record["aliases"] = aliases
        types[key] = record
        if kind in ("Pointer", "BlockPointer"):
            record["element"] = describe(pointee(structure))
        elif kind in ("ConstantArray", "IncompleteArray", "VariableArray"):
            record.update(element=describe(array_element(structure)), count=array_size(value))
        elif kind in ("FunctionProto", "FunctionNoProto"):
            record.update(result=describe(result_type(structure)), parameters=[describe(arg_type(structure, n)) for n in range(max(0, num_args(value)))], variadic=bool(is_variadic(value)))
        elif kind == "Enum":
            record["element"] = describe(enum_type(declaration(value)))
        elif kind == "Record":
            decl = declaration(value)
            record["union"] = text(cursor_kind(decl.kind)) == "UnionDecl"
            anonymous = 0
            record["fields"] = []
            for field in fields(value):
                name = text(cursor_name(field))
                if not name or is_anonymous_record(declaration(cursor_type(field))):
                    name = "@anonymous" + str(anonymous)
                    anonymous += 1
                record["fields"].append({"name": name, "type": describe(cursor_type(field)), "offset": field_offset(field), "bits": bit_width(field) if is_bitfield(field) else 0})
        return key

    variants = [(header, level) for header in options["headers"] for level in options.get("headerLevels", {}).get(header, [36])]
    for header, level in variants:
        current_arguments = ["-D__ANDROID_API__=" + str(level) if value.startswith("-D__ANDROID_API__=") else value for value in arguments]
        argv = (C.c_char_p * len(current_arguments))(*[value.encode() for value in current_arguments])
        contents = (options.get("headerPrelude", {}).get(header, "") + '#include <' + header + '>\nvoid __maclauncher_character_types(__WCHAR_TYPE__);\n').encode()
        unsaved = Unsaved(b"android-abi.c", contents, len(contents))
        tu = C.c_void_p()
        error = parse(index, b"android-abi.c", argv, len(arguments), C.byref(unsaved), 1, 0x40, C.byref(tu))
        if error:
            headers.append({"header": header, "error": "Clang translation unit " + str(error)})
            continue
        errors, categories = [], []
        for n in range(diagnostic_count(tu)):
            diagnostic = diagnostic_get(tu, n)
            if diagnostic_level(diagnostic) >= 3:
                errors.append(text(diagnostic_text(diagnostic)))
                categories.append(text(diagnostic_category(diagnostic)))
            diagnostic_free(diagnostic)
        if errors:
            dispose_tu(tu)
            cpp_arguments = ["-target", target, "-x", "c++", "-std=gnu++17", "-nostdinc++", "-I" + str(include / "c++/v1")] + current_arguments[5:]
            cpp_argv = (C.c_char_p * len(cpp_arguments))(*[value.encode() for value in cpp_arguments])
            error = parse(index, b"android-abi.c", cpp_argv, len(cpp_arguments), C.byref(unsaved), 1, 0x40, C.byref(tu))
            errors = [] if not error else ["Clang C++ translation unit " + str(error)]
            categories = []
            if not error:
                for n in range(diagnostic_count(tu)):
                    diagnostic = diagnostic_get(tu, n)
                    if diagnostic_level(diagnostic) >= 3:
                        errors.append(text(diagnostic_text(diagnostic)))
                        categories.append(text(diagnostic_category(diagnostic)))
                    diagnostic_free(diagnostic)
        # An original #error is a supplier-declared target constraint (for
        # example LP32 time64.h on LP64). Preserve that declaration and reason;
        # the other architecture still contributes its complete APIs.
        target_rejection = bool(errors) and len(categories) == len(errors) and all(value == "User-Defined Issue" for value in categories)
        headers.append({"header": header, "api": level, "errors": errors, "originalTargetConstraint": target_rejection})
        if errors:
            if tu:
                dispose_tu(tu)
            continue
        pending = children(root_cursor(tu))
        while pending:
            cursor = pending.pop()
            kind = text(cursor_kind(cursor.kind))
            if kind in ("LinkageSpec", "Namespace", "UnexposedDecl"):
                pending.extend(children(cursor))
                continue
            # Erased payload APIs (for example interface queries) refer to
            # public types which are not reachable from an exported function's
            # signature. Retain every declared C type from the same header
            # universe, rather than discovering it from individual programs.
            if kind in ("TypedefDecl", "StructDecl", "UnionDecl", "EnumDecl"):
                signature = describe(cursor_type(cursor))
                types[signature]["declaration"] = location(cursor)
                continue
            # Public inline definitions (notably Bionic ctype) are the actual
            # API declarations even when their header spelling uses static.
            # Static data still has no external native symbol identity.
            if kind not in ("FunctionDecl", "VarDecl") or kind == "VarDecl" and storage(cursor) == 3:
                continue
            name = text(cursor_name(cursor))
            symbol = text(mangling(cursor)) or name
            value = cursor_type(cursor)
            if kind == "FunctionDecl":
                signature = describe(value)
                policy = cursor_policy(cursor)
                printed = text(cursor_printed(cursor, policy))
                dispose_policy(policy)
                import re
                annotation = re.search(r"\b(?:__)?format(?:__)?\s*\(\s*(_*(?:printf|scanf)_*)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)", printed)
                format_info = {"kind": annotation[1].strip("_"), "index": int(annotation[2]) - 1, "first": int(annotation[3]) - 1} if annotation else functions.get(symbol, {}).get("format")
                functions[symbol] = {"name": name, "symbol": symbol, "type": signature, "parameters": [text(cursor_name(arg_cursor(cursor, n))) for n in range(max(0, num_args(value)))], "format": format_info}
            else:
                signature = describe(value)
                previous = variables.get(symbol)
                if not previous or types[previous["type"]]["size"] < 0 or types[signature]["size"] >= 0:
                    variables[symbol] = {"name": name, "symbol": symbol, "type": signature, "declaration": location(cursor)}
        dispose_tu(tu)
    # CXType_WChar retains its language name after canonicalization. Resolve
    # its storage once, after all C/C++ translation units, so header order and
    # the first complete spelling cannot freeze an unsupported type entry.
    character = next((item for item in functions.values() if item["name"] == "__maclauncher_character_types"), None)
    if character:
        signature = types[character["type"]]
        underlying = types[signature["parameters"][0]]["kind"]
        for value in types.values():
            if value["kind"] == "WChar":
                value["kind"] = underlying
    dispose_index(index)
    return {"target": target, "types": types, "functions": functions, "variables": variables, "headers": headers}


if __name__ == "__main__":
    options = json.load(open(sys.argv[1]))
    result = extract(options)
    with open(sys.argv[2], "w") as output:
        json.dump(result, output, separators=(",", ":"))
