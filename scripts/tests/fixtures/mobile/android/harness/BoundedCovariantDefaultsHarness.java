import fixture.*;
import java.lang.reflect.*;

@SuppressWarnings({"rawtypes", "unchecked"})
public class BoundedCovariantDefaultsHarness {
    static int values, identities, failures;
    static void check(boolean value) { values++; if (!value) throw new AssertionError("value " + values); }
    static void same(Object a, Object b) { identities++; if (a != b) throw new AssertionError("identity " + identities); }
    static ResultBase call(int entry, Receiver owner, Pair pair) {
        if (entry == 0) return ((Provider) owner).make(pair);
        if (entry == 1) return ((Root) owner).make(pair);
        if (entry == 2) return BoundedCovariantDefaultsCaller.narrow(owner, pair);
        return BoundedCovariantDefaultsCaller.wide(owner, pair);
    }
    static void reset(RuntimeException failure) {
        Effects.receiver = null; Effects.input = null; Effects.calls = 0; Effects.failure = failure;
    }
    static void failed(int entry, Receiver owner, Pair pair, RuntimeException expected, int count) {
        try { call(entry, owner, pair); throw new AssertionError("missing failure"); }
        catch (RuntimeException actual) {
            failures++;
            if (expected == null) check(actual instanceof NullPointerException);
            else same(actual, expected);
            check(Effects.calls == count);
            same(Effects.receiver, count == 0 ? null : owner);
            same(Effects.input, count == 0 ? null : pair);
        }
    }
    static void contract(Class<?> owner, String name, Class<?> result, String formal) throws Exception {
        Method method = owner.getDeclaredMethod(name, Pair.class);
        check(method.getReturnType() == result);
        check(method.isDefault() == (owner == Provider.class));
        TypeVariable<?>[] variables = method.getTypeParameters();
        check(variables.length == 1);
        check(variables[0].getName().equals(formal));
        same(variables[0].getGenericDeclaration(), method);
        same(variables[0].getBounds()[0], Bound.class);
        ParameterizedType parameter = (ParameterizedType) method.getGenericParameterTypes()[0];
        same(parameter.getRawType(), Pair.class);
        Type first = parameter.getActualTypeArguments()[0];
        if (owner == Provider.class) same(first, String.class);
        else same(((TypeVariable<?>) first).getGenericDeclaration(), Root.class);
        check(parameter.getActualTypeArguments()[1].equals(variables[0]));
        ParameterizedType returned = (ParameterizedType) method.getGenericReturnType();
        same(returned.getRawType(), result);
        check(returned.getActualTypeArguments()[0].equals(variables[0]));
    }
    public static void main(String[] args) throws Exception {
        Receiver[] owners = {new Receiver(), new Receiver()};
        Bound[] bounds = {new Bound(), new OtherBound(), null};
        Object[] firsts = {"text", Integer.valueOf(7), new Object(), null};
        for (Receiver owner : owners) for (Bound bound : bounds) for (Object first : firsts) {
            Pair pair = new Pair(first, bound);
            ResultBase previous = null;
            for (int entry = 0; entry < 4; entry++) {
                reset(null);
                ResultBase result = call(entry, owner, pair);
                check(result instanceof ResultLeaf);
                check(result != previous);
                same(result.value(), bound);
                same(((ResultLeaf) result).item, bound);
                same(Effects.receiver, owner);
                same(Effects.input, pair);
                same(pair.first, first);
                check(Effects.calls == 1);
                previous = result;
            }
        }
        for (int entry = 0; entry < 4; entry++) {
            RuntimeException sentinel = new RuntimeException("owned default failure");
            Pair pair = new Pair(new Object(), new Bound());
            reset(sentinel); failed(entry, owners[0], pair, sentinel, 1);
            reset(sentinel); failed(entry, owners[0], null, sentinel, 1);
            reset(null); failed(entry, owners[0], null, null, 1);
            reset(sentinel); failed(entry, null, pair, null, 0);
        }
        for (int entry = 0; entry < 4; entry++) {
            Pair pair = new Pair(new Object(), new Object());
            reset(null);
            try { call(entry, owners[0], pair); throw new AssertionError("missing bound cast"); }
            catch (ClassCastException expected) {
                failures++;
                check(Effects.calls == 1);
                same(Effects.receiver, owners[0]); same(Effects.input, pair);
            }
        }
        final ResultLeaf selected = new ResultLeaf(new OtherBound());
        Receiver override = new Receiver() {
            @Override public <R extends Bound> ResultLeaf<R> make(Pair<String, R> input) {
                Effects.enter(this, input);
                return selected;
            }
        };
        for (int entry = 0; entry < 4; entry++) {
            reset(null); same(call(entry, override, null), selected);
            check(Effects.calls == 1); same(Effects.receiver, override); same(Effects.input, null);
        }
        contract(Root.class, "make", ResultBase.class, "M");
        contract(Provider.class, "make", ResultLeaf.class, "R");
        int bridges = 0;
        for (Method method : Provider.class.getDeclaredMethods()) if (method.isBridge()) {
            bridges++; check(method.isSynthetic()); check(method.isDefault());
            same(method.getReturnType(), ResultBase.class);
        }
        check(bridges == 1);
        int originalMethods = 0, helperMethods = 0;
        for (Method method : Receiver.class.getDeclaredMethods()) {
            if (args.length == 1 && method.getName().equals("__neverdThrow") &&
                    Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()) &&
                    method.getReturnType() == RuntimeException.class &&
                    java.util.Arrays.equals(method.getParameterTypes(), new Class<?>[]{Throwable.class}))
                helperMethods++;
            else originalMethods++;
        }
        check(originalMethods == 0 && helperMethods == (args.length == 0 ? 0 : 1));
        ParameterizedType parent = (ParameterizedType) Provider.class.getGenericInterfaces()[0];
        same(parent.getRawType(), Root.class); same(parent.getActualTypeArguments()[0], String.class);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("bounded-default-scopes=retained,original-interface-bridges-and-ordered-effects=matched");
    }
}
