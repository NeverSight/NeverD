import fixture.*;
import java.lang.reflect.*;
public final class BoundedCovariantConsumerHarness {
    static final class FutureConsumer extends Consumer {}
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned bounded covariant consumer invariant");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    static void put(Carrier<?> input, Bound value) { ((Carrier) input).value = value; }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        final int route = Integer.parseInt(args[0]);
        final int mode = Integer.parseInt(args[1]);
        Method method = Consumer.class.getMethod("accept", Carrier.class);
        TypeVariable<?> formal = method.getTypeParameters()[0];
        Type argument = ((ParameterizedType) method.getGenericParameterTypes()[0]).getActualTypeArguments()[0];
        require(formal.getBounds()[0] == Bound.class && method.getReturnType() == void.class);
        require(((WildcardType) argument).getUpperBounds()[0].equals(formal));
        for (int attempt = 0; attempt < 2; ++attempt) {
            Consumer.events = 0;
            final Consumer consumer = new FutureConsumer();
            final Carrier<Payload> input = new Carrier<Payload>();
            final Payload initial = new Payload(), alternate = new Payload();
            final Bound poison = new Bound(), sentinel = new Bound();
            final Object oldMarker = new Object(), marker = mode == 7 ? null : new Object();
            final RuntimeException callbackFailure = new RuntimeException();
            final RuntimeException targetFailure = new RuntimeException();
            final Error callbackError = new AssertionError("callback sentinel");
            final int[] calls = {0};
            consumer.savedValue = sentinel; consumer.marker = oldMarker;
            consumer.failure = mode == 5 || mode == 13 ? targetFailure : null;
            input.value = mode == 10 ? null : initial;
            if (mode == 6) put(input, poison);
            input.probe = mode == 11 ? null : new Probe() {
                public Object observe(Consumer owner, Carrier actualInput) {
                    require(++calls[0] == 1 && owner == consumer && actualInput == input);
                    require(Consumer.events == (route < 2 ? 16 : 1));
                    require(consumer.savedInput == input && consumer.marker == oldMarker
                            && consumer.savedValue == sentinel);
                    if (mode == 3 || mode == 13) throw callbackFailure;
                    if (mode == 4) throw callbackError;
                    if (mode == 8) input.value = alternate;
                    if (mode == 9) consumer.failure = targetFailure;
                    if (mode == 12) input.value = null;
                    if (mode == 15) put(input, poison);
                    return marker;
                }
            };
            Throwable failure = null;
            try {
                Consumer supplied = mode == 1 ? null : consumer;
                Carrier argumentInput = mode == 2 ? null : input;
                if (route == 0) Core.call(supplied, argumentInput);
                else if (route == 1) Core.typed(supplied, argumentInput);
                else if (route == 2) Core.direct(supplied, argumentInput);
                else supplied.accept(argumentInput);
            } catch (Throwable caught) { failure = caught; }
            boolean beforeCallback = mode == 1 || mode == 2 || mode == 11;
            boolean callbackFailed = mode == 3 || mode == 4 || mode == 13;
            boolean targetFailed = mode == 5 || mode == 9;
            boolean success = !beforeCallback && !callbackFailed && !targetFailed;
            int events = mode == 1 ? (route < 2 ? 3 : 0)
                    : route < 2 ? (success ? 414 : 16) : (success ? 7 : 1);
            require(Consumer.events == events && calls[0] == (beforeCallback ? 0 : 1));
            if (beforeCallback) require(failure instanceof NullPointerException);
            else if (mode == 4) require(failure == callbackError);
            else if (callbackFailed) require(failure == callbackFailure);
            else if (targetFailed) require(failure == targetFailure);
            else require(failure == null);
            Bound expected = mode == 6 || mode == 15 ? poison : mode == 8 ? alternate
                    : mode == 10 || mode == 12 ? null : initial;
            require(consumer.savedInput == (mode == 1 || mode == 2 ? null : input));
            require(consumer.marker == (beforeCallback || callbackFailed ? oldMarker : marker));
            require(consumer.savedValue == (success ? expected : sentinel));
            if (success) {
                if (mode == 6 || mode == 15) {
                    boolean bad = false;
                    try { Payload ignored = (Payload) consumer.savedValue; }
                    catch (ClassCastException caught) { bad = true; }
                    require(bad && Consumer.events == events);
                } else require((Payload) consumer.savedValue == expected);
                put(input, new Payload());
                require(consumer.savedInput == input && consumer.savedValue == expected);
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
