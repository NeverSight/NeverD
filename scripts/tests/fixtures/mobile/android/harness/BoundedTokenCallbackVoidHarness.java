import fixture.*;
import java.lang.reflect.*;
public final class BoundedTokenCallbackVoidHarness {
    static long trace;
    static int failAt, mode;
    static Object mark;
    static RuntimeException failure;
    static Factory factory;
    static Context expected, after;
    static void check(boolean value) { if (!value) throw new AssertionError("owned bounded token callback void"); }
    static void hit(int n) { trace = trace * 10 + n; if (failAt == n) throw failure; }
    static final class FutureToken implements Token {
        int calls;
        public Object inspect() { calls++; hit(1); if (mode == 11) factory.context = expected; return mark; }
    }
    static final class FutureContext extends Context {
        int calls;
        public void touch() { calls++; check(this == expected); hit(2); }
    }
    static final class FutureCallback implements Callback {
        int calls; Object reply;
        public Object apply(Object input) { calls++; check(input == expected); hit(3); if (mode == 12) factory.context = after; return reply; }
    }
    public static void main(String[] args) throws Exception {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        Method method = Factory.class.getMethod("accept", Token.class, Callback.class);
        TypeVariable<?> t = method.getTypeParameters()[0];
        check(!method.isVarArgs() && method.getTypeParameters().length == 1 && t.getBounds()[0] == Bound.class && method.getGenericReturnType() == void.class);
        check(((ParameterizedType) method.getGenericParameterTypes()[0]).getActualTypeArguments()[0].equals(t));
        ParameterizedType callback = (ParameterizedType) method.getGenericParameterTypes()[1];
        check(((WildcardType) callback.getActualTypeArguments()[0]).getLowerBounds()[0] == Context.class
                && ((WildcardType) callback.getActualTypeArguments()[1]).getUpperBounds()[0].equals(t));
        for (int attempt = 0; attempt < 2; attempt++) {
            trace = 0; Factory.events = 0; failAt = mode == 4 ? 1 : mode == 5 ? 2 : mode == 6 ? 3 : 0;
            failure = new RuntimeException("owned callback failure"); RuntimeException providerFailure = new RuntimeException("owned provider failure");
            factory = new Factory(); FutureToken token = new FutureToken(); FutureCallback action = new FutureCallback();
            FutureContext context = new FutureContext(); expected = context; after = new FutureContext();
            factory.context = mode == 3 ? null : mode == 11 ? after : context;
            Bound value = mode == 10 ? null : new Bound();
            if (value != null) value.value = mode == 13 ? null : mode == 14 ? new Long(Long.MIN_VALUE + attempt)
                    : mode == 15 ? new Long(Long.MAX_VALUE - attempt) : attempt == 0 ? new String("owned") : new Object();
            mark = new Object(); action.reply = mode == 7 || mode == 9 ? new Object() : value;
            Bound sentinel = new Bound(); factory.produced = sentinel; factory.saved = sentinel;
            factory.failure = mode == 8 || mode == 9 ? providerFailure : null;
            Token suppliedToken = mode == 1 ? null : token; Callback suppliedCallback = mode == 2 ? null : action;
            boolean success = mode == 0 || mode >= 10; Throwable caught = null;
            try {
                if (route == 0) Core.call(factory, suppliedToken, suppliedCallback);
                else if (route == 1) Core.ignored(factory, suppliedToken, suppliedCallback);
                else if (route == 2) Core.typed(factory, suppliedToken, suppliedCallback);
                else factory.accept(suppliedToken, suppliedCallback);
            } catch (Throwable error) { caught = error; }
            if (success) check(caught == null);
            else if (mode == 1 || mode == 2 || mode == 3) check(caught instanceof NullPointerException);
            else if (mode == 7 || mode == 9) check(caught instanceof ClassCastException);
            else check(caught == (mode == 8 ? providerFailure : failure));
            long expectedTrace = mode == 1 ? 0 : mode == 3 || mode == 4 ? 1 : mode == 2 || mode == 5 ? 12 : 123;
            int events = route == 3 ? success ? 7 : 1 : success ? 414 : 16;
            check(trace == expectedTrace && Factory.events == events && factory.savedToken == suppliedToken && factory.savedCallback == suppliedCallback);
            check(factory.tokenMark == (mode == 1 || mode == 4 ? null : mark));
            check(token.calls == (mode == 1 ? 0 : 1) && context.calls == (mode == 1 || mode == 3 || mode == 4 ? 0 : 1));
            check(action.calls == (mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 5 ? 0 : 1));
            check(factory.produced == (success || mode == 8 ? value : sentinel) && factory.saved == (success ? value : sentinel));
            check(factory.context == (mode == 3 ? null : mode == 12 ? after : expected));
            if (success) {
                Bound next = new Bound(); action.reply = next; failAt = 0;
                check(factory.savedCallback.apply(expected) == next && action.calls == 2);
                try {
                    String checked = (String) factory.saved.value;
                    check(value != null && (mode == 13 ? checked == null : mode != 14 && mode != 15 && attempt == 0 && checked == value.value));
                } catch (NullPointerException error) { check(value == null); }
                catch (ClassCastException error) { check(value != null && (mode == 14 || mode == 15 || attempt == 1 && mode != 13)); }
            }
            check(Factory.events == events);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
