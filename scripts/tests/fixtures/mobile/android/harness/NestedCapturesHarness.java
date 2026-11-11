import fixture.Base;
import fixture.CaptureHost;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;

public class NestedCapturesHarness {
    public static void main(String[] args) throws Throwable {
        Object token = new Object();
        for (int value : new int[]{Integer.MIN_VALUE, -3, 0, 7, Integer.MAX_VALUE}) {
            Base outer = CaptureHost.make(value, token);
            Base inner = outer.nested();
            if (outer.seen != value + 3 || inner.seen != value + 6
                    || inner.value() != value + 1 || inner.token() != token) {
                throw new AssertionError("Nested capture value or initialization order changed");
            }
            System.out.println(value + ":" + outer.seen + ":" + inner.seen);
        }
        Base outer = CaptureHost.make(4, token);
        Class<?> parent = outer.getClass();
        Class<?> child = outer.nested().getClass();
        if (parent.getEnclosingClass() != CaptureHost.class
                || !parent.getEnclosingMethod().getName().equals("make")
                || child.getEnclosingClass() != parent
                || !child.getEnclosingMethod().getName().equals("nested")) {
            throw new AssertionError("Nested lexical method scopes changed");
        }
        for (Class<?> local : new Class<?>[]{parent, child}) {
            Type result = local.getDeclaredMethod("token").getGenericReturnType();
            if (!(result instanceof TypeVariable<?>)
                    || !((TypeVariable<?>) result).getGenericDeclaration().equals(parent.getEnclosingMethod())) {
                throw new AssertionError("Nested method type-variable binding changed");
            }
        }
        System.out.println("anonymous=" + (parent.isAnonymousClass() && child.isAnonymousClass())
                + ",local=" + (parent.isLocalClass() && child.isLocalClass()));
    }
}
