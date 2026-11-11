import fixture.*;
import java.lang.reflect.*;

public final class PublicSuperclassAccessBridgesHarness {
    static int values, identities, states, failures;
    static final String[] NAMES = {"write", "flag", "scalar", "wide", "decimal", "reference"};
    static final class Leaf extends Core<Object> {
        @Override public void write(Object marker, int number, long word) {
            calls += 4;
            super.write(marker, number, word);
        }
        @Override public boolean flag(Object marker, int number, long word) {
            calls += 4;
            return super.flag(marker, number, word);
        }
        @Override public int scalar(Object marker, int number, long word) {
            calls += 4;
            return super.scalar(marker, number, word);
        }
        @Override public long wide(Object marker, int number, long word) {
            calls += 4;
            return super.wide(marker, number, word);
        }
        @Override public double decimal(Object marker, int number, long word) {
            calls += 4;
            return super.decimal(marker, number, word);
        }
        @Override public Object reference(Object marker, int number, long word) {
            calls += 4;
            return super.reference(marker, number, word);
        }
    }
    static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    static void identity(Object actual, Object expected) {
        identities++;
        require(actual == expected, "changed reference identity");
    }
    static Object direct(int method, Core<Object> receiver, Object marker, int number, long word) {
        switch (method) {
        case 0: receiver.write(marker, number, word); return null;
        case 1: return receiver.flag(marker, number, word);
        case 2: return receiver.scalar(marker, number, word);
        case 3: return receiver.wide(marker, number, word);
        case 4: return receiver.decimal(marker, number, word);
        default: return receiver.reference(marker, number, word);
        }
    }
    static Object ancestor(int method, Base receiver, Object marker, int number, long word) {
        switch (method) {
        case 0: receiver.write(marker, number, word); return null;
        case 1: return receiver.flag(marker, number, word);
        case 2: return receiver.scalar(marker, number, word);
        case 3: return receiver.wide(marker, number, word);
        case 4: return receiver.decimal(marker, number, word);
        default: return receiver.reference(marker, number, word);
        }
    }
    static Object call(int route, int method, Core<Object> receiver, Object marker, int number, long word)
            throws Throwable {
        try {
            if (route == 0) return direct(method, receiver, marker, number, word);
            if (route == 1) return ancestor(method, receiver, marker, number, word);
            Class<?> owner = route == 2 ? Core.class : route == 3 ? Base.class : Class.forName("fixture.Root");
            Method selected = owner.getDeclaredMethod(NAMES[method], Object.class, int.class, long.class);
            selected.setAccessible(true);
            return selected.invoke(receiver, marker, number, word);
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static void state(Core<Object> receiver, Object marker, int calls, int number, long word,
                      int[] mutable, int arrayValue) {
        identity(receiver.marker, marker);
        states++;
        require(receiver.calls == calls, "changed virtual or nearest superclass dispatch");
        states++;
        require(receiver.number == number, "changed integer word");
        states++;
        require(receiver.word == word, "changed wide word");
        states++;
        if (mutable != null) require(mutable[0] == arrayValue, "changed mutable alias");
    }
    public static void main(String[] args) throws Throwable {
        boolean original = args[0].equals("original");
        require(Core.class.getGenericSuperclass().getTypeName().equals("fixture.Root<G>"), "changed original parent edge");
        Class<?> root = Class.forName("fixture.Root");
        require(!Modifier.isPublic(root.getModifiers()) && Modifier.isAbstract(root.getModifiers()), "changed parent visibility");
        require(Core.class.getDeclaredMethod("value").getGenericReturnType().getTypeName().equals("G") &&
            root.getDeclaredMethod("value").getGenericReturnType().getTypeName().equals("P"), "changed complete class scopes");
        for (String name : NAMES) {
            Method method = Core.class.getDeclaredMethod(name, Object.class, int.class, long.class);
            require(method.isBridge() == original && method.isSynthetic() == original &&
                method.getTypeParameters().length == 0 && method.getExceptionTypes().length == 0,
                "unreported bridge flags or changed complete declaration");
        }
        int[] numbers = {Integer.MIN_VALUE, 0, Integer.MAX_VALUE};
        long[] words = {0x8000000000000000L, 0x7ff8000000001234L, Long.MAX_VALUE};
        for (boolean leaf : new boolean[]{false, true}) {
            int count = leaf ? 7 : 3;
            for (int route = 0; route < 5; route++)
                for (int method = 0; method < NAMES.length; method++) {
                    for (boolean mutable : new boolean[]{false, true})
                        for (int sample = 0; sample < numbers.length; sample++) {
                            Core<Object> receiver = leaf ? new Leaf() : new Core<Object>();
                            Object marker = mutable ? new int[]{7} : new Object();
                            Object result = call(route, method, receiver, marker, numbers[sample], words[sample]);
                            values++;
                            switch (method) {
                            case 0: require(result == null, "changed void result"); break;
                            case 1: require(result.equals(numbers[sample] > 0), "changed boolean result"); break;
                            case 2: require(result.equals(numbers[sample]), "changed integer result"); break;
                            case 3: require(result.equals(words[sample]), "changed long result"); break;
                            case 4: require(Double.doubleToRawLongBits((Double) result) == words[sample], "changed NaN or zero bits"); break;
                            default: require(result == marker, "changed reference result identity");
                            }
                            state(receiver, marker, count, numbers[sample], words[sample], mutable ? (int[]) marker : null, 10);
                        }
                    for (int mode = 0; mode < 2; mode++) {
                        Core<Object> receiver = leaf ? new Leaf() : new Core<Object>();
                        RuntimeException expected = new RuntimeException("owned-ancestor-failure");
                        receiver.failure = expected;
                        int[] marker = {7};
                        try {
                            call(route, method, mode == 0 ? receiver : null, marker, 9, 13L);
                            throw new AssertionError("missing original failure");
                        } catch (Throwable error) {
                            failures++;
                            if (mode == 0) identity(error, expected);
                            else require(error instanceof NullPointerException, "changed null-receiver failure");
                        }
                        state(receiver, mode == 0 ? marker : null, mode == 0 ? count : 0,
                              mode == 0 ? 9 : 0, mode == 0 ? 13L : 0, marker, mode == 0 ? 10 : 7);
                    }
                }
            for (int route = 0; route < 4; route++) {
                Core<Object> receiver = leaf ? new Leaf() : new Core<Object>();
                Object value;
                if (route == 0) value = receiver.value();
                else if (route == 1) value = ((Core) receiver).value();
                else {
                    Method method = (route == 2 ? Core.class : root).getDeclaredMethod("value");
                    method.setAccessible(true);
                    value = method.invoke(receiver);
                }
                values++;
                identity(value, null);
                states++;
                require(receiver.calls == 0, "getter introduced unrelated effect");
            }
        }
        require(values == 368 && identities == 548 && states == 1928 && failures == 120,
                "incomplete independent inventory: " + values + ":" + identities + ":" + states + ":" + failures);
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("original-public-access-bridges=retained,nearest-super-dispatch-and-complete-scopes=retained,bridge-flag-differences=reported");
    }
}
