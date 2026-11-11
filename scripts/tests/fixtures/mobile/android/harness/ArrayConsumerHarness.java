import fixture.Client;
import fixture.Sink;
import fixture.Trace;
import fixture.api.Facade;
import fixture.api.Provider;
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
public final class ArrayConsumerHarness {
    static void check(boolean condition) {
        if (!condition) throw new AssertionError("owned array consumer changed");
    }
    static final class Callback implements Sink<Object> {
        final Object[] original, late;
        final Object left, right, changed, lateReplacement;
        final int mode, begin;
        int calls;
        Callback(Object[] original, Object[] late, Object left, Object right,
                 Object changed, Object lateReplacement, int mode, int begin) {
            this.original = original; this.late = late; this.left = left; this.right = right;
            this.changed = changed; this.lateReplacement = lateReplacement; this.mode = mode; this.begin = begin;
        }
        public int compare(Object a, Object b) {
            check(a == left && b == right); calls++; Trace.hit(3);
            Trace.current = late; Trace.sink = null; Trace.replacement = lateReplacement;
            original[begin] = changed;
            if (mode == 7) throw Trace.failure;
            if (mode == 8) throw Trace.error;
            return mode == 1 ? 1 : mode == 2 ? 0 : -1;
        }
    }
    static void metadata() throws Exception {
        Method method = Provider.class.getDeclaredMethod("apply", Object[].class, Sink.class, int.class, int.class);
        check(method.getReturnType() == void.class && method.getGenericReturnType() == void.class);
        check(Modifier.isPublic(method.getModifiers()) && Modifier.isStatic(method.getModifiers()) && !method.isSynthetic() && !method.isVarArgs());
        check(method.getExceptionTypes().length == 0 && method.getTypeParameters().length == 1);
        TypeVariable<?> formal = method.getTypeParameters()[0];
        check(formal.getGenericDeclaration().equals(method) && formal.getBounds().length == 1 && formal.getBounds()[0] == Object.class);
        Type[] types = method.getGenericParameterTypes();
        check(types.length == 4 && types[0] instanceof GenericArrayType && ((GenericArrayType)types[0]).getGenericComponentType().equals(formal));
        check(types[1] instanceof ParameterizedType && ((ParameterizedType)types[1]).getRawType() == Sink.class);
        Type slot = ((ParameterizedType)types[1]).getActualTypeArguments()[0];
        check(slot instanceof WildcardType);
        WildcardType wildcard = (WildcardType)slot;
        check(wildcard.getLowerBounds().length == 1 && wildcard.getLowerBounds()[0].equals(formal));
        check(wildcard.getUpperBounds().length == 1 && wildcard.getUpperBounds()[0] == Object.class);
        check(types[2] == int.class && types[3] == int.class);
        check(Facade.class.getMethod("apply", Object[].class, Sink.class, int.class, int.class).getDeclaringClass() == Provider.class);
    }
    static long events(int... values) { long result = 0; for (int value : values) result = result * 11 + value; return result; }
    static void invoke(int route, int begin, int end) throws Throwable {
        if (route == 0) Client.call(begin, end);
        else if (route == 1) ArrayConsumerCaller.throughClient(begin, end);
        else if (route == 2) ArrayConsumerCaller.direct(begin, end);
        else {
            Trace.hit(2);
            if (route == 3) {
                try { Facade.class.getMethod("apply", Object[].class, Sink.class, int.class, int.class)
                    .invoke(null, Trace.current, Trace.sink, begin, end); }
                catch (InvocationTargetException failure) { throw failure.getCause(); }
            } else MethodHandles.lookup().findStatic(Facade.class, "apply",
                MethodType.methodType(void.class, Object[].class, Sink.class, int.class, int.class))
                .invokeExact(Trace.current, Trace.sink, begin, end);
            Trace.hit(9);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]); metadata();
        for (int attempt = 0; attempt < 2; attempt++) {
            Object left = new Object(), right = new Object(), written = new Object(), changed = new Object();
            if (mode == 16 || mode == 17 || mode == 21) { left = "left"; right = "right"; changed = "changed"; }
            if (mode == 17) written = "written";
            if (mode == 20 || mode == 21) { left = null; right = null; written = null; }
            Object[] original = (mode == 16 || mode == 17 || mode == 21) ? new String[]{(String)left, (String)right} : new Object[]{left, right};
            Object[] late = new Object[]{new Object(), new Object()}; Object late0 = late[0], late1 = late[1];
            int begin = mode == 4 ? -1 : mode == 18 ? 1 : 0;
            int end = mode == 5 ? 2 : mode == 22 ? 0 : 1;
            Object expectedLeft = begin == 1 ? right : left, expectedRight = end == 0 ? left : right;
            Callback callback = new Callback(original, late, expectedLeft, expectedRight, changed, new Object(), mode, begin);
            Trace.events = 0; Trace.fail = mode == 9 ? 2 : mode == 10 ? 1 : mode == 11 ? 5 : mode == 12 ? 6 : mode == 13 ? 7 : mode == 14 ? 8 : mode == 15 ? 9 : mode == 19 ? -6 : 0;
            Trace.failure = new IllegalArgumentException("original"); Trace.error = new LinkageError("original");
            Trace.current = mode == 3 ? null : original; Trace.sink = mode == 6 ? null : callback;
            Trace.observed = null; Trace.left = null; Trace.right = null; Trace.replacement = written;
            Throwable failure = null;
            try { invoke(route, begin, end); } catch (Throwable caught) { failure = caught; }
            boolean initialized = mode != 9 && !(mode == 10 && attempt == 1);
            boolean entered = mode != 9 && mode != 10;
            boolean leftRead = entered && mode != 3 && mode != 4 && mode != 11;
            boolean rightRead = leftRead && mode != 5 && mode != 12 && mode != 19;
            boolean compared = rightRead && mode != 6;
            boolean returned = compared && mode != 7 && mode != 8;
            boolean stored = returned && mode != 13 && mode != 16;
            if (mode == 10) check(attempt == 0 ? failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure : failure instanceof NoClassDefFoundError);
            else if (mode == 8 || mode == 19) check(failure == Trace.error);
            else if ((mode >= 7 && mode <= 15) || mode == 11 || mode == 12) check(failure == Trace.failure);
            else if (mode == 3 || mode == 6) check(failure instanceof NullPointerException);
            else if (mode == 4 || mode == 5) check(failure instanceof ArrayIndexOutOfBoundsException);
            else if (mode == 16) check(failure instanceof ArrayStoreException);
            else check(failure == null);
            check(callback.calls == (compared ? 1 : 0));
            check(Trace.observed == (entered && mode != 11 ? (mode == 3 ? null : original) : null));
            check(Trace.left == (leftRead ? expectedLeft : null) && Trace.right == (rightRead ? expectedRight : null));
            Object expected0 = left, expected1 = right;
            if (compared) { if (begin == 0) expected0 = changed; else expected1 = changed; }
            if (stored) { int target = mode == 1 ? end : begin; if (target == 0) expected0 = written; else expected1 = written; }
            check(original[0] == expected0 && original[1] == expected1 && late[0] == late0 && late[1] == late1);
            check(Trace.current == (compared ? late : mode == 3 ? null : original));
            check(Trace.sink == (compared || mode == 6 ? null : callback));
            long expectedEvents = 2;
            if (attempt == 0 && initialized) expectedEvents = expectedEvents * 11 + 1;
            if (entered) expectedEvents = expectedEvents * 11 + 5;
            if (leftRead) expectedEvents = expectedEvents * 11 + 6;
            if (compared) expectedEvents = expectedEvents * 11 + 3;
            if (returned) expectedEvents = expectedEvents * 11 + 7;
            if (stored) expectedEvents = expectedEvents * 11 + 8;
            if (stored && mode != 14) expectedEvents = expectedEvents * 11 + 9;
            check(Trace.events == expectedEvents);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
