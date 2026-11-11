import fixture.Child;
import fixture.Counter;
import fixture.Parent;
import fixture.Root;
import fixture.Tag;
import java.lang.reflect.Method;

@SuppressWarnings({"rawtypes", "unchecked"})
public class NongenericAncestorHarness {
    static int identities;
    static int failures;
    static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    static void same(Object actual, Object expected) {
        check(actual == expected);
        ++identities;
    }
    static class Override extends Child<Tag, Tag> {
        Tag selected;
        int dispatched;
        Override(Tag selected) { super(6); this.selected = selected; }
        public Tag echo(Tag value) { ++dispatched; return selected; }
    }
    public static void main(String[] args) throws Exception {
        Tag a = new Tag(new Object());
        Tag b = new Tag(new Object());
        Child<Tag, Tag> child = new Child<Tag, Tag>(5);
        Parent raw = child;
        Counter counter = child;
        check(counter.count() == 6 && child.seed == 5);
        for (Tag value : new Tag[] {a, b, null, a}) {
            int calls = child.calls;
            same(child.echo(value), value);
            same(child.value, value);
            same(raw.echo(value), value);
            same(child.value, value);
            check(child.calls == calls + 2);
        }
        for (int seed : new int[] {-9, 0, 7, Integer.MAX_VALUE}) {
            child.seed = seed;
            int views = child.views;
            check(counter.count() == seed + 1 && ((Root) child).count() == seed + 1);
            for (int position : new int[] {-3, 0, 5, Integer.MIN_VALUE}) {
                check(child.view(position) == seed + position + 2);
                check(raw.view(position) == seed + position + 2);
            }
            check(child.views == views + 8);
        }
        RuntimeException sentinel = new IllegalStateException();
        child.failure = sentinel;
        for (Object invalid : new Object[] {new Object(), "invalid", new Object[0]}) {
            int calls = child.calls;
            try {
                raw.echo(invalid);
                throw new AssertionError();
            } catch (ClassCastException expected) { ++failures; }
            check(child.calls == calls);
        }
        for (Tag value : new Tag[] {a, b, null, a}) {
            int calls = child.calls;
            Tag previous = child.value;
            try {
                raw.echo(value);
                throw new AssertionError();
            } catch (RuntimeException actual) {
                same(actual, sentinel);
                ++failures;
            }
            same(child.value, previous);
            check(child.calls == calls + 1);
        }
        try {
            ((Parent) null).echo(a);
            throw new AssertionError();
        } catch (NullPointerException expected) { ++failures; }
        Override override = new Override(b);
        same(((Parent) override).echo(a), b);
        check(override.dispatched == 1 && override.calls == 0);
        check(Parent.class.getSuperclass() == Root.class && Parent.class.getGenericInterfaces()[0] == Counter.class);
        check(Child.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<S>"));
        check(Child.class.getTypeParameters().length == 2
                && Child.class.getTypeParameters()[0].getBounds()[0] == Tag.class
                && Child.class.getTypeParameters()[1].getBounds()[0] == Tag.class);
        Method target = Child.class.getDeclaredMethod("echo", Tag.class);
        check(target.getGenericParameterTypes()[0].equals(Child.class.getTypeParameters()[0])
                && target.getGenericReturnType().equals(Child.class.getTypeParameters()[0]));
        int bridges = 0;
        for (Method method : Child.class.getDeclaredMethods()) {
            if (method.isBridge()) {
                check(method.isSynthetic() && method.getName().equals("echo") && method.getReturnType() == Object.class
                        && method.getParameterTypes()[0] == Object.class);
                ++bridges;
            }
        }
        check(bridges == 1);
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=1,views=32");
        System.out.println("generic-and-ordinary-ancestors=retained,mutable-super-state=matched,virtual-dispatch=matched");
    }
}
