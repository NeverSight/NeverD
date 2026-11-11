import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class SuspendedCarrierHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned suspended carrier contract");
    }
    static void raise(Throwable failure) {
        if (failure instanceof RuntimeException) throw (RuntimeException) failure;
        throw (Error) failure;
    }
    static final class Value implements Carrier<Object> {
        Object item;
        int reads;
        Value(Object value) { item = value; }
        public Object value() { Object captured = item; reads++; Trace.hit(7); return captured; }
    }
    static final class Channel implements Completion<Boolean> {
        int deliveries;
        Boolean received;
        public void resume(Boolean value) { received = value; deliveries++; Trace.hit(9); }
    }
    static final class WrongChannel implements Completion<String> {
        public void resume(String value) { throw new AssertionError("wrong channel bridge was bypassed"); }
    }
    static final class Predicate implements Callback<Object,Completion<? super Boolean>,Object> {
        Object value;
        Completion<?> channel;
        int calls;
        public Object invoke(Object input, Completion<? super Boolean> completion) {
            value = input; channel = completion; calls++;
            Object result = Trace.callbackResult; Trace.hit(8); return result;
        }
    }
    static Object lower(Type type) {
        if (type instanceof WildcardType) return ((WildcardType) type).getLowerBounds()[0];
        return type;
    }
    static void metadata() throws Exception {
        Method method = Provider.class.getMethod("choose", Carrier.class, Callback.class);
        require(Facade.class.getMethod("choose", Carrier.class, Callback.class).getDeclaringClass() == Provider.class);
        TypeVariable<Method> t = method.getTypeParameters()[0];
        require(method.getTypeParameters().length == 1 && t.getBounds().length == 1 && t.getBounds()[0] == Object.class);
        ParameterizedType input = (ParameterizedType) method.getGenericParameterTypes()[0];
        require(input.getRawType() == Carrier.class && ((WildcardType) input.getActualTypeArguments()[0]).getUpperBounds()[0].equals(t));
        ParameterizedType callback = (ParameterizedType) method.getGenericParameterTypes()[1];
        Type[] args = callback.getActualTypeArguments();
        require(callback.getRawType() == Callback.class && lower(args[0]).equals(t));
        ParameterizedType nested = (ParameterizedType) lower(args[1]);
        require(nested.getRawType() == Completion.class && lower(nested.getActualTypeArguments()[0]) == Boolean.class);
        require(((WildcardType) args[2]).getUpperBounds()[0] == Object.class);
        ParameterizedType result = (ParameterizedType) method.getGenericReturnType();
        require(result.getRawType() == Carrier.class && result.getActualTypeArguments()[0].equals(t));
    }
    static Carrier call(int route, Carrier source, Callback callback) throws Throwable {
        try {
            if (route == 0) return Client.pick(source, callback);
            if (route == 1) return SuspendedCarrierCaller.pick(source, callback);
            if (route == 2) return (Carrier) Client.class.getMethod("pick", Carrier.class, Callback.class).invoke(null, source, callback);
            return (Carrier) MethodHandles.publicLookup().findStatic(Client.class, "pick",
                MethodType.methodType(Carrier.class, Carrier.class, Callback.class)).invokeExact(source, callback);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), variant = Integer.parseInt(args[1]);
        int fail = Integer.parseInt(args[2]), kind = Integer.parseInt(args[3]);
        metadata();
        Throwable sentinel = kind == 0 ? new IllegalArgumentException("owned callback failure") : new LinkageError("owned callback failure");
        for (int attempt = 0; attempt < 2; ++attempt) {
            Object initial = new Object(), selectedValue = variant == 1 ? null : variant == 12 ? "owned value" : new int[] {19};
            Object lateValue = new Object(), token = variant == 7 ? null : variant == 8 ? Boolean.FALSE : variant == 9 ? Integer.valueOf(37) : new Object();
            Value source = new Value(initial), selected = new Value(new Object()), lateResult = new Value(new Object());
            Channel original = new Channel(), other = new Channel();
            Completion channel = variant == 4 ? null : variant == 5 ? new WrongChannel() : original;
            Boolean done = variant == 10 ? null : variant == 11 ? Boolean.FALSE : Boolean.TRUE;
            Predicate predicate = new Predicate();
            Trace.events = 0; Trace.mask = 0;
            Trace.seenInput = Trace.seenValue = Trace.seenReturn = Trace.seenOutput = null;
            Trace.seenCompletion = null; Trace.seenDone = null;
            Trace.completion = channel; Trace.done = done; Trace.callbackResult = token;
            Trace.output = new Value(new Object());
            Trace.observer = event -> {
                require(event != 0);
                if (event == 1) source.item = selectedValue;
                if (event == 7) source.item = lateValue;
                if (event == 2) { Trace.completion = other; Trace.done = null; }
                if (event == 8) Trace.callbackResult = new Object();
                if (event == 3) Trace.done = Boolean.FALSE;
                if (event == 4) Trace.output = variant == 6 ? null : selected;
                if (event == 5) Trace.output = lateResult;
                if (event == fail) raise(sentinel);
            };
            Carrier result = null; Throwable caught = null;
            try { result = call(route, variant == 2 ? null : source, variant == 3 ? null : predicate); }
            catch (Throwable failure) { caught = failure; }
            if (fail == 6) {
                if (attempt == 1) require(caught instanceof NoClassDefFoundError);
                else if (kind == 0) require(caught instanceof ExceptionInInitializerError && caught.getCause() == sentinel);
                else require(caught == sentinel);
            } else if (fail != 0) require(caught == sentinel);
            else if (variant == 2 || variant == 3) require(caught instanceof NullPointerException);
            else if (variant == 5) require(caught instanceof ClassCastException);
            else require(caught == null && result == (variant == 6 ? null : selected));
            int[] events = {6,1,7,2,8,3,9,4,5};
            long expected = 0; int mask = 0;
            if (!(fail == 6 && attempt == 1)) for (int event : events) {
                if (attempt == 1 && event == 6 || variant == 4 && event == 9) continue;
                if (event == 7 && variant == 2 || event == 8 && variant == 3 || event == 9 && variant == 5) break;
                if (event == 1) mask |= 1;
                if (event == 2) mask |= 2;
                if (event == 3) mask |= 4;
                if (event == 4) mask |= 8;
                if (event == 5) mask |= 16;
                expected = expected * 10 + event;
                if (event == fail) break;
            }
            require(Trace.events == expected && Trace.mask == mask);
            if ((mask & 1) != 0) require(Trace.seenInput == (variant == 2 ? null : source));
            if ((mask & 2) != 0) require(Trace.seenValue == selectedValue && Trace.seenCompletion == channel && Trace.seenDone == done);
            if ((mask & 4) != 0) require(Trace.seenReturn == token);
            if ((mask & 16) != 0) require(Trace.seenOutput == (variant == 6 ? null : selected));
            int callbackCalls = expected != 0 && Long.toString(expected).indexOf('8') >= 0 ? 1 : 0;
            require(predicate.calls == callbackCalls);
            if (callbackCalls != 0) require(predicate.value == selectedValue && predicate.channel == channel);
            int delivered = expected != 0 && Long.toString(expected).indexOf('9') >= 0 ? 1 : 0;
            require(original.deliveries == delivered && other.deliveries == 0);
            if (delivered != 0) require(original.received == done);
            require(selected.reads == 0 && lateResult.reads == 0);
            System.out.println("route=" + route + ",variant=" + variant + ",fail=" + fail + ",kind=" + kind + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
