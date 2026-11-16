package org.d2recomp;

import java.io.File;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.view.View;
import org.libsdl2.app.SDLActivity;

/**
 * Diablo II (statically recompiled 1.13c, libd2recomp.so) on the launcher's SDL2. Arguments: the
 * player's game folder (MPQs + 1.13c DLLs) and a save/log folder, then the game's own switches.
 * Runs in its own process (:d2): the native game can start only once per process.
 */
public class D2Activity extends SDLActivity {
    @Override protected String[] getLibraries() { return new String[] {"SDL2", "d2recomp"}; }

    @Override protected String[] getArguments() {
        String dir = getIntent().getStringExtra("dir");
        if (dir == null || dir.isEmpty()) dir = "/storage/emulated/0/Diablo2";
        File save = new File(getExternalFilesDir(null), "d2");
        save.mkdirs();
        // a language other than English: its pack is <game>/lang/<code>/data (the runtime adds -direct)
        String lang = getIntent().getStringExtra("lang");
        // the game's 800x600 widened to the screen's shape (600 high), so the controls sit over the picture
        android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(dm);
        int big = Math.max(dm.widthPixels, dm.heightPixels), small = Math.min(dm.widthPixels, dm.heightPixels);
        String wide = "--wide=" + (small > 0 ? 600 * big / small : 1068);
        if (lang != null && !lang.equals("en"))
            return new String[] {dir, save.getAbsolutePath(), wide, "--lang", lang.equals("ru") ? "rus" : lang, "--", "-3dfx"};
        return new String[] {dir, save.getAbsolutePath(), wide, "--", "-3dfx"};
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused) getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    @Override protected void onDestroy() {
        super.onDestroy();
        if (isFinishing()) android.os.Process.killProcess(android.os.Process.myPid());
    }
}
