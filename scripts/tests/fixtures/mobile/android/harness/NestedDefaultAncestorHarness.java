import fixture.Api;
import fixture.Owner;
import fixture.Reader;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

public final class NestedDefaultAncestorHarness {
    private enum Choice { VALUE }

    private static final class Child extends Reader<Object> {
        @Override
        public void touch(Class<?> type, Object key) {
            stamp += 3;
            super.touch(type, key);
        }
    }

    public static void main(String[] args) throws Throwable {
        RecursiveEnumDefaultHarness.main(args);
        if (Owner.class.getTypeParameters().length != 1
                || Owner.State.class.getTypeParameters().length != 0
                || Owner.State.class.getDeclaringClass() != Owner.class
                || !Modifier.isStatic(Owner.State.class.getModifiers())
                || Reader.class.getGenericSuperclass() != Owner.State.class)
            throw new AssertionError("static ancestor borrowed enclosing generic scope");
        Method choose = Reader.class.getMethod("choose", Class.class, Object.class, Enum.class);
        if (choose.getDeclaringClass() != Api.class || !choose.isDefault())
            throw new AssertionError("default body was copied into class ancestor");
        Reader<Object> reader = new Reader<>();
        if (reader.state() != 7) throw new AssertionError("ancestor initialization changed");
        Owner.State alias = reader;
        alias.stamp = 11;
        Object key = new Object();
        if (reader.choose(Choice.class, key, Choice.VALUE) != Choice.VALUE
                || reader.seen != key || reader.type != Choice.class || reader.state() != 11)
            throw new AssertionError("inherited default changed ancestor aliases");
        Child child = new Child();
        if (child.choose(Choice.class, key, Choice.VALUE) != Choice.VALUE || child.state() != 10)
            throw new AssertionError("default lost original virtual dispatch");
        RuntimeException failure = new IllegalStateException("owned nested ancestor failure");
        child.failure = failure;
        try {
            child.choose(Choice.class, key, Choice.VALUE);
            throw new AssertionError("default swallowed original failure");
        } catch (RuntimeException problem) {
            if (problem != failure || child.state() != 13 || child.calls != 2)
                throw new AssertionError("default reordered virtual effects before failure");
        }
        try {
            choose.invoke(child, Choice.class, key, Choice.VALUE);
            throw new AssertionError("reflected default swallowed original failure");
        } catch (InvocationTargetException problem) {
            if (problem.getCause() != failure || child.state() != 16 || child.calls != 3)
                throw new AssertionError("reflected default changed virtual effects or failure identity");
        }
        System.out.println("named-static-ancestor-scope=independent,initialization-aliases-and-virtual-effects=matched");
    }
}
