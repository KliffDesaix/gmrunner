// Numbers, strings and other runtime behaviour games depend on.
function gmt_misc() {
    gmt_log("num.div_zero", 1 / 0);
    gmt_log("num.neg_div_zero", -1 / 0);
    gmt_log("num.string_real", string(0.1 + 0.2));
    gmt_log("num.string_big", string(123456789.123));
    gmt_log("num.int64", string(int64(9007199254740993)));
    gmt_log("num.mod_neg", -7 mod 3);
    gmt_log("num.div_neg", -7 div 3);
    gmt_log("num.round_half", string(round(2.5)) + "," + string(round(3.5)) + "," + string(round(-2.5)));
    gmt_log("str.format", string_format(3.14159, 5, 2));
    gmt_log("str.upper", string_upper("straße"));
    gmt_log("str.length_utf8", string_length("é✓a"));
    gmt_log("str.byte_length", string_byte_length("é✓a"));
    gmt_log("str.split", json_stringify(string_split("a,b,,c", ",")));
    gmt_log("str.replace", string_replace_all("aaa", "a", "bb"));
    gmt_log("arr.sort", json_stringify(array_sort([3, 1, 2], true)));
}
