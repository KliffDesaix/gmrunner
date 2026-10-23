// The test log: each test records what it observed as "name = value". The results of a run on
// GameMaker's own runner are the reference; gmRunner's results are compared line by line.
function gmt_log(_name, _value) {
    array_push(global.gmt_lines, _name + " = " + string(_value));
}

function gmt_run_all() {
    global.gmt_lines = [];
    gmt_log("runtime", GM_runtime_version);
    // A group that stops on an error logs it, and the others still run.
    var _groups = [["language", gmt_language], ["json", gmt_json], ["assets", gmt_assets],
                   ["fonts", gmt_fonts], ["misc", gmt_misc]];
    for (var _g = 0; _g < array_length(_groups); _g++) {
        try { _groups[_g][1](); }
        catch (_e) { gmt_log(_groups[_g][0] + ".stopped", is_struct(_e) ? _e.message : _e); }
    }
    var _f = file_text_open_write("gmtest_results.txt");
    for (var _i = 0; _i < array_length(global.gmt_lines); _i++) {
        file_text_write_string(_f, global.gmt_lines[_i]);
        file_text_writeln(_f);
        show_debug_message(global.gmt_lines[_i]);
    }
    file_text_close(_f);
    show_debug_message("gmtest: " + string(array_length(global.gmt_lines)) + " results written to " + game_save_id + "gmtest_results.txt");
}
