import fixture.Base;
import fixture.OwnedSelf;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class PublicBoundedSelfGetterHarness {
    private static int values, identities, failures;
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("owned getter effects changed");
        values++;
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("owned getter identity changed");
        identities++;
    }
    private interface Call { Object invoke() throws Throwable; }
    private static void failure(Call call, RuntimeException expected) {
        try { call.invoke(); throw new AssertionError("missing owned failure"); }
        catch (Throwable error) {
            if (error instanceof InvocationTargetException) error = error.getCause();
            if (error != expected) throw new AssertionError("owned exception identity changed", error);
            failures++;
            identities++;
        }
    }
    public static void main(String[] args) throws Throwable {
        MethodHandle erased = MethodHandles.publicLookup().findVirtual(
            OwnedSelf.class, "self", MethodType.methodType(Base.class));
        Method typed = null, bridge = null;
        for (Method method : OwnedSelf.class.getDeclaredMethods()) {
            if (!method.getName().equals("self")) continue;
            if (method.getReturnType() == OwnedSelf.class) typed = method;
            if (method.getReturnType() == Base.class) bridge = method;
        }
        if (typed == null || bridge == null || !bridge.isBridge() || !bridge.isSynthetic() ||
            !Modifier.isPublic(bridge.getModifiers()) || typed.isBridge() || typed.isSynthetic() ||
            (args[0].equals("original") ? !Modifier.isProtected(typed.getModifiers()) :
                                        !Modifier.isPublic(typed.getModifiers())))
            throw new AssertionError("target access differs from its reported projection");
        typed.setAccessible(true);
        final Method getter = typed;
        OwnedSelf first = new OwnedSelf(), second = new OwnedSelf();
        for (int repetition = 0; repetition < 40; repetition++)
            for (OwnedSelf expected : new OwnedSelf[] {null, first, second}) {
                OwnedSelf local = new OwnedSelf();
                local.value = expected;
                identity(local.dispatch(), expected); value(local.calls, 1); value(local.baseCalls, 1);
                identity(erased.invoke(local), expected); value(local.calls, 2);
                identity(getter.invoke(local), expected); value(local.calls, 3);
                identity(local.peek(), expected); value(local.calls, 4);
                local.value = local;
                identity(local.dispatch(), local); value(local.calls, 5); value(local.baseCalls, 2);
            }
        OwnedSelf failing = new OwnedSelf();
        RuntimeException problem = new IllegalStateException("owned getter failure");
        failing.failure = problem;
        failure(() -> failing.dispatch(), problem); value(failing.calls, 1); value(failing.baseCalls, 1);
        failure(() -> failing.peek(), problem); value(failing.calls, 2);
        failure(() -> erased.invoke(failing), problem); value(failing.calls, 3);
        failure(() -> getter.invoke(failing), problem); value(failing.calls, 4);
        try { erased.invoke((OwnedSelf) null); throw new AssertionError("null receiver accepted"); }
        catch (NullPointerException expected) { failures++; }
        if (!OwnedSelf.class.getGenericSuperclass().getTypeName().equals("fixture.Base<fixture.OwnedSelf>") ||
            !Base.class.getTypeParameters()[0].getBounds()[0].getTypeName().equals("fixture.Base<T>"))
            throw new AssertionError("complete recursive superclass bound changed");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("public-erased-nominal-entry-and-recursive-bound=matched,target-access=reported");
    }
}
