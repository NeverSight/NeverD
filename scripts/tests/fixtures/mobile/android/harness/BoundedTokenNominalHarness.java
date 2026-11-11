import fixture.*;
import java.lang.reflect.*;
public final class BoundedTokenNominalHarness {
    static int mode;
    static Object mark;
    static RuntimeException failure;
    static String alternate;
    static void check(boolean value) { if (!value) throw new AssertionError("owned bounded token nominal"); }
    static final class FutureToken implements Token {
        final Factory owner; int calls;
        FutureToken(Factory owner) { this.owner = owner; }
        public Object inspect() {
            calls++;
            if (mode == 3) throw failure;
            if (mode == 8 || mode == 9) owner.replacement = alternate;
            return mark;
        }
    }
    public static void main(String[] args) throws Exception {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        Method method = Factory.class.getMethod("key", Token.class);
        TypeVariable<?> t = method.getTypeParameters()[0];
        check(t.getBounds()[0] == Bound.class && method.getGenericReturnType() == String.class
                && ((ParameterizedType) method.getGenericParameterTypes()[0]).getActualTypeArguments()[0].equals(t));
        for (int attempt = 0; attempt < 2; attempt++) {
            Factory.events = 0; failure = new RuntimeException("owned token failure");
            RuntimeException providerFailure = new RuntimeException("owned provider failure");
            Factory factory = new Factory(); FutureToken token = new FutureToken(factory);
            Factory supplied = mode == 1 ? null : factory; Token argument = mode == 2 ? null : token;
            mark = mode == 7 ? null : mode == 10 ? new Long(Long.MIN_VALUE + attempt) : mode == 11 ? new Long(Long.MAX_VALUE - attempt) : new Object();
            String cached = mode == 5 ? null : mode == 6 ? new String("") : mode == 12 ? new String("\u0000owned\ud800") : new String("owned" + attempt);
            alternate = new String("alternate" + attempt); String sentinel = new String("before");
            factory.replacement = cached; factory.savedResult = sentinel;
            factory.failure = mode == 4 || mode == 9 ? providerFailure : null;
            boolean success = mode != 1 && mode != 2 && mode != 3 && mode != 4 && mode != 9;
            String result = null; Throwable caught = null;
            try {
                if (route == 0) result = Core.call(supplied, argument);
                else if (route == 1) Core.discard(supplied, argument);
                else if (route == 2) result = Core.typed(supplied, argument);
                else result = supplied.key(argument);
            } catch (Throwable error) { caught = error; }
            if (success) check(caught == null);
            else if (mode == 1 || mode == 2) check(caught instanceof NullPointerException);
            else check(caught == (mode == 3 ? failure : providerFailure));
            int events = mode == 1 ? route == 3 ? 0 : 3 : route == 3 ? success ? 7 : 1 : success ? 414 : 16;
            String expected = mode == 8 || mode == 9 ? alternate : cached;
            check(Factory.events == events && token.calls == (mode == 1 || mode == 2 ? 0 : 1));
            check(factory.savedToken == (mode == 1 ? null : argument));
            check(factory.marker == (mode == 1 || mode == 2 || mode == 3 ? null : mark));
            check(factory.replacement == expected && factory.savedResult == (success ? expected : sentinel));
            check(result == (success && route != 1 ? expected : null));
            if (success) {
                Object future = new Object(); mark = future;
                check(factory.savedToken.inspect() == future && token.calls == 2 && factory.savedResult == expected);
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
