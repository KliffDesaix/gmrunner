// font_add from an included file and by system font name, styles, measuring text.
function gmt_measure(_name, _font) {
    draw_set_font(_font);
    gmt_log(_name + ".width", string_width("Hello, World! 123"));
    gmt_log(_name + ".height", string_height("Hello"));
    gmt_log(_name + ".wrapped_height", string_height_ext("The quick brown fox jumps over the lazy dog", -1, 100));
}

function gmt_fonts() {
    gmt_measure("font.default", -1);
    var _file = font_add("test_mono.ttf", 14, false, false, 32, 127);
    gmt_log("font.file_added", font_exists(_file));
    gmt_measure("font.file", _file);
    gmt_log("font.file_size", font_get_size(_file));
    gmt_measure("font.file_bold", font_add("test_mono.ttf", 14, true, false, 32, 127));
    gmt_measure("font.file_italic", font_add("test_mono.ttf", 14, false, true, 32, 127));
    var _info = font_get_info(_file);
    gmt_log("font.info_is_struct", is_struct(_info));
    draw_set_font(-1);
}
