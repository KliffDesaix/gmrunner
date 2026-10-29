// try / catch / finally, throw, runtime error structs, statics and constructors.
function gmt_thrower(_v) {
    throw _v;
}

function gmt_return_in_try() {
    global.gmt_flag = "unset";
    try {
        return "returned";
    } finally {
        global.gmt_flag = "finally ran";
    }
    return "after";
}

function gmt_counter() constructor {
    static made = 0;
    made++;
    id_number = made;
    static describe = function() { return "counter " + string(id_number); };
}

function gmt_counter_child() : gmt_counter() constructor {
    static describe = function() { return "child " + string(id_number); };
}

function gmt_language() {
    var _log = "";
    try { _log += "a"; throw "x"; _log += "b"; } catch (_e) { _log += "c" + string(_e); } finally { _log += "f"; }
    gmt_log("try.throw_catch_finally", _log);

    _log = "";
    try { _log += "a"; } catch (_e) { _log += "c"; } finally { _log += "f"; }
    gmt_log("try.no_throw", _log);

    _log = "";
    try {
        try { _log += "a"; throw 5; } finally { _log += "f"; }
    } catch (_e) { _log += "o" + string(_e); }
    gmt_log("try.finally_without_catch", _log);

    _log = "";
    try {
        try { throw 1; } catch (_e) { _log += "c"; throw 2; } finally { _log += "f"; }
    } catch (_e2) { _log += "o" + string(_e2); }
    gmt_log("try.throw_in_catch", _log);

    gmt_log("try.return_in_try", gmt_return_in_try());
    gmt_log("try.return_in_try_finally", global.gmt_flag);

    _log = "";
    try { gmt_thrower("deep"); } catch (_e) { _log = string(_e); }
    gmt_log("try.from_function", _log);

    try { throw { code: 7 }; } catch (_e) { gmt_log("try.throw_struct", _e.code); }

    try {
        var _arr = [1, 2];
        var _v = _arr[5];
    } catch (_e) {
        gmt_log("try.error_is_struct", is_struct(_e));
        gmt_log("try.error_has_message", variable_struct_exists(_e, "message"));
        gmt_log("try.error_has_stacktrace", variable_struct_exists(_e, "stacktrace"));
    }

    // Reading past the end of an array: an empty array's [0], a short array's [5], a negative index.
    var _empty = [];
    try { var _x = _empty[0]; gmt_log("array.read_empty_0", _x); } catch (_e) { gmt_log("array.read_empty_0", "error"); }
    var _short = [1, 2];
    try { var _y = _short[5]; gmt_log("array.read_past_end", _y); } catch (_e) { gmt_log("array.read_past_end", "error"); }
    try { var _z = _short[-1]; gmt_log("array.read_negative", _z); } catch (_e) { gmt_log("array.read_negative", "error"); }
    var _grid = [[1, 2], []];
    try { var _w = _grid[1][0]; gmt_log("array.read_empty_row", _w); } catch (_e) { gmt_log("array.read_empty_row", "error"); }

    // Compound assignment to an element of a nested array (savearef / restorearef in the bytecode),
    // on global, instance, local and struct arrays, with the index from a variable.
    var _k = 1;
    global.gmt_grid = [[0, 0], [5, 5]];
    global.gmt_grid[0][_k] += 3;
    global.gmt_grid[1][_k] -= 2;
    global.gmt_grid[1][0] *= 4;
    gmt_log("array2d.global_compound", global.gmt_grid);
    gmt_cells = [[10, 20], [30, 40]];
    gmt_cells[_k][0] -= 1;
    gmt_cells[0][_k] /= 4;
    gmt_log("array2d.instance_compound", gmt_cells);
    var _local = [[1], [2, 3]];
    _local[1][_k] += _local[0][0];
    _local[0][0] |= 6;
    gmt_log("array2d.local_compound", _local);
    var _holder = { rows: [["a"], ["b"]] };
    _holder.rows[1][0] += "c";
    gmt_log("array2d.struct_compound", _holder.rows);
    var _counts = [[0, 0, 0]];
    _counts[0][_k]++;
    ++_counts[0][2];
    _counts[0][0]--;
    var _post = _counts[0][_k]++;
    gmt_log("array2d.increment", string(_counts) + " " + string(_post));
    var _cube = [[[1, 2]]];
    _cube[0][0][_k] += 10;
    gmt_log("array2d.three_levels", _cube);
    var _cursor = [[0, 0]];
    for (var _i = 0; _i < 3; _i++) _cursor[0][_k] = (_cursor[0][_k] + 1) % 3;
    _cursor[0][0] -= 1;
    gmt_log("array2d.cursor_wrap", _cursor);
    var _legacy;
    _legacy[0, 1] = 7;
    _legacy[0, 1] += 1;
    gmt_log("array2d.legacy_syntax", _legacy[0, 1]);

    // Nested: the inner try's handler is gone once its catch ran; the outer one still works.
    _log = "";
    try {
        try { throw "in"; } catch (_e) { _log += "1"; }
        throw "out";
    } catch (_e) { _log += "2" + _e; }
    gmt_log("try.nested_handlers", _log);

    var _a = new gmt_counter();
    var _b = new gmt_counter_child();
    gmt_log("static.count", gmt_counter.made);
    gmt_log("static.describe", _a.describe());
    gmt_log("static.child_describe", _b.describe());
    gmt_log("static.child_inherits", is_instanceof(_b, gmt_counter));
}
