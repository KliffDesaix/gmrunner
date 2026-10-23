# SDL's Java side is called from native code (JNI): keep it whole.
-keep class org.libsdl.app.** { *; }
-keep class org.gmrunner.app.GameActivity { *; }
