import fixture.ArrayChoice;
import fixture.Fallback;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.TypeVariable;

public final class ArrayBoundsHarness {
    private static int calls;
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("Array bound behavior changed");
    }
    public static void main(String[] args) throws Exception {
        Object token = new Object();
        Object[][] arrays = {new Object[0], new Object[] {token}, new String[] {"value"}, new Object[][] {{token}}};
        int probes = 0;
        for (Object value : new Object[] {token, "fallback", new String[] {"array"}, null}) {
            Fallback<Object> callback = new Fallback<Object>() {
                public Object get() { ++calls; return value; }
            };
            for (Object[] input : arrays) {
                int before = calls;
                Object result = ArrayChoice.call(input, callback);
                require(result == (input.length == 0 ? value : input));
                require(calls == before + (input.length == 0 ? 1 : 0));
                ++probes;
            }
        }
        RuntimeException failure = new RuntimeException("owned sentinel");
        Fallback<Object> throwing = new Fallback<Object>() {
            public Object get() { ++calls; throw failure; }
        };
        require(ArrayChoice.call(arrays[1], throwing) == arrays[1] && calls == 4);
        try {
            ArrayChoice.call(arrays[0], throwing);
            throw new AssertionError("missing callback failure");
        } catch (RuntimeException actual) { require(actual == failure && calls == 5); }
        require(ArrayChoice.call(arrays[1], null) == arrays[1]);
        try {
            ArrayChoice.call(arrays[0], null);
            throw new AssertionError("missing null callback failure");
        } catch (NullPointerException expected) {}
        try {
            ArrayChoice.call(null, throwing);
            throw new AssertionError("missing null array failure");
        } catch (NullPointerException expected) { require(calls == 5); }
        Method method = ArrayChoice.class.getDeclaredMethod("choose", Object[].class, Fallback.class);
        require(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()));
        require(method.getReturnType() == Object.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        if (args[0].equals("original")) {
            require(formals.length == 2 && formals[0].getBounds().length == 2);
            require(formals[0].getBounds()[0] == Object[].class && formals[0].getBounds()[1].equals(formals[1]));
            require(formals[1].getBounds().length == 1 && formals[1].getBounds()[0] == Object.class);
        } else require(formals.length == 0);
        System.out.println("identity-probes=" + probes + ";callback-calls=" + calls + ";callback-failures=1;null-failures=2");
    }
}
