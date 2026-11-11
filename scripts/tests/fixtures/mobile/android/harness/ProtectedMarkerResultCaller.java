import fixture.*;
public final class ProtectedMarkerResultCaller {
    public static Object call(Base subject, int shape) {
        if (shape == 0) return Core.call(subject);
        if (shape == 1) return Core.object(subject);
        if (shape == 2) { Core.discard(subject); return null; }
        return Core.checked(subject);
    }
}
