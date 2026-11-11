import fixture.Child;
import fixture.Parent;
import fixture.Trace;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.util.AbstractSet;
import java.util.Iterator;
import java.util.Set;

public final class CheckedInterfaceHarness {
    private static final class SpySet extends AbstractSet<Object> {
        int queries;
        final Object element;
        SpySet(Object element) { this.element = element; }
        public int size() { ++queries; throw new AssertionError("unexpected size query"); }
        public Iterator<Object> iterator() { ++queries; throw new AssertionError("unexpected iterator query"); }
    }

    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned checked interface super changed");
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Class<?> type = Class.forName("fixture.Child", false, CheckedInterfaceHarness.class.getClassLoader());
        Constructor<?> constructor = type.getConstructor(Set.class);
        MethodHandle handle = MethodHandles.publicLookup().findConstructor(type, MethodType.methodType(void.class, Set.class));
        require(type.getTypeParameters().length == 1 && type.getGenericSuperclass().getTypeName().equals("fixture.Parent<E>"));
        require(constructor.getGenericParameterTypes()[0].getTypeName().equals("java.util.Set<? extends E>"));
        require(Parent.class.getConstructor(java.util.Collection.class).getGenericParameterTypes()[0].getTypeName()
                .equals("java.util.Collection<? extends E>"));
        for (int attempt = 0; attempt < 2; ++attempt) {
            SpySet value = new SpySet(mode == 2 ? Integer.valueOf(513) : new String("seed"));
            Set input = mode == 1 || mode == 11 ? null : value;
            Object replacement = new Object();
            int fail = mode >= 3 && mode <= 8 ? 1 + (mode - 3) / 2 : mode == 10 ? 2 : mode == 11 ? 3 : 0;
            Throwable failure = (mode == 4 || mode == 6 || mode == 8) ? new AssertionError("sentinel") : new IllegalStateException("sentinel");
            Trace.events = 0; Trace.fail = fail; Trace.failure = failure;
            Trace.input = input; Trace.replacement = replacement; Trace.seen = null; Trace.owner = null; Trace.wrong = false;
            Object result = null; Throwable caught = null;
            try {
                if (route == 0) result = CheckedInterfaceCaller.make(input);
                else if (route == 1) result = constructor.newInstance(input);
                else if (route == 2) result = handle.invoke(input);
                else result = new Child<String>(input);
            } catch (InvocationTargetException error) { caught = error.getCause(); }
              catch (Throwable error) { caught = error; }
            int events = attempt == 0 ? 89 : 0;
            for (int step = 1; step <= (fail == 0 ? 3 : fail); ++step) events = events * 10 + step;
            require(Trace.events == events && Trace.seen == input && Trace.input == replacement && !Trace.wrong);
            require(value.queries == 0 && value.element != null && caught == (fail == 0 ? null : failure));
            require((result == null) == (fail != 0));
            require((Trace.owner == null) == (fail == 1));
            if (Trace.owner != null) require(Trace.owner.impl == input);
            if (result != null) require(result == Trace.owner && ((Parent<?>)result).impl == input);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
