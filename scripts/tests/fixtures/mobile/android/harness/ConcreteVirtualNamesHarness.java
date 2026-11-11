import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public class ConcreteVirtualNamesHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned concrete virtual family contract");
    }
    static Object call(int route, Root receiver, Object input, String spelling) throws Throwable {
        if (route == 0) return receiver.entry(input);
        if (route == 1) return ConcreteVirtualNamesCaller.call(receiver, input);
        if (route == 2) {
            try { return Root.class.getDeclaredMethod(spelling, Object.class).invoke(receiver, input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return MethodHandles.publicLookup().findVirtual(Root.class, spelling,
                MethodType.methodType(Object.class, Object.class)).invoke(receiver, input);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), receiverKind = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]); String spelling = args[3];
        require(Root.class.getDeclaredMethod(spelling, Object.class).getModifiers() == 1);
        require(Child.class.getDeclaredMethod(spelling, Object.class).getModifiers() == 1);
        require(Leaf.class.getDeclaredMethod(spelling, Object.class).getModifiers() == 1);
        require(java.lang.reflect.Modifier.isAbstract(Root.class.getModifiers()));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Trace.events = 0; Trace.seed = attempt == 0 ? 7 : 200;
            Trace.fail = mode == 1 ? 1 : mode == 2 ? 3 : mode == 3 ? 9 : 0;
            Trace.failure = new IllegalArgumentException("owned virtual family failure");
            Object initial = new Object(), value = new Object();
            Trace.initialized = initial; Trace.value = value;
            int[] values = {13}; Object input = mode == 4 ? null : mode == 5 ? value : values;
            long expected = 0; Object remaining = value, result = null;
            int changed = 13;
            if (mode == 3) { if (attempt == 0) { expected = 9; remaining = initial; } }
            else {
                if (attempt == 0) { expected = 9; remaining = initial; }
                if (receiverKind == 2) expected = expected * 11 + 4;
                if (receiverKind != 0) expected = expected * 11 + 2;
                expected = expected * 11 + 1;
                if (mode != 1 && mode != 4 && mode != 5) {
                    changed += attempt == 0 ? 100 : 200;
                    expected = expected * 11 + 3;
                    if (mode != 2) { result = remaining; remaining = null; }
                }
            }
            Throwable caught = null; Object actual = null; Root receiver = null;
            try {
                receiver = receiverKind == 0 ? new Middle() : receiverKind == 1 ? new Child() : new Leaf();
                actual = call(route, receiver, input, spelling);
            } catch (Throwable failure) { caught = failure; }
            if (mode == 0) require(caught == null && actual == result);
            else if (mode == 1 || mode == 2) require(caught == Trace.failure);
            else if (mode == 3) {
                if (attempt == 0) require(caught instanceof ExceptionInInitializerError && caught.getCause() == Trace.failure);
                else require(caught instanceof NoClassDefFoundError);
            } else if (mode == 4) require(caught instanceof NullPointerException);
            else require(caught instanceof ClassCastException);
            require(values[0] == changed && Trace.events == expected && Trace.value == remaining);
            if (mode == 0 && receiver instanceof Child) {
                Trace.events = 0; Trace.value = value;
                require(((Child) receiver).parent(values) == value && Trace.events == 14 && Trace.value == null);
                require(values[0] == changed + (attempt == 0 ? 100 : 200));
            }
        }
        System.out.println("attempts=2,virtual-family-values-identities-effects-failures=matched");
    }
}
