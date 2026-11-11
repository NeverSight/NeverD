import fixture.Base;
import fixture.Bounded;
import fixture.Cell;
import fixture.Child;
import fixture.Option;
import fixture.Producer;
import fixture.Tag;
import fixture.Variant;
import java.lang.reflect.Method;
import java.lang.reflect.TypeVariable;

public class OpenInheritanceHarness {
    static int identities;
    static int failures;
    static void check(boolean condition) {
        if (!condition) throw new AssertionError();
    }
    static void same(Object actual, Object expected) {
        check(actual == expected);
        ++identities;
    }
    static class Override extends Child<Object, Tag> {
        Object selected;
        int events;
        Override(Cell<Object, Tag> cell) { super(cell); }
        public void apply(Object value, Option option) {
            ++events;
            selected = value;
        }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Object a = new Object();
        Object b = new Object();
        Tag tag = new Tag(a);
        Tag other = new Tag(b);
        Option option = new Option();
        option.token = a;
        Cell<Object, Tag> cell = new Cell<Object, Tag>();
        Child<Object, Tag> child = new Child<Object, Tag>(cell);
        Base<Object> base = child;
        Base raw = child;
        for (Object value : new Object[] {a, b, null, a}) {
            int calls = child.calls;
            cell.second = value == a ? tag : other;
            child.apply(value, option);
            same(child.last, value);
            same(cell.first, value);
            same(child.option, option);
            same(child.result(), cell);
            base.apply(value);
            same(child.last, value);
            same(child.option, null);
            same(base.result(), cell);
            raw.apply(value, option);
            same(child.last, value);
            same(child.option, option);
            same(raw.result(), cell);
            same(cell.read(), cell.second);
            check(child.calls == calls + 3);
        }
        RuntimeException sentinel = new IllegalStateException();
        child.failure = sentinel;
        for (int i = 0; i < 4; ++i) {
            Object previous = child.last;
            int calls = child.calls;
            try {
                if (i == 0) child.apply(null, null);
                else if (i == 1) base.apply(a);
                else if (i == 2) raw.apply(b, option);
                else raw.apply(null);
                throw new AssertionError();
            } catch (RuntimeException actual) {
                same(actual, sentinel);
                ++failures;
            }
            same(child.last, previous);
            same(cell.first, previous);
            check(child.calls == calls + 1);
        }
        Cell<Tag, Object> boundedCell = new Cell<Tag, Object>();
        Bounded<Tag> bounded = new Bounded<Tag>(boundedCell);
        Base<Tag> boundedBase = bounded;
        raw = bounded;
        for (Tag value : new Tag[] {tag, other, null, tag}) {
            int calls = bounded.calls;
            boundedCell.second = value == tag ? a : b;
            bounded.apply(value, option);
            same(bounded.last, value);
            same(boundedCell.first, value);
            same(bounded.option, option);
            same(bounded.result(), boundedCell);
            boundedBase.apply(value);
            same(bounded.last, value);
            same(bounded.option, null);
            same(boundedBase.result(), boundedCell);
            raw.apply(value, option);
            same(bounded.last, value);
            same(bounded.option, option);
            same(raw.result(), boundedCell);
            same(boundedCell.read(), boundedCell.second);
            check(bounded.calls == calls + 3);
        }
        bounded.failure = sentinel;
        for (Object invalid : new Object[] {a, "invalid", new Object[0]}) {
            int calls = bounded.calls;
            try {
                raw.apply(invalid, option);
                throw new AssertionError();
            } catch (ClassCastException expected) { ++failures; }
            try {
                raw.apply(invalid);
                throw new AssertionError();
            } catch (ClassCastException expected) { ++failures; }
            check(bounded.calls == calls);
        }
        for (int i = 0; i < 4; ++i) {
            Tag previous = bounded.last;
            int calls = bounded.calls;
            try {
                if (i == 0) bounded.apply(null, null);
                else if (i == 1) boundedBase.apply(tag);
                else if (i == 2) raw.apply(other, option);
                else raw.apply(null);
                throw new AssertionError();
            } catch (RuntimeException actual) {
                same(actual, sentinel);
                ++failures;
            }
            same(bounded.last, previous);
            same(boundedCell.first, previous);
            check(bounded.calls == calls + 1);
        }
        try {
            ((Base) null).apply(a);
            throw new AssertionError();
        } catch (NullPointerException expected) { ++failures; }
        Override override = new Override(cell);
        ((Base) override).apply(b);
        same(override.selected, b);
        check(override.events == 1 && override.calls == 0);
        Cell<Tag, Tag> pair = new Cell<Tag, Tag>();
        Cell<Tag, Tag>[] cells = (Cell<Tag, Tag>[]) new Cell<?, ?>[] {pair};
        Variant<Tag> variant = new Variant<Tag>(pair, cells);
        Producer<Tag> producer = variant;
        for (Tag value : new Tag[] {tag, other, null, tag}) {
            pair.second = value;
            same(producer.upper(), pair);
            same(producer.lower(), pair);
            same(producer.array(), cells);
            same(producer.identity(value), value);
            same(producer.upper().read(), value);
        }
        check(cell.reads == 4 && boundedCell.reads == 4 && pair.reads == 4);
        TypeVariable<?>[] formals = Child.class.getTypeParameters();
        check(formals.length == 2 && formals[0].getName().equals("I") && formals[1].getName().equals("O"));
        check(formals[0].getGenericDeclaration() == Child.class && formals[1].getGenericDeclaration() == Child.class);
        check(Child.class.getGenericSuperclass().getTypeName().equals("fixture.Base<I>"));
        check(Child.class.getDeclaredMethod("apply", Object.class, Option.class)
                .getGenericParameterTypes()[0].equals(formals[0]));
        check(Child.class.getDeclaredMethod("result").getGenericReturnType().getTypeName()
                .equals("fixture.Cell<I, O>"));
        check(Base.class.getDeclaredMethod("result").getGenericReturnType().getTypeName()
                .equals("fixture.Cell<P, ?>"));
        check(java.lang.reflect.Modifier.isFinal(Base.class.getDeclaredMethod("apply", Object.class).getModifiers()));
        check(Bounded.class.getTypeParameters()[0].getBounds()[0] == Tag.class);
        check(Bounded.class.getGenericSuperclass().getTypeName().equals("fixture.Base<T>"));
        check(Bounded.class.getDeclaredMethod("apply", Tag.class, Option.class)
                .getGenericParameterTypes()[0].equals(Bounded.class.getTypeParameters()[0]));
        int bridges = 0;
        for (Method method : Bounded.class.getDeclaredMethods()) {
            if (method.isBridge()) {
                check(method.isSynthetic() && method.getName().equals("apply") && method.getReturnType() == void.class
                        && method.getParameterTypes()[0] == Object.class && method.getParameterTypes()[1] == Option.class);
                ++bridges;
            }
        }
        check(bridges == 1);
        check(Variant.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Producer<L>"));
        check(Variant.class.getDeclaredMethod("upper").getGenericReturnType().getTypeName().equals("fixture.Cell<L, L>"));
        check(Variant.class.getDeclaredMethod("lower").getGenericReturnType().getTypeName().equals("fixture.Cell<L, L>"));
        check(Variant.class.getDeclaredMethod("array").getGenericReturnType().getTypeName().equals("fixture.Cell<L, L>[]"));
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=" + bridges);
        System.out.println("owned-formals-and-wildcard-returns=retained,final-overload-dispatch=matched,cast-order=matched");
    }
}
