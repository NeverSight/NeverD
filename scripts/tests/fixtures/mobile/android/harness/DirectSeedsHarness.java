import fixture.Operation;
import fixture.State;
import fixture.Trace;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.util.Arrays;

public final class DirectSeedsHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("direct seed invocation changed");
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
        Class<?> provider = Class.forName("fixture.Provider", false, DirectSeedsHarness.class.getClassLoader());
        require(provider.getTypeParameters().length == 0);
        for (boolean wide : new boolean[]{false, true}) {
            Class<?>[] erased = wide
                ? new Class<?>[]{State.class, int.class, Object.class, int.class, Object.class, long.class}
                : new Class<?>[]{State.class, int.class, Object.class, int.class, Object.class};
            Method method = provider.getDeclaredMethod(wide ? "writeWide" : "write", erased);
            TypeVariable<?>[] formals = method.getTypeParameters();
            require(formals.length == 2 && formals[0].getName().equals("A") && formals[1].getName().equals("B"));
            require(!formals[0].equals(formals[1]));
            for (TypeVariable<?> formal : formals)
                require(formal.getGenericDeclaration().equals(method) && formal.getBounds().length == 1
                    && formal.getBounds()[0] == Object.class);
            Type[] parameters = method.getGenericParameterTypes();
            require(parameters.length == erased.length && parameters[0] == State.class && parameters[1] == int.class
                && parameters[2].equals(formals[0]) && parameters[3] == int.class && parameters[4].equals(formals[1]));
            if (wide) require(parameters[5] == long.class);
            require(method.getGenericReturnType() == void.class && method.getGenericExceptionTypes().length == 0);
            require(method.getModifiers() == (Modifier.PUBLIC | Modifier.STATIC | Modifier.FINAL));
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        require(route >= 0 && route < 4 && mode >= 0 && mode < 18);
        contract();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.trace = 0; Trace.bits = 0;
            State state = new State(); state.size = 3; state.used = 1;
            Object oldMarker = new Object(); Object[] old = new Object[5]; Arrays.fill(old, oldMarker);
            Object nextMarker = (mode == 12 || mode == 13) ? "old" : new Object();
            Object[] next = (mode == 12 || mode == 13) ? new String[5] : new Object[5];
            Arrays.fill(next, nextMarker); state.cells = mode == 16 ? null : old;
            Width width = new Width(state, next, mode); state.operations = new Operation[]{width};
            Object a = new int[]{attempt, mode}; Object b = new Object();
            if (mode == 1) a = null;
            if (mode == 2) b = null;
            if (mode == 3) b = a;
            if (mode == 13) a = new String("first");
            if (mode == 5) state.operations = null;
            if (mode == 6) state.used = 0;
            if (mode == 7) state.operations[0] = null;
            int first = mode == 10 ? -8 : 0, second = mode == 11 ? 8 : mode == 14 ? 0 : 1;
            long bits = 0x8123456789abcdefL ^ mode;
            Throwable caught = null;
            try { DirectSeedsCaller.call(route, mode == 4 ? null : state, first, a, second, b, bits); }
            catch (Throwable thrown) { caught = thrown; }
            boolean early = mode >= 4 && mode <= 7;
            boolean success = mode <= 3 || mode == 14 || mode >= 16;
            if (success) require(caught == null);
            else if (mode == 8) require(caught == width.failure);
            else if (mode == 9) require(caught == width.error);
            else if (mode == 6 || mode == 10 || mode == 11) require(caught instanceof ArrayIndexOutOfBoundsException);
            else if (mode == 12 || mode == 13) require(caught instanceof ArrayStoreException);
            else require(caught instanceof NullPointerException);
            require(width.calls == (early ? 0 : 1));
            long expectedTrace = attempt == 0 ? 1 : 0;
            expectedTrace = expectedTrace * 10 + 2;
            if (!early) expectedTrace = expectedTrace * 10 + 4;
            if (success) expectedTrace = (expectedTrace * 10 + 6) * 10 + 7;
            require(Trace.trace == expectedTrace && Trace.bits == (route >= 2 ? bits : 0));
            for (Object value : old) require(value == oldMarker);
            Object[] expected = new Object[5]; Arrays.fill(expected, nextMarker);
            if (success) { expected[2] = a; expected[mode == 14 ? 2 : 3] = b; }
            if (mode == 11 || mode == 13) expected[2] = a;
            for (int i = 0; i < next.length; i++) require(next[i] == expected[i]);
            require(state.size == (early ? 3 : 91));
            if (!early) require(state.operations == null && state.cells == (mode == 15 ? null : next));
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.trace);
        }
    }
}
