import fixture.Client;
import fixture.Pair;
import fixture.Provider;
import fixture.Sink;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.GenericArrayType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.WildcardType;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class PairedArrayVoidHarness {
    static void check(boolean condition) { if (!condition) throw new AssertionError("owned paired array void changed"); }
    static final class Element extends Pair<Object, Object> {
        Pair[] original, late;
        Sink other;
        Pair replacement;
        Object lateValue;
        int slot, firstCalls, secondCalls;
        Element(Object key, Object value, Object lateValue) { super(key, value); this.lateValue = lateValue; }
        @Override public Object component1() {
            firstCalls++; Trace.hit(3);
            original[slot] = replacement; second = lateValue;
            Trace.pairs = late; Trace.sink = other;
            return first;
        }
        @Override public Object component2() { secondCalls++; Trace.hit(4); return second; }
    }
    static class State {
        int calls, stores;
        Object[] keys = new Object[2], values = new Object[2];
        Object write(Object key, Object value) {
            int index = calls++; Trace.hit(5);
            keys[index] = key; values[index] = value; stores++;
            if (calls == 1) Trace.original[1] = Trace.next;
            Trace.hit(6); return this;
        }
    }
    static final class RawSink extends State implements Sink<Object, Object> {
        public Object put(Object key, Object value) { return write(key, value); }
    }
    static final class TypedSink extends State implements Sink<String, String> {
        public String put(String key, String value) { write(key, value); return "discarded"; }
    }
    static void metadata() throws Exception {
        Method method = Provider.class.getDeclaredMethod("apply", Sink.class, Pair[].class);
        check(Modifier.isPublic(method.getModifiers()) && Modifier.isStatic(method.getModifiers()) && !method.isSynthetic() && !method.isVarArgs());
        check(method.getParameterCount() == 2 && method.getExceptionTypes().length == 0 && method.getReturnType() == void.class && method.getGenericReturnType() == void.class);
        TypeVariable<?>[] formals = method.getTypeParameters(); check(formals.length == 2 && !formals[0].equals(formals[1]));
        for (TypeVariable<?> formal : formals) check(formal.getGenericDeclaration().equals(method) && formal.getBounds().length == 1 && formal.getBounds()[0] == Object.class);
        Type[] parameters = method.getGenericParameterTypes();
        check(parameters[0] instanceof ParameterizedType && parameters[1] instanceof GenericArrayType);
        ParameterizedType consumer = (ParameterizedType)parameters[0];
        Type component = ((GenericArrayType)parameters[1]).getGenericComponentType(); check(component instanceof ParameterizedType);
        ParameterizedType producer = (ParameterizedType)component;
        check(consumer.getRawType() == Sink.class && producer.getRawType() == Pair.class);
        Type[] from = producer.getActualTypeArguments(), to = consumer.getActualTypeArguments(); check(from.length == 2 && to.length == 2);
        for (int index = 0; index < 2; index++) {
            check(from[index] instanceof WildcardType && to[index] instanceof WildcardType);
            WildcardType output = (WildcardType)from[index], input = (WildcardType)to[index];
            check(output.getLowerBounds().length == 0 && output.getUpperBounds().length == 1 && output.getUpperBounds()[0].equals(formals[index]));
            check(input.getLowerBounds().length == 1 && input.getLowerBounds()[0].equals(formals[index]) && input.getUpperBounds().length == 1 && input.getUpperBounds()[0] == Object.class);
        }
    }
    static void invoke(int route) throws Throwable {
        if (route == 0) Client.call();
        else if (route == 1) PairedArrayVoidCaller.call();
        else if (route == 2) PairedArrayVoidCaller.direct();
        else {
            Trace.hit(9);
            if (route == 3) {
                try { Provider.class.getMethod("apply", Sink.class, Pair[].class).invoke(null, Trace.sink, Trace.pairs); }
                catch (InvocationTargetException failure) { throw failure.getCause(); }
            } else MethodHandles.lookup().findStatic(Provider.class, "apply", MethodType.methodType(void.class, Sink.class, Pair[].class))
                .invokeExact(Trace.sink, Trace.pairs);
            Trace.hit(8);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]); metadata();
        for (int attempt = 0; attempt < 2; attempt++) {
            Object key0 = mode == 16 || mode == 22 ? null : mode == 17 || mode == 18 ? new Object() : new String("key0");
            Object value0 = new String("before0");
            Object late0 = mode == 16 || mode == 22 ? null : mode == 17 || mode == 19 ? new Object() : new String("late0");
            Object key2 = new String("key2"), value2 = new String("before2"), late2 = mode == 21 ? new Object() : new String("late2");
            Element first = new Element(key0, value0, late0), next = new Element(key2, value2, late2);
            Pair initial = new Pair(new Object(), new Object()), replacement = new Pair(new Object(), new Object());
            Pair[] original = mode == 1 ? null : mode == 4 || mode == 5 ? new Pair[0] : new Pair[]{mode == 3 ? null : first, initial};
            Pair[] late = {replacement};
            RawSink other = new RawSink();
            State state = mode >= 18 ? new TypedSink() : new RawSink();
            Sink sink = (Sink)state;
            for (Element element : new Element[]{first, next}) { element.original = original; element.late = late; element.other = other; element.replacement = replacement; }
            first.slot = 0; next.slot = 1;
            Trace.original = original; Trace.next = next; Trace.pairs = original; Trace.sink = mode == 2 || mode == 5 ? null : sink;
            Trace.failure = new IllegalArgumentException("original"); Trace.error = new LinkageError("original");
            Trace.fail = mode >= 6 && mode <= 13 ? mode - 5 : mode == 14 ? -3 : mode == 15 ? -5 : 0;
            Trace.events = 0; Throwable failure = null;
            try { invoke(route); } catch (Throwable caught) { failure = caught; }
            boolean entered = mode != 6, nonempty = entered && mode != 7 && mode != 1 && mode != 3 && mode != 4 && mode != 5;
            boolean firstGetter = nonempty, firstReturned = firstGetter && mode != 8 && mode != 14;
            boolean secondGetter = firstReturned, secondReturned = secondGetter && mode != 9;
            boolean firstPut = secondReturned && mode != 2 && mode != 18 && mode != 19;
            boolean firstStored = firstPut && mode != 10 && mode != 15;
            boolean firstPutReturned = firstStored && mode != 11;
            boolean secondEntry = firstPutReturned && mode != 12;
            boolean secondPut = secondEntry && mode != 21;
            boolean finished = entered && (mode == 4 || mode == 5 || secondPut);
            long events = 9;
            if (attempt == 0) events = events * 11 + 1;
            if (entered) events = events * 11 + 2;
            if (firstGetter) events = events * 11 + 3;
            if (secondGetter) events = events * 11 + 4;
            if (firstPut) events = events * 11 + 5;
            if (firstStored) events = events * 11 + 6;
            if (firstPutReturned) events = events * 11 + 7;
            if (secondEntry) { events = events * 11 + 3; events = events * 11 + 4; }
            if (secondPut) { events = events * 11 + 5; events = events * 11 + 6; events = events * 11 + 7; }
            if (finished) events = events * 11 + 8;
            if (mode == 6 && attempt == 1) events = 9;
            check(Trace.events == events);
            check(first.firstCalls == (firstGetter ? 1 : 0) && first.secondCalls == (secondGetter ? 1 : 0));
            check(next.firstCalls == (secondEntry ? 1 : 0) && next.secondCalls == (secondEntry ? 1 : 0));
            check(first.second == (firstReturned ? late0 : value0) && next.second == (secondEntry ? late2 : value2));
            check(state.calls == (firstPut ? 1 : 0) + (secondPut ? 1 : 0) && state.stores == (firstStored ? 1 : 0) + (secondPut ? 1 : 0));
            check(state.keys[0] == (firstStored ? key0 : null) && state.values[0] == (firstStored ? late0 : null));
            check(state.keys[1] == (secondPut ? key2 : null) && state.values[1] == (secondPut ? late2 : null));
            check(other.calls == 0 && other.stores == 0 && late.length == 1 && late[0] == replacement);
            check(Trace.pairs == (firstReturned ? late : original) && Trace.sink == (firstReturned ? other : mode == 2 || mode == 5 ? null : sink));
            if (original != null && original.length != 0) check(original[0] == (firstReturned ? replacement : mode == 3 ? null : first)
                && original[1] == (secondEntry ? replacement : firstStored ? next : initial));
            if (mode == 6) check(attempt == 0 ? failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure : failure instanceof NoClassDefFoundError);
            else if (mode >= 7 && mode <= 13) check(failure == Trace.failure);
            else if (mode == 14 || mode == 15) check(failure == Trace.error);
            else if (mode == 1 || mode == 2 || mode == 3) check(failure instanceof NullPointerException);
            else if (mode == 18 || mode == 19 || mode == 21) check(failure instanceof ClassCastException);
            else check(failure == null);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
