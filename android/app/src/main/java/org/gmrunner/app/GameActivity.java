package org.gmrunner.app;

import android.content.Intent;

import org.libsdl.app.SDLActivity;

// A running game: SDL's activity, given the game folder by the launcher. It runs in its own
// process (":game"), which ends with it; SDL's native side can then start fresh next time.
public class GameActivity extends SDLActivity {
    public static final String EXTRA_GAME = "game";

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "main"};
    }

    @Override
    protected String[] getArguments() {
        String game = getIntent().getStringExtra(EXTRA_GAME);
        return game == null ? new String[0] : new String[] {game};
    }

    // Called from the runner (TargetMobile.cpp): the display cutout's insets in pixels (left, top,
    // right, bottom), which the on-screen controls keep clear of. SDL lays the game out under it.
    public int[] safeInsets() {
        int[] out = new int[4];
        if (android.os.Build.VERSION.SDK_INT >= 28) {
            android.view.WindowInsets wi = getWindow().getDecorView().getRootWindowInsets();
            android.view.DisplayCutout c = wi == null ? null : wi.getDisplayCutout();
            if (c != null) {
                out[0] = c.getSafeInsetLeft();
                out[1] = c.getSafeInsetTop();
                out[2] = c.getSafeInsetRight();
                out[3] = c.getSafeInsetBottom();
            }
        }
        return out;
    }

    // Called from the runner (TargetMobile.cpp) when its in-game menu opens: MenuActivity, in
    // Material 3 over the paused game. It saves its changes in the game's gmrunner.ini;
    // nativeMenuResult tells the runner when it has closed (0 resume, 1 exit the game).
    private static final int MENU_REQUEST = 1;

    public boolean showMenu(String json) {
        runOnUiThread(() -> startActivityForResult(
            new Intent(this, MenuActivity.class).putExtra(MenuActivity.EXTRA_JSON, json), MENU_REQUEST));
        return true;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == MENU_REQUEST) nativeMenuResult(resultCode == MenuActivity.RESULT_EXIT ? 1 : 0);
    }

    private static native void nativeMenuResult(int result);

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (isFinishing()) android.os.Process.killProcess(android.os.Process.myPid());
    }
}
