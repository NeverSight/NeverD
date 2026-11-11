import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class ErasedCallbackHarness {
    static int reads, writes, readFailures, writeFailures;
    static final int FLAGS = Integer.MIN_VALUE + 7;
    static void require(boolean ok) { if (!ok) throw new AssertionError("erased callback contract"); }
    static final class Maker implements Creator<Object> {
        Object result; Input seen; int calls; RuntimeException failure;
        public Object create(Input input) { calls++; seen = input; if (failure != null) throw failure; return result; }
    }
    static final class Writer implements Writable {
        Input seen; int flags, calls; RuntimeException failure;
        public void write(Input input, int value) {
            calls++; seen = input; flags = value;
            require(input.tag == 1 && input.writes == 1);
            if (failure != null) throw failure;
        }
    }
    static Object read(int route, boolean checked, Input input, Creator<?> creator) throws Throwable {
        String name = checked ? "checkedRead" : "erasedRead";
        try {
            if (route == 0) return checked ? Host.Helper.checkedRead(input, creator) : Host.Helper.erasedRead(input, creator);
            if (route == 1) return checked ? ErasedCallbackCaller.checked(input, creator) : ErasedCallbackCaller.read(input, creator);
            if (route == 2) return Host.Helper.class.getMethod(name, Input.class, Creator.class).invoke(null, input, creator);
            return MethodHandles.publicLookup().findStatic(Host.Helper.class, name,
                MethodType.methodType(checked ? Writable.class : Object.class, Input.class, Creator.class)).invokeWithArguments(input, creator);
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    static void write(int route, Input input, Writable value) throws Throwable {
        try {
            if (route == 0) Host.Helper.erasedWrite(input, value, FLAGS);
            else if (route == 1) ErasedCallbackCaller.write(input, value, FLAGS);
            else if (route == 2) Host.Helper.class.getMethod("erasedWrite", Input.class, Writable.class, int.class).invoke(null, input, value, FLAGS);
            else MethodHandles.publicLookup().findStatic(Host.Helper.class, "erasedWrite",
                MethodType.methodType(void.class, Input.class, Writable.class, int.class)).invokeWithArguments(input, value, FLAGS);
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    static void failed(Throwable failure, Class<?> kind, Throwable original) {
        require(failure.getClass() == kind && (original == null || failure == original));
    }
    public static void main(String[] args) throws Throwable {
        Method reader = Host.Helper.class.getDeclaredMethod("read", Input.class, Creator.class);
        Method writer = Host.Helper.class.getDeclaredMethod("write", Input.class, Writable.class, int.class);
        for (Method method : new Method[]{reader, writer}) {
            require(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()));
            require(method.getTypeParameters().length == 1 && method.getExceptionTypes().length == 0);
        }
        TypeVariable<?> r = reader.getTypeParameters()[0], w = writer.getTypeParameters()[0];
        require(r.getBounds()[0] == Object.class && reader.getGenericReturnType().equals(r));
        require(((ParameterizedType)reader.getGenericParameterTypes()[1]).getActualTypeArguments()[0].equals(r));
        require(w.getBounds()[0] == Writable.class && writer.getGenericParameterTypes()[1].equals(w));
        require(writer.getReturnType() == void.class);
        for (int route = 0; route < 4; route++) {
            for (int tag : new int[]{0, 1, -1, Integer.MIN_VALUE}) for (Object result : new Object[]{null, new Writer()}) {
                Input input = new Input(); input.tag = tag; Maker maker = new Maker(); maker.result = result;
                require(read(route, false, input, maker) == (tag == 0 ? null : result));
                require(input.reads == 1 && maker.calls == (tag == 0 ? 0 : 1));
                require(maker.seen == (tag == 0 ? null : input)); reads++;
            }
            Input empty = new Input(); require(read(route, false, empty, null) == null && empty.reads == 1); reads++;
            Maker typed = new Maker(); typed.result = new Writer(); Input full = new Input(); full.tag = 1;
            require(read(route, true, full, typed) == typed.result && typed.calls == 1); reads++;
            for (int mode = 0; mode < 5; mode++) {
                Input input = mode == 0 ? null : new Input(); if (input != null) input.tag = 1;
                Maker maker = mode == 1 ? null : new Maker();
                RuntimeException original = new IllegalArgumentException("owned read failure");
                if (mode == 2) input.failure = original;
                if (mode == 3) maker.failure = original;
                if (mode == 4) maker.result = new Object();
                try { read(route, mode == 4, input, maker); throw new AssertionError("missing read failure"); }
                catch (Throwable failure) {
                    failed(failure, mode < 2 ? NullPointerException.class : mode == 4 ? ClassCastException.class : original.getClass(),
                           mode == 2 || mode == 3 ? original : null); readFailures++;
                }
                require(input == null || input.reads == 1);
                require(maker == null || maker.calls == (mode >= 3 ? 1 : 0));
            }
            for (boolean present : new boolean[]{false, true}) {
                Input input = new Input(); Writer value = present ? new Writer() : null;
                write(route, input, value); require(input.writes == 1 && input.tag == (present ? 1 : 0));
                if (present) require(value.calls == 1 && value.seen == input && value.flags == FLAGS); writes++;
            }
            for (int mode = 0; mode < 3; mode++) {
                Input input = mode == 2 ? null : new Input(); Writer value = new Writer();
                RuntimeException original = new UnsupportedOperationException("owned write failure");
                if (mode == 0) input.failure = original;
                if (mode == 1) value.failure = original;
                try { write(route, input, value); throw new AssertionError("missing write failure"); }
                catch (Throwable failure) { failed(failure, mode == 2 ? NullPointerException.class : original.getClass(), mode == 2 ? null : original); writeFailures++; }
                require(value.calls == (mode == 1 ? 1 : 0));
                require(input == null || input.writes == 1);
            }
        }
        System.out.println("reads=" + reads + ",writes=" + writes + ",read-failures=" + readFailures + ",write-failures=" + writeFailures);
    }
}
