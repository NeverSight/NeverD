import fixture.Base;
import fixture.Carrier;
import fixture.Leaf;
import fixture.OtherLeaf;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.TypeVariable;

public final class BoundedCarrierHarness {
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("bounded carrier behavior changed");
    }
    public static void main(String[] args) throws Exception {
        Object[] tokens = {null, "owned", Integer.valueOf(51), new Object()};
        int successful = 0, reads = 0, failures = 0;
        for (Object token : tokens) {
            for (Base<?> node : new Base<?>[] {new Leaf(token), new OtherLeaf(token)}) {
                Carrier.calls = Carrier.closedCalls = 0;
                Carrier.failure = null;
                Carrier boxed = Carrier.wrap(node);
                require(boxed.unwrap() == node && boxed.unwrap() == node);
                for (int round = 0; round < 3; ++round) {
                    node.value = tokens[(successful + round) % tokens.length];
                    Base<?> actual = Carrier.segment(boxed.unwrap());
                    require(actual == node && actual.read() == node.value && node.reads == round + 1);
                    ++successful;
                    ++reads;
                }
                node.failure = new IllegalArgumentException("owned read sentinel");
                try { Carrier.segment(node).read(); throw new AssertionError("read failure missing"); }
                catch (RuntimeException actual) { require(actual == node.failure && node.reads == 4); }
                ++failures;
                Carrier.failure = new IllegalStateException("owned prefix sentinel");
                try { Carrier.segment(node); throw new AssertionError("prefix failure missing"); }
                catch (RuntimeException actual) { require(actual == Carrier.failure && node.reads == 4); }
                ++failures;
                require(Carrier.calls == 5 && Carrier.closedCalls == 0);
                Carrier.failure = null;
                System.out.println(node.getClass().getSimpleName() + ":calls=5;reads=4;identity=matched");
            }
        }
        Object closed = Carrier.closedSentinel();
        Carrier.calls = Carrier.closedCalls = 0;
        Object[] invalid = {null, "invalid", Integer.valueOf(3), new Object(), closed};
        for (Object value : invalid) {
            require(Carrier.wrap(value).unwrap() == value);
            int count = Carrier.calls;
            try { Carrier.segment(value); throw new AssertionError("invalid carrier accepted"); }
            catch (RuntimeException actual) {
                require(actual.getClass() == (value == null ? NullPointerException.class
                        : value == closed ? IllegalStateException.class : ClassCastException.class));
                require(Carrier.calls == count + 1);
            }
            ++failures;
        }
        require(Carrier.closedCalls == 1 && Carrier.wrap(null).unwrap() == null);
        Carrier.failure = new UnsupportedOperationException("owned ordering sentinel");
        for (Object value : new Object[] {null, "invalid"}) {
            try { Carrier.segment(value); throw new AssertionError("prefix failure missing"); }
            catch (RuntimeException actual) { require(actual == Carrier.failure); }
            ++failures;
        }
        try { Carrier.segment(closed); throw new AssertionError("closed failure missing"); }
        catch (IllegalStateException actual) { require(actual != Carrier.failure && Carrier.closedCalls == 2); }
        ++failures;
        Carrier.failure = null;
        try { ((Carrier) null).unwrap(); throw new AssertionError("null wrapper receiver accepted"); }
        catch (NullPointerException expected) { ++failures; }
        var formal = Carrier.class.getTypeParameters()[0];
        var bound = (ParameterizedType) formal.getBounds()[0];
        require(bound.getRawType() == Base.class && bound.getActualTypeArguments()[0].equals(formal));
        var method = Carrier.class.getDeclaredMethod("segment", Object.class);
        require(method.getReturnType() == Base.class && method.getTypeParameters().length == 0);
        if (args[0].equals("original")) {
            require(method.getGenericReturnType() instanceof TypeVariable);
            require(method.getGenericReturnType().equals(formal));
        } else require(method.getGenericReturnType() == Base.class);
        System.out.println("successful=" + successful + ";reads=" + reads + ";failures=" + failures
                + ";runtime-descriptors=matched;recursive-class-bound=matched;reflection=matched");
    }
}
