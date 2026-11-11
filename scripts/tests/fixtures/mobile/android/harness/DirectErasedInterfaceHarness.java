import fixture.Box;
import fixture.Contract;
import fixture.Cursor;
import fixture.DirectTransform;
import fixture.MutableReader;
import fixture.Pair;
import java.lang.reflect.ParameterizedType;

public final class DirectErasedInterfaceHarness {
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("direct erased interface behavior changed");
    }
    private static Object call(DirectTransform target, Object input, boolean dispatch) {
        return dispatch ? ((Contract) target).map(input) : target.map(input);
    }
    public static void main(String[] args) throws Exception {
        String[] tokens = {null, "owned", new String("same"), ""};
        int normal = 0, failures = 0;
        DirectTransform target = null;
        for (boolean dispatch : new boolean[] {false, true}) {
            for (int i = 0; i < tokens.length; ++i) {
                MutableReader reader = new MutableReader(tokens[i]);
                Pair<Box<String>, Box<Object>> pair = new Pair<Box<String>, Box<Object>>(reader);
                Cursor<Pair<? extends Box<String>, ? extends Box<Object>>> cursor =
                        new Cursor<Pair<? extends Box<String>, ? extends Box<Object>>>(pair);
                target = new DirectTransform(cursor);
                DirectTransform.calls = 0;
                for (int round = 0; round < 2; ++round) {
                    reader.value = tokens[(i + round) % tokens.length];
                    require(call(target, pair, dispatch) == reader.value
                            && pair.accesses == round + 1 && reader.reads == round + 1);
                    ++normal;
                }
                pair.failure = new IllegalArgumentException("owned getter sentinel");
                try { call(target, pair, dispatch); throw new AssertionError("getter failure missing"); }
                catch (RuntimeException actual) {
                    require(actual == pair.failure && pair.accesses == 3 && reader.reads == 2);
                    ++failures;
                }
                pair.failure = null;
                reader.failure = new IllegalStateException("owned reader sentinel");
                try { call(target, pair, dispatch); throw new AssertionError("reader failure missing"); }
                catch (RuntimeException actual) {
                    require(actual == reader.failure && pair.accesses == 4 && reader.reads == 3);
                    ++failures;
                }
                pair.reader = null;
                try { call(target, pair, dispatch); throw new AssertionError("missing reader failure missing"); }
                catch (NullPointerException expected) {
                    require(pair.accesses == 5 && reader.reads == 3);
                    ++failures;
                }
                require(DirectTransform.calls == 5 && target.items() == cursor
                        && ((Contract) target).items() == cursor && cursor.next() == pair && cursor.positions == 1);
                System.out.println(dispatch + ":" + i + ":calls=5;getters=5;reads=3;cursor-identity=matched");
            }
            for (Object value : new Object[] {null, "invalid", Integer.valueOf(4), new Object()}) {
                int calls = DirectTransform.calls;
                try { call(target, value, dispatch); throw new AssertionError("invalid input accepted"); }
                catch (RuntimeException actual) {
                    require(actual.getClass() == (value == null ? NullPointerException.class : ClassCastException.class)
                            && DirectTransform.calls == calls + (value == null ? 1 : 0));
                    ++failures;
                }
            }
        }
        int calls = DirectTransform.calls;
        try { ((DirectTransform) null).map("invalid"); throw new AssertionError("typed null receiver accepted"); }
        catch (NullPointerException expected) { require(DirectTransform.calls == calls); ++failures; }
        try { ((Contract) null).map("invalid"); throw new AssertionError("interface null receiver accepted"); }
        catch (NullPointerException expected) { require(DirectTransform.calls == calls); ++failures; }
        var method = DirectTransform.class.getDeclaredMethod("map", Object.class);
        require(method.getReturnType() == Object.class && !method.isBridge());
        if (args[0].equals("original")) {
            require(method.getGenericParameterTypes()[0] instanceof ParameterizedType
                    && ((ParameterizedType) method.getGenericParameterTypes()[0]).getRawType() == Pair.class
                    && method.getGenericReturnType() == String.class);
            require(DirectTransform.class.getGenericInterfaces()[0] instanceof ParameterizedType);
        } else {
            require(method.getGenericParameterTypes()[0] == Object.class && method.getGenericReturnType() == Object.class
                    && DirectTransform.class.getGenericInterfaces()[0] == Contract.class);
        }
        var items = DirectTransform.class.getDeclaredMethod("items");
        var constructor = DirectTransform.class.getDeclaredConstructor(Cursor.class);
        require(items.getGenericReturnType().equals(constructor.getGenericParameterTypes()[0])
                && items.getGenericReturnType().equals(DirectTransform.class.getDeclaredField("values").getGenericType()));
        System.out.println("normal=" + normal + ";failures=" + failures
                + ";original-descriptors=matched;cursor-metadata=matched;reflection=matched");
    }
}
