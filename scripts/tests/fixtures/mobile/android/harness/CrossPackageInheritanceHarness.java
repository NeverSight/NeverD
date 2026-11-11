import fixture.base.Parent;
import fixture.base.Root;
import fixture.client.Child;
import fixture.client.Tag;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;

@SuppressWarnings({"rawtypes", "unchecked"})
public class CrossPackageInheritanceHarness {
    static int values;
    static int identities;
    static int failures;
    static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    static void value(int actual, int expected) { check(actual == expected); ++values; }
    static void same(Object actual, Object expected) { check(actual == expected); ++identities; }
    static class Override extends Child<Tag, Tag> {
        Tag selected;
        Override(Tag selected) { super(3); this.selected = selected; }
        public Tag echo(Tag value) { return selected; }
    }
    public static void main(String[] args) throws Exception {
        Tag a = new Tag(new Object());
        Tag b = new Tag(new Object());
        Child<Tag, Tag> child = new Child<Tag, Tag>(5);
        Root raw = child;
        for (Tag input : new Tag[] {a, b, null, a}) {
            int parentCalls = child.parentCalls;
            int calls = child.calls;
            int ownCalls = child.ownCalls;
            same(child.hidden(input), input);
            same(raw.rootEntry(input), input);
            same(child.echo(input), input);
            same(child.last, input);
            check(child.parentCalls == parentCalls + 1 && child.calls == calls + 2
                    && child.ownCalls == ownCalls + 1);
        }
        for (int seed : new int[] {-9, 0, Integer.MIN_VALUE, Integer.MAX_VALUE}) {
            child.seed = seed;
            for (int position : new int[] {-3, 0, 5, Integer.MAX_VALUE}) {
                value(raw.rootMode(position), seed + position + 10);
                value(child.mode(position), seed + position + 2);
            }
        }
        RuntimeException sentinel = new IllegalStateException();
        child.failure = sentinel;
        for (Object invalid : new Object[] {new Object(), "invalid", new Object[0]}) {
            int parentCalls = child.parentCalls;
            int calls = child.calls;
            try {
                raw.rootEntry(invalid);
                throw new AssertionError();
            } catch (ClassCastException expected) { ++failures; }
            check(child.parentCalls == parentCalls + 1 && child.calls == calls);
            same(child.hidden(invalid), invalid);
        }
        for (Tag input : new Tag[] {a, b, null, a}) {
            int parentCalls = child.parentCalls;
            int calls = child.calls;
            Tag previous = child.last;
            try {
                raw.rootEntry(input);
                throw new AssertionError();
            } catch (RuntimeException actual) { same(actual, sentinel); ++failures; }
            same(child.last, previous);
            same(child.hidden(input), input);
            check(child.parentCalls == parentCalls + 1 && child.calls == calls + 1);
        }
        try {
            ((Root) null).rootEntry(a);
            throw new AssertionError();
        } catch (NullPointerException expected) { ++failures; }
        Override override = new Override(b);
        same(((Root) override).rootEntry(a), b);
        check(override.parentCalls == 1 && override.calls == 0 && override.ownCalls == 0);
        check(Modifier.isAbstract(Root.class.getModifiers()) && Modifier.isAbstract(Parent.class.getModifiers()));
        check(((ParameterizedType) Parent.class.getGenericSuperclass()).getActualTypeArguments()[0]
                .equals(Parent.class.getTypeParameters()[0]));
        check(((ParameterizedType) Child.class.getGenericSuperclass()).getActualTypeArguments()[0]
                .equals(Child.class.getTypeParameters()[0]));
        Method rootHidden = Root.class.getDeclaredMethod("hidden", Object.class);
        Method childHidden = Child.class.getDeclaredMethod("hidden", Object.class);
        check(Modifier.isFinal(rootHidden.getModifiers()) && (rootHidden.getModifiers() & 7) == 0
                && Modifier.isPublic(childHidden.getModifiers()) && !childHidden.isBridge());
        check(rootHidden.getGenericParameterTypes()[0].equals(Root.class.getTypeParameters()[0])
                && rootHidden.getGenericReturnType().equals(Root.class.getTypeParameters()[0]));
        check(Modifier.isStatic(Root.class.getDeclaredMethod("mode", int.class).getModifiers())
                && !Modifier.isStatic(Child.class.getDeclaredMethod("mode", int.class).getModifiers()));
        Method target = Child.class.getDeclaredMethod("echo", Tag.class);
        check(target.getGenericParameterTypes()[0].equals(Child.class.getTypeParameters()[0])
                && target.getGenericReturnType().equals(Child.class.getTypeParameters()[0]));
        int bridges = 0;
        for (Method method : Child.class.getDeclaredMethods()) {
            if (!method.isBridge()) continue;
            check(method.isSynthetic() && method.getName().equals("echo")
                    && method.getReturnType() == Object.class && method.getParameterTypes()[0] == Object.class);
            ++bridges;
        }
        check(bridges == 1);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures + ",bridges=1");
        System.out.println("package-dispatch=retained,hidden-final-and-static-members=independent,generic-scopes=retained");
    }
}
