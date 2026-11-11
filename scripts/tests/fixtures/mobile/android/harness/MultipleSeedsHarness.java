import fixture.Operation;
import fixture.State;
import fixture.Trace;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.util.Arrays;

public final class MultipleSeedsHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("multiple seed invocation changed");
    }
    static final class Width extends Operation {
        final State state;
        final Object[] next;
        final int mode;
        final RuntimeException failure = new IllegalStateException("owned width failure");
        final Error error = new AssertionError("owned width error");
        int calls;
        Width(State state, Object[] next, int mode) {
            this.state = state; this.next = next; this.mode = mode;
        }
        public int width() {
            calls++; Trace.hit(4);
            state.size = 91; state.operations = null;
            state.cells = mode == 15 ? null : next;
            if (mode == 8) throw failure;
            if (mode == 9) throw error;
            return 1;
        }
    }
    static void contract() throws Exception {
        Class<?> provider = Class.forName("fixture.Provider", false, MultipleSeedsHarness.class.getClassLoader());
        require(provider.getTypeParameters().length == 0);
        for (int count : new int[]{3, 4}) {
            Class<?>[] erased = new Class<?>[1 + 2 * count]; erased[0] = State.class;
            for (int i = 0; i < count; i++) { erased[1 + 2 * i] = int.class; erased[2 + 2 * i] = Object.class; }
            Method method = provider.getDeclaredMethod("write" + count, erased);
            TypeVariable<?>[] formals = method.getTypeParameters();
            Type[] parameters = method.getGenericParameterTypes();
            require(formals.length == count && parameters.length == erased.length && parameters[0] == State.class);
            for (int i = 0; i < count; i++) {
                require(formals[i].getName().equals("ABCD".substring(i, i + 1)));
                require(formals[i].getGenericDeclaration().equals(method) && formals[i].getBounds().length == 1
                    && formals[i].getBounds()[0] == Object.class);
                require(parameters[1 + 2 * i] == int.class && parameters[2 + 2 * i].equals(formals[i]));
                for (int j = 0; j < i; j++) require(!formals[i].equals(formals[j]));
            }
            require(method.getGenericReturnType() == void.class && method.getGenericExceptionTypes().length == 0);
            require(method.getModifiers() == (Modifier.PUBLIC | Modifier.STATIC | Modifier.FINAL));
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        require(route >= 0 && route < 4 && mode >= 0 && mode < 24);
        int count = route < 2 ? 3 : 4;
        contract();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.trace = 0; Trace.bits = 0;
            State state = new State(); state.size = 3; state.used = 1;
            Object oldMarker = new Object(); Object[] old = new Object[6]; Arrays.fill(old, oldMarker);
            boolean strings = mode == 12 || mode == 13 || mode == 20 || mode == 21;
            Object nextMarker = strings ? "old" : new Object();
            Object[] next = strings ? new String[6] : new Object[6];
            Arrays.fill(next, nextMarker); state.cells = mode == 16 ? null : old;
            Width width = new Width(state, next, mode); state.operations = new Operation[]{width};
            Object a = new int[]{attempt, mode}; Object b = new Object();
            Object c = new long[]{attempt, mode}; Object d = new StringBuilder("fourth");
            if (mode == 1) a = null;
            if (mode == 2) b = null;
            if (mode == 3) b = a;
            if (mode == 13) a = new String("first");
            if (mode == 17) c = b;
            if (mode == 20 || mode == 21) { a = new String("first"); b = new String("second"); }
            if (mode == 21) c = new String("third");
            if (mode == 22) c = null;
            if (mode == 23) d = null;
            if (mode == 5) state.operations = null;
            if (mode == 6) state.used = 0;
            if (mode == 7) state.operations[0] = null;
            int first = mode == 10 ? -8 : 0;
            int second = mode == 11 ? 8 : mode == 14 ? 0 : 1;
            int third = mode == 18 ? 8 : mode == 14 ? 0 : 2;
            int fourth = mode == 19 ? 8 : mode == 14 ? 0 : 3;
            Throwable caught = null;
            try { MultipleSeedsCaller.call(route, mode == 4 ? null : state, first, a, second, b, third, c, fourth, d); }
            catch (Throwable thrown) { caught = thrown; }
            boolean early = mode >= 4 && mode <= 7;
            boolean success = mode <= 3 || mode == 14 || mode == 16 || mode == 17 || mode >= 22
                || (count == 3 && (mode == 19 || mode == 21));
            if (success) require(caught == null);
            else if (mode == 8) require(caught == width.failure);
            else if (mode == 9) require(caught == width.error);
            else if (mode == 6 || mode == 10 || mode == 11 || mode == 18 || mode == 19)
                require(caught instanceof ArrayIndexOutOfBoundsException);
            else if (mode == 12 || mode == 13 || mode == 20 || mode == 21)
                require(caught instanceof ArrayStoreException);
            else require(caught instanceof NullPointerException);
            require(width.calls == (early ? 0 : 1));
            long expectedTrace = attempt == 0 ? 1 : 0;
            expectedTrace = expectedTrace * 10 + 2;
            if (!early) expectedTrace = expectedTrace * 10 + 4;
            if (success) expectedTrace = (expectedTrace * 10 + 6) * 10 + 7;
            require(Trace.trace == expectedTrace && Trace.bits == 0);
            for (Object value : old) require(value == oldMarker);
            Object[] expected = new Object[6]; Arrays.fill(expected, nextMarker);
            int completed = success ? count : (mode == 11 || mode == 13) ? 1
                : (mode == 18 || mode == 20) ? 2 : (count == 4 && (mode == 19 || mode == 21)) ? 3 : 0;
            int[] indices = {first, second, third, fourth}; Object[] values = {a, b, c, d};
            for (int i = 0; i < completed; i++) expected[2 + indices[i]] = values[i];
            for (int i = 0; i < next.length; i++) require(next[i] == expected[i]);
            require(state.size == (early ? 3 : 91));
            if (!early) require(state.operations == null && state.cells == (mode == 15 ? null : next));
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.trace);
        }
    }
}
