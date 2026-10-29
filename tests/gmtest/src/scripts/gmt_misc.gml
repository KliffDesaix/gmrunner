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

    // Functions that used to be stubs: values that don't depend on the machine or a controller.
    application_surface_enable(false);
    var _app_off = application_surface_is_enabled();
    application_surface_enable(true);
    gmt_log("sys.app_surface_enabled", string(_app_off) + " " + string(application_surface_is_enabled()));
    gamepad_set_axis_deadzone(0, 0.25);
    gmt_log("sys.pad_deadzone", gamepad_get_axis_deadzone(0));
    gamepad_set_axis_deadzone(0, 0);
    gmt_log("sys.pad_hats_unplugged", string(gamepad_hat_count(11)) + " " + string(gamepad_hat_value(11, 0)));
    mouse_clear(mb_left);
    gmt_log("sys.mouse_clear", mouse_check_button(mb_left));
    gmt_log("sys.env_missing", environment_get_variable("GMTEST_SURELY_UNSET_VARIABLE"));
    gmt_log("sys.permission", os_check_permission("android.permission.INTERNET"));
}
