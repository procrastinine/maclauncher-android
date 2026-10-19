package maclauncher.android.web;

/** Completion from the native browser data store. */
@FunctionalInterface
public interface WebKitResult { void complete(String json,String error); }
