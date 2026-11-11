import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProtectedMixedConsumerHarness {
    static int values, identities, states, failures;
    static final Method[] methods = new Method[3];
    static final MethodHandle[] handles = new MethodHandle[3];
    static {
        try {
            Class<?>[] slots = { Object.class, Builder.class, Special.class };
            for (int i=0; i<3; i++) {
                methods[i] = Core.class.getDeclaredMethod("read", Token.class, int.class, slots[i], boolean.class, long.class, double.class);
                methods[i].setAccessible(true); handles[i] = MethodHandles.lookup().unreflect(methods[i]);
            }
        } catch (Exception error) { throw new AssertionError(error); }
    }
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    static void call(int route, Core self, Token token, int index, Special input, boolean flag, long cookie, double weight) throws Throwable {
        try {
            switch (route) {
            case 0: ProtectedMixedConsumerCaller.root(self, token, index, input, flag, cookie, weight); return;
            case 1: ProtectedMixedConsumerCaller.middle(self, token, index, input, flag, cookie, weight); return;
            case 2: ProtectedMixedConsumerCaller.typed(self, token, index, input, flag, cookie, weight); return;
            default:
                int slot = (route-3)/2;
                if ((route & 1) != 0) methods[slot].invoke(self, token, index, input, flag, cookie, weight);
                else handles[slot].invoke(self, token, index, input, flag, cookie, weight);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length != 0 && args[0].equals("rebuilt");
        value(methods[2].getModifiers() == (rebuilt ? Modifier.PUBLIC : Modifier.PROTECTED));
        for (int i=0; i<2; i++) value(methods[i].isBridge() && methods[i].isSynthetic() && Modifier.isPublic(methods[i].getModifiers()));
        RuntimeException problem = new IllegalStateException("owned failure"); Object marker = new Object();
        long[] cookies = { Long.MIN_VALUE, -1, 0, 1, Long.MAX_VALUE };
        double[] weights = { Double.NEGATIVE_INFINITY, -0.0, 0.0, 1.25, Double.NaN };
        for (int route=0; route<9; route++)
            for (boolean nullSelf : new boolean[]{false, true})
                for (boolean nullToken : new boolean[]{false, true})
                    for (boolean nullInput : new boolean[]{false, true})
                        for (boolean fail : new boolean[]{false, true})
                            for (int sample=0; sample<cookies.length; sample++) {
                                Core self = nullSelf ? null : new Core(); Token token = nullToken ? null : new Token();
                                Special input = nullInput ? null : new Special(); boolean flag = (sample & 1) != 0;
                                int index = sample == 0 ? Integer.MIN_VALUE : sample == 4 ? Integer.MAX_VALUE : sample-2;
                                if (self != null) { self.seen = marker; self.failure = fail ? problem : null; }
                                Throwable error = null;
                                try { call(route, self, token, index, input, flag, cookies[sample], weights[sample]); }
                                catch (Throwable caught) { error = caught; }
                                if (self == null) { value(error instanceof NullPointerException); failures++; }
                                else {
                                    state(self.calls == 1 && self.visits == (route == 0 ? 1 : 0));
                                    state(self.index == index && self.flag == flag && self.cookie == cookies[sample]);
                                    value(Double.doubleToRawLongBits(self.weight) == Double.doubleToRawLongBits(weights[sample]));
                                    same(self.seen, input); same(self.tokenSeen, token);
                                    if (fail) { same(error, problem); failures++; }
                                    else if (token == null || input == null) { value(error instanceof NullPointerException); failures++; }
                                    else same(error, null);
                                }
                                if (token != null) state(token.calls == (!nullSelf && !fail ? 1 : 0));
                                if (input != null) state(input.calls == (!nullSelf && !fail && !nullToken ? 1 : 0));
                            }
        for (int route : new int[]{0, 1, 3, 4, 5, 6}) {
            Core self = new Core(); self.seen = marker; Token token = new Token(); Builder wrong = new Builder();
            Throwable error = null;
            try {
                if (route == 0) ProtectedMixedConsumerCaller.root(self, token, 7, wrong, true, 9L, -0.0);
                else if (route == 1) ProtectedMixedConsumerCaller.middle(self, token, 7, wrong, true, 9L, -0.0);
                else if ((route & 1) != 0) methods[(route-3)/2].invoke(self, token, 7, wrong, true, 9L, -0.0);
                else handles[(route-3)/2].invoke(self, token, 7, wrong, true, 9L, -0.0);
            } catch (InvocationTargetException caught) { error = caught.getCause(); }
            catch (Throwable caught) { error = caught; }
            value(error instanceof ClassCastException); failures++;
            state(self.calls == 0 && self.visits == (route == 0 ? 1 : 0) && token.calls == 0 && wrong.calls == 0);
            same(self.seen, marker);
        }
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("both-original-erased-bounded-bridges-and-wide-argument-bits=retained,typed-access-change=reported");
    }
}
