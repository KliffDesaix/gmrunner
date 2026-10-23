// json_parse and json_stringify edge cases.

// A struct's field names, sorted.
function gmt_names(_struct) {
    var _n = variable_struct_get_names(_struct);
    array_sort(_n, true);
    return string(_n);
}

function gmt_json() {
    // Fields are listed in GameMaker's hash order, which shifts as the game's code changes, so
    // tests that print a struct give it one field or sort its names.
    var _s = json_parse("{\"b\": [1, 2, 3,], }");
    gmt_log("json.trailing_commas", json_stringify(_s));
    gmt_log("json.number", json_stringify({ n: 0.1 + 0.2 }));
    gmt_log("json.int", json_stringify({ n: 3 }));
    gmt_log("json.bool", json_stringify([true, false]));
    gmt_log("json.nested", json_stringify(json_parse("{\"x\":{\"y\":[{\"z\":null}]}}")));
    gmt_log("json.unicode", json_stringify({ s: "é✓" }));
    gmt_log("json.int64", json_stringify({ n: int64(5) }));
    gmt_log("json.undefined", json_stringify({ u: undefined }));
    var _r = { a: 1, b: 2, c: 3 };
    variable_struct_remove(_r, "b");
    gmt_log("json.removed", gmt_names(_r));
    _r.b = 4;
    gmt_log("json.readded", gmt_names(_r));
    var _m = ds_map_create();
    _m[? "n"] = 2;
    gmt_log("json.encode", json_encode(_m));
    ds_map_destroy(_m);
    var _o = { zeta: 1, alpha: 2, mid: 3 };
    gmt_log("struct.names", gmt_names(_o));
    gmt_log("struct.string", string({ zeta: 1 }));
    var _u = { a: undefined, b: 1 };
    gmt_log("struct.undefined_count", variable_struct_names_count(_u));
    gmt_log("struct.undefined_exists", variable_struct_exists(_u, "a"));
    try { json_parse("{oops"); gmt_log("json.invalid", "no error"); }
    catch (_e) { gmt_log("json.invalid", "error"); }
}
