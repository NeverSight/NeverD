package fixture;

public final class Provider {
    private Provider() {}

    public static Box first() {
        Trace.log = Trace.log * 10 + 1;
        Trace.reFirst = Api.Z_FIRST;
        Trace.reSecond = Api.A_SECOND;
        Trace.reSelf = Api.SELF;
        Trace.stage = 1;
        Trace.action.run();
        Trace.stage = 2;
        return Trace.first;
    }

    public static Box next(Box previous) {
        Trace.log = Trace.log * 10 + 2;
        Trace.inputFirst = previous;
        Trace.nextFirst = Api.Z_FIRST;
        Trace.nextSecond = Api.A_SECOND;
        Trace.nextSelf = Api.SELF;
        Trace.stage = 3;
        Trace.action.run();
        Trace.stage = 4;
        return Trace.second;
    }

    public static Box self(Box previous) {
        Trace.log = Trace.log * 10 + 3;
        Trace.inputSecond = previous;
        Trace.selfFirst = Api.Z_FIRST;
        Trace.selfSecond = Api.A_SECOND;
        Trace.selfSelf = Api.SELF;
        Trace.stage = 5;
        Trace.action.run();
        Trace.stage = 6;
        return Trace.third;
    }
}
