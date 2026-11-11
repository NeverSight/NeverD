import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ArrayFixedReferenceResultHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned fixed reference array result");
    }
    static Object[] call(int route, int operation, Core<?> value) throws Throwable {
        if (route == 0) {
            if (operation == 0) return value.snapshot();
            if (operation == 1) return value.narrowed();
            value.ignored(); return null;
        }
        if (route == 1) {
            if (operation == 0) return ArrayFixedReferenceResultCaller.snapshot(value);
            if (operation == 1) return ArrayFixedReferenceResultCaller.narrowed(value);
            ArrayFixedReferenceResultCaller.ignored(value); return null;
        }
        String name = operation == 0 ? "snapshot" : operation == 1 ? "narrowed" : "ignored";
        if (route == 2) {
            try { return (Object[]) Core.class.getMethod(name).invoke(value); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        Class<?> result = operation == 0 ? Object[].class : operation == 1 ? String[].class : void.class;
        return (Object[]) MethodHandles.publicLookup().findVirtual(Core.class, name, MethodType.methodType(result)).invoke(value);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; attempt++) for (int operation = 0; operation < 3; operation++) {
            final Object first = new Object(), replacement = new Object();
            final Object[] values = mode == 4 || mode == 21 ? null : mode == 5 ? new Object[0]
                : mode == 2 || mode == 3 || mode == 9 ? new String[]{"owned-a", "owned-b"}
                : mode == 16 ? new Integer[]{17, 29} : new Object[]{first, null};
            final Object[] alternate = mode == 6 ? new String[]{"owned-replacement"}
                : mode == 7 || mode == 21 ? null : mode == 20 ? new Object[][]{{first}} : new Object[]{replacement};
            boolean reuse = mode != 1 && mode != 3 && mode != 5;
            Trace.events = 0; Trace.calls = 0; Trace.actions = 0; Trace.input = null; Trace.chosen = null;
            Trace.flag = false; Trace.override = mode == 6 || mode == 7 || mode == 20 || mode == 21;
            Trace.result = alternate; Trace.failAt = mode == 12 ? 1 : mode == 13 ? 2 : mode == 14 ? 3 : 0;
            Trace.failure = new RuntimeException("owned array failure"); Trace.error = new AssertionError("owned array error");
            Trace.fatal = false;
            final int scenario = mode;
            Trace.action = mode == 15 ? null : new Runnable() { public void run() {
                Trace.actions++;
                require(Trace.input == values && Trace.calls == 1 && Trace.chosen == null);
                if (scenario == 8 || scenario == 9) values[0] = replacement;
                if (scenario == 10) throw Trace.failure;
                if (scenario == 11) throw Trace.error;
                if (scenario == 17) { Trace.override = true; Trace.result = alternate; }
            }};
            Core<Object> core = mode == 18 ? null : new Core<Object>(values, reuse);
            Object[] answer = null; Throwable failure = null;
            try { answer = call(route, operation, core); } catch (Throwable caught) { failure = caught; }
            require(Trace.facadeInitializations == 0 && Trace.initializations == (mode == 18 ? 0 : 1));
            if (mode == 18) { require(failure instanceof NullPointerException && Trace.events == 0 && Trace.calls == 0); continue; }
            if (mode == 12) { require(failure == Trace.failure && Trace.events == 1 && Trace.calls == 0 && Trace.input == null); continue; }
            require(Trace.input == values && Trace.flag == reuse && Trace.calls == 1);
            if (mode == 15) { require(failure instanceof NullPointerException && Trace.events == 1 && Trace.actions == 0); continue; }
            require(Trace.actions == 1);
            if (mode == 9) { require(failure instanceof ArrayStoreException && Trace.events == 1 && "owned-a".equals(values[0])); continue; }
            if (mode == 10 || mode == 11) { require(failure == (mode == 10 ? Trace.failure : Trace.error) && Trace.events == 1); continue; }
            if (mode == 13) { require(failure == Trace.failure && Trace.events == 12 && Trace.chosen == null); continue; }
            if (mode == 4) { require(failure instanceof NullPointerException && Trace.events == 12 && Trace.chosen == null); continue; }
            Object[] chosen = Trace.chosen;
            if (mode == 6 || mode == 7 || mode == 17 || mode == 20 || mode == 21) require(chosen == alternate);
            else {
                require(chosen != null && chosen.getClass() == Object[].class && chosen.length == values.length);
                require((chosen == values) == (reuse && values.getClass() == Object[].class));
                for (int i = 0; i < values.length; i++) require(chosen[i] == values[i]);
            }
            if (mode == 14) { require(failure == Trace.failure && Trace.events == 123); continue; }
            if (operation == 1 && chosen != null && !(chosen instanceof String[]))
                require(failure instanceof ClassCastException && Trace.events == 1234);
            else {
                require(failure == null && Trace.events == (operation == 2 ? 12345 : 1234));
                require(answer == (operation == 2 ? null : chosen));
                if (operation == 0 && answer != null && answer.getClass() == Object[].class && answer.length != 0 && values != null) {
                    Object before = values[0]; answer[0] = replacement;
                    require(values[0] == (answer == values ? replacement : before));
                }
            }
        }
        System.out.println("attempts=2,operations=3,fixed-reference-array=matched");
    }
}
