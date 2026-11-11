import fixture.Container;
import fixture.ContainerChoice;
import fixture.Fallback;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.WildcardType;

public final class InterfaceBoundsHarness {
    private static int queries, callbacks;
    private static int sequence;
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("Interface bound behavior changed");
    }
    private static Container<Object> container(boolean empty) {
        return new Container<Object>() {
            public boolean isEmpty() {
                ++queries;
                sequence = sequence * 10 + 1;
                return empty;
            }
        };
    }
    public static void main(String[] args) throws Exception {
        Container<Object> empty = container(true), full = container(false);
        int probes = 0;
        for (Object value : new Object[] {new Object(), "fallback", new String[] {"array"}, null}) {
            Fallback<Object> callback = new Fallback<Object>() {
                public Object get() {
                    ++callbacks;
                    sequence = sequence * 10 + 2;
                    return value;
                }
            };
            for (Container<Object> input : new Container[] {empty, full}) {
                sequence = 0;
                int before = callbacks;
                require(ContainerChoice.call(input, callback) == (input == empty ? value : input));
                require(callbacks == before + (input == empty ? 1 : 0));
                require(sequence == (input == empty ? 12 : 1));
                ++probes;
            }
        }
        RuntimeException failure = new RuntimeException("owned sentinel");
        Fallback<Object> throwing = new Fallback<Object>() {
            public Object get() { ++callbacks; throw failure; }
        };
        require(ContainerChoice.call(full, throwing) == full && callbacks == 4);
        try {
            ContainerChoice.call(empty, throwing);
            throw new AssertionError("missing callback failure");
        } catch (RuntimeException actual) { require(actual == failure && callbacks == 5); }
        Container<Object> failedQuery = new Container<Object>() {
            public boolean isEmpty() { ++queries; throw failure; }
        };
        try {
            ContainerChoice.call(failedQuery, throwing);
            throw new AssertionError("missing query failure");
        } catch (RuntimeException actual) { require(actual == failure && callbacks == 5); }
        require(ContainerChoice.call(full, null) == full);
        try {
            ContainerChoice.call(empty, null);
            throw new AssertionError("missing null callback failure");
        } catch (NullPointerException expected) {}
        try {
            ContainerChoice.call(null, throwing);
            throw new AssertionError("missing null input failure");
        } catch (NullPointerException expected) { require(callbacks == 5 && queries == 13); }
        Method method = ContainerChoice.class.getDeclaredMethod("choose", Container.class, Fallback.class);
        require(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()));
        require(method.getReturnType() == Object.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        if (args[0].equals("original")) {
            require(formals.length == 2 && formals[0].getBounds().length == 2);
            ParameterizedType bound = (ParameterizedType) formals[0].getBounds()[0];
            require(bound.getRawType() == Container.class && bound.getActualTypeArguments().length == 1);
            WildcardType argument = (WildcardType) bound.getActualTypeArguments()[0];
            require(argument.getLowerBounds().length == 0 && argument.getUpperBounds().length == 1
                    && argument.getUpperBounds()[0] == Object.class);
            require(formals[0].getBounds()[1].equals(formals[1]));
            require(formals[1].getBounds().length == 1 && formals[1].getBounds()[0] == Object.class);
        } else require(formals.length == 0);
        System.out.println("identity-probes=" + probes + ";queries=" + queries + ";callbacks=" + callbacks
                + ";callback-failures=1;query-failures=1;null-failures=2");
    }
}
