package maclauncher.android.framework;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Base64;
import java.util.HashSet;

/** Launch-owned environment for native Android image initialization. */
final class HostNativeStartup {
    private HostNativeStartup() {}
    private static String[] admitted;

    static synchronized String[] environment() {
        if (admitted == null) admitted = decode(System.getenv("KL_ANDROID_NATIVE_STARTUP_ENVIRONMENT"));
        return admitted.clone();
    }

    static String[] decode(String packet) {
        if (packet == null || !packet.startsWith("1:") || packet.length() > 21848)
            throw invalid();
        String encoded = packet.substring(2);
        if (!encoded.matches("[A-Za-z0-9_-]*")) throw invalid();
        byte[] bytes;
        try { bytes = Base64.getUrlDecoder().decode(encoded); }
        catch (IllegalArgumentException failure) { throw invalid(); }
        if (bytes.length > 16384 || !Base64.getUrlEncoder().withoutPadding().encodeToString(bytes).equals(encoded))
            throw invalid();
        if (bytes.length == 0) return new String[0];
        if (bytes[bytes.length - 1] != 0) throw invalid();
        String text;
        try {
            text = StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes)).toString();
        } catch (CharacterCodingException failure) { throw invalid(); }
        var values = new ArrayList<String>();
        var keys = new HashSet<String>();
        int start = 0;
        for (int at = 0; at < text.length(); at++) {
            if (text.charAt(at) != 0) continue;
            if (values.size() == 64) throw invalid();
            String value = text.substring(start, at);
            int equals = value.indexOf('=');
            if (equals < 1) throw invalid();
            String key = value.substring(0, equals);
            if (!key.matches("[A-Za-z_][A-Za-z0-9_]*") || !keys.add(key)) throw invalid();
            values.add(value);
            start = at + 1;
        }
        return values.toArray(String[]::new);
    }

    private static IllegalStateException invalid() {
        return new IllegalStateException("Invalid Android native startup environment");
    }
}
