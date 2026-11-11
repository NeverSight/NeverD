import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class PlainNonReturningVoidHarness {
    private static int values, identities, states, failures;
    private static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    private static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    private static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    public static void main(String[] args) throws Throwable {
        String[] names = args.length == 0 ? new String[]{"dispatch", "schedule"} : args;
        Core first = new Core(), second = new Core();
        Object mark = new Object();
        Entry<Object> entry = new Entry<Object>(mark);
        RuntimeException markerFailure = new IllegalArgumentException("prefix");
        for (Object item : new Object[]{null, mark, entry}) {
            same(first.echo(item), item);
            same(second.echo(item), item);
        }
        value(Core.class.getTypeParameters().length == 0);
        value(Base.class.getMethod("echo", Object.class).getTypeParameters().length == 1);
        for (int action = 0; action < 2; action++) {
            String name = action == 0 ? "dispatch" : "schedule";
            Class<?>[] parameters = action == 0 ? new Class<?>[]{Object.class, Object.class}
                                               : new Class<?>[]{long.class, Entry.class};
            Method bridge = null, typed = null;
            for (Method method : Core.class.getDeclaredMethods()) {
                if (method.getName().equals(name) && method.getReturnType() == void.class) bridge = method;
                if (method.getName().equals(names[action]) && method.getReturnType() == Void.class) typed = method;
            }
            value(bridge != null && typed != null);
            value(typed.getTypeParameters().length == 0);
            value(typed.getGenericReturnType() == Void.class);
            value(bridge.isBridge() == (args.length == 0));
            value(bridge.isSynthetic() == (args.length == 0));
            if (action == 1) {
                value(typed.getGenericParameterTypes()[1].getTypeName().equals("fixture.Entry<? super java.lang.Throwable>"));
                value(bridge.getGenericParameterTypes()[1].getTypeName().equals(args.length == 0 ? "fixture.Entry" : "fixture.Entry<? super java.lang.Throwable>"));
            }
            MethodHandle bh = MethodHandles.lookup().findVirtual(Core.class, name,
                    MethodType.methodType(void.class, parameters));
            MethodHandle th = MethodHandles.lookup().findVirtual(Core.class, names[action],
                    MethodType.methodType(Void.class, parameters));
            for (int route = 0; route < 7; route++)
                for (Core receiver : new Core[]{first, second, null})
                    for (boolean prefixFailure : new boolean[]{false, true})
                        for (long delay : new long[]{0L, -1L, Long.MIN_VALUE, Long.MAX_VALUE})
                            for (Entry<Object> argument : new Entry[]{null, entry}) {
                                Effects.calls = 0;
                                Effects.receiver = mark;
                                Effects.failure = prefixFailure ? markerFailure : null;
                                Throwable failure = null;
                                try {
                                    if (action == 0) {
                                        if (route == 0) receiver.dispatch(argument, mark);
                                        else if (route == 1) ((Base)receiver).dispatch(argument, mark);
                                        else if (route == 2) PlainNonReturningVoidCaller.dispatch(receiver, argument, mark);
                                        else if (route == 3) bridge.invoke(receiver, argument, mark);
                                        else if (route == 4) bh.invokeExact(receiver, (Object)argument, mark);
                                        else if (route == 5) typed.invoke(receiver, argument, mark);
                                        else { Void unused = (Void)th.invokeExact(receiver, (Object)argument, mark); }
                                    } else {
                                        if (route == 0) receiver.schedule(delay, argument);
                                        else if (route == 1) ((Api)receiver).schedule(delay, argument);
                                        else if (route == 2) PlainNonReturningVoidCaller.schedule(receiver, delay, argument);
                                        else if (route == 3) bridge.invoke(receiver, delay, argument);
                                        else if (route == 4) bh.invokeExact(receiver, delay, (Entry)argument);
                                        else if (route == 5) typed.invoke(receiver, delay, argument);
                                        else { Void unused = (Void)th.invokeExact(receiver, delay, (Entry)argument); }
                                    }
                                } catch (InvocationTargetException error) { failure = error.getCause(); }
                                catch (Throwable error) { failure = error; }
                                if (receiver == null) {
                                    state(failure instanceof NullPointerException);
                                    same(Effects.receiver, mark);
                                    state(Effects.calls == 0);
                                } else {
                                    if (prefixFailure) same(failure, markerFailure);
                                    else state(failure != null && failure.getClass() == Failure.class);
                                    same(Effects.receiver, receiver);
                                    state(Effects.calls == 1);
                                }
                                same(entry.value, mark);
                                same(Effects.failure, prefixFailure ? markerFailure : null);
                                failures++;
                            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("ancestor-and-interface-dispatch=matched,private-prefix-effects=retained,source-name-and-bridge-metadata=reported");
    }
}
