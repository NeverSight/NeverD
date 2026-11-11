import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ContextualProducerCompletionHarness {
    static long events;
    static int failAt, reads, accepts;
    static Object item, token;
    static RuntimeException failure;
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned contextual producer completion");
    }
    static void hit(int n) { events = events * 10 + n; if (failAt == n) throw failure; }
    static final class FutureContext extends Context {
        final int n;
        FutureContext(int n) { this.n = n; }
        public void touch() { hit(n); }
    }
    static final class Read implements Producer {
        public Object next() { reads++; hit(5); return item; }
    }
    static final class Finish implements Completion {
        public Object context() { hit(4); return token; }
        public void accept(Object value) { accepts++; require(value == item); hit(6); }
    }
    static Object call(int route, Context a, Context b, boolean flag, Context c,
            Producer producer, Completion completion) throws Throwable {
        if (route == 0) return Client.call(a, b, flag, c, producer, completion);
        if (route == 1) return ContextualProducerCompletionCaller.call(a, b, flag, c, producer, completion);
        if (route == 2) {
            try {
                return Provider.class.getMethod("first", Context.class, Context.class, boolean.class,
                        Context.class, Producer.class, Completion.class).invoke(null, a, b, flag, c, producer, completion);
            } catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Object) MethodHandles.publicLookup().findStatic(Provider.class, "first",
                MethodType.methodType(Object.class, Context.class, Context.class, boolean.class,
                        Context.class, Producer.class, Completion.class)).invokeExact(a, b, flag, c, producer, completion);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Method method = Provider.class.getMethod("first", Context.class, Context.class, boolean.class,
                Context.class, Producer.class, Completion.class);
        TypeVariable<?> t = method.getTypeParameters()[0];
        require(method.getTypeParameters().length == 1 && t.getBounds()[0] == Object.class
                && method.getGenericReturnType() == Object.class);
        Type[] parameters = method.getGenericParameterTypes();
        require(parameters.length == 6 && parameters[0] == Context.class && parameters[1] == Context.class
                && parameters[2] == boolean.class && parameters[3] == Context.class);
        ParameterizedType p = (ParameterizedType) parameters[4], q = (ParameterizedType) parameters[5];
        require(((WildcardType) p.getActualTypeArguments()[0]).getUpperBounds()[0].equals(t)
                && ((WildcardType) q.getActualTypeArguments()[0]).getLowerBounds()[0].equals(t));
        for (int attempt = 0; attempt < 2; attempt++) {
            events = 0; reads = accepts = 0;
            failAt = mode >= 6 && mode <= 11 ? mode - 5 : 0;
            failure = new RuntimeException("owned contextual failure");
            item = mode == 1 ? null : mode == 14 ? new Long(Long.MIN_VALUE + attempt)
                    : mode == 15 ? new Long(Long.MAX_VALUE - attempt) : new Object();
            token = new Object(); Provider.sentinel = new Object();
            Provider.seenA = Provider.seenB = Provider.seenC = null;
            Provider.seenFlag = false; Provider.context = Provider.seenItem = null;
            Provider.pendingProducer = null; Provider.pendingCompletion = null;
            Context a = mode == 2 ? null : new FutureContext(1);
            Context b = mode == 3 ? null : new FutureContext(2);
            Context c = mode == 4 ? null : new FutureContext(3);
            Producer producer = mode == 5 ? null : new Read();
            Completion completion = mode == 12 ? null : new Finish();
            boolean flag = mode != 13;
            Object result = null; Throwable caught = null;
            try { result = call(route, a, b, flag, c, producer, completion); }
            catch (Throwable error) { caught = error; }
            if (failAt != 0) require(caught == failure);
            else if (mode >= 2 && mode <= 5 || mode == 12) require(caught instanceof NullPointerException);
            else require(caught == null && result == (flag ? item : Provider.sentinel));
            int stage = mode == 2 ? 0 : mode == 3 ? 1 : mode == 4 ? 2
                    : mode == 12 ? 3 : mode == 5 ? 4 : failAt != 0 ? failAt : flag ? 6 : 5;
            long expected = 0; for (int n = 1; n <= stage; n++) expected = expected * 10 + n;
            require(events == expected && reads == (stage >= 5 ? 1 : 0)
                    && accepts == (stage >= 6 ? 1 : 0));
            require(Provider.seenA == a && Provider.seenB == (stage >= 1 && failAt != 1 ? b : null)
                    && Provider.seenC == (stage >= 2 && failAt != 2 ? c : null));
            require(Provider.seenFlag == (stage >= 2 && failAt != 2 && flag));
            require(Provider.context == (stage >= 4 && failAt != 4 ? token : null));
            require(Provider.seenItem == (stage >= 5 && failAt != 5 ? item : null));
            boolean saved = stage >= 4 && failAt != 4;
            require(Provider.pendingProducer == (saved ? producer : null)
                    && Provider.pendingCompletion == (saved ? completion : null));
            if (caught == null) {
                Object next = new Object(); item = next; failAt = 0;
                require(Provider.pendingProducer.next() == next);
                Provider.pendingCompletion.accept(next);
                require(reads == 2 && accepts == (flag ? 2 : 1));
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + expected);
        }
    }
}
