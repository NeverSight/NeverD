import fixture.*;
import java.lang.reflect.*;
import java.util.*;
import java.util.function.*;

public final class RawOwnedInterfaceHarness {
    private static int values, identities, states, failures, keys;
    private interface Call { int run(Object left, Object right) throws Throwable; }
    private static int invoke(Method method, Object receiver, Object left, Object right) throws Throwable {
        try { return (Integer) method.invoke(receiver, left, right); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original comparator identity changed");
        identities++;
    }
    private static void run(Call call, Object left, Object right, boolean reversed,
                            boolean absent, boolean chained, RuntimeException problem) {
        Trace.calls = 0; Trace.left = Trace.right = null; keys = 0;
        Object first = reversed ? right : left, second = reversed ? left : right;
        Trace.failure = first; Trace.problem = problem;
        try {
            int result = call.run(left, right);
            if (absent || problem != null) throw new AssertionError("original comparator failure missing");
            int expected = first == second ? 0 : first == null ? -1 : 1;
            if (result != expected) throw new AssertionError("integer comparator result changed");
            values++;
        } catch (Throwable error) {
            if (absent) {
                if (error.getClass() != NullPointerException.class)
                    throw new AssertionError("null receiver exception changed", error);
            } else {
                same(error, problem);
                if (problem == null) throw new AssertionError("unexpected comparator failure", error);
            }
            failures++;
        }
        if (Trace.calls != (absent ? 0 : 1)
                || keys != (!absent && problem == null && chained && first == second ? 2 : 0))
            throw new AssertionError("original SDK default dispatch or exception order changed");
        same(Trace.left, absent ? null : first);
        same(Trace.right, absent ? null : second);
        states++;
    }
    public static void main(String[] args) throws Exception {
        boolean original = args[0].equals("original");
        TypeVariable<?> formal = Receiver.class.getTypeParameters()[0];
        if (!formal.getName().equals("T") || !Arrays.equals(formal.getBounds(), new Type[] {Object.class}))
            throw new AssertionError("original class-owned scope changed");
        Type edge = Receiver.class.getGenericInterfaces()[0];
        if (original ? edge != Comparator.class : !(edge instanceof ParameterizedType)
                || ((ParameterizedType) edge).getRawType() != Comparator.class
                || !Arrays.equals(((ParameterizedType) edge).getActualTypeArguments(), new Type[] {formal}))
            throw new AssertionError("raw interface argument projection differs from its report");
        Method implementation = Receiver.class.getDeclaredMethod("compare", Object.class, Object.class);
        Method declaration = Comparator.class.getMethod("compare", Object.class, Object.class);
        if (!Arrays.equals(implementation.getGenericParameterTypes(), new Type[] {formal, formal})
                || implementation.getTypeParameters().length != 0 || implementation.getReturnType() != int.class
                || implementation.isBridge() || implementation.isSynthetic()
                || implementation.getExceptionTypes().length != 0 || Receiver.class.getDeclaredFields().length != 0)
            throw new AssertionError("original compare scope, integer result or runtime inventory changed");
        int helpers = 0;
        for (Method method : Receiver.class.getDeclaredMethods()) {
            if (method.equals(implementation)) continue;
            if (!method.getName().equals("__neverdThrow") || !Modifier.isPrivate(method.getModifiers())
                    || !Modifier.isStatic(method.getModifiers()) || method.getReturnType() != RuntimeException.class
                    || !Arrays.equals(method.getParameterTypes(), new Class<?>[] {Throwable.class}))
                throw new AssertionError("source acquired an unreported comparator entry");
            helpers++;
        }
        if (helpers != (original ? 0 : 1)) throw new AssertionError("reported throw helper inventory changed");
        for (String name : new String[] {"reversed", "thenComparing"}) {
            for (Method method : Receiver.class.getMethods()) {
                if (method.getName().equals(name) && method.getDeclaringClass() != Comparator.class)
                    throw new AssertionError("SDK default acquired an invented source owner");
            }
        }
        Method genericDefault = Comparator.class.getMethod("thenComparing", Function.class, Comparator.class);
        if (genericDefault.getTypeParameters().length != 1 || !genericDefault.getTypeParameters()[0].getGenericDeclaration().equals(genericDefault))
            throw new AssertionError("SDK method-owned default scope changed");
        Receiver<Object> receiver = new Receiver<Object>();
        Comparator<Object> view = receiver;
        Comparator<Object> reverse = view.reversed();
        Comparator<Object> chained = view.thenComparing(value -> { keys++; return value; }, (a, b) -> 0);
        Call[] paths = {
            receiver::compare, view::compare,
            (left, right) -> invoke(implementation, receiver, left, right),
            (left, right) -> invoke(declaration, receiver, left, right),
            reverse::compare, chained::compare
        };
        Object first = new ArrayList<Object>(), second = new LinkedList<Object>();
        Object[][] pairs = {{first, second}, {second, first}, {first, first}, {null, first},
                           {first, null}, {null, null}};
        for (int i = 0; i < paths.length; i++) {
            for (Object[] pair : pairs) run(paths[i], pair[0], pair[1], i == 4, false, i == 5, null);
            run(paths[i], first, second, i == 4, false, i == 5,
                new IllegalStateException("owned comparator failure"));
        }
        Receiver<Object> absent = null;
        Comparator<Object> absentView = null;
        Call[] missing = {
            (left, right) -> absent.compare(left, right),
            (left, right) -> absentView.compare(left, right),
            (left, right) -> invoke(implementation, null, left, right),
            (left, right) -> invoke(declaration, null, left, right)
        };
        for (Call path : missing) run(path, first, second, false, true, false, null);
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("class-owned-method-formals=retained,sdk-default-owner=retained,raw-edge="
            + (original ? "original" : "reported-owned-argument"));
    }
}
