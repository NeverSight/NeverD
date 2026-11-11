import fixture.Bounded;
import fixture.CheckedBound;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.TypeVariable;
import java.util.Iterator;

public final class SyntheticBoundsHarness {
    private static int calls;
    private static Object seen;
    private static final RuntimeException SENTINEL = new RuntimeException("owned callback failure");
    private static final class Receiver implements Bounded<Integer>, Iterable<Integer> {
        public boolean accept(Integer value) {
            ++calls;
            seen = value;
            if (value == -1) throw SENTINEL;
            return value > 0;
        }
        public Iterator<Integer> iterator() { throw new AssertionError("unexpected iteration"); }
    }
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("Checked synthetic bound behavior changed");
    }
    private static Object call(Method method, Object receiver, Object input) throws Exception {
        try { return method.invoke(null, receiver, input); }
        catch (InvocationTargetException error) { return error.getCause(); }
    }
    public static void main(String[] args) throws Exception {
        Method method = CheckedBound.class.getDeclaredMethod("probe", Bounded.class, Object.class);
        require(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()));
        require(method.getReturnType() == boolean.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        if (args[0].equals("original")) {
            require(method.isSynthetic() && formals.length == 2);
            require(formals[0].getBounds().length == 1 && formals[0].getBounds()[0] == Object.class);
            require(formals[1].getBounds().length == 2);
            ParameterizedType bound = (ParameterizedType) formals[1].getBounds()[0];
            require(bound.getRawType() == Bounded.class && bound.getActualTypeArguments().length == 1
                    && bound.getActualTypeArguments()[0].equals(formals[0]));
        } else require(formals.length == 0);
        method.setAccessible(true);
        Receiver receiver = new Receiver();
        int probes = 0;
        for (Integer value : new Integer[] {17, 0, -7, 99}) {
            require(call(method, receiver, value).equals(value > 0));
            require(seen == value);
            ++probes;
        }
        require(call(method, receiver, null).equals(false) && calls == 4);
        require(call(method, null, null).equals(false) && calls == 4);
        require(call(method, receiver, new Object()) instanceof ClassCastException && calls == 4);
        require(call(method, null, new Object()) instanceof ClassCastException && calls == 4);
        require(call(method, receiver, "wrong bridge type") instanceof ClassCastException && calls == 4);
        require(call(method, null, Integer.valueOf(1)) instanceof NullPointerException && calls == 4);
        require(call(method, receiver, Integer.valueOf(-1)) == SENTINEL && calls == 5);
        System.out.println("identity-probes=" + probes + ";callback-calls=" + calls
                + ";null-short-circuits=2;cast-failures=3;null-receiver-failures=1;callback-failures=1");
    }
}
