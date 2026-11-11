import fixture.Arity;
import fixture.Base;
import fixture.Child;
import fixture.Marker;
import fixture.Producer;
import fixture.Value;
import java.lang.reflect.Method;

public class MarkerInheritanceHarness {
    static int identities;
    static int failures;
    static void check(boolean condition) {
        if (!condition) throw new AssertionError();
    }
    static void same(Object actual, Object expected) {
        check(actual == expected);
        ++identities;
    }
    static class Override extends Child {
        Value selected;
        Override(Value value) { super(null); selected = value; }
        public Value read() { ++reads; return selected; }
        public Value echo(Value value) { ++echoes; return selected; }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Object first = new Object();
        Object second = new Object();
        Value a = new Value(first);
        Value b = new Value(second);
        Child child = new Child(a);
        Producer raw = child;
        for (Value selected : new Value[] {a, b, null, a, b, a}) {
            child.value = selected;
            same(child.read(), selected);
            same(raw.read(), selected);
            same(child.echo(selected), selected);
            same(raw.echo(selected), selected);
        }
        check(child.reads == 12 && child.echoes == 12);
        a.payload = second;
        child.value = a;
        same(((Value) raw.read()).payload, second);
        RuntimeException sentinel = new IllegalStateException();
        child.failure = sentinel;
        for (Object invalid : new Object[] {first, "wrong", Integer.valueOf(7)}) {
            int before = child.echoes;
            try {
                raw.echo(invalid);
                throw new AssertionError();
            } catch (ClassCastException expected) {
                check(child.echoes == before);
                ++failures;
            }
        }
        for (Value selected : new Value[] {a, b, null}) {
            int before = child.echoes;
            try {
                raw.echo(selected);
                throw new AssertionError();
            } catch (RuntimeException actual) {
                same(actual, sentinel);
                check(child.echoes == before + 1);
                ++failures;
            }
        }
        int before = child.reads;
        try {
            raw.read();
            throw new AssertionError();
        } catch (RuntimeException actual) {
            same(actual, sentinel);
            check(child.reads == before + 1);
            ++failures;
        }
        child.failure = null;
        raw = new Override(b);
        same(raw.read(), b);
        same(raw.echo(a), b);
        check(((Child) raw).reads == 1 && ((Child) raw).echoes == 1);
        check(child.getArity() == 0 && ((Arity) child).getArity() == 0);
        check(new Base<Object>(17).getArity() == 17);
        check(child instanceof Marker && child instanceof java.io.Serializable);
        check(Marker.class.getDeclaredMethods().length == 0);
        check(Marker.class.getDeclaredFields().length == 0);
        check(Producer.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Marker<T>"));
        check(Arity.class.getGenericInterfaces()[0].getTypeName().equals("fixture.ArityMarker<R>"));
        check(Base.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Arity<R>"));
        check(Base.class.getTypeParameters()[0].getName().equals("R"));
        check(Child.class.getGenericSuperclass() == Base.class);
        check(Child.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Producer<fixture.Value>"));
        int bridges = 0;
        for (Method method : Child.class.getDeclaredMethods()) {
            if (method.isBridge()) {
                check(method.isSynthetic());
                check(method.getReturnType() == Object.class);
                ++bridges;
            }
        }
        check(bridges == 2);
        System.out.println("identities=" + identities + ",failures=" + failures + ",bridges=" + bridges);
        System.out.println("marker-reflection=retained,unused-formals=retained,raw-superclass=retained");
    }
}
