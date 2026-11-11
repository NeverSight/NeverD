import fixture.*;
import java.lang.reflect.*;

public final class ScopedRawClassParametersHarness {
    static int values, identities, states, failures;
    static final class Leaf extends Core<Button> {
        @Override public boolean read(Object marker, Button input, Object after) {
            calls += 2;
            return super.read(marker, input, after);
        }
    }
    static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    static void identity(Object actual, Object expected) {
        identities++;
        require(actual == expected, "changed reference identity");
    }
    static Object call(int route, Core<Button> receiver, Object marker,
                       View input, Object after) throws Throwable {
        try {
            switch (route) {
            case 0: return receiver.read(marker, (Button) input, after);
            case 1: return ((Middle<Button>) receiver).read(marker, (Button) input, after);
            case 2: return ((Middle) receiver).read(marker, input, after);
            case 3: return ((Parent<Button>) receiver).read(marker, (Button) input, after);
            case 4: return ((Parent) receiver).read(marker, input, after);
            case 5: return Core.class.getDeclaredMethod("read", Object.class, Button.class, Object.class)
                .invoke(receiver, marker, input, after);
            case 6: return Core.class.getDeclaredMethod("read", Object.class, View.class, Object.class)
                .invoke(receiver, marker, input, after);
            case 7: return Parent.class.getDeclaredMethod("read", Object.class, View.class, Object.class)
                .invoke(receiver, marker, input, after);
            default: return Middle.class.getMethod("read", Object.class, View.class, Object.class)
                .invoke(receiver, marker, input, after);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static void state(Core<Button> receiver, Object marker, View input, Object after,
                      int calls, int arrayValue) {
        identity(receiver.marker, marker);
        identity(receiver.input, input);
        identity(receiver.after, after);
        states++;
        require(receiver.calls == calls, "changed virtual dispatch or effect count");
        states++;
        if (marker instanceof int[]) require(((int[]) marker)[0] == arrayValue, "changed mutable alias");
    }
    public static void main(String[] args) throws Throwable {
        require(Core.class.getTypeParameters().length == 1 &&
            Core.class.getTypeParameters()[0].getBounds()[0] == Button.class, "changed class bound");
        require(Core.class.getGenericSuperclass().getTypeName().equals("fixture.Middle<T>") &&
            Middle.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<M>"), "changed complete parent edges");
        Method typed = Core.class.getDeclaredMethod("read", Object.class, Button.class, Object.class);
        Method bridge = Core.class.getDeclaredMethod("read", Object.class, View.class, Object.class);
        require(typed.getGenericParameterTypes()[1] == Button.class && typed.getTypeParameters().length == 0,
            "invented target method Signature");
        require(Parent.class.getDeclaredMethod("read", Object.class, View.class, Object.class)
            .getGenericParameterTypes()[1].getTypeName().equals("P"), "lost original parent formal");
        require(bridge.isBridge() && bridge.isSynthetic() && !typed.isBridge(), "changed original bridge roles");
        for (boolean leaf : new boolean[]{false, true}) {
            int count = leaf ? 3 : 1;
            for (int route = 0; route < 9; route++) {
                for (View input : new View[]{null, new Button(), new ChildButton()})
                    for (boolean mutable : new boolean[]{false, true}) {
                        Core<Button> receiver = leaf ? new Leaf() : new Core<Button>();
                        Object marker = mutable ? new int[]{7} : new Object();
                        Object after = marker;
                        Object actual = call(route, receiver, marker, input, after);
                        values++;
                        require(actual.equals(input != null), "changed scalar result");
                        state(receiver, marker, input, after, count, 10);
                    }
                for (int mode = 0; mode < 4; mode++) {
                    Core<Button> receiver = leaf ? new Leaf() : new Core<Button>();
                    int[] marker = {7};
                    RuntimeException expected = new RuntimeException("owned-target-failure");
                    receiver.failure = expected;
                    View input = mode == 1 || mode == 3 ? new View() : new Button();
                    try {
                        call(route, mode == 2 ? null : receiver, marker, input, marker);
                        throw new AssertionError("missing original failure");
                    } catch (Throwable error) {
                        failures++;
                        if (mode == 0) identity(error, expected);
                        else require(mode == 2 ? error instanceof NullPointerException :
                            route == 5 ? error instanceof IllegalArgumentException : error instanceof ClassCastException,
                            "changed pre-effect cast or null-receiver failure: " + route + ":" + error);
                    }
                    if (mode == 0) state(receiver, marker, input, marker, count, 10);
                    else {
                        state(receiver, null, null, null, 0, 7);
                        require(marker[0] == 7, "cast failure changed mutable input");
                    }
                }
            }
        }
        require(values == 108 && identities == 558 && states == 360 && failures == 72,
            "incomplete independent route inventory: " + values + ":" + identities + ":" + states + ":" + failures);
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("scoped-raw-class-parameters=retained,original-super-dispatch-and-checked-bridge=retained,complete-runtime-metadata=retained");
    }
}
