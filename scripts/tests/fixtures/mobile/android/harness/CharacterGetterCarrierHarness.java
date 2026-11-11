import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class CharacterGetterCarrierHarness {
    static int values, identities, states, failures;
    static void identity(Object actual, Object wanted) {
        if (actual != wanted) throw new AssertionError("reference identity changed");
        identities++;
    }
    static void state(int actual, int wanted) {
        if (actual != wanted) throw new AssertionError("original state changed");
        states++;
    }
    static Object reflected(Method method, Object receiver, Object... input) throws Throwable {
        try { return method.invoke(receiver, input); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    interface Run { Object call() throws Throwable; }
    static void failure(Run run, Class<?> type, Throwable original) throws Throwable {
        try { run.call(); throw new AssertionError("missing original failure"); }
        catch (Throwable error) {
            if (error.getClass() != type) throw new AssertionError("exception type changed", error);
            if (original != null) identity(error, original);
            failures++;
        }
    }
    static Method erased, parent, typed;
    static MethodHandle ownHandle, parentHandle, typedHandle;
    static Object key(int route, Object receiver, Object input) throws Throwable {
        switch (route) {
        case 0: return ((Function) receiver).key(input);
        case 1: return reflected(erased, receiver, input);
        case 2: return reflected(parent, receiver, input);
        case 3: return ownHandle.invoke(receiver, input);
        case 4: return parentHandle.invoke(receiver, input);
        case 5: return Caller.key((Function) receiver, input);
        case 6: return reflected(typed, receiver, ((Character) input).charValue());
        default: return typedHandle.invoke(receiver, ((Character) input).charValue());
        }
    }
    public static void main(String[] args) throws Throwable {
        boolean original = args[0].equals("original");
        Object first = Owner.make(new Object()), second = Owner.another(new Object());
        if (first == second || first.getClass() != second.getClass()) throw new AssertionError("shared class changed");
        Class<?> worker = first.getClass();
        erased = worker.getMethod("key", Object.class);
        parent = Function.class.getMethod("key", Object.class);
        typed = worker.getMethod("key", char.class);
        ownHandle = MethodHandles.publicLookup().unreflect(erased);
        parentHandle = MethodHandles.publicLookup().unreflect(parent);
        typedHandle = MethodHandles.publicLookup().unreflect(typed);
        Method source = worker.getMethod("source"), sourceParent = Function.class.getMethod("source");
        MethodHandle sourceHandle = MethodHandles.publicLookup().unreflect(source);
        Field result = worker.getField("result"), view = worker.getField("view");
        Field calls = worker.getField("calls"), reads = worker.getField("reads");
        Field last = worker.getField("last"), problem = worker.getField("failure");
        char[] inputs = {0, 127, 255, 32767, 32768, 55296, 65535};
        for (int route = 0; route < 8; route++) {
            for (Object receiver : new Object[]{first, second}) {
                int count = calls.getInt(receiver), read = reads.getInt(receiver);
                for (Object output : new Object[]{new Object(), "result", null}) {
                    result.set(receiver, output);
                    for (char input : inputs) {
                        identity(key(route, receiver, Character.valueOf(input)), output);
                        if (last.getChar(receiver) != input) throw new AssertionError("char bits changed");
                        values++;
                        state(calls.getInt(receiver), ++count);
                        state(reads.getInt(receiver), read);
                    }
                }
                View<Character> same = new View<>(Character.valueOf((char) 65535));
                view.set(receiver, same);
                Object actual;
                switch (route % 4) {
                case 0: actual = ((Function) receiver).source(); break;
                case 1: actual = reflected(sourceParent, receiver); break;
                case 2: actual = sourceHandle.invoke(receiver); break;
                default: actual = Caller.source((Function) receiver); break;
                }
                identity(actual, same); identity(((View) actual).value, same.value);
                state(reads.getInt(receiver), ++read); state(calls.getInt(receiver), count);
                if (route < 6) {
                    final int selected = route;
                    failure(() -> key(selected, receiver, null), NullPointerException.class, null);
                    failure(() -> key(selected, receiver, Integer.valueOf(7)), ClassCastException.class, null);
                    state(calls.getInt(receiver), count); state(reads.getInt(receiver), read);
                }
                RuntimeException expected = new IllegalStateException("owned char callback failure");
                problem.set(receiver, expected);
                final int selected = route;
                failure(() -> key(selected, receiver, Character.valueOf((char) 9)), expected.getClass(), expected);
                state(calls.getInt(receiver), ++count); state(reads.getInt(receiver), read);
                state(last.getChar(receiver), 9);
                problem.set(receiver, null);
            }
            final int selected = route;
            failure(() -> key(selected, null, Character.valueOf((char) 1)), NullPointerException.class, null);
        }
        if (erased.isBridge() != original || erased.isSynthetic() != original)
            throw new AssertionError("bridge metadata not reported");
        Type getterType = ((ParameterizedType) source.getGenericReturnType()).getActualTypeArguments()[0];
        if (getterType != Character.class) throw new AssertionError("sibling getter contract changed");
        Type contract = worker.getGenericInterfaces()[0];
        if (original) {
            Method enclosing = Owner.class.getMethod("make", Object.class);
            Type formal = enclosing.getTypeParameters()[0];
            if (!worker.isAnonymousClass() || !enclosing.equals(worker.getEnclosingMethod())
                    || !formal.equals(typed.getGenericReturnType()) || !formal.equals(result.getGenericType()))
                throw new AssertionError("enclosing result scope changed");
            Type[] arguments = ((ParameterizedType) contract).getActualTypeArguments();
            if (arguments[0] != Character.class || !formal.equals(arguments[1]))
                throw new AssertionError("full interface scope changed");
        } else if (contract != Function.class || typed.getGenericReturnType() != Object.class
                || result.getGenericType() != Object.class || worker.isAnonymousClass())
            throw new AssertionError("reported shared scope projection missing");
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-char-bits-captured-results-and-sibling-getter=retained,source-scope-differences=reported");
    }
}
