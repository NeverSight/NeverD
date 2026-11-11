import fixture.Api;
import fixture.Box;
import fixture.Trace;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class GenericInterfaceFinalFieldHarness {
    private static final RuntimeException RUNTIME = new RuntimeException("owned interface failure");
    private static final Error ERROR = new AssertionError("owned interface error");

    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned generic interface final field mismatch");
    }

    private static Object read(int route, Class<?> api) throws Throwable {
        if (route == 0) return Api.Z_FIRST;
        if (route == 1) return Api.A_SECOND;
        if (route == 2) return Api.SELF;
        return api.getField("A_SECOND").get(null);
    }

    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        final Object a = new Object(), b = new StringBuilder("owned unchecked contents"), c = new Object();
        final Box first = new Box(a), second = new Box(b), third = new Box(c), other = new Box(new Object());
        final Box selectedFirst = mode == 12 ? null : mode == 9 ? other : first;
        final Box selectedSecond = mode == 12 ? null : mode == 10 ? selectedFirst : second;
        final Box selectedThird = mode == 11 || mode == 12 ? null : mode == 15 ? selectedSecond : third;
        final int failStage = mode == 1 || mode == 2 || mode == 7 ? 1
                : mode == 3 || mode == 4 || mode == 8 ? 3
                : mode == 5 || mode == 6 ? 5 : 0;
        final int[] calls = new int[1];
        Trace.log = Trace.stage = 0;
        Trace.first = mode == 12 ? null : first;
        Trace.second = mode == 12 ? null : second;
        Trace.third = mode == 12 ? null : third;
        Trace.reFirst = Trace.reSecond = Trace.reSelf = null;
        Trace.inputFirst = Trace.nextFirst = Trace.nextSecond = Trace.nextSelf = null;
        Trace.inputSecond = Trace.selfFirst = Trace.selfSecond = Trace.selfSelf = null;
        Trace.action = mode == 7 ? null : new Runnable() {
            public void run() {
                int stage = Trace.stage;
                calls[0]++;
                require(stage == 1 || stage == 3 || stage == 5);
                require(Trace.log == (stage == 1 ? 1 : stage == 3 ? 12 : 123));
                require(Api.Z_FIRST == (stage == 1 ? null : selectedFirst));
                require(Api.A_SECOND == (stage == 5 ? selectedSecond : null));
                require(Api.SELF == null);
                if (stage == failStage) {
                    if (mode == 2 || mode == 4 || mode == 6) throw ERROR;
                    if (mode == 8) throw (RuntimeException)null;
                    throw RUNTIME;
                }
                if (stage == 1 && mode == 9) Trace.first = other;
                if (stage == 3 && mode == 10) Trace.second = selectedFirst;
                if (stage == 3 && mode == 14) Trace.first = other;
                if (stage == 5 && mode == 11) Trace.third = null;
                if (stage == 5 && mode == 15) {
                    Trace.first = other;
                    Trace.third = selectedSecond;
                }
            }
        };
        Class<?> api = Class.forName("fixture.Api", false, GenericInterfaceFinalFieldHarness.class.getClassLoader());
        require(Trace.log == 0 && Trace.stage == 0 && calls[0] == 0);
        for (int attempt = 0; attempt < 2; attempt++) {
            Object result = null;
            Throwable failure = null;
            try { result = read(route, api); }
            catch (Throwable caught) { failure = caught; }
            if (failStage != 0) {
                if (attempt != 0) require(failure instanceof NoClassDefFoundError);
                else if (mode == 2 || mode == 4 || mode == 6) require(failure == ERROR);
                else {
                    require(failure instanceof ExceptionInInitializerError);
                    Throwable cause = failure.getCause();
                    if (mode == 7 || mode == 8) require(cause instanceof NullPointerException);
                    else require(cause == RUNTIME);
                }
                require(Trace.stage == failStage && Trace.log == (failStage == 1 ? 1 : failStage == 3 ? 12 : 123));
                require(calls[0] == (mode == 7 ? 0 : (failStage + 1) / 2));
            } else {
                require(failure == null && Trace.stage == 6 && Trace.log == 123 && calls[0] == 3);
                require(result == (route == 0 ? selectedFirst : route == 2 ? selectedThird : selectedSecond));
                require(Api.Z_FIRST == selectedFirst && Api.A_SECOND == selectedSecond && Api.SELF == selectedThird);
                require(Trace.selfFirst == selectedFirst && Trace.selfSecond == selectedSecond && Trace.selfSelf == null);
                Trace.first = Trace.second = Trace.third = other;
            }
            require(Trace.reFirst == null && Trace.reSecond == null && Trace.reSelf == null);
            if (Trace.log >= 12)
                require(Trace.inputFirst == selectedFirst && Trace.nextFirst == selectedFirst
                        && Trace.nextSecond == null && Trace.nextSelf == null);
            if (Trace.log == 123) require(Trace.inputSecond == selectedSecond);
            require(first.value == a && second.value == b && third.value == c);
        }
        System.out.println("attempts=2,generic-interface-final-field=matched");
    }
}
