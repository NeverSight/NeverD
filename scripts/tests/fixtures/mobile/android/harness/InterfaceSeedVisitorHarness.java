import fixture.*;
import fixture.Node.Companion.Action;
import java.lang.reflect.*;
public final class InterfaceSeedVisitorHarness {
    static void check(boolean condition) {
        if (!condition) throw new AssertionError("owned interface seed visitor");
    }
    static final class First implements Node {}
    static final class Second implements Node {}
    static final class Callback implements Fn<Object,Object> {
        final Node expected, late;
        final Object result;
        int calls;
        Callback(Node expected, Node late, Object result) { this.expected = expected; this.late = late; this.result = result; }
        public Object apply(Object value) {
            check(value == expected && Trace.seed == expected);
            calls++; Trace.seed = late; Trace.produced = Action.SKIP; Trace.step(2);
            return result;
        }
    }
    static void metadata() throws Exception {
        Method method = Class.forName("fixture.Provider", false, InterfaceSeedVisitorHarness.class.getClassLoader())
            .getDeclaredMethod("visit", Node.class, Fn.class);
        check(method.getTypeParameters().length == 1 && method.getGenericReturnType() == void.class);
        TypeVariable<?> formal = method.getTypeParameters()[0];
        check(formal.getGenericDeclaration().equals(method) && formal.getBounds().length == 1 && formal.getBounds()[0] == Node.class);
        Type[] parameters = method.getGenericParameterTypes();
        check(parameters[0].equals(formal));
        ParameterizedType callback = (ParameterizedType)parameters[1];
        check(callback.getRawType() == Fn.class);
        WildcardType input = (WildcardType)callback.getActualTypeArguments()[0], output = (WildcardType)callback.getActualTypeArguments()[1];
        check(input.getLowerBounds().length == 1 && input.getLowerBounds()[0].equals(formal));
        check(output.getUpperBounds().length == 1 && output.getUpperBounds()[0] == Action.class);
        check(Action.class.isEnum() && Modifier.isStatic(Action.class.getModifiers()));
        check(Action.class.getEnclosingClass() == Node.Companion.class && Node.Companion.class.getEnclosingClass() == Node.class);
        Action[] first = Action.values(), second = Action.values();
        check(first != second && first.length == 3 && second.length == 3);
        check(first[0] == Action.NEXT && first[1] == Action.STOP && first[2] == Action.SKIP);
        first[0] = null;
        check(Action.values()[0] == Action.NEXT && Action.valueOf("STOP") == Action.STOP);
        check(Action.NEXT.ordinal() == 0 && Action.STOP.ordinal() == 1 && Action.SKIP.ordinal() == 2);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]); metadata();
        for (int attempt = 0; attempt < 2; attempt++) {
            Node start = mode == 1 ? null : mode == 13 ? new Second() : new First(), late = new Second();
            Object result = mode == 3 ? null : mode == 4 ? Action.STOP : mode == 5 ? Action.SKIP
                : mode == 12 ? new Object() : mode == 14 ? "owned wrong result" : mode == 15 ? new Object[1] : Action.NEXT;
            Callback callback = new Callback(start, late, result);
            Trace.events = 0; Trace.before = Trace.after = Trace.seed = null; Trace.choice = Trace.produced = null;
            Trace.failure = new IllegalArgumentException("owned failure"); Trace.error = new LinkageError("owned error");
            Trace.failAt = mode == 6 ? 1 : mode == 7 ? -1 : mode == 8 ? 2 : mode == 9 ? -2 : mode == 10 ? 3 : mode == 11 ? -3 : 0;
            Throwable caught = null;
            try { InterfaceSeedVisitorCaller.call(route, start, mode == 2 ? null : callback); }
            catch (Throwable failure) { caught = failure; }
            boolean invoked = mode != 2 && mode != 6 && mode != 7;
            boolean completed = invoked && mode != 8 && mode != 9 && mode != 12 && mode != 14 && mode != 15;
            if (Trace.failAt > 0) check(caught == Trace.failure);
            else if (Trace.failAt < 0) check(caught == Trace.error);
            else if (mode == 2) check(caught instanceof NullPointerException);
            else if (mode == 12 || mode == 14 || mode == 15) check(caught instanceof ClassCastException);
            else check(caught == null);
            check(callback.calls == (invoked ? 1 : 0) && Trace.before == start);
            check(Trace.seed == (invoked ? late : start));
            check(Trace.after == (completed ? start : null) && Trace.choice == (completed ? result : null));
            check(Trace.produced == (invoked ? Action.SKIP : null));
            int events = attempt == 0 ? 4 : 0;
            events = events * 10 + 1;
            if (invoked) events = events * 10 + 2;
            if (completed) events = events * 10 + 3;
            check(Trace.events == events);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
