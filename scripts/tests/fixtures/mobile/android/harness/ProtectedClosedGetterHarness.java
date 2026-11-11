import fixture.OwnedLocal;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class ProtectedClosedGetterHarness {
    private static int values, identities, failures;
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("getter effects changed");
        values++;
    }
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("getter identity changed");
        identities++;
    }
    private interface Call { Object invoke() throws Throwable; }
    private static void failure(Call call, RuntimeException expected) {
        try { call.invoke(); throw new AssertionError("failure missing"); }
        catch (Throwable error) {
            if (error instanceof InvocationTargetException) error = error.getCause();
            if (error != expected) throw new AssertionError("exception identity changed", error);
            failures++; identities++;
        }
    }
    public static void main(String[] args) throws Throwable {
        MethodHandle erased = MethodHandles.publicLookup().findVirtual(
            OwnedLocal.class, "initialValue", MethodType.methodType(Object.class));
        Method typed = null, bridge = null;
        for (Method method : OwnedLocal.class.getDeclaredMethods()) {
            if (!method.getName().equals("initialValue")) continue;
            if (method.getReturnType() == String.class) typed = method;
            if (method.getReturnType() == Object.class) bridge = method;
        }
        if (typed == null || bridge == null || !bridge.isBridge() || !bridge.isSynthetic() ||
            !Modifier.isPublic(bridge.getModifiers()) || typed.isBridge() || typed.isSynthetic() ||
            (args[0].equals("original") ? !Modifier.isProtected(typed.getModifiers()) :
                                        !Modifier.isPublic(typed.getModifiers())))
            throw new AssertionError("target access differs from the reported source projection");
        typed.setAccessible(true);
        final Method getter = typed;
        String[] cases = {null, "", "owned string", new String("owned string")};
        for (int repetition = 0; repetition < 40; repetition++)
            for (String expected : cases) {
                OwnedLocal local = new OwnedLocal();
                local.value = expected;
                identity(local.get(), expected); value(local.calls, 1);
                identity(local.get(), expected); value(local.calls, 1);
                identity(erased.invoke(local), expected); value(local.calls, 2);
                identity(getter.invoke(local), expected); value(local.calls, 3);
                identity(local.peek(), expected); value(local.calls, 4);
                local.remove();
                identity(local.get(), expected); value(local.calls, 5);
            }
        OwnedLocal failing = new OwnedLocal();
        RuntimeException problem = new IllegalStateException("owned getter failure");
        failing.failure = problem;
        failure(() -> failing.get(), problem); value(failing.calls, 1);
        failure(() -> failing.get(), problem); value(failing.calls, 2);
        failure(() -> failing.peek(), problem); value(failing.calls, 3);
        failure(() -> erased.invoke(failing), problem); value(failing.calls, 4);
        failure(() -> getter.invoke(failing), problem); value(failing.calls, 5);
        try { erased.invoke((OwnedLocal) null); throw new AssertionError("null receiver accepted"); }
        catch (NullPointerException expected) { failures++; }
        if (OwnedLocal.class.getGenericSuperclass().getTypeName().equals("java.lang.ThreadLocal<java.lang.String>") == false)
            throw new AssertionError("generic parent changed");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("public-erased-entry-and-thread-local-state=matched,target-access=reported");
    }
}
