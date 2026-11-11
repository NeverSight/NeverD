import fixture.Base;
import fixture.CaptureHost;

public class ConstructorCapturesHarness {
    public static void main(String[] args) throws Throwable {
        Object token = new Object();
        for (int value : new int[] {Integer.MIN_VALUE, -3, 0, 7, Integer.MAX_VALUE}) {
            Base object = new CaptureHost(value, token).product;
            if (object.seen != value + 3 || object.extra() != value + 3
                    || object.value() != value || object.token() != token) {
                throw new AssertionError("Constructor-local capture changed");
            }
            System.out.println(value + ":" + object.seen + ":" + object.extra());
        }
        Class<?> type = new CaptureHost(4, token).product.getClass();
        if (type.getEnclosingClass() != CaptureHost.class || type.getEnclosingMethod() != null
                || !type.getEnclosingConstructor().equals(
                        CaptureHost.class.getConstructor(int.class, Object.class))) {
            throw new AssertionError("Lexical constructor scope changed");
        }
        System.out.println("anonymous=" + type.isAnonymousClass() + ",local=" + type.isLocalClass());
    }
}
