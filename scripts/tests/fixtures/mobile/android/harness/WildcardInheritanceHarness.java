import fixture.Bindings;
import fixture.dependency.ChildTag;
import fixture.dependency.Contravariant;
import fixture.dependency.Covariant;
import fixture.dependency.Leaf;
import fixture.dependency.Node;
import fixture.dependency.Root;
import fixture.dependency.Tag;
import fixture.dependency.Unbounded;

public final class WildcardInheritanceHarness {
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("forwarded wildcard behavior changed");
    }
    private static Object select(int kind, boolean first, Object left, Object right) {
        if (kind == 0) return Bindings.chooseUpper(first,
                (Covariant<Leaf<? extends ChildTag>>) left, (Covariant<Leaf<? extends ChildTag>>) right);
        if (kind == 1) return Bindings.chooseLower(first,
                (Contravariant<Leaf<? super Tag>>) left, (Contravariant<Leaf<? super Tag>>) right);
        return Bindings.chooseAny(first, (Unbounded<Leaf<?>>) left, (Unbounded<Leaf<?>>) right);
    }
    private static Root<?> node(int kind, Object holder) {
        if (kind == 0) return ((Covariant<?>) holder).node();
        if (kind == 1) return ((Contravariant<?>) holder).node();
        return ((Unbounded<?>) holder).node();
    }
    public static void main(String[] args) throws Exception {
        Object token = new Object();
        Node<ChildTag> upperLeft = new Node<ChildTag>(new ChildTag(token));
        Node<ChildTag> upperRight = new Node<ChildTag>(new ChildTag(null));
        Node<Tag> lowerLeft = new Node<Tag>(new Tag(token));
        Node<Object> lowerRight = new Node<Object>(new Object());
        Covariant<Leaf<? extends ChildTag>> upperA = new Covariant<Leaf<? extends ChildTag>>(upperLeft);
        Covariant<Leaf<? extends ChildTag>> upperB = new Covariant<Leaf<? extends ChildTag>>(upperRight);
        Contravariant<Leaf<? super Tag>> lowerA = new Contravariant<Leaf<? super Tag>>(lowerLeft);
        Contravariant<Leaf<? super Tag>> lowerB = new Contravariant<Leaf<? super Tag>>(lowerRight);
        Unbounded<Leaf<?>> anyA = new Unbounded<Leaf<?>>(upperLeft);
        Unbounded<Leaf<?>> anyB = new Unbounded<Leaf<?>>(lowerRight);
        Bindings.upper = upperA;
        Bindings.lower = lowerA;
        Bindings.any = anyA;
        require(Bindings.upper == upperA && Bindings.lower == lowerA && Bindings.any == anyA);
        Object[][] holders = {{upperA, upperB, null}, {lowerA, lowerB, null}, {anyA, anyB, null}};
        Object[][] values = {{upperLeft.stored, upperRight.stored}, {lowerLeft.stored, lowerRight.stored},
                             {upperLeft.stored, lowerRight.stored}};
        int choices = 0, reads = 0;
        for (int kind = 0; kind < holders.length; ++kind) {
            for (int left = 0; left < 3; ++left) {
                for (int right = 0; right < 3; ++right) {
                    for (boolean first : new boolean[] {false, true}) {
                        Object selected = select(kind, first, holders[kind][left], holders[kind][right]);
                        int index = first ? left : right;
                        require(selected == holders[kind][index]);
                        ++choices;
                        if (selected != null) {
                            require(node(kind, selected).value() == values[kind][index]);
                            ++reads;
                        }
                    }
                }
            }
        }
        require(choices == 54 && reads == 36 && upperLeft.reads == 12 && upperRight.reads == 6
                && lowerLeft.reads == 6 && lowerRight.reads == 12
                && upperA.accesses == 6 && upperB.accesses == 6 && lowerA.accesses == 6 && lowerB.accesses == 6
                && anyA.accesses == 6 && anyB.accesses == 6);
        ChildTag changed = new ChildTag(new Object());
        upperLeft.stored = changed;
        require(node(0, select(0, true, upperA, upperB)).value() == changed);
        RuntimeException sentinel = new IllegalStateException("owned wildcard sentinel");
        upperLeft.failure = sentinel;
        for (int kind : new int[] {0, 2}) {
            try { node(kind, select(kind, false, null, holders[kind][0])).value(); throw new AssertionError("failure missing"); }
            catch (RuntimeException actual) { require(actual == sentinel); }
        }
        for (String name : new String[] {"upper", "lower", "any"}) {
            var field = Bindings.class.getDeclaredField(name);
            String methodName = name.equals("upper") ? "chooseUpper" : name.equals("lower") ? "chooseLower" : "chooseAny";
            var method = Bindings.class.getDeclaredMethod(methodName, boolean.class, field.getType(), field.getType());
            require(field.getGenericType().equals(method.getGenericReturnType())
                    && field.getGenericType().equals(method.getGenericParameterTypes()[1])
                    && field.getGenericType().equals(method.getGenericParameterTypes()[2]));
            System.out.println(name + ":" + field.getGenericType().getTypeName());
        }
        System.out.println("choices=54;reads=36;mutable-identity=matched;exception-identity=matched;generic-metadata=matched");
    }
}
