// asset_* constants and the asset functions that take them.
function gmt_assets() {
    gmt_log("const.asset_object", asset_object);
    gmt_log("const.asset_sprite", asset_sprite);
    gmt_log("const.asset_sound", asset_sound);
    gmt_log("const.asset_room", asset_room);
    gmt_log("const.asset_path", asset_path);
    gmt_log("const.asset_script", asset_script);
    gmt_log("const.asset_font", asset_font);
    gmt_log("const.asset_timeline", asset_timeline);
    gmt_log("const.asset_shader", asset_shader);
    gmt_log("const.asset_tiles", asset_tiles);
    gmt_log("asset_get_type.room", asset_get_type("rm_gmtest"));
    gmt_log("asset_get_type.object", asset_get_type("obj_gmtest"));
    gmt_log("asset_get_type.script", asset_get_type("gmt_core"));
    gmt_log("asset_get_ids.room", array_length(asset_get_ids(asset_room)));
    gmt_log("asset_get_ids.object", array_length(asset_get_ids(asset_object)));
}
