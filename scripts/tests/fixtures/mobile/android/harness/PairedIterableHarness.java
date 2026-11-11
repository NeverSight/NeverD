import fixture.Client;
import fixture.Pair;
import fixture.Provider;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.WildcardType;
import java.util.AbstractMap;
import java.util.Collections;
import java.util.Iterator;
import java.util.Map;
import java.util.Set;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class PairedIterableHarness {
    static final boolean VOID_ENTRY = false;
    static void check(boolean condition) { if (!condition) throw new AssertionError("owned paired iterable transfer changed"); }
    static final class Rows implements Iterable {
        Pair[] original;
        int iterators, hasCalls, nextCalls, position;
        Rows(Pair[] original) { this.original = original; }
        public Iterator iterator() {
            iterators++; Trace.hit(12);
            return new Iterator() {
                public boolean hasNext() {
                    hasCalls++; Trace.hit(13);
                    if (Trace.mode == 28 && hasCalls == 2) throw Trace.failure;
                    return position < original.length;
                }
                public Object next() {
                    nextCalls++; Trace.hit(14);
                    Object result = original[position++];
                    return Trace.mode == 24 ? new Object() : result;
                }
                public void remove() { throw new UnsupportedOperationException(); }
            };
        }
    }
    static final class Element extends Pair<Object, Object> {
        Pair[] original;
        Rows late;
        Map other;
        Pair replacement;
        Object lateValue;
        int slot, firstCalls, secondCalls;
        Element(Object key, Object value, Object lateValue) { super(key, value); this.lateValue = lateValue; }
        @Override public Object component1() {
            firstCalls++; Trace.hit(3);
            original[slot] = replacement; second = lateValue;
            Trace.rows = late; Trace.sink = other;
            return first;
        }
        @Override public Object component2() { secondCalls++; Trace.hit(4); return second; }
    }
    static final class State {
        int calls, stores, sizeCalls;
        Object[] keys = new Object[2], values = new Object[2];
        Rows late;
        Map destination;
        Object write(Object key, Object value) {
            int index = calls++; Trace.hit(5);
            keys[index] = key; values[index] = value; stores++;
            if (calls == 1) {
                Trace.original[1] = Trace.next;
                if (Trace.mode == 32) Trace.returned = Trace.alternate;
            }
            Trace.hit(6); return this;
        }
        int size() {
            sizeCalls++; Trace.hit(10);
            Trace.rows = late; Trace.output = destination;
            return 17;
        }
    }
    static final class RawMap extends AbstractMap<Object, Object> {
        final State state;
        RawMap(State state) { this.state = state; }
        public Object put(Object key, Object value) { return state.write(key, value); }
        public int size() { return state.size(); }
        public Set<Map.Entry<Object, Object>> entrySet() { return Collections.emptySet(); }
    }
    static final class TypedMap extends AbstractMap<String, String> {
        final State state;
        TypedMap(State state) { this.state = state; }
        public String put(String key, String value) { state.write(key, value); return "discarded"; }
        public int size() { return state.size(); }
        public Set<Map.Entry<String, String>> entrySet() { return Collections.emptySet(); }
    }
    static void metadata() throws Exception {
        Method method = Provider.class.getDeclaredMethod(VOID_ENTRY ? "write" : "merge", Map.class, Iterable.class);
        check(Modifier.isPublic(method.getModifiers()) && Modifier.isStatic(method.getModifiers()) && !method.isSynthetic() && !method.isVarArgs());
        check(method.getParameterCount() == 2 && method.getExceptionTypes().length == 0);
        TypeVariable<?>[] formals = method.getTypeParameters(); check(formals.length == 2 && !formals[0].equals(formals[1]));
        for (TypeVariable<?> formal : formals) check(formal.getGenericDeclaration().equals(method) && formal.getBounds().length == 1 && formal.getBounds()[0] == Object.class);
        Type[] parameters = method.getGenericParameterTypes();
        check(parameters[0] instanceof ParameterizedType && parameters[1] instanceof ParameterizedType);
        ParameterizedType map = (ParameterizedType)parameters[0], envelope = (ParameterizedType)parameters[1];
        check(map.getRawType() == Map.class && envelope.getRawType() == Iterable.class && envelope.getActualTypeArguments().length == 1);
        Type item = envelope.getActualTypeArguments()[0]; check(item instanceof WildcardType);
        WildcardType outer = (WildcardType)item;
        check(outer.getLowerBounds().length == 0 && outer.getUpperBounds().length == 1 && outer.getUpperBounds()[0] instanceof ParameterizedType);
        ParameterizedType pair = (ParameterizedType)outer.getUpperBounds()[0]; check(pair.getRawType() == Pair.class);
        Type[] from = pair.getActualTypeArguments(), to = map.getActualTypeArguments(); check(from.length == 2 && to.length == 2);
        for (int index = 0; index < 2; index++) {
            check(from[index] instanceof WildcardType && to[index] instanceof WildcardType);
            WildcardType producer = (WildcardType)from[index], mapSlot = (WildcardType)to[index];
            check(producer.getLowerBounds().length == 0 && producer.getUpperBounds().length == 1 && producer.getUpperBounds()[0].equals(formals[index]));
            check(VOID_ENTRY ? mapSlot.getLowerBounds().length == 1 && mapSlot.getLowerBounds()[0].equals(formals[index]) && mapSlot.getUpperBounds().length == 1 && mapSlot.getUpperBounds()[0] == Object.class
                            : mapSlot.getLowerBounds().length == 0 && mapSlot.getUpperBounds().length == 1 && mapSlot.getUpperBounds()[0].equals(formals[index]));
        }
        if (VOID_ENTRY) check(method.getReturnType() == void.class && method.getGenericReturnType() == void.class);
        else {
            check(method.getReturnType() == Map.class && method.getGenericReturnType() instanceof ParameterizedType);
            ParameterizedType result = (ParameterizedType)method.getGenericReturnType();
            Type[] slots = result.getActualTypeArguments();
            check(result.getRawType() == Map.class && slots.length == 2 && slots[0].equals(formals[0]) && slots[1].equals(formals[1]));
        }
    }
    static Object invoke(int route) throws Throwable {
        if (route == 0) return Client.call();
        if (route == 1) return PairedIterableCaller.call();
        if (route == 2) return PairedIterableCaller.direct();
        Trace.hit(9); Map result;
        if (route == 3) {
            try { result = (Map)Provider.class.getMethod("merge", Map.class, Iterable.class).invoke(null, Trace.input, Trace.rows); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        } else result = (Map)MethodHandles.lookup().findStatic(Provider.class, "merge", MethodType.methodType(Map.class, Map.class, Iterable.class))
            .invokeExact(Trace.input, Trace.rows);
        Trace.after = result; Trace.hit(8); return result;
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
            Pair[] original = mode == 4 || mode == 5 ? new Pair[0] : new Pair[]{mode == 3 ? null : first, initial};
            Rows rows = mode == 1 ? null : new Rows(original), late = new Rows(new Pair[]{replacement});
            State state = new State(), otherState = new State(), inputState = new State();
            Map other = new RawMap(otherState), input = new RawMap(inputState);
            Map sink = mode >= 18 && mode <= 22 ? new TypedMap(state) : new RawMap(state);
            Map destination = mode == 2 || mode == 5 ? null : sink;
            inputState.late = late; inputState.destination = destination;
            for (Element element : new Element[]{first, next}) { element.original = original; element.late = late; element.other = other; element.replacement = replacement; }
            first.slot = 0; next.slot = 1;
            Trace.input = mode == 23 ? null : input; Trace.sink = destination; Trace.output = other;
            Trace.returned = mode == 31 ? null : new RawMap(new State()); Trace.alternate = new RawMap(new State());
            Map originalResult = Trace.returned;
            Trace.original = original; Trace.next = next; Trace.rows = rows; Trace.firstCount = -1;
            Object sentinel = new Object(); Trace.after = sentinel;
            Trace.failure = new IllegalArgumentException("original"); Trace.error = new LinkageError("original"); Trace.mode = mode;
            Trace.fail = mode >= 6 && mode <= 13 ? mode - 5 : mode == 14 ? -3 : mode == 15 ? -5 : mode == 25 ? 12 : mode == 26 ? 13 : mode == 27 ? 14 : mode == 29 ? 10 : mode == 30 ? 11 : 0;
            Trace.events = ""; Throwable failure = null; Object actual = null;
            try { actual = invoke(route); } catch (Throwable caught) { failure = caught; }
            boolean initialized = mode != 6;
            boolean sized = !VOID_ENTRY && initialized && mode != 23;
            boolean sizeReturned = sized && mode != 29;
            boolean entered = initialized && (VOID_ENTRY || sizeReturned);
            boolean iterator = entered && mode != 7 && mode != 1;
            boolean hasFirst = iterator && mode != 25;
            boolean nextFirst = hasFirst && mode != 26 && mode != 4 && mode != 5;
            boolean nextReturned = nextFirst && mode != 27;
            boolean firstGetter = nextReturned && mode != 24 && mode != 3;
            boolean firstReturned = firstGetter && mode != 8 && mode != 14;
            boolean secondGetter = firstReturned, secondReturned = secondGetter && mode != 9;
            boolean firstPut = secondReturned && mode != 2 && mode != 18 && mode != 19;
            boolean firstStored = firstPut && mode != 10 && mode != 15;
            boolean firstPutReturned = firstStored && mode != 11;
            boolean hasSecond = firstPutReturned && mode != 12;
            boolean secondEntry = hasSecond && mode != 28;
            boolean secondPut = secondEntry && mode != 21;
            boolean helperReturned = hasFirst && mode != 26 && (mode == 4 || mode == 5 || secondPut);
            boolean completed = helperReturned && (VOID_ENTRY || mode != 30);
            String events = "9,";
            if (attempt == 0) events += "1,";
            if (sized) events += "10,";
            if (entered) events += "2,";
            if (iterator) events += "12,";
            if (hasFirst) events += "13,";
            if (nextFirst) events += "14,";
            if (firstGetter) events += "3,";
            if (secondGetter) events += "4,";
            if (firstPut) events += "5,";
            if (firstStored) events += "6,";
            if (firstPutReturned) events += "7,";
            if (hasSecond) events += "13,";
            if (secondEntry) events += "14,3,4,";
            if (secondPut) events += "5,6,7,13,";
            if (helperReturned && !VOID_ENTRY) events += "11,";
            if (completed) events += "8,";
            if (mode == 6 && attempt == 1) events = "9,";
            check(Trace.events.equals(events));
            check(inputState.sizeCalls == (sized ? 1 : 0) && Trace.firstCount == (sizeReturned ? 17 : -1));
            check(rows == null || rows.iterators == (iterator ? 1 : 0) && rows.hasCalls == (hasFirst ? 1 : 0) + (hasSecond ? 1 : 0) + (secondPut ? 1 : 0)
                && rows.nextCalls == (nextFirst ? 1 : 0) + (secondEntry ? 1 : 0) && rows.position == (nextReturned ? 1 : 0) + (secondEntry ? 1 : 0));
            check(first.firstCalls == (firstGetter ? 1 : 0) && first.secondCalls == (secondGetter ? 1 : 0));
            check(next.firstCalls == (secondEntry ? 1 : 0) && next.secondCalls == (secondEntry ? 1 : 0));
            check(first.second == (firstReturned ? late0 : value0) && next.second == (secondEntry ? late2 : value2));
            check(state.calls == (firstPut ? 1 : 0) + (secondPut ? 1 : 0) && state.stores == (firstStored ? 1 : 0) + (secondPut ? 1 : 0));
            check(state.keys[0] == (firstStored ? key0 : null) && state.values[0] == (firstStored ? late0 : null));
            check(state.keys[1] == (secondPut ? key2 : null) && state.values[1] == (secondPut ? late2 : null));
            check(otherState.calls == 0 && otherState.stores == 0 && state.sizeCalls == 0 && late.iterators == 0 && late.original[0] == replacement);
            check(Trace.rows == (sizeReturned || firstReturned ? late : rows) && Trace.sink == (firstReturned ? other : destination));
            check(Trace.output == (sizeReturned ? destination : other));
            if (original.length != 0) check(original[0] == (firstReturned ? replacement : mode == 3 ? null : first)
                && original[1] == (secondEntry ? replacement : firstStored ? next : initial));
            Map expectedResult = mode == 32 && firstStored ? Trace.alternate : originalResult;
            check(Trace.returned == expectedResult && Trace.after == (!VOID_ENTRY && completed ? expectedResult : sentinel));
            if (mode == 6) check(attempt == 0 ? failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure : failure instanceof NoClassDefFoundError);
            else if (mode >= 7 && mode <= 13 || mode == 25 || mode == 26 || mode == 27 || mode == 28 || mode == 29 && !VOID_ENTRY || mode == 30 && !VOID_ENTRY) check(failure == Trace.failure);
            else if (mode == 14 || mode == 15) check(failure == Trace.error);
            else if (mode == 1 || mode == 2 || mode == 3 || mode == 23 && !VOID_ENTRY) check(failure instanceof NullPointerException);
            else if (mode == 18 || mode == 19 || mode == 21 || mode == 24) check(failure instanceof ClassCastException);
            else check(failure == null && actual == (VOID_ENTRY ? null : expectedResult));
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
