import fixture.*;
import java.lang.reflect.*;
public final class ClosedSuperParametersHarness {
    static int mode;
    static Object marker;
    static Throwable failure;
    static void check(boolean v) { if (!v) throw new AssertionError("owned closed super parameters"); }
    static final class FutureCore extends Core {
        int traps;
        public void deliver(Payload value) { traps++; throw new AssertionError("virtual redispatch"); }
    }
    static final class FutureProbe implements Probe {
        int calls; Object value;
        public Object observe(Object value) {
            calls++; this.value = value;
            if (mode == 4) throw (RuntimeException) failure;
            if (mode == 9) throw (Error) failure;
            if (mode == 8 && value != null) ((Payload) value).value = new String("mutated");
            return marker;
        }
    }
    public static void main(String[] args) throws Exception {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        check(((ParameterizedType) Core.class.getGenericSuperclass()).getActualTypeArguments()[0] == Payload.class);
        for (int attempt = 0; attempt < 2; attempt++) {
            Core.events = 0; failure = mode == 9 ? new AssertionError("owned callback error") : new RuntimeException("owned callback");
            RuntimeException providerFailure = new RuntimeException("owned provider");
            FutureCore owner = new FutureCore(); FutureProbe probe = new FutureProbe();
            Core supplied = mode == 1 ? null : owner; owner.probe = mode == 3 ? null : probe;
            marker = mode == 7 || mode == 13 ? null : new Object(); Object sentinel = new Object(); owner.marker = sentinel;
            owner.failure = mode == 5 ? providerFailure : null;
            Payload initial = new Payload(new Object()); owner.last = initial;
            Payload input = mode == 2 ? null : new Payload(mode == 10 ? new Integer(Integer.MIN_VALUE + attempt) : mode == 11 ? new Integer(Integer.MAX_VALUE - attempt) : new String("payload" + attempt));
            boolean bad = mode == 6 && route % 2 == 1;
            Object argument = bad ? new Object() : input; Throwable caught = null;
            try {
                if (route == 0) supplied.submit(input);
                else if (route == 1) supplied.checked(argument);
                else if (route == 2) Core.class.getMethod("submit", Payload.class).invoke(supplied, input);
                else Core.class.getMethod("checked", Object.class).invoke(supplied, argument);
            } catch (Throwable error) { caught = error instanceof InvocationTargetException ? error.getCause() : error; }
            boolean success = mode != 1 && mode != 3 && mode != 4 && mode != 5 && mode != 9 && !bad;
            if (success) check(caught == null);
            else if (mode == 1 || mode == 3) check(caught instanceof NullPointerException);
            else if (bad) check(caught instanceof ClassCastException);
            else check(caught == (mode == 5 ? providerFailure : failure));
            int events = mode == 1 ? 0 : bad ? 3 : success ? 414 : 16;
            boolean reached = mode != 1 && !bad;
            boolean called = reached && mode != 3;
            check(Core.events == events && owner.traps == 0 && owner.stage == (mode == 1 ? 0 : success ? 2 : 1));
            check(probe.calls == (called ? 1 : 0) && probe.value == (called ? input : null));
            check(owner.last == (reached ? input : initial));
            check(owner.marker == (called && mode != 4 && mode != 9 ? marker : sentinel));
            if (mode == 8) check(input.value instanceof String && input.value.equals("mutated"));
            if (success) {
                ((Base) owner).last = new Object(); Throwable late = null;
                try { Payload value = owner.last; check(value == input); } catch (Throwable error) { late = error; }
                check(late instanceof ClassCastException && Core.events == events && owner.traps == 0);
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
