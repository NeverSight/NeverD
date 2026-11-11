import fixture.Base;
import fixture.Box;
import fixture.Factory;
import fixture.Option;
import fixture.Tag;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.TypeVariable;

@SuppressWarnings({"rawtypes", "unchecked"})
public class EnclosedInheritanceHarness {
    static int identities;
    static int failures;
    static void check(boolean condition) {
        if (!condition) throw new AssertionError();
    }
    static void same(Object actual, Object expected) {
        check(actual == expected);
        ++identities;
    }
    static void scope(Base<?> instance, String name, Class<?> erasure, boolean bounded, boolean source) throws Exception {
        Class<?> local = instance.getClass();
        Method enclosing = Factory.class.getDeclaredMethod(name, Box.class);
        check(local.isAnonymousClass() == !source && local.isLocalClass() == source
                && local.getEnclosingMethod().equals(enclosing));
        TypeVariable<?> formal = enclosing.getTypeParameters()[0];
        check(formal.getGenericDeclaration().equals(enclosing)
                && !formal.equals(Factory.class.getTypeParameters()[0]));
        check(formal.getBounds()[0] == (bounded ? Tag.class : Object.class));
        ParameterizedType parent = (ParameterizedType) local.getGenericSuperclass();
        check(parent.getRawType() == Base.class && parent.getActualTypeArguments()[0].equals(formal));
        Method target = local.getDeclaredMethod("echo", erasure, Option.class);
        check(target.getGenericParameterTypes()[0].equals(formal) && target.getGenericReturnType().equals(formal));
        ParameterizedType result = (ParameterizedType) local.getDeclaredMethod("box").getGenericReturnType();
        check(result.getActualTypeArguments()[0].equals(formal)
                && result.getActualTypeArguments()[1].equals(enclosing.getTypeParameters()[1]));
        int bridges = 0;
        for (Method method : local.getDeclaredMethods()) {
            if (method.isBridge()) {
                check(method.isSynthetic() && method.getName().equals("echo") && method.getReturnType() == Object.class
                        && method.getParameterTypes()[0] == Object.class && method.getParameterTypes()[1] == Option.class);
                ++bridges;
            }
        }
        check(bridges == (bounded ? 1 : 0));
    }
    public static void main(String[] args) throws Exception {
        Object a = new Object();
        Object b = new Object();
        Option option = new Option();
        option.token = a;
        Box<Object, Tag> box = new Box<Object, Tag>();
        box.secondary = new Tag(a);
        Base<Object> direct = Factory.make(box);
        Base raw = direct;
        for (Object value : new Object[] {a, b, null, a}) {
            int calls = box.calls;
            same(direct.echo(value, option), value);
            same(box.value, value);
            same(box.option, option);
            same(direct.echo(value), value);
            same(box.option, null);
            same(raw.echo(value, option), value);
            same(raw.box(), box);
            check(box.calls == calls + 3);
        }
        RuntimeException sentinel = new IllegalStateException();
        box.failure = sentinel;
        for (int i = 0; i < 4; ++i) {
            Object previous = box.value;
            int calls = box.calls;
            try {
                if (i == 0) direct.echo(null, null);
                else if (i == 1) direct.echo(a);
                else if (i == 2) raw.echo(b, option);
                else raw.echo(null);
                throw new AssertionError();
            } catch (RuntimeException actual) {
                same(actual, sentinel);
                ++failures;
            }
            same(box.value, previous);
            check(box.calls == calls + 1);
        }
        Tag first = new Tag(a);
        Tag second = new Tag(b);
        Box<Tag, Object> boundedBox = new Box<Tag, Object>();
        boundedBox.secondary = a;
        Base<Tag> bounded = Factory.makeBounded(boundedBox);
        raw = bounded;
        for (Tag value : new Tag[] {first, second, null, first}) {
            int calls = boundedBox.calls;
            same(bounded.echo(value, option), value);
            same(boundedBox.value, value);
            same(boundedBox.option, option);
            same(bounded.echo(value), value);
            same(boundedBox.option, null);
            same(raw.echo(value, option), value);
            same(raw.box(), boundedBox);
            check(boundedBox.calls == calls + 3);
        }
        boundedBox.failure = sentinel;
        for (Object invalid : new Object[] {a, "invalid", new Object[0]}) {
            int calls = boundedBox.calls;
            try {
                raw.echo(invalid, option);
                throw new AssertionError();
            } catch (ClassCastException expected) { ++failures; }
            try {
                raw.echo(invalid);
                throw new AssertionError();
            } catch (ClassCastException expected) { ++failures; }
            check(boundedBox.calls == calls);
        }
        for (int i = 0; i < 4; ++i) {
            Tag previous = boundedBox.value;
            int calls = boundedBox.calls;
            try {
                if (i == 0) bounded.echo(null, null);
                else if (i == 1) bounded.echo(first);
                else if (i == 2) raw.echo(second, option);
                else raw.echo(null);
                throw new AssertionError();
            } catch (RuntimeException actual) {
                same(actual, sentinel);
                ++failures;
            }
            same(boundedBox.value, previous);
            check(boundedBox.calls == calls + 1);
        }
        try {
            ((Base) null).echo(a);
            throw new AssertionError();
        } catch (NullPointerException expected) { ++failures; }
        box.failure = null;
        Base<Object> independent = Factory.make(new Box<Object, Tag>());
        same(direct.echo(b), b);
        check(independent.box().calls == 0 && independent.box() != box);
        Base<Object> nullCapture = Factory.<Object, Tag>make(null);
        same(nullCapture.box(), null);
        try {
            nullCapture.echo(a);
            throw new AssertionError();
        } catch (NullPointerException expected) { ++failures; }
        Base<Tag> nullBoundedCapture = Factory.<Tag, Object>makeBounded(null);
        same(nullBoundedCapture.box(), null);
        try {
            ((Base) nullBoundedCapture).echo(a);
            throw new AssertionError();
        } catch (ClassCastException expected) { ++failures; }
        try {
            nullBoundedCapture.echo(first);
            throw new AssertionError();
        } catch (NullPointerException expected) { ++failures; }
        boolean source = args.length == 1 && args[0].equals("source");
        scope(direct, "make", Object.class, false, source);
        scope(bounded, "makeBounded", Tag.class, true, source);
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=1");
        System.out.println("enclosing-method-formals=retained,class-formal-shadowing=matched,capture-and-cast-order=matched");
        System.out.println("class-reflection=" + (source ? "reported-named-local" : "original-anonymous"));
    }
}
