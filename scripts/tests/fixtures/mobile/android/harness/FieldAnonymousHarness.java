import fixture.Base;
import fixture.CaptureHost;
import java.lang.reflect.Modifier;

public class FieldAnonymousHarness {
    public static void main(String[] args) throws Throwable {
        Object token = new Object();
        for (int value : new int[]{Integer.MIN_VALUE, -3, 0, 7, Integer.MAX_VALUE}) {
            Base outer = CaptureHost.make(value, token);
            Base child = outer.nested();
            if (outer.seen != value + 3 || child.seen != value + 6
                    || child.value() != value + 1 || child.token() != token
                    || outer.token() != token || outer.nested() != child) {
                throw new AssertionError("Field capture value or initialization order changed");
            }
            System.out.println(value + ":" + outer.seen + ":" + child.seen);
        }
        Base outer = CaptureHost.make(4, token);
        Class<?> parent = outer.getClass();
        Class<?> child = outer.nested().getClass();
        if (parent.getEnclosingClass() != CaptureHost.class
                || !parent.getEnclosingMethod().getName().equals("make")
                || child.getEnclosingClass() != parent
                || child.getEnclosingMethod() != null
                || child.getEnclosingConstructor() != null
                || child.getDeclaredConstructors().length != 1) {
            throw new AssertionError("Field anonymous enclosure changed");
        }
        System.out.println("parentAnonymous=" + parent.isAnonymousClass()
                + ",parentLocal=" + parent.isLocalClass()
                + ",childAnonymous=" + child.isAnonymousClass()
                + ",childMember=" + child.isMemberClass()
                + ",childStatic=" + Modifier.isStatic(child.getModifiers()));
    }
}
