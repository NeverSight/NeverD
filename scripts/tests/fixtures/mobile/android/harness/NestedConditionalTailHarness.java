import fixture.Core;
import fixture.Envelope;
import fixture.Item;
import fixture.Provider;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class NestedConditionalTailHarness {
    static void require(boolean value, String message) {
        if (!value) throw new AssertionError("owned nested conditional tail: " + message);
    }
    static Envelope call(Provider provider, long bits, int route) throws Throwable {
        if (route == 0) return Core.make(provider, bits);
        if (route == 1) return NestedConditionalTailCaller.call(provider, bits);
        if (route == 2) {
            try { return (Envelope)Core.class.getMethod("make", Provider.class, long.class).invoke(null, provider, bits); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Envelope)MethodHandles.publicLookup().findStatic(Core.class, "make",
                MethodType.methodType(Envelope.class, Provider.class, long.class)).invoke(provider, bits);
    }
    static long expectedTrace(int attempt, int failAt, boolean nullProvider, boolean fallback) {
        long value = 0;
        if (attempt == 1 && (failAt == 1 || failAt == 2)) return 0;
        if (attempt == 0) {
            value = 1;
            if (failAt == 1) return value;
            value = value * 16 + 2;
            if (failAt == 2) return value;
        }
        if (nullProvider) return value;
        if (fallback) {
            value = value * 16 + 3;
            if (failAt == 3) return value;
        }
        for (int stage = 4; stage <= 11; stage++) {
            value = value * 16 + stage;
            if (stage == failAt) break;
        }
        return value;
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        int[] errors = {4,5,8,10,11};
        int failAt = mode >= 4 && mode <= 14 ? mode - 3 : mode == 15 ? 6 :
                mode >= 18 && mode <= 22 ? errors[mode - 18] : mode == 23 ? 1 : mode == 24 ? 2 : 0;
        boolean fatal = mode == 15 || mode >= 18 && mode <= 24;
        RuntimeException failure = new RuntimeException("owned staged failure");
        Error error = new Error("owned staged Error");
        Provider provider = new Provider();
        Trace.provider = provider; Trace.failAt = failAt; Trace.failure = failure; Trace.fatal = fatal ? error : null;
        for (int attempt = 0; attempt < 2; attempt++) {
            boolean nullInput = mode == 1 || mode == 2 || failAt == 3 || mode == 26 && attempt == 0 || mode == 27;
            Object original = nullInput ? null : new Object();
            Object outerValue = new Object(), innerValue = nullInput ? null : new Object();
            Object fallbackValue = mode == 2 ? null : new Object();
            Object oldExtra = new Object(), lateExtra = mode == 16 ? null : new Object();
            Object changedPayload = new Object();
            long inputBits = attempt == 0 ? 0x7fedcba987654321L : Long.MIN_VALUE + mode;
            long returnedBits = inputBits ^ 0x0102030405060708L;
            provider.payload = original; provider.extra = oldExtra;
            Trace.outerPayload = outerValue; Trace.innerPayload = innerValue;
            Trace.fallbackValue = new Object(); Trace.changedPayload = changedPayload; Trace.changedExtra = new Object();
            Trace.trace = 0; Trace.inputBits = 41; Trace.tailBits = -1;
            Trace.item = null; Trace.outer = null; Trace.fallbackCalls = 0; Trace.tailCalls = 0;
            Object selected = attempt == 0 ? innerValue : original;
            boolean usesFallback = selected == null;
            Object itemValue = usesFallback ? fallbackValue : selected;
            Trace.observer = stage -> {
                if (stage == 1) require(provider.payload == original, "field read moved before outer initialization");
                if (stage == 2) require(provider.payload == outerValue, "nested initialization order changed");
                if (stage == 3) { require(Trace.item == null, "fallback ran after inner construction"); Trace.fallbackValue = fallbackValue; }
                if (stage == 4) require(Trace.item != null && Trace.item.value == null, "partial inner defaults changed");
                if (stage == 5) require(Trace.item.value == itemValue, "selected inner argument changed");
                if (stage == 6) {
                    require(Trace.item != null && Trace.item.value == itemValue && Trace.outer == null,
                            "tail effect crossed inner or outer initialization");
                    Trace.tailBits = returnedBits + 1;
                }
                if (stage == 7) {
                    require(provider.payload == changedPayload && Trace.inputBits == inputBits,
                            "original tail input or partial effects changed");
                    provider.extra = lateExtra; Trace.tailBits = returnedBits;
                }
                if (stage == 8) {
                    require(Trace.outer != null && Trace.outer.item == null && Trace.outer.bits == 0 && Trace.outer.extra == null,
                            "partial outer defaults changed");
                    Trace.tailBits = ~returnedBits;
                }
                if (stage == 9) require(Trace.outer.item == Trace.item && Trace.outer.bits == 0 && Trace.outer.extra == null,
                        "outer reference store timing changed");
                if (stage == 10) require(Trace.outer.bits == returnedBits && Trace.outer.extra == null,
                        "captured wide result or store timing changed");
                if (stage == 11) require(Trace.outer.extra == lateExtra, "later volatile argument read changed");
            };
            Envelope result = null; Throwable caught = null;
            try { result = call(mode == 3 ? null : provider, inputBits, route); }
            catch (Throwable problem) { caught = problem; }
            require(Trace.trace == expectedTrace(attempt, failAt, mode == 3, usesFallback), "original event sequence changed");
            boolean initialized = failAt != 1 && failAt != 2;
            boolean itemStarted = initialized && mode != 3 && failAt != 3;
            boolean itemWritten = itemStarted && failAt != 4;
            boolean tailStarted = itemWritten && failAt != 5;
            boolean tailWritten = tailStarted && failAt != 6;
            boolean outerStarted = tailWritten && failAt != 7;
            require(Trace.fallbackCalls == (initialized && mode != 3 && usesFallback ? 1 : 0), "fallback call count changed");
            require(Trace.tailCalls == (tailStarted ? 1 : 0), "tail call count changed");
            require((Trace.item != null) == itemStarted && (Trace.outer != null) == outerStarted,
                    "published partial object identity changed");
            if (itemStarted) require(Trace.item.value == (itemWritten ? itemValue : null), "partial inner field changed");
            if (outerStarted) {
                require(Trace.outer.item == (failAt == 8 ? null : Trace.item), "partial outer reference changed");
                require(Trace.outer.bits == (failAt == 8 || failAt == 9 ? 0 : returnedBits), "partial outer wide field changed");
                require(Trace.outer.extra == (failAt == 8 || failAt == 9 || failAt == 10 ? null : lateExtra),
                        "partial outer tail field changed");
            }
            require(Trace.inputBits == (tailWritten ? inputBits : 41), "tail input publication changed");
            Object wantedPayload = tailWritten ? changedPayload : attempt == 0 ?
                    failAt == 1 ? original : failAt == 2 ? outerValue : innerValue : original;
            require(provider.payload == wantedPayload && provider.extra == (tailWritten ? lateExtra : oldExtra),
                    "provider mutation ordering changed");
            if (failAt == 1 || failAt == 2) {
                if (attempt == 1) require(caught instanceof NoClassDefFoundError, "initialization failure is not sticky");
                else if (fatal) require(caught == error, "initializer Error identity changed");
                else require(caught instanceof ExceptionInInitializerError && caught.getCause() == failure,
                        "initializer cause identity changed");
            } else if (mode == 3) require(caught instanceof NullPointerException, "null receiver failure changed");
            else if (failAt != 0) require(caught == (fatal ? error : failure), "original throwable identity changed");
            else require(caught == null && result == Trace.outer && result.item == Trace.item,
                    "original constructed result changed");
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.trace);
        }
    }
}
