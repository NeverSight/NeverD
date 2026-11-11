package fixture;
import java.lang.reflect.*;
import java.lang.invoke.*;

public final class PackageGenericCovarianceHarness {
    static int values, identities, states, failures;
    static final class LeafReceiver extends Core {
        @Override public Leaf<? extends Tag> copy() {
            calls += 4;
            return super.copy();
        }
    }
    static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    static void identity(Object actual, Object expected) {
        identities++;
        require(actual == expected, "changed reference identity");
    }
    static Method method(Class<?> owner, Class<?> result) {
        for (Method method : owner.getDeclaredMethods())
            if (method.getName().equals("copy") && method.getReturnType() == result) {
                method.setAccessible(true);
                return method;
            }
        throw new AssertionError("missing original return entry");
    }
    static Object call(int route, Core receiver) throws Throwable {
        try {
            switch (route) {
            case 0: return receiver.copy();
            case 1: return ((Parent<String>) receiver).copy();
            case 2: return ((Parent) receiver).copy();
            case 3: return method(Core.class, Leaf.class).invoke(receiver);
            case 4: return method(Core.class, Result.class).invoke(receiver);
            case 5: return method(Parent.class, Result.class).invoke(receiver);
            default: return MethodHandles.lookup().findVirtual(Parent.class, "copy", MethodType.methodType(Result.class)).invoke(receiver);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static void state(Core receiver, Leaf<? extends Tag> selected, Object marker, int count, int arrayValue) {
        identity(receiver.selected, selected);
        identity(receiver.inherited, selected);
        identity(receiver.marker, marker);
        states++;
        require(receiver.calls == count, "changed virtual or parent effect order");
        states++;
        if (marker instanceof int[]) require(((int[]) marker)[0] == arrayValue, "changed mutable alias");
    }
    public static void main(String[] args) throws Throwable {
        require(Core.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<java.lang.String>"), "changed complete superclass argument");
        require(Leaf.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Result<java.lang.String>"), "changed full covariant interface result");
        require(method(Parent.class, Result.class).getGenericReturnType().getTypeName().equals("fixture.Result<P>") &&
            !Modifier.isPublic(method(Parent.class, Result.class).getModifiers()), "changed parent scope or package visibility");
        require(method(Core.class, Leaf.class).getGenericReturnType().getTypeName().equals("fixture.Leaf<? extends fixture.Tag>") &&
            method(Core.class, Result.class).isBridge() && method(Core.class, Result.class).isSynthetic(), "changed wildcard result or original bridge");
        for (boolean leaf : new boolean[]{false, true}) {
            int count = leaf ? 7 : 3;
            for (int route = 0; route < 7; route++) {
                for (boolean present : new boolean[]{false, true})
                    for (boolean mutable : new boolean[]{false, true}) {
                        Core receiver = leaf ? new LeafReceiver() : new Core();
                        Leaf<Tag> selected = present ? new Leaf<Tag>() : null;
                        Object marker = mutable ? new int[]{5} : new Object();
                        receiver.selected = selected;
                        receiver.inherited = selected;
                        receiver.marker = marker;
                        Object result = call(route, receiver);
                        values++;
                        identity(result, selected);
                        state(receiver, selected, marker, count, 6);
                    }
                for (int mode = 0; mode < 2; mode++) {
                    Core receiver = leaf ? new LeafReceiver() : new Core();
                    Leaf<Tag> selected = new Leaf<Tag>();
                    int[] marker = {5};
                    receiver.selected = selected;
                    receiver.inherited = selected;
                    receiver.marker = marker;
                    RuntimeException expected = new RuntimeException("owned-package-ancestor-failure");
                    receiver.failure = expected;
                    try {
                        call(route, mode == 0 ? receiver : null);
                        throw new AssertionError("missing original failure");
                    } catch (Throwable error) {
                        failures++;
                        if (mode == 0) identity(error, expected);
                        else require(error instanceof NullPointerException, "changed null receiver failure");
                    }
                    state(receiver, selected, marker, mode == 0 ? count : 0, mode == 0 ? 6 : 5);
                }
            }
        }
        require(values == 56 && identities == 322 && states == 168 && failures == 28,
            "incomplete independent route inventory: " + values + ":" + identities + ":" + states + ":" + failures);
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-package-covariance=retained,complete-wildcard-and-interface-results=retained,original-public-return-bridge=retained");
    }
}
