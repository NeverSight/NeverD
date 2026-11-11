import fixture.Base;
import fixture.CaptureHost;

public class LocalCapturesHarness {
    public static void main(String[] args) throws Throwable {
        Object token = new Object();
        for (int value : new int[] {Integer.MIN_VALUE, -3, 0, 7, Integer.MAX_VALUE}) {
            Base object = CaptureHost.make(value, token);
            if (object.seen != value + 3 || object.extra() != value + 3
                    || object.value() != value || object.token() != token) {
                throw new AssertionError("Captured constructor behavior changed");
            }
            System.out.println(value + ":" + object.seen + ":" + object.extra());
        }
        Class<?> type = CaptureHost.make(4, token).getClass();
        if (type.getEnclosingClass() != CaptureHost.class
                || !type.getEnclosingMethod().getName().equals("make")) {
            throw new AssertionError("Lexical method scope changed");
        }
        if (args.length != 0 && args[0].equals("generic")) {
            java.lang.reflect.Type result = type.getDeclaredMethod("token").getGenericReturnType();
            if (!(result instanceof java.lang.reflect.TypeVariable<?>))
                throw new AssertionError("Captured type variable was erased");
            java.lang.reflect.TypeVariable<?> variable = (java.lang.reflect.TypeVariable<?>) result;
            if (!variable.getName().equals("T")
                    || !variable.getGenericDeclaration().equals(type.getEnclosingMethod()))
                throw new AssertionError("Captured type variable has the wrong declaring method");
        }
        System.out.println("anonymous=" + type.isAnonymousClass() + ",local=" + type.isLocalClass());
    }
}
