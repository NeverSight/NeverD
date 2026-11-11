import fixture.Base;
import fixture.Carrier;
import fixture.Outer.Child;
import fixture.Probe;
import fixture.Factory;
import fixture.Leaf;

public final class TokenPrefixDelegationHarness {
    static final class FutureChild extends Child {
        FutureChild(Carrier<? extends Child> source) { super(source); }
        FutureChild(Factory factory) { super(factory); }
    }
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned token prefix delegation invariant");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    static void put(Carrier<?> carrier, Base value) { ((Carrier) carrier).value = value; }
    public static void main(String[] args) {
        final int route = Integer.parseInt(args[0]);
        final int mode = Integer.parseInt(args[1]);
        for (int attempt = 0; attempt < 2; ++attempt) {
            Factory.events = 0;
            final Factory factory = new Factory();
            final Child initial = new Child();
            final Child alternate = new Child();
            final Base poisoned = new Base();
            final Object marker = new Object();
            final RuntimeException callbackFailure = new RuntimeException();
            final RuntimeException targetFailure = new RuntimeException();
            final Error callbackError = new AssertionError("callback sentinel");
            final int[] calls = {0};
            final Carrier<Child> source = new Carrier<Child>(mode == 10 ? null : initial, null,
                    mode == 5 || mode == 13 || mode == 14 ? targetFailure : null);
            source.probe = mode == 2 ? null : new Probe() {
                public Object observe(Base self, Object actualSource) {
                    require(++calls[0] == 1 && actualSource == source && Base.latest == self);
                    require(Factory.events == (route >= 2 ? 1 : 0));
                    require(factory.savedToken == (route >= 2 ? Leaf.class : null));
                    require(self instanceof Child && self.source == source && self.events == 1);
                    require(self.value == null && self.marker == null && ((Child) self).finished == 0);
                    if (mode == 3 || mode == 13) throw callbackFailure;
                    if (mode == 4) throw callbackError;
                    if (mode == 7) source.value = alternate;
                    if (mode == 8) put(source, poisoned);
                    if (mode == 9) source.failure = targetFailure;
                    if (mode == 11) source.value = null;
                    if (mode == 14) source.failure = null;
                    return mode == 15 ? null : marker;
                }
            };
            if (mode == 6) put(source, poisoned);
            factory.replacement = mode == 1 ? null : source;
            factory.failure = mode == 14 ? targetFailure : null;
            Base.latest = null;
            Child result = null;
            Throwable failure = null;
            try {
                Carrier<? extends Child> argument = mode == 1 ? null : source;
                if (route == 0) result = new Child(argument);
                else if (route == 1) result = new FutureChild(argument);
                else if (route == 2) result = new Child(mode == 12 ? (Factory) null : factory);
                else result = new FutureChild(mode == 12 ? (Factory) null : factory);
            } catch (Throwable caught) { failure = caught; }
            if (route >= 2 && (mode == 12 || mode == 14)) {
                require(Base.latest == null && result == null && calls[0] == 0);
                require(mode == 12 ? failure instanceof NullPointerException : failure == targetFailure);
                require(Factory.events == (mode == 12 ? 0 : 1));
                require(factory.savedToken == (mode == 12 ? null : Leaf.class));
                System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=0");
                continue;
            }
            require(Factory.events == (route >= 2 ? 1 : 0));
            require(factory.savedToken == (route >= 2 ? Leaf.class : null));
            Base partial = Base.latest;
            require(partial instanceof Child && partial.source == (mode == 1 ? null : source));
            boolean beforeCallback = mode == 1 || mode == 2;
            boolean callbackFailed = mode == 3 || mode == 4 || mode == 13;
            boolean targetFailed = mode == 5 || mode == 9;
            boolean succeeded = !beforeCallback && !callbackFailed && !targetFailed;
            require(calls[0] == (beforeCallback ? 0 : 1));
            require(partial.events == (succeeded ? 123 : targetFailed ? 12 : 1));
            require(partial.marker == (beforeCallback || callbackFailed || mode == 15 ? null : marker));
            Base expected = mode == 6 || mode == 8 ? poisoned : mode == 7 ? alternate
                    : mode == 10 || mode == 11 ? null : initial;
            require(partial.value == (beforeCallback || callbackFailed ? null : expected));
            require(((Child) partial).finished == (succeeded ? (route >= 2 ? 10 : 9) : 0));
            if (beforeCallback) require(failure instanceof NullPointerException);
            else if (mode == 4) require(failure == callbackError);
            else if (callbackFailed) require(failure == callbackFailure);
            else if (targetFailed) require(failure == targetFailure);
            else require(failure == null && result == partial);
            if (succeeded) {
                if (mode == 6 || mode == 8) {
                    boolean bad = false;
                    try { Child ignored = (Child) result.value; }
                    catch (ClassCastException caught) { bad = true; }
                    require(bad && partial.events == 123);
                } else require((Child) result.value == expected);
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt
                    + ",events=" + partial.events);
        }
    }
}
