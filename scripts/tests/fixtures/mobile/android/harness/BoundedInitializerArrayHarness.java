import fixture.*;
import java.lang.reflect.*;
public final class BoundedInitializerArrayHarness {
    static long trace;
    static int failAt;
    static Object mark;
    static RuntimeException failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned bounded initializer array"); }
    static void hit(int n) { trace = trace * 10 + n; if (failAt == n) throw failure; }
    static final class FutureToken implements Token {
        int calls;
        public Object inspect() { calls++; hit(1); return mark; }
    }
    static final class FutureContext extends Context {
        int calls;
        public void touch() { calls++; hit(2); }
    }
    static final class FutureEntry implements Entry {
        final Bound result;
        final int phase;
        int calls;
        FutureEntry(Bound result, int phase) { this.result = result; this.phase = phase; }
        public Bound build() { calls++; hit(phase); return result; }
    }
    public static void main(String[] args) throws Exception {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Method method = Factory.class.getMethod("create", Token.class, Context.class, Entry[].class);
        TypeVariable<?> t = method.getTypeParameters()[0];
        check(method.isVarArgs() && method.getTypeParameters().length == 1 && t.getBounds()[0] == Bound.class && method.getGenericReturnType().equals(t));
        check(((ParameterizedType) method.getGenericParameterTypes()[0]).getActualTypeArguments()[0].equals(t));
        GenericArrayType array = (GenericArrayType) method.getGenericParameterTypes()[2];
        ParameterizedType component = (ParameterizedType) array.getGenericComponentType();
        WildcardType star = (WildcardType) component.getActualTypeArguments()[0];
        check(component.getRawType() == Entry.class && star.getLowerBounds().length == 0 && star.getUpperBounds()[0] == Object.class
                && Entry.class.getTypeParameters()[0].getBounds()[0] == Bound.class);
        for (int attempt = 0; attempt < 2; attempt++) {
            trace = 0; failAt = mode == 4 ? 1 : mode == 5 ? 2 : mode == 8 ? 3 : 0;
            failure = new RuntimeException("owned initializer failure");
            RuntimeException providerFailure = new RuntimeException("owned provider failure");
            mark = new Object(); FutureToken token = new FutureToken(); FutureContext context = new FutureContext();
            Bound built = new Bound(), second = new Bound(), cached = mode == 10 ? null : new Bound();
            if (cached != null) cached.value = mode == 11 ? null : mode == 14 ? new Long(Long.MIN_VALUE + attempt)
                    : mode == 15 ? new Long(Long.MAX_VALUE - attempt) : attempt == 0 ? new String("owned") : new Object();
            FutureEntry first = new FutureEntry(built, 3), other = new FutureEntry(second, 4);
            Entry[] entries = mode == 3 ? null : mode == 6 ? new Entry[0]
                    : new Entry[] {mode == 7 ? null : first, other};
            Factory factory = new Factory(); Bound sentinel = new Bound(); Factory.events = 0;
            factory.length = -1; factory.created = sentinel; factory.saved = sentinel;
            factory.replacement = cached; factory.failure = mode == 9 ? providerFailure : null;
            Token suppliedToken = mode == 1 ? null : token; Context suppliedContext = mode == 2 ? null : context;
            boolean success = mode == 0 || mode >= 10;
            Bound result = null; Throwable caught = null;
            try {
                if (route == 0) result = Core.call(factory, suppliedToken, suppliedContext, entries);
                else if (route == 1) Core.discard(factory, suppliedToken, suppliedContext, entries);
                else if (route == 2) result = Core.typed(factory, suppliedToken, suppliedContext, entries);
                else result = factory.create(suppliedToken, suppliedContext, entries);
            } catch (Throwable error) { caught = error; }
            if (success) check(caught == null);
            else if (mode == 6) check(caught instanceof ArrayIndexOutOfBoundsException);
            else if (mode == 1 || mode == 2 || mode == 3 || mode == 7) check(caught instanceof NullPointerException);
            else check(caught == (mode == 9 ? providerFailure : failure));
            long expectedTrace = mode == 1 ? 0 : mode == 2 || mode == 4 ? 1
                    : mode == 3 || mode == 5 || mode == 6 || mode == 7 ? 12 : 123;
            int events = route == 3 ? success ? 7 : 1 : success ? 414 : 16;
            check(trace == expectedTrace && Factory.events == events);
            check(factory.savedToken == suppliedToken && factory.savedContext == suppliedContext && factory.savedEntries == entries);
            check(factory.tokenMark == (mode == 1 || mode == 4 ? null : mark));
            check(token.calls == (mode == 1 ? 0 : 1) && context.calls == (mode == 1 || mode == 2 || mode == 4 ? 0 : 1));
            boolean sized = mode != 1 && mode != 2 && mode != 3 && mode != 4 && mode != 5;
            check(factory.length == (sized ? entries.length : -1));
            boolean called = success || mode == 8 || mode == 9;
            check(first.calls == (called ? 1 : 0) && other.calls == 0);
            check(factory.created == (called && mode != 8 ? built : sentinel));
            check(factory.saved == (success ? cached : sentinel) && result == (success && route != 1 ? cached : null));
            if (success) {
                FutureEntry future = new FutureEntry(second, 4); entries[0] = future; failAt = 0;
                check(factory.savedEntries[0].build() == second && future.calls == 1 && first.calls == 1);
                if (route != 1) {
                    try {
                        String checked = (String) result.value;
                        check(cached != null && (mode == 11 ? checked == null : mode != 14 && mode != 15 && attempt == 0 && checked == cached.value));
                    } catch (NullPointerException error) { check(cached == null); }
                    catch (ClassCastException error) { check(cached != null && (mode == 14 || mode == 15 || attempt == 1 && mode != 11)); }
                }
            }
            check(Factory.events == events);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
